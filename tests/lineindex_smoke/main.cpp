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

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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

// Полная индексация файла; блокирует до завершения.
static std::shared_ptr<LineIndex> indexFile(const LogFilePtr& lf,
                                            bool* fallback = nullptr)
{
    auto index = std::make_shared<LineIndex>();
    LogIndexer indexer;
    std::atomic_bool done{false};
    std::atomic_bool fb{false};
    QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                     [&](qint64, const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::indexingFailed, &indexer,
                     [&](const LogFilePtr&) { done = true; },
                     Qt::DirectConnection);
    QObject::connect(&indexer, &LogIndexer::needsResidentFallback, &indexer,
                     [&](const LogFilePtr&, const QString&) { fb = true; done = true; },
                     Qt::DirectConnection);
    indexer.startIndexing(lf, index);
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
                        bool reindexProvisionalTail)
{
    std::atomic_bool done{false};
    auto conn = QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                                 [&](qint64, const LogFilePtr&) { done = true; },
                                 Qt::DirectConnection);
    indexer.startIndexingFrom(lf, index, reindexProvisionalTail);
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < 30000)
        QThread::msleep(2);
    CHECK(done.load(), "incremental indexing did not finish in time");
    QObject::disconnect(conn);
}

// Резидентный парсинг файла; блокирует до завершения.
static QVector<std::shared_ptr<LogEntry>> parseFile(const LogFilePtr& lf)
{
    QVector<std::shared_ptr<LogEntry>> all;
    LogParser parser;
    std::atomic_bool done{false};
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
    parser.startParsing(lf);
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
            ? e.timestamp().toMSecsSinceEpoch() : -1;
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
        QObject::connect(&indexer, &LogIndexer::indexingFinished, &indexer,
                         [&](qint64, const LogFilePtr&) { done = true; },
                         Qt::DirectConnection);
        indexer.startIndexing(lf, index);
        QElapsedTimer t; t.start();
        while (!done && t.elapsed() < 30000)
            QThread::msleep(2);
        CHECK(done.load(), "append: initial indexing timeout");
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

// detectTimestamp целиком: ISO-регекс + ручные фолбэк-форматы, без
// делегирования новому коду — иначе ложные срабатывания сканера
// маскировались бы.
static bool refDetectTimestamp(const QRegularExpression& isoRe,
                               const QRegularExpression& suffixRe,
                               const QString& line, QDateTime& ts)
{
    const auto match = isoRe.match(line);
    if (match.hasMatch()) {
        const auto dateTimePartRef = match.capturedView(1);

        bool ok = true;
        const int year = dateTimePartRef.mid(0, 4).toInt(&ok);
        if (!ok) return false;
        const int month = dateTimePartRef.mid(5, 2).toInt(&ok);
        if (!ok) return false;
        const int day = dateTimePartRef.mid(8, 2).toInt(&ok);
        if (!ok) return false;
        const int hour = dateTimePartRef.mid(11, 2).toInt(&ok);
        if (!ok) return false;
        const int minute = dateTimePartRef.mid(14, 2).toInt(&ok);
        if (!ok) return false;
        const int second = dateTimePartRef.mid(17, 2).toInt(&ok);
        if (!ok) return false;

        const RefSuffix suffix = refParseSuffix(match, 2);
        if (QDate::isValid(year, month, day)
            && QTime::isValid(hour, minute, second, suffix.millis)) {
            ts = refMoment(year, month, day, hour, minute, second, suffix);
            return true;
        }
        return false;
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
            const auto suffix = suffixRe.match(line, 19, QRegularExpression::NormalMatch,
                                               QRegularExpression::AnchorAtOffsetMatchOption);
            ts = refMoment(year, month, day, hour, minute, second, refParseSuffix(suffix, 1));
            return true;
        }
    }

    ts = QDateTime();
    return false;
}

static bool refDetectLogLevel(const QRegularExpression& levelRe,
                              const QString& line, LogLevel& level)
{
    const auto match = levelRe.match(line);
    if (match.hasMatch()) {
        level = StrToLevel(match.captured(1).toUpper());
        return true;
    }
    return false;
}

