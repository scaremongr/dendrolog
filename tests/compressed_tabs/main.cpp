// ============================================================================
// compressed_tabs — сжатый источник во вкладке (F-2, шаг 2; docs/design/F-2.md,
// «Вкладка», «Перезагрузка и авто-обновление», «Слияние вкладок», «Что
// изменилось к 0.6.0»). Вкладки — настоящие LogViewWidget, корень временных
// файлов — свой QTemporaryDir (настоящий кэш пользователя не трогается).
//
//   • строки .gz совпадают со строками несжатого близнеца (текст, номера
//     строк) на обоих бэкендах, в одно- и в слитой вкладке; имя вкладки,
//     displayPath и источник — архив, копия — в каталоге спула;
//   • тот же архив дважды во вкладку не попадает;
//   • закрытие посреди распаковки и отмена не оставляют файла; закрытие
//     загруженной вкладки убирает копию;
//   • битый архив — понятная причина, F5 после исправления — загружен;
//   • конвертация вкладки в индексную, пока второй файл распаковывается;
//   • слияние вкладок делит копию: источник закрыт — копия жива, строки те же;
//   • F5 при изменённом архиве — новое содержимое; при пропавшем — строки
//     остаются, сообщение; тик авто-обновления архив не трогает;
//   • идентичность сжатого источника (R-1): закладка → память файла →
//     закрыть → открыть — закладка на месте; сессия с .gz, перенесённая
//     вместе с архивом, — вкладка и закладка на месте; переписанный архив —
//     закладка серая.
// ============================================================================

#include "appsettings.h"
#include "bookmarkstore.h"
#include "documentstate.h"
#include "filememorystore.h"
#include "gzipbytes.h"
#include "logviewwidget.h"
#include "recordlocator.h"
#include "sessionfile.h"
#include "testdocuments.h"
#include "viewexport.h"

#include <QApplication>
#include <QDirIterator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

// Счётчик сигнала (без Qt Test).
struct Spy {
    int count = 0;
    template <typename Sender, typename Signal>
    Spy(Sender* sender, Signal signal)
    { QObject::connect(sender, signal, sender, [this](auto&&...) { ++count; }); }
    int size() const { return count; }
    bool isEmpty() const { return count == 0; }
};

static QString g_spool;
static QString g_schema;

static QByteArray record(int i, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(i / 3600 % 60).rightJustified(2, '0')
         + ':' + QByteArray::number(i / 60 % 60).rightJustified(2, '0') + ':'
         + QByteArray::number(i % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

static QByteArray logBytes(const QByteArray& tag, int lines)
{
    QByteArray bytes;
    for (int i = 0; i < lines; ++i) {
        bytes += record(i, i % 7 == 3 ? "ERROR" : "INFO", tag + ' ' + QByteArray::number(i));
        if (i % 11 == 5)
            bytes += "    continuation of " + QByteArray::number(i) + '\n';
    }
    return bytes;
}

static std::unique_ptr<LogViewWidget> newTab()
{
    auto tab = std::make_unique<LogViewWidget>();
    tab->setParserPattern(g_schema);
    tab->setExtractionEnabled(true);
    tab->setSpoolRoot(g_spool);
    tab->resize(800, 300);
    return tab;
}

static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading() && !tab.isPositionPending(); }, 60000)
        && testdocs::settle(*tab.model());
}

// «номер строки файла: текст» всех видимых строк.
static QStringList numbered(const LogModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << QStringLiteral("%1:%2").arg(model.keyAt(row).line).arg(model.messageAt(row));
    return out;
}

// Обычные файлы под корнем спула (блокировки каталогов не в счёт).
static QStringList spoolFiles()
{
    QStringList out;
    QDirIterator it(g_spool, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (!path.endsWith(QLatin1String(".lock")))
            out << path;
    }
    return out;
}

static bool spoolEmpty()
{
    return testdocs::waitFor([] { return spoolFiles().isEmpty(); }, 10000);
}

static int rowOfLine(const LogModel& model, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.keyAt(row).line == line)
            return row;
    return -1;
}

// ---------------------------------------------------------------------------

