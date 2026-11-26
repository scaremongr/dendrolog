// ============================================================================
// scrollbar_marks — отметки на полосе прокрутки (R-6, docs/design/R-6.md):
//   • ядро: прорежённый набор строк → пиксели дорожки совпадает с перебором
//     всех строк (синтетический снапшот в 3 млн строк, uniform-проекция,
//     несколько длин дорожки, натуральный и минимальный ползунок, сжатая шкала);
//     до kFineBuckets строк набор точный построчно; отмена;
//   • view на обоих бэкендах (одно- и многофайловая вкладка, многострочные
//     записи, фильтр, перенос строк): пиксели отметок по категориям против
//     независимого перебора видимых строк (entryAt), а строка, поставленная
//     наверх вида, — её отметка внутри ползунка;
//   • вкладка: обновление после фильтра, дозаписи, полной перезагрузки,
//     добавления файла, смены размера, постановки и снятия закладки;
//   • поиск: выдача панели Search на вкладке-источнике (ключи строк выдачи →
//     строки вкладки), рост выдачи на дозаписи, снятие поиска, чужая вкладка;
//     слитая и одно-файловая неупорядоченная вкладка, поиск по отфильтрованной;
//     отставшая выдача (панель скрыта) отметок не даёт; снятие поиска посреди
//     первого расчёта; дозапись с активным поиском отметки не гасит;
//   • reset посреди расчёта — результат отброшен; поток батчей — запусков
//     расчёта единицы, а не по батчу; скрытый view не считает.
// Режим замера: scrollbar_marks --bench <file> — вкладка на файле, расчёт
// отметок, прокрутка; самое долгое событие GUI-потока.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "bookmarkstore.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "scrollbarmarks.h"
#include "scrollbarmarksoverlay.h"
#include "searchresultscontroller.h"
#include "testdocuments.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

using Category = ScrollbarMarkSet::Category;
static const char* const kCategoryNames[] = { "error", "warning", "search", "bookmark" };

// Самое долгое событие GUI-потока (режим замера и проверка «нет фризов»).
class TimingApplication : public QApplication {
public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!timing)
            return QApplication::notify(receiver, event);
        QElapsedTimer t;
        t.start();
        const bool result = QApplication::notify(receiver, event);
        const qint64 ms = t.elapsed();
        if (ms > maxMs) {
            maxMs = ms;
            maxWhat = QString::fromLatin1(receiver->metaObject()->className())
                + QLatin1Char('/') + QString::number(int(event->type()));
        }
        if (ms > 100)
            ++slowEvents;
        return result;
    }
    bool timing = false;
    qint64 maxMs = 0;
    int slowEvents = 0;
    QString maxWhat;
};

// ---------------------------------------------------------------------------
// Ядро на синтетическом снапшоте
// ---------------------------------------------------------------------------

struct SyntheticRows {
    std::vector<quint8> level;    // LogLevel
    std::vector<int> logical;
    LogFilePtr file = std::make_shared<LogFile>(QStringLiteral("synthetic.log"));
};

class SyntheticImpl final : public LogScanSnapshot::Impl {
public:
    // rows — номера строк data, которые показывает снапшот (пусто — все).
    SyntheticImpl(std::shared_ptr<const SyntheticRows> data, std::vector<qint64> rows = {})
        : m_data(std::move(data)), m_rows(std::move(rows)) {}

    qint64 rowCount() const override
    {
        return m_rows.empty() ? qint64(m_data->level.size()) : qint64(m_rows.size());
    }
    qint64 base(qint64 row) const { return m_rows.empty() ? row : m_rows[size_t(row)]; }
    LogEntryMeta metaAt(qint64 row) const override
    {
        LogEntryMeta m;
        if (row < 0 || row >= rowCount())
            return m;
        const qint64 b = base(row);
        m.level = LogLevel(m_data->level[size_t(b)]);
        m.logicalEntryId = m_data->logical[size_t(b)];
        m.sourceFile = m_data->file.get();
        return m;
    }
    LogLevel levelAt(qint64 row) const override
    {
        return row < 0 || row >= rowCount() ? LogLevel::Unknown
                                            : LogLevel(m_data->level[size_t(base(row))]);
    }
    qint64 lineAt(qint64 row, const LogFile** file) const override
    {
        if (row < 0 || row >= rowCount()) {
            *file = nullptr;
            return 0;
        }
        *file = m_data->file.get();
        return base(row) + 1;
    }
    void forEachMeta(qint64, const std::function<bool(qint64, const LogEntryMeta&)>&) const override {}
    void forEachLine(qint64, const std::function<bool(qint64, const LogEntryMeta&, QStringView)>&) const override {}
    void forEachLineBackward(qint64, const std::function<bool(qint64, const LogEntryMeta&, QStringView)>&) const override {}
    QString textAt(qint64) const override { return QString(); }
    std::unique_ptr<LogScanSnapshot::LineCursor> lineCursor(qint64) const override { return nullptr; }
    LogFilePtr sourceFilePtrAt(qint64) const override { return m_data->file; }
    LogEntryFields fieldsAt(qint64, const QString&) const override { return LogEntryFields(); }

private:
    std::shared_ptr<const SyntheticRows> m_data;
    std::vector<qint64> m_rows;
};

