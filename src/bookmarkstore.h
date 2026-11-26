#ifndef BOOKMARKSTORE_H
#define BOOKMARKSTORE_H

#include "locatedplaces.h"
#include "recordid.h"

#include <QJsonObject>
#include <optional>

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include <utility>

class LogModel;
class RecordLocator;

// ============================================================================
// BookmarkStore — закладки с заметками одной вкладки (R-2, docs/design/R-5.md).
//
// Закладка привязана к ФИЗИЧЕСКОЙ строке: ключ строки RecordKey (recordid.h)
// плюс путь файла, номер строки и фрагмент текста (preview, до 200 символов —
// чтобы закладка оставалась понятной, когда строку найти нельзя). Владелец —
// LogModel вкладки (по одному хранилищу на модель), поэтому закладки живут до
// закрытия вкладки и переживают конверсию бэкенда: модель та же.
// Только GUI-поток; диск не трогает (его пишет R-4).
//
// Цена:
//   • isBookmarkedRow (роль модели BookmarkRole, отметка в жёлобе) — O(1):
//     keyAt + поиск в хэше по (файл, строка). На батч индексатора здесь
//     ничего не пересчитывается (инвариант 3);
//   • поиск строк после сброса — RecordLocator: O(log K + найденные) на
//     порцию загрузки; на события модели здесь ничего не считается.
//
// Жизнь закладки при смене данных:
//   • фильтр — ключ не меняется; скрытая закладка остаётся (статус Hidden);
//   • дозапись хвоста — ключи прежних строк не меняются;
//   • строки файла сброшены (полная перезагрузка, файл исчез, конверсия
//     вкладки в другой бэкенд) — invalidateFile / invalidateAll: закладка
//     «не проверена» (Pending), в жёлобе не показана, а её строку ищет
//     RecordLocator модели (reacquire): по номеру строки и отпечатку текста
//     (RecordId), в каждой пришедшей порции — видима строка или скрыта
//     фильтром. Нашлась — закладка снова живая с новым ключом; номера нет
//     или текст другой — «строка не найдена» (Missing). Источник при этом
//     не проверяется: строка того же номера и текста в переписанном файле —
//     та же строка;
//   • сохранённая закладка (BookmarkState, R-4) — restore: ждёт файл со
//     своим путём; источник проверяется (переписан — «не найдена»).
//
// Ловушки:
//   • ключ действителен, только когда строка уже показана модели (R-1): пока
//     файл грузится, закладка в статусе Pending, а не Missing;
//   • фрагмент (preview) — только для показа: его текст зависит от
//     кодировки; сверяет строку отпечаток RecordId (от кодировки не зависит);
//   • закладка на недописанной последней строке (без '\n') хранит
//     идентификатор без отпечатка, пока строку не допишут
//     (RecordLocator::watchCompletion → completed).
// ============================================================================

// Закладка — место в логе (LocatedPlace: ключ, идентификатор, путь, номер,
// фрагмент; состояние поиска строки — общая обвязка M-8) плюс заметка.
struct Bookmark : LocatedPlace {
    int id = -1;
    QString note;
    QDateTime created;
};

// Закладка вне вкладки (R-4 пишет её на диск, docs/design/R-5.md): источник,
// строка в нём, заметка и фрагмент. Чистые данные и JSON.
struct BookmarkState {
    static constexpr int kFormatVersion = 1;

    SourceIdentity source;
    RecordId record;
    QString note;
    QString preview;
    QDateTime created;

    // {"version":1,"source":{…},"record":{…},"note":…,"preview":…,"created":…}
    QJsonObject toJson() const;
    // Чужая версия, битый источник или строка — nullopt и причина.
    static std::optional<BookmarkState> fromJson(const QJsonObject& json, QString* error = nullptr);
};

class BookmarkStore : public LocatedPlaces {
    Q_OBJECT
public:
    enum class Status {
        Visible,  // строка показана — в жёлобе отметка, F2 к ней ходит
        Hidden,   // строка в данных, но скрыта фильтром
        Pending,  // строки сейчас нет в модели (файл грузится, удалён) или
                  // она ещё не перепроверена после перезагрузки
        Missing,  // строка не найдена: файл переписан, фрагмент не совпал
    };

    explicit BookmarkStore(LogModel& model);

    bool isEmpty() const { return m_items.isEmpty(); }
    int count() const { return int(m_items.size()); }
    // Все закладки по порядку: путь файла, затем номер строки.
    QVector<Bookmark> bookmarks() const;
    // nullptr — нет такой.
    const Bookmark* find(int id) const;

    // ---- По видимой строке ------------------------------------------------------
    // Живая закладка на видимой строке; -1 — нет. O(1).
    int idAtRow(int row) const;
    bool isBookmarkedRow(int row) const { return idAtRow(row) >= 0; }
    // Живая закладка на строке с ключом key; -1 — нет. O(1). Им выдача
    // Search (U-19) спрашивает закладку строки источника.
    int idForKey(const RecordKey& key) const;

