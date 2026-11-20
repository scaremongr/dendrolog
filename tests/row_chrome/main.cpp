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
// PR 2, метка файла слитой вкладки:
//   • FileLabels::build — таблица и случайные наборы (метки попарно разные);
//   • вкладка на обоих бэкендах: один файл — ни колонки, ни легенды; второй
//     файл — колонка и легенда (порядок добавления, цвета полос = цвета
//     чипов = AppTheme::fileColor); левый край и ширина текста у строк всех
//     файлов одни; режимы Name / Color Only / Hidden и ini; число строк на
//     чипе против строк файла — после загрузки, дозаписи, полной
//     перезагрузки и удаления файла (колонка и легенда уходят); переход к
//     следующей / предыдущей строке файла против перебора и в историю;
//   • высокая строка: полоса цвета — в любой видимой части, метка — у верха
//     вьюпорта с путём в подсказке;
//   • hit-test: выделение мышью попадает в те же символы с колонкой и без.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "apptheme.h"
#include "filelabels.h"
#include "filelegend.h"
#include "logviewwidget.h"
#include "navhistory.h"
#include "logmodel.h"
#include "shortcount.h"
#include "shortcutmanager.h"
#include "testdocuments.h"

#include <QApplication>
#include <QClipboard>
#include <QRandomGenerator>
#include <QSettings>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QScreen>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolTip>
#include <QFile>
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


// ---------------------------------------------------------------------------
// PR 2: метки файлов
// ---------------------------------------------------------------------------
static void checkFileLabelsTable()
{
    section("FileLabels::build");
    const QChar e(0x2026);
    const struct { QStringList names; int max; QStringList expected; const char* what; } cases[] = {
        {{"app.log", "db.log"}, 16, {"app.log", "db.log"}, "short names stay"},
        {{"app.log.1", "app.log.2"}, 16, {"app.log.1", "app.log.2"}, "rotated names fit"},
        {{"payment-service-prod-eu-1.log", "payment-service-prod-us-1.log"}, 12,
         {QString(e) + "eu" + e, QString(e) + "us" + e}, "common prefix and suffix"},
        {{"payment-service-prod-eu-1.log", "payment-service-prod-us-1.log", "auth.log"}, 12,
         {"payment…1.log", "payment…1.log~2", "auth.log"}, "nothing common: middle elided, repeats tagged"},
        {{"server.log", "server.log"}, 16, {"server.log", "server.log~2"}, "same names from different folders"},
        {{"журнал-сервера-основной.log", "журнал-сервера-резервный.log"}, 12,
         {QString(e) + "основной" + e, QString(e) + "резервный" + e}, "cyrillic"},
        {{"a-very-long-single-file-name.log"}, 16, {"a-very-l…ame.log"}, "one file: elided to the limit"},
        {{"x-aaaaaaaaaaaaaaaaaaaa-1.log", "x-bbbbbbbbbbbbbbbbbbbb-1.log"}, 10,
         {QString(e) + "aaaa…aaa" + e, QString(e) + "bbbb…bbb" + e}, "limit below the distinct part"},
        {{"app.log", "app.log.1", "app.log.2", "very-long-name-of-a-log.log"}, 8,
         {"app.log", "app.log.1", "app.log.2", "very…log"}, "a name that is a prefix of another"},
    };
    for (const auto& c : cases) {
        const QStringList got = FileLabels::build(c.names, c.max);
        CHECK(got.size() == c.names.size(), QStringLiteral("%1: one label per name").arg(QLatin1String(c.what)));
        for (const QString& l : got)
            CHECK(l.size() <= c.max, QStringLiteral("%1: '%2' fits %3").arg(QLatin1String(c.what), l).arg(c.max));
        CHECK(QSet<QString>(got.begin(), got.end()).size() == got.size(),
              QStringLiteral("%1: labels differ: %2").arg(QLatin1String(c.what), got.join(QLatin1String(" | "))));
        if (c.what != std::string_view("nothing common: middle elided, repeats tagged")
            && c.what != std::string_view("a name that is a prefix of another"))
            CHECK(got == c.expected, QStringLiteral("%1: %2 (expected %3)")
                                         .arg(QLatin1String(c.what), got.join(QLatin1String(" | ")),
                                              c.expected.join(QLatin1String(" | "))));
    }
    // Случайные наборы: метки попарно разные и в пределе.
    QRandomGenerator rng(2311);
    const QStringList parts = {"app", "srv", "prod", "eu", "us", "1", "2", "log", "worker", "db", "x"};
    const QStringList seps = {"-", "_", ".", " "};
    for (int round = 0; round < 400; ++round) {
        QStringList names;
        const int n = 2 + int(rng.bounded(8));
        for (int i = 0; i < n; ++i) {
            QString name = parts[rng.bounded(parts.size())];
            const int k = 1 + int(rng.bounded(6));
            for (int j = 0; j < k; ++j)
                name += seps[rng.bounded(seps.size())] + parts[rng.bounded(parts.size())];
            names << name;
        }
        const int max = 3 + int(rng.bounded(16));
        const QStringList got = FileLabels::build(names, max);
        bool ok = got.size() == names.size() && QSet<QString>(got.begin(), got.end()).size() == got.size();
        for (const QString& l : got)
            ok = ok && l.size() <= max && !l.isEmpty();
        CHECK(ok, QStringLiteral("random set %1 (max %2): %3 -> %4").arg(round).arg(max)
                      .arg(names.join(QLatin1String(" | ")), got.join(QLatin1String(" | "))));
        if (!ok)
            break;
    }
    CHECK(FileLabels::modeFromName(FileLabels::modeName(FileLabelMode::ColorOnly)) == FileLabelMode::ColorOnly
              && FileLabels::modeFromName(QStringLiteral("junk")) == FileLabelMode::Name,
          "mode names round-trip");
}

