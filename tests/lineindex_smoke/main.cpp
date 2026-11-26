// ============================================================================
// lineindex_smoke — смоук-тест индексного ядра больших логов.
//
// Проверяет:
//   • корректность байтовых смещений/длин LineIndex (LF, CRLF, UTF-8
//     многобайтные символы, пустые строки, файл без завершающего \n, BOM);
//   • «золотое» сравнение: LogParser (резидентный путь) и LogIndexer на одном
//     файле дают идентичную по-строчную классификацию (текст, уровень,
//     logicalId, таймстамп, plain-text);
//   • дозапись: переиндексация предварительного хвоста (truncateFrom) даёт
//     тот же индекс, что свежая полная индексация итогового файла;
//   • согласованность endOffset с якорями FileChangeDetector;
//   • классификатор: ручные сканеры эквивалентны регекс-эталону, а политика
//     времени (зоны, дробная часть, метки без зоны) сверена с заранее
//     известными моментами.
// ============================================================================

#include "filechangedetector.h"
#include "lineclassifier.h"
#include "lineindex.h"
#include "logentry.h"
#include "logindexer.h"
#include "logparser.h"
#include "patternheuristics.h"
#include "sequentiallinereader.h"
#include "textchunkcache.h"
#include "textencoding.h"
#include "keyedfixtures.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>
#include <QTimeZone>
#include <QElapsedTimer>

#include <atomic>
#include <cstdio>
#include <limits>
#include <vector>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL(%s:%d): %s\n", __FILE__, __LINE__,    \
                         msg);                                               \
        }                                                                    \
    } while (false)

// ---------------------------------------------------------------------------
// Помощники
// ---------------------------------------------------------------------------

static QString writeFile(const QDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
        std::exit(2);
    }
    f.write(bytes);
    f.close();
    return path;
}

static void appendToFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        std::fprintf(stderr, "cannot append %s\n", qPrintable(path));
        std::exit(2);
    }
    f.write(bytes);
}

// Схема прохода (F-5) из строки схемы: пустая — без схемы. Одна схема на
// файл — колонки тождественны колонкам схемы.
static FileSchemaPtr passSchema(const QString& schema)
{
    if (schema.isEmpty())
        return nullptr;
    FieldLayout layout;
    return layout.schemaFor(schema);
}

// Полная индексация файла; блокирует до завершения.
static std::shared_ptr<LineIndex> indexFile(const LogFilePtr& lf,
                                            bool* fallback = nullptr,
                                            const QString& schema = QString())
{
    auto index = std::make_shared<LineIndex>();
    // Флаги объявлены ДО индексатора: при таймауте его деструктор дожидается
    // воркера, а тот ещё пишет в них из direct-слота.
    std::atomic_bool done{false};
    std::atomic_bool fb{false};
    LogIndexer indexer;
    QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                     [&](qint64, const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexingFailed, &indexer,
                     [&](const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::needsResidentFallback, &indexer,
                     [&](const LogFilePtr&, const QString&) { fb = true; done = true; },
                     Qt::DirectConnection);
    indexer.startIndexing(lf, index, FileEncoding(), passSchema(schema));
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 30000)
        QThread::msleep(2);
    CHECK(done.load(), "indexing did not finish in time");
    if (fallback)
        *fallback = fb.load();
    return index;
}

// Дозапись хвоста; блокирует до завершения.
static void indexAppend(LogIndexer& indexer, const LogFilePtr& lf,
                        const std::shared_ptr<LineIndex>& index,
                        bool reindexProvisionalTail, const QString& schema = QString())
{
    std::atomic_bool done{false};
    auto conn = QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                                 [&](qint64, const LogFilePtr&) { done = true; },
                                 Qt::DirectConnection);
    indexer.startIndexingFrom(lf, index, reindexProvisionalTail, FileEncoding(),
                              passSchema(schema));
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 30000)
        QThread::msleep(2);
    CHECK(done.load(), "incremental indexing did not finish in time");
    QObject::disconnect(conn);
}

// Резидентный парсинг файла; блокирует до завершения.
static QVector<std::shared_ptr<LogEntry>> parseFile(const LogFilePtr& lf,
                                                    const QString& schema = QString())
{
    QVector<std::shared_ptr<LogEntry>> all;
    std::atomic_bool done{false}; // до парсера — см. indexFile()
    LogParser parser;
    QObject::connect(&parser, &LogParser::entriesParsed, &parser,
                     [&](const QVector<std::shared_ptr<LogEntry>>& batch,
                         const LogFilePtr&) { all += batch; },
                     Qt::DirectConnection);
    QObject::connect(&parser, &LogParser::parsingFinished, &parser,
                     [&](int, const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&parser, &LogParser::parsingFailed, &parser,
                     [&](const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    parser.startParsing(lf, FileEncoding(), passSchema(schema));
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 30000)
        QThread::msleep(2);
    CHECK(done.load(), "parsing did not finish in time");
    return all;
}

// Текст строки по индексу: чтение сырых байт по смещению + декодирование.
static QString textFromIndex(const QString& path, const LineIndex& index, qint64 line)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("<open failed>");
    const qint64 off = index.lineStartOffset(line);
    const quint32 len = index.lineByteLength(line);
    if (off < 0 || !f.seek(off))
        return QStringLiteral("<seek failed>");
    return QString::fromUtf8(f.read(len));
}

// Построчное сравнение резидентного парсинга и индекса одного файла.
static void compareParserVsIndex(const QString& tag, const QString& path,
                                 const LogFilePtr& lf,
                                 const QVector<std::shared_ptr<LogEntry>>& entries,
                                 const LineIndex& index)
{
    const QByteArray tagBytes = tag.toUtf8();
    CHECK(index.lineCount() == entries.size(),
          qPrintable(QString("%1: line counts differ: index %2 vs parser %3")
                         .arg(tag).arg(index.lineCount()).arg(entries.size())));
    const qint64 n = qMin<qint64>(index.lineCount(), entries.size());
    for (qint64 i = 0; i < n; ++i) {
        const LogEntry& e = *entries[int(i)];
        const QString idxText = textFromIndex(path, index, i);
        if (idxText != e.message()) {
            ++g_failures;
            std::fprintf(stderr,
                         "FAIL %s line %lld text mismatch:\n  parser: '%s'\n  index:  '%s'\n",
                         tagBytes.constData(), (long long)i,
                         qPrintable(e.message()), qPrintable(idxText));
            return; // дальнейшие сравнения зашумят вывод
        }
        CHECK(index.level(i) == e.level(),
              qPrintable(QString("%1: line %2 level mismatch").arg(tag).arg(i)));
        CHECK(qint64(index.logicalId(i)) == qint64(e.logicalEntryId()),
              qPrintable(QString("%1: line %2 logicalId mismatch: %3 vs %4")
                             .arg(tag).arg(i).arg(index.logicalId(i))
                             .arg(e.logicalEntryId())));
        const qint64 expectedMs = e.timestamp().isValid()
            ? e.timestamp().toMSecsSinceEpoch() : kNoTimestampMs;
        CHECK(index.timestampMs(index.logicalId(i)) == expectedMs,
              qPrintable(QString("%1: line %2 timestamp mismatch").arg(tag).arg(i)));
        CHECK(index.isPlainTextLine(i) == e.isPlainText(),
              qPrintable(QString("%1: line %2 isPlainText mismatch").arg(tag).arg(i)));
    }
}

// Полное построчное сравнение двух индексов (инкрементального и свежего).
static void compareIndexes(const QString& tag, const LineIndex& a, const LineIndex& b)
{
    CHECK(a.lineCount() == b.lineCount(),
          qPrintable(QString("%1: line counts differ: %2 vs %3")
                         .arg(tag).arg(a.lineCount()).arg(b.lineCount())));
    const qint64 n = qMin(a.lineCount(), b.lineCount());
    for (qint64 i = 0; i < n; ++i) {
        CHECK(a.lineStartOffset(i) == b.lineStartOffset(i),
              qPrintable(QString("%1: line %2 offset differs").arg(tag).arg(i)));
        CHECK(a.lineByteLength(i) == b.lineByteLength(i),
              qPrintable(QString("%1: line %2 length differs").arg(tag).arg(i)));
        CHECK(a.lineEolBytes(i) == b.lineEolBytes(i),
              qPrintable(QString("%1: line %2 eol differs").arg(tag).arg(i)));
        CHECK(a.level(i) == b.level(i),
              qPrintable(QString("%1: line %2 level differs").arg(tag).arg(i)));
        CHECK(a.logicalId(i) == b.logicalId(i),
              qPrintable(QString("%1: line %2 logicalId differs").arg(tag).arg(i)));
        CHECK(a.timestampMs(a.logicalId(i)) == b.timestampMs(b.logicalId(i)),
              qPrintable(QString("%1: line %2 ts differs").arg(tag).arg(i)));
    }
    CHECK(a.endOffset() == b.endOffset(),
          qPrintable(QString("%1: endOffset differs").arg(tag)));
}

// ---------------------------------------------------------------------------

static QByteArray sampleLog(bool crlf, bool trailingNewline)
{
    const char* eol = crlf ? "\r\n" : "\n";
    QByteArray b;
    b += "preamble before any record"; b += eol;                       // plain
    b += ""; b += eol;                                                 // пустая
    b += "2026-07-10 09:00:00,001 INFO [core] first record"; b += eol;
    b += "2026-07-10 09:00:00,500 ERROR [db] Ошибка записи в сегмент 42"; b += eol; // UTF-8
    b += "com.example.Boom: множественный стектрейс"; b += eol;        // continuation
    b += "    at com.example.A(A.java:10)"; b += eol;                  // continuation
    b += ""; b += eol;                                                 // пустая continuation
    b += "2026-07-10 09:00:01,000 WARN [net] третий — кириллица в тексте"; b += eol;
    b += "07/10/2026 09:00:02 INFO manual-format record"; b += eol;    // MM/dd/yyyy
    b += "tail line without level or timestamp";
    if (trailingNewline)
        b += eol;
    return b;
}

static void testGolden(const QDir& dir)
{
    for (const bool crlf : {false, true}) {
        for (const bool trailing : {false, true}) {
            const QString name = QString("golden_%1_%2.log")
                                     .arg(crlf ? "crlf" : "lf")
                                     .arg(trailing ? "nl" : "nonl");
            const QString path = writeFile(dir, name, sampleLog(crlf, trailing));
            auto lf = std::make_shared<LogFile>(path);
            const auto entries = parseFile(lf);
            const auto index = indexFile(lf);
            compareParserVsIndex(name, path, lf, entries, *index);

            // Хвост без \n обязан быть «предварительным».
            CHECK(index->lastLineProvisional() == !trailing,
                  qPrintable(name + ": provisional-tail flag wrong"));

            // endOffset согласован с реальным размером файла.
            CHECK(index->endOffset() == QFile(path).size(),
                  qPrintable(name + ": endOffset != file size"));
        }
    }
}

// Метки syslog, access-лога и nginx error на обоих путях загрузки: моменты
// одинаковы и равны заранее известным, год меток syslog — по mtime файла,
// в том числе через Новый год и при дозаписи после него.
static void setMtime(const QString& path, const QDateTime& when)
{
    QFile f(path);
    CHECK(f.open(QIODevice::ReadWrite)
              && f.setFileTime(when, QFileDevice::FileModificationTime),
          "set file mtime");
}

static void testNewTimestampFormsBackends(const QDir& dir)
{
    const auto local = [](int y, int mo, int d, int h, int mi, int s) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s)).toMSecsSinceEpoch();
    };
    const QString path = writeFile(dir, QStringLiteral("forms.log"),
        "Dec 31 23:59:58 web01 sshd[1]: error: first\n"
        "\tcontinuation of the first\n"
        "Jan  1 00:00:02 web01 app[2]: WARN second, new year\n"
        "2026/12/31 23:59:59 [error] 1#1: nginx\n"
        "1.2.3.4 - - [31/Dec/2026:22:00:00 -0200] \"GET /x HTTP/1.1\" 500 0 ERROR\n"
        "at Dec 31 23:59:58 continuation with a level ERROR\n");
    setMtime(path, QDateTime(QDate(2027, 1, 1), QTime(0, 0, 3)));
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf);
    const auto index = indexFile(lf);
    compareParserVsIndex(QStringLiteral("forms.log"), path, lf, entries, *index);
    const qint64 expected[] = {
        local(2026, 12, 31, 23, 59, 58), local(2026, 12, 31, 23, 59, 58),
        local(2027, 1, 1, 0, 0, 2), local(2026, 12, 31, 23, 59, 59),
        QDateTime(QDate(2027, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch(),
        QDateTime(QDate(2027, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch(),
    };
    CHECK(index->lineCount() == 6, "forms.log: six lines");
    for (qint64 i = 0; i < qMin<qint64>(6, index->lineCount()); ++i)
        CHECK(index->timestampMs(index->logicalId(i)) == expected[i],
              qPrintable(QString("forms.log: line %1 moment").arg(i)));
    CHECK(index->logicalEntryCount() == 4, "forms.log: syslog in the middle of a line starts no record");

    // Дозапись после Нового года: строки, прочитанные до неё, сохраняют год
    // по прежнему mtime, дописанные получают новый — и так же, как свежий
    // разбор того же файла.
    const QString grow = writeFile(dir, QStringLiteral("forms_grow.log"),
                                   "Dec 31 23:59:58 web01 app: INFO before\n");
    setMtime(grow, QDateTime(QDate(2026, 12, 31), QTime(23, 59, 59)));
    auto growLf = std::make_shared<LogFile>(grow);
    const auto growIndex = indexFile(growLf);
    appendToFile(grow, "Jan  1 00:00:02 web01 app: INFO after\n");
    setMtime(grow, QDateTime(QDate(2027, 1, 1), QTime(0, 0, 3)));
    {
        LogIndexer indexer;
        indexAppend(indexer, growLf, growIndex, false);
    }
    const auto fresh = indexFile(std::make_shared<LogFile>(grow));
    compareIndexes(QStringLiteral("forms_grow.log"), *growIndex, *fresh);
    CHECK(growIndex->lineCount() == 2
              && growIndex->timestampMs(growIndex->logicalId(0)) == local(2026, 12, 31, 23, 59, 58)
              && growIndex->timestampMs(growIndex->logicalId(1)) == local(2027, 1, 1, 0, 0, 2),
          "forms_grow.log: appended line after New Year gets the new year");
}

// F-11. Время без даты на обоих путях загрузки и в сканере каталога:
// последняя строка — дата по mtime (ближайшая), к началу файла дата
// уменьшается на сутки, когда время идёт назад больше чем на час. Моменты —
// против заранее заданных; дозапись через полночь продолжает уже показанные
// строки и совпадает со свежим разбором.

// Дозапись резидентным парсером с конца прежнего прохода (контекст — от
// последней строки, как у вкладки); блокирует до завершения.
static QVector<std::shared_ptr<LogEntry>> parseAppend(const LogFilePtr& lf, qint64 from,
                                                      const std::shared_ptr<LogEntry>& last)
{
    QVector<std::shared_ptr<LogEntry>> all;
    std::atomic_bool done{false};
    LogParser parser;
    QObject::connect(&parser, &LogParser::entriesParsed, &parser,
                     [&](const QVector<std::shared_ptr<LogEntry>>& batch,
                         const LogFilePtr&) { all += batch; },
                     Qt::DirectConnection);
    QObject::connect(&parser, &LogParser::parsingFinished, &parser,
                     [&](int, const LogFilePtr&) { done = true; }, Qt::DirectConnection);
    LogParser::ResumeContext context;
    if (last) {
        context.nextLogicalEntryId = last->logicalEntryId() + 1;
        context.currentLogicalEntryId = last->logicalEntryId();
        context.currentTimestamp = last->timestamp();
        context.currentLevel = last->level();
        context.nextLineNumber = last->originalLineNumber() + 1;
    }
    parser.startParsingFrom(lf, from, context);
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 30000)
        QThread::msleep(2);
    CHECK(done.load(), "incremental parsing did not finish in time");
    return all;
}

static qint64 localMs(const QDate& date, int h, int mi, int sec, int ms = 0)
{
    return QDateTime(date, QTime(h, mi, sec, ms)).toMSecsSinceEpoch();
}

static qint64 entryMs(const LogEntry& e)
{
    return e.timestamp().isValid() ? e.timestamp().toMSecsSinceEpoch() : kNoTimestampMs;
}

static void checkMoments(const QString& tag, const LineIndex& index,
                         const QVector<std::shared_ptr<LogEntry>>& entries,
                         const QVector<qint64>& expected)
{
    CHECK(index.lineCount() == expected.size() && entries.size() == expected.size(),
          qPrintable(QString("%1: %2 lines (index %3, parser %4)").arg(tag).arg(expected.size())
                         .arg(index.lineCount()).arg(entries.size())));
    for (int i = 0; i < expected.size() && i < index.lineCount() && i < entries.size(); ++i) {
        const qint64 idx = index.timestampMs(index.logicalId(i));
        const qint64 res = entryMs(*entries[i]);
        if (idx != expected[i] || res != expected[i]) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s line %d moment: index %s, parser %s, expected %s\n",
                         qPrintable(tag), i + 1,
                         qPrintable(QDateTime::fromMSecsSinceEpoch(idx).toString(Qt::ISODateWithMs)),
                         qPrintable(QDateTime::fromMSecsSinceEpoch(res).toString(Qt::ISODateWithMs)),
                         qPrintable(QDateTime::fromMSecsSinceEpoch(expected[i]).toString(Qt::ISODateWithMs)));
        }
    }
}