static void checkClassifierLine(const LineClassifier& classifier,
                                const QRegularExpression& isoRe,
                                const QRegularExpression& suffixRe,
                                const QRegularExpression& levelRe,
                                const QString& line)
{
    QDateTime tsNew, tsRef;
    const bool hasNew = classifier.detectTimestamp(line, tsNew);
    const bool hasRef = refDetectTimestamp(isoRe, suffixRe, line, tsRef);
    if (hasNew != hasRef || (hasNew && tsNew != tsRef)) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL classifier ts mismatch on '%s': new=%d '%s' ref=%d '%s'\n",
                     qPrintable(line), int(hasNew),
                     qPrintable(tsNew.toString(Qt::ISODateWithMs)), int(hasRef),
                     qPrintable(tsRef.toString(Qt::ISODateWithMs)));
    }

    // Быстрый вариант индексатора обязан давать те же мс epoch.
    qint64 msNew = -2;
    const bool hasMsNew = classifier.detectTimestampMs(line, msNew);
    if (hasMsNew != hasRef || (hasMsNew && msNew != tsRef.toMSecsSinceEpoch())) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL classifier tsMs mismatch on '%s': new=%d %lld ref=%d %lld\n",
                     qPrintable(line), int(hasMsNew), (long long)msNew, int(hasRef),
                     (long long)(hasRef ? tsRef.toMSecsSinceEpoch() : -1));
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
    const QRegularExpression isoRe(PatternHeuristics::isoTimestampDetectPattern());
    const QRegularExpression suffixRe(PatternHeuristics::timestampSuffixDetectPattern());
    const QRegularExpression levelRe(PatternHeuristics::levelDetectPattern(),
                                     QRegularExpression::CaseInsensitiveOption);
    CHECK(isoRe.isValid() && suffixRe.isValid(), "reference timestamp regexes compile");
    const auto check = [&](const QString& line) {
        checkClassifierLine(classifier, isoRe, suffixRe, levelRe, line);
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
    };
    for (const QString& base : bases) {
        for (int iter = 0; iter < 4000; ++iter) {
            QString line = base;
            const int pos = int(rng.bounded(line.size()));
            line[pos] = alphabet.at(int(rng.bounded(alphabet.size())));
            check(line);
        }
    }
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
}

// Бенчмарк-режим: lineindex_smoke --bench <файл> [строка-схемы] — чистая
// скорость LogIndexer без GUI (для локализации узких мест индексации).
static int runBench(const QString& path, const QString& patternString)
{
    auto lf = std::make_shared<LogFile>(path);
    auto index = std::make_shared<LineIndex>();
    LogIndexer indexer;
    if (!patternString.isEmpty()) {
        indexer.setPattern(patternString);
        indexer.setExtractionEnabled(true);
    }
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
    indexer.startIndexing(lf, index);
    while (!done)
        QThread::msleep(20);
    std::fprintf(stderr, "bench: %lld lines, %lld logical, %.1f s\n",
                 (long long)index->lineCount(),
                 (long long)index->logicalEntryCount(), t.elapsed() / 1000.0);
    return 0;
}

// Ступенчатая атрибуция стоимости индексации: 0 — чтение+разрез по \n,
// 1 — +декодирование UTF-8, 2 — +detectTimestamp (с toMSecsSinceEpoch),
// 3 — +detectLogLevel. Дельты между ступенями = цена каждого шага.
static int runBenchStages(const QString& path)
{
    for (int stage = 0; stage <= 3; ++stage) {
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
                if (stage >= 1) {
                    text = QString::fromUtf8(data, int(len));
                    sink += text.size();
                    if (stage >= 2) {
                        QDateTime ts;
                        if (classifier.detectTimestamp(text, ts)) {
                            ++tsCount;
                            sink += ts.toMSecsSinceEpoch() & 1;
                        }
                    }
                    if (stage >= 3) {
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
                     "stage %d: %6.1f s  (lines=%lld ts=%lld lvl=%lld sink=%lld)\n",
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
        SequentialLineReader reader(path, window);
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc >= 3 && qstrcmp(argv[1], "--bench") == 0)
        return runBench(QString::fromLocal8Bit(argv[2]),
                        argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString());
    if (argc >= 3 && qstrcmp(argv[1], "--bench-stages") == 0)
        return runBenchStages(QString::fromLocal8Bit(argv[2]));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }
    const QDir dir(tmp.path());

    testClassifierRegexEquivalence();
    testTimestampGolden();
    testGolden(dir);
    testBom(dir);
    testBlockBoundaries(dir);
    testAppendReindex(dir);
    testSnapshotStability(dir);
    testSequentialReaderDirections(dir);

    if (g_failures == 0)
        std::fprintf(stderr, "lineindex_smoke: all checks passed\n");
    else
        std::fprintf(stderr, "lineindex_smoke: %d check(s) FAILED\n", g_failures);
    return g_failures;
}