static std::shared_ptr<SyntheticRows> makeSynthetic(qint64 n, quint32 seed)
{
    auto data = std::make_shared<SyntheticRows>();
    data->level.resize(size_t(n));
    data->logical.resize(size_t(n));
    QRandomGenerator rng(seed);
    int record = -1;
    LogLevel level = LogLevel::Info;
    for (qint64 i = 0; i < n; ++i) {
        // Плотный участок: сплошные ошибки (много отметок в одной корзине).
        const bool dense = i >= n / 3 && i < n / 3 + n / 50;
        if (i == 0 || rng.bounded(100) < 70 || dense) {
            ++record;
            const quint32 roll = rng.bounded(1000);
            level = dense ? LogLevel::Error
                : roll < 20 ? LogLevel::Error
                : roll < 25 ? LogLevel::Fatal
                : roll < 75 ? LogLevel::Warn
                : LogLevel::Info;
        }
        data->level[size_t(i)] = quint8(level);
        data->logical[size_t(i)] = record;
    }
    return data;
}

// Перебор: первая строка записи нужного уровня.
static QVector<qint64> bruteLevelRows(const LogScanSnapshot& s, bool errors)
{
    QVector<qint64> out;
    for (qint64 r = 0; r < s.rowCount(); ++r) {
        const LogLevel l = s.levelAt(r);
        const bool hit = errors ? (l == LogLevel::Error || l == LogLevel::Fatal) : l == LogLevel::Warn;
        if (!hit)
            continue;
        if (r > 0 && s.metaAt(r - 1).logicalEntryId == s.metaAt(r).logicalEntryId)
            continue;
        out.append(r);
    }
    return out;
}

static QVector<int> pixelsOf(const QVector<qint64>& rows, double perRow, const ScrollbarTrack& t)
{
    QVector<int> out;
    for (qint64 r : rows) {
        const int px = ScrollbarMarks::trackPosition(double(r) * perRow, t);
        if (out.isEmpty() || out.constLast() != px)
            out.append(px);
    }
    return out;
}