static void checkTwin(const QDir& dir, const QString& name)
{
    const QString label = QStringLiteral("twin/%1").arg(name);
    const QByteArray bytes = logBytes("twin", 400);
    const QString plain = testdocs::writeFile(dir, "twin.log", bytes);
    const QString gz = testdocs::writeFile(dir, "twin.log.gz", testgzip::gzip(bytes));
    const QByteArray otherBytes = logBytes("other", 50);
    const QString other = testdocs::writeFile(dir, "other.log", otherBytes);
    const int otherLines = int(otherBytes.count('\n'));

    QStringList expected;
    {
        auto tab = newTab();
        tab->addLogFile(plain);
        CHECK(settled(*tab), label + QStringLiteral(": plain loads"));
        expected = numbered(*tab->model());
    }
    {
        auto tab = newTab();
        tab->addLogFile(gz);
        CHECK(tab->isUnpacking() && tab->isLoading(), label + QStringLiteral(": unpacking is loading"));
        tab->addLogFile(gz); // тот же архив — тот же файл вкладки
        CHECK(settled(*tab), label + QStringLiteral(": gz loads"));
        CHECK(tab->fileCount() == 1, label + QStringLiteral(": one file for one archive"));
        CHECK(numbered(*tab->model()) == expected, label + QStringLiteral(": rows equal the plain twin"));
        const LogFilePtr lf = tab->loadedFiles().value(0);
        CHECK(lf && lf->shortName() == QLatin1String("twin.log.gz"), label + QStringLiteral(": tab name"));
        CHECK(lf && lf->displayPath() == LogViewWidget::normalizedPath(gz),
              label + QStringLiteral(": displayPath is the archive"));
        CHECK(lf && lf->source().kind == LogSource::Kind::Archive && lf->filePath.startsWith(g_spool),
              label + QStringLiteral(": reads the copy in the spool"));
        CHECK(lf && tab->isUnpacked(lf) && lf->fileSize == bytes.size(),
              label + QStringLiteral(": size of the unpacked copy"));
        // Защита экспорта: архив и копия.
        const QStringList paths = ViewExport::protectedPaths(tab->loadedFiles());
        CHECK(paths.contains(lf->filePath) && paths.contains(LogViewWidget::normalizedPath(gz)),
              label + QStringLiteral(": export protects archive and copy"));
        const ViewExport::Result r = ViewExport::save(gz, paths, 1, [](int) { return QStringLiteral("x"); });
        CHECK(r.error == ViewExport::Error::SourceFile, label + QStringLiteral(": export onto the archive refused"));
        CHECK(testgzip::gzip(bytes) == [&] { QFile f(gz); f.open(QIODevice::ReadOnly); return f.readAll(); }(),
              label + QStringLiteral(": archive untouched"));

        // Слитая вкладка: строки .gz и обычного файла.
        tab->addLogFile(other);
        CHECK(settled(*tab), label + QStringLiteral(": merged settles"));
        CHECK(tab->model()->rowCount() == expected.size() + otherLines, label + QStringLiteral(": merged rows"));

        // Авто-обновление (тик) архив не трогает, даже изменившийся.
        const QByteArray changed = logBytes("changed", 10);
        testdocs::writeFile(dir, "twin.log.gz", testgzip::gzip(changed));
        tab->reloadChangedFiles(/*force=*/false);
        CHECK(!tab->isUnpacking() && settled(*tab)
                  && tab->model()->rowCount() == expected.size() + otherLines,
              label + QStringLiteral(": auto-reload skips the archive"));
        // F5 — изменившийся архив распаковывается заново.
        Spy finished(tab.get(), &LogViewWidget::unpackingFinished);
        CHECK(tab->reloadChangedFiles(/*force=*/true), label + QStringLiteral(": F5 reloads"));
        CHECK(settled(*tab) && finished.size() == 1, label + QStringLiteral(": F5 settles"));
        CHECK(tab->model()->rowCount() == int(changed.count('\n')) + otherLines, label + QStringLiteral(": F5 shows the new archive"));
        // Пропал — строки остаются, сообщение.
        Spy message(tab.get(), &LogViewWidget::statusMessage);
        QFile::remove(gz);
        CHECK(!tab->reloadChangedFiles(true) && settled(*tab), label + QStringLiteral(": F5 on missing archive"));
        CHECK(tab->model()->rowCount() == int(changed.count('\n')) + otherLines && message.size() == 1,
              label + QStringLiteral(": rows stay, the user is told"));
    }
    CHECK(spoolEmpty(), label + QStringLiteral(": closed tab removes its copy: ") + spoolFiles().join(QLatin1Char(' ')));
}

