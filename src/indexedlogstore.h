#ifndef INDEXEDLOGSTORE_H
#define INDEXEDLOGSTORE_H

#include "lineindex.h"
#include "logpattern.h"
#include "logstore.h"
#include "textchunkcache.h"

#include <QFuture>
#include <QSet>
#include <atomic>

class QTimer;
class SequentialLineReaders;

// ============================================================================
// IndexedLogStore — бэкенд очень больших файлов: текст на диске, в памяти
// только LineIndex (~10 байт/строку). Строки адресуются RowRef =
// (fileId << 40) | lineNo; для одно-файловой вкладки список всех строк НЕ
// материализуется (тождественное отображение row == line — доминирующий
// 20-ГБ случай стоит 0 байт), при нескольких файлах строки сливаются по
// времени в m_allRefs. Пустой активный фильтр — тоже тождество
// (m_identityVisible), фильтр всегда считается АСИНХРОННО (он читает диск).
//
// Мутации приходят от LogViewWidget по сигналам LogIndexer:
//   attachFile → appendIndexedRows(батчи) → … reload: appendIndexedRows с
//   перекрытием одной строки (переиндексация предварительного хвоста) или
//   resetFileIndex (файл заменён).
// ============================================================================
class IndexedLogStore final : public LogStore {
public:
    using RowRef = quint64;
    static constexpr int kFileBits = 40; // строк на файл: 2^40

    explicit IndexedLogStore(LogModel& model);
    // Search-result base: share indices and the source's visible row references,
    // never materialize text. No rows are shown until applyFilter completes.
    IndexedLogStore(LogModel& model, const IndexedLogStore& source);
    ~IndexedLogStore() override;

    // ---- Живая поисковая база (созданная конструктором выше) ------------------
    // Строки source [first, last] стали видимыми (дозапись, инкрементальный
    // фильтр): дописать их в базу и проверить запросом только их, результат —
    // вставками без reset. false — так нельзя (у источника сменился порядок
    // строк или набор файлов), нужен полный поиск.
    bool appendSearchRows(const IndexedLogStore& source, int first, int last);
    // Текст видимой строки source изменился (переиндексирован хвост без '\n'):
    // переоценить её запросом. false — нужен полный поиск.
    bool refreshSearchRow(const IndexedLogStore& source, int row);

    Backend backend() const override { return Backend::Indexed; }

    // ---- Подключение файлов (GUI-поток, из LogViewWidget) --------------------
    int attachFile(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index);
    // Очередной опубликованный диапазон строк файла. Перекрытие с уже
    // показанными строками (переиндексированный предварительный хвост)
    // обрабатывается как dataChanged первой строки + вставка остальных.
    // markNew — пометить строки зелёным маркером гаттера (tail-догрузка).
    void appendIndexedRows(const LogFilePtr& logFile, qint64 firstLine,
                           qint64 count, bool markNew);
    // Файл заменён: сброс его строк и индекса (свежий index начнёт с нуля).
    void resetFileIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> fresh);
    std::shared_ptr<LineIndex> indexForFile(const QString& filePath) const;

    // Схема полей: для отображения/фильтрации извлекается по требованию.
    void setFieldPattern(const QString& patternString, bool extractionEnabled);
    void setTextCacheBudget(qint64 bytes);

    // ---- Виртуальный интерфейс LogStore ---------------------------------------
    qint64 allCount() const override;
    int visibleCount() const override;
    int uniqueSourceFileCount() const override { return int(m_files.size()); }
    std::shared_ptr<LogEntry> entryAt(int visibleRow) const override;
    LogEntryMeta visibleMetaAt(int row) const override;
    QDateTime visibleTimestampAt(int row) const override;
    LogLevel visibleLevelAt(int row) const override;
    QString messageAt(int visibleRow) const override;
    int rawTextLengthAt(int visibleRow) const override;
    bool isNewAt(int visibleRow) const override;
    int firstVisibleRowAtOrAfter(const QDateTime& t) const override;
    QPair<QDateTime, QDateTime> fullTimeRange() const override;
    QStringList sampleMessages(int maxCount) const override;
    QVector<std::shared_ptr<LogEntry>> logicalRecordLines(
        const std::shared_ptr<LogEntry>& line, int maxLines) const override;
    int rowForEntry(int logicalEntryId, const LogFile* sourceFile) const override;
    int nearestVisibleRow(int logicalEntryId, const LogFile* sourceFile) const override;
    int findNextOccurrence(const QString& text, int startRow,
                           Qt::CaseSensitivity cs, bool wrapAround) const override;
    int findPreviousOccurrence(const QString& text, int startRow,
                               Qt::CaseSensitivity cs, bool wrapAround) const override;
    LogScanSnapshot scanSnapshot(bool filteredOnly) const override;
    void applyFilter() override;
    void cancelPendingFilter(bool wait) override;
    void reapplyFilterIfStale() override;
    bool isFiltering() const override { return m_filterJobActive; }

