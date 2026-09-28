#include "viewexport.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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

    std::fprintf(stdout, "View export: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