static void checkCore()
{
    // Точно построчно до kFineBuckets строк.
    {
        const auto data = makeSynthetic(50000, 7);
        const LogScanSnapshot s(std::make_shared<SyntheticImpl>(data));
        const ScrollbarMarkSet set = ScrollbarMarks::compute(s, LogScanSnapshot());
        CHECK(set.rowCount == 50000, "row count");
        CHECK(set.rows[Category::Error] == bruteLevelRows(s, true), "small: error rows exact");
        CHECK(set.rows[Category::Warning] == bruteLevelRows(s, false), "small: warning rows exact");
        CHECK(set.rows[Category::Search].isEmpty() && set.rows[Category::Bookmark].isEmpty(),
              "small: no search, no bookmarks");
    }

    // Выдача с несколькими строками, которых вкладка не показывает: они
    // пропускаются, отметки остальных — на своих строках вкладки.
    {
        const auto data = makeSynthetic(20000, 5);
        std::vector<qint64> visible;
        for (qint64 r = 0; r < 20000; ++r)
            if (r % 1000 != 500)
                visible.push_back(r);
        const LogScanSnapshot tab(std::make_shared<SyntheticImpl>(data, visible));
        std::vector<qint64> hits;
        QVector<qint64> want;
        // Каждая 50-я строка; спрятанные (…500) — только первые четыре.
        for (qint64 b = 0; b < 20000; b += 50) {
            const bool hidden = b % 1000 == 500;
            if (hidden && b > 3500)
                continue;
            hits.push_back(b);
            if (!hidden)
                want.append(qint64(std::lower_bound(visible.begin(), visible.end(), b) - visible.begin()));
        }
        const LogScanSnapshot stale(std::make_shared<SyntheticImpl>(data, hits));
        const ScrollbarMarkSet set = ScrollbarMarks::compute(tab, stale);
        CHECK(set.rows[Category::Search] == want,
              QStringLiteral("hidden result rows are skipped: %1 search rows, expected %2")
                  .arg(set.rows[Category::Search].size()).arg(want.size()));
    }

    const qint64 n = 3000000;
    const auto data = makeSynthetic(n, 11);
    const LogScanSnapshot s(std::make_shared<SyntheticImpl>(data));
    // Выдача поиска — каждая 7-я строка и плотный участок подряд.
    std::vector<qint64> hits;
    for (qint64 r = 0; r < n; ++r)
        if (r % 7 == 0 || (r >= n / 2 && r < n / 2 + 40000))
            hits.push_back(r);
    QVector<qint64> bruteSearch;
    for (qint64 r : hits)
        bruteSearch.append(r);
    const LogScanSnapshot search(std::make_shared<SyntheticImpl>(data, hits));

    QElapsedTimer timer;
    timer.start();
    const ScrollbarMarkSet set = ScrollbarMarks::compute(s, search);
    std::printf("core: %lld rows, %lld search rows: %lld ms; kept %lld/%lld/%lld rows\n",
                qint64(n), qint64(hits.size()), qint64(timer.elapsed()),
                qint64(set.rows[Category::Error].size()), qint64(set.rows[Category::Warning].size()),
                qint64(set.rows[Category::Search].size()));
    const QVector<qint64> bruteErrors = bruteLevelRows(s, true);
    const QVector<qint64> bruteWarnings = bruteLevelRows(s, false);
    CHECK(set.rows[Category::Error].size() <= 2 * ScrollbarMarks::kFineBuckets
              && set.rows[Category::Search].size() <= 2 * ScrollbarMarks::kFineBuckets,
          "thinned to at most two rows per bucket");
    const auto subset = [](const QVector<qint64>& part, const QVector<qint64>& all) {
        return std::includes(all.begin(), all.end(), part.begin(), part.end());
    };
    CHECK(subset(set.rows[Category::Error], bruteErrors), "kept error rows are error rows");
    CHECK(subset(set.rows[Category::Search], bruteSearch), "kept search rows are search rows");

    // Попиксельно: uniform-проекция (perRow — шкала скроллбара на строку:
    // 18 px, либо сжатая шкала огромного лога).
    for (double perRow : { 18.0, 0.37 }) {
        for (int groove : { 250, 977, 3000 }) {
            for (bool minimalSlider : { false, true }) {
                const int page = 700;
                ScrollbarTrack t;
                t.maximum = int(double(n) * perRow) - page;
                t.pageStep = page;
                t.grooveLength = groove;
                t.sliderLength = minimalSlider ? 24
                    : qMax(1, int(qint64(groove) * page / (qint64(t.maximum) + page)));
                const QString where = QStringLiteral("perRow %1, groove %2, slider %3")
                                          .arg(perRow).arg(groove).arg(t.sliderLength);
                CHECK(pixelsOf(set.rows[Category::Error], perRow, t) == pixelsOf(bruteErrors, perRow, t),
                      "error pixels equal brute force: " + where);
                CHECK(pixelsOf(set.rows[Category::Warning], perRow, t) == pixelsOf(bruteWarnings, perRow, t),
                      "warning pixels equal brute force: " + where);
                CHECK(pixelsOf(set.rows[Category::Search], perRow, t) == pixelsOf(bruteSearch, perRow, t),
                      "search pixels equal brute force: " + where);
            }
        }
    }

    // Проекция: верх ползунка при value в диапазоне, последний пиксель — край.
    {
        ScrollbarTrack t{1000, 100, 200, 20};
        CHECK(ScrollbarMarks::trackPosition(0, t) == 0, "track: top");
        CHECK(ScrollbarMarks::trackPosition(1000, t) == 180, "track: max is slider top at the end");
        CHECK(ScrollbarMarks::trackPosition(1099, t) == 197, "track: last page continues the proportion");
        CHECK(ScrollbarMarks::trackPosition(5000, t) == 199, "track: clamped to the groove");
        CHECK(ScrollbarMarks::trackPosition(10, ScrollbarTrack{1000, 100, 0, 0}) == -1, "track: no groove");
    }

    // Отмена.
    {
        std::atomic_bool cancel{true};
        bool cancelled = false;
        const ScrollbarMarkSet none = ScrollbarMarks::compute(s, search, &cancel, &cancelled);
        CHECK(cancelled && none.isEmpty(), "cancelled compute returns nothing");
    }
}

// ---------------------------------------------------------------------------
// View: отметки против перебора видимых строк
// ---------------------------------------------------------------------------

static QVector<int> bruteViewLevelRows(const LogModel& model, bool errors)
{
    QVector<int> out;
    std::shared_ptr<LogEntry> previous;
    for (int r = 0; r < model.rowCount(); ++r) {
        const std::shared_ptr<LogEntry> e = model.entryAt(r);
        const LogLevel l = e ? e->level() : LogLevel::Unknown;
        const bool hit = errors ? (l == LogLevel::Error || l == LogLevel::Fatal) : l == LogLevel::Warn;
        const bool continuation = previous && e
            && previous->logicalEntryId() == e->logicalEntryId()
            && previous->sourceFile() == e->sourceFile();
        if (hit && !continuation)
            out.append(r);
        previous = e;
    }
    return out;
}

static QVector<int> viewPixels(const LogListView& view, const QVector<int>& rows)
{
    const ScrollbarTrack t = view.scrollbarMarksOverlay()->track();
    QVector<int> out;
    for (int r : rows) {
        const int px = ScrollbarMarks::trackPosition(view.markScrollValue(r), t);
        if (out.isEmpty() || out.constLast() != px)
            out.append(px);
    }
    return out;
}

static bool marksReady(LogListView& view)
{
    auto* model = qobject_cast<LogModel*>(view.model());
    const bool ok = testdocs::waitFor([&] {
        return model && !model->isFiltering() && view.scrollbarMarks()->isUpToDate();
    });
    testdocs::pump(20); // отложенные пересчёты высот view
    return ok && testdocs::waitFor([&] { return view.scrollbarMarks()->isUpToDate(); });
}

