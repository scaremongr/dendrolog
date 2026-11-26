#ifndef LOCATEDPLACES_H
#define LOCATEDPLACES_H

#include "recordid.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <functional>

class LogModel;
class RecordLocator;

// ============================================================================
// LocatedPlaces — общая обвязка «мест в логе» вкладки (M-8): закладок
// (BookmarkStore, R-2) и записей истории переходов (NavHistory, R-7).
//
// Место — физическая строка: ключ RecordKey, сохраняемый идентификатор
// RecordId (номер строки + отпечаток текста), путь файла, номер строки и
// фрагмент текста (LocatedPlace). Здесь — то, что у закладок и истории
// одинаково, одной реализацией:
//   • привязка к видимой строке (ключ, идентификатор, фрагмент) и слежение за
//     недописанной последней строкой (RecordLocator::watchCompletion);
//   • токены RecordLocator: запрос → место по id владельца, отпускание
//     (cancel), ответы resolved / refused / completed — общий переход
//     состояния, затем хук владельца;
//   • сброс строк файла и всех строк: сверенное место ищется заново
//     (RecordLocator::reacquire — номер и отпечаток, источник не проверяется),
//     сохранённое место без строки, получившее отказ, — снова request;
//   • «потеряно» (lost) и «не сверено» (verified == false);
//   • обновление фрагментов видимых сверенных мест файла (смена кодировки,
//     дописанная предварительная строка).
//
// Хранит места владелец (у закладок — хэш по id, у истории — вектор): здесь
// только карта «токен → id места». Владелец обязан отпустить токен места
// (release) до того, как место исчезнет; ответ локатора на исчезнувшее место
// (placeById → nullptr) молча отбрасывается.
//
// Цена: на события модели ничего не считается (инвариант 3) — поиск строк
// после сброса ведёт RecordLocator по порциям загрузки. Сброс — O(мест).
// Только GUI-поток. Владелец — LogModel (через наследника): места
// переживают конверсию бэкенда.
// ============================================================================

// Поля состояния (token, verified, lost, source) ведёт только LocatedPlaces;
// копия места снаружи (Bookmark из bookmarks(), NavEntry) — снимок: читать
// можно, полагаться на token и на свежесть verified / lost — нет.
struct LocatedPlace {
    RecordKey key;          // ключ строки на момент последней проверки
    RecordId record;        // номер строки + отпечаток текста (R-1)
    QString filePath;
    qint64 line = 0;        // номер физической строки в файле, с 1
    QString preview;        // первые kPreviewLength символов текста строки
    SourceIdentity source;  // сохранённое место (R-4): источник, который оно
                            // ждёт; у истории не используется
    int token = 0;          // запрос к RecordLocator (поиск, слежение)
    bool verified = true;   // ключ сверен после последнего сброса строк
    bool lost = false;      // строка не найдена (переписана, файл убран)

    // Сверено и не потеряно: ключ указывает на эту строку.
    bool isLive() const { return verified && !lost; }
};

class LocatedPlaces : public QObject {
    Q_OBJECT
public:
    static constexpr int kPreviewLength = 200;

protected:
    explicit LocatedPlaces(LogModel& model);

    LogModel& model() const { return m_model; }
    RecordLocator& locator() const;
    QString previewOf(int row) const;

    // ---- Хуки владельца ----------------------------------------------------
    // Место по id; nullptr — нет такого.
    virtual LocatedPlace* placeById(int id) = 0;
    // Все места владельца (порядок не важен).
    virtual void forEachPlace(const std::function<void(int id, LocatedPlace&)>& fn) = 0;
    // Локатор нашёл строку: ключ, путь, номер уже новые, место сверено.
    // previousKey — ключ до ответа (у сохранённого места — невалидный).
    virtual void placeResolved(int id, const RecordKey& previousKey) = 0;
    // Строка не найдена: место потеряно (verified, lost).
    virtual void placeRefused(int id) = 0;
    // Недописанная строка дописана: отпечаток новый; keyChanged — строка
    // начала запись, ключ другой.
    virtual void placeCompleted(int id, bool keyChanged) = 0;

    // ---- Привязка ----------------------------------------------------------
    // Заполнить место видимой строкой row (ключ, путь, номер, фрагмент,
    // идентификатор; сверено), без запросов к локатору. false — строки нет.
    bool fill(LocatedPlace& p, int row) const;
    // Отпустить прежний запрос, заполнить и следить за недописанной строкой.
    bool bindToRow(int id, LocatedPlace& p, int row);
    // Недописанная последняя строка (без '\n', отпечатка нет): отпечаток —
    // когда её допишут. row — видимая строка места. Есть запрос — ничего.
    void watchCompletion(int id, LocatedPlace& p, int row);
    // Отпустить запрос места (перед удалением, перепривязкой).
    void release(LocatedPlace& p);
    // Строки места сброшены: искать заново (RecordLocator::reacquire).
    void reacquire(int id, LocatedPlace& p);
    // Сохранённое место без строки: ждать файл (RecordLocator::request).
    void requestSaved(int id, LocatedPlace& p);

    // ---- Жизненный цикл данных ----------------------------------------------
    // Строки файла сброшены: его места ищутся заново, сохранённые места с
    // этим путём, получившие отказ, — тоже (файл читается заново).
    void invalidatePlacesOfFile(const QString& filePath);
    // Сброшены все строки.
    void invalidateAllPlaces();
    // Потерянные сохранённые места источника sourceKey проверить снова.
    void recheckSavedSource(const QString& sourceKey);
    // Фрагменты сверенных мест файла, чьи строки видны, — по тексту модели.
    // true — какой-то изменился.
    bool refreshVisiblePreviews(const QString& filePath);

private:
    void track(int id, LocatedPlace& p, int token);
    void onResolved(int token, const RecordKey& key);
    void onRefused(int token);
    void onCompleted(int token, const RecordId& record, const RecordKey& key);

    LogModel& m_model;
    QHash<int, int> m_byToken; // токен локатора → id места
};

#endif // LOCATEDPLACES_H
