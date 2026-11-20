// ============================================================================
// row_chrome — служебные элементы строки LogListView (U-23,
// docs/design/U-23.md).
// PR 1, переключатель переноса:
//   • stickyTop — таблица случаев (строка выше, ниже, пересекает верх,
//     короче элемента);
//   • строка в 20 экранов развёрнута, вид в её середине и у последней линии:
//     «−» у верха вьюпорта, рейка — в любой точке по высоте; щелчок
//     сворачивает; свёрнутая строка видна и стоит у верха; верх был на
//     экране — строка не сдвинулась;
//   • «+N»: 9 999, 10 000, миллион скрытых символов — плашка в резерве,
//     показано + скрыто = длина строки (и на строке с табуляцией);
//   • протяжка выделения, законченная на рейке, не сворачивает; нажатие на
//     рейке с протяжкой — тоже;
//   • hugeRowMode (индексный бэкенд): щелчок по «+N» и команда переноса
//     испускают fullTextRequested, строка выбрана, высоты не меняются;
//   • подсказки: над переключателем — текст, над текстом — событие
//     проглочено, подсказки нет;
//   • отрисовка после прокрутки (блит базового scrollContentsBy): вид,
//     прокрученный по шагам, выглядит как свежий вид в той же позиции.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "logmodel.h"
#include "shortcount.h"
#include "shortcutmanager.h"
#include "testdocuments.h"

#include <QApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolTip>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static void section(const char* name)
{
    std::fprintf(stderr, "== %s\n", name);
}

using Chrome = LogListView::RowChrome;

static QString prefix(int i)
{
    return QStringLiteral("2026-03-05 10:00:%1.000 [w] INFO - ").arg(i % 60, 2, 10, QLatin1Char('0'));
}

static QString writeLines(const QDir& dir, const QString& name, const QStringList& lines)
{
    return testdocs::writeFile(dir, name, (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
}

static QString words(int chars)
{
    static const QString w = QStringLiteral("lorem ipsum dolor sit amet consectetur adipiscing elit ");
    QString s;
    s.reserve(chars + w.size());
    while (s.size() < chars)
        s += w;
    s.truncate(chars);
    return s;
}

static bool settleView(LogListView& view, LogModel& model)
{
    testdocs::settle(model);
    return testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 10000);
}

struct ShownView {
    std::unique_ptr<LogListView> view;
    ShownView(LogModel* model, bool wrap, QSize size = QSize(800, 400))
        : view(std::make_unique<LogListView>())
    {
        view->setAttribute(Qt::WA_ShowWithoutActivating);
        view->resize(size);
        view->move(200, 200);
        view->setModel(model);
        view->setWordWrap(wrap);
        view->show();
        settleView(*view, *model);
        testdocs::pump(30);
    }
    LogListView* operator->() const { return view.get(); }
    QWidget* vp() const { return view->viewport(); }
};

static int rowHeight1(LogListView& v)
{
    // Высота однострочной строки: visualRect первой (короткой) строки.
    return v.visualRect(v.model()->index(0, 0)).height();
}

// Поставить контент так, чтобы строка row начиналась на y экрана (может
// быть отрицательным — верх строки над экраном).
static void placeRowTop(LogListView& v, int row, int y)
{
    QScrollBar* bar = v.verticalScrollBar();
    const int contentTop = bar->value() + v.visualRect(v.model()->index(row, 0)).top();
    bar->setValue(contentTop - y);
    testdocs::waitFor([&] { return !v.hasPendingLayout(); }, 5000);
    testdocs::pump(20);
    // Высоты уточняются отрисовкой — второй проход ставит точно.
    const int contentTop2 = bar->value() + v.visualRect(v.model()->index(row, 0)).top();
    if (contentTop2 != contentTop) {
        bar->setValue(contentTop2 - y);
        testdocs::pump(20);
    }
}

static void click(QWidget* w, QPoint p)
{
    QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, p);
    testdocs::pump(20);
}