// Сверка всех категорий; searchRows — ожидаемые строки поиска (или пусто).
static void compareView(LogListView& view, const QVector<int>& searchRows, const QString& label)
{
    CHECK(marksReady(view), label + ": marks computed");
    auto* model = qobject_cast<LogModel*>(view.model());
    CHECK(view.verticalScrollBar()->isVisible(), label + ": the scrollbar is shown");
    const ScrollbarMarksOverlay* overlay = view.scrollbarMarksOverlay();
    CHECK(overlay->geometry() == view.verticalScrollBar()->rect(), label + ": overlay covers the scrollbar");
    QVector<int> expected[ScrollbarMarkSet::CategoryCount];
    expected[Category::Error] = bruteViewLevelRows(*model, true);
    expected[Category::Warning] = bruteViewLevelRows(*model, false);
    expected[Category::Search] = searchRows;
    expected[Category::Bookmark] = model->bookmarks().visibleRows();
    for (int c = 0; c < ScrollbarMarkSet::CategoryCount; ++c) {
        const QVector<int> want = viewPixels(view, expected[c]);
        const QVector<int>& got = overlay->pixels(Category(c));
        CHECK(got == want, QStringLiteral("%1: %2 marks: %3 px, expected %4 px (%5 rows)")
                               .arg(label, QLatin1String(kCategoryNames[c]))
                               .arg(got.size()).arg(want.size()).arg(expected[c].size()));
    }
}

// Строка наверху вида: её отметка — внутри ползунка.
static void checkInsideSlider(LogListView& view, const QVector<int>& rows, const QString& label)
{
    auto* model = view.model();
    const ScrollbarMarksOverlay* overlay = view.scrollbarMarksOverlay();
    const int step = qMax(1, int(rows.size() / 12));
    int checked = 0;
    for (int i = 0; i < rows.size(); i += step) {
        const int row = rows[i];
        view.scrollTo(model->index(row, 0), QAbstractItemView::PositionAtTop);
        testdocs::pump(30);
        const QRect groove = overlay->grooveRect();
        const QRect slider = overlay->sliderRect();
        const int px = groove.top()
            + ScrollbarMarks::trackPosition(view.markScrollValue(row), overlay->track());
        CHECK(px >= slider.top() && px <= slider.bottom(),
              QStringLiteral("%1: row %2 on screen, its mark %3 is inside the slider [%4, %5]")
                  .arg(label).arg(row).arg(px).arg(slider.top()).arg(slider.bottom()));
        CHECK(overlay->pixels(Category::Error).contains(px - groove.top()),
              QStringLiteral("%1: row %2 is drawn at that pixel").arg(label).arg(row));
        ++checked;
    }
    CHECK(checked > 0, label + ": rows checked against the slider");
}

static QStringList makeFiles(const QDir& dir, const QString& name, int count, int records)
{
    QStringList paths;
    for (int i = 0; i < count; ++i)
        paths << testdocs::writeFile(dir, QStringLiteral("%1-%2.log").arg(name).arg(i),
            testdocs::makeLogBytes(records, 100 + quint32(i), "\n",
                                   testdocs::fixtureBaseTime().addSecs(i * 7), i == 0,
                                   true, false));
    return paths;
}

static void checkViews(const QDir& dir, const QString& schema)
{
    struct Case { bool indexed; int files; bool wrap; };
    const Case cases[] = {
        { false, 1, false }, { false, 2, false }, { false, 1, true }, { false, 2, true },
        { true, 1, false }, { true, 2, false },
    };
    for (const Case& c : cases) {
        const QString label = QStringLiteral("%1, %2 file(s)%3")
                                  .arg(c.indexed ? "indexed" : "resident").arg(c.files)
                                  .arg(c.wrap ? ", wrap" : "");
        const QStringList paths = makeFiles(dir, QStringLiteral("view-%1-%2-%3")
                                                     .arg(c.indexed).arg(c.files).arg(c.wrap),
                                            c.files, 2500);
        testdocs::Document doc = c.indexed ? testdocs::buildIndexed(paths, schema)
                                           : testdocs::buildResident(paths, schema);
        CHECK(doc.errors.isEmpty(), label + ": fixture loads");
        LogListView view;
        // Узкий view с переносом — строки в 2–3 экранные строки, высоты разные.
        view.resize(c.wrap ? 320 : 900, 500);
        view.setModel(doc.model.get());
        view.setWordWrap(c.wrap);
        view.show();
        compareView(view, {}, label);
        CHECK(!view.scrollbarMarksOverlay()->pixels(Category::Error).isEmpty()
                  && !view.scrollbarMarksOverlay()->pixels(Category::Warning).isEmpty(),
              label + ": the fixture has error and warning marks");
        checkInsideSlider(view, bruteViewLevelRows(*doc.model, true), label);

        // Фильтр уровней: отметки — по ВИДИМЫМ строкам.
        doc.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Fatal, LogLevel::Info });
        testdocs::settle(*doc.model);
        compareView(view, {}, label + ", level filter");
        CHECK(view.scrollbarMarksOverlay()->pixels(Category::Warning).isEmpty(),
              label + ": warnings filtered out leave no marks");
        checkInsideSlider(view, bruteViewLevelRows(*doc.model, true), label + ", level filter");
        doc.model->setLogLevelFilter({});
        testdocs::settle(*doc.model);
        compareView(view, {}, label + ", filter off");
    }
}