static void testClockTimestamps(const QDir& dir)
{
    const QDate d0(2026, 11, 16), d1(2026, 11, 17), d2(2026, 11, 18);
    // Трое суток; mtime — чуть после последней строки.
    const QByteArray threeDays =
        "23:50:00.000 [main] INFO  app - started\n"          // d0
        "23:59:59.500 [main] DEBUG app - tick\n"            // d0
        "00:00:01,250 [main] INFO  app - after midnight\n"  // d1: назад больше чем на час
        "java.lang.IllegalStateException: at 00:00:01\n"    // продолжение
        "\tat com.example.App.run(App.java:1)\n"            // продолжение
        "12:00:00 [main] INFO  app - noon\n"                 // d1
        "11:00:00 [main] WARN  app - clock moved back\n"     // d1: ровно час назад — перевод часов
        "10:59:59 [pool-1] INFO  app - threads interleave\n" // d1: чуть назад
        "[23:00:00 INF] evening\n"                           // d1
        "took 00:01:23 to finish\n"                          // продолжение: длительность в тексте
        "2026-01-10 10:00:00 INFO  iso line keeps its date\n"
        "[00:30:00 ERR] next day\n"                          // d2
        "00:40:00.5 [main] ERROR app - last\n";              // d2
    const QString path = writeFile(dir, QStringLiteral("clock.log"), threeDays);
    setMtime(path, QDateTime(d2, QTime(0, 45)));
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf);
    const auto index = indexFile(lf);
    compareParserVsIndex(QStringLiteral("clock.log"), path, lf, entries, *index);
    const qint64 afterMidnight = localMs(d1, 0, 0, 1, 250);
    const QVector<qint64> expected = {
        localMs(d0, 23, 50, 0), localMs(d0, 23, 59, 59, 500),
        afterMidnight, afterMidnight, afterMidnight,
        localMs(d1, 12, 0, 0), localMs(d1, 11, 0, 0), localMs(d1, 10, 59, 59),
        localMs(d1, 23, 0, 0), localMs(d1, 23, 0, 0),
        localMs(QDate(2026, 1, 10), 10, 0, 0),
        localMs(d2, 0, 30, 0), localMs(d2, 0, 40, 0, 500),
    };
    checkMoments(QStringLiteral("clock.log"), *index, entries, expected);
    CHECK(index->logicalEntryCount() == 10, "clock.log: ten records (time without a date is leading)");
    CHECK(ClockDateState::fromPacked(lf->clockState.load()).isSet(),
          "clock.log: date state kept on the file for an append");

    // Без контекста файла форма — не метка (граница фильтра из текста, Go to).
    {
        const LineClassifier classifier;
        qint64 ms = 0;
        CHECK(!classifier.detectTimestampMs(QStringLiteral("13:55:40.016 [main] INFO x"), ms),
              "time without a date needs the file context");
    }

    // Последняя строка у полуночи: сброшена на диск после полуночи — день
    // её записи, а не mtime; писатель в поясе восточнее — следующий день.
    const QString flush = writeFile(dir, QStringLiteral("clock_flush.log"),
                                    "23:59:58 INFO a\n23:59:59 INFO b\n");
    setMtime(flush, QDateTime(d2, QTime(0, 0, 3)));
    auto flushLf = std::make_shared<LogFile>(flush);
    checkMoments(QStringLiteral("clock_flush.log"), *indexFile(flushLf), parseFile(flushLf),
                 { localMs(d1, 23, 59, 58), localMs(d1, 23, 59, 59) });
    const QString east = writeFile(dir, QStringLiteral("clock_east.log"),
                                   "22:00:00 INFO a\n02:00:00 INFO b\n");
    setMtime(east, QDateTime(d1, QTime(23, 0)));
    auto eastLf = std::make_shared<LogFile>(east);
    checkMoments(QStringLiteral("clock_east.log"), *indexFile(eastLf), parseFile(eastLf),
                 { localMs(d1, 22, 0, 0), localMs(d2, 2, 0, 0) });

    // UTF-16 (только резидентный путь): предпросмотр читает поток в кодировке.
    {
        QStringEncoder enc(QStringConverter::Utf16LE, QStringConverter::Flag::WriteBom);
        const QByteArray wide = enc.encode(QStringLiteral("23:00:00 INFO a\n01:00:00 INFO b\n"));
        const QString widePath = writeFile(dir, QStringLiteral("clock_utf16.log"), wide);
        setMtime(widePath, QDateTime(d2, QTime(1, 0, 1)));
        const auto wideEntries = parseFile(std::make_shared<LogFile>(widePath));
        CHECK(wideEntries.size() == 2 && entryMs(*wideEntries[0]) == localMs(d1, 23, 0, 0)
                  && entryMs(*wideEntries[1]) == localMs(d2, 1, 0, 0),
              "clock_utf16.log: dates through the UTF-16 look-ahead");
    }

    // Сканер каталога: From/To — те же моменты, без предпросмотра.
    {
        const LogParser parser;
        const LogParser::StatsScan scan = parser.analyzeFileForStats(path, LogParser::StatsResume());
        CHECK(scan.stats.totalEntries == 10, "clock.log scanner: ten records");
        CHECK(scan.stats.firstEntryTimestamp.toMSecsSinceEpoch() == expected.first()
                  && scan.stats.lastEntryTimestamp.toMSecsSinceEpoch() == expected.last(),
              qPrintable(QString("clock.log scanner: From/To %1 / %2")
                             .arg(scan.stats.firstEntryTimestamp.toString(Qt::ISODateWithMs),
                                  scan.stats.lastEntryTimestamp.toString(Qt::ISODateWithMs))));
    }

    // Дозапись через полночь на обоих путях и в сканере: прочитанные строки
    // сохраняют даты, дописанная после полуночи получает следующий день —
    // как при свежем разборе того же файла.
    const QString grow = writeFile(dir, QStringLiteral("clock_grow.log"),
                                   "23:59:57 INFO a\n23:59:58 INFO b\n");
    setMtime(grow, QDateTime(d1, QTime(23, 59, 59)));
    auto growLf = std::make_shared<LogFile>(grow);
    auto growIndexLf = std::make_shared<LogFile>(grow);
    auto growEntries = parseFile(growLf);
    const auto growIndex = indexFile(growIndexLf);
    const LogParser scanner;
    const LogParser::StatsScan head = scanner.analyzeFileForStats(grow, LogParser::StatsResume());
    const qint64 headSize = QFileInfo(grow).size();

    // Сначала оборванная строка (писатель не дописал '\n'), затем она целиком.
    appendToFile(grow, "00:00:0");
    setMtime(grow, QDateTime(d2, QTime(0, 0, 2)));
    {
        LogIndexer indexer;
        indexAppend(indexer, growIndexLf, growIndex, false);
    }
    const auto partial = parseAppend(growLf, headSize, growEntries.last());
    CHECK(partial.size() == 1
              && partial.first()->logicalEntryId() == growEntries.last()->logicalEntryId(),
          "clock_grow.log: an unfinished time is not a timestamp yet (continuation)");
    appendToFile(grow, "2 INFO c\n00:00:03 INFO d\n");
    setMtime(grow, QDateTime(d2, QTime(0, 0, 4)));
    {
        LogIndexer indexer;
        indexAppend(indexer, growIndexLf, growIndex, true);
    }
    // Резидентный путь перечитывает оборванную строку с её начала.
    growEntries += parseAppend(growLf, headSize, growEntries.last());
    const QVector<qint64> grown = {
        localMs(d1, 23, 59, 57), localMs(d1, 23, 59, 58),
        localMs(d2, 0, 0, 2), localMs(d2, 0, 0, 3),
    };
    checkMoments(QStringLiteral("clock_grow.log"), *growIndex, growEntries, grown);
    const auto freshGrow = indexFile(std::make_shared<LogFile>(grow));
    compareIndexes(QStringLiteral("clock_grow.log"), *growIndex, *freshGrow);

    const LogParser::StatsScan tail = scanner.analyzeFileForStats(grow, head.next);
    const LogParser::FileStats merged = LogParser::mergeStats(head.complete, tail.stats);
    CHECK(head.stats.lastEntryTimestamp.toMSecsSinceEpoch() == grown[1]
              && merged.firstEntryTimestamp.toMSecsSinceEpoch() == grown.first()
              && merged.lastEntryTimestamp.toMSecsSinceEpoch() == grown.last()
              && merged.totalEntries == 4,
          qPrintable(QString("clock_grow.log scanner tail: %1 … %2, %3 records")
                         .arg(merged.firstEntryTimestamp.toString(Qt::ISODate),
                              merged.lastEntryTimestamp.toString(Qt::ISODate))
                         .arg(merged.totalEntries)));

    // Огрызок оборванной строки похож на время, целая строка — нет
    // («13:55:36» от «13:55:36:123»): состояние дат не должно его помнить —
    // дописанная после полуночи строка получает следующий день, а не через
    // день. Первый проход видел огрызок последней строкой и привязал к нему
    // дату первой (d0); показанное не пересчитывается.
    const QString cut = writeFile(dir, QStringLiteral("clock_cut.log"), "23:30:00 INFO a\n13:55:36");
    setMtime(cut, QDateTime(d1, QTime(23, 30, 1)));
    auto cutLf = std::make_shared<LogFile>(cut);
    auto cutIndexLf = std::make_shared<LogFile>(cut);
    auto cutEntries = parseFile(cutLf);
    const auto cutIndex = indexFile(cutIndexLf);
    const qint64 cutFrom = QByteArray("23:30:00 INFO a\n").size();
    cutEntries.removeLast();   // оборванную строку вкладка перечитывает с начала
    appendToFile(cut, ":123 INFO tail of the line\n00:10:00 INFO b\n");
    setMtime(cut, QDateTime(d2, QTime(0, 10, 1)));
    {
        LogIndexer indexer;
        indexAppend(indexer, cutIndexLf, cutIndex, true);
    }
    cutEntries += parseAppend(cutLf, cutFrom, cutEntries.last());
    checkMoments(QStringLiteral("clock_cut.log"), *cutIndex, cutEntries,
                 { localMs(d0, 23, 30, 0), localMs(d0, 23, 30, 0), localMs(d1, 0, 10, 0) });

    // Дозапись в файл, где времени без даты ещё не было: дата дописанных строк
    // — по mtime в момент дозаписи (предпросмотр от начала дозаписи).
    const QString late = writeFile(dir, QStringLiteral("clock_late.log"),
                                   "2026-11-17 10:00:00 INFO iso first\n");
    setMtime(late, QDateTime(d1, QTime(10, 0, 1)));
    auto lateLf = std::make_shared<LogFile>(late);
    auto lateIndexLf = std::make_shared<LogFile>(late);
    auto lateEntries = parseFile(lateLf);
    const auto lateIndex = indexFile(lateIndexLf);
    const qint64 lateSize = QFileInfo(late).size();
    appendToFile(late, "23:30:00 INFO a\n00:15:00 INFO b\n");
    setMtime(late, QDateTime(d2, QTime(0, 15, 1)));
    {
        LogIndexer indexer;
        indexAppend(indexer, lateIndexLf, lateIndex, false);
    }
    lateEntries += parseAppend(lateLf, lateSize, lateEntries.last());
    checkMoments(QStringLiteral("clock_late.log"), *lateIndex, lateEntries,
                 { localMs(d1, 10, 0, 0), localMs(d1, 23, 30, 0), localMs(d2, 0, 15, 0) });
}

// F-12. Границы записей без слова уровня на обоих путях загрузки: запись
// начинает строка с ведущей меткой (позиция 0, «[» на позиции 0, access за
// «[»), а метка в середине строки или под отступом — продолжение. Оба пути
// дают одни и те же записи и моменты, и они равны заданным; дозапись,
// начатая продолжением с меткой в теле, остаётся в своей записи.
struct BoundaryLine {
    const char* text;
    bool starts;   // начинает запись
};

static const BoundaryLine kBoundaryCorpus[] = {
    { "=== service banner ===", true },                                     // преамбула: своя запись
    { "built 2026-11-18 10:00:00 from main", false },                       // метка в середине
    { "192.168.1.10 - - [18/Nov/2026:13:55:36 +0000] \"GET / HTTP/1.1\" 200 512", true },
    { "2001:db8::1 - alice@example.com [18/Nov/2026:13:55:37 +0000] \"POST /api HTTP/2\" 201 0", true },
    { "Nov 18 13:55:38 web01 sshd[1234]: Accepted publickey for deploy", true },
    { "2026-11-18 13:55:39 Starting backup job", true },                    // «метка сообщение»
    { "[2026-11-18 13:55:40] cache warmed", true },
    { "2026-11-18 13:55:41 query:", true },
    { "SELECT * FROM t WHERE created_at > '2026-11-18 10:00:00'", false },
    { "    AND updated_at < '2026-11-18 11:00:00'", false },
    { "2026-11-18 13:55:42 payload:", true },
    { "{", false },
    { "  \"ts\": \"2026-11-18T10:00:00Z\",", false },
    { "\"created\": \"2026-11-18 10:00:00\",", false },
    { "Response: {\"ts\":\"2026-11-18T10:00:00Z\"}", false },
    { "<timestamp>2026-11-18T10:00:00</timestamp>", false },
    { "\t2026-11-18 10:00:00 tab-indented stamp", false },
    { "}", false },
    { "2026-11-18 13:55:43.731 [pool-3] ERROR c.e.Billing - Failed", true },
    { "java.lang.IllegalStateException: SMTP did not respond at 2026-11-18 13:55:39", false },
    { "\tat com.example.MailGateway.send(MailGateway.java:88)", false },
    { "Traceback (most recent call last):", false },
    { "  File \"/srv/app.py\", line 12, in <module>", false },
    { "ValueError: bad date 2026-11-18 13:55:40", false },
    { "   at Foo.Bar() in C:\\src\\Foo.cs:line 10", false },
    { "prefix 2026-11-18 13:55:44 WARN level with a stamp in the middle", true }, // как раньше
};

static QByteArray boundaryCorpusBytes()
{
    QByteArray b;
    for (const BoundaryLine& l : kBoundaryCorpus)
        b += QByteArray(l.text) + '\n';
    return b;
}

static void checkBoundaries(const QString& tag, const LineIndex& index)
{
    const qint64 n = qint64(std::size(kBoundaryCorpus));
    CHECK(index.lineCount() == n, qPrintable(tag + ": line count"));
    for (qint64 i = 0; i < qMin(n, index.lineCount()); ++i) {
        const bool starts = i == 0 || index.logicalId(i) != index.logicalId(i - 1);
        if (starts != kBoundaryCorpus[i].starts) {
            ++g_failures;
            std::fprintf(stderr, "FAIL %s: line %lld %s a record: %s\n", qPrintable(tag),
                         (long long)i, kBoundaryCorpus[i].starts ? "must start" : "must not start",
                         kBoundaryCorpus[i].text);
        }
    }
}

static void testRecordBoundariesBackends(const QDir& dir)
{
    const QString path = writeFile(dir, QStringLiteral("boundaries.log"), boundaryCorpusBytes());
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf);
    const auto index = indexFile(lf);
    compareParserVsIndex(QStringLiteral("boundaries.log"), path, lf, entries, *index);
    checkBoundaries(QStringLiteral("boundaries.log"), *index);

    // Моменты записей без уровня — свои, а продолжения — моменты записи.
    const auto utc = [](int h, int mi, int s) {
        return QDateTime(QDate(2026, 11, 18), QTime(h, mi, s), QTimeZone::UTC).toMSecsSinceEpoch();
    };
    const auto local = [](int h, int mi, int s) {
        return QDateTime(QDate(2026, 11, 18), QTime(h, mi, s)).toMSecsSinceEpoch();
    };
    if (index->lineCount() == qint64(std::size(kBoundaryCorpus))) {
        CHECK(index->timestampMs(index->logicalId(1)) == kNoTimestampMs, "boundaries.log: banner has no moment");
        CHECK(index->timestampMs(index->logicalId(2)) == utc(13, 55, 36), "boundaries.log: access moment");
        CHECK(index->timestampMs(index->logicalId(3)) == utc(13, 55, 37), "boundaries.log: IPv6 access moment");
        CHECK(index->timestampMs(index->logicalId(5)) == local(13, 55, 39), "boundaries.log: stamp-message moment");
        CHECK(index->timestampMs(index->logicalId(6)) == local(13, 55, 40), "boundaries.log: bracketed moment");
        CHECK(index->timestampMs(index->logicalId(12)) == local(13, 55, 42),
              "boundaries.log: JSON body inherits the record's moment, not its own");
        CHECK(index->logicalEntryCount() == 10, "boundaries.log: ten records");
    }

    // Дозапись: голова кончается записью, порция начинается продолжением с
    // меткой в теле. Итог — как у свежей индексации всего файла.
    const QByteArray all = boundaryCorpusBytes();
    const int split = all.indexOf("{\n");
    const QString grow = writeFile(dir, QStringLiteral("boundaries_grow.log"), all.left(split));
    auto growLf = std::make_shared<LogFile>(grow);
    const auto growIndex = indexFile(growLf);
    appendToFile(grow, all.mid(split));
    {
        LogIndexer indexer;
        indexAppend(indexer, growLf, growIndex, false);
    }
    checkBoundaries(QStringLiteral("boundaries_grow.log"), *growIndex);
    compareIndexes(QStringLiteral("boundaries_grow.log"), *growIndex, *index);
}

// Слова словаря уровней на обоих путях загрузки: запись с таким словом после
// метки времени — первичная, её уровень одинаков у LogParser и LogIndexer и
// равен заданному; строка-продолжение наследует его.
static void testLevelVocabularyBackends(const QDir& dir)
{
    const struct { const char* word; LogLevel level; } cases[] = {
        { "CRITICAL", LogLevel::Fatal }, { "crit", LogLevel::Fatal },
        { "SEVERE", LogLevel::Fatal },   { "emerg", LogLevel::Fatal },
        { "FTL", LogLevel::Fatal },      { "ERR", LogLevel::Error },
        { "WRN", LogLevel::Warn },       { "INF", LogLevel::Info },
        { "Notice", LogLevel::Info },    { "DBG", LogLevel::Debug },
        { "VERBOSE", LogLevel::Trace },  { "VRB", LogLevel::Trace },
    };
    QByteArray b;
    int second = 0;
    for (const auto& c : cases) {
        b += QByteArray("2026-07-10 09:00:") + QByteArray::number(10 + second++)
           + ",000 [" + c.word + "] record\n";
        b += "    continuation of the record\n";
    }
    const QString path = writeFile(dir, QStringLiteral("level_words.log"), b);
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf);
    const auto index = indexFile(lf);
    compareParserVsIndex(QStringLiteral("level_words"), path, lf, entries, *index);

    const int n = int(std::size(cases));
    CHECK(entries.size() == 2 * n && index->lineCount() == 2 * n, "level_words: line count");
    for (int i = 0; i < n && 2 * i + 1 < entries.size(); ++i) {
        const bool ok = entries[2 * i]->level() == cases[i].level
                     && entries[2 * i + 1]->level() == cases[i].level
                     && entries[2 * i + 1]->logicalEntryId() == entries[2 * i]->logicalEntryId()
                     && (i == 0 || entries[2 * i]->logicalEntryId()
                                       != entries[2 * i - 1]->logicalEntryId());
        if (!ok) {
            ++g_failures;
            std::fprintf(stderr, "FAIL level_words: record with '%s'\n", cases[i].word);
        }
    }
}

static void testBom(const QDir& dir)
{
    // UTF-8 BOM: пропускается, первая строка начинается после него.
    QByteArray withBom("\xEF\xBB\xBF");
    withBom += sampleLog(false, true);
    const QString path = writeFile(dir, "bom_utf8.log", withBom);
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf); // QTextStream сам понимает BOM
    const auto index = indexFile(lf);
    CHECK(index->lineStartOffset(0) == 3, "utf8 BOM not skipped");
    compareParserVsIndex("bom_utf8", path, lf, entries, *index);

    // UTF-16 BOM: индексный путь отказывается в пользу резидентного.
    QByteArray utf16;
    utf16 += char(0xFF); utf16 += char(0xFE);
    utf16 += QByteArray::fromRawData("l\0o\0g\0\n\0", 8);
    const QString p16 = writeFile(dir, "bom_utf16.log", utf16);
    bool fellBack = false;
    indexFile(std::make_shared<LogFile>(p16), &fellBack);
    CHECK(fellBack, "utf16 BOM must trigger resident fallback");
}

static void testBlockBoundaries(const QDir& dir)
{
    // >3 блоков по 1024 строки; проверяем смещения на границах напрямую.
    QByteArray b;
    QVector<qint64> offsets;
    QVector<int> lengths;
    qint64 off = 0;
    for (int i = 0; i < 3500; ++i) {
        const QByteArray line =
            QString("2026-07-10 10:%1:%2,%3 INFO [gen] line %4 наполнение")
                .arg((i / 60) % 60, 2, 10, QChar('0'))
                .arg(i % 60, 2, 10, QChar('0'))
                .arg(i % 1000, 3, 10, QChar('0'))
                .arg(i)
                .toUtf8();
        offsets.append(off);
        lengths.append(line.size());
        b += line;
        b += '\n';
        off += line.size() + 1;
    }
    const QString path = writeFile(dir, "blocks.log", b);
    auto lf = std::make_shared<LogFile>(path);
    const auto index = indexFile(lf);
    CHECK(index->lineCount() == 3500, "blocks: wrong line count");
    for (const qint64 probe : {qint64(0), qint64(1023), qint64(1024), qint64(2047),
                               qint64(2048), qint64(3499)}) {
        CHECK(index->lineStartOffset(probe) == offsets[int(probe)],
              qPrintable(QString("blocks: offset differs at %1").arg(probe)));
        CHECK(int(index->lineByteLength(probe)) == lengths[int(probe)],
              qPrintable(QString("blocks: length differs at %1").arg(probe)));
    }
}

