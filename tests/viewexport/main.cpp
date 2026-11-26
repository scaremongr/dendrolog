#include "viewexport.h"
#include "keyedfixtures.h"
#include "testdocuments.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThreadPool>
#include <cstdio>
#include <filesystem>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

static void put(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly), "fixture opens");
    CHECK(file.write(bytes) == bytes.size(), "fixture writes");
}

static QByteArray read(const QString& path)
{
    QFile file(path);
    CHECK(file.open(QIODevice::ReadOnly), "result opens for reading");
    return file.readAll();
}

// Exercise both a short write and a device error, including a failure that is
// only observed when QTextStream flushes the final small buffered row.
class FailingDevice : public QIODevice {
public:
    explicit FailingDevice(bool shortWrite) : m_shortWrite(shortWrite)
    { open(QIODevice::WriteOnly); }
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char*, qint64 size) override
    {
        setErrorString(QStringLiteral("Injected disk write failure"));
        return m_shortWrite ? size / 2 : -1;
    }
private:
    bool m_shortWrite;
};

// Строки файла результата (перевод строки платформы снят).
static QStringList exportedLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {QStringLiteral("<cannot open>")};
    QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

static QStringList displayedLines(const LogModel& model)
{
    QStringList lines;
    for (int row = 0; row < model.rowCount(); ++row)
        lines << model.data(model.index(row), Qt::DisplayRole).toString();
    return lines;
}

// Фоновый экспорт до конца; результат задания.
static ViewExport::Result runJob(const LogModel& model, const QString& destination,
                                 const QStringList& sources)
{
    ViewExportJob job;
    ViewExport::Result result{ViewExport::Error::Write, QStringLiteral("not finished")};
    bool done = false;
    QObject::connect(&job, &ViewExportJob::finished, [&](const ViewExport::Result& r) {
        result = r;
        done = true;
    });
    if (!job.start(model, destination, sources))
        return {ViewExport::Error::Write, QStringLiteral("not started")};
    testdocs::waitFor([&] { return done; });
    return result;
}

