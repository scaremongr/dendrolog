#ifndef LOGPARSER_H
#define LOGPARSER_H

#include "logentry.h"
#include "logpattern.h"
#include "lineclassifier.h"
#include "textencoding.h"
#include <QVector>
#include <QString>
#include <QStringConverter>
#include <QDateTime>
#include <QThreadPool>
#include <atomic>
#include <memory>
#include <QObject>

class LogParser : public QObject
{
    Q_OBJECT

public:
    explicit LogParser(QObject* parent = nullptr);
    ~LogParser() override;

    // Счётчики файла для сканера каталога. Записи — ЛОГИЧЕСКИЕ, по тому же
    // правилу, что у вкладки без схемы (LineClassifier::isPrimaryLine: метка
    // времени с уровнем или ведущая метка); строки-продолжения (стек-трейс) записью не
    // считаются. W/E/F — записи этого уровня, From/To — метки первичных строк.
    struct FileStats {
        qint64 fileSize = 0;
        int totalEntries = 0;
        QDateTime firstEntryTimestamp;
        QDateTime lastEntryTimestamp;
        int warnCount = 0;
        int errorCount = 0;
        int fatalCount = 0;
        bool parseSuccess = true; 
    };

    // Откуда продолжать разбор для счётчиков: сырое байтовое смещение начала
    // строки, кодировка файла (у хвоста нет BOM — без неё UTF-16 читался бы
    // как UTF-8) и есть ли до смещения запись, которую продолжат строки без
    // метки. По умолчанию — начало файла, кодировка — по BOM.
    struct StatsResume {
        qint64 offset = 0;
        QStringConverter::Encoding encoding = QStringConverter::Utf8;
        bool inRecord = false;
        // Дата времени без даты (F-11) на смещении: хвост продолжает её, как
        // дозапись вкладки. Не задана — дни считаются от предварительной
        // опоры и в конце разбора сдвигаются так, чтобы последняя такая
        // строка получила дату по mtime: то же, что у вкладки с предпросмотром.
        ClockDateState clock;
    };

    // Итог разбора: `stats` — все прочитанные строки, включая последнюю без
    // перевода строки (её вкладка тоже показывает); `complete` — только целые
    // строки; `next` — продолжение сразу за последней целой строкой. Оборванная
    // строка при дозаписи перечитывается с начала, поэтому база для слияния —
    // `complete`, а не `stats`. При fromOffset > 0 счётчики описывают только
    // прочитанный кусок — их складывает mergeStats.
    struct StatsScan {
        FileStats stats;
        FileStats complete;
        StatsResume next;
    };

    // This method can be called from a separate thread for stat analysis.
    // Файл читается БЕЗ QIODevice::Text: все смещения — сырые байты. Сжатый
    // (сигнатура gzip, F-2) читается потоком через GzipDevice: продолжения с
    // середины нет (next.offset = 0, запрос с from.offset > 0 — parseSuccess
    // = false), повреждённый поток — parseSuccess = false; fileSize — размер
    // на диске.
    //
    // Строка длиннее kMaxStatsLineBytes считается по голове этой длины
    // (бинарный файл без '\n' или `.gz`-«бомба» не растит буфер без предела).
    // cancelled (может быть nullptr) проверяется между кусками по 1 МБ и
    // внутри распаковки; прерванный разбор — parseSuccess = false.
    static constexpr qsizetype kMaxStatsLineBytes = 4 * 1024 * 1024;
    StatsScan analyzeFileForStats(const QString& filePath, const StatsResume& from,
                                  const std::atomic<bool>* cancelled = nullptr) const;
    StatsScan analyzeFileForStats(const QString& filePath) const
    {
        return analyzeFileForStats(filePath, StatsResume());
    }
    // Счётчики головы [0, X) плюс хвоста, прочитанного с X.
    static FileStats mergeStats(const FileStats& head, const FileStats& tail);

    // Optional: configure a dynamic block schema so that the parser extracts
    // structured fields into LogEntry::fields for every primary line.
    // The string may be either the new serialized schema format or a legacy
    // log4cxx/log4j conversion pattern, which will be migrated on read.
    // Passing an empty string clears structured extraction.
    void setPattern(const QString& schemaString);
    void setExtractionEnabled(bool enabled) { m_extractionEnabled = enabled; }
    const LogPattern& pattern() const noexcept { return m_pattern; }