private:
    struct IndexedFile {
        LogFilePtr logFile;
        std::shared_ptr<LineIndex> index;
        int cacheFileId = -1;
    };

    static RowRef makeRef(int fileId, qint64 line)
    {
        return (RowRef(quint32(fileId)) << kFileBits) | RowRef(line);
    }
    static int refFile(RowRef ref) { return int(ref >> kFileBits); }
    static qint64 refLine(RowRef ref) { return qint64(ref & ((RowRef(1) << kFileBits) - 1)); }

    // Тождественный режим — полный один файл: row == line, m_allRefs пуст.
    // Решают число файлов и явная поисковая база, а не пустота m_allRefs:
    // файлы, подключённые до первого батча, оставляли m_allRefs пустым, и
    // стор считал вкладку одно-файловой — allCount() отдавал 0 строк.
    bool identityAll() const { return m_files.size() <= 1 && !m_explicitBase; }
    RowRef rowToRef(int visibleRow) const;
    // Глобальный порядок строк — зеркало LogEntry::operator< по метаданным.
    bool lessRef(RowRef a, RowRef b) const;
    // Порядок строк ВКЛАДКИ, в котором лежат m_allRefs и m_visibleRefs: один
    // файл — порядок файла (включая поисковую базу), иначе lessRef. Все
    // бинарные поиски и слияния по этим спискам обязаны идти по нему — lessRef
    // на одно-файловой вкладке с метками не по порядку промахивается.
    bool rowLess(RowRef a, RowRef b) const { return m_files.size() <= 1 ? a < b : lessRef(a, b); }
    // Номер файла вкладки; -1, если такой файл не подключён.
    int fileIdOf(const LogFile* file) const;
    // Первая строка логической записи id в файле (logicalId по строкам не
    // убывает — двоичный поиск); -1, если записи в файле нет.
    static qint64 firstLineOfRecord(const LineIndex& index, quint32 id);
    QStringList filePaths() const;
    // Текст видимой строки для синхронного скана (быстрый поиск): через
    // ридеры скана, чтобы не вымывать кэш вьюпорта.
    const QString& scanTextAt(SequentialLineReaders& readers, int visibleRow) const;
    bool hasActiveFilterSettings() const;
    // Проверка одной строки текущими настройками (GUI, точечно: хвост).
    bool refPassesFiltersNow(RowRef ref) const;
    // Переоценить одну строку при активном фильтре: вставить, убрать или
    // перерисовать её в m_visibleRefs.
    void reevaluateVisibleRef(RowRef ref);
    // Переход «один файл → несколько»: m_allRefs из показанных строк первого
    // файла, в порядке lessRef (sortByTime — порядок файла с ним расходится).
    void materializeAllRefs(bool sortByTime);
    struct PendingRange;
    void startFilterJob(bool fullRescan, const PendingRange* range = nullptr);
    // Вставить строки в ВИДИМЫЙ список list (m_visibleRefs или, без фильтра
    // на слитой вкладке, m_allRefs) сериями beginInsertRows; сотни серий —
    // одним reset. Уже присутствующие строки пропускаются.
    void insertSortedRefs(QVector<RowRef>& list, const QVector<RowRef>& refs);
    void startNextPendingRange();
    std::shared_ptr<LogEntry> materializeEntry(RowRef ref) const;

    QVector<IndexedFile> m_files;
    mutable TextChunkCache m_textCache;

    // Строк ПОКАЗАНО модели (тождественный режим одного файла). Индекс растёт
    // в воркере раньше уведомления — rowCount() обязан отражать состояние
    // между begin/endInsertRows, а не живой lineCount() индекса.
    qint64 m_shownAllCount = 0;

    QVector<RowRef> m_allRefs;     // пуст для одного файла (тождество)
    // A filtered single-file search base also uses m_allRefs, in FILE order.
    // Mapping identity and chronological ordering are separate properties.
    bool m_explicitBase = false;
    QVector<RowRef> m_visibleRefs; // действителен, когда !m_identityVisible
    bool m_identityVisible = true; // нет активного фильтра — видно всё

    QSet<RowRef> m_newRefs;        // строки последнего tail-батча (IsNewRole)

    LogPattern m_fieldPattern;
    bool m_extractionEnabled = false;

    // ---- Асинхронная фильтрация (поколение/cancel — как у резидентного) ------
    // Диапазон строк одного файла либо (refs не пуст) явный список строк в
    // порядке вкладки — новые строки поисковой базы.
    struct PendingRange {
        int fileId = -1;
        qint64 first = 0;
        qint64 count = 0;
        QVector<RowRef> refs;
    };
    int m_filterGeneration = 0;
    bool m_filterJobActive = false;
    bool m_filteredListStale = false;
    std::shared_ptr<std::atomic_bool> m_filterJobCancel;
    QFuture<QVector<RowRef>> m_filterJobFuture;
    QVector<PendingRange> m_pendingRanges; // FIFO инкрементальных диапазонов
    std::shared_ptr<std::atomic<int>> m_filterProgress;
    QTimer* m_progressTimer = nullptr; // parent — LogModel
    std::shared_ptr<int> m_aliveGuard = std::make_shared<int>(0);
};

#endif // INDEXEDLOGSTORE_H