// Save View As в фоне пишет ровно то, что показывает модель, и только снимок
// на момент старта; отмена оставляет прежний файл.
static void testBackgroundExport(const QDir& dir)
{
    const QString schema = testdocs::buildSchema();
    QByteArray bytes;
    for (int i = 0; i < 3000; ++i) {
        bytes += "2026-03-05 10:00:" + QByteArray::number(i / 60 % 60).rightJustified(2, '0')
               + ".000 [worker-" + QByteArray::number(i % 3) + "] " + (i % 4 ? "INFO" : "ERROR")
               + " - message " + QByteArray::number(i) + '\n';
        if (i % 500 == 7)
            bytes += "    continuation of " + QByteArray::number(i) + '\n';
    }
    const QString path = testdocs::writeFile(dir, "export-source.log", bytes);
    auto resident = testdocs::buildResident({path}, schema);
    auto indexed = testdocs::buildIndexed({path}, schema);
    const QStringList fieldNames = LogPattern(schema).fieldNames();

    struct { const char* name; LogModel* model; } models[] = {
        {"resident", resident.model.get()}, {"indexed", indexed.model.get()}};
    for (const auto& m : models) {
        LogModel& model = *m.model;
        model.setAvailableFields(fieldNames);
        const QString target = dir.filePath(QStringLiteral("export-%1.log").arg(m.name));
        for (const bool fields : {false, true}) {
            model.setFieldDisplaySelection(fields, {3, 1}); // Message, Thread
            const auto result = runJob(model, target, {path});
            CHECK(result.ok(), qPrintable(QStringLiteral("%1: export succeeds").arg(m.name)));
            CHECK(exportedLines(target) == displayedLines(model),
                  qPrintable(QStringLiteral("%1 fields=%2: file equals the displayed text")
                                 .arg(m.name).arg(fields)));
        }

        // Экспорт пишет снимок на момент старта: фильтр, применённый сразу
        // после, на файл не влияет.
        model.setLogLevelFilter({LogLevel::Error});
        CHECK(testdocs::settle(model), "filter settles");
        const QStringList filtered = displayedLines(model);
        {
            ViewExportJob job;
            bool done = false;
            ViewExport::Result result;
            QObject::connect(&job, &ViewExportJob::finished,
                             [&](const ViewExport::Result& r) { result = r; done = true; });
            CHECK(job.start(model, target, {path}), "start export of the filtered view");
            CHECK(!job.start(model, target, {path}), "second export is refused while running");
            model.setLogLevelFilter({});
            CHECK(testdocs::waitFor([&] { return done; }), "export finishes");
            CHECK(result.ok() && exportedLines(target) == filtered,
                  qPrintable(QStringLiteral("%1: export writes the view as it was at start").arg(m.name)));
            CHECK(testdocs::settle(model), "filter reset settles");
        }

        // Q-25: воркер закончил, finished ещё не доставлен (событий не было) —
        // задание всё ещё идёт: второй start() отклонён, а первый отдаёт
        // finished ровно один раз. Раньше isRunning() смотрел на QFuture, и
        // на быстром пуле маленький экспорт «заканчивался» до второго start.
        {
            ViewExportJob job;
            int finishedCount = 0;
            ViewExport::Result result;
            QObject::connect(&job, &ViewExportJob::finished,
                             [&](const ViewExport::Result& r) { result = r; ++finishedCount; });
            CHECK(job.start(model, target, {path}), "start export (worker done before delivery)");
            QThreadPool::globalInstance()->waitForDone();
            CHECK(job.isRunning(), "job runs until finished is delivered");
            CHECK(!job.start(model, target, {path}), "second export is refused after the worker ended");
            CHECK(testdocs::waitFor([&] { return finishedCount > 0; }), "export finishes");
            QCoreApplication::processEvents();
            CHECK(finishedCount == 1 && result.ok(), "finished is delivered exactly once");
            CHECK(!job.isRunning(), "job is not running after finished");
        }

        // Источник защищён и в фоне.
        const auto own = runJob(model, path, {path});
        CHECK(own.error == ViewExport::Error::SourceFile,
              qPrintable(QStringLiteral("%1: background export refuses the source").arg(m.name)));
    }

    // Отмена: прежний файл назначения не тронут, finished приходит с Cancelled.
    {
        QByteArray big;
        big.reserve(400000 * 60);
        for (int i = 0; i < 400000; ++i)
            big += "2026-03-05 10:00:00.000 [w] INFO - row " + QByteArray::number(i) + '\n';
        const QString bigPath = testdocs::writeFile(dir, "export-big.log", big);
        auto doc = testdocs::buildIndexed({bigPath}, schema);
        const QString target = dir.filePath("export-cancel.log");
        put(target, "previous export");
        ViewExportJob job;
        bool done = false;
        ViewExport::Result result;
        QObject::connect(&job, &ViewExportJob::finished,
                         [&](const ViewExport::Result& r) { result = r; done = true; });
        CHECK(job.start(*doc.model, target, {bigPath}), "start large export");
        job.cancel();
        CHECK(testdocs::waitFor([&] { return done; }), "cancelled export reports finished");
        CHECK(result.error == ViewExport::Error::Cancelled, "cancelled export reports Cancelled");
        CHECK(read(target) == "previous export", "cancelled export keeps the destination");
        CHECK(!job.isRunning(), "cancelled export is not running");
    }

    // Отмена синхронной записи через флаг.
    {
        const QString target = dir.filePath("export-flag.log");
        put(target, "kept");
        std::atomic_bool cancel{false};
        int produced = 0;
        const auto result = ViewExport::save(target, {path}, [&](const ViewExport::RowSink& sink) {
            for (int i = 0; i < 100000; ++i) {
                ++produced;
                if (i == 1000)
                    cancel = true;
                if (!sink(QStringLiteral("row %1").arg(i)))
                    return;
            }
        }, &cancel);
        CHECK(result.error == ViewExport::Error::Cancelled, "cancel flag stops the export");
        CHECK(produced < 100000, "rows stop being produced after cancel");
        CHECK(read(target) == "kept", "cancelled save keeps the destination");
    }
}

// Файл не в UTF-8 (F-1, шаг 3): Save View As пишет UTF-8 ровно тот текст,
// что показан, — на резидентной вкладке так же, как на индексной; и это
// исходный текст, а не байты cp1251, прочитанные как UTF-8.
static void testEncodedExport(const QDir& dir)
{
    const QString schema = testdocs::buildSchema();
    const QString text = testdocs::cyrillicLogText(5);
    const QString path = testdocs::writeFile(
        dir, "export-cp1251.log", testdocs::encodeText(text, TextEncoding::Windows1251));
    auto resident = testdocs::buildResident({path}, schema);
    auto indexed = testdocs::buildIndexed({path}, schema);
    QStringList want = text.split(QLatin1Char('\n'));
    want.removeLast();
    struct { const char* name; LogModel* model; } models[] = {
        {"resident", resident.model.get()}, {"indexed", indexed.model.get()}};
    for (const auto& m : models) {
        const QString target = dir.filePath(QStringLiteral("export-cp1251-%1.log").arg(m.name));
        const auto result = runJob(*m.model, target, {path});
        CHECK(result.ok(), qPrintable(QStringLiteral("%1 cp1251: export succeeds").arg(m.name)));
        const QStringList written = exportedLines(target);
        CHECK(written == displayedLines(*m.model),
              qPrintable(QStringLiteral("%1 cp1251: file equals the displayed text").arg(m.name)));
        CHECK(written == want,
              qPrintable(QStringLiteral("%1 cp1251: the exported text is the original text").arg(m.name)));
    }
}


