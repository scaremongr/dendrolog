// logviewwidget.h
#ifndef LOGVIEWWIDGET_H
#define LOGVIEWWIDGET_H

#include "textquery.h"
#include <QWidget>
#include <QVBoxLayout>
#include "logmodel.h"
#include "logparser.h"
#include "logindexer.h"
#include <QVector>
#include <QSet>
#include <QStringConverter>
#include "logfile.h"
#include "LogListView.h"
#include "filechangedetector.h"
#include "bookmarkstore.h"

class QuickSearch;

class LogViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit LogViewWidget(QWidget *parent = nullptr);
    ~LogViewWidget();

    LogModel*    model()    const { return m_model; }
    LogListView* view()     const { return m_view;  }

    // Добавляет файл в текущий view
    void addLogFile(const QString &filePath);
    
    // Возвращает список загруженных файлов
    QList<LogFilePtr> loadedFiles() const { return m_loadedFiles.toList(); }
    
    // Возвращает количество загруженных файлов
    int fileCount() const { return m_loadedFiles.size(); }

    // Проверяет, изменились ли файлы на диске, и дочитывает новые данные.
    // Возвращает true если хотя бы один файл был обновлён.
    // force (ручной F5) дополнительно повторяет попытку по файлам, чью
    // прошлую загрузку сорвало и на диске с тех пор ничего не изменилось.
    bool reloadChangedFiles(bool force = false);

    // Per-tab auto-reload. Default is taken from AppSettings at construction time.
    bool autoReload() const { return m_autoReload; }
    void setAutoReload(bool enabled) { m_autoReload = enabled; }

    // Идёт ли по вкладке фоновая загрузка данных: первичный разбор/индексация
    // либо дочитывание хвоста. Потребители (панель статистики) используют это,
    // чтобы не считать по заведомо неполному документу.
    bool isLoading() const { return m_loading; }

    // Задаёт схему извлечения блоков для парсера (применяется к следующим загрузкам).
    void setParserPattern(const QString& pattern);
    void setExtractionEnabled(bool enabled);
    // Запасная кодировка автоопределения (F-1): однобайтная, которую
    // индексатор и резидентный парсер выбирают, если окно у первого байта
    // ≥ 0x80 не похоже на UTF-8. Действует на проходы, начатые после вызова,
    // — новые файлы и дозапись файлов, кодировка которых ещё не решена;
    // решённые не меняются. Умолчание — по локали системы.
    void setFallbackEncoding(TextEncoding encoding);
    // Кодировка файла вкладки и откуда она (F-1) — на любом бэкенде: у
    // индексного её держит стор, у резидентного — вкладка (m_fileEncodings).
    // Файл UTF-16/32 (только резидентный путь) — «не решено»; как он читается,
    // говорит fileStreamEncoding.
    FileEncoding fileEncoding(const LogFilePtr& logFile) const;
    // Кодировка, в которой байты файла превращаются в текст: UTF-16/32 по
    // BOM, иначе кодировка декодера файла (UTF-8, пока «не решено»).
    TextEncoding fileStreamEncoding(const LogFilePtr& logFile) const;
    // Выбор кодировки пользователем (F-1; интерфейс — шаг 4). Индексная
    // вкладка — IndexedLogStore::setFileEncoding, без переиндексации.
    // Резидентная — файл перечитывается этой кодировкой в сторону, и его
    // записи подменяются под одним reset (startReencode): текущая запись,
    // выделение и позиция остаются. Загрузка в полёте — сразу после неё. Выбор живёт, пока открыта
    // вкладка: переживает дозапись, перечитывание переписанного файла и
    // переход порога в обе стороны. UTF-16/32 и недекодируемые кодировки
    // игнорируются.
    void setFileEncoding(const LogFilePtr& logFile, TextEncoding encoding);
    QString parserPattern() const;
    QStringList parserFieldNames() const;

    // Быстрый поиск (F3 / Shift+F3) — в фоне (QuickSearch): найденная строка
    // становится текущей, когда скан закончится. Новый поиск отменяет прежний.
    // Невалидный запрос (TextQuery::isValid) не запускается — проверяет вызывающий.
    void searchTextNext(const TextQuery& query);
    void searchTextPrevious(const TextQuery& query);
    void cancelQuickSearch();
    // Запрос последнего быстрого поиска вкладки.
    const TextQuery& quickSearchQuery() const { return m_quickSearchQuery; }
    bool isQuickSearchRunning() const;

    // Переход к записи (из Search Results, из статистики) как выбор
    // пользователя (LogListView::goToRow). Запись скрыта фильтром — к ближайшей
    // видимой строке (LogModel::nearestVisibleRow); false — перейти некуда.
    bool goToEntry(int logicalEntryId, const LogFile* sourceFile);

    // Переход к следующей / предыдущей видимой записи с уровнем из levels
    // (F8 / Shift+F8; к первой показанной строке записи) от строки, выбранной
    // пользователем; через край — заворачивает. Переход — выбор пользователя
    // (LogListView::goToRow).
    // Небольшая вкладка ищется сразу (LogModel::findRowWithLevel), большая —
    // в фоне по снапшоту (QuickSearch::startLevel): проход по сотням
    // миллионов строк на GUI-потоке был бы фризом. Итог в обоих случаях —
    // levelJumpFinished; новый переход отменяет прежний.
    enum class LevelJumpOutcome {
        Moved,        // перешли к следующей по направлению
        Wrapped,      // дальше таких нет — перешли через край вкладки
        OnlyCurrent,  // единственная такая строка — та, на которой стоим
        NotFound,     // среди видимых строк таких нет
        Interrupted,  // модель перестроилась посреди фонового поиска
    };
    void goToLevel(LogLevelMask levels, bool forward);
    void cancelLevelJump();

    // ---- Закладки (R-2) ---------------------------------------------------------
    // Хранилище закладок вкладки — у модели (BookmarkStore).
    BookmarkStore& bookmarks() const { return m_model->bookmarks(); }
    // Поставить / снять закладку на видимой строке; true — теперь стоит.
    bool toggleBookmark(int row);
    // Ctrl+F2: на строке, выбранной пользователем (инвариант 10); false —
    // строка не выбрана.
    bool toggleBookmarkAtSelection();
    // F2 / Shift+F2: к следующей / предыдущей видимой закладке от выбранной
    // строки, через край — заворачивает (как F8). Итог — bookmarkJumpFinished.
    void goToBookmark(bool forward);
    // Переход к закладке из панели: видна — к ней, скрыта фильтром — к
    // ближайшей видимой строке; false — строки нет (Pending, Missing) или не
    // видно ни одной строки. Переход — выбор пользователя (goToRow).
    bool goToBookmarkId(int id);
    // Вкладка не больше стольких видимых строк ищется синхронно.
    void setLevelJumpSyncRows(int rows) { m_levelJumpSyncRows = rows; }