static void checkCloseAndCancel(const QDir& dir)
{
    const QString label = QStringLiteral("close");
    QByteArray big;
    big.reserve(24 << 20);
    for (int i = 0; big.size() < (24 << 20); ++i)
        big += record(i, "INFO", "a fairly long line of text to unpack " + QByteArray::number(i * 7919));
    const QString gz = testdocs::writeFile(dir, "big.log.gz", testgzip::gzip(big, 1));
    {
        auto tab = newTab();
        tab->addLogFile(gz);
        CHECK(tab->isUnpacking(), label + QStringLiteral(": unpacking started"));
        testdocs::pump(5);
    }
    CHECK(spoolEmpty(), label + QStringLiteral(": closing mid-unpack leaves no file: ") + spoolFiles().join(QLatin1Char(' ')));

    {
        auto tab = newTab();
        Spy failed(tab.get(), &LogViewWidget::fileParsingFailed);
        tab->addLogFile(gz);
        tab->cancelUnpacking();
        CHECK(testdocs::waitFor([&] { return !tab->isLoading(); }), label + QStringLiteral(": cancel ends loading"));
        CHECK(failed.size() == 1 && tab->model()->rowCount() == 0, label + QStringLiteral(": cancelled file not loaded"));
        CHECK(tab->loadFailureMessage(tab->loadedFiles().value(0)).contains(QLatin1String("cancelled")),
              label + QStringLiteral(": reason — cancelled"));
        CHECK(spoolFiles().isEmpty(), label + QStringLiteral(": cancel leaves no file"));
    }
    CHECK(spoolEmpty(), label + QStringLiteral(": nothing left"));
}

static void checkBroken(const QDir& dir)
{
    const QString label = QStringLiteral("broken");
    const QByteArray bytes = logBytes("fixed", 120);
    QByteArray cut = testgzip::gzip(bytes);
    cut.chop(20);
    const QString gz = testdocs::writeFile(dir, "broken.log.gz", cut);
    auto tab = newTab();
    Spy failed(tab.get(), &LogViewWidget::fileParsingFailed);
    tab->addLogFile(gz);
    CHECK(settled(*tab) && failed.size() == 1, label + QStringLiteral(": failure reported"));
    const QString message = tab->loadFailureMessage(tab->loadedFiles().value(0));
    CHECK(message.contains(QLatin1String("broken.log.gz")) && message.size() > 30,
          label + QStringLiteral(": reason is named: ") + message);
    CHECK(tab->model()->rowCount() == 0, label + QStringLiteral(": no rows"));
    // Авто-обновление не повторяет, F5 после исправления — загружен.
    testdocs::writeFile(dir, "broken.log.gz", testgzip::gzip(bytes));
    tab->reloadChangedFiles(false);
    CHECK(!tab->isUnpacking(), label + QStringLiteral(": tick does not retry"));
    CHECK(tab->reloadChangedFiles(true) && settled(*tab), label + QStringLiteral(": F5 retries"));
    CHECK(tab->model()->rowCount() == bytes.count('\n'), label + QStringLiteral(": loaded after the fix"));
}

static void checkConversionWhileUnpacking(const QDir& dir)
{
    const QString label = QStringLiteral("conversion");
    AppSettings::instance().setIndexedThresholdMB(1);
    QByteArray large;
    for (int i = 0; large.size() < (3 << 20); ++i)
        large += record(i, "INFO", "large " + QByteArray::number(i));
    const QString plain = testdocs::writeFile(dir, "large.log", large);
    const QByteArray small = logBytes("small", 300);
    const QString gz = testdocs::writeFile(dir, "small.log.gz", testgzip::gzip(small));
    {
        // --encoding второго открытия, пока первое распаковывается: выбор —
        // к загрузке копии, без перечитывания недописанной.
        auto first = newTab();
        first->addLogFile(gz);
        first->addLogFile(gz, FileEncoding().withUserChoice(TextDecoder(TextEncoding::Windows1251)));
        CHECK(settled(*first) && first->model()->rowCount() == small.count('\n'),
              label + QStringLiteral(": encoding while unpacking — rows once"));
        const FileEncoding enc = first->fileEncoding(first->loadedFiles().value(0));
        CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Windows1251,
              label + QStringLiteral(": encoding while unpacking — choice kept"));
    }
    auto tab = newTab();
    tab->addLogFile(gz);            // распаковка в фоне
    CHECK(tab->isUnpacking(), label + QStringLiteral(": unpacking"));
    tab->addLogFile(plain);         // выше порога — вкладка индексная
    CHECK(tab->model()->isIndexedBackend(), label + QStringLiteral(": converted"));
    CHECK(settled(*tab), label + QStringLiteral(": settles"));
    const int largeLines = large.count('\n');
    CHECK(tab->model()->rowCount() == largeLines + small.count('\n'),
          label + QStringLiteral(": both files loaded once: %1").arg(tab->model()->rowCount()));
    AppSettings::instance().setIndexedThresholdMB(512);
}