// ---------------------------------------------------------------------------
static void checkStickyTop()
{
    section("stickyTop table");
    const struct { qint64 rowTop; int rowHeight; int item; int expected; const char* what; } cases[] = {
        {  50, 1000, 18,   50, "row top on screen: item at the row top" },
        {   0, 1000, 18,    0, "row top at the viewport top" },
        {-500, 1000, 18,    0, "row crosses the top: item sticks to the viewport top" },
        {-982, 1000, 18,    0, "last line exactly at the top" },
        {-990, 1000, 18,   -8, "row bottom leaves: item goes with the bottom" },
        {-5000, 1000, 18, -4018, "row entirely above the screen" },
        { 300,   18, 18,  300, "one-line row below the top" },
        {  -5,   10, 18,   -5, "row shorter than the item: at the row top" },
        { 700, 1000, 18,  700, "row below the screen" },
    };
    for (const auto& c : cases) {
        const int got = LogListView::stickyTop(c.rowTop, c.rowHeight, c.item);
        CHECK(got == c.expected, QStringLiteral("stickyTop(%1, %2, %3) = %4, expected %5 (%6)")
                                     .arg(c.rowTop).arg(c.rowHeight).arg(c.item).arg(got).arg(c.expected)
                                     .arg(QLatin1String(c.what)));
    }
}

// ---------------------------------------------------------------------------
static void checkShortCount()
{
    section("short counts");
    const struct { qint64 n; const char* expected; } cases[] = {
        {0, "0"}, {9999, "9999"}, {10000, "10K"}, {12345, "12K"}, {999999, "999K"},
        {1000000, "1.0M"}, {3456789, "3.4M"}, {9999999, "9.9M"}, {25000000, "25M"},
        {999999999, "999M"}, {2100000000, "2.1G"},
    };
    for (const auto& c : cases) {
        const QString got = ShortCount::format(c.n, 10000, false);
        CHECK(got == QLatin1String(c.expected),
              QStringLiteral("short(%1) = %2, expected %3").arg(c.n).arg(got, QLatin1String(c.expected)));
        CHECK(got.size() <= 4, QStringLiteral("short(%1) fits four characters").arg(c.n));
    }
    CHECK(ShortCount::format(99999, 100000, true) == QStringLiteral("99,999"), "grouped exact count");
    CHECK(ShortCount::format(120500, 100000, true) == QStringLiteral("120K"), "grouped short count");
    CHECK(ShortcutManager::instance().defaultSequence(QStringLiteral("toggleLineWrap")) == QKeySequence(Qt::Key_Space),
          "toggleLineWrap is a ShortcutManager command with Space by default");
}