signals: // Сигналы, которые LogViewWidget будет пробрасывать для MainWindow
    void fileParsingStarted(const LogFilePtr& logFile);
    void fileParsingProgress(const LogFilePtr& logFile, int progressPercentage);
    void fileParsingFinished(const LogFilePtr& logFile, int totalEntries);
    void fileParsingFailed(const LogFilePtr& logFile);

    // Emitted when an incremental reload finishes (new entry count passed).
    void reloadFinished(int newEntriesCount);

    // Началась/закончилась фоновая загрузка данных вкладки (см. isLoading()).
    void loadingChanged(bool loading);

    // Новые сигналы для информации о строках
    void totalRowCountChanged(int totalRows);
    void currentRowChanged(int currentRow, int totalRows); // Передает и текущую, и общую
    void modelFiltered(int totalRowsAfterFilter); // Signal when model has been filtered

    // Быстрый поиск: прогресс долгого скана и итог (отменённый поиск итога
    // не присылает).
    void quickSearchProgress(const QString& term, int percent);
    void quickSearchFinished(const QString& term, bool found);
    // Переход по уровню: прогресс долгого скана и итог.
    void levelJumpProgress(LogLevelMask levels, bool forward, int percent);
    void levelJumpFinished(LogLevelMask levels, LogViewWidget::LevelJumpOutcome outcome);
    // Итог F2 / Shift+F2 (Interrupted не бывает: поиск синхронный).
    void bookmarkJumpFinished(bool forward, LogViewWidget::LevelJumpOutcome outcome);
    // Пункт контекстного меню «заметка к закладке»: закладка уже стоит,
    // заметку спрашивает окно. created — закладку поставил сам этот пункт:
    // отказ от заметки её снимает.
    void bookmarkNoteEditRequested(int bookmarkId, bool created);