static void testAppendReindex(const QDir& dir)
{
    // Файл обрывается посреди строки (нет \n); дозапись завершает её и
    // добавляет продолжение записи и новую запись.
    QByteArray part1;
    part1 += "2026-07-10 12:00:00,000 INFO [app] запись до дозаписи\n";
    part1 += "2026-07-10 12:00:01,000 ERROR [db] half-writ";
    const QString path = writeFile(dir, "append.log", part1);
    auto lf = std::make_shared<LogFile>(path);

    LogIndexer indexer;
    auto index = std::make_shared<LineIndex>();
    {
        std::atomic_bool done{false};
        auto conn = QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                                     [&](qint64, const LogFilePtr&) { done = true; },
                                     Qt::DirectConnection);
        indexer.startIndexing(lf, index);
        QElapsedTimer t; t.start();
        while (!done && t.elapsed() < 30000)
            QThread::msleep(2);
        CHECK(done.load(), "append: initial indexing timeout");
        // Индексатор живёт дольше флага: без disconnect следующая дозапись
        // записала бы в `done` за пределами его области (находка ASan).
        QObject::disconnect(conn);
    }
    CHECK(index->lineCount() == 2, "append: initial line count");
    CHECK(index->lastLineProvisional(), "append: tail must be provisional");

    // Якорь FileChangeDetector по опубликованному концу.
    const auto anchor = FileChangeDetector::capture(path, index->endOffset());

    QByteArray part2;
    part2 += "ten timeout при обработке\n";               // завершение строки
    part2 += "    at db.Pool.acquire(Pool.java:99)\n";     // continuation ERROR-записи
    part2 += "2026-07-10 12:00:02,000 INFO [app] запись после дозаписи\n";
    appendToFile(path, part2);

    CHECK(FileChangeDetector::classify(path, anchor)
              == FileChangeDetector::Change::Appended,
          "append: detector must classify as Appended");

    indexAppend(indexer, lf, index, index->lastLineProvisional());

    CHECK(index->lineCount() == 4, "append: final line count");
    CHECK(!index->lastLineProvisional(), "append: tail must be complete now");
    // Завершённая строка: текст сшит из двух кусков.
    CHECK(textFromIndex(path, *index, 1)
              == QStringLiteral("2026-07-10 12:00:01,000 ERROR [db] half-written timeout при обработке"),
          "append: reindexed tail text wrong");
    // Continuation унаследовала запись ERROR-строки.
    CHECK(index->logicalId(2) == index->logicalId(1),
          "append: continuation must inherit logical record");
    CHECK(index->level(2) == LogLevel::Error,
          "append: continuation must inherit level");

    // Инкрементальный индекс идентичен свежему полному индексу итогового файла.
    const auto fresh = indexFile(lf);
    compareIndexes("append-vs-fresh", *index, *fresh);
}

// ---------------------------------------------------------------------------
// S-13. Минимум и максимум меток, которые ведёт индекс: против перебора
// timestampMs по всем опубликованным записям — на случайной дозаписи с
// метками не по порядку (до 1970, «метки нет», записи из нескольких строк),
// через границы блоков меток (4096 записей) и после truncateFrom посреди
// блока и на его границе. Снапшот, взятый до усечения, свою пару не меняет.
// ---------------------------------------------------------------------------
static void testTimeRangeTracking()
{
    LineIndex index;
    QRandomGenerator rng(1313u);
    qint64 offset = 0;
    quint32 nextId = 0;
    index.beginAppend(0);

    const auto bruteCheck = [&](const char* what) {
        qint64 lo = (std::numeric_limits<qint64>::max)();
        qint64 hi = kNoTimestampMs;
        for (quint32 id = 0; id < index.logicalEntryCount(); ++id) {
            const qint64 ts = index.timestampMs(id);
            if (!hasTimestampMs(ts))
                continue;
            lo = qMin(lo, ts);
            hi = qMax(hi, ts);
        }
        qint64 gotLo = 0, gotHi = 0;
        const bool has = index.timeRangeMs(&gotLo, &gotHi);
        CHECK(has == hasTimestampMs(hi), what);
        if (has && hasTimestampMs(hi))
            CHECK(gotLo == lo && gotHi == hi, what);
        qint64 sLo = 0, sHi = 0;
        const LineIndexSnapshot snap = index.snapshot();
        CHECK(snap.timeRangeMs(&sLo, &sHi) == has && (!has || (sLo == gotLo && sHi == gotHi)),
              "time range: snapshot copies the published pair");
    };

    CHECK(!index.timeRangeMs(nullptr, nullptr), "time range: empty index has none");

    const auto appendRecords = [&](int records) {
        for (int r = 0; r < records; ++r) {
            const quint32 id = nextId++;
            const int roll = int(rng.bounded(100));
            qint64 ts = kNoTimestampMs;
            if (roll >= 15) // разброс в обе стороны от эпохи: −10^12…+10^12 мс
                ts = qint64(rng.bounded(2000000)) * 1000000 - 1000000000000LL;
            index.setLogicalTimestamp(id, ts);
            const int lines = 1 + (rng.bounded(100) < 20 ? int(rng.bounded(3)) : 0);
            for (int l = 0; l < lines; ++l) {
                LineIndex::LineRecord rec;
                rec.byteLength = 10;
                rec.eolBytes = 1;
                rec.isPrimary = l == 0;
                rec.hasOwnTimestamp = l == 0 && hasTimestampMs(ts);
                rec.logicalId = id;
                index.appendLine(rec);
                offset += 11;
            }
        }
        index.publish();
    };

    appendRecords(3000);
    bruteCheck("time range: first publish");
    appendRecords(6000); // через границы блоков меток
    bruteCheck("time range: across timestamp blocks");

    // Усечение посреди второго блока меток, затем на границе блока и до нуля.
    const qint64 cutLines[] = { index.lineCount() - 3, 0, 0 };
    for (int step = 0; step < 3; ++step) {
        qint64 cut = cutLines[step];
        if (step == 1) {
            // Первая строка записи 4096 — граница блока меток.
            for (qint64 line = 0; line < index.lineCount(); ++line) {
                if (index.logicalId(line) == quint32(LineIndex::kTsBlockEntries)) {
                    cut = line;
                    break;
                }
            }
        }
        if (step == 2) {
            cut = index.lineCount() / 3;
        }
        qint64 beforeLo = 0, beforeHi = 0;
        const LineIndexSnapshot before = index.snapshot();
        const bool beforeHas = before.timeRangeMs(&beforeLo, &beforeHi);

        index.truncateFrom(cut);
        nextId = index.logicalEntryCount();
        index.beginAppend(index.endOffset());
        bruteCheck("time range: after truncateFrom");

        qint64 lo = 0, hi = 0;
        CHECK(before.timeRangeMs(&lo, &hi) == beforeHas && lo == beforeLo && hi == beforeHi,
              "time range: snapshot before truncateFrom keeps its pair");

        appendRecords(1500 + int(rng.bounded(3000)));
        bruteCheck("time range: append after truncateFrom");
    }

    // Только записи без метки — диапазона нет.
    LineIndex plain;
    plain.beginAppend(0);
    for (quint32 id = 0; id < 10; ++id) {
        plain.setLogicalTimestamp(id, kNoTimestampMs);
        LineIndex::LineRecord rec;
        rec.logicalId = id;
        plain.appendLine(rec);
    }
    plain.publish();
    CHECK(!plain.timeRangeMs(nullptr, nullptr), "time range: no stamps — no range");
}

static void testSnapshotStability(const QDir& dir)
{
    // Снапшот, взятый до truncateFrom, продолжает видеть старую версию хвоста.
    QByteArray b;
    b += "2026-07-10 13:00:00,000 INFO [a] first\n";
    b += "provisional tail";
    const QString path = writeFile(dir, "snapshot.log", b);
    auto lf = std::make_shared<LogFile>(path);
    auto index = indexFile(lf);
    CHECK(index->lineCount() == 2, "snapshot: initial count");

    const LineIndexSnapshot snap = index->snapshot();
    const quint32 oldLen = snap.lineByteLength(1);

    appendToFile(path, QByteArray(" completed now\nnext line\n"));
    LogIndexer indexer;
    indexAppend(indexer, lf, index, true);

    CHECK(snap.lineCount() == 2, "snapshot: count must not change");
    CHECK(snap.lineByteLength(1) == oldLen,
          "snapshot: old tail length must be stable after reindex");
    CHECK(index->lineCount() == 3, "snapshot: new index count");
    CHECK(index->lineByteLength(1) > oldLen,
          "snapshot: live index must see the completed line");
}

// ---------------------------------------------------------------------------
// Эквивалентность ручных сканеров LineClassifier регекс-реализации.
// Эталон — код на QRegularExpression поверх канонических паттернов
// PatternHeuristics, момент считается через QDateTime/QTimeZone.
// Эквивалентность ловит ошибки сканера, но не общую ошибку семантики —
// для неё testTimestampGolden() сверяет с заранее известными моментами.
// ---------------------------------------------------------------------------

// Дробная часть и зона по раскладке timestampSuffixDetectPattern(); first —
// номер группы дробной части в совпадении.
struct RefSuffix {
    int millis = 0;
    bool hasZone = false;
    int offsetSeconds = 0;
};

static RefSuffix refParseSuffix(const QRegularExpressionMatch& m, int first)
{
    RefSuffix s;
    const QString fraction = m.captured(first);
    if (!fraction.isEmpty())
        s.millis = fraction.mid(1).left(3).leftJustified(3, QLatin1Char('0')).toInt();

    int hhGroup = -1;
    if (m.hasCaptured(first + 1) || m.hasCaptured(first + 6)) {
        s.hasZone = true; // Z / UTC / GMT
        return s;
    }
    if (m.hasCaptured(first + 2))
        hhGroup = first + 2;
    else if (m.hasCaptured(first + 4))
        hhGroup = first + 4;
    else
        return s;
    const QString hh = m.captured(hhGroup);
    const int hours = hh.mid(1).toInt();
    const int minutes = m.hasCaptured(hhGroup + 1) ? m.captured(hhGroup + 1).toInt() : 0;
    if (minutes > 59 || hours * 60 + minutes > 14 * 60)
        return s; // невозможное смещение (дальше ±14:00) — как без зоны
    s.hasZone = true;
    s.offsetSeconds = (hh.startsWith(QLatin1Char('-')) ? -1 : 1) * (hours * 3600 + minutes * 60);
    return s;
}

static QDateTime refMoment(int year, int month, int day, int hour, int minute,
                           int second, const RefSuffix& s)
{
    const QDate date(year, month, day);
    const QTime time(hour, minute, second, s.millis);
    if (!s.hasZone)
        return QDateTime(date, time); // локальное время машины
    return QDateTime(date, time, QTimeZone::fromSecondsAheadOfUtc(s.offsetSeconds));
}

// Эталонные регексы меток (PatternHeuristics) и опорная дата года.
struct RefTimestampRes {
    QRegularExpression iso, access, syslog, suffix;
    LineClassifier::YearReference year;
    // Время без даты (F-11) — PatternHeuristics::clockTimestampDetectPattern.
    QRegularExpression clock{PatternHeuristics::clockTimestampDetectPattern()};
};

// Дата одиночной строки с временем без даты независимо от сканера (F-11):
// последняя дата, при которой местный момент раньше опорного + 12 ч.
static QDateTime refClockMoment(const LineClassifier::YearReference& ref, const QTime& time)
{
    const QDateTime anchor = QDateTime::fromMSecsSinceEpoch(ref.momentMs);
    const qint64 limit = ref.momentMs + 12 * 3600 * 1000;
    for (QDate date = anchor.date().addDays(1);; date = date.addDays(-1)) {
        const QDateTime moment(date, time);
        if (moment.toMSecsSinceEpoch() < limit)
            return moment;
    }
}

static int refMonth(const QString& name)
{
    static const QStringList kMonths = {
        QStringLiteral("JAN"), QStringLiteral("FEB"), QStringLiteral("MAR"),
        QStringLiteral("APR"), QStringLiteral("MAY"), QStringLiteral("JUN"),
        QStringLiteral("JUL"), QStringLiteral("AUG"), QStringLiteral("SEP"),
        QStringLiteral("OCT"), QStringLiteral("NOV"), QStringLiteral("DEC") };
    return int(kMonths.indexOf(name.toUpper())) + 1;
}

// Год метки без года независимо от сканера: последний год, в котором
// (месяц, день) — валидная дата не позже опорной (уже «+1 день»).
static int refResolveYear(int month, int day, const LineClassifier::YearReference& ref)
{
    const QDate cutoff(ref.year, ref.month, ref.day);
    for (int y = ref.year; y > ref.year - 10; --y) {
        const QDate date(y, month, day);
        if (date.isValid() && date <= cutoff)
            return y;
    }
    return 0;
}

// detectTimestamp целиком на регексах + ручные фолбэк-форматы, без
// делегирования новому коду — иначе ложные срабатывания сканера
// маскировались бы. Порядок: форма с полной датой (ISO, dddd/dd/dd, access)
// на позиции 0; формы позиции 0 (фолбэк, syslog), только валидные; самая
// левая форма с полной датой; невалидная — не метка.
// leading — эталон «ведущей» метки (F-12, LineClassifier::isPrimaryLine):
// формы позиции 0; форма с полной датой с позиции 0, с позиции 1 за «[» или
// access за «[» в строке, которая не начинается с пробела или табуляции.
static bool refDetectTimestamp(const RefTimestampRes& re, const QString& line, QDateTime& ts,
                               bool* leading = nullptr, bool withClock = false)
{
    bool leadingDummy = false;
    bool& lead = leading ? *leading : leadingDummy;
    lead = false;
    const auto isoMatch = re.iso.match(line);
    const auto accessMatch = re.access.match(line);
    // Ведущая форма с полной датой: самая левая из двух.
    const QRegularExpressionMatch* mid = nullptr;
    if (isoMatch.hasMatch())
        mid = &isoMatch;
    if (accessMatch.hasMatch() && (!mid || accessMatch.capturedStart(0) < mid->capturedStart(0)))
        mid = &accessMatch;

    const auto fromMid = [&](const QRegularExpressionMatch& m) {
        int year, month, day, hour, minute, second, first;
        bool ok = true;
        if (&m == &isoMatch) {
            const auto p = m.capturedView(1);
            year = p.mid(0, 4).toInt(&ok);
            month = p.mid(5, 2).toInt(&ok);
            day = p.mid(8, 2).toInt(&ok);
            hour = p.mid(11, 2).toInt(&ok);
            minute = p.mid(14, 2).toInt(&ok);
            second = p.mid(17, 2).toInt(&ok);
            first = 2;
        } else {
            day = m.captured(1).toInt();
            month = refMonth(m.captured(2));
            year = m.captured(3).toInt();
            hour = m.captured(4).toInt();
            minute = m.captured(5).toInt();
            second = m.captured(6).toInt();
            first = 7;
        }
        const RefSuffix suffix = refParseSuffix(m, first);
        if (QDate::isValid(year, month, day)
            && QTime::isValid(hour, minute, second, suffix.millis)) {
            ts = refMoment(year, month, day, hour, minute, second, suffix);
            return true;
        }
        ts = QDateTime();
        return false;
    };

    if (mid && mid->capturedStart(0) == 0) {
        lead = fromMid(*mid);
        return lead;
    }

    const QStringView lineRef{line};
    const QStringList timeFormats = {
        QStringLiteral("dd/MM/yyyy HH:mm:ss"),
        QStringLiteral("MM/dd/yyyy HH:mm:ss"),
        QStringLiteral("dd.MM.yyyy HH:mm:ss"),
    };
    for (const QString& formatString : timeFormats) {
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
        bool convOk = true;

        if (formatString == QLatin1String("dd/MM/yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue;
            if (lineRef.at(2) != QLatin1Char('/') || lineRef.at(5) != QLatin1Char('/') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            day    = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            month  = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
        } else if (formatString == QLatin1String("MM/dd/yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue;
            if (lineRef.at(2) != QLatin1Char('/') || lineRef.at(5) != QLatin1Char('/') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            month  = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            day    = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
        } else if (formatString == QLatin1String("dd.MM.yyyy HH:mm:ss")) {
            if (lineRef.length() < 19) continue;
            if (lineRef.at(2) != QLatin1Char('.') || lineRef.at(5) != QLatin1Char('.') ||
                lineRef.at(10) != QLatin1Char(' ') || lineRef.at(13) != QLatin1Char(':') || lineRef.at(16) != QLatin1Char(':')) {
                continue;
            }
            day    = lineRef.mid(0, 2).toInt(&convOk); if (!convOk) continue;
            month  = lineRef.mid(3, 2).toInt(&convOk); if (!convOk) continue;
            year   = lineRef.mid(6, 4).toInt(&convOk); if (!convOk) continue;
            hour   = lineRef.mid(11, 2).toInt(&convOk); if (!convOk) continue;
            minute = lineRef.mid(14, 2).toInt(&convOk); if (!convOk) continue;
            second = lineRef.mid(17, 2).toInt(&convOk); if (!convOk) continue;
        } else {
            continue;
        }

        if (convOk && QDate::isValid(year, month, day)
            && QTime::isValid(hour, minute, second, millis)) {
            const auto suffix = re.suffix.match(line, 19, QRegularExpression::NormalMatch,
                                               QRegularExpression::AnchorAtOffsetMatchOption);
            ts = refMoment(year, month, day, hour, minute, second, refParseSuffix(suffix, 1));
            lead = true;
            return true;
        }
    }


    const auto syslog = re.syslog.match(line);
    if (syslog.hasMatch()) {
        const int month = refMonth(syslog.captured(1));
        const int day = syslog.captured(2).trimmed().toInt();
        const int year = refResolveYear(month, day, re.year);
        const RefSuffix suffix = refParseSuffix(syslog, 6);
        const int hour = syslog.captured(3).toInt();
        const int minute = syslog.captured(4).toInt();
        const int second = syslog.captured(5).toInt();
        if (year != 0 && QDate::isValid(year, month, day)
            && QTime::isValid(hour, minute, second, suffix.millis)) {
            ts = refMoment(year, month, day, hour, minute, second, suffix);
            lead = true;
            return true;
        }
    }

    // F-11: время без даты, только с контекстом файла; невалидное — не метка,
    // поиск идёт дальше.
    if (withClock) {
        const auto clock = re.clock.match(line);
        if (clock.hasMatch()) {
            const int hour = clock.captured(1).toInt();
            const int minute = clock.captured(2).toInt();
            const int second = clock.captured(3).toInt();
            const int millis = clock.captured(4).isEmpty()
                ? 0 : clock.captured(4).mid(1).left(3).leftJustified(3, QLatin1Char('0')).toInt();
            if (hour < 24 && minute < 60 && second < 60) {
                ts = refClockMoment(re.year, QTime(hour, minute, second, millis));
                lead = true;
                return true;
            }
        }
    }

    if (mid) {
        if (!fromMid(*mid))
            return false;
        const qsizetype at = mid->capturedStart(0);
        const bool afterBracket = line.at(at - 1) == QLatin1Char('[');
        lead = afterBracket
            && (at == 1
                || (mid == &accessMatch && line.at(0) != QLatin1Char(' ')
                    && line.at(0) != QLatin1Char('\t')));
        return true;
    }
    ts = QDateTime();
    return false;
}

static bool refDetectLogLevel(const QRegularExpression& levelRe,
                              const QString& line, LogLevel& level)
{
    const auto match = levelRe.match(line);
    if (match.hasMatch()) {
        level = StrToLevel(match.captured(1));
        return true;
    }
    return false;
}

static void checkClassifierLine(const LineClassifier& classifier,
                                const RefTimestampRes& tsRe,
                                const QRegularExpression& levelRe,
                                const QString& line)
{
    QDateTime tsNew, tsRef;
    bool leadNew = false, leadRef = false, leadMsNew = false;
    const bool hasNew = classifier.detectTimestamp(line, tsNew, tsRe.year, &leadNew);
    const bool hasRef = refDetectTimestamp(tsRe, line, tsRef, &leadRef);
    if (hasNew != hasRef || (hasNew && tsNew != tsRef) || leadNew != leadRef) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL classifier ts mismatch on '%s': new=%d '%s' leading=%d ref=%d '%s' leading=%d\n",
                     qPrintable(line), int(hasNew),
                     qPrintable(tsNew.toString(Qt::ISODateWithMs)), int(leadNew), int(hasRef),
                     qPrintable(tsRef.toString(Qt::ISODateWithMs)), int(leadRef));
    }

    // Быстрый вариант индексатора обязан давать те же мс epoch.
    qint64 msNew = -2;
    const bool hasMsNew = classifier.detectTimestampMs(line, msNew, tsRe.year, &leadMsNew);
    if (hasMsNew != hasRef || (hasMsNew && msNew != tsRef.toMSecsSinceEpoch())
        || leadMsNew != leadRef) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL classifier tsMs mismatch on '%s': new=%d %lld ref=%d %lld\n",
                     qPrintable(line), int(hasMsNew), (long long)msNew, int(hasRef),
                     (long long)(hasRef ? tsRef.toMSecsSinceEpoch() : -1));
    }

    // С контекстом файла (F-11): одиночная строка, без предпросмотра —
    // дата по опорному моменту; остальные формы — те же.
    {
        ClockDates clock;
        QDateTime tsClock, tsClockRef;
        bool leadClock = false, leadClockRef = false;
        const bool hasClock = classifier.detectTimestamp(line, tsClock, tsRe.year, &leadClock, &clock);
        const bool hasClockRef = refDetectTimestamp(tsRe, line, tsClockRef, &leadClockRef, true);
        if (hasClock != hasClockRef || (hasClock && tsClock != tsClockRef) || leadClock != leadClockRef) {
            ++g_failures;
            std::fprintf(stderr,
                         "FAIL classifier clock mismatch on '%s': new=%d '%s' leading=%d ref=%d '%s' leading=%d\n",
                         qPrintable(line), int(hasClock),
                         qPrintable(tsClock.toString(Qt::ISODateWithMs)), int(leadClock),
                         int(hasClockRef), qPrintable(tsClockRef.toString(Qt::ISODateWithMs)),
                         int(leadClockRef));
        }
        // matched — метку дала форма без даты: всё, что нашлось только с
        // контекстом, и ничего, чего нет.
        if ((hasClock && !hasNew && !clock.matched) || (clock.matched && !hasClock)
            || (clock.matched && !clock.state.isSet())) {
            ++g_failures;
            std::fprintf(stderr, "FAIL clock.matched on '%s'\n", qPrintable(line));
        }
    }

    LogLevel lvlNew = LogLevel::Unknown, lvlRef = LogLevel::Unknown;
    const bool gotNew = classifier.detectLogLevel(line, lvlNew);
    const bool gotRef = refDetectLogLevel(levelRe, line, lvlRef);
    if (gotNew != gotRef || (gotNew && lvlNew != lvlRef)) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL classifier level mismatch on '%s': new=%d/%s ref=%d/%s\n",
                     qPrintable(line), int(gotNew), qPrintable(LevelToStr(lvlNew)),
                     int(gotRef), qPrintable(LevelToStr(lvlRef)));
    }
}