    // Where an incremental parse continues from: the file's last record (a
    // continuation line at the start of the new bytes belongs to it) and the
    // numbering. Default — the start of a file.
    struct ResumeContext {
        int nextLogicalEntryId = 0;         // id of the next primary line
        int currentLogicalEntryId = -1;     // -1: no record yet
        QDateTime currentTimestamp;         // of the current record
        LogLevel currentLevel = LogLevel::Unknown;
        int nextLineNumber = 1;             // 1-based line number in the file
    };

    // Кодировка файлов не в UTF-8, на которую автоопределение отступает
    // (F-1; как LogIndexer::setFallbackEncoding — вкладка задаёт обоим одну).
    // Применяется к проходам, начатым после вызова.
    void setFallbackEncoding(TextEncoding encoding) { m_fallbackEncoding = encoding; }
    TextEncoding fallbackEncoding() const { return m_fallbackEncoding; }

public slots:
    // Полный разбор файла. encoding — состояние кодировки файла на старте
    // (F-1): по умолчанию «не решено»; выбор пользователя (Source::User)
    // переживает перечитывание и автоматикой не меняется.
    void startParsing(const LogFilePtr& logFile, FileEncoding encoding = {});
    // Incremental parse: read only bytes starting at startOffset (a line
    // boundary), continuing the numbering and the current record of context.
    // streamEncoding — то, что сообщил tailState: у дозаписи нет BOM, поэтому
    // UTF-16/32 продолжают читаться своей кодировкой (QTextStream). Для
    // остальных (UTF-8 и однобайтные) строки — байты, декодируемые по
    // encoding — состоянию файла; «не решено» переживает дозапись, и
    // решение примет первая не-ASCII строка хвоста.
    void startParsingFrom(const LogFilePtr& logFile, qint64 startOffset,
                          const ResumeContext& context,
                          TextEncoding streamEncoding = TextEncoding::Utf8,
                          FileEncoding encoding = {});

signals:
    void parsingStarted(const LogFilePtr& logFile);
    // Автоопределение решило кодировку файла (F-1) — тем же правилом, что
    // LogIndexer (EncodingDetector::advance), и так же: до порции с первой
    // не-ASCII строкой. BOM UTF-8 — решение Source::Bom. Вкладка хранит
    // состояние и передаёт его следующему проходу (дозапись, перечитывание,
    // переход порога в индексный бэкенд).
    void encodingDecided(const LogFilePtr& logFile, const FileEncoding& encoding);
    void entriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& logFile);
    // Right before parsingFinished of a successful parse: the exact byte
    // offset reading stopped at (the next incremental parse starts there), and
    // whether the last line had no newline yet — then lastLineStart is its
    // byte offset, to read it again once the writer completes it. Both offsets
    // fall on a code unit boundary (UTF-16: even, UTF-32: multiple of four).
    // encoding — как читались байты: UTF-16/32 по BOM либо кодировка
    // декодера файла (UTF-8, пока «не решено»). Передаётся в startParsingFrom
    // как streamEncoding.
    void tailState(const LogFilePtr& logFile, qint64 endOffset, bool lastLinePartial,
                   qint64 lastLineStart, TextEncoding encoding);
    void parsingFinished(int totalEntries, const LogFilePtr& logFile);
    void parsingFailed(const LogFilePtr& logFile);
    void parsingProgress(int progressPercentage, const LogFilePtr& logFile);

private:
    // The worker methods receive an immutable snapshot of the schema and the
    // extraction flag taken on the GUI thread at launch time. They never read
    // the mutable m_pattern / m_extractionEnabled members, so reconfiguring
    // the parser (setPattern) while a parse is in flight is race-free.
    // The one parse loop for both the initial load (initial: progress and
    // parsingStarted/Failed) and an incremental read.
    void parseFrom(const LogFilePtr& logFile, qint64 startOffset,
                   const ResumeContext& context, const LogPattern& pattern,
                   bool extraction, bool initial,
                   TextEncoding streamEncoding, FileEncoding encoding,
                   TextEncoding fallback);

    // Распознавание таймстампа/уровня и правило primary-строки; const-методы,
    // безопасно читается воркерами пула (см. LineClassifier).
    const LineClassifier m_classifier;
    LogPattern m_pattern; // Optional block schema for structured field extraction
    bool m_extractionEnabled = false; // Only extract fields when filter is active
    TextEncoding m_fallbackEncoding = TextEncodings::systemFallback();

    // Parse tasks run on this private pool, joined in the destructor so a
    // worker can never outlive the parser (and its members) it reads from.
    QThreadPool      m_pool;
    std::atomic_bool m_abort{false}; // Set on teardown to cut a running parse short.
};

#endif // LOGPARSER_H