// ---------------------------------------------------------------------------
// Вкладка: обновление по событиям
// ---------------------------------------------------------------------------

static bool settledTab(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); })
        && testdocs::settle(*tab.model());
}

static void checkTabUpdates(const QDir& dir, const QString& schema, bool indexed)
{
    const QString label = indexed ? QStringLiteral("tab, indexed") : QStringLiteral("tab, resident");
    AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
    const QStringList paths = makeFiles(dir, QStringLiteral("tab-%1").arg(indexed), 2, 1500);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.resize(900, 600);
    tab.show();
    tab.addLogFile(paths[0]);
    CHECK(settledTab(tab), label + ": loaded");
    LogListView& view = *tab.view();
    LogModel& model = *tab.model();
    CHECK(model.isIndexedBackend() == indexed, label + ": backend");
    compareView(view, {}, label + ": opened");

    // Дозапись хвоста: новая ошибка в конце.
    const int before = view.scrollbarMarks()->launchCount();
    QByteArray tail;
    for (int i = 0; i < 40; ++i)
        tail += "2030-01-01 00:00:0" + QByteArray::number(i % 10) + ".000 [main] ERROR - appended failure "
            + QByteArray::number(i) + "\n";
    CHECK(testdocs::appendToFile(paths[0], tail), label + ": append");
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settledTab(tab), label + ": tail settles");
    compareView(view, {}, label + ": after tail");
    CHECK(view.scrollbarMarks()->launchCount() > before, label + ": tail recomputes the marks");
    const QVector<int> errors = bruteViewLevelRows(model, true);
    CHECK(!errors.isEmpty() && errors.last() > model.rowCount() - 5, label + ": the appended error is marked");

    // Закладки: сразу, без нового прохода по строкам.
    const int launches = view.scrollbarMarks()->launchCount();
    const int id = tab.bookmarks().add(model.rowCount() / 2);
    CHECK(id >= 0, label + ": bookmark added");
    CHECK(view.scrollbarMarks()->marks().rows[Category::Bookmark]
              == QVector<qint64>{ qint64(model.rowCount() / 2) },
          label + ": bookmark mark is published at once");
    compareView(view, {}, label + ": bookmark");
    CHECK(view.scrollbarMarksOverlay()->pixels(Category::Bookmark).size() == 1, label + ": one bookmark mark");
    tab.bookmarks().remove(id);
    CHECK(view.scrollbarMarks()->marks().rows[Category::Bookmark].isEmpty(), label + ": bookmark removed");
    compareView(view, {}, label + ": bookmark removed");
    CHECK(view.scrollbarMarks()->launchCount() == launches, label + ": bookmarks need no scan");

    // Смена размера: те же строки, другие пиксели.
    const QVector<int> wide = view.scrollbarMarksOverlay()->pixels(Category::Error);
    tab.resize(900, 360);
    testdocs::pump(200);
    compareView(view, {}, label + ": resized");
    CHECK(view.scrollbarMarksOverlay()->pixels(Category::Error) != wide, label + ": resize moves the marks");

    // Фильтр.
    model.setLogLevelFilter({ LogLevel::Warn, LogLevel::Info });
    testdocs::settle(model);
    compareView(view, {}, label + ": filter");
    CHECK(view.scrollbarMarksOverlay()->pixels(Category::Error).isEmpty(), label + ": no errors when filtered out");
    model.setLogLevelFilter({});
    testdocs::settle(model);

    // Добавление файла.
    tab.addLogFile(paths[1]);
    CHECK(settledTab(tab), label + ": second file loads");
    compareView(view, {}, label + ": file added");

    // Полная перезагрузка: файл переписан короче, ошибки только в начале.
    QByteArray rewritten;
    for (int i = 0; i < 300; ++i)
        rewritten += "2026-03-05 10:00:00.000 [main] " + QByteArray(i < 10 ? "ERROR" : "INFO")
            + " - rewritten " + QByteArray::number(i) + "\n";
    {
        QFile f(paths[0]);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate), label + ": rewrite");
        f.write(rewritten);
    }
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settledTab(tab), label + ": reload settles");
    compareView(view, {}, label + ": full reload");
    AppSettings::instance().setIndexedThresholdMB(512);
}

// ---------------------------------------------------------------------------
// Поиск
// ---------------------------------------------------------------------------

static QVector<int> searchRowsInSource(const LogModel& results, const LogModel& source)
{
    QVector<int> rows;
    for (int j = 0; j < results.rowCount(); ++j)
        rows.append(source.rowForKey(results.keyAt(j)));
    std::sort(rows.begin(), rows.end());
    return rows;
}