static void testClassifierRegexEquivalence()
{
    const LineClassifier classifier;
    // Опорная дата года — посреди года, чтобы метки syslog по обе стороны
    // от неё уходили в разные годы.
    RefTimestampRes tsRe{
        QRegularExpression(PatternHeuristics::isoTimestampDetectPattern()),
        QRegularExpression(PatternHeuristics::accessTimestampDetectPattern()),
        QRegularExpression(PatternHeuristics::syslogTimestampDetectPattern()),
        QRegularExpression(PatternHeuristics::timestampSuffixDetectPattern()),
        LineClassifier::yearReference(QDate(2026, 7, 11)) };
    // Регистр сворачивается в самом шаблоне и только для ASCII: опция
    // CaseInsensitive приравняла бы «ſ» к S, а знак Кельвина — к K.
    const QRegularExpression levelRe(PatternHeuristics::levelDetectPattern());
    CHECK(tsRe.iso.isValid() && tsRe.access.isValid() && tsRe.syslog.isValid()
              && tsRe.suffix.isValid() && levelRe.isValid(),
          "reference timestamp and level regexes compile");
    const auto check = [&](const QString& line) {
        checkClassifierLine(classifier, tsRe, levelRe, line);
    };

    const QStringList corpus = {
        QString(),
        QStringLiteral("short"),
        QStringLiteral("2026-07-11 10:22:33 plain"),
        QStringLiteral("2026-07-11T10:22:33 with T"),
        QStringLiteral("2026-07-11 10:22:33.123 INFO dot millis"),
        QStringLiteral("2026-07-11 10:22:33,7 WARN comma millis"),
        QStringLiteral("prefix 2026-07-11 10:22:33 ERROR mid-line"),
        QStringLiteral("12026-07-11 10:22:33 leading digit"),
        QStringLiteral("2026-13-11 10:22:33 bad month"),
        QStringLiteral("2026-02-30 10:22:33 bad day"),
        QStringLiteral("2026-07-11 25:22:33 bad hour"),
        QStringLiteral("0000-01-01 00:00:00 year zero"),
        QStringLiteral("9999-99-99 99:99:99 all nines"),
        QStringLiteral("2026-07-11 10:22:33.1234 millis out of range"),
        QStringLiteral("2026-07-11 10:22:33.999999999999 millis overflow"),
        QStringLiteral("2026-07-11 10:22:33. dot without digits"),
        QStringLiteral("2026-07-11 10:22:33,x comma without digits"),
        QStringLiteral("2026-07-11 10:22:33abc suffix"),
        QStringLiteral("2026-07-11 10:22:3 too short"),
        QStringLiteral("x2026-07-11Q10:22:33 bad separator"),
        QStringLiteral("2026-13-40 00:00:00 then 2026-07-11 10:22:33 valid later"),
        QStringLiteral("11/07/2026 10:22:33 dd/MM fallback"),
        QStringLiteral("07/14/2026 10:22:33 MM/dd fallback"),
        QStringLiteral("31.12.2025 23:59:59 dotted fallback"),
        QStringLiteral("99/99/9999 10:22:33 bad fallback"),
        QStringLiteral("ИНФО 2026-07-11 10:22:33,42 кириллица"),
        QStringLiteral("Ошибка ERROR после кириллицы"),
        QStringLiteral("INFO"),
        QStringLiteral("info lowercase"),
        QStringLiteral("An INFO word"),
        QStringLiteral("INFOS not a word"),
        QStringLiteral("M_INFO underscore boundary"),
        QStringLiteral("WARNING before WARN"),
        QStringLiteral("warning lowercase long"),
        QStringLiteral("Warn: colon after"),
        QStringLiteral("[ERROR] in brackets"),
        QStringLiteral("error_code no boundary"),
        QStringLiteral("TRACE1 trailing digit"),
        QStringLiteral("FATAL. trailing dot"),
        QStringLiteral("fatal error two keywords"),
        QStringLiteral("DEBUGGING then DEBUG"),
        QStringLiteral("ıNFO turkish dotless i"),
        QStringLiteral("İNFO turkish dotted I"),
        QStringLiteral("2026-07-11 10:22:33,123 WARN [net] полная строка"),
        QStringLiteral("\ttab\tINFO\ttabs"),
        QStringLiteral("2026-07-11 10:22:33 CRITICAL disk full"),
        QStringLiteral("2026-07-11 10:22:33 [crit] nginx style"),
        QStringLiteral("SEVERE: java.util.logging"),
        QStringLiteral("<3>ERR syslog"),
        QStringLiteral("ERR0R not a word"),
        QStringLiteral("ERRO four letters"),
        QStringLiteral("errors plural"),
        QStringLiteral("[10:22:33 INF] serilog"),
        QStringLiteral("[10:22:33 WRN] serilog"),
        QStringLiteral("[10:22:33 DBG] serilog"),
        QStringLiteral("[10:22:33 VRB] serilog"),
        QStringLiteral("[10:22:33 FTL] serilog"),
        QStringLiteral("loss=inf value"),
        QStringLiteral("notice lowercase"),
        QStringLiteral("NOTICED not a word"),
        QStringLiteral("Verbose mode"),
        QStringLiteral("verbo\u017Fe long s is not S"),
        QStringLiteral("\u017Fevere long s first"),
        QStringLiteral("\u212AERNEL kelvin"),
        QStringLiteral("[emerg] system is unusable"),
        QStringLiteral("[alert] action required"),
        QStringLiteral("panic: runtime error"),
        QStringLiteral("CRITICALITY then CRIT"),
        QStringLiteral("critical_section then severe"),
        QStringLiteral("FINE CONFIG FINER FINEST are not levels"),
        QStringLiteral("a ab abc abcd abcde abcdef abcdefg abcdefgh abcdefghi"),
        QStringLiteral("00000000-0000-0000 00:00:00"),
        QStringLiteral("2026-07-11T10:22:33Z INFO utc"),
        QStringLiteral("2026-07-11T10:22:33.5z lowercase z"),
        QStringLiteral("2026-07-11T10:22:33Zulu word after Z"),
        QStringLiteral("2026-07-11T10:22:33Z_ underscore after Z"),
        QStringLiteral("2026-07-11T10:22:33+02:00 colon offset"),
        QStringLiteral("2026-07-11T10:22:33-0530 compact offset"),
        QStringLiteral("2026-07-11T10:22:33+02 hours offset"),
        QStringLiteral("2026-07-11T10:22:33+2 one digit"),
        QStringLiteral("2026-07-11T10:22:33+023 three digits"),
        QStringLiteral("2026-07-11T10:22:33+02001 five digits"),
        QStringLiteral("2026-07-11T10:22:33+02:0012 digits after minutes"),
        QStringLiteral("2026-07-11T10:22:33+02:0 short minutes"),
        QStringLiteral("2026-07-11T10:22:33+24:00 hour out of range"),
        QStringLiteral("2026-07-11T10:22:33+02:60 minute out of range"),
        QStringLiteral("2026-07-11 10:22:33 +0200 spaced compact"),
        QStringLiteral("2026-07-11 10:22:33 -05:30 spaced colon"),
        QStringLiteral("2026-07-11 10:22:33 +02 spaced hours only"),
        QStringLiteral("2026-07-11 10:22:33 +02:001 spaced extra digit"),
        QStringLiteral("2026-07-11 10:22:33.123 UTC [1] LOG"),
        QStringLiteral("2026-07-11 10:22:33 GMT"),
        QStringLiteral("2026-07-11 10:22:33 UTC+3"),
        QStringLiteral("2026-07-11 10:22:33 UTCx"),
        QStringLiteral("2026-07-11 10:22:33 utc lowercase"),
        QStringLiteral("2026-07-11 10:22:33  UTC two spaces"),
        QStringLiteral("2026-07-11 10:22:33. +02:00 dot then zone"),
        QStringLiteral("2026-07-11 10:22:33.123456789Z nanos"),
        QStringLiteral("2026-07-11 10:22:33,999999999999 long fraction"),
        QStringLiteral("11/07/2026 10:22:33.25Z fallback with suffix"),
        QStringLiteral("31.12.2025 23:59:59,5 +0100 dotted with zone"),
        // nginx error: dddd/dd/dd.
        QStringLiteral("2026/11/18 13:55:36 [error] 1234#1234: *99 open()"),
        QStringLiteral("2026/11/18T13:55:36Z slashed with T"),
        QStringLiteral("2026/11-18 13:55:36 mixed separators"),
        QStringLiteral("2026-11/18 13:55:36 mixed separators back"),
        QStringLiteral("2026/13/18 13:55:36 bad month slashed"),
        QStringLiteral("2026.11.18 13:55:36 dotted is not a form"),
        QStringLiteral("prefix 2026/11/18 13:55:36 mid-line slashed"),
        // access-лог: dd/Mmm/dddd:dd:dd:dd.
        QStringLiteral("127.0.0.1 - frank [10/Oct/2026:13:55:36 -0700] \"GET /a HTTP/1.0\" 200 2326"),
        QStringLiteral("[18/Nov/2026:13:55:36 +0000]"),
        QStringLiteral("18/Nov/2026:13:55:36 at start without zone"),
        QStringLiteral("18/nov/2026:13:55:36 lowercase month"),
        QStringLiteral("18/NOV/2026:13:55:36 upper month"),
        QStringLiteral("18/Nob/2026:13:55:36 not a month"),
        QStringLiteral("31/Nov/2026:13:55:36 +0000 bad day"),
        QStringLiteral("29/Feb/2028:23:59:59 +0100 leap"),
        QStringLiteral("29/Feb/2027:23:59:59 not leap"),
        QStringLiteral("18/Nov/2026:24:00:00 +0000 bad hour"),
        QStringLiteral("18/Nov/2026:13:55:36.250+0530 fraction and glued zone"),
        QStringLiteral("18/Nov/2026 13:55:36 space instead of colon"),
        QStringLiteral("x 18/Nov/2026:13:55:36 then 2026-11-17 10:00:00"),
        QStringLiteral("x 2026-11-17 10:00:00 then 18/Nov/2026:13:55:36"),
        // syslog RFC 3164: только с начала строки.
        QStringLiteral("Nov 18 13:55:36 web01 sshd[2231]: Accepted"),
        QStringLiteral("Nov  8 03:04:05 web01 kernel: usb"),
        QStringLiteral("Nov 8 03:04:05 single space"),
        QStringLiteral("Nov 08 03:04:05 zero padded"),
        QStringLiteral("Jul 11 10:22:33 the reference day"),
        QStringLiteral("Jul 12 10:22:33 the day after reference"),
        QStringLiteral("Jul 13 10:22:33 two days after reference"),
        QStringLiteral("Jan  1 00:00:00 new year"),
        QStringLiteral("Dec 31 23:59:59 old year"),
        QStringLiteral("Feb 29 12:00:00 leap day"),
        QStringLiteral("Feb 30 12:00:00 no such day"),
        QStringLiteral("Nov 31 12:00:00 no such day"),
        QStringLiteral("Nov  0 12:00:00 day zero"),
        QStringLiteral("nov 18 13:55:36 lowercase"),
        QStringLiteral("NOV 18 13:55:36 uppercase"),
        QStringLiteral("Foo 18 13:55:36 not a month"),
        QStringLiteral("Nov 18 13:55 no seconds"),
        QStringLiteral("Nov 18 13:55:3 short seconds"),
        QStringLiteral("Nov 18  13:55:36 two spaces before time"),
        QStringLiteral("Nov   8 13:55:36 three spaces"),
        QStringLiteral("Nov 123 13:55:36 three digit day"),
        QStringLiteral("Nov 18 13:55:36.123456 high precision"),
        QStringLiteral("Nov 18 13:55:36 +0200 zone after space"),
        QStringLiteral("Nov 18 13:55:36Z glued zone"),
        QStringLiteral(" Nov 18 13:55:36 leading space"),
        QStringLiteral("at Nov 18 13:55:36 the job failed"),
        QStringLiteral("Nov 18 13:55:36 app: started 2026-11-17 10:00:00"),
        QStringLiteral("Nov 18 25:55:36 bad hour falls back to 2026-11-17 10:00:00"),
        QStringLiteral("11/07/2026 10:22:33 then 2026-07-11 10:22:33 later"),
        QStringLiteral("99/99/2026 10:22:33 then 2026-07-11 10:22:33 later"),
        QStringLiteral("Nov 18 13:55:36 then 2026-13-40 00:00:00 invalid later"),
        QStringLiteral("x 2026-13-40 00:00:00 invalid first then 2026-07-11 10:22:33"),
        // F-12: ведущая метка — с позиции 0, с позиции 1 за «[», access за «[».
        QStringLiteral("[2026-07-11 10:22:33] bracketed"),
        QStringLiteral("[2026/07/11 10:22:33] bracketed slashed"),
        QStringLiteral("[[2026-07-11 10:22:33]] two brackets"),
        QStringLiteral(" [2026-07-11 10:22:33] space then bracket"),
        QStringLiteral("x [2026-07-11 10:22:33] iso after a word and bracket"),
        QStringLiteral("2001:db8::1 - alice@example.com [18/Nov/2026:13:55:37 +0000] \"POST /api HTTP/2\" 201 0"),
        QStringLiteral(" 10.0.0.1 - - [18/Nov/2026:13:55:36 +0000] indented access"),
        QStringLiteral("\t10.0.0.1 - - [18/Nov/2026:13:55:36 +0000] tab-indented access"),
        QStringLiteral("10.0.0.1 - - 18/Nov/2026:13:55:36 access without bracket"),
        QStringLiteral("\"time\": \"[18/Nov/2026:13:55:36 +0000]\" quoted access"),
        QStringLiteral("  \"ts\": \"2026-11-18T10:00:00Z\","),
        QStringLiteral("\"created\": \"2026-11-18 10:00:00\","),
        QStringLiteral("Response: {\"ts\":\"2026-11-18T10:00:00Z\"}"),
        QStringLiteral("<timestamp>2026-11-18T10:00:00</timestamp>"),
        QStringLiteral("\t2026-11-18 10:00:00 tab then iso"),
        QStringLiteral("[2026-13-40 00:00:00] bracketed invalid"),
        QStringLiteral("["),
        // F-11: время без даты — с позиции 0 или 1 за «[», только с контекстом.
        QStringLiteral("13:55:40.016 [main] INFO c.e.App - started"),
        QStringLiteral("[13:55:36 INF] Starting web host"),
        QStringLiteral("00:00:00 midnight"),
        QStringLiteral("23:59:59.999 last millisecond"),
        QStringLiteral("23:59:59.9999999 long fraction"),
        QStringLiteral("13:55:36,5 comma fraction"),
        QStringLiteral("13:55:36, comma without digits"),
        QStringLiteral("13:55:36."),
        QStringLiteral("13:55:36"),
        QStringLiteral("[13:55:36]"),
        QStringLiteral("24:00:00 bad hour"),
        QStringLiteral("13:60:00 bad minute"),
        QStringLiteral("13:55:60 bad second"),
        QStringLiteral("13:55:36:00 four parts"),
        QStringLiteral("13:55:361 digit after seconds"),
        QStringLiteral("13:55 no seconds"),
        QStringLiteral("1:02:03 one-digit hour"),
        QStringLiteral(" 13:55:36 leading space"),
        QStringLiteral("\t13:55:36 leading tab"),
        QStringLiteral("[[13:55:36] two brackets"),
        QStringLiteral("x 13:55:36 in the middle"),
        QStringLiteral("took 00:01:23 to finish"),
        QStringLiteral("13:55:36Z zone is text"),
        QStringLiteral("13:55:36 +0300 zone is text"),
        QStringLiteral("13:55:36abc letters after"),
        QStringLiteral("[13:55:36] then 2026-11-17 10:00:00 later"),
        QStringLiteral("25:00:00 bad then 2026-11-17 10:00:00 later"),
        QStringLiteral("13:55:36 ERROR then [18/Nov/2026:13:55:36 +0000]"),
        QStringLiteral("13-55-36 dashes"),
        QStringLiteral("１３:55:36 fullwidth digit"),
    };
    for (const QString& line : corpus)
        check(line);

    // Детерминированный фаззинг: плотный алфавит вокруг цифр, разделителей,
    // зон и букв ключевых слов, чтобы случайно собирались почти-совпадения.
    const QString alphabet = QStringLiteral(
        "0123456789-+:.,/ TIWEDFANROGLZzUCMest_[]абвШKſ");
    QRandomGenerator rng(1234567);
    for (int iter = 0; iter < 20000; ++iter) {
        const int len = int(rng.bounded(61));
        QString line;
        line.reserve(len);
        for (int k = 0; k < len; ++k)
            line.append(alphabet.at(int(rng.bounded(alphabet.size()))));
        check(line);
    }

    // Фаззинг «почти таймстампов»: валидные основы с точечными искажениями.
    const QStringList bases = {
        QStringLiteral("2026-07-11 10:22:33,123 INFO msg"),
        QStringLiteral("2026-07-11T10:22:33.123+02:00 INFO msg"),
        QStringLiteral("2026-07-11 10:22:33 -0530 WARN msg"),
        QStringLiteral("2026-07-11 10:22:33.5 UTC ERROR msg"),
        QStringLiteral("11.07.2026 10:22:33,1Z INFO msg"),
        QStringLiteral("2026/07/11 10:22:33 [error] 1#1: msg"),
        QStringLiteral("1.2.3.4 - - [11/Jul/2026:10:22:33 +0200] \"GET /\" 200 1"),
        QStringLiteral("Jul 11 10:22:33 host app[1]: msg"),
        QStringLiteral("Jul  1 10:22:33.5 host app: msg"),
        QStringLiteral("Dec 1 10:22:33 host app: msg"),
    };
    for (const QString& base : bases) {
        for (int iter = 0; iter < 4000; ++iter) {
            QString line = base;
            const int pos = int(rng.bounded(line.size()));
            line[pos] = alphabet.at(int(rng.bounded(alphabet.size())));
            check(line);
        }
    }

    // Фаззинг слов уровней: строка из ключевых слов, их обрезков и склеек со
    // случайным регистром и разделителями. Случайный алфавит выше собирает
    // трёхбуквенные слова, но почти никогда — CRITICAL или VERBOSE.
    const QStringList pieces = {
        QStringLiteral("TRACE"), QStringLiteral("DEBUG"), QStringLiteral("INFO"),
        QStringLiteral("WARN"), QStringLiteral("WARNING"), QStringLiteral("ERROR"),
        QStringLiteral("FATAL"), QStringLiteral("CRITICAL"), QStringLiteral("CRIT"),
        QStringLiteral("SEVERE"), QStringLiteral("EMERG"), QStringLiteral("ALERT"),
        QStringLiteral("PANIC"), QStringLiteral("FTL"), QStringLiteral("ERR"),
        QStringLiteral("WRN"), QStringLiteral("INF"), QStringLiteral("NOTICE"),
        QStringLiteral("DBG"), QStringLiteral("VERBOSE"), QStringLiteral("VRB"),
        QStringLiteral("FINE"), QStringLiteral("CONFIG"), QStringLiteral("LOG"),
        QStringLiteral("x"), QStringLiteral("S"), QStringLiteral("_"),
        QStringLiteral("7"), QStringLiteral("\u017F"), QStringLiteral("\u212A"),
        QStringLiteral("я"),
    };
    const QString seps = QStringLiteral("  [],:.-=/");
    for (int iter = 0; iter < 20000; ++iter) {
        QString line;
        const int count = 1 + int(rng.bounded(5));
        for (int k = 0; k < count; ++k) {
            QString piece = pieces.at(int(rng.bounded(pieces.size())));
            const int mode = int(rng.bounded(6));
            if (mode == 0)
                piece = piece.toLower();
            else if (mode == 1 && piece.size() > 1)
                piece = piece.left(1) + piece.mid(1).toLower();
            else if (mode == 2 && piece.size() > 1)
                piece.chop(1);                       // обрезок: «CRITICA»
            else if (mode == 3)
                piece[int(rng.bounded(piece.size()))] =
                    alphabet.at(int(rng.bounded(alphabet.size())));
            line += piece;
            // Иногда без разделителя — склейка: «ERRWARN» не уровень.
            if (rng.bounded(4) != 0)
                line += seps.at(int(rng.bounded(seps.size())));
        }
        check(line);
    }

    // Фаззинг меток с названиями месяцев: склейки из месяцев (и их
    // обрезков), чисел, пробелов и разделителей — случайный алфавит выше
    // почти никогда не собирает «Nov 18 13:55:36» или «18/Nov/2026:…».
    const QStringList tsPieces = {
        QStringLiteral("Jan"), QStringLiteral("Feb"), QStringLiteral("feb"),
        QStringLiteral("JUL"), QStringLiteral("Nov"), QStringLiteral("Dec"),
        QStringLiteral("No"), QStringLiteral("Nox"), QStringLiteral("2026"),
        QStringLiteral("2028"), QStringLiteral("1"), QStringLiteral("8"),
        QStringLiteral("18"), QStringLiteral("29"), QStringLiteral("31"),
        QStringLiteral("00"), QStringLiteral("13:55:36"), QStringLiteral("23:59:59"),
        QStringLiteral("13:55"), QStringLiteral("+0000"), QStringLiteral("-0700"),
        QStringLiteral(".123"), QStringLiteral("Z"), QStringLiteral("UTC"),
        QStringLiteral("2026-07-11"), QStringLiteral("2026/07/11"),
        QStringLiteral("[13:55:36"), QStringLiteral("00:00:00.5"), QStringLiteral(",25"),
        QStringLiteral("24:00:00"),
    };
    const QString tsSeps = QStringLiteral("  /:[]-.T");
    for (int iter = 0; iter < 30000; ++iter) {
        QString line;
        const int count = 1 + int(rng.bounded(7));
        for (int k = 0; k < count; ++k) {
            line += tsPieces.at(int(rng.bounded(tsPieces.size())));
            if (rng.bounded(3) != 0)
                line += tsSeps.at(int(rng.bounded(tsSeps.size())));
        }
        check(line);
    }

    // F-11: опорный момент у полуночи и посреди суток — одна и та же строка
    // с временем без даты получает разные даты.
    const LineClassifier::YearReference savedYear = tsRe.year;
    for (const QDateTime& anchor : { QDateTime(QDate(2026, 11, 18), QTime(0, 0, 3)),
                                     QDateTime(QDate(2026, 11, 18), QTime(23, 59, 50)),
                                     QDateTime(QDate(2026, 11, 18), QTime(11, 59, 59, 999)),
                                     QDateTime(QDate(2026, 11, 18), QTime(12, 0)),
                                     QDateTime(QDate(2026, 3, 29), QTime(2, 30)) }) {
        tsRe.year = LineClassifier::referenceAt(anchor);
        for (const char* l : { "00:00:00 a", "00:00:02 a", "11:59:59.999 a", "12:00:00 a",
                               "12:00:00.001 a", "13:55:36 a", "23:59:58 a", "[23:59:59.5 a",
                               "02:30:00 a", "03:30:00 a" })
            check(QString::fromLatin1(l));
    }
    tsRe.year = savedYear;

    // Опорная дата у границы года: та же основа даёт разные годы.
    for (const QDate& refDate : { QDate(2026, 12, 31), QDate(2027, 1, 1), QDate(2028, 2, 28),
                                  QDate(2028, 2, 29), QDate(2029, 3, 1) }) {
        tsRe.year = LineClassifier::yearReference(refDate);
        for (const char* l : { "Jan  1 00:00:00 a", "Jan  2 00:00:00 a", "Dec 31 23:59:59 a",
                               "Feb 28 10:00:00 a", "Feb 29 10:00:00 a", "Mar  1 10:00:00 a",
                               "Mar  2 10:00:00 a", "Nov 18 13:55:36 a" })
            check(QString::fromLatin1(l));
    }
}