    // ---- Правка ---------------------------------------------------------------------
    // Поставить закладку на видимую строку; id новой либо уже стоящей. Закладка,
    // потерявшая эту строку (Pending, Missing), привязывается к ней заново и
    // сохраняет заметку. -1 — строки нет.
    int add(int row, const QString& note = QString());
    bool remove(int id);
    // ---- Сохранение (R-4) ---------------------------------------------------------
    // Закладка вне вкладки: идентичность источника по концу прочитанного
    // (RecordLocator::sourceFor), строка, заметка, фрагмент. nullopt — нет
    // такой или источник не снять (файла нет и прежней идентичности нет).
    std::optional<BookmarkState> state(int id) const;
    // Все, в порядке bookmarks(); файл stdin (Ephemeral) — тоже, решает
    // вызывающий (toJson у него пустой источник).
    QVector<BookmarkState> states() const;
    // Вернуть сохранённую закладку: статус Pending, пока RecordLocator не
    // найдёт строку (файл со своим путём подключён к вкладке, источник цел,
    // текст совпал), иначе Missing. Звать можно до загрузки файла и после.
    // Закладка на той же строке уже есть — её id (пустая заметка дополняется).
    // -1 — идентификатор невалиден.
    int restore(const BookmarkState& state);

    // Поставить или снять; true — закладка теперь стоит.
    bool toggle(int row);
    void clear();
    bool setNote(int id, const QString& note);

    // ---- Где закладка -----------------------------------------------------------
    Status status(int id) const;
    // Видимая строка живой закладки; -1 — она не Visible.
    int visibleRow(int id) const;
    // Куда вести переход: видна — её строка; скрыта фильтром — ближайшая
    // видимая (LogModel::nearestRowForKey); иначе -1.
    int targetRow(int id) const;
    // Видимые строки живых закладок по возрастанию. O(закладок · log N).
    QVector<int> visibleRows() const;
    // Ближайшая по направлению видимая закладка от startRow (не включая её);
    // startRow < 0 — вперёд с начала, назад с конца. wrap — через край. -1 —
    // видимых закладок нет. Как F8: только видимые строки.
    int nextRow(int startRow, bool forward, bool wrap = true) const;

    // ---- Жизненный цикл данных (зовёт вкладка и модель) -----------------------
    // Строки файла сброшены и будут прочитаны заново.
    void invalidateFile(const QString& filePath);
    // Серые сохранённые закладки источника sourceKey (RecordLocator::
    // sourceKey) проверить снова: сжатый файл распакован заново (F-2) —
    // invalidateFile узнаёт файл по пути чтения, а ждущая закладка знает
    // только путь архива.
    void recheckSource(const QString& sourceKey);
    // Сброшены все строки (конверсия бэкенда, clear).
    void invalidateAll();
    // Загрузка файла закончилась: fullLoad — файл прочитан с нуля, иначе —
    // дочитан хвост. После дозаписи фрагменты видимых закладок файла
    // обновляются (предварительная строка без '\n' дописалась). Строки
    // ищет RecordLocator — сам, по порциям.
    void fileLoaded(const QString& filePath, bool fullLoad);
    // Текст строк файла стал другим при тех же ключах — сменилась кодировка
    // (F-1: выбор пользователя, пересмотр автоматики). Фрагменты сверенных
    // закладок на ВИДИМЫХ строках файла переписываются новым текстом: иначе
    // сверка после следующей полной загрузки (тем же декодером) сочла бы
    // строку чужой. Закладку, скрытую фильтром, текстом не обновить —
    // вкладка зовёт это ещё раз, когда перефильтрация закончится.
    void refreshPreviews(const QString& filePath);

signals:
    // Набор закладок, заметка или проверка изменились (не на каждый фильтр:
    // статус Visible/Hidden панель пересчитывает по сигналам модели сама).
    void changed();
    // Отметка закладки строки key могла смениться (поставлена, снята,
    // найдена после перезагрузки). Ключ, а не номер ряда: выдача Search
    // (U-19) находит свой ряд rowForKey — O(log N) на правку, без прохода.
    void markChanged(const RecordKey& key);

protected:
    LocatedPlace* placeById(int id) override;
    void forEachPlace(const std::function<void(int, LocatedPlace&)>& fn) override;
    void placeResolved(int id, const RecordKey& previousKey) override;
    void placeRefused(int id) override;
    void placeCompleted(int id, bool keyChanged) override;

private:
    using LineKey = std::pair<const LogFile*, qint64>;

    static LineKey lineKey(const Bookmark& b) { return { b.key.file.get(), b.line }; }
    // Перерисовать отметку строки key: ряд модели и markChanged.
    void notifyKey(const RecordKey& key);
    void scheduleChanged();
    // Привязать закладку к строке row (постановка, повторная постановка):
    // ключ, идентификатор, слежение за недописанной строкой, источник.
    void bindBookmark(int id, Bookmark& b, int row);

    QHash<int, Bookmark> m_items;
    QHash<LineKey, int> m_byLine;
    int m_nextId = 1;
    QTimer m_changedTimer;
};

#endif // BOOKMARKSTORE_H