// files = 1: файл не упорядочен по времени (makeFiles), резидентная вкладка
// его сортирует, индексная — нет (row == line); выдача должна совпасть с
// вкладкой в обоих случаях. sourceFilter — поиск по отфильтрованной вкладке
// (у индексной одно-файловой — поисковая база с явным списком строк).
static void checkSearch(const QDir& dir, const QString& schema, bool indexed, int files = 2,
                        bool sourceFilter = false)
{
    const QString label = QStringLiteral("search, %1, %2 file(s)%3")
                              .arg(indexed ? "indexed" : "resident").arg(files)
                              .arg(sourceFilter ? ", filtered source" : "");
    const QStringList paths = makeFiles(dir, QStringLiteral("search-%1-%2-%3")
                                                 .arg(indexed).arg(files).arg(sourceFilter),
                                        files, 1500);
    testdocs::Document doc = indexed ? testdocs::buildIndexed(paths, schema)
                                     : testdocs::buildResident(paths, schema);
    testdocs::Document other = testdocs::buildResident({ paths[0] }, schema);
    LogListView view;
    view.resize(900, 500);
    view.setModel(doc.model.get());
    LogListView otherView;
    otherView.resize(900, 500);
    otherView.setModel(other.model.get());
    SearchResultsController search;
    view.scrollbarMarks()->setSearch(&search);
    otherView.scrollbarMarks()->setSearch(&search);
    view.show();
    otherView.show();
    if (sourceFilter) {
        doc.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Warn, LogLevel::Info });
        testdocs::settle(*doc.model);
    }
    compareView(view, {}, label + ": before search");

    search.setSource(doc.model.get());
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = QStringLiteral("timeout");
    rules.rules = { rule };
    search.search(rules);
    CHECK(testdocs::settle(*search.model()), label + ": results settle");
    const QVector<int> found = searchRowsInSource(*search.model(), *doc.model);
    CHECK(!found.isEmpty() && !found.contains(-1), label + ": results map to source rows");
    compareView(view, found, label + ": results");
    CHECK(!view.scrollbarMarksOverlay()->pixels(Category::Search).isEmpty(), label + ": search marks shown");
    compareView(otherView, {}, label + ": another tab has no search marks");

    // Фильтр источника — выдача пересчитывается, отметки за ней.
    doc.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Warn });
    if (sourceFilter) {
        search.clear();
        return;
    }
    // Ждать состояния, а не таймера: фильтр вкладки применён (выдача
    // отстала), затем отложенный полный поиск выдачи закончен.
    testdocs::settle(*doc.model);
    CHECK(testdocs::waitFor([&] { return !search.isStale() && !search.model()->isFiltering(); }),
          label + ": results follow the filter");
    compareView(view, searchRowsInSource(*search.model(), *doc.model), label + ": filtered results");
    doc.model->setLogLevelFilter({});
    testdocs::settle(*doc.model);
    CHECK(testdocs::waitFor([&] { return !search.isStale() && !search.model()->isFiltering(); }),
          label + ": results back");

    // Панель Search скрыта (не live): фильтр вкладки прячет часть строк
    // выдачи, а полный поиск отложен до показа панели. Выдача отстала от
    // вкладки — отметок поиска нет (а не отметки только до первой спрятанной
    // строки выдачи); панель показана — отметки по свежей выдаче.
    search.setLive(false);
    doc.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Fatal });
    testdocs::settle(*doc.model);
    CHECK(searchRowsInSource(*search.model(), *doc.model).contains(-1),
          label + ": stale results contain rows the tab now hides");
    compareView(view, {}, label + ": stale results while the panel is hidden");
    search.setLive(true);
    CHECK(testdocs::waitFor([&] { return !search.model()->isFiltering()
                                         && !searchRowsInSource(*search.model(), *doc.model).contains(-1); }),
          label + ": results refresh when the panel is shown");
    compareView(view, searchRowsInSource(*search.model(), *doc.model), label + ": refreshed results");
    CHECK(!view.scrollbarMarksOverlay()->pixels(Category::Search).isEmpty(), label + ": refreshed search marks");
    doc.model->setLogLevelFilter({});
    testdocs::settle(*doc.model);
    CHECK(testdocs::waitFor([&] { return !search.model()->isFiltering()
                                         && search.model()->rowCount() == found.size(); }),
          label + ": results back after the filter");

    search.clear();
    compareView(view, {}, label + ": search cleared");
    CHECK(view.scrollbarMarksOverlay()->pixels(Category::Search).isEmpty(), label + ": no search marks after clear");

    // Поиск снят, пока идёт ПЕРВЫЙ расчёт с его выдачей: опубликованных
    // отметок поиска ещё нет, но результат расчёта их принесёт — снятие
    // обязано его отбросить или пересчитать.
    otherView.hide();
    search.setSource(other.model.get());
    search.search(rules);
    CHECK(testdocs::settle(*search.model()), label + ": other tab results settle");
    CHECK(search.model()->rowCount() > 0, label + ": other tab has results");
    otherView.show(); // показ запускает расчёт сразу, с выдачей
    CHECK(otherView.scrollbarMarks()->isComputing(), label + ": compute with results is running");
    CHECK(otherView.scrollbarMarks()->marks().rows[Category::Search].isEmpty(),
          label + ": no search marks published yet");
    search.clear();
    compareView(otherView, {}, label + ": search cleared during the first compute");
    CHECK(otherView.scrollbarMarks()->marks().rows[Category::Search].isEmpty(),
          label + ": no stale search marks after clear");
}