// ---------------------------------------------------------------------------
// F-9: CSV и JSON Lines
// ---------------------------------------------------------------------------

// Независимый разбор CSV по RFC 4180: записи через CRLF, поле в кавычках может
// содержать запятые, CR, LF и удвоенные кавычки. ok = false — нарушение формата.
static QList<QStringList> parseCsv(const QByteArray& bytes, bool* ok, bool* bom)
{
    *ok = true;
    QByteArray body = bytes;
    *bom = body.startsWith("\xEF\xBB\xBF");
    if (*bom)
        body.remove(0, 3);
    const QString text = QString::fromUtf8(body);
    QList<QStringList> records;
    QStringList record;
    QString field;
    bool quoted = false, inQuotes = false, fieldStarted = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('"')) {
                    field += c;
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                field += c;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            if (fieldStarted || quoted) { *ok = false; return records; }
            quoted = inQuotes = fieldStarted = true;
        } else if (c == QLatin1Char(',')) {
            record << field;
            field.clear();
            quoted = fieldStarted = false;
        } else if (c == QLatin1Char('\r')) {
            if (i + 1 >= text.size() || text.at(i + 1) != QLatin1Char('\n')) { *ok = false; return records; }
            ++i;
            record << field;
            records << record;
            record.clear();
            field.clear();
            quoted = fieldStarted = false;
        } else if (c == QLatin1Char('\n')) {
            *ok = false;   // голый LF вне кавычек — не RFC 4180
            return records;
        } else {
            if (quoted) { *ok = false; return records; }
            field += c;
            fieldStarted = true;
        }
    }
    if (inQuotes || fieldStarted || !record.isEmpty())
        *ok = false;   // последняя запись обязана кончаться CRLF
    return records;
}

// Строки JSON Lines, разобранные QJsonDocument; ok = false — не JSON-объект.
static QList<QJsonObject> parseJsonLines(const QByteArray& bytes, bool* ok)
{
    *ok = !bytes.startsWith("\xEF\xBB\xBF") && !bytes.contains('\r');
    QList<QJsonObject> objects;
    QList<QByteArray> lines = bytes.split('\n');
    if (lines.isEmpty() || !lines.last().isEmpty())
        *ok = false;
    else
        lines.removeLast();
    for (const QByteArray& line : lines) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            *ok = false;
            std::fprintf(stderr, "bad JSON line: %s\n", line.constData());
            return objects;
        }
        objects << doc.object();
    }
    return objects;
}

struct ExpectedTable {
    QStringList headers;
    QList<QStringList> rows;           // по заголовкам
    QList<qint64> moments;             // мс; kNoTimestampMs — метки нет
};

// Эталон перебором видимых строк модели (entryAt), мимо снапшота задания:
// колонки — заданные тестом (имена схемы), Message — сырой текст строки без
// полей, значения — раскодированные.
static ExpectedTable expectedTable(const LogModel& model, const QStringList& fieldNames,
                                   const QVector<int>& fieldColumns, int messageColumn,
                                   bool fileColumn)
{
    ExpectedTable t;
    t.headers << QStringLiteral("_time");
    if (fileColumn)
        t.headers << QStringLiteral("_file");
    for (const int fi : fieldColumns)
        t.headers << (fi >= 0 ? fieldNames.at(fi) : QStringLiteral("Message"));
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto entry = model.entryAt(row);
        QStringList values;
        const QDateTime ts = entry->timestamp();
        t.moments << (ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs);
        values << QString();   // _time проверяется моментом
        if (fileColumn)
            values << QDir::toNativeSeparators(entry->sourceFile()->displayPath());
        const LogEntryFields fields = model.fieldsOf(*entry);
        for (int k = 0; k < fieldColumns.size(); ++k) {
            const int fi = fieldColumns.at(k);
            if (k == messageColumn && (fields.isEmpty() || (fi >= 0 && !fields.has(fi))))
                values << entry->message();
            else if (fields.isEmpty())
                values << QString();
            else
                values << (fi >= 0 ? fields.value(fi, entry->message()) : QString());
        }
        t.rows << values;
    }
    return t;
}

static qint64 parseIsoMoment(const QString& text)
{
    if (text.isEmpty())
        return kNoTimestampMs;
    const QDateTime t = QDateTime::fromString(text, Qt::ISODateWithMs);
    return t.isValid() ? t.toMSecsSinceEpoch() : kNoTimestampMs + 1;
}

static ViewExport::Result runJobAs(const LogModel& model, const QString& destination,
                                   const QStringList& sources, ViewExport::Format format)
{
    ViewExportJob job;
    ViewExport::Result result{ViewExport::Error::Write, QStringLiteral("not finished")};
    bool done = false;
    QObject::connect(&job, &ViewExportJob::finished, [&](const ViewExport::Result& r) {
        result = r;
        done = true;
    });
    if (!job.start(model, destination, sources, format))
        return {ViewExport::Error::Write, QStringLiteral("not started")};
    testdocs::waitFor([&] { return done; });
    return result;
}