// Словарь уровней на заранее заданном соответствии — независимо от таблицы
// kLevelWords, из которой построены регексы: ошибку в самой таблице сверка
// сканера с регексом не увидит. Это же соответствие описано в справке.
static void testLevelVocabulary()
{
    const struct { const char* word; LogLevel level; } expected[] = {
        { "FATAL", LogLevel::Fatal },    { "CRITICAL", LogLevel::Fatal },
        { "CRIT", LogLevel::Fatal },     { "SEVERE", LogLevel::Fatal },
        { "EMERG", LogLevel::Fatal },    { "FTL", LogLevel::Fatal },
        { "ERROR", LogLevel::Error },    { "ERR", LogLevel::Error },
        { "WARNING", LogLevel::Warn },   { "WARN", LogLevel::Warn },
        { "WRN", LogLevel::Warn },
        { "INFO", LogLevel::Info },      { "INF", LogLevel::Info },
        { "NOTICE", LogLevel::Info },
        { "DEBUG", LogLevel::Debug },    { "DBG", LogLevel::Debug },
        { "TRACE", LogLevel::Trace },    { "VERBOSE", LogLevel::Trace },
        { "VRB", LogLevel::Trace },
    };
    const LineClassifier classifier;
    const QRegularExpression blockRe(
        QRegularExpression::anchoredPattern(PatternHeuristics::levelRegex()));
    CHECK(blockRe.isValid(), "Level block regex compiles");

    const auto classify = [&](const QString& line) {
        LogLevel lvl = LogLevel::Unknown;
        return classifier.detectLogLevel(line, lvl) ? lvl : LogLevel::Unknown;
    };
    const auto expectLevel = [&](const QString& line, LogLevel want) {
        const LogLevel got = classify(line);
        if (got != want) {
            ++g_failures;
            std::fprintf(stderr, "FAIL level of '%s': got %s, want %s\n", qPrintable(line),
                         qPrintable(LevelToStr(got)), qPrintable(LevelToStr(want)));
        }
    };

    int count = 0;
    for (const auto& e : expected) {
        ++count;
        const QString upper = QString::fromLatin1(e.word);
        const QString lower = upper.toLower();
        const QString title = upper.left(1) + lower.mid(1);
        for (const QString& w : { upper, lower, title }) {
            expectLevel(w, e.level);
            expectLevel(QStringLiteral("2026-07-11 10:22:33 [%1] message").arg(w), e.level);
            expectLevel(QStringLiteral("%1: message").arg(w), e.level);
            if (StrToLevel(w) != e.level || StrToLevel(QStringLiteral(" %1 ").arg(w)) != e.level) {
                ++g_failures;
                std::fprintf(stderr, "FAIL StrToLevel('%s')\n", qPrintable(w));
            }
            if (!blockRe.match(w).hasMatch()) {
                ++g_failures;
                std::fprintf(stderr, "FAIL Level block does not accept '%s'\n", qPrintable(w));
            }
        }
        // Слово внутри идентификатора — не уровень.
        expectLevel(upper + QStringLiteral("X"), LogLevel::Unknown);
        expectLevel(QStringLiteral("X") + upper, LogLevel::Unknown);
        expectLevel(upper + QStringLiteral("_1"), LogLevel::Unknown);
    }
    // Таблица приложения и это соответствие — одно и то же множество.
    int tableSize = 0;
    for (const LevelWord& w : kLevelWords) {
        ++tableSize;
        bool found = false;
        for (const auto& e : expected)
            found = found || (qstrcmp(e.word, w.word) == 0 && e.level == w.level);
        if (!found) {
            ++g_failures;
            std::fprintf(stderr, "FAIL kLevelWords has unexpected '%s'\n", w.word);
        }
    }
    CHECK(tableSize == count, "kLevelWords size matches the documented vocabulary");

    // Намеренно НЕ уровни (решение Q-2): обычные слова и чужие обрезки.
    for (const char* w : { "ALERT", "PANIC", "FINE", "FINER", "FINEST", "CONFIG", "FAIL", "LOG", "ERRO",
                           "WARNINGS", "INFORM", "NOTE", "CRITIC", "EMERGENCY", "DBUG" }) {
        const QString word = QString::fromLatin1(w);
        expectLevel(word, LogLevel::Unknown);
        CHECK(StrToLevel(word) == LogLevel::Unknown, "non-level word has no level");
        CHECK(!blockRe.match(word).hasMatch(), "Level block rejects a non-level word");
    }
    // Не-ASCII «двойники» букв — не буквы ключевых слов.
    expectLevel(QStringLiteral("verbo\u017Fe"), LogLevel::Unknown);
    CHECK(StrToLevel(QStringLiteral("verbo\u017Fe")) == LogLevel::Unknown, "long s is not S");
    CHECK(!blockRe.match(QStringLiteral("verbo\u017Fe")).hasMatch(), "Level block: long s is not S");

    // Побеждает самое левое слово.
    expectLevel(QStringLiteral("notice: fatal error"), LogLevel::Info);
    expectLevel(QStringLiteral("ERR while handling WARNING"), LogLevel::Error);
    expectLevel(QStringLiteral("errors 3, crit 1"), LogLevel::Fatal);
    // Обычное слово перед настоящим уровнем его не перебивает — поэтому
    // ALERT и PANIC в словарь не взяты.
    expectLevel(QStringLiteral("2026-07-11 10:22:33,123 - alert.manager - ERROR - failed"),
                LogLevel::Error);
    expectLevel(QStringLiteral("2026-07-11 10:22:33 [panic-handler] WARN recovered"),
                LogLevel::Warn);
}

// Политика времени на заранее известных моментах — независимо от эталонной
// регекс-реализации, которая могла бы ошибаться так же, как сканер.
static void testTimestampGolden()
{
    const LineClassifier classifier;
    const auto utc = [](int y, int mo, int d, int h, int mi, int s, int ms) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms), QTimeZone::UTC)
            .toMSecsSinceEpoch();
    };
    const auto local = [](int y, int mo, int d, int h, int mi, int s, int ms) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms)).toMSecsSinceEpoch();
    };
    const qint64 tenUtc = utc(2026, 9, 20, 10, 0, 0, 0);
    const qint64 tenLocal = local(2026, 9, 20, 10, 0, 0, 0);
    constexpr qint64 kNoTimestamp = std::numeric_limits<qint64>::min();

    const struct { const char* line; qint64 expected; } cases[] = {
        // Одинаковый момент в разных записях зоны.
        { "2026-09-20T10:00:00Z INFO", tenUtc },
        { "2026-09-20T10:00:00z", tenUtc },
        { "2026-09-20T12:00:00+02:00 INFO", tenUtc },
        { "2026-09-20T12:00:00+0200", tenUtc },
        { "2026-09-20T12:00:00+02 INFO", tenUtc },
        { "2026-09-20T04:30:00-05:30", tenUtc },
        { "2026-09-20 12:00:00 +0200 git --date=iso", tenUtc },
        { "2026-09-20 04:30:00 -05:30 msg", tenUtc },
        { "2026-09-20 10:00:00 GMT", tenUtc },
        { "2026-09-20 10:00:00.123 UTC [42] LOG: postgres", utc(2026, 9, 20, 10, 0, 0, 123) },
        // Дробная часть — доли секунды, усечение до миллисекунд.
        { "2026-09-20T10:00:00.1Z", utc(2026, 9, 20, 10, 0, 0, 100) },
        { "2026-09-20T10:00:00.12Z", utc(2026, 9, 20, 10, 0, 0, 120) },
        { "2026-09-20T10:00:00.123Z", utc(2026, 9, 20, 10, 0, 0, 123) },
        { "2026-09-20T10:00:00,5Z", utc(2026, 9, 20, 10, 0, 0, 500) },
        { "2026-09-20T10:00:00.123456Z", utc(2026, 9, 20, 10, 0, 0, 123) },
        { "2026-09-20T10:00:00.123456789Z", utc(2026, 9, 20, 10, 0, 0, 123) },
        { "2026-09-20T10:00:00.1234567890123Z", utc(2026, 9, 20, 10, 0, 0, 123) },
        // Переход суток, года, 29 февраля, крайние смещения.
        { "2026-09-20T01:00:00+03:00", utc(2026, 9, 19, 22, 0, 0, 0) },
        { "2026-12-31T23:30:00-01:00", utc(2027, 1, 1, 0, 30, 0, 0) },
        { "2028-02-29T23:59:59.999-00:30", utc(2028, 3, 1, 0, 29, 59, 999) },
        { "2026-01-01T00:00:00+14:00", utc(2025, 12, 31, 10, 0, 0, 0) },
        { "2026-01-01T00:00:00-12:00", utc(2026, 1, 1, 12, 0, 0, 0) },
        { "1970-01-01T00:00:00Z", 0 },
        // Фолбэк-форматы получают ту же дробь и зону.
        { "20.09.2026 12:00:00,250+02:00 INFO", utc(2026, 9, 20, 10, 0, 0, 250) },
        { "20/09/2026 10:00:00Z", tenUtc },
        { "20.09.2026 10:00:00,5 INFO", local(2026, 9, 20, 10, 0, 0, 500) },
        // Без зоны — локальное время машины.
        { "2026-09-20 10:00:00 INFO", tenLocal },
        { "2026-09-20 10:00:00,5 INFO", local(2026, 9, 20, 10, 0, 0, 500) },
        // Похоже на зону, но не зона — тоже локальное время.
        { "2026-09-20T10:00:00Zulu", tenLocal },
        { "2026-09-20 10:00:00 UTC+3", tenLocal },
        { "2026-09-20 10:00:00 +1 points", tenLocal },
        { "2026-09-20T10:00:00+25:00", tenLocal },
        { "2026-09-20T10:00:00-14:30", tenLocal },
        { "2026-09-20T10:00:00+02:75", tenLocal },
        { "2026-09-20 10:00:00 +02:0", tenLocal },
        { "2026-09-20 10:00:00 - message", tenLocal },
        // Известная неоднозначность: четыре цифры со знаком через пробел —
        // это зона, даже если автор имел в виду число в тексте сообщения.
        { "2026-09-20 10:00:00 -1000 balance", utc(2026, 9, 20, 20, 0, 0, 0) },
        // Невалидная дата — не таймстамп, зона не спасает.
        { "2026-02-30T10:00:00Z", kNoTimestamp },
        // nginx error: dddd/dd/dd, без зоны — локальное время.
        { "2026/09/20 10:00:00 [error] 1#1: x", tenLocal },
        { "2026/09/20T12:00:00+02:00", tenUtc },
        // access-лог: зона через пробел внутри скобок; без зоны — локальное.
        { "1.2.3.4 - - [20/Sep/2026:10:00:00 +0000] \"GET /\" 200 1", tenUtc },
        { "1.2.3.4 - - [20/Sep/2026:03:00:00 -0700] \"GET /\" 200 1", tenUtc },
        { "[20/Sep/2026:15:30:00 +0530]", tenUtc },
        { "[20/sep/2026:10:00:00]", tenLocal },
        { "[20/Sep/2026:10:00:00.5 +0000]", utc(2026, 9, 20, 10, 0, 0, 500) },
        { "[01/Jan/2027:01:00:00 +0200]", utc(2026, 12, 31, 23, 0, 0, 0) },
        { "[29/Feb/2028:23:30:00 -0100]", utc(2028, 3, 1, 0, 30, 0, 0) },
        // Похоже на метку, но не метка.
        { "[29/Feb/2027:10:00:00 +0000]", kNoTimestamp },
        { "[31/Nov/2026:10:00:00 +0000]", kNoTimestamp },
        { "[20/Sept/2026:10:00:00 +0000]", kNoTimestamp },
        { "[20/Sep/2026 10:00:00 +0000]", kNoTimestamp },
        { "2026.09.20 10:00:00", kNoTimestamp },
        { "2026/09-20 10:00:00", kNoTimestamp },
        // Метка в начале строки главнее метки в середине.
        { "20/09/2026 10:00:00Z then 2026-01-01 00:00:00Z", tenUtc },
    };
    for (const auto& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        qint64 ms = kNoTimestamp;
        const bool found = classifier.detectTimestampMs(line, ms);
        QDateTime ts;
        const bool foundDt = classifier.detectTimestamp(line, ts);
        const qint64 got = found ? ms : kNoTimestamp;
        const qint64 gotDt = foundDt ? ts.toMSecsSinceEpoch() : kNoTimestamp;
        if (got != c.expected || gotDt != c.expected) {
            ++g_failures;
            std::fprintf(stderr, "FAIL golden timestamp '%s': ms=%lld dt=%lld expected=%lld\n",
                         c.line, (long long)got, (long long)gotDt, (long long)c.expected);
        }
    }

    // Метки syslog без года: год — по опорной дате (mtime файла), сутки
    // запаса, переход через Новый год, 29 февраля; только с начала строки.
    const struct { const char* line; QDate reference; qint64 expected; } yearCases[] = {
        { "Sep 20 10:00:00 host app: x", QDate(2026, 9, 20), tenLocal },
        { "Sep 20 10:00:00 host app: x", QDate(2027, 3, 1), tenLocal },
        { "Sep 20 10:00:00 host app: x", QDate(2026, 9, 19), tenLocal },   // сутки запаса
        { "Sep 20 10:00:00 host app: x", QDate(2026, 9, 18), local(2025, 9, 20, 10, 0, 0, 0) },
        { "Dec 31 23:59:59 host app: x", QDate(2027, 1, 2), local(2026, 12, 31, 23, 59, 59, 0) },
        { "Jan  1 00:00:01 host app: x", QDate(2027, 1, 2), local(2027, 1, 1, 0, 0, 1, 0) },
        { "Jan  1 00:00:01 host app: x", QDate(2026, 12, 31), local(2027, 1, 1, 0, 0, 1, 0) },
        { "Jan  2 00:00:01 host app: x", QDate(2026, 12, 31), local(2026, 1, 2, 0, 0, 1, 0) },
        { "Feb 29 12:00:00 host app: x", QDate(2028, 3, 10), local(2028, 2, 29, 12, 0, 0, 0) },
        { "Feb 29 12:00:00 host app: x", QDate(2029, 3, 10), local(2028, 2, 29, 12, 0, 0, 0) },
        { "Feb 29 12:00:00 host app: x", QDate(2028, 2, 27), local(2024, 2, 29, 12, 0, 0, 0) },
        { "Feb 29 12:00:00 host app: x", QDate(2028, 2, 28), local(2028, 2, 29, 12, 0, 0, 0) },
        { "Sep  2 10:00:00 host app: x", QDate(2026, 9, 20), local(2026, 9, 2, 10, 0, 0, 0) },
        { "Sep 2 10:00:00.250 host app: x", QDate(2026, 9, 20), local(2026, 9, 2, 10, 0, 0, 250) },
        { "sep 20 12:00:00 +0200 host", QDate(2026, 9, 20), tenUtc },
        { "Sep 20 10:00:00 app: 2025-01-01 00:00:00", QDate(2026, 9, 20), tenLocal },
        // Похоже на метку, но не метка.
        { "Feb 30 12:00:00 host app: x", QDate(2026, 9, 20), kNoTimestamp },
        { "Sep 20 10:00 host app: x", QDate(2026, 9, 20), kNoTimestamp },
        { "Sept 20 10:00:00 host app: x", QDate(2026, 9, 20), kNoTimestamp },
        { "at Sep 20 10:00:00 the job failed", QDate(2026, 9, 20), kNoTimestamp },
        { "\tSep 20 10:00:00 continuation", QDate(2026, 9, 20), kNoTimestamp },
        { "Sep 20 24:00:00 host app: x", QDate(2026, 9, 20), kNoTimestamp },
    };
    for (const auto& c : yearCases) {
        const QString line = QString::fromUtf8(c.line);
        const auto ref = LineClassifier::yearReference(c.reference);
        qint64 ms = kNoTimestamp;
        const qint64 got = classifier.detectTimestampMs(line, ms, ref) ? ms : kNoTimestamp;
        if (got != c.expected) {
            ++g_failures;
            std::fprintf(stderr, "FAIL golden year '%s' ref %s: ms=%lld expected=%lld\n",
                         c.line, qPrintable(c.reference.toString(Qt::ISODate)),
                         (long long)got, (long long)c.expected);
        }
    }

    // Без опорной даты — сегодняшняя: метка сегодняшнего дня — этого года.
    const QDate today = QDate::currentDate();
    const QString todayLine = QLocale::c().toString(today, QStringLiteral("MMM d"))
        + QStringLiteral(" 10:00:00 host app: x");
    qint64 todayMs = kNoTimestamp;
    CHECK(classifier.detectTimestampMs(todayLine, todayMs)
              && todayMs == QDateTime(today, QTime(10, 0)).toMSecsSinceEpoch(),
          "syslog stamp without a reference date takes the current year");
    // Опорная дата по файлу — его mtime.
    CHECK(LineClassifier::yearReferenceForFile(QStringLiteral("/no/such/file")).year
              == today.addDays(1).year(),
          "missing file: reference date is today");
}