// Дозапись на индексной вкладке с активным поиском: выдача проверяет новые
// строки фоновым джобом (isFiltering), а расчёт отметок, запущенный в это
// время, выдачу не берёт. Отметки поиска, уже найденные, при этом не должны
// пропадать: прежние строки выдачи от дозаписи в конец не меняются.
static void checkSearchDuringTail(const QDir& dir, const QString& schema)
{
    const QString label = QStringLiteral("search during tail, indexed");
    const QStringList paths = makeFiles(dir, QStringLiteral("search-tail"), 1, 1500);
    testdocs::Document doc = testdocs::buildIndexed(paths, schema);
    LogListView view;
    view.resize(900, 500);
    view.setModel(doc.model.get());
    SearchResultsController search;
    view.scrollbarMarks()->setSearch(&search);
    view.show();
    search.setSource(doc.model.get());
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = QStringLiteral("timeout");
    rules.rules = { rule };
    search.search(rules);
    CHECK(testdocs::settle(*search.model()), label + ": results settle");
    compareView(view, searchRowsInSource(*search.model(), *doc.model), label + ": results");
    ScrollbarMarks& marks = *view.scrollbarMarks();
    CHECK(!marks.marks().rows[Category::Search].isEmpty(), label + ": search marks published");

    // Слот после контроллера: выдача уже запустила джоб по новым строкам.
    bool launchedWhileFiltering = false;
    const QMetaObject::Connection inserted = QObject::connect(
        doc.model.get(), &QAbstractItemModel::rowsInserted, &view, [&]() {
            if (!search.model()->isFiltering() || marks.isComputing())
                return;
            marks.flush();
            launchedWhileFiltering = launchedWhileFiltering || marks.isComputing();
        });
    int published = 0;
    int publishedWithoutSearch = 0;
    const QMetaObject::Connection changed = QObject::connect(
        &marks, &ScrollbarMarks::marksChanged, &view, [&]() {
            ++published;
            if (marks.marks().rows[Category::Search].isEmpty())
                ++publishedWithoutSearch;
        });
    QByteArray tail;
    for (int i = 0; i < 2000; ++i)
        tail += "2030-01-01 00:00:00.000 [main] WARN - timeout on appended row "
            + QByteArray::number(i) + "\n";
    CHECK(testdocs::appendIndexedTail(doc, 0, tail, schema), label + ": append");
    CHECK(testdocs::settle(*search.model()), label + ": results follow the tail");
    CHECK(marksReady(view), label + ": marks settle");
    QObject::disconnect(inserted);
    QObject::disconnect(changed);
    CHECK(launchedWhileFiltering, label + ": a compute ran while the results were filtering");
    CHECK(published > 0, label + ": marks were published");
    CHECK(publishedWithoutSearch == 0,
          QStringLiteral("%1: %2 of %3 publications dropped the search marks")
              .arg(label).arg(publishedWithoutSearch).arg(published));
    compareView(view, searchRowsInSource(*search.model(), *doc.model), label + ": after tail");

    // Перестройка выдачи отбрасывает только расчёт, который её сопоставлял:
    // расчёт одних уровней (выдача ещё считалась) доводится до конца.
    const int discarded = marks.discardedCount();
    search.search(rules); // полный поиск заново: выдача сброшена и считается
    CHECK(search.model()->isFiltering(), label + ": full search is running");
    marks.flush();
    CHECK(marks.isComputing(), label + ": a compute without results is running");
    search.clear();
    CHECK(marks.discardedCount() == discarded,
          label + ": results rebuilt, the compute that did not use them is kept");
    compareView(view, {}, label + ": search cleared");
}

// ---------------------------------------------------------------------------
// Жизнь расчёта: отмена на reset, дебаунс, скрытый view
// ---------------------------------------------------------------------------