// Файл CSV и JSON Lines против эталона: заголовки, каждое значение, момент.
static void checkTable(const QString& label, const QString& csvPath, const QString& jsonPath,
                       const ExpectedTable& want)
{
    bool ok = false, bom = false;
    const QList<QStringList> csv = parseCsv(read(csvPath), &ok, &bom);
    CHECK(ok, qPrintable(label + QStringLiteral(": CSV is RFC 4180")));
    CHECK(bom, qPrintable(label + QStringLiteral(": CSV starts with a UTF-8 BOM")));
    CHECK(!csv.isEmpty() && csv.first() == want.headers,
          qPrintable(label + QStringLiteral(": CSV header %1").arg(csv.value(0).join(QLatin1Char('|')))));
    CHECK(csv.size() == want.rows.size() + 1,
          qPrintable(label + QStringLiteral(": CSV has %1 records, want %2")
                                 .arg(csv.size() - 1).arg(want.rows.size())));
    int bad = 0;
    for (int r = 0; r < want.rows.size() && r + 1 < csv.size(); ++r) {
        QStringList got = csv.at(r + 1);
        const qint64 moment = parseIsoMoment(got.value(0));
        got[0] = QString();
        if ((got != want.rows.at(r) || moment != want.moments.at(r)) && bad++ < 3)
            std::fprintf(stderr, "  %s CSV row %d: got [%s] want [%s] moment %lld/%lld\n",
                         qPrintable(label), r, qPrintable(got.join(QLatin1Char('|'))),
                         qPrintable(want.rows.at(r).join(QLatin1Char('|'))),
                         (long long)moment, (long long)want.moments.at(r));
    }
    CHECK(bad == 0, qPrintable(label + QStringLiteral(": CSV values equal the visible rows")));

    const QList<QJsonObject> json = parseJsonLines(read(jsonPath), &ok);
    CHECK(ok, qPrintable(label + QStringLiteral(": JSON Lines parses, LF, no BOM")));
    CHECK(json.size() == want.rows.size(),
          qPrintable(label + QStringLiteral(": JSON Lines has %1 objects, want %2")
                                 .arg(json.size()).arg(want.rows.size())));
    // Порядок ключей — как у заголовков CSV (QJsonObject его не хранит).
    const QByteArray rawJson = read(jsonPath);
    const QByteArray firstLine = rawJson.left(rawJson.indexOf('\n'));
    qsizetype at = 0;
    bool ordered = true;
    for (const QString& h : want.headers) {
        const qsizetype pos = firstLine.indexOf("\"" + h.toUtf8() + "\":", at);
        ordered = ordered && pos >= at;
        at = pos;
    }
    CHECK(json.isEmpty() || ordered, qPrintable(label + QStringLiteral(": JSON keys follow the column order")));
    bad = 0;
    for (int r = 0; r < want.rows.size() && r < json.size(); ++r) {
        const QJsonObject& o = json.at(r);
        bool same = o.size() == want.headers.size();
        for (int c = 1; c < want.headers.size(); ++c)
            same = same && o.value(want.headers.at(c)).isString()
                   && o.value(want.headers.at(c)).toString() == want.rows.at(r).at(c);
        const QJsonValue time = o.value(QStringLiteral("_time"));
        const qint64 moment = time.isNull() ? kNoTimestampMs : parseIsoMoment(time.toString());
        same = same && (time.isNull() || time.isString()) && moment == want.moments.at(r);
        if (!same && bad++ < 3)
            std::fprintf(stderr, "  %s JSON row %d: %s\n", qPrintable(label), r,
                         QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
    }
    CHECK(bad == 0, qPrintable(label + QStringLiteral(": JSON values equal the visible rows")));
}

// Чистые функции формата: имя файла, колонки, экранирование.
static void testTableFormatting()
{
    using ViewExport::Format;
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.CSV")) == Format::Csv, "csv by suffix");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.jsonl")) == Format::JsonLines, "jsonl by suffix");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.ndjson")) == Format::JsonLines, "ndjson by suffix");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.json")) == Format::Text, "json is not JSON Lines");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.log"), Format::Csv) == Format::Text, "log is text");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a.out"), Format::Csv) == Format::Csv, "unknown — fallback");
    CHECK(ViewExport::formatForFileName(QStringLiteral("a")) == Format::Text, "no suffix — text");

    const QStringList schema = {QStringLiteral("Timestamp"), QStringLiteral("Thread"),
                                QStringLiteral("Level"), QStringLiteral("Message")};
    auto c = ViewExport::columnsFor(schema, true, {3, 1}, false);
    CHECK(c.headers == QStringList({"_time", "Message", "Thread"}), "selected columns in view order");
    CHECK(c.messageColumn == 0 && c.fieldIndexes == QVector<int>({3, 1}), "message column of the selection");
    c = ViewExport::columnsFor(schema, false, {3}, true);
    CHECK(c.headers == QStringList({"_time", "_file", "Timestamp", "Thread", "Level", "Message"}),
          "raw view — all schema columns, file column in a merged tab");
    c = ViewExport::columnsFor(schema, true, {1, 2}, false);
    CHECK(c.headers == QStringList({"_time", "Thread", "Level", "Message"}) && c.fieldIndexes.last() == 3,
          "Message not selected — the schema's Message is appended");
    c = ViewExport::columnsFor({QStringLiteral("Time"), QStringLiteral("Text")}, true, {1}, false);
    CHECK(c.headers == QStringList({"_time", "Text", "Message"}) && c.fieldIndexes.last() == -1,
          "schema without Message — a synthetic Message column");
    c = ViewExport::columnsFor({QStringLiteral("_time"), QStringLiteral("Message"), QStringLiteral("Message")},
                               false, {}, false);
    CHECK(c.headers == QStringList({"_time", "_time~2", "Message", "Message~2"}),
          qPrintable(QStringLiteral("clashing column names are made unique: %1").arg(c.headers.join(QLatin1Char(',')))));
    c = ViewExport::columnsFor({}, false, {}, false);
    CHECK(c.headers == QStringList({"_time", "Message"}), "no schema — time and raw Message");

    CHECK(ViewExport::csvRecord({"a", "b,c", "say \"hi\"", "x\ny", "", " pad"})
              == QStringLiteral("a,\"b,c\",\"say \"\"hi\"\"\",\"x\ny\",,\" pad\""),
          "CSV quoting");
    QString json;
    const QChar lone = QChar(0xD800);
    ViewExport::appendJsonString(json, QStringLiteral("q\"b\\\n\r\t") + QChar(1) + lone
                                           + QString::fromUtf8("\xF0\x9F\x98\x80 Ж"));
    CHECK(json == QStringLiteral("\"q\\\"b\\\\\\n\\r\\t\\u0001\\ud800")
                      + QString::fromUtf8("\xF0\x9F\x98\x80 Ж\""),
          qPrintable(QStringLiteral("JSON escaping: %1").arg(json)));
    const QString rec = ViewExport::jsonRecord({"_time", "k"}, {QString(), "v"}, {true, false});
    CHECK(rec == QStringLiteral("{\"_time\":null,\"k\":\"v\"}"), "JSON record with null");
}