static QByteArray fileLines(const QString& tag, int count, int secondBase, int longAt = -1)
{
    QByteArray out;
    for (int i = 0; i < count; ++i) {
        const int sec = secondBase + 2 * i;
        out += QStringLiteral("2026-03-05 10:%1:%2.000 [w] INFO - %3 line %4 ")
                   .arg(sec / 60 % 60, 2, 10, QLatin1Char('0')).arg(sec % 60, 2, 10, QLatin1Char('0'))
                   .arg(tag).arg(i).toUtf8();
        out += (i == longAt ? words(30000) : QStringLiteral("payload")).toUtf8() + '\n';
    }
    return out;
}

static bool settledTab(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); })
        && testdocs::settle(*tab.model())
        && testdocs::waitFor([&] { return !tab.view()->hasPendingLayout(); }, 10000);
}

// Строк каждого файла среди видимых (без фильтра — все строки вкладки).
static QHash<const LogFile*, int> countRows(LogModel& model)
{
    QHash<const LogFile*, int> n;
    for (int r = 0; r < model.rowCount(); ++r)
        ++n[model.keyAt(r).file.get()];
    return n;
}

static void checkLegendCounts(LogViewWidget& tab, const QString& label)
{
    FileLegend* legend = tab.fileLegend();
    const QHash<const LogFile*, int> rows = countRows(*tab.model());
    for (int i = 0; i < legend->chipCount(); ++i) {
        const int expected = rows.value(legend->chipFile(i));
        CHECK(legend->chipText(i).endsWith(QLatin1Char(' ') + QString::number(expected)),
              QStringLiteral("%1: chip %2 '%3' counts %4 lines").arg(label).arg(i).arg(legend->chipText(i)).arg(expected));
    }
}