static void checkLifecycle(const QDir& dir, const QString& schema)
{
    const QStringList paths = makeFiles(dir, QStringLiteral("life"), 2, 2000);
    testdocs::Document doc = testdocs::buildResident({ paths[0] }, schema);
    LogListView view;
    view.resize(900, 500);
    view.setModel(doc.model.get());
    view.show();
    compareView(view, {}, "lifecycle: opened");
    ScrollbarMarks& marks = *view.scrollbarMarks();

    // Reset посреди расчёта: результат отброшен, опубликован только новый.
    const int launches = marks.launchCount();
    const int discarded = marks.discardedCount();
    doc.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Info });
    marks.flush();
    CHECK(marks.isComputing(), "lifecycle: a compute is running");
    const quint64 serial = marks.serial();
    doc.model->setLogLevelFilter({ LogLevel::Warn, LogLevel::Debug });
    CHECK(marks.discardedCount() == discarded + 1, "lifecycle: reset discards the running compute");
    compareView(view, {}, "lifecycle: after reset");
    CHECK(marks.launchCount() == launches + 2, "lifecycle: one more compute after the reset");
    CHECK(marks.serial() == serial + 1, "lifecycle: the discarded result was not published");
    doc.model->setLogLevelFilter({});
    testdocs::settle(*doc.model);
    compareView(view, {}, "lifecycle: filter off");

    // Поток батчей: каждый — только флаг и таймер.
    testdocs::Document second = testdocs::buildResident({ paths[1] }, schema);
    QVector<std::shared_ptr<LogEntry>> entries = second.model->residentEntriesForFieldMutation();
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return *a < *b; });
    const int start = marks.launchCount();
    const int batches = 100;
    const int per = int(entries.size() + batches - 1) / batches;
    for (int b = 0; b < batches; ++b) {
        const QVector<std::shared_ptr<LogEntry>> batch = entries.mid(b * per, per);
        if (!batch.isEmpty())
            doc.model->mergeEntries(batch);
    }
    CHECK(marks.launchCount() == start, "batches: nothing is computed while batches arrive");
    compareView(view, {}, "batches: merged");
    CHECK(marks.launchCount() - start <= 2,
          QStringLiteral("batches: %1 computes for %2 batches").arg(marks.launchCount() - start).arg(batches));

    // Скрытый view не считает; показ — догоняет.
    view.hide();
    CHECK(!marks.isEnabled(), "hidden: marks disabled");
    const int hidden = marks.launchCount();
    doc.model->setLogLevelFilter({ LogLevel::Error });
    testdocs::settle(*doc.model);
    testdocs::pump(ScrollbarMarks::kDebounceMs + 150);
    CHECK(marks.launchCount() == hidden, "hidden: no compute while the view is hidden");
    view.show();
    compareView(view, {}, "hidden: shown again");
    CHECK(marks.launchCount() == hidden + 1, "hidden: one compute on show");
}

// ---------------------------------------------------------------------------
// Замер
// ---------------------------------------------------------------------------

static int bench(TimingApplication& app, const QString& path)
{
    LogViewWidget tab;
    tab.resize(1200, 800);
    tab.show();
    QElapsedTimer t;
    t.start();
    app.timing = true;
    tab.addLogFile(path);
    testdocs::waitFor([&] { return !tab.isLoading(); }, 3600000);
    testdocs::settle(*tab.model(), 200, 3600000);
    const qint64 loadMs = t.elapsed();
    ScrollbarMarks& marks = *tab.view()->scrollbarMarks();
    testdocs::waitFor([&] { return marks.isUpToDate(); }, 3600000);
    std::printf("bench: %d rows, indexed %d; load %lld ms, marks ready at %lld ms\n",
                tab.model()->rowCount(), int(tab.model()->isIndexedBackend()), loadMs,
                qint64(t.elapsed()));
    std::printf("bench: load — max GUI event %lld ms (%s), events > 100 ms: %d\n",
                app.maxMs, qPrintable(app.maxWhat), app.slowEvents);

    // Пересчёт отметок под прокруткой.
    app.maxMs = 0;
    app.slowEvents = 0;
    QElapsedTimer compute;
    compute.start();
    // Пересчёт с нуля: отвязать и снова привязать модель.
    marks.setModel(nullptr);
    marks.setModel(tab.model());
    marks.flush();
    QScrollBar* bar = tab.view()->verticalScrollBar();
    QRandomGenerator rng(5);
    int steps = 0;
    QElapsedTimer scrolling;
    scrolling.start();
    while (!marks.isUpToDate() || steps < 200) {
        bar->setValue(int(rng.bounded(qMax(1, bar->maximum()))));
        tab.view()->viewport()->repaint();
        bar->repaint();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        ++steps;
    }
    std::printf("bench: compute %lld ms; %d scroll steps in %lld ms; max GUI event %lld ms (%s), "
                "events > 100 ms: %d; marks %lld/%lld rows\n",
                compute.elapsed(), steps, scrolling.elapsed(), app.maxMs,
                qPrintable(app.maxWhat), app.slowEvents,
                qint64(marks.marks().rows[Category::Error].size()),
                qint64(marks.marks().rows[Category::Warning].size()));
    return 0;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    TimingApplication app(argc, argv);
    if (argc >= 3 && QByteArray(argv[1]) == "--bench")
        return bench(app, QString::fromLocal8Bit(argv[2]));

    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();

    checkCore();
    checkViews(dir, schema);
    checkTabUpdates(dir, schema, false);
    checkTabUpdates(dir, schema, true);
    checkSearch(dir, schema, false);
    checkSearch(dir, schema, true);
    checkSearch(dir, schema, false, 1);
    checkSearch(dir, schema, true, 1);
    checkSearch(dir, schema, false, 1, true);
    checkSearch(dir, schema, true, 1, true);
    checkSearchDuringTail(dir, schema);
    checkLifecycle(dir, schema);

    if (failures) {
        std::fprintf(stderr, "scrollbar_marks: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("scrollbar_marks: OK\n");
    return 0;
}