// Экспорт CSV / JSON Lines теми же заданием и снимком на обоих бэкендах:
// регексная схема (кириллица, кавычки, запятые, многострочная запись), ключевая
// (раскодированные значения с переводами строк и \u), слитая вкладка, фильтр.
static void testTableExport(const QDir& dir)
{
    using ViewExport::Format;
    // Регексная схема: Timestamp, Thread, Level, Message.
    const QString schema = testdocs::buildSchema();
    const QStringList fieldNames = LogPattern(schema).fieldNames();
    QByteArray a, b;
    for (int i = 0; i < 400; ++i) {
        const QByteArray sec = QByteArray::number(i / 10 % 60).rightJustified(2, '0');
        QByteArray msg = "сообщение " + QByteArray::number(i);
        if (i % 7 == 0) msg += ", с запятой";
        if (i % 11 == 0) msg += " и \"кавычками\"";
        a += "2026-03-05 10:00:" + sec + ".100 [поток-" + QByteArray::number(i % 3) + "] "
           + (i % 5 ? "INFO" : "ERROR") + " - " + msg + '\n';
        if (i % 50 == 3)
            a += "    продолжение, \"строки\" " + QByteArray::number(i) + '\n';
        b += "2026-03-05 10:00:" + sec + ".200 [b-" + QByteArray::number(i % 2) + "] "
           + (i % 3 ? "WARN" : "ERROR") + " - второй файл " + QByteArray::number(i) + '\n';
    }
    // Схема узнаёт метку, поток и уровень, но не Message (префиксный вариант).
    const QString partial = QString::fromUtf8("2026-03-05 10:00:59.900 [поток-9] ERROR");
    a += partial.toUtf8() + '\n';
    a += "строка без метки в конце\n";
    const QString pathA = testdocs::writeFile(dir, "table-a.log", a);
    const QString pathB = testdocs::writeFile(dir, "table-b.log", b);

    // Ключевая схема JSON Lines: значения с \", \\, \n и \u-кириллицей.
    const auto fx = keyedfx::make(KeyedDefinition::Format::JsonObject, 64);
    const QString keyedPath = testdocs::writeFile(dir, "table-keyed.jsonl", fx.bytes);
    const QString keyedSchema = keyedfx::schema(KeyedDefinition::Format::JsonObject);
    const QStringList keyedFields = LogPattern(keyedSchema).fieldNames();
    QVector<int> firstLine(fx.records.size(), -1);
    for (int line = 0; line < fx.lineRecord.size(); ++line) {
        const int rec = fx.lineRecord.at(line);
        if (rec >= 0 && firstLine[rec] < 0)
            firstLine[rec] = line;
    }

    struct Case {
        QString name;
        QStringList paths;
        QString schema;
        QStringList fields;
    };
    const QList<Case> cases = {
        {QStringLiteral("regex"), {pathA}, schema, fieldNames},
        {QStringLiteral("merged"), {pathA, pathB}, schema, fieldNames},
        {QStringLiteral("keyed"), {keyedPath}, keyedSchema, keyedFields},
    };
    for (const Case& cs : cases) {
        for (const bool indexedBackend : {false, true}) {
            auto doc = indexedBackend ? testdocs::buildIndexed(cs.paths, cs.schema)
                                      : testdocs::buildResident(cs.paths, cs.schema);
            CHECK(doc.errors.isEmpty(), "table fixture loads");
            LogModel& model = *doc.model;
            model.setAvailableFields(cs.fields);
            const bool merged = cs.paths.size() > 1;
            CHECK(merged == (model.labeledFiles().size() > 1), "merged tab has several files");
            const int message = int(cs.fields.indexOf(QStringLiteral("Message")));
            const int thread = int(cs.fields.indexOf(QStringLiteral("Thread")));
            CHECK(message >= 0 && thread >= 0, "fixture schema has Message and Thread");
            struct Selection { const char* name; bool shown; QVector<int> visible; QVector<int> columns; int messageAt; };
            QVector<int> all;
            for (int i = 0; i < cs.fields.size(); ++i)
                all << i;
            const QList<Selection> selections = {
                {"fields", true, {message, thread}, {message, thread}, 0},
                {"raw", false, {}, all, message},
                {"no-message", true, {thread}, {thread, message}, 1},
            };
            for (const auto& sel : selections) {
                for (const bool filtered : {false, true}) {
                    model.setFieldDisplaySelection(sel.shown, sel.visible);
                    model.setLogLevelFilter(filtered ? QSet<LogLevel>{LogLevel::Error} : QSet<LogLevel>{});
                    CHECK(testdocs::settle(model), "filter settles");
                    const QString label = QStringLiteral("%1 %2 %3%4")
                                              .arg(cs.name, indexedBackend ? "indexed" : "resident",
                                                   QString::fromLatin1(sel.name),
                                                   filtered ? QStringLiteral(" errors") : QString());
                    const ExpectedTable want = expectedTable(model, cs.fields, sel.columns, sel.messageAt, merged);
                    CHECK(!want.rows.isEmpty(), "expected table has rows");
                    const QString csvPath = dir.filePath(QStringLiteral("table.csv"));
                    const QString jsonPath = dir.filePath(QStringLiteral("table.jsonl"));
                    CHECK(runJobAs(model, csvPath, cs.paths, Format::Csv).ok(),
                          qPrintable(label + QStringLiteral(": CSV export succeeds")));
                    CHECK(runJobAs(model, jsonPath, cs.paths, Format::JsonLines).ok(),
                          qPrintable(label + QStringLiteral(": JSON Lines export succeeds")));
                    checkTable(label, csvPath, jsonPath, want);
                    if (cs.name != QLatin1String("keyed")) {
                        bool ok = false, bom = false;
                        const auto csv = parseCsv(read(csvPath), &ok, &bom);
                        const int at = int(want.headers.indexOf(QStringLiteral("Message")));
                        bool found = false;
                        for (const QStringList& rec : csv)
                            found = found || rec.value(at) == partial;
                        CHECK(found, qPrintable(label + QStringLiteral(": a partly matched line keeps its text in Message")));
                    }

                    // Ключевая схема: Message первой строки записи — заданное
                    // значение фикстуры (раскодированное: \", \\, \n, \u).
                    if (cs.name == QLatin1String("keyed") && sel.columns.first() == message) {
                        bool ok = false, bom = false;
                        const auto csv = parseCsv(read(csvPath), &ok, &bom);
                        int checked = 0, bad = 0;
                        for (int row = 0; row < model.rowCount() && row + 1 < csv.size(); ++row) {
                            const int line = model.entryAt(row)->originalLineNumber() - 1;
                            const int rec = fx.lineRecord.value(line, -1);
                            if (rec < 0 || firstLine[rec] != line || fx.records[rec].message.isEmpty())
                                continue;
                            ++checked;
                            if (csv.at(row + 1).value(1) != fx.records[rec].message && bad++ < 3)
                                std::fprintf(stderr, "  keyed line %d: '%s' want '%s'\n", line + 1,
                                             qPrintable(csv.at(row + 1).value(1)),
                                             qPrintable(fx.records[rec].message));
                        }
                        CHECK(checked > 0 && bad == 0,
                              qPrintable(label + QStringLiteral(": keyed Message is the decoded value")));
                    }
                }
            }
            model.setLogLevelFilter({});
            CHECK(testdocs::settle(model), "filter reset settles");

            // Снимок на момент старта: фильтр сразу после старта на файл не влияет.
            model.setFieldDisplaySelection(true, {message, thread});
            model.setLogLevelFilter({LogLevel::Error});
            CHECK(testdocs::settle(model), "filter settles");
            const ExpectedTable want = expectedTable(model, cs.fields, {message, thread}, 0, merged);
            const QString csvPath = dir.filePath(QStringLiteral("snapshot.csv"));
            const QString jsonPath = dir.filePath(QStringLiteral("snapshot.jsonl"));
            for (const Format format : {Format::Csv, Format::JsonLines}) {
                ViewExportJob job;
                bool done = false;
                ViewExport::Result result;
                QObject::connect(&job, &ViewExportJob::finished,
                                 [&](const ViewExport::Result& r) { result = r; done = true; });
                CHECK(job.start(model, format == Format::Csv ? csvPath : jsonPath, cs.paths, format),
                      "table export starts");
                CHECK(job.format() == format, "job reports its format");
                model.setLogLevelFilter({});
                model.setFieldDisplaySelection(false, {});
                CHECK(testdocs::waitFor([&] { return done; }) && result.ok(), "table export finishes");
                CHECK(testdocs::settle(model), "filter reset settles");
                model.setFieldDisplaySelection(true, {message, thread});
                model.setLogLevelFilter({LogLevel::Error});
                CHECK(testdocs::settle(model), "filter settles");
            }
            checkTable(cs.name + QStringLiteral(" snapshot"), csvPath, jsonPath, want);

            // Источник защищён и в табличном формате.
            CHECK(runJobAs(model, cs.paths.first(), cs.paths, Format::Csv).error
                      == ViewExport::Error::SourceFile, "CSV export refuses the source");
        }
    }

    // Отмена CSV: прежний файл назначения не тронут.
    {
        QByteArray big;
        big.reserve(300000 * 60);
        for (int i = 0; i < 300000; ++i)
            big += "2026-03-05 10:00:00.000 [w] INFO - row, " + QByteArray::number(i) + '\n';
        const QString bigPath = testdocs::writeFile(dir, "table-big.log", big);
        auto doc = testdocs::buildIndexed({bigPath}, schema);
        doc.model->setAvailableFields(fieldNames);
        for (const Format format : {Format::Csv, Format::JsonLines}) {
            const QString target = dir.filePath("table-cancel.out");
            put(target, "previous export");
            ViewExportJob job;
            bool done = false;
            ViewExport::Result result;
            QObject::connect(&job, &ViewExportJob::finished,
                             [&](const ViewExport::Result& r) { result = r; done = true; });
            CHECK(job.start(*doc.model, target, {bigPath}, format), "start large table export");
            job.cancel();
            CHECK(testdocs::waitFor([&] { return done; }), "cancelled table export reports finished");
            CHECK(result.error == ViewExport::Error::Cancelled, "cancelled table export reports Cancelled");
            CHECK(read(target) == "previous export", "cancelled table export keeps the destination");
        }
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    CHECK(temporary.isValid(), "temporary directory exists");
    if (!temporary.isValid()) return 1;
    const QDir dir(temporary.path());
    const QByteArray original("original source bytes\r\n");
    const QString source = dir.filePath(QString::fromUtf8("Source_\xD0\xBB\xD0\xBE\xD0\xB3.log"));
    put(source, original);

    const auto assertProtected = [&](const QString& target, const char* label) {
        int calls = 0;
        const auto result = ViewExport::save(target, {source}, 1, [&](int) {
            ++calls;
            return QStringLiteral("must never be written");
        });
        CHECK(result.error == ViewExport::Error::SourceFile, label);
        CHECK(calls == 0, "source rejected before reading export rows");
        CHECK(read(source) == original, "source bytes unchanged");
    };
    assertProtected(source, "exact source path is protected");
    assertProtected(dir.filePath("./" + QFileInfo(source).fileName()), "normalized source path is protected");
    CHECK(dir.mkdir("child"), "create relative-path fixture");
    assertProtected(dir.filePath("child/../" + QFileInfo(source).fileName()), "parent traversal alias is protected");
    assertProtected(QDir::current().relativeFilePath(source), "relative source path is protected");

    const QString hardLink = dir.filePath("hard-link.log");
    std::error_code ec;
    std::filesystem::create_hard_link(nativePath(source), nativePath(hardLink), ec);
    CHECK(!ec, "create hard-link fixture");
    if (!ec) assertProtected(hardLink, "hard-link alias is protected");

#ifdef Q_OS_WIN
    const QString shortcut = dir.filePath("qt-shortcut.lnk");
    CHECK(QFile::link(source, shortcut), "create Qt shortcut fixture");
    assertProtected(shortcut, "Qt filesystem shortcut is protected");
    const auto shortcutSource = ViewExport::save(source, {shortcut}, 1,
        [](int) { return QStringLiteral("must not overwrite shortcut source"); });
    CHECK(shortcutSource.error == ViewExport::Error::SourceFile, "source opened via shortcut is protected");
    CHECK(read(source) == original, "shortcut source bytes unchanged");
#endif

    const QString caseAlias = dir.filePath(QFileInfo(source).fileName().toLower());
    if (std::filesystem::exists(nativePath(caseAlias))) {
        assertProtected(caseAlias, "case alias is protected on insensitive filesystem");
    } else {
        put(caseAlias, "separate file");
        const auto result = ViewExport::save(caseAlias, {source}, 1,
            [](int) { return QStringLiteral("distinct destination"); });
        CHECK(result.ok(), "distinct case-sensitive file remains writable");
        CHECK(read(source) == original, "distinct path export preserves source");
    }

    const QString symlink = dir.filePath("symbolic-link.log");
    std::filesystem::create_symlink(nativePath(source), nativePath(symlink), ec);
    if (!ec) {
        assertProtected(symlink, "symbolic link is protected");
    } else {
#ifdef Q_OS_WIN
        std::fprintf(stdout, "SKIP: symbolic link fixture unavailable: %s\n", ec.message().c_str());
#else
        CHECK(false, "create symbolic-link fixture");
#endif
    }

    // UTF-8, selected display text, empty rows, and a row longer than the
    // stream buffer all survive an atomic replacement of an existing export.
    const QString target = dir.filePath("export.log");
    const QByteArray oldResult("previous export");
    put(target, oldResult);
    const QStringList rows = {QString::fromUtf8("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"),
                              QString(), QString(40000, QLatin1Char('x'))};
    const auto saved = ViewExport::save(target, {source}, rows.size(), [&](int row) {
        CHECK(read(target) == oldResult, "old export is intact until publication");
        return rows.at(row);
    });
    CHECK(saved.ok(), "export publishes successfully");
#ifdef Q_OS_WIN
    const QString newline = QStringLiteral("\r\n");
#else
    const QString newline = QStringLiteral("\n");
#endif
    CHECK(read(target) == (rows.join(newline) + newline).toUtf8(), "exact exported UTF-8 bytes and line endings");
    CHECK(read(source) == original, "normal export preserves source");

    for (bool shortWrite : {false, true}) {
        FailingDevice output(shortWrite);
        const auto failure = ViewExport::writeRows(output, 1,
            [](int) { return QStringLiteral("small buffered row"); });
        CHECK(failure.error == ViewExport::Error::Write, "final flush failure is reported");
        CHECK(failure.detail == QStringLiteral("Injected disk write failure"), "device error detail retained");
    }
    FailingDevice output(false);
    int calls = 0;
    const auto writeFailure = ViewExport::writeRows(output, 10, [&](int) {
        ++calls;
        return QString(40000, QLatin1Char('x'));
    });
    CHECK(writeFailure.error == ViewExport::Error::Write && calls < 10,
          "stop producing rows after a buffered write fails");

    const auto openFailure = ViewExport::save(dir.filePath("missing/output.log"), {source}, 1,
        [](int) { return QStringLiteral("unused"); });
    CHECK(openFailure.error == ViewExport::Error::Open, "open failure is reported");

    // External changes while exporting must not be published over a source.
    const QString changedTarget = dir.filePath("changed.log");
    const auto changed = ViewExport::save(changedTarget, {source}, 1, [&](int) {
        std::filesystem::create_hard_link(nativePath(source), nativePath(changedTarget), ec);
        CHECK(!ec, "create source alias during export");
        return QStringLiteral("discard this export");
    });
    CHECK(changed.error == ViewExport::Error::SourceFile, "recheck source identity before publication");
    CHECK(read(source) == original && read(changedTarget) == original, "cancelled publication preserves both source names");

    // Force publication failure after successful writing without filling the
    // user's disk: a nonempty directory appears where the new file should go.
    const QString blockedTarget = dir.filePath("blocked.log");
    const auto commitFailure = ViewExport::save(blockedTarget, {source}, 1, [&](int) {
        CHECK(dir.mkdir("blocked.log"), "create publication obstacle");
        put(QDir(blockedTarget).filePath("keep"), "external data");
        return QStringLiteral("discard this export too");
    });
    CHECK(commitFailure.error == ViewExport::Error::Commit, "commit failure is reported");
    CHECK(read(QDir(blockedTarget).filePath("keep")) == "external data", "commit failure preserves external data");
    CHECK(!commitFailure.detail.isEmpty(), "commit error detail retained");

    testBackgroundExport(dir);
    testEncodedExport(dir);
    testTableFormatting();
    testTableExport(dir);

    std::fprintf(stdout, "View export: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