static void checkMergedTab(const QDir& dir, bool indexed)
{
    const QString label = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
    section(indexed ? "merged tab, indexed: file column and legend" : "merged tab, resident: file column and legend");
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(indexed ? 0 : 512);
    settings.setFileLabelMode(FileLabelMode::Name);
    const QString a = testdocs::writeFile(dir, QStringLiteral("payment-service-prod-eu-1-%1.log").arg(label),
                                          fileLines(QStringLiteral("eu"), 60, 0));
    const QString b = testdocs::writeFile(dir, QStringLiteral("payment-service-prod-us-1-%1.log").arg(label),
                                          fileLines(QStringLiteral("us"), 40, 1, /*longAt=*/20));
    LogViewWidget tab;
    tab.setParserPattern(testdocs::buildSchema());
    tab.resize(900, 500);
    tab.show();
    tab.addLogFile(a);
    CHECK(settledTab(tab), label + ": first file loads");
    LogListView& view = *tab.view();
    LogModel& model = *tab.model();
    CHECK(model.isIndexedBackend() == indexed, label + ": backend");
    CHECK(view.fileColumnWidth() == 0 && tab.fileLegend()->isHidden(),
          label + ": one file — no column, no legend");
    const int plainLeft = view.textLeft();

    view.goToRow(10);
    testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    const RecordKey selectedKey = model.keyAt(10);

    tab.addLogFile(b);
    CHECK(settledTab(tab), label + ": second file loads");
    CHECK(model.rowCount() == 100, QStringLiteral("%1: 100 rows (%2)").arg(label).arg(model.rowCount()));
    CHECK(view.fileColumnWidth() > 0 && view.textLeft() == plainLeft + view.fileColumnWidth(),
          QStringLiteral("%1: second file adds the column (%2 px)").arg(label).arg(view.fileColumnWidth()));
    const int selRow = view.userSelectedRow();
    CHECK(selRow >= 0 && model.keyAt(selRow).file == selectedKey.file
              && model.keyAt(selRow).line == selectedKey.line,
          label + ": the selected line stays selected");

    // Легенда: порядок добавления, цвета полос = цвета чипов.
    FileLegend* legend = tab.fileLegend();
    CHECK(legend->isVisible() && legend->chipCount() == 2, label + ": legend shows two chips");
    CHECK(legend->chipFile(0) && legend->chipFile(0)->filePath == a && legend->chipFile(1)->filePath == b,
          label + ": chips in the order files were added");
    checkLegendCounts(tab, label + " loaded");
    const QVector<LogModel::LabeledFile>& files = model.labeledFiles();
    CHECK(files.size() == 2 && files[0].label != files[1].label
              && files[0].label.contains(QStringLiteral("eu")) && files[1].label.contains(QStringLiteral("us")),
          QStringLiteral("%1: distinguishing labels: %2 / %3").arg(label, files.value(0).label, files.value(1).label));

    // Текст всех строк — с одной вертикали и одной ширины.
    int left = -1, rightEdge = -1;
    bool sameColumn = true;
    for (int r = 0; r < qMin(model.rowCount(), 30); ++r) {
        const LogListView::RowWrapInfo info = view.rowWrapInfo(r);
        if (left < 0) left = info.textRect.left();
        sameColumn = sameColumn && info.textRect.left() == left;
        if (!info.hasToggle) {
            if (rightEdge < 0) rightEdge = info.textRect.right();
            sameColumn = sameColumn && info.textRect.right() == rightEdge;
        }
    }
    CHECK(sameColumn && left == view.textLeft(), label + ": every file's text starts and ends on the same vertical");

    // Цвет полосы строки — цвет её файла.
    {
        view.verticalScrollBar()->setValue(0);
        testdocs::pump(40);
        const QImage image = view.viewport()->grab().toImage();
        const QPalette& pal = view.viewport()->palette();
        bool colors = true;
        for (int r = 0; r < 8; ++r) {
            const QRect vr = view.visualRect(model.index(r, 0));
            const LogModel::LabeledFile* f = model.labeledFile(model.keyAt(r).file.get());
            const QColor expected = AppTheme::fileColor(f ? f->colorOrdinal : -1, pal);
            colors = colors && f && image.pixelColor(view.textLeft() - view.fileColumnWidth() + 1,
                                                     vr.top() + vr.height() / 2) == expected;
        }
        CHECK(colors, label + ": stripes are painted in the file colours");
    }

    // Колонка появляется и уходит — текущая строка и её место на экране
    // остаются (тот же пересчёт, что при смене ширины вьюпорта).
    {
        view.goToRow(40);
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
        const int top = view.visualRect(model.index(40, 0)).top();
        for (FileLabelMode m : {FileLabelMode::Hidden, FileLabelMode::Name}) {
            settings.setFileLabelMode(m);
            testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
            testdocs::pump(20);
            CHECK(view.userSelectedRow() == 40 && view.visualRect(model.index(40, 0)).top() == top,
                  QStringLiteral("%1: column %2 — the selected line keeps its place (%3 vs %4)")
                      .arg(label, m == FileLabelMode::Hidden ? QStringLiteral("removed") : QStringLiteral("added"))
                      .arg(view.visualRect(model.index(40, 0)).top()).arg(top));
        }
    }

    // Режимы.
    settings.setFileLabelMode(FileLabelMode::ColorOnly);
    testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    const int colorOnly = view.fileColumnWidth();
    CHECK(colorOnly > 0 && colorOnly < 12 && legend->isVisible(),
          QStringLiteral("%1: Color Only — a stripe (%2 px) and the legend").arg(label).arg(colorOnly));
    settings.setFileLabelMode(FileLabelMode::Hidden);
    testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    CHECK(view.fileColumnWidth() == 0 && view.textLeft() == plainLeft && legend->isHidden(),
          label + ": Hidden — no column, no legend");
    settings.setFileLabelMode(FileLabelMode::Name);
    testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    CHECK(view.fileColumnWidth() > colorOnly && legend->isVisible(), label + ": back to Name");

    // Переход к следующей / предыдущей строке файла.
    {
        view.goToRow(5);
        const LogFile* fileB = legend->chipFile(1);
        int expectedNext = -1;
        for (int r = 6; r < model.rowCount() && expectedNext < 0; ++r)
            if (model.keyAt(r).file.get() == fileB)
                expectedNext = r;
        const bool hadBack = tab.navHistory().canGoBack();
        CHECK(legend->goToFileLine(1, true) && view.userSelectedRow() == expectedNext,
              QStringLiteral("%1: next line of file B (%2, expected %3)").arg(label).arg(view.userSelectedRow()).arg(expectedNext));
        CHECK(tab.navHistory().canGoBack() || hadBack, label + ": the jump is in the navigation history");
        int expectedPrev = -1;
        for (int r = expectedNext - 1; r >= 0 && expectedPrev < 0; --r)
            if (model.keyAt(r).file.get() == fileB)
                expectedPrev = r;
        if (expectedPrev < 0)
            for (int r = model.rowCount() - 1; r > expectedNext && expectedPrev < 0; --r)
                if (model.keyAt(r).file.get() == fileB)
                    expectedPrev = r;
        CHECK(legend->goToFileLine(1, false) && view.userSelectedRow() == expectedPrev,
              QStringLiteral("%1: previous line of file B (%2, expected %3)").arg(label).arg(view.userSelectedRow()).arg(expectedPrev));
    }

    // Высокая строка (резидентный бэкенд, перенос): полоса в любой части,
    // метка у верха вьюпорта.
    if (!indexed) {
        view.setWordWrap(true);
        int tall = -1;
        for (int r = 0; r < model.rowCount() && tall < 0; ++r)
            if (model.keyAt(r).file.get() == legend->chipFile(1) && model.keyAt(r).line == 21)
                tall = r;
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
        const int h = view.visualRect(model.index(tall, 0)).height();
        const int vpH = view.viewport()->height();
        CHECK(h > 3 * vpH, QStringLiteral("resident: the long line is tall (%1 px)").arg(h));
        placeRowTop(view, tall, -h / 2);
        const QImage image = view.viewport()->grab().toImage();
        const QColor color = AppTheme::fileColor(model.labeledFile(legend->chipFile(1))->colorOrdinal,
                                                 view.viewport()->palette());
        const int stripeX = view.textLeft() - view.fileColumnWidth() + 1;
        CHECK(image.pixelColor(stripeX, 3) == color && image.pixelColor(stripeX, vpH / 2) == color
                  && image.pixelColor(stripeX, vpH - 3) == color,
              "resident: the file stripe runs through every visible part of the tall line");
        const LogListView::RowChromeHit hit = view.rowChromeAt(QPoint(stripeX + 8, 6));
        CHECK(hit.kind == LogListView::RowChrome::FileLabel && hit.row == tall
                  && hit.toolTip == legend->chipFile(1)->displayPath(),
              QStringLiteral("resident: the label sticks to the top, its tooltip is the path (%1)").arg(hit.toolTip));
        view.setWordWrap(false);
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    }

    // Hit-test: выделение мышью попадает в те же символы с колонкой и без.
    {
        const auto selectedChars = [&](int row) {
            view.verticalScrollBar()->setValue(0);
            testdocs::pump(30);
            const QRect vr = view.visualRect(model.index(row, 0));
            const qreal cw = QFontMetricsF(view.font()).horizontalAdvance(QLatin1Char('M'));
            const int y = vr.top() + vr.height() / 2;
            const QPoint from(view.textLeft() + 4 + int(cw * 3.5), y);
            const QPoint to(view.textLeft() + 4 + int(cw * 11.5), y);
            QTest::mousePress(view.viewport(), Qt::LeftButton, Qt::NoModifier, from);
            QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(view.viewport()->mapToGlobal(to)),
                             Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(view.viewport(), &move);
            QTest::mouseRelease(view.viewport(), Qt::LeftButton, Qt::NoModifier, to);
            QApplication::clipboard()->clear();
            QTest::keyClick(view.viewport(), Qt::Key_C, Qt::ControlModifier);
            QKeyEvent copyKey(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
            QApplication::sendEvent(&view, &copyKey);
            return QApplication::clipboard()->text();
        };
        const QString withColumn = selectedChars(2);
        settings.setFileLabelMode(FileLabelMode::Hidden);
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
        const QString without = selectedChars(2);
        settings.setFileLabelMode(FileLabelMode::Name);
        testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
        CHECK(withColumn.size() == 8 && withColumn == without,
              QStringLiteral("%1: mouse selection hits the same characters ('%2' vs '%3')").arg(label, withColumn, without));
    }

    // Дозапись в B: число строк на чипе растёт.
    testdocs::appendToFile(b, fileLines(QStringLiteral("us-tail"), 15, 200));
    tab.reloadChangedFiles(true);
    CHECK(settledTab(tab) && testdocs::waitFor([&] { return model.rowCount() == 115; }, 10000),
          QStringLiteral("%1: appended lines arrive (%2)").arg(label).arg(model.rowCount()));
    checkLegendCounts(tab, label + " appended");

    // Полная перезагрузка: B переписан короче.
    {
        QFile f(b);
        f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        f.write(fileLines(QStringLiteral("rewritten"), 12, 3));
    }
    tab.reloadChangedFiles(true);
    CHECK(settledTab(tab) && testdocs::waitFor([&] { return model.rowCount() == 72; }, 10000),
          QStringLiteral("%1: full reload (%2 rows)").arg(label).arg(model.rowCount()));
    checkLegendCounts(tab, label + " reloaded");

    // Файл удалён: его строки уходят, файл остаётся во вкладке — чип с
    // нулём строк, колонка не мигает (как и на полной перезагрузке).
    const int columnBefore = view.fileColumnWidth();
    QFile::remove(b);
    tab.reloadChangedFiles(true);
    CHECK(settledTab(tab) && testdocs::waitFor([&] { return model.rowCount() == 60; }, 10000),
          QStringLiteral("%1: removed file's rows go (%2)").arg(label).arg(model.rowCount()));
    testdocs::waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    CHECK(view.fileColumnWidth() == columnBefore && legend->isVisible() && legend->chipCount() == 2
              && legend->chipText(1).endsWith(QStringLiteral(" 0")),
          QStringLiteral("%1: the missing file keeps its chip with 0 lines (%2)").arg(label, legend->chipText(1)));

    // ini: режим сохраняется в [View] fileLabels.
    settings.setFileLabelMode(FileLabelMode::ColorOnly);
    settings.save();
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("View/fileLabels")).toString() == QStringLiteral("colorOnly"),
              "the mode is stored in [View] fileLabels");
    }
    settings.setFileLabelMode(FileLabelMode::Name);
    settings.save();
    settings.setIndexedThresholdMB(512);
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
    checkFileLabelsTable();
    checkMergedTab(dir, false);
    checkMergedTab(dir, true);

    if (failures) {
        std::fprintf(stderr, "row_chrome: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "row_chrome: OK\n");
    return 0;
}