// F-13. Метки до 1970 года и на краях диапазона. «Метки нет» — одно
// значение kNoTimestampMs на всё хранилище, и ни один законный момент с ним
// не совпадает: 0 мс (ровно эпоха), −1 мс, отрицательный момент из-за зоны,
// год 0001 и год 9999 — обычные метки. Моменты заданы заранее (QDateTime в
// UTC), а не взяты у второго бэкенда: общую ошибку сравнение не увидит.
static void testPre1970(const QDir& dir)
{
    const LineClassifier classifier;
    const auto utc = [](int y, int mo, int d, int h, int mi, int s, int ms) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms), QTimeZone::UTC)
            .toMSecsSinceEpoch();
    };
    const auto local = [](int y, int mo, int d, int h, int mi, int s, int ms) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms)).toMSecsSinceEpoch();
    };
    const qint64 year1 = utc(1, 1, 1, 0, 0, 0, 0);
    const qint64 year9999 = utc(9999, 12, 31, 23, 59, 59, 999);
    CHECK(year1 == -62135596800000LL && year9999 == 253402300799999LL,
          "pre-1970: reference moments of years 0001 and 9999");

    struct Case { const char* line; qint64 expected; };
    const Case cases[] = {
        { "1970-01-01T00:00:00.000Z INFO epoch", 0 },
        { "1969-12-31T23:59:59.999Z INFO one ms before", -1 },
        { "1970-01-01T00:00:00.001Z INFO one ms after", 1 },
        { "1970-01-01T01:00:00.000+02:00 INFO zone makes it negative", -3600000 },
        { "1970-01-01 00:00:00.000 -0100 INFO west of UTC", 3600000 },
        { "1969-07-20T20:17:40.000Z WARN landing", utc(1969, 7, 20, 20, 17, 40, 0) },
        { "1900-01-01T00:00:00.000Z ERROR far past", utc(1900, 1, 1, 0, 0, 0, 0) },
        { "0001-01-01T00:00:00.000Z INFO year one", year1 },
        { "9999-12-31T23:59:59.999Z INFO year 9999", year9999 },
        { "1965-05-05 05:05:05.005 DEBUG local, no zone", local(1965, 5, 5, 5, 5, 5, 5) },
        { "no timestamp at all", kNoTimestampMs },
    };
    for (const Case& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        qint64 ms = 12345;
        const bool found = classifier.detectTimestampMs(line, ms);
        const qint64 got = found ? ms : kNoTimestampMs;
        QDateTime ts;
        const bool foundDt = classifier.detectTimestamp(line, ts);
        const qint64 gotDt = foundDt ? ts.toMSecsSinceEpoch() : kNoTimestampMs;
        if (got != c.expected || gotDt != c.expected) {
            ++g_failures;
            std::fprintf(stderr, "FAIL pre-1970 classifier '%s': ms=%lld dt=%lld expected=%lld\n",
                         c.line, (long long)got, (long long)gotDt, (long long)c.expected);
        }
        // Найденная метка никогда не равна признаку отсутствия.
        CHECK(!found || ms != kNoTimestampMs, "pre-1970: a found stamp is never kNoTimestampMs");
    }

    // Индекс и резидентный разбор: каждая запись — с заданным моментом;
    // продолжение без метки наследует момент своей записи, а у первой строки
    // без метки момента нет. Запись с меткой и без слова уровня — не
    // «свободный текст» (isPlainText), даже если момент −1 мс.
    QByteArray bytes = "preamble without a stamp\n";
    QVector<qint64> expected = { kNoTimestampMs };
    for (const Case& c : cases) {
        if (c.expected == kNoTimestampMs)
            continue;
        bytes += c.line;
        bytes += "\n    continuation line\n";
        expected << c.expected << c.expected;
    }
    bytes += "1969-12-31T23:59:59.999Z no level, one ms before\n";
    bytes += "    continuation of a record without a level\n";
    expected << -1 << -1;
    const QString path = writeFile(dir, QStringLiteral("pre1970.log"), bytes);
    auto lf = std::make_shared<LogFile>(path);
    const auto entries = parseFile(lf);
    const auto index = indexFile(lf);
    compareParserVsIndex(QStringLiteral("pre1970.log"), path, lf, entries, *index);
    CHECK(index->lineCount() == expected.size(), "pre1970.log: line count");
    for (qint64 i = 0; i < qMin<qint64>(index->lineCount(), expected.size()); ++i) {
        const qint64 got = index->timestampMs(index->logicalId(i));
        if (got != expected[int(i)]) {
            ++g_failures;
            std::fprintf(stderr, "FAIL pre1970.log line %lld: index ms=%lld expected=%lld\n",
                         (long long)i, (long long)got, (long long)expected[int(i)]);
        }
        const LineIndexSnapshot snap = index->snapshot();
        CHECK(snap.timestampMs(snap.logicalId(i)) == got, "pre1970.log: snapshot moment");
    }
    const qint64 last = index->lineCount() - 1;
    CHECK(last > 0 && !index->isPlainTextLine(last) && !index->isPlainTextLine(last - 1),
          "pre1970.log: a record stamped -1 ms is not plain text");
    CHECK(index->isPlainTextLine(0), "pre1970.log: the preamble is plain text");
    // Вне опубликованного — тоже «нет метки», а не 0 и не −1.
    CHECK(index->timestampMs(index->logicalEntryCount()) == kNoTimestampMs,
          "pre1970.log: an unpublished record has no stamp");

    // Дозапись: продолжение, дописанное отдельной порцией за записью с
    // моментом −1 мс, остаётся в своей записи и наследует её момент.
    const QString grow = writeFile(dir, QStringLiteral("pre1970_grow.log"),
                                   "1969-12-31T23:59:59.999Z started\n");
    auto growLf = std::make_shared<LogFile>(grow);
    const auto growIndex = indexFile(growLf);
    appendToFile(grow, "    appended continuation\n"
                       "1970-01-01T00:00:00.000Z INFO epoch\n"
                       "no stamp, continues the epoch record\n");
    {
        LogIndexer indexer;
        indexAppend(indexer, growLf, growIndex, false);
    }
    const auto fresh = indexFile(std::make_shared<LogFile>(grow));
    compareIndexes(QStringLiteral("pre1970_grow.log"), *growIndex, *fresh);
    compareParserVsIndex(QStringLiteral("pre1970_grow.log"), grow, growLf,
                         parseFile(std::make_shared<LogFile>(grow)), *growIndex);
    const qint64 growExpected[] = { -1, -1, 0, 0 };
    CHECK(growIndex->lineCount() == 4, "pre1970_grow.log: four lines");
    for (qint64 i = 0; i < qMin<qint64>(4, growIndex->lineCount()); ++i)
        CHECK(growIndex->timestampMs(growIndex->logicalId(i)) == growExpected[i],
              qPrintable(QString("pre1970_grow.log: line %1 moment").arg(i)));
    CHECK(growIndex->lineCount() == 4 && !growIndex->isPlainTextLine(1),
          "pre1970_grow.log: the appended continuation of a -1 ms record is not plain text");
}

// Схема для замеров: «jsonl» / «logfmt» — ключевая схема с ключами по
// умолчанию (F-4), путь к существующему файлу — его текст, иначе сама строка.
static QString benchSchema(const QString& arg)
{
    if (arg == QLatin1String("jsonl") || arg == QLatin1String("logfmt")) {
        PatternDefinition def;
        KeyedDefinition k;
        k.format = arg == QLatin1String("jsonl") ? KeyedDefinition::Format::JsonObject
                                                 : KeyedDefinition::Format::Logfmt;
        def.keyed = k;
        return LogPattern::serializeDefinition(def);
    }
    QFile f(arg);
    if (!arg.isEmpty() && f.open(QIODevice::ReadOnly))
        return QString::fromUtf8(f.readAll());
    return arg;
}

// Бенчмарк-режим: lineindex_smoke --bench <файл> [схема] — чистая
// скорость LogIndexer без GUI (для локализации узких мест индексации).
// Схема — см. benchSchema; с ней включено извлечение полей.
static int runBench(const QString& path, const QString& patternString)
{
    auto lf = std::make_shared<LogFile>(path);
    auto index = std::make_shared<LineIndex>();
    LogIndexer indexer;
    const FileSchemaPtr schema = passSchema(patternString);
    std::atomic_bool done{false};
    QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                     [&](qint64, const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexingFailed, &indexer,
                     [&](const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::needsResidentFallback, &indexer,
                     [&](const LogFilePtr&, const QString&) { done = true; },
                     Qt::DirectConnection);
    QElapsedTimer t;
    t.start();
    indexer.startIndexing(lf, index, FileEncoding(), schema);
    // Шаг опроса 1 мс: прогон — доли секунды, шаг 20 мс давал шум ~4 % (S-17).
    while (!done)
        QThread::msleep(1);
    std::fprintf(stderr, "bench: %lld lines, %lld logical, %.3f s\n",
                 (long long)index->lineCount(),
                 (long long)index->logicalEntryCount(), t.elapsed() / 1000.0);
    return 0;
}

// Бенчмарк резидентного пути: lineindex_smoke --bench-resident <файл>
// [схема] — чистая скорость LogParser (разбор в записи, без модели): время
// до parsingFinished. Замер цены транспорта байтов F-1 (шаг 3) и ключевой
// схемы F-4 (схема — см. benchSchema; с ней включено извлечение полей).
static int runBenchResident(const QString& path, const QString& patternString)
{
    auto lf = std::make_shared<LogFile>(path);
    LogParser parser;
    const FileSchemaPtr schema = passSchema(patternString);
    std::atomic_bool done{false};
    std::atomic<qint64> entries{0};
    QObject::connect(&parser, &LogParser::entriesParsed, &parser,
                     [&](const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr&) {
                         entries += batch.size();
                     },
                     Qt::DirectConnection);
    QObject::connect(&parser, &LogParser::parsingFinished, &parser,
                     [&](int, const LogFilePtr&) { done = true; }, Qt::DirectConnection);
    QObject::connect(&parser, &LogParser::parsingFailed, &parser,
                     [&](const LogFilePtr&) { done = true; }, Qt::DirectConnection);
    QElapsedTimer t;
    t.start();
    parser.startParsing(lf, FileEncoding(), schema);
    while (!done)
        QThread::msleep(5);
    std::fprintf(stderr, "bench-resident: %lld lines, %.3f s\n", (long long)entries.load(),
                 t.elapsed() / 1000.0);
    return 0;
}

// Ступенчатая атрибуция стоимости индексации: 0 — чтение+разрез по \n,
// 1 — +декодирование UTF-8, 2 — +detectTimestamp (с toMSecsSinceEpoch),
// 3 — +detectLogLevel. Дельты между ступенями = цена каждого шага.
// encodingName — кодировка ступени 6 (имя TextEncodings::fromName); пусто —
// решает EncodingDetector по первой не-ASCII строке (запасная — Windows-1251),
// у файла из одних ASCII — UTF-8.
static int runBenchStages(const QString& path, const QString& encodingName = QString())
{
    // Стадии 0–3 накопительные: I/O → декод → timestamp → level. Стадия 4 —
    // декод + level без timestamp: цена классификации уровня отдельно, без
    // шума самой дорогой стадии. Стадия 5 — декод + detectTimestampMs с
    // опорной датой файла и флагом ведущей метки: горячий путь индексатора, без QDateTime (у
    // стадии 2 её цену определяет построение QDateTime на строку).
    // Стадия 6 — декодирование выбранной кодировкой (TextDecoder, F-1): то же,
    // что стадия 1, но декодером файла; для однобайтной — fromLatin1 и
    // таблица. Разница 6 − 1 — цена (или выигрыш) своего декодера против
    // fromUtf8 на том же файле.
    const LineClassifier::YearReference yearRef = LineClassifier::yearReferenceForFile(path);
    TextEncoding encoding = TextEncoding::Utf8;
    if (!encodingName.isEmpty()) {
        const auto named = TextEncodings::fromName(encodingName);
        if (!named) {
            std::fprintf(stderr, "unknown encoding %s\n", qPrintable(encodingName));
            return 2;
        }
        encoding = *named;
    } else {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray all = f.readAll();
            const qsizetype high = EncodingDetector::firstHighByte(all);
            if (high >= 0) {
                const qsizetype lineStart = all.lastIndexOf('\n', high) + 1;
                encoding = EncodingDetector::decide(
                    QByteArrayView(all).mid(lineStart, EncodingDetector::kWindowBytes),
                    TextEncoding::Windows1251).value_or(TextEncoding::Utf8);
            }
        }
    }
    const TextDecoder decoder(encoding);
    std::fprintf(stderr, "stage 6 decoder: %s%s\n", qPrintable(TextEncodings::displayName(encoding)),
                 encodingName.isEmpty() ? " (detected)" : "");
    for (int stage = 0; stage <= 6; ++stage) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot open %s\n", qPrintable(path));
            return 2;
        }
        QByteArray buf;
        buf.resize(8 * 1024 * 1024);
        QByteArray carry;
        QString text;
        LineClassifier classifier;
        qint64 lines = 0, tsCount = 0, lvlCount = 0;
        qint64 sink = 0; // от выкидывания работы оптимизатором
        QElapsedTimer t;
        t.start();
        while (true) {
            const qint64 got = f.read(buf.data(), buf.size());
            if (got <= 0)
                break;
            const char* p = buf.constData();
            qint64 pos = 0;
            while (pos < got) {
                const char* nl = static_cast<const char*>(
                    memchr(p + pos, '\n', size_t(got - pos)));
                if (!nl) {
                    carry.append(p + pos, int(got - pos));
                    break;
                }
                const qint64 nlPos = nl - p;
                const char* data;
                qint64 len;
                if (carry.isEmpty()) {
                    data = p + pos;
                    len = nlPos - pos;
                } else {
                    carry.append(p + pos, int(nlPos - pos));
                    data = carry.constData();
                    len = carry.size();
                }
                if (len > 0 && data[len - 1] == '\r')
                    --len;
                ++lines;
                if (stage == 6) {
                    text = decoder.decode(data, len);
                    sink += text.size();
                } else if (stage >= 1) {
                    text = QString::fromUtf8(data, int(len));
                    sink += text.size();
                    if (stage == 5) {
                        // С флагом ведущей метки — как у LogIndexer (F-12).
                        qint64 ms = 0;
                        bool leading = false;
                        if (classifier.detectTimestampMs(text, ms, yearRef, &leading)) {
                            ++tsCount;
                            sink += (ms & 1) + leading;
                        }
                    }
                    if (stage == 2 || stage == 3) {
                        QDateTime ts;
                        if (classifier.detectTimestamp(text, ts, yearRef)) {
                            ++tsCount;
                            sink += ts.toMSecsSinceEpoch() & 1;
                        }
                    }
                    if (stage == 3 || stage == 4) {
                        LogLevel lvl = LogLevel::Unknown;
                        if (classifier.detectLogLevel(text, lvl))
                            ++lvlCount;
                    }
                }
                if (!carry.isEmpty())
                    carry.clear();
                pos = nlPos + 1;
            }
        }
        std::fprintf(stderr,
                     "stage %d: %7.3f s  (lines=%lld ts=%lld lvl=%lld sink=%lld)\n",
                     stage, t.elapsed() / 1000.0, (long long)lines,
                     (long long)tsCount, (long long)lvlCount, (long long)sink);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// SequentialLineReader: верный текст при любом порядке доступа И ограниченная
