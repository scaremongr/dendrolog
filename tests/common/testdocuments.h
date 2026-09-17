#ifndef TESTDOCUMENTS_H
#define TESTDOCUMENTS_H

// ============================================================================
// testdocuments.h — общие для тестов фикстуры и загрузка документа в LogModel
// обоими путями хранилища, ровно так, как это делает LogViewWidget:
//   • резидентный — LogParser → сортировка батча → LogModel::mergeEntries;
//   • индексный   — LogIndexer → IndexedLogStore::attachFile/appendIndexedRows.
//
// Header-only: подключается из тестов, которые сами компилируют нужные
// исходники src/. Ошибки загрузки не печатаются здесь, а копятся в
// Document::errors — каждый тест сообщает о них своим способом.
// ============================================================================

#include "indexedlogstore.h"
#include "lineindex.h"
#include "logentry.h"
#include "logfile.h"
#include "logindexer.h"
#include "logmodel.h"
#include "logparser.h"
#include "logpattern.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QRandomGenerator>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace testdocs {

// ---------------------------------------------------------------------------
// Схема полей и генерация фикстур
// ---------------------------------------------------------------------------

inline PatternBlock patternBlock(PatternBlock::MatchKind kind, const QString& name,
                                 const QString& lead = QString(),
                                 const QString& closing = QString(),
                                 const QString& sep = QString())
{
    PatternBlock b;
    b.matchKind   = kind;
    b.name        = name;
    b.leadingText = lead;
    b.closingText = closing;
    b.separator   = sep;
    return b;
}

// Схема, под которую генерируются фикстуры:
//   2026-03-05 10:00:00.000 [worker-1] INFO - текст
// Поля: 0 Timestamp, 1 Thread, 2 Level, 3 Message.
inline QString buildSchema()
{
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        patternBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Thread"),
                     QStringLiteral("["), QStringLiteral("]")),
        patternBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"), QString(),
                     QString(), QStringLiteral("-")),
        patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
    };
    return LogPattern::serializeDefinition(def);
}

inline QDateTime fixtureBaseTime()
{
    return QDateTime::fromString(QStringLiteral("2026-03-05 10:00:00.000"),
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

// Пустая строка при ошибке — тест проверяет и сообщает сам.
inline QString writeFile(const QDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(bytes);
    return path;
}

// Детерминированный синтетический лог. Намеренно содержит всё, что ломает
// наивные реализации: многострочные записи, пустые строки, не-ASCII,
// дубликаты меток, а по желанию — преамбулу свободного текста и нарушения
// порядка таймстампов.
// monotonic — метки только возрастают: порядок строк в файле совпадает с
// порядком по времени, и оба бэкенда обязаны показывать строку в строку одно
// и то же даже для одно-файловой вкладки (где индексный не сортирует).
inline QByteArray makeLogBytes(int records, quint32 seed, const QByteArray& eol,
                               const QDateTime& base, bool withPreamble,
                               bool trailingNewline, bool monotonic)
{
    QRandomGenerator rng(seed);
    QByteArray out;

    const char* threads[] = { "worker-1", "worker-2", "io-pool", "main" };
    const char* levels[]  = { "TRACE", "DEBUG", "INFO", "INFO", "WARN", "ERROR", "FATAL" };
    const char* messages[] = {
        "Connection established to %1",
        "Timeout waiting for response from node %1",
        "Disk usage at %1 percent",
        "Кэш прогрет, записей: %1",
        "Retrying request #%1 after transient failure",
        "Checkpoint %1 written",
        "Disk timeout on volume %1",
        "Пользователь %1 вышел из системы",
    };

    auto appendLine = [&](const QString& line) {
        out += line.toUtf8();
        out += eol;
    };

    if (withPreamble) {
        appendLine(QStringLiteral("### DendroLog synthetic fixture"));
        appendLine(QStringLiteral("generated deterministically, seed=%1").arg(seed));
        appendLine(QString());
    }

    qint64 tick = 0;
    for (int i = 0; i < records; ++i) {
        // Шаг времени с разбросом: часть записей получает одинаковый штамп,
        // часть — уходит назад, чтобы слияние/сортировка были нетривиальны.
        const int roll = int(rng.bounded(100));
        if (roll < 10)
            ;                                   // тот же штамп, что у предыдущей
        else if (!monotonic && roll < 15)
            tick -= 400;                        // шаг назад
        else
            tick += 500 + rng.bounded(2000);

        const QDateTime ts = base.addMSecs(tick);
        const QString line =
            QStringLiteral("%1 [%2] %3 - %4")
                .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                     QString::fromLatin1(threads[rng.bounded(4)]),
                     QString::fromLatin1(levels[rng.bounded(7)]),
                     QString::fromLatin1(messages[rng.bounded(8)])
                         .arg(rng.bounded(1000)));
        appendLine(line);

        // Continuation-строки: стек-трейс без таймстампа.
        if (rng.bounded(100) < 18) {
            const int frames = 1 + int(rng.bounded(3));
            for (int f = 0; f < frames; ++f)
                appendLine(QStringLiteral("    at com.example.Module%1.call(Module%1.java:%2)")
                               .arg(f).arg(100 + rng.bounded(900)));
        }
        // Пустая строка как часть записи.
        if (rng.bounded(100) < 6)
            appendLine(QString());
    }

    if (!trailingNewline && out.endsWith(eol))
        out.chop(eol.size());
    return out;
}

// ---------------------------------------------------------------------------
// Прокачка событий
// ---------------------------------------------------------------------------

inline void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    } while (t.elapsed() < ms);
}

