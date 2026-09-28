#include "viewexport.h"

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

    std::fprintf(stdout, "View export: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
