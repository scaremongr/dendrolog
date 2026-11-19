#ifndef FILEMEMORYSTORE_H
#define FILEMEMORYSTORE_H

#include "documentstate.h"

#include <QByteArray>
#include <QDateTime>
#include <QFuture>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <functional>
#include <optional>

// ============================================================================
// Память файла на диске (R-4, шаг 2; docs/design/R-5.md, «Три хранилища»,
// «Память файла: ключ и поиск»). Ядро, без виджетов и без QObject.
//
// По файлу памяти на лог в одном каталоге (по умолчанию
// <configDir>/memory/, каталог — параметр: тесту нужен свой). Имя файла — отпечаток нормализованного абсолютного
// пути (fileNameForPath; на Windows без учёта регистра). Что внутри
// (FileMemory): идентичность источника и ручной выбор кодировки, закладки
// файла, ViewState — только если файл был во вкладке один, время последней
// записи (для вытеснения).
//
// Формат (версия 1; эталон — tests/document_state/file-memory-v1.json):
//   {"format":"dendrolog-file-memory","version":1,"hash":"sha1-64",
//    "lastUsed":"2026-11-19T10:00:00.000Z",
//    "source":{SourceIdentity},"encoding":"Windows-1251",
//    "bookmarks":[{BookmarkState}…], "view":{ViewState}}
// Вложенные объекты — форматы R-1 и documentstate со своими version.
// Читатель игнорирует неизвестные ключи; файл БОЛЬШЕЙ версии не читается,
// не перезаписывается, не переносится и не вытесняется (его писала новая
// программа). Битый файл не читается; следующая запись памяти этого пути его
// заменяет — в нём нечего беречь.
//
// Поиск при открытии (open):
//   1. по пути есть память, источник Valid — она и есть (ByPath);
//   2. по пути есть, источник Changed (ротация, перезапись) — память
//      относится к ПРЕЖНЕМУ содержимому: с закладками она отсоединяется
//      (переименовывается в <ключ>~<время>.json и остаётся в каталоге — её
//      найдёт вторичный поиск, когда прежнее содержимое появится под другим
//      именем), без закладок удаляется (Stale);
//   3. по пути нет — вторичный поиск по содержимому: кандидатам каталога
//      SourceIdentity::checkAt(новый путь). Совпало — память переезжает на
//      новый путь (ByContent). Если файл по прежнему пути на месте и всё ещё
//      Valid — это копия, а не перенос: память копируется, прежняя остаётся.
//      Перечень идентичностей каталога читается один раз за жизнь хранилища
//      (дальше ведётся по своим записям); кандидат перечитывается с диска,
//      поэтому устаревший перечень (другой экземпляр, «Clear Memory») не
//      вредит. Пустой источник (размер 0) по содержимому не ищется — с ним
//      совпал бы любой файл.
//
// Запись (save) — в фоне, в одном потоке по порядку (QThreadPool на один
// поток), через QSaveFile: прерванная запись не портит прежний файл.
// open() сперва дожидается записей: только что закрытая вкладка того же
// файла уже на диске.
//
// Несколько экземпляров (--new-instance, второе окно на тот же каталог):
// блокировки нет, последняя запись побеждает — но по закладкам с
// объединением. Пишущий передаёт «базу» — закладки, которые он прочитал из
// памяти или записал в прошлый раз; запись берёт свои закладки плюс те из
// лежащих на диске, которых нет ни в базе, ни в своих (их поставил кто-то
// другой). Удалённые этим окном закладки (были в базе) не воскресают,
// поставленные другим — не теряются. Совпадение закладок — по RecordId
// (номер строки + отпечаток); у лежащих на диске проверяется источник
// (checkAt того же пути = Valid), иначе их файл переписан и они не
// вливаются. ViewState, позиция и фильтры — последней записи; запись без
// ViewState (файл был в слитой вкладке) оставляет лежащий на диске.
//
// Лимит — 200 файлов (параметр): после записи, если файлов больше,
// удаляются давно не записывавшиеся без закладок; память с закладками сама не
// вытесняется (лимит может быть превышен ею).
//
// Потоки: все методы — с одного (GUI) потока; фоновые задачи получают копии.
// ============================================================================

struct FileMemory {
    static constexpr int kFormatVersion = 1;
    static constexpr char kFormatName[] = "dendrolog-file-memory";