static void checkMerge(const QDir& dir)
{
    const QString label = QStringLiteral("merge");
    const QByteArray bytes = logBytes("merge", 200);
    const QString gz = testdocs::writeFile(dir, "merge.log.gz", testgzip::gzip(bytes));
    const QByteArray plainBytes = logBytes("plain", 40);
    const QString plain = testdocs::writeFile(dir, "plain.log", plainBytes);
    auto source = newTab();
    source->addLogFile(gz);
    CHECK(settled(*source), label + QStringLiteral(": source loads"));
    const QString copy = source->loadedFiles().value(0)->filePath;
    const QStringList expected = numbered(*source->model());
    auto target = newTab();
    target->addLogFile(plain);
    CHECK(settled(*target), label + QStringLiteral(": target loads"));
    Spy unpacking(target.get(), &LogViewWidget::unpackingStarted);
    target->addFilesFrom(*source);
    source.reset();
    CHECK(settled(*target), label + QStringLiteral(": merged settles"));
    CHECK(unpacking.isEmpty(), label + QStringLiteral(": copy shared, not unpacked again"));
    CHECK(QFileInfo::exists(copy), label + QStringLiteral(": copy survives the closed source"));
    CHECK(target->fileCount() == 2 && target->model()->rowCount() == expected.size() + int(plainBytes.count('\n')),
          label + QStringLiteral(": rows of both files"));
    target->addLogFile(gz);
    CHECK(target->fileCount() == 2, label + QStringLiteral(": archive not added twice"));
    target.reset();
    CHECK(spoolEmpty(), label + QStringLiteral(": copy removed with the last tab"));
}

// Закладка на .gz → память файла → закрыть → открыть.
static void checkMemory(const QDir& dir, const QString& name)
{
    const QString label = QStringLiteral("memory/%1").arg(name);
    QDir work(dir.filePath(QStringLiteral("memory-") + name));
    QDir().mkpath(work.path());
    FileMemoryStore store(work.filePath(QStringLiteral("memory")));
    const QByteArray bytes = logBytes("memo", 150);
    const QString gz = testdocs::writeFile(work, "memo.log.gz", testgzip::gzip(bytes));
    const auto open = [&] {
        auto tab = newTab();
        tab->setFileMemory(&store);
        tab->show();
        tab->addLogFile(gz);
        CHECK(settled(*tab), label + QStringLiteral(": loads"));
        return tab;
    };
    {
        auto tab = open();
        LogModel& model = *tab->model();
        tab->bookmarks().add(rowOfLine(model, 20), QStringLiteral("twenty"));
        tab->view()->goToRow(rowOfLine(model, 60));
        tab->saveFileMemory();
        store.waitForWrites();
        const TabState state = tab->captureState();
        CHECK(state.sources.size() == 1 && state.sources[0].source.compressed
                  && SavedPath::same(state.sources[0].source.path, LogViewWidget::normalizedPath(gz)),
              label + QStringLiteral(": saved source is the archive"));
    }
    {
        auto tab = open();
        LogModel& model = *tab->model();
        const auto list = tab->bookmarks().bookmarks();
        CHECK(list.size() == 1 && list[0].note == QLatin1String("twenty")
                  && tab->bookmarks().visibleRow(list[0].id) == rowOfLine(model, 20),
              label + QStringLiteral(": bookmark on its line"));
        CHECK(tab->view()->userSelectedRow() == rowOfLine(model, 60), label + QStringLiteral(": position restored"));
    }
    // Тот же лог, сжатый заново (другие байты архива, то же содержимое), — та же память.
    testdocs::writeFile(work, "memo.log.gz", testgzip::gzip(bytes, 1));
    {
        auto tab = open();
        CHECK(tab->bookmarks().visibleRows().size() == 1, label + QStringLiteral(": recompressed — same content"));
    }
    // Переписанный архив — закладка серая.
    const QByteArray other = logBytes("other", 90);
    testdocs::writeFile(work, "memo.log.gz", testgzip::gzip(other));
    {
        auto tab = open();
        const auto list = tab->bookmarks().bookmarks();
        CHECK(tab->bookmarks().visibleRows().isEmpty(), label + QStringLiteral(": rewritten — no bookmark on rows"));
        for (const Bookmark& b : list)
            CHECK(tab->bookmarks().status(b.id) == BookmarkStore::Status::Missing,
                  label + QStringLiteral(": rewritten — bookmark greyed"));
        CHECK(tab->model()->rowCount() == other.count('\n'), label + QStringLiteral(": rewritten — new rows"));
        // Прежний архив вернули, F5 — серая закладка проверяется снова и находится.
        testdocs::writeFile(work, "memo.log.gz", testgzip::gzip(bytes));
        CHECK(tab->reloadChangedFiles(true) && settled(*tab), label + QStringLiteral(": F5 with the old archive"));
        CHECK(tab->bookmarks().visibleRows().size() == 1
                  && tab->bookmarks().visibleRows().value(0) == rowOfLine(*tab->model(), 20),
              label + QStringLiteral(": grey bookmark found again after F5"));
        // Снимок посреди перераспаковки — с идентичностью прежнего содержимого.
        testdocs::writeFile(work, "memo.log.gz", testgzip::gzip(bytes, 1));
        CHECK(tab->reloadChangedFiles(true) && tab->isUnpacking(), label + QStringLiteral(": F5 unpacks again"));
        const TabState during = tab->captureState();
        CHECK(during.sources.size() == 1 && during.sources[0].source.compressed
                  && during.sources[0].source.size == bytes.size(),
              label + QStringLiteral(": state during re-unpack keeps the source"));
        CHECK(settled(*tab) && tab->bookmarks().visibleRows().size() == 1,
              label + QStringLiteral(": same content after re-unpack — bookmark stays"));
    }
}