// ---------------------------------------------------------------------------
// Строка в 20 экранов, развёрнутая переносом.
// ---------------------------------------------------------------------------
static void checkTallRow(const QDir& dir)
{
    section("tall wrapped row: sticky toggle, rail, collapse keeps the row on screen");
    QStringList lines;
    for (int i = 0; i < 5; ++i)
        lines << prefix(i) + QStringLiteral("short line %1").arg(i);
    const int giant = lines.size();
    lines << prefix(5) + words(90000);
    for (int i = 6; i < 60; ++i)
        lines << prefix(i) + QStringLiteral("tail line %1").arg(i);
    const QString path = writeLines(dir, QStringLiteral("tall.log"), lines);
    testdocs::Document doc = testdocs::buildResident({path}, testdocs::buildSchema());
    CHECK(doc.errors.isEmpty(), "tall fixture loads");

    ShownView v(doc.model.get(), /*wrap=*/true);
    const QModelIndex gi = doc.model->index(giant, 0);
    const int vpH = v.vp()->height();
    const int single = rowHeight1(*v.view);
    const int rowH = v->visualRect(gi).height();
    CHECK(rowH > 20 * vpH, QStringLiteral("the giant row is over 20 screens (%1 px, viewport %2)").arg(rowH).arg(vpH));

    // Середина строки.
    placeRowTop(*v.view, giant, -rowH / 2);
    LogListView::RowWrapInfo info = v->rowWrapInfo(giant);
    CHECK(info.hasToggle && info.expanded, "the giant row is expanded and has a toggle");
    CHECK(info.toggleText == QString(QChar(0x2212)), QStringLiteral("expanded toggle shows the minus: %1").arg(info.toggleText));
    CHECK(info.toggleRect.top() >= 0 && info.toggleRect.bottom() < single,
          QStringLiteral("the minus sticks to the viewport top: %1..%2").arg(info.toggleRect.top()).arg(info.toggleRect.bottom()));
    LogListView::RowChromeHit hit = v->rowChromeAt(info.toggleRect.center());
    CHECK(hit.kind == Chrome::WrapToggle && hit.row == giant, "rowChromeAt finds the minus at the top");
    const int railX = info.toggleRect.center().x();
    for (int y : {single + 2, vpH / 3, vpH / 2, vpH - 3}) {
        hit = v->rowChromeAt(QPoint(railX, y));
        CHECK(hit.kind == Chrome::WrapRail && hit.row == giant,
              QStringLiteral("rail at y=%1 (kind %2, row %3)").arg(y).arg(int(hit.kind)).arg(hit.row));
    }
    CHECK(v->rowChromeAt(QPoint(info.textRect.left() + 40, vpH / 2)).kind == Chrome::None,
          "the text area is not chrome");

    // Протяжка выделения из текста, законченная на рейке, не сворачивает.
    {
        const QPoint from(info.textRect.left() + 30, vpH / 3);
        const QPoint to(railX, vpH / 2);
        QTest::mousePress(v.vp(), Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(v.vp()->mapToGlobal(to)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(v.vp(), &move);
        QTest::mouseRelease(v.vp(), Qt::LeftButton, Qt::NoModifier, to);
        testdocs::pump(20);
        CHECK(v->rowWrapInfo(giant).expanded, "a selection drag that ends on the rail does not collapse");
    }
    // Нажатие на рейке и протяжка — не щелчок.
    {
        const QPoint from(railX, vpH / 3);
        const QPoint to(railX, vpH / 3 + 60);
        QTest::mousePress(v.vp(), Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(v.vp()->mapToGlobal(to)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(v.vp(), &move);
        QTest::mouseRelease(v.vp(), Qt::LeftButton, Qt::NoModifier, to);
        testdocs::pump(20);
        CHECK(v->rowWrapInfo(giant).expanded, "press on the rail with a drag is not a click");
    }

    // Щелчок по рейке посреди строки сворачивает; строка — у верха экрана.
    click(v.vp(), QPoint(railX, vpH / 2));
    testdocs::waitFor([&] { return !v->hasPendingLayout(); }, 5000);
    QRect r = v->visualRect(gi);
    CHECK(!v->rowWrapInfo(giant).expanded, "click on the rail collapses the row");
    CHECK(r.height() == single, QStringLiteral("collapsed row is one line (%1)").arg(r.height()));
    CHECK(r.top() == 0, QStringLiteral("collapsed row stands at the viewport top (top %1)").arg(r.top()));

    // Развернуть щелчком по «+N»: верх строки остаётся на месте.
    info = v->rowWrapInfo(giant);
    CHECK(info.hasToggle && info.toggleText.startsWith(QLatin1Char('+')), "collapsed row shows +N");
    click(v.vp(), info.toggleRect.center());
    testdocs::waitFor([&] { return !v->hasPendingLayout(); }, 5000);
    r = v->visualRect(gi);
    CHECK(v->rowWrapInfo(giant).expanded && r.top() == 0 && r.height() > 20 * vpH,
          QStringLiteral("+N expands in place (top %1, height %2)").arg(r.top()).arg(r.height()));

    // Последняя линия строки у верха экрана: «−» над ней, щелчок сворачивает.
    placeRowTop(*v.view, giant, -(r.height() - single));
    info = v->rowWrapInfo(giant);
    r = v->visualRect(gi);
    CHECK(info.toggleRect.top() >= r.top() && info.toggleRect.bottom() <= r.bottom()
              && info.toggleRect.bottom() < single + 2,
          QStringLiteral("at the last line the minus goes with the row bottom (toggle %1..%2, row %3..%4)")
              .arg(info.toggleRect.top()).arg(info.toggleRect.bottom()).arg(r.top()).arg(r.bottom()));
    hit = v->rowChromeAt(info.toggleRect.center());
    CHECK(hit.kind == Chrome::WrapToggle && hit.row == giant, "rowChromeAt finds the minus at the last line");
    click(v.vp(), info.toggleRect.center());
    r = v->visualRect(gi);
    CHECK(r.height() == single && r.top() == 0,
          QStringLiteral("collapse from the last line: the row at the top (top %1)").arg(r.top()));

    // Верх строки на экране: сворачивание не двигает строку.
    click(v.vp(), v->rowWrapInfo(giant).toggleRect.center());
    placeRowTop(*v.view, giant, 50);
    r = v->visualRect(gi);
    CHECK(r.top() == 50 && v->rowWrapInfo(giant).expanded, QStringLiteral("expanded row placed at 50 (%1)").arg(r.top()));
    click(v.vp(), QPoint(railX, 200));
    r = v->visualRect(gi);
    CHECK(r.height() == single && r.top() == 50,
          QStringLiteral("collapse with the top on screen keeps the row in place (top %1)").arg(r.top()));

    // Команда переноса текущей строки: выбрать строку, развернуть и свернуть.
    v->setCurrentIndex(gi);
    CHECK(v->toggleCurrentLineWrap() && v->rowWrapInfo(giant).expanded, "toggleCurrentLineWrap expands");
    CHECK(v->toggleCurrentLineWrap() && !v->rowWrapInfo(giant).expanded, "toggleCurrentLineWrap collapses");
    v->setCurrentIndex(doc.model->index(0, 0));
    CHECK(!v->toggleCurrentLineWrap(), "a short line has nothing to toggle");
    // Текущая без выделения (её назначает Qt по фокусу) — не выбор
    // пользователя: команда её не трогает (инвариант 10).
    v->selectionModel()->setCurrentIndex(gi, QItemSelectionModel::Clear);
    CHECK(v->currentIndex() == gi && v->userSelectedRow() < 0, "current but not selected");
    CHECK(!v->toggleCurrentLineWrap() && !v->rowWrapInfo(giant).expanded,
          "the command ignores a current row the user did not select");

    // Двойной щелчок по «+N» — два щелчка: развернуть и свернуть, а не
    // выделение слова у края строки.
    {
        placeRowTop(*v.view, giant, 40);
        const QPoint at = v->rowWrapInfo(giant).toggleRect.center();
        QTest::mouseClick(v.vp(), Qt::LeftButton, Qt::NoModifier, at);
        CHECK(v->rowWrapInfo(giant).expanded, "first click of a double click expands");
        // Второе нажатие приходит от Qt парой «нажатие + двойной щелчок».
        const QPoint again = v->rowWrapInfo(giant).toggleRect.center();
        QTest::mousePress(v.vp(), Qt::LeftButton, Qt::NoModifier, again);
        QTest::mouseDClick(v.vp(), Qt::LeftButton, Qt::NoModifier, again);
        QTest::mouseRelease(v.vp(), Qt::LeftButton, Qt::NoModifier, again);
        testdocs::pump(20);
        CHECK(!v->rowWrapInfo(giant).expanded, "the double-click event counts as the second click");
        CHECK(v->visualRect(gi).top() == 40, "and the row stays in place");
    }

    // Подсказки: над текстом — событие проглочено, подсказки нет; над
    // переключателем — текст про перенос.
    {
        const QPoint textPos(info.textRect.left() + 30, 5);
        QHelpEvent onText(QEvent::ToolTip, textPos, v.vp()->mapToGlobal(textPos));
        const bool handled = QApplication::sendEvent(v.vp(), &onText);
        CHECK(handled && !QToolTip::isVisible(), "tooltip over the text: swallowed, nothing shown");
        click(v.vp(), v->rowWrapInfo(giant).toggleRect.center()); // развернуть
        placeRowTop(*v.view, giant, -vpH);
        const QPoint togglePos = v->rowWrapInfo(giant).toggleRect.center();
        QHelpEvent onToggle(QEvent::ToolTip, togglePos, v.vp()->mapToGlobal(togglePos));
        QApplication::sendEvent(v.vp(), &onToggle);
        CHECK(QToolTip::isVisible() && QToolTip::text().contains(QStringLiteral("collapse")),
              QStringLiteral("tooltip over the minus: %1").arg(QToolTip::text()));
        const QPoint railPos(railX, vpH / 2);
        CHECK(v->rowChromeAt(railPos).toolTip.contains(QStringLiteral("collapse")), "the rail has the same tooltip");
        QToolTip::hideText();
    }
}

// ---------------------------------------------------------------------------
// «+N»: сокращённое число в резерве, показано + скрыто = длина.
// ---------------------------------------------------------------------------
static void checkHiddenCounts(const QDir& dir)
{
    section("+N: short numbers fit the reserve, visible + hidden = length");
    // Сколько символов строки без табуляции видно при этой ширине.
    int visible = 0;
    int prefixLen = 0;
    {
        const QString path = writeLines(dir, QStringLiteral("probe.log"), {prefix(0) + QString(5000, QLatin1Char('x'))});
        testdocs::Document doc = testdocs::buildResident({path}, testdocs::buildSchema());
        ShownView v(doc.model.get(), false);
        const LogListView::RowWrapInfo info = v->rowWrapInfo(0);
        visible = info.visibleChars;
        prefixLen = info.length - 5000;
        CHECK(visible > 40 && info.hiddenChars == info.length - visible, "probe row: visible and hidden");
    }
    const struct { int hidden; const char* text; } cases[] = {
        {9999, "+9999"}, {10000, "+10K"}, {1000000, "+1.0M"}, {123456, "+123K"},
    };
    QStringList lines;
    for (const auto& c : cases)
        lines << prefix(0) + QString(visible + c.hidden - prefixLen, QLatin1Char('x'));
    QString tabbed = prefix(1);
    for (int i = 0; i < 3000; ++i)
        tabbed += (i % 5 == 0) ? QStringLiteral("\t") : QStringLiteral("ab");
    lines << tabbed;
    const QString path = writeLines(dir, QStringLiteral("hidden.log"), lines);
    testdocs::Document doc = testdocs::buildResident({path}, testdocs::buildSchema());
    ShownView v(doc.model.get(), false);
    for (int row = 0; row < lines.size(); ++row) {
        const LogListView::RowWrapInfo info = v->rowWrapInfo(row);
        CHECK(info.hasToggle && !info.expanded, QStringLiteral("row %1 has +N").arg(row));
        CHECK(info.visibleChars + info.hiddenChars == info.length,
              QStringLiteral("row %1: visible %2 + hidden %3 = length %4")
                  .arg(row).arg(info.visibleChars).arg(info.hiddenChars).arg(info.length));
        CHECK(info.toggleRect.width() <= info.toggleReserve,
              QStringLiteral("row %1: badge %2 px fits the reserve %3 px").arg(row).arg(info.toggleRect.width()).arg(info.toggleReserve));
        CHECK(info.textRect.right() < info.toggleRect.left(), QStringLiteral("row %1: text stops before the badge").arg(row));
        if (row < int(std::size(cases))) {
            CHECK(info.hiddenChars == cases[row].hidden,
                  QStringLiteral("row %1 hides %2 (expected %3)").arg(row).arg(info.hiddenChars).arg(cases[row].hidden));
            CHECK(info.toggleText == QLatin1String(cases[row].text),
                  QStringLiteral("row %1 shows %2 (expected %3)").arg(row).arg(info.toggleText, QLatin1String(cases[row].text)));
        }
        // Подсказка — точное число.
        const QString tip = v->rowChromeAt(info.toggleRect.center()).toolTip;
        QLocale c = QLocale::c();
        c.setNumberOptions(QLocale::DefaultNumberOptions);
        CHECK(tip.contains(c.toString(info.hiddenChars)), QStringLiteral("row %1 tooltip has the exact count: %2").arg(row).arg(tip));
    }
}

// ---------------------------------------------------------------------------
// hugeRowMode: переноса нет — «+N» и команда просят полный текст.
// ---------------------------------------------------------------------------
static void checkHugeRowMode(const QDir& dir)
{
    section("hugeRowMode: +N asks for the full text");
    QStringList lines;
    for (int i = 0; i < 40; ++i)
        lines << prefix(i) + (i % 3 == 1 ? words(3000) : QStringLiteral("short %1").arg(i));
    const QString path = writeLines(dir, QStringLiteral("huge.log"), lines);
    testdocs::Document doc = testdocs::buildIndexed({path}, testdocs::buildSchema());
    CHECK(doc.errors.isEmpty() && doc.model->isIndexedBackend(), "indexed fixture loads");
    ShownView v(doc.model.get(), /*wrap=*/true);
    const int single = v->visualRect(doc.model->index(0, 0)).height();
    const int row = 4;
    LogListView::RowWrapInfo info = v->rowWrapInfo(row);
    CHECK(info.hasToggle && !info.expanded && info.toggleText.startsWith(QLatin1Char('+')),
          "indexed long line shows +N even with Word Wrap on");
    QSignalSpy spy(v.view.get(), &LogListView::fullTextRequested);
    click(v.vp(), info.toggleRect.center());
    CHECK(spy.count() == 1 && spy.at(0).at(0).toInt() == row, "click on +N requests the full text");
    CHECK(v->userSelectedRow() == row, "and selects the row");
    CHECK(v->visualRect(doc.model->index(row, 0)).height() == single && !v->rowWrapInfo(row).expanded,
          "heights do not change");
    CHECK(v->rowChromeAt(info.toggleRect.center()).toolTip.contains(QStringLiteral("Entry Details")),
          "the tooltip says where the full text is");
    v->setCurrentIndex(doc.model->index(7, 0));
    CHECK(v->toggleCurrentLineWrap() && spy.count() == 2 && spy.at(1).at(0).toInt() == 7,
          "the wrap command requests the full text of the current line");
    v->setCurrentIndex(doc.model->index(0, 0));
    CHECK(!v->toggleCurrentLineWrap() && spy.count() == 2, "a short line requests nothing");
}

// ---------------------------------------------------------------------------
// Отрисовка после прокрутки: база QListView сдвигает вьюпорт блитом
// (scrollContentsBy, LogListView его не переопределяет — предположение
// документа). Прилипающий «−» и рейка не должны оставлять след: снимок
// экрана (backing store, а не grab() с полной перерисовкой) после прокрутки
// по шагам равен свежему виду в той же позиции.
// ---------------------------------------------------------------------------
static void checkScrollRepaint(const QDir& dir)
{
    section("scrolling repaints sticky chrome without trails");
    QStringList lines;
    for (int i = 0; i < 3; ++i)
        lines << prefix(i) + QStringLiteral("short line %1").arg(i);
    lines << prefix(3) + words(40000);
    for (int i = 4; i < 30; ++i)
        lines << prefix(i) + QStringLiteral("tail line %1 ").arg(i) + words(i % 4 == 0 ? 400 : 20);
    const QString path = writeLines(dir, QStringLiteral("scroll.log"), lines);
    testdocs::Document doc = testdocs::buildResident({path}, testdocs::buildSchema());

    const auto screenImage = [](LogListView& view) {
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
        testdocs::pump(60);
        QWidget* vp = view.viewport();
        const QPoint at = vp->mapTo(&view, QPoint(0, 0));
        return view.screen()->grabWindow(view.winId(), at.x(), at.y(), vp->width(), vp->height())
            .toImage().convertToFormat(QImage::Format_RGB32);
    };

    ShownView scrolled(doc.model.get(), true);
    placeRowTop(*scrolled.view, 3, -600);
    QScrollBar* bar = scrolled->verticalScrollBar();
    for (int step = 0; step < 6; ++step) {
        bar->setValue(bar->value() + (step % 2 ? 37 : 3 * bar->singleStep()));
        testdocs::pump(40);
    }
    const int finalValue = bar->value();
    const QImage afterScroll = screenImage(*scrolled.view);
    CHECK(!afterScroll.isNull() && afterScroll.width() > 100, "screen image of the scrolled view");

    ShownView fresh(doc.model.get(), true);
    placeRowTop(*fresh.view, 3, -600);
    fresh->verticalScrollBar()->setValue(finalValue);
    const QImage reference = screenImage(*fresh.view);
    int differing = 0;
    QPoint first(-1, -1);
    if (afterScroll.size() == reference.size()) {
        for (int y = 0; y < reference.height(); ++y)
            for (int x = 0; x < reference.width(); ++x)
                if (afterScroll.pixel(x, y) != reference.pixel(x, y)) {
                    if (!differing)
                        first = QPoint(x, y);
                    ++differing;
                }
    } else {
        differing = -1;
    }
    CHECK(differing == 0, QStringLiteral("scrolled view looks like a fresh one: %1 px differ, first at (%2,%3)")
                              .arg(differing).arg(first.x()).arg(first.y()));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    AppSettings::instance().load();
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "temporary directory");
    const QDir dir(tmp.path());

    checkStickyTop();
    checkShortCount();
    checkTallRow(dir);
    checkHiddenCounts(dir);
    checkHugeRowMode(dir);
    checkScrollRepaint(dir);

    if (failures) {
        std::fprintf(stderr, "row_chrome: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "row_chrome: OK\n");
    return 0;
}