// цена скана — число перечитываний окна с диска. Дефект, который это ловит:
// окно ставилось ВПЕРЁД от запрошенного смещения, поэтому скан назад
// (findPreviousOccurrence) промахивался мимо окна на каждой строке и делал
// seek + чтение целого окна на строку.
// ---------------------------------------------------------------------------
static void testSequentialReaderDirections(const QDir& dir)
{
    // ~3,5 МБ строк переменной длины, LF и CRLF вперемешку, несколько строк
    // длиннее окна.
    const qint64 window = 64 * 1024;
    QByteArray bytes;
    QRandomGenerator rng(7);
    int longLines = 0;
    for (int i = 0; i < 40000; ++i) {
        int len = 20 + int(rng.bounded(120));
        if (i > 0 && i % 9973 == 0) {
            len = 100000; // длиннее окна
            ++longLines;
        }
        QByteArray line = QByteArray::number(i) + ':';
        while (line.size() < len)
            line += char('a' + (line.size() % 26));
        bytes += line;
        bytes += (i % 2) ? "\r\n" : "\n";
    }
    const QString path = writeFile(dir, "reader_directions.log", bytes);
    const auto lf = std::make_shared<LogFile>(path);
    const auto index = indexFile(lf);
    const qint64 n = index->lineCount();
    CHECK(n == 40000, "reader: fixture line count");
    if (n != 40000)
        return;

    const auto expected = [&](qint64 l) {
        return QString::fromUtf8(bytes.constData() + index->lineStartOffset(l),
                                 int(index->lineByteLength(l)));
    };

    // Честный скан сдвигает окно не реже чем на половину окна; каждая строка
    // длиннее окна стоит пару лишних перечитываний. Сломанный скан назад —
    // одно перечитывание на строку, т.е. ~40000.
    const qint64 slideBound = qint64(bytes.size()) / (window / 2) + 2 * longLines + 4;

    struct Order {
        const char* name;
        std::vector<qint64> lines;
        bool bounded; // проверять ли цену
    };
    std::vector<Order> orders;

    Order fwd{ "forward", {}, true };
    for (qint64 l = 0; l < n; ++l) fwd.lines.push_back(l);

    Order bwd{ "backward", {}, true };
    for (qint64 l = n - 1; l >= 0; --l) bwd.lines.push_back(l);

    // «Дрожание»: соседние строки попарно переставлены — так идёт доступ по
    // времени в мульти-файловой вкладке, где строки файла почти по порядку.
    Order jitFwd{ "jitter forward", {}, true };
    for (qint64 l = 0; l + 1 < n; l += 2) { jitFwd.lines.push_back(l + 1); jitFwd.lines.push_back(l); }
    Order jitBwd{ "jitter backward", {}, true };
    jitBwd.lines.assign(jitFwd.lines.rbegin(), jitFwd.lines.rend());

    // Поиск назад с заворотом: от середины к началу, затем от конца к середине.
    Order wrap{ "backward with wrap", {}, true };
    for (qint64 l = n / 2 - 1; l >= 0; --l) wrap.lines.push_back(l);
    for (qint64 l = n - 1; l >= n / 2; --l) wrap.lines.push_back(l);

    Order rnd{ "random", {}, false };
    for (int k = 0; k < 3000; ++k) rnd.lines.push_back(qint64(rng.bounded(quint32(n))));

    orders.push_back(std::move(fwd));
    orders.push_back(std::move(bwd));
    orders.push_back(std::move(jitFwd));
    orders.push_back(std::move(jitBwd));
    orders.push_back(std::move(wrap));
    orders.push_back(std::move(rnd));

    QByteArray summary;
    for (const Order& order : orders) {
        SequentialLineReader reader(path, TextDecoder(), window);
        CHECK(reader.open(), "reader: open");
        int mismatches = 0;
        for (const qint64 l : order.lines) {
            if (reader.lineAt(index->lineStartOffset(l), index->lineByteLength(l)) != expected(l))
                ++mismatches;
        }
        if (mismatches > 0)
            std::fprintf(stderr, "reader %s: %d wrong line(s)\n", order.name, mismatches);
        CHECK(mismatches == 0, "reader: line text matches for every access order");

        if (order.bounded && reader.slideCount() > slideBound)
            std::fprintf(stderr, "reader %s: %d window reads, bound %lld\n", order.name,
                         reader.slideCount(), static_cast<long long>(slideBound));
        CHECK(!order.bounded || reader.slideCount() <= slideBound,
              "reader: window reads stay bounded for monotonic and jittery scans");
        summary += QByteArray(summary.isEmpty() ? "" : ", ") + order.name + '='
                   + QByteArray::number(reader.slideCount());
    }
    std::fprintf(stderr, "reader window reads over %lld lines (bound %lld): %s\n",
                 static_cast<long long>(n), static_cast<long long>(slideBound),
                 summary.constData());
}


// ---------------------------------------------------------------------------
// Кодировки (F-1, шаг 2): определение в индексаторе
// ---------------------------------------------------------------------------

// Текст → байты однобайтной кодировки по обратной таблице декодера.
static QByteArray encodeAs(const QString& text, TextEncoding encoding)
{
    if (!TextEncodings::isSingleByte(encoding))
        return text.toUtf8();
    const TextDecoder decoder(encoding);
    QByteArray out;
    for (const QChar c : text) {
        if (c.unicode() < 0x80) {
            out += char(c.unicode());
            continue;
        }
        char found = '?';
        for (int b = 0x80; b <= 0xFF; ++b) {
            const char byte = char(b);
            if (decoder.decode(&byte, 1) == QString(c)) {
                found = byte;
                break;
            }
        }
        out += found;
    }
    return out;
}

// Прогон индексатора с записью событий: решения о кодировке и порции, в том
// порядке, в каком воркер их выпустил (DirectConnection — в его потоке).
struct EncodingRun {
    std::shared_ptr<LineIndex> index;
    QVector<FileEncoding> decisions;
    // Строк опубликовано к моменту первого решения (-1 — решения не было).
    qint64 publishedAtFirstDecision = -1;
};

static EncodingRun indexWithEncoding(const LogFilePtr& lf, FileEncoding start,
                                     std::shared_ptr<LineIndex> index = nullptr,
                                     bool append = false,
                                     TextEncoding fallback = TextEncoding::Windows1251)
{
    EncodingRun run;
    run.index = index ? index : std::make_shared<LineIndex>();
    std::atomic_bool done{false};
    qint64 published = append ? run.index->lineCount() : 0;
    if (append && run.index->lastLineProvisional())
        published -= 1;
    LogIndexer indexer;
    indexer.setFallbackEncoding(fallback);
    QObject::connect(&indexer, &LogIndexer::encodingDecided, &indexer,
                     [&](const LogFilePtr&, const FileEncoding& enc) {
                         if (run.decisions.isEmpty())
                             run.publishedAtFirstDecision = published;
                         run.decisions.append(enc);
                     }, Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexBatchReady, &indexer,
                     [&](const LogFilePtr&, qint64 first, qint64 count) {
                         published = qMax(published, first + count);
                     }, Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                     [&](qint64, const LogFilePtr&) { done = true; }, Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexingFailed, &indexer,
                     [&](const LogFilePtr&) { done = true; }, Qt::DirectConnection);
    if (append)
        indexer.startIndexingFrom(lf, run.index, run.index->lastLineProvisional(), start);
    else
        indexer.startIndexing(lf, run.index, start);
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 60000)
        QThread::msleep(2);
    CHECK(done.load(), "encoding: indexing did not finish in time");
    return run;
}

static FileEncoding forcedUtf8()
{
    return FileEncoding{TextDecoder(TextEncoding::Utf8), FileEncoding::Source::User};
}

// Текст строк файла: эталонные строки (split по '\n') против чтения по
// индексу ридером воркера и кэшем отображения с декодером кодировки.
static void checkIndexedText(const QString& tag, const QString& path, const LineIndex& index,
                             const QStringList& expected, TextEncoding encoding)
{
    CHECK(index.lineCount() == expected.size(),
          qPrintable(QString("%1: line count %2, expected %3")
                         .arg(tag).arg(index.lineCount()).arg(expected.size())));
    SequentialLineReader reader(path, TextDecoder(encoding));
    CHECK(reader.open(), "encoding: reader open");
    TextChunkCache cache;
    const int cacheId = cache.addFile(path, TextDecoder(encoding));
    int wrong = 0;
    for (qint64 i = 0; i < qMin<qint64>(index.lineCount(), expected.size()); ++i) {
        const QString viaReader = reader.lineAt(index.lineStartOffset(i), index.lineByteLength(i));
        const QString viaCache = cache.lineText(cacheId, index.lineStartOffset(i),
                                                index.lineByteLength(i));
        if (viaReader != expected[int(i)] || viaCache != expected[int(i)]) {
            if (++wrong <= 3)
                std::fprintf(stderr, "%s line %lld:\n  want   '%s'\n  reader '%s'\n  cache  '%s'\n",
                             qPrintable(tag), (long long)i, qPrintable(expected[int(i)]),
                             qPrintable(viaReader), qPrintable(viaCache));
        }
    }
    CHECK(wrong == 0, qPrintable(tag + ": decoded text of every line"));
}