// Сессия с .gz, перенесённая вместе с архивом.
static void checkSession(const QDir& dir, const QString& name)
{
    const QString label = QStringLiteral("session/%1").arg(name);
    const QString from = dir.filePath(QStringLiteral("session-a-") + name);
    const QString to = dir.filePath(QStringLiteral("session-b-") + name);
    QDir().mkpath(from + QStringLiteral("/logs"));
    const QByteArray bytes = logBytes("sess", 180);
    const QString gz = testdocs::writeFile(QDir(from + QStringLiteral("/logs")), "sess.log.gz",
                                           testgzip::gzip(bytes));
    const QString sessionPath = from + QStringLiteral("/work.dlsession");
    {
        auto tab = newTab();
        tab->show();
        tab->addLogFile(gz);
        CHECK(settled(*tab), label + QStringLiteral(": loads"));
        tab->bookmarks().add(rowOfLine(*tab->model(), 33), QStringLiteral("note"));
        SessionState state;
        state.tabs.append(tab->captureState());
        state.activeTab = 0;
        QString error;
        CHECK(SessionFile::save(sessionPath, state, &error), label + QStringLiteral(": save ") + error);
    }
    CHECK(QDir().rename(from, to), label + QStringLiteral(": move the folder"));
    // По прежнему абсолютному пути теперь другой архив (ротация): он не тот.
    QDir().mkpath(from + QStringLiteral("/logs"));
    testdocs::writeFile(QDir(from + QStringLiteral("/logs")), "sess.log.gz", testgzip::gzip(logBytes("rotated", 30)));
    SessionFile::LoadResult loaded = SessionFile::load(to + QStringLiteral("/work.dlsession"));
    CHECK(loaded.state.has_value(), label + QStringLiteral(": load ") + loaded.error);
    if (!loaded.state)
        return;
    const auto resolutions = SessionFile::rebind(*loaded.state, to + QStringLiteral("/work.dlsession"));
    CHECK(resolutions.size() == 1 && resolutions[0].kind == SessionFile::Resolution::Kind::Relocated,
          label + QStringLiteral(": archive found next to the session"));
    auto tab = newTab();
    tab->show();
    const auto report = tab->applyState(loaded.state->tabs.value(0));
    CHECK(report.missingSources.isEmpty() && report.changedSources.isEmpty(), label + QStringLiteral(": nothing missing"));
    CHECK(settled(*tab), label + QStringLiteral(": restored tab loads"));
    CHECK(tab->model()->rowCount() == bytes.count('\n'), label + QStringLiteral(": rows"));
    const auto list = tab->bookmarks().bookmarks();
    CHECK(list.size() == 1 && tab->bookmarks().visibleRow(list.value(0).id) == rowOfLine(*tab->model(), 33),
          label + QStringLiteral(": bookmark on its line"));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    g_spool = QDir::cleanPath(dir.filePath(QStringLiteral("spool")));
    g_schema = testdocs::buildSchema();
    AppSettings& settings = AppSettings::instance();
    settings.setRememberFileState(true);

    for (const int threshold : { 512, 0 }) {
        settings.setIndexedThresholdMB(threshold);
        const QString name = threshold ? QStringLiteral("resident") : QStringLiteral("indexed");
        QDir work(dir.filePath(name));
        QDir().mkpath(work.path());
        checkTwin(work, name);
        checkBroken(work);
        checkMerge(work);
        checkMemory(work, name);
        checkSession(work, name);
    }
    settings.setIndexedThresholdMB(512);
    checkCloseAndCancel(dir);
    checkConversionWhileUnpacking(dir);

    std::fprintf(stdout, "compressed tabs: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