private slots:
    void handleEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& parsedLogFile);
    void handleParsingFinished(int totalEntries, const LogFilePtr& parsedLogFile);
    void handleParsingFailed(const LogFilePtr& parsedLogFile);
    void handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile);
    void handleModelFilteredRelay(int totalRowsAfterFilter);
    void handleQuickSearchFinished(int row);
    // Incremental reload: append new entries without model reset
    void handleIncrementalEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr& logFile);
    void handleIncrementalParsingFinished(int totalEntries, const LogFilePtr& logFile);
    void handleIncrementalParsingFailed(const LogFilePtr& logFile);
    // Индексный бэкенд: приём опубликованных порций строк и завершения.
    void handleIndexBatchReady(const LogFilePtr& logFile, qint64 firstLine, qint64 count);
    void handleIndexingFinished(qint64 newLines, const LogFilePtr& logFile);
    void handleIndexingFailed(const LogFilePtr& logFile);
    void handleResidentFallback(const LogFilePtr& logFile, const QString& reason);

private:
    // Ленивая инициализация LogIndexer с подключением сигналов.
    void ensureIndexer();
    // Запуск полной индексации файла (store уже индексный). encoding —
    // кодировка, уже решённая для файла резидентным путём (переход порога):
    // индексатор и стор начинают с неё, а не определяют заново.
    void startIndexedLoad(const LogFilePtr& logFile, FileEncoding encoding = {});
    // Состояние кодировок резидентных файлов → индексная вкладка и обратно:
    // одна точка на каждый переход бэкенда, чтобы решение не терялось.
    QHash<QString, FileEncoding> takeEncodingsForIndexed();
    void captureEncodingsFromIndexed();
    // Файл переписан (полное перечитывание): решение автоматики относится к
    // старому содержимому — «не решено»; выбор пользователя остаётся (как
    // IndexedLogStore::resetFileIndex).
    void forgetAutoEncoding(const QString& filePath);
    // Смена кодировки резидентного файла: полный разбор отдельным парсером в
    // буфер, затем LogModel::replaceEntriesForFile (один reset, якорь view
    // переживает — в отличие от startFullReload, где строки сначала
    // удаляются). Загрузка файла в полёте на всё время разбора.
    void startReencode(const LogFilePtr& logFile);
    void finishReencode(const LogFilePtr& logFile);
    void handleReencodeFailed(const LogFilePtr& logFile);
    LogParser* m_reencodeParser = nullptr; // ленивый
    QHash<QString, QVector<std::shared_ptr<LogEntry>>> m_reencodeBuffers;

    // Дешёвая подпись файла на диске: пара (размер, mtime). Нужна только
    // чтобы понять «изменилось ли на диске хоть что-то» после неудачной
    // загрузки — иначе нечитаемый (или всё ещё удалённый) файл перечитывался
    // бы на каждый тик авто-обновления. size < 0 означает «файла нет».
    struct DiskStamp {
        qint64 size    = -1;
        qint64 mtimeMs = -1;
        bool exists() const { return size >= 0; }
        bool operator==(const DiskStamp& o) const
        { return size == o.size && mtimeMs == o.mtimeMs; }
        static DiskStamp of(const QString& filePath);
    };

    // Пересчитывает агрегат «идёт фоновая загрузка» по состояниям файлов и
    // сигналит об изменении. Вызывается в конце каждого события, способного
    // это состояние изменить (старт загрузки, её завершение или обрыв).
    void updateLoadingState();

    // Сбрасывает уже показанные строки файла (файл исчез или переписан).
    void dropRowsForFile(const LogFilePtr& logFile);
    // Полная перезагрузка файла с нуля: строки сбрасываются и файл читается
    // заново. Используется, когда файл переписан, обрезан или удалён и создан
    // заново. Состояние файла обнуляется и помечается «загрузка в полёте».
    void startFullReload(const LogFilePtr& logFile);
    // Загрузка файла закончилась — закладки его строк сверяются (fullLoad —
    // файл прочитан с нуля).
    void bookmarksFileLoaded(const LogFilePtr& logFile, bool fullLoad);

    LogListView *m_view;
    LogModel    *m_model;
    QVector<LogFilePtr> m_loadedFiles;  // Список загруженных файлов

    // Быстрый поиск и его запрос — чтобы повторить, если модель перестроится
    // посреди скана.
    void startQuickSearch();
    QuickSearch* m_quickSearch = nullptr;
    TextQuery m_quickSearchQuery;
    bool m_quickSearchForward = true;

    void finishLevelJump(int row, int startRow);
    QuickSearch* m_levelJump = nullptr;
    LogLevelMask m_levelJumpLevels = 0;
    bool m_levelJumpForward = true;
    int m_levelJumpSyncRows = 1000000;

    LogParser* m_logParser;

    // Per-file reload state: the anchor fingerprinting the already-consumed
    // prefix (used to tell appends from rewrites) and the next logicalEntryId.
    //
    // Инварианты (проверяются в reloadChangedFiles):
    //   loadInFlight            — фоновый джоб пишет данные этого файла;
    //                             второй по нему не запускаем;
    //   !initialLoadDone && !loadInFlight — файла нет либо загрузка сорвалась;
    //                             failedStamp хранит то, что мы видели на диске,
    //                             и повтор случится, когда подпись изменится.
    struct FileReloadState {
        FileChangeDetector::Anchor anchor;        // prefix size + boundary fingerprint
        int    nextLogicalEntryId = 0;
        bool   initialLoadDone = false;           // файл успешно прочитан целиком
        // Загрузка (полная или дозапись) в полёте: вторую по тому же файлу не
        // запускаем — LineIndex single-writer, а резидентный ре-парс иначе
        // продублировал бы записи.
        bool   loadInFlight = false;
        // Ни один бэкенд не тянет этот файл (например UTF-16 во вкладке с
        // несколькими файлами) — повторять загрузку бессмысленно.
        bool   unsupported = false;
        // Подпись файла на момент исчезновения/неудачной загрузки.
        DiskStamp failedStamp;
        // Резидентный путь (LogParser::tailState): где кончилось прочитанное,
        // и не оборвана ли последняя строка — тогда она показана
        // предварительно и при дозаписи читается заново с lastLineStart.
        qint64 endOffset = -1;
        bool   lastLinePartial = false;
        qint64 lastLineStart = -1;
        // Как читались байты (LogParser::tailState): UTF-16/32 по BOM — у
        // дозаписи BOM нет, она читается в той же кодировке; иначе —
        // кодировка декодера файла. Декодирует однобайтный хвост состояние
        // файла (m_fileEncodings), не это поле.
        TextEncoding encoding = TextEncoding::Utf8;
        // Пользователь сменил кодировку, пока файл читался: перечитать его,
        // как только загрузка закончится.
        bool   reparsePending = false;
        // Две последние строки файла: контекст дочитывания (продолжение в
        // начале дозаписи принадлежит последней записи) и предварительная
        // строка, которую надо убрать перед перечитыванием.
        std::shared_ptr<LogEntry> lastEntry;
        std::shared_ptr<LogEntry> prevEntry;
    };
    QHash<QString, FileReloadState> m_fileReloadStates; // key = filePath
    static void rememberLastLines(FileReloadState& state,
                                  const QVector<std::shared_ptr<LogEntry>>& batch);
    void handleTailState(const LogFilePtr& logFile, qint64 endOffset,
                         bool lastLinePartial, qint64 lastLineStart, TextEncoding encoding);
    // Резидентный путь: кодировка файла и откуда она (F-1). Ключ — путь
    // файла. Переживает сброс FileReloadState (перечитывание, дозапись) и
    // переходы бэкенда: при конвертации в индексный путь уходит в стор
    // (takeEncodingsForIndexed), при откате — возвращается из стора. У
    // индексной вкладки не используется: там состояние держит стор.
    QHash<QString, FileEncoding> m_fileEncodings;
    void handleEncodingDecided(const LogFilePtr& logFile, const FileEncoding& encoding);
    // Файлы, которые индексный путь вернул в резидентный (UTF-16/32): по
    // порогу размера вкладка с таким файлом не конвертируется — индексатор
    // откатил бы её снова, и файл перечитывался бы целиком на каждый тик.
    // Отметка относится к содержимому: переписанный файл её теряет.
    QSet<QString> m_residentOnlyFiles;

    // Parser used exclusively for incremental reads (so it doesn't interfere with
    // the initial parse that uses m_logParser).
    LogParser* m_reloadParser = nullptr;

    // Индексатор больших файлов (ленивый: создаётся при первом индексном файле).
    LogIndexer* m_indexer = nullptr;
    // Зеркало флага извлечения полей (для индексатора/индексного store).
    bool m_extractionEnabled = false;
    TextEncoding m_fallbackEncoding = TextEncodings::systemFallback();

    // Per-tab auto-reload flag. Defaults to the global AppSettings value at construction.
    bool m_autoReload = false;

    // Кэш агрегата loadInFlight по всем файлам вкладки (см. updateLoadingState).
    bool m_loading = false;
};

#endif // LOGVIEWWIDGET_H