static QStringList linesOf(const QString& text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

static QString cyrLine(int second, const char* level, const char16_t* message)
{
    return QStringLiteral("2026-07-10 12:%1:%2,000 %3 [db] %4\n")
        .arg((second / 60) % 60, 2, 10, QChar('0'))
        .arg(second % 60, 2, 10, QChar('0'))
        .arg(QLatin1String(level))
        .arg(QString::fromUtf16(message));
}

static void testEncodingDetection(const QDir& dir)
{
    // ASCII-преамбула длиннее буфера чтения индексатора (8 МБ): первая
    // не-ASCII строка — в середине файла, во втором буфере.
    QString prefix;
    int second = 0;
    while (prefix.size() < 9 * 1024 * 1024) {
        prefix += QStringLiteral("2026-07-10 12:%1:%2,000 INFO [app] ascii filler line %3\n")
                      .arg((second / 60) % 60, 2, 10, QChar('0'))
                      .arg(second % 60, 2, 10, QChar('0'))
                      .arg(second);
        ++second;
    }
    const qint64 firstHighLine = linesOf(prefix).size();
    QString body;
    body += cyrLine(1, "ERROR", u"Ошибка соединения с базой данных: таймаут");
    body += QStringLiteral("    в модуле Хранилище.Подключение\n");
    body += cyrLine(2, "INFO", u"Пользователь Иванов вошёл в систему");
    body += cyrLine(3, "WARN", u"ПРЕДУПРЕЖДЕНИЕ: диск заполнен (срочно)");
    body += QStringLiteral("2026-07-10 12:00:04,000 INFO [app] ascii after\n");
    const QString text = prefix + body;
    const QStringList expected = linesOf(text);

    for (const TextEncoding enc : {TextEncoding::Windows1251, TextEncoding::Ibm866,
                                   TextEncoding::Koi8R, TextEncoding::Utf8}) {
        const QString tag = QStringLiteral("encoding mid-file %1").arg(TextEncodings::displayName(enc));
        const QString path = writeFile(dir, QStringLiteral("enc_mid_%1.log").arg(int(enc)),
                                       encodeAs(text, enc));
        auto lf = std::make_shared<LogFile>(path);
        const EncodingRun run = indexWithEncoding(lf, FileEncoding());
        CHECK(run.decisions.size() == 1,
              qPrintable(tag + QStringLiteral(": one decision, got %1").arg(run.decisions.size())));
        if (!run.decisions.isEmpty()) {
            CHECK(run.decisions.first().decoder.encoding() == enc,
                  qPrintable(tag + ": detected " + TextEncodings::displayName(
                                 run.decisions.first().decoder.encoding())));
            CHECK(run.decisions.first().source == FileEncoding::Source::Auto,
                  qPrintable(tag + ": source is auto"));
        }
        CHECK(run.publishedAtFirstDecision >= 0 && run.publishedAtFirstDecision <= firstHighLine,
              qPrintable(tag + QStringLiteral(": decided before the batch with the first non-ASCII "
                                              "line (published %1, line %2)")
                                   .arg(run.publishedAtFirstDecision).arg(firstHighLine)));
        checkIndexedText(tag, path, *run.index, expected, enc);

        // Индекс от кодировки не зависит: тот же файл, прочитанный как UTF-8,
        // даёт те же смещения, длины, уровни, записи и метки.
        const EncodingRun asUtf8 = indexWithEncoding(lf, forcedUtf8());
        // Под выбором пользователя автоматика решает рядом (F-1, шаг 4):
        // декодер остаётся выбранным, решение автоматики — то же, что без
        // выбора.
        bool keepsChoice = true;
        for (const FileEncoding& d : asUtf8.decisions)
            keepsChoice = keepsChoice && d.source == FileEncoding::Source::User
                && d.decoder.isUtf8();
        CHECK(keepsChoice, qPrintable(tag + ": a user choice is not re-decided"));
        CHECK(!asUtf8.decisions.isEmpty()
                  && asUtf8.decisions.last().automatic().decoder.encoding() == enc
                  && asUtf8.decisions.last().automatic().source == FileEncoding::Source::Auto,
              qPrintable(tag + ": detection under a user choice decides alongside"));
        compareIndexes(tag + " vs read as UTF-8", *run.index, *asUtf8.index);
    }

    // Только ASCII — решать нечего, решения нет.
    {
        QByteArray ascii;
        for (int i = 0; i < 100; ++i)
            ascii += "2026-07-10 12:00:" + QByteArray::number(i % 60).rightJustified(2, '0')
                     + ",000 INFO [app] plain ascii " + QByteArray::number(i) + '\n';
        const QString path = writeFile(dir, "enc_ascii.log", ascii);
        const EncodingRun run = indexWithEncoding(std::make_shared<LogFile>(path), FileEncoding());
        CHECK(run.decisions.isEmpty(), "encoding: ASCII-only file stays undecided");
    }

    // BOM UTF-8 — решение «BOM», даже если дальше только ASCII.
    {
        const QString path = writeFile(dir, "enc_bom.log",
                                       QByteArray("\xEF\xBB\xBF") + sampleLog(false, true));
        const EncodingRun run = indexWithEncoding(std::make_shared<LogFile>(path), FileEncoding());
        CHECK(run.decisions.size() == 1
                  && run.decisions.first().source == FileEncoding::Source::Bom
                  && run.decisions.first().decoder.isUtf8(),
              "encoding: UTF-8 BOM decides UTF-8 by BOM");
    }
}

static void testEncodingInTail(const QDir& dir)
{
    // Файл начинается ASCII — «не решено»; кириллица приходит дозаписью.
    // Состояние «не решено» переживает первую дозапись (тоже ASCII).
    for (const TextEncoding enc : {TextEncoding::Windows1251, TextEncoding::Koi8R}) {
        const QString tag = QStringLiteral("encoding tail %1").arg(TextEncodings::displayName(enc));
        QString text = QStringLiteral("2026-07-10 12:00:00,000 INFO [app] start\n");
        const QString path = writeFile(dir, QStringLiteral("enc_tail_%1.log").arg(int(enc)),
                                       encodeAs(text, enc));
        auto lf = std::make_shared<LogFile>(path);
        EncodingRun run = indexWithEncoding(lf, FileEncoding());
        CHECK(run.decisions.isEmpty(), qPrintable(tag + ": ASCII head undecided"));

        const QString ascii = QStringLiteral("2026-07-10 12:00:01,000 INFO [app] still ascii\n");
        text += ascii;
        appendToFile(path, encodeAs(ascii, enc));
        const EncodingRun second = indexWithEncoding(lf, FileEncoding(), run.index, /*append=*/true);
        CHECK(second.decisions.isEmpty(), qPrintable(tag + ": ASCII tail keeps it undecided"));

        const QString cyr = cyrLine(2, "ERROR", u"Ошибка записи в журнал: нет места")
                          + QStringLiteral("    повтор через 5 секунд\n");
        text += cyr;
        appendToFile(path, encodeAs(cyr, enc));
        const qint64 before = run.index->lineCount();
        const EncodingRun third = indexWithEncoding(lf, FileEncoding(), run.index, true);
        CHECK(third.decisions.size() == 1 && third.decisions.first().decoder.encoding() == enc,
              qPrintable(tag + ": the appended non-ASCII line decides"));
        CHECK(third.publishedAtFirstDecision >= 0 && third.publishedAtFirstDecision <= before,
              qPrintable(tag + ": decided before the tail batch is published"));
        checkIndexedText(tag, path, *run.index, linesOf(text), enc);

        // Решённая кодировка продолжает дозапись без нового решения.
        const QString more = cyrLine(3, "INFO", u"Журнал очищен");
        text += more;
        appendToFile(path, encodeAs(more, enc));
        // Решение по двум строкам предварительное (шаг 3а): дозапись копит
        // свидетельство дальше и может сообщить его, но кодировка та же.
        CHECK(third.decisions.value(0).provisional,
              qPrintable(tag + ": a decision on two lines is provisional"));
        const EncodingRun fourth = indexWithEncoding(lf, third.decisions.value(0), run.index, true);
        bool sameDecoder = true;
        for (const FileEncoding& d : fourth.decisions)
            sameDecoder = sameDecoder && d.decoder.encoding() == enc
                          && d.source == FileEncoding::Source::Auto;
        CHECK(sameDecoder, qPrintable(tag + ": a decided file keeps its encoding"));
        // Окончательное решение не трогается вовсе.
        FileEncoding final = fourth.decisions.isEmpty() ? third.decisions.value(0)
                                                        : fourth.decisions.last();
        final.provisional = false;
        appendToFile(path, encodeAs(more, enc));
        text += more;
        const EncodingRun fifth = indexWithEncoding(lf, final, run.index, true);
        CHECK(fifth.decisions.isEmpty(), qPrintable(tag + ": a settled file is not re-decided"));
        checkIndexedText(tag + " after decision", path, *run.index, linesOf(text), enc);
    }
}

static void testEncodingCutWindow(const QDir& dir)
{
    // Порция оборвана внутри окна определения: файл кончается посреди
    // единственной многобайтной последовательности (растущий лог, строка без
    // '\n'). Решать нечего — «не решено», строка предварительная; дозапись
    // перечитывает её с начала и решает по полному окну.
    {
        const QByteArray full = QString(QStringLiteral("2026-07-10 12:00:00,000 INFO [app] head\n")
                                        + cyrLine(1, "ERROR", u"Ёжик")).toUtf8();
        const qsizetype cut = full.indexOf("\xD0\x81") + 1; // внутри «Ё»
        const QString path = writeFile(dir, "enc_cut_utf8.log", full.left(cut));
        auto lf = std::make_shared<LogFile>(path);
        EncodingRun run = indexWithEncoding(lf, FileEncoding());
        CHECK(run.decisions.isEmpty(), "encoding cut utf8: truncated sequence decides nothing");
        CHECK(run.index->lastLineProvisional(), "encoding cut utf8: tail provisional");
        appendToFile(path, full.mid(cut));
        const EncodingRun next = indexWithEncoding(lf, FileEncoding(), run.index, true);
        CHECK(next.decisions.size() == 1 && next.decisions.first().decoder.isUtf8(),
              "encoding cut utf8: completed line decides UTF-8");
        checkIndexedText("encoding cut utf8", path, *run.index,
                         linesOf(QString::fromUtf8(full)), TextEncoding::Utf8);
    }
    {
        // cp1251: первый байт ≥ 0x80 похож на ведущий байт UTF-8 и оборван —
        // тоже nullopt; дозапись решает Windows-1251.
        const QString text = QStringLiteral("2026-07-10 12:00:00,000 INFO [app] head\n")
                           + cyrLine(1, "ERROR", u"Ошибка чтения файла настроек");
        const QByteArray full = encodeAs(text, TextEncoding::Windows1251);
        const qsizetype cut = full.indexOf(char(0xCE)) + 1; // «О»
        const QString path = writeFile(dir, "enc_cut_1251.log", full.left(cut));
        auto lf = std::make_shared<LogFile>(path);
        EncodingRun run = indexWithEncoding(lf, FileEncoding());
        CHECK(run.decisions.isEmpty(), "encoding cut cp1251: lone lead byte decides nothing");
        appendToFile(path, full.mid(cut));
        const EncodingRun next = indexWithEncoding(lf, FileEncoding(), run.index, true);
        CHECK(next.decisions.size() == 1
                  && next.decisions.first().decoder.encoding() == TextEncoding::Windows1251,
              "encoding cut cp1251: completed line decides Windows-1251");
        checkIndexedText("encoding cut cp1251", path, *run.index, linesOf(text),
                         TextEncoding::Windows1251);
    }
    {
        // Окно у границы буфера: первый не-ASCII байт — в последних байтах
        // 8-МБ буфера, окно дочитывается вторым дескриптором.
        QByteArray bytes;
        int second = 0;
        const QByteArray filler = " INFO [app] filler to the buffer edge\n";
        const qint64 buffer = 8 * 1024 * 1024;
        while (bytes.size() < buffer - 200)
            bytes += "2026-07-10 12:00:" + QByteArray::number(second++ % 60).rightJustified(2, '0')
                     + ",000" + filler;
        const QByteArray pad(int(buffer - 40 - bytes.size()), 'x');
        bytes += pad + '\n';
        const QString cyr = cyrLine(5, "ERROR", u"Сбой: очередь сообщений переполнена, ожидание");
        QString tail;
        for (int i = 0; i < 50; ++i)
            tail += cyr;
        bytes += encodeAs(tail, TextEncoding::Ibm866);
        const QString path = writeFile(dir, "enc_edge_866.log", bytes);
        const EncodingRun run = indexWithEncoding(std::make_shared<LogFile>(path), FileEncoding());
        CHECK(run.decisions.size() == 1
                  && run.decisions.first().decoder.encoding() == TextEncoding::Ibm866,
              "encoding edge: window past the buffer decides IBM866");
    }
}


// ---------------------------------------------------------------------------
// Значения ключей ключевой схемы (F-4, docs/design/F-4.md, PR 1): эпоха всех
// единиц и их границы по величине, отрицательная (до 1970, вниз до мс),
// строка из цифр; строка ISO в значении = та же метка в строке
// (detectTimestampMs); числовые уровни всех шкал. Ожидаемое — заданные
// значения, а не второй разборщик.
// ---------------------------------------------------------------------------
static void testKeyedValues()
{
    using Unit = KeyedDefinition::EpochUnit;
    using Scale = KeyedDefinition::NumericLevels;
    const LineClassifier classifier;
    constexpr qint64 kNone = std::numeric_limits<qint64>::min();

    const struct { const char* value; Unit unit; qint64 expected; } epochs[] = {
        // Auto: по величине целой части.
        { "1700000000", Unit::Auto, 1700000000000LL },              // секунды
        { "1700000000.123456", Unit::Auto, 1700000000123LL },       // дробь — мс, усечение
        { "1700000000.9", Unit::Auto, 1700000000900LL },
        { "99999999999", Unit::Auto, 99999999999000LL },            // < 1e11 — ещё секунды
        { "100000000000", Unit::Auto, 100000000000LL },             // = 1e11 — уже мс
        { "1700000000123", Unit::Auto, 1700000000123LL },           // мс
        { "1700000000123.9", Unit::Auto, 1700000000123LL },
        { "99999999999999", Unit::Auto, 99999999999999LL },         // < 1e14 — мс
        { "100000000000000", Unit::Auto, 100000000000LL },          // = 1e14 — мкс
        { "1700000000123456", Unit::Auto, 1700000000123LL },        // мкс
        { "99999999999999999", Unit::Auto, 99999999999999LL },      // < 1e17 — мкс
        { "100000000000000000", Unit::Auto, 100000000000LL },       // = 1e17 — нс
        { "1700000000123456789", Unit::Auto, 1700000000123LL },     // нс, 19 цифр (zerolog, zap)
        { "9999999999999999999", Unit::Auto, 9999999999999LL },     // наибольшее в 19 цифрах — 2286 год
        { "17000000001234567890", Unit::Auto, kNone },              // 20 цифр — не читается
        { "170000000012345678", Unit::Auto, 170000000012LL },       // нс, 18 цифр
        { "0", Unit::Auto, 0 },                                     // ровно эпоха
        { "0.0005", Unit::Auto, 0 },
        // Отрицательная — до 1970, момент вниз до миллисекунды.
        { "-1", Unit::Auto, -1000 },
        { "-1.5", Unit::Auto, -1500 },
        { "-0.0005", Unit::Auto, -1 },
        { "-86400", Unit::Auto, -86400000 },
        { "-1700000000123", Unit::Auto, -1700000000123LL },
        { "-1500", Unit::Micros, -2 },
        { "-1000", Unit::Micros, -1 },
        // Явная единица главнее величины.
        { "1700000000", Unit::Millis, 1700000000 },
        { "1700000000123", Unit::Seconds, kNone },                  // за 9999 годом
        { "1700000000123456", Unit::Micros, 1700000000123LL },
        { "1700000000123456789", Unit::Nanos, 1700000000123LL },
        { "-1700000000123456789", Unit::Nanos, -1700000000124LL },  // вниз до мс
        { "1700000000", Unit::Nanos, 1700 },
        // Границы годов 0001–9999.
        { "-62135596800", Unit::Seconds, -62135596800000LL },
        { "-62135596801", Unit::Seconds, kNone },
        { "253402300799", Unit::Seconds, 253402300799000LL },
        { "253402300800", Unit::Seconds, kNone },
        // Не эпоха.
        { "", Unit::Auto, kNone },
        { "-", Unit::Auto, kNone },
        { "12.", Unit::Auto, kNone },
        { ".5", Unit::Auto, kNone },
        { "1.7e9", Unit::Auto, kNone },
        { "17 00", Unit::Auto, kNone },
        { "+5", Unit::Auto, kNone },
    };
    for (const auto& c : epochs) {
        qint64 ms = kNone;
        const bool ok = LineClassifier::epochFromValue(QString::fromLatin1(c.value), c.unit, ms);
        CHECK((ok ? ms : kNone) == c.expected,
              qPrintable(QStringLiteral("keyed epoch %1 (%2) -> %3, expected %4")
                  .arg(QLatin1String(c.value), KeyedDefinition::epochUnitName(c.unit))
                  .arg(ok ? ms : kNone).arg(c.expected)));
    }

    // timestampFromValue: число и строка из цифр — эпоха; строка с датой —
    // тот же сканер, что у строки: ISO в значении = ISO в строке.
    {
        qint64 ms = 0;
        CHECK(classifier.timestampFromValue(u"1700000000123", true, Unit::Auto, {}, ms)
                  && ms == 1700000000123LL, "keyed: number value is epoch");
        CHECK(classifier.timestampFromValue(u"1700000000", false, Unit::Auto, {}, ms)
                  && ms == 1700000000000LL, "keyed: digit string value is epoch");
        CHECK(!classifier.timestampFromValue(u"1.7e9", true, Unit::Auto, {}, ms),
              "keyed: exponent number is not a timestamp");
        CHECK(!classifier.timestampFromValue(u"later", false, Unit::Auto, {}, ms),
              "keyed: word value is not a timestamp");
        CHECK(!classifier.timestampFromValue(u"13:55:40.016", false, Unit::Auto, {}, ms),
              "keyed: time without date has no file context in a value (F-11)");
        const char* isoValues[] = {
            "2026-11-18T10:00:00Z", "2026-11-18T10:00:00.123+03:00", "2026-11-18 10:00:00,456",
            "2026/11/18 10:00:00", "18/Nov/2026:13:55:36 +0000", "1969-12-31T23:59:59.999Z",
            "0001-01-01T00:00:00Z", "Nov 18 13:55:36",
        };
        const auto ref = LineClassifier::yearReference(QDate(2026, 11, 20));
        for (const char* v : isoValues) {
            const QString value = QString::fromLatin1(v);
            qint64 fromValue = kNone, fromLine = kNone;
            const bool a = classifier.timestampFromValue(value, false, Unit::Auto, ref, fromValue);
            const bool b = classifier.detectTimestampMs(value + QStringLiteral(" INFO x"), fromLine, ref);
            CHECK(a && b && fromValue == fromLine,
                  qPrintable(QStringLiteral("keyed: value '%1' -> %2, same text in a line -> %3")
                      .arg(value).arg(fromValue).arg(fromLine)));
        }
    }

    // Числовые уровни всех шкал и слова.
    const struct { const char* value; bool isNumber; Scale scale; LogLevel expected; } levels[] = {
        { "0", true, Scale::Auto, LogLevel::Fatal },  { "1", true, Scale::Auto, LogLevel::Fatal },
        { "2", true, Scale::Auto, LogLevel::Fatal },  { "3", true, Scale::Auto, LogLevel::Error },
        { "4", true, Scale::Auto, LogLevel::Warn },   { "5", true, Scale::Auto, LogLevel::Info },
        { "6", true, Scale::Auto, LogLevel::Info },   { "7", true, Scale::Auto, LogLevel::Debug },
        { "8", true, Scale::Auto, LogLevel::Unknown }, { "9", true, Scale::Auto, LogLevel::Unknown },
        { "10", true, Scale::Auto, LogLevel::Trace }, { "20", true, Scale::Auto, LogLevel::Debug },
        { "30", true, Scale::Auto, LogLevel::Info },  { "35", true, Scale::Auto, LogLevel::Info },
        { "40", true, Scale::Auto, LogLevel::Warn },  { "50", true, Scale::Auto, LogLevel::Error },
        { "60", true, Scale::Auto, LogLevel::Fatal }, { "70", true, Scale::Auto, LogLevel::Fatal },
        { "30", false, Scale::Auto, LogLevel::Info }, // строка из цифр — тоже шкала
        { "7", true, Scale::Syslog, LogLevel::Debug }, { "3", true, Scale::Syslog, LogLevel::Error },
        { "30", true, Scale::Syslog, LogLevel::Unknown },
        { "5", true, Scale::Python, LogLevel::Unknown }, { "10", true, Scale::Python, LogLevel::Debug },
        { "20", true, Scale::Python, LogLevel::Info }, { "30", true, Scale::Python, LogLevel::Warn },
        { "40", true, Scale::Python, LogLevel::Error }, { "50", true, Scale::Python, LogLevel::Fatal },
        { "-1", true, Scale::Auto, LogLevel::Unknown }, { "3.5", true, Scale::Auto, LogLevel::Unknown },
        { "info", false, Scale::Auto, LogLevel::Info }, { "WARNING", false, Scale::Auto, LogLevel::Warn },
        { "err", false, Scale::Auto, LogLevel::Error }, { "Information", false, Scale::Auto, LogLevel::Unknown },
        { "true", false, Scale::Auto, LogLevel::Unknown },
    };
    for (const auto& c : levels) {
        LogLevel level = LogLevel::Unknown;
        const bool ok = classifier.levelFromValue(QString::fromLatin1(c.value), c.isNumber,
                                                  c.scale, level);
        CHECK((ok ? level : LogLevel::Unknown) == c.expected,
              qPrintable(QStringLiteral("keyed level %1 (%2) -> %3, expected %4")
                  .arg(QLatin1String(c.value), KeyedDefinition::numericLevelsName(c.scale))
                  .arg(int(ok ? level : LogLevel::Unknown)).arg(int(c.expected))));
    }
}

// F-4, PR 2: ключевая схема на загрузке. Парсер и индексатор берут момент,
// уровень и границу записи из ключей одним правилом (classifyLine):
// золотое сравнение на фикстурах JSON Lines и logfmt с заданными моментами
// и уровнями (tests/common/keyedfixtures.h), дозапись объекта частями (обрыв
// посреди строки в кавычках, \u и вложенного) = индекс, построенный разом;
// без извлечения полей схема инертна (эвристика); счётчики сканера
// (analyzeFileForStats) с той же схемой — те же записи, уровни и From/To.
static void testKeyedLoad(const QDir& dir)
{
    using Format = KeyedDefinition::Format;
    for (const Format format : {Format::JsonObject, Format::Logfmt}) {
        const bool json = format == Format::JsonObject;
        const QString tag = json ? QStringLiteral("keyed jsonl") : QStringLiteral("keyed logfmt");
        const QString schema = keyedfx::schema(format);
        const keyedfx::Fixture fx = keyedfx::make(format, 64);
        const QString path = writeFile(dir, tag + ".log", fx.bytes);
        auto lf = std::make_shared<LogFile>(path);
        const auto entries = parseFile(lf, schema);
        const auto index = indexFile(lf, nullptr, schema);
        compareParserVsIndex(tag, path, lf, entries, *index);

        // Заданные значения: момент и уровень каждой строки, записи.
        CHECK(index->lineCount() == fx.lineRecord.size(), qPrintable(tag + ": line count"));
        for (qint64 i = 0; i < qMin<qint64>(index->lineCount(), fx.lineRecord.size()); ++i) {
            const keyedfx::Record want = fx.records[fx.lineRecord[int(i)]];
            CHECK(index->timestampMs(index->logicalId(i)) == want.ms,
                  qPrintable(QString("%1: line %2 moment %3, expected %4").arg(tag).arg(i + 1)
                                 .arg(index->timestampMs(index->logicalId(i))).arg(want.ms)));
            CHECK(index->level(i) == want.level,
                  qPrintable(QString("%1: line %2 level").arg(tag).arg(i + 1)));
            const bool first = i == 0 || fx.lineRecord[int(i)] != fx.lineRecord[int(i) - 1];
            CHECK(index->isPrimary(i) == first,
                  qPrintable(QString("%1: line %2 primary").arg(tag).arg(i + 1)));
        }
        CHECK(index->logicalEntryCount() == fx.records.size(),
              qPrintable(tag + ": one logical record per fixture record"));

        // Без извлечения полей схема не действует — те же строки, что без схемы.
        {
            const auto heuristic = parseFile(lf);
            const auto heuristicIndex = indexFile(lf);
            compareParserVsIndex(tag + " (extraction off)", path, lf, heuristic, *heuristicIndex);
            CHECK(heuristicIndex->logicalEntryCount() != fx.records.size(),
                  qPrintable(tag + ": the heuristic splits this file differently (fixture sanity)"));
        }

        // Дозапись частями: объект, оборванный посреди вложенного, строки и
        // \u, — индекс тот же, что построенный разом по итоговому файлу.
        {
            const QByteArray last = json
                ? "{\"ts\":\"" + keyedfx::iso(70, 0) + "\",\"ctx\":{\"a\":{\"b\":\"x\\\"y\"}},"
                  "\"level\":40,\"msg\":\"\\u041a\\u044d\\u0448 end\"}\n"
                : "ts=" + keyedfx::iso(70, 0) + " level=40 msg=\"Кэш \\\"end\\\"\"\n";
            const QVector<int> cuts = json
                ? QVector<int>{int(last.indexOf("\"b\"")) + 2, int(last.indexOf("x\\")) + 2,
                               int(last.indexOf("\\u041a")) + 4, int(last.size())}
                : QVector<int>{int(last.indexOf("level=")) + 3, int(last.indexOf("\\\"end")) + 1,
                               int(last.size())};
            const QString growPath = writeFile(dir, tag + "-grow.log", fx.bytes);
            auto growLf = std::make_shared<LogFile>(growPath);
            const auto grow = indexFile(growLf, nullptr, schema);
            LogIndexer indexer;
            int done = 0;
            for (const int to : cuts) {
                appendToFile(growPath, last.mid(done, to - done));
                done = to;
                indexAppend(indexer, growLf, grow, grow->lastLineProvisional(), schema);
                const QString wholePath = writeFile(dir, tag + QString("-whole-%1.log").arg(to),
                                                    fx.bytes + last.left(to));
                auto wholeLf = std::make_shared<LogFile>(wholePath);
                const auto whole = indexFile(wholeLf, nullptr, schema);
                compareIndexes(tag + QString(" append up to %1").arg(to), *grow, *whole);
                compareParserVsIndex(tag + QString(" whole up to %1").arg(to), wholePath, wholeLf,
                                     parseFile(wholeLf, schema), *whole);
            }
            const qint64 lastLine = grow->lineCount() - 1;
            CHECK(grow->timestampMs(grow->logicalId(lastLine)) == keyedfx::baseMs() + 70000
                      && grow->level(lastLine) == LogLevel::Warn && grow->isPrimary(lastLine),
                  qPrintable(tag + ": the completed object is a record with its key moment and pino level"));
        }

        // Счётчики сканера: то же правило, что у вкладки с этой схемой.
        {
            LogParser scanner;
            const LogPattern scanSchema(schema);
            const LogParser::FileStats st =
                scanner.analyzeFileForStats(path, LogParser::StatsResume(), nullptr, &scanSchema).stats;
            int warn = 0, error = 0, fatal = 0;
            for (const keyedfx::Record& r : fx.records) {
                warn += r.level == LogLevel::Warn;
                error += r.level == LogLevel::Error;
                fatal += r.level == LogLevel::Fatal;
            }
            CHECK(st.totalEntries == fx.records.size(),
                  qPrintable(QString("%1: scanner records %2, expected %3").arg(tag)
                                 .arg(st.totalEntries).arg(fx.records.size())));
            CHECK(st.warnCount == warn && st.errorCount == error && st.fatalCount == fatal,
                  qPrintable(tag + ": scanner level counts"));
            CHECK(st.firstEntryTimestamp.toMSecsSinceEpoch() == fx.records.first().ms
                      && st.lastEntryTimestamp.toMSecsSinceEpoch() == fx.records.last().ms,
                  qPrintable(tag + ": scanner From/To from the keys"));
            // Сканер каталога схему не задаёт — счётчики по эвристике.
            LogParser noSchema;
            CHECK(noSchema.analyzeFileForStats(path).stats.totalEntries != fx.records.size(),
                  qPrintable(tag + ": without the schema the scanner counts by the heuristic"));
        }
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc >= 3 && qstrcmp(argv[1], "--bench") == 0)
        return runBench(QString::fromLocal8Bit(argv[2]),
                        benchSchema(argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString()));
    if (argc >= 3 && qstrcmp(argv[1], "--bench-resident") == 0)
        return runBenchResident(QString::fromLocal8Bit(argv[2]),
                                benchSchema(argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString()));
    if (argc >= 3 && qstrcmp(argv[1], "--bench-stages") == 0)
        return runBenchStages(QString::fromLocal8Bit(argv[2]),
                              argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString());

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }
    const QDir dir(tmp.path());

    testClassifierRegexEquivalence();
    testLevelVocabulary();
    testTimestampGolden();
    testKeyedValues();
    testKeyedLoad(dir);
    testPre1970(dir);
    testGolden(dir);
    testNewTimestampFormsBackends(dir);
    testClockTimestamps(dir);
    testRecordBoundariesBackends(dir);
    testLevelVocabularyBackends(dir);
    testBom(dir);
    testBlockBoundaries(dir);
    testAppendReindex(dir);
    testSnapshotStability(dir);
    testTimeRangeTracking();
    testSequentialReaderDirections(dir);
    testEncodingDetection(dir);
    testEncodingInTail(dir);
    testEncodingCutWindow(dir);

    if (g_failures == 0)
        std::fprintf(stderr, "lineindex_smoke: all checks passed\n");
    else
        std::fprintf(stderr, "lineindex_smoke: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