// Крутит цикл событий, пока предикат не станет истинным; false — по таймауту.
inline bool waitFor(const std::function<bool()>& ready, int timeoutMs = 60000)
{
    QElapsedTimer t;
    t.start();
    while (!ready()) {
        if (t.elapsed() >= timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// Ожидание тишины модели: фильтрация индексного бэкенда всегда асинхронна (у
// резидентного — от 100k записей) и может идти несколькими инкрементальными
// джобами, поэтому ждём, пока перестанут приходить modelFiltered /
// filterProgress И перестанет меняться rowCount(). Заодно отрабатывают
// отложенные таймеры view (singleShot(0) восстановления прокрутки).
// false — модель не успокоилась за timeoutMs.
inline bool settle(LogModel& model, int quietMs = 80, int timeoutMs = 60000)
{
    QElapsedTimer quiet;
    QElapsedTimer total;
    quiet.start();
    total.start();

    bool progressPending = false;
    auto c1 = QObject::connect(&model, &LogModel::modelFiltered, &model,
                               [&](int) { quiet.restart(); });
    auto c2 = QObject::connect(&model, &LogModel::filterProgress, &model,
                               [&](int p) {
                                   progressPending = (p < 100);
                                   quiet.restart();
                               });

    bool calm = false;
    int lastRows = model.rowCount();
    while (total.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const int rows = model.rowCount();
        if (rows != lastRows) {
            lastRows = rows;
            quiet.restart();
        }
        if (!progressPending && quiet.elapsed() >= quietMs) {
            calm = true;
            break;
        }
    }

    QObject::disconnect(c1);
    QObject::disconnect(c2);
    return calm;
}

// ---------------------------------------------------------------------------
// Документ: модель + её файлы
// ---------------------------------------------------------------------------

struct Document {
    std::unique_ptr<LogModel> model;
    QVector<LogFilePtr>       files;
    QStringList               errors;   // пусто — загрузка прошла чисто
};

// Резидентный путь: LogParser → mergeEntries, как в LogViewWidget. Файлы
// добавляются в УЖЕ существующую модель — так же, как дописывание файла во
// вкладку (строки второго файла встают в середину по времени).
inline void addResidentFiles(Document& doc, const QStringList& paths, const QString& schema)
{
    for (const QString& path : paths) {
        auto logFile = std::make_shared<LogFile>(path);
        doc.files.append(logFile);

        LogParser parser;
        parser.setPattern(schema);
        parser.setExtractionEnabled(true);

        QVector<QVector<std::shared_ptr<LogEntry>>> batches;
        bool done = false;

        QObject relay;
        QObject::connect(&parser, &LogParser::entriesParsed, &relay,
                         [&](const QVector<std::shared_ptr<LogEntry>>& batch,
                             const LogFilePtr&) { batches.append(batch); });
        QObject::connect(&parser, &LogParser::parsingFinished, &relay,
                         [&](int, const LogFilePtr&) { done = true; });
        QObject::connect(&parser, &LogParser::parsingFailed, &relay,
                         [&](const LogFilePtr&) { done = true; });

        parser.startParsing(logFile);
        if (!waitFor([&done] { return done; }))
            doc.errors << QStringLiteral("резидентный парсинг %1 не завершился").arg(path);

        // Порядок подачи как в production: батч сортируется и сливается.
        for (auto& batch : batches) {
            std::sort(batch.begin(), batch.end(), logEntryPtrLess);
            doc.model->mergeEntries(batch);
        }
    }
}

inline Document buildResident(const QStringList& paths, const QString& schema)
{
    Document doc;
    doc.model = std::make_unique<LogModel>();
    addResidentFiles(doc, paths, schema);
    if (!settle(*doc.model))
        doc.errors << QStringLiteral("резидентная модель не пришла в покой после загрузки");
    return doc;
}

// Как файлы попадают в индексную вкладку. Порядок строк у стора собирается
// по-разному, поэтому оба сценария проверяются отдельно:
//   • Sequential — файл полностью проиндексирован, затем добавлен следующий
//     («дописал файл в открытую вкладку»): второй файл подключается к уже
//     показанным строкам первого (materializeAllRefs);
//   • Concurrent — все файлы подключены ДО первого батча и индексируются
//     параллельно («открыл несколько файлов разом», объединение вкладок,
//     конверсия вкладки в индексную): батчи разных файлов приходят вперемешку.
enum class IndexedLoad { Sequential, Concurrent };

inline QString describe(IndexedLoad mode)
{
    return mode == IndexedLoad::Sequential ? QStringLiteral("по очереди")
                                           : QStringLiteral("разом");
}

// Индексный путь: LogIndexer → attachFile/appendIndexedRows, как в
// LogViewWidget::startIndexedLoad + handleIndexBatchReady. Сигналы индексатора
// приходят из воркера очередью — порядок «батчи, затем finished» сохраняется.
// Модель документа переводится на индексный бэкенд, если ещё не переведена.
inline void addIndexedFiles(Document& doc, const QStringList& paths, const QString& schema,
                            IndexedLoad mode = IndexedLoad::Sequential)
{
    IndexedLogStore* store = doc.model->indexedOrNull();
    if (!store) {
        store = doc.model->convertToIndexedBackend();
        store->setFieldPattern(schema, true);
    }

    struct Job {
        LogFilePtr                  logFile;
        std::shared_ptr<LineIndex>  index;
        std::unique_ptr<LogIndexer> indexer;
        bool                        done = false;
        bool                        fallback = false;
    };
    std::vector<std::unique_ptr<Job>> jobs;
    QObject relay;

    const auto prepare = [&](const QString& path) -> Job& {
        auto job = std::make_unique<Job>();
        job->logFile = std::make_shared<LogFile>(path);
        job->index = std::make_shared<LineIndex>();
        job->indexer = std::make_unique<LogIndexer>();
        job->indexer->setPattern(schema);
        job->indexer->setExtractionEnabled(true);
        doc.files.append(job->logFile);
        store->attachFile(job->logFile, job->index);

        Job* raw = job.get();
        QObject::connect(raw->indexer.get(), &LogIndexer::indexBatchReady, &relay,
                         [store](const LogFilePtr& lf, qint64 first, qint64 count) {
                             store->appendIndexedRows(lf, first, count, false);
                         });
        QObject::connect(raw->indexer.get(), &LogIndexer::indexingFinished, &relay,
                         [raw](qint64, const LogFilePtr&) { raw->done = true; });
        QObject::connect(raw->indexer.get(), &LogIndexer::indexingFailed, &relay,
                         [raw](const LogFilePtr&) { raw->done = true; });
        QObject::connect(raw->indexer.get(), &LogIndexer::needsResidentFallback, &relay,
                         [raw](const LogFilePtr&, const QString&) {
                             raw->fallback = true;
                             raw->done = true;
                         });
        jobs.push_back(std::move(job));
        return *raw;
    };

    if (mode == IndexedLoad::Sequential) {
        for (const QString& path : paths) {
            Job& job = prepare(path);
            job.indexer->startIndexing(job.logFile, job.index);
            waitFor([&job] { return job.done; });
        }
    } else {
        for (const QString& path : paths)
            prepare(path);
        for (auto& job : jobs)
            job->indexer->startIndexing(job->logFile, job->index);
        waitFor([&jobs] {
            for (const auto& job : jobs)
                if (!job->done)
                    return false;
            return true;
        });
    }

    for (const auto& job : jobs) {
        if (!job->done)
            doc.errors << QStringLiteral("индексация %1 не завершилась").arg(job->logFile->filePath);
        if (job->fallback)
            doc.errors << QStringLiteral("неожиданный откат в резидентный путь: %1")
                              .arg(job->logFile->filePath);
    }
}

inline Document buildIndexed(const QStringList& paths, const QString& schema,
                             IndexedLoad mode = IndexedLoad::Sequential)
{
    Document doc;
    doc.model = std::make_unique<LogModel>();
    addIndexedFiles(doc, paths, schema, mode);
    if (!settle(*doc.model))
        doc.errors << QStringLiteral("индексная модель не пришла в покой после загрузки");
    return doc;
}

} // namespace testdocs

#endif // TESTDOCUMENTS_H