    TabSource source;                 // идентичность по концу прочитанного + выбор кодировки
    QVector<BookmarkState> bookmarks; // закладки этого файла
    std::optional<ViewState> view;    // только если файл был во вкладке один
    QDateTime lastUsed;               // время записи (UTC); вытесняются старые

    QJsonObject toJson() const;
    // Чужой format, версия не 1, битые поля — nullopt и причина. newerVersion
    // (если передан) — true, когда отказ из-за большей версии.
    static std::optional<FileMemory> fromJson(const QJsonObject& json, QString* error = nullptr,
                                              bool* newerVersion = nullptr);

    // Подменить путь источника (переезд, копия): у источника, закладок этого
    // файла и текущей строки.
    void relocate(const QString& newPath);
    // Снимок вкладки из одного файла (для LogViewWidget::applyState).
    TabState toTabState() const;
};

class FileMemoryStore {
public:
    static constexpr int kDefaultLimit = 200;

    enum class Found {
        None,      // памяти нет
        ByPath,    // случай 1
        ByContent, // случай 3: переехала или скопирована с другого пути
        Stale,     // случай 2: память прежнего содержимого (отсоединена или удалена)
        Refused,   // файл памяти большей версии — не тронут
    };
    struct Lookup {
        Found found = Found::None;
        // ByPath, ByContent — память с путём открываемого файла; Stale — память
        // прежнего содержимого как была (закладки показать серыми).
        std::optional<FileMemory> memory;
        QString previousPath; // ByContent — откуда память
        bool copied = false;  // ByContent — прежняя осталась на месте
    };

    explicit FileMemoryStore(const QString& directory, int limit = kDefaultLimit);
    ~FileMemoryStore(); // дожидается записей
    FileMemoryStore(const FileMemoryStore&) = delete;
    FileMemoryStore& operator=(const FileMemoryStore&) = delete;

    QString directory() const { return m_dir; }
    int limit() const { return m_limit; }

    // Нормализованный абсолютный путь (без «.»/«..», прямые слеши; на Windows
    // — в нижнем регистре) и имя файла памяти для него: 16 hex SHA-1 + .json.
    // Путь другой ОС (SavedPath::isForeign; каталог памяти с другой машины,
    // R-8) не достраивается от текущего каталога: ключ — он сам, ни с одним
    // локальным не совпадает. Такая память читается (не битая и не
    // удаляется как битая), по пути не находится, по содержимому — да.
    static QString normalizedKeyPath(const QString& path);
    static QString fileNameForPath(const QString& path);

    // Найти память открываемого файла (три случая выше). Путь — что открывает
    // вкладка (абсолютный). Спул stdin и нечитаемый файл — None.
    Lookup open(const QString& path);

    // Записать память файла memory.source.source.path. base — закладки,
    // которые пишущий прочитал из памяти или записал прошлый раз (правило
    // нескольких экземпляров). Ephemeral не пишется.
    void save(const FileMemory& memory, const QVector<RecordId>& base);
    // Дождаться фоновых записей (выход из программы, тесты).
    void waitForWrites();

    // Удалить всю память (Settings → Clear Memory). Файлы большей версии
    // тоже: пользователь просил очистить всё. Количество удалённых.
    int clear();
    // То же для каталога без живого хранилища.
    static int clearDirectory(const QString& directory);

    // Файлов памяти в каталоге сейчас (с диска; тесты и настройки).
    int fileCount() const;

    // Атомарная запись (QSaveFile). abortAfterBytes ≥ 0 — оборвать запись
    // после стольких байтов, как при сбое: прежний файл не меняется (тест).
    static bool writeFileAtomically(const QString& filePath, const QByteArray& bytes,
                                    qint64 abortAfterBytes = -1);

private:
    struct Entry {
        SourceIdentity source;
        QDateTime lastUsed;
        int bookmarks = 0;
    };

    QString filePath(const QString& fileName) const;
    void ensureIndex();
    void enqueue(std::function<void()> task);
    void evictIfNeeded(const QString& keep);
    std::optional<FileMemory> readMemory(const QString& fileName, bool* newer = nullptr) const;

    QString m_dir;
    int m_limit;
    bool m_indexed = false;
    QHash<QString, Entry> m_index; // имя файла → что в нём
    QThreadPool m_pool;
    QVector<QFuture<void>> m_writes;
};

#endif // FILEMEMORYSTORE_H
