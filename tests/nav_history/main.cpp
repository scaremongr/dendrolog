// ============================================================================
// nav_history — история переходов (R-7): NavHistory, Alt+← / Alt+→.
//
// Ядро (документ, собранный как во вкладке, testdocuments.h; оба бэкенда):
//   • назад / вперёд, отбрасывание «вперёд» новым переходом;
//   • склейка серии переходов одного вида без движения между ними; другой
//     вид или сдвиг пользователя серию рвут; серия, вернувшаяся к началу;
//   • переход на ту же строку и возврат (NavKind::History) не пишутся;
//   • предел длины истории;
//   • фильтр скрывает место — возврат к ближайшей видимой строке.
// Вкладка (LogViewWidget на живых файлах, резидентная и индексная):
//   • переходы каждого вида (Go to, таймлайн, статистика, Search, F3, F8,
//     F2 и панель закладок, «↓ N новых строк») → назад → вперёд против
//     ожидаемых строк; возврат записей не создаёт и снимает follow-tail;
//   • стрелки и щелчок мышью историю не пишут; боковая кнопка мыши — назад;
//   • дозапись, удаление файла и его возврат с тем же содержимым,
//     переписанный файл (пропавшая строка пропускается);
//   • рост файла за порог — вкладка становится индексной, история на месте;
//   • сочетания Alt+← / Alt+→ свободны и уступают переназначенным.
// ============================================================================

#include "appsettings.h"
#include "bookmarkstore.h"
#include "logviewwidget.h"
#include "navhistory.h"
#include "searchresultscontroller.h"
#include "searchresultsnavigator.h"
#include "shortcutmanager.h"
#include "testdocuments.h"

#include "LogListView.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

using namespace testdocs;

static int failures = 0;
static int checks = 0;
static QString g_context;
#define U8(s) ((s).toUtf8().constData())
#define CHECK(condition, label) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        if (failures < 200) \
            std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n", U8(g_context), \
                         U8(QString(label)), __LINE__); \
    } \
} while (0)

static qint64 lineAt(const LogModel& model, int row)
{
    const RecordKey key = model.keyAt(row);
    return key.isValid() ? key.line : -1;
}

static int rowOfLine(const LogModel& model, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (lineAt(model, row) == line)
            return row;
    return -1;
}

static QString linesOf(const LogModel& model, const NavHistory& h)
{
    Q_UNUSED(model);
    QStringList parts;
    for (int i = 0; i < h.entries().size(); ++i)
        parts << (i == h.position() ? QStringLiteral("[%1]") : QStringLiteral("%1"))
                     .arg(h.entries()[i].line);
    return parts.join(QLatin1Char(' '));
}

// Строки записей истории по порядку.
static QVector<qint64> historyLines(const NavHistory& h)
{
    QVector<qint64> lines;
    for (const NavEntry& e : h.entries())
        lines << e.line;
    return lines;
}

// ---------------------------------------------------------------------------
// Ядро
// ---------------------------------------------------------------------------

static void runCore(const QString& name, Document doc)
{
    g_context = name + QStringLiteral(" / ядро");
    LogModel& model = *doc.model;
    CHECK(doc.errors.isEmpty(), QStringLiteral("документ загружен: ") + doc.errors.join("; "));
    CHECK(model.rowCount() > 400, QStringLiteral("строк достаточно: %1").arg(model.rowCount()));
    if (model.rowCount() <= 400)
        return;
    NavHistory& h = model.navHistory();
    int changes = 0;
    QObject::connect(&h, &NavHistory::changed, &h, [&] { ++changes; });
    const auto L = [&](int row) { return lineAt(model, row); };

    // ---- Пустая история ----------------------------------------------------
    CHECK(!h.canGoBack() && !h.canGoForward() && h.back(-1) == -1 && h.forward(-1) == -1,
          QStringLiteral("пустая история: идти некуда"));

    // ---- Назад / вперёд ----------------------------------------------------
    h.recordJump(10, 50, NavKind::GoTo);
    CHECK(historyLines(h) == QVector<qint64>({ L(10), L(50) }) && h.position() == 1,
          QStringLiteral("переход пишет откуда и куда: ") + linesOf(model, h));
    CHECK(changes == 1, QStringLiteral("changed на запись"));
    CHECK(h.canGoBack() && !h.canGoForward(), QStringLiteral("после перехода — только назад"));
    CHECK(h.back(50) == 10 && h.position() == 0, QStringLiteral("назад — откуда ушли"));
    CHECK(!h.canGoBack() && h.canGoForward(), QStringLiteral("в начале — только вперёд"));
    CHECK(h.back(10) == -1, QStringLiteral("дальше назад некуда"));
    CHECK(h.forward(10) == 50 && h.position() == 1, QStringLiteral("вперёд — куда пришли"));
    CHECK(h.entries().size() == 2, QStringLiteral("возврат записей не создаёт"));

    // ---- Новый переход отбрасывает «вперёд» --------------------------------
    CHECK(h.back(50) == 10, QStringLiteral("снова назад"));
    h.recordJump(10, 80, NavKind::Timeline);
    CHECK(historyLines(h) == QVector<qint64>({ L(10), L(80) }) && !h.canGoForward(),
          QStringLiteral("«вперёд» отброшено: ") + linesOf(model, h));

    // ---- Переход на месте и возврат не пишутся -----------------------------
    const int before = int(h.entries().size());
    h.recordJump(80, 80, NavKind::GoTo);
    h.recordJump(80, 120, NavKind::History);
    CHECK(h.entries().size() == before, QStringLiteral("переход на ту же строку и возврат не пишутся"));

    // ---- Склейка серии -------------------------------------------------------
    h.clear();
    CHECK(h.entries().isEmpty() && !h.canGoBack(), QStringLiteral("clear"));
    h.recordJump(5, 20, NavKind::QuickSearch);
    h.recordJump(20, 30, NavKind::QuickSearch);
    h.recordJump(30, 40, NavKind::QuickSearch);
    CHECK(historyLines(h) == QVector<qint64>({ L(5), L(40) }),
          QStringLiteral("F3, F3, F3 — одна цель: ") + linesOf(model, h));
    h.recordJump(40, 60, NavKind::LevelJump);
    CHECK(historyLines(h) == QVector<qint64>({ L(5), L(40), L(60) }),
          QStringLiteral("другой вид серию рвёт: ") + linesOf(model, h));
    h.recordJump(70, 90, NavKind::LevelJump);
    CHECK(historyLines(h) == QVector<qint64>({ L(5), L(40), L(60), L(70), L(90) }),
          QStringLiteral("сдвиг пользователя серию рвёт и пишет, откуда ушли: ") + linesOf(model, h));
    CHECK(h.back(90) == 70 && h.back(70) == 60 && h.back(60) == 40 && h.back(40) == 5,
          QStringLiteral("назад по всей истории"));
    // С начальной позиции новый переход того же вида не склеивается с ней.
    h.recordJump(5, 25, NavKind::QuickSearch);
    CHECK(historyLines(h) == QVector<qint64>({ L(5), L(25) }),
          QStringLiteral("после возврата — новая ветка: ") + linesOf(model, h));
    h.recordJump(25, 5, NavKind::QuickSearch);
    CHECK(historyLines(h) == QVector<qint64>({ L(5) }) && !h.canGoBack(),
          QStringLiteral("серия вернулась к началу: ") + linesOf(model, h));

    // Разовые переходы (Go to, таймлайн, статистика) не склеиваются.
    h.clear();
    h.recordJump(1, 100, NavKind::GoTo);
    h.recordJump(100, 200, NavKind::GoTo);
    h.recordJump(200, 300, NavKind::Timeline);
    h.recordJump(300, 310, NavKind::Timeline);
    CHECK(historyLines(h) == QVector<qint64>({ L(1), L(100), L(200), L(300), L(310) }),
          QStringLiteral("Go to, Go to — два места: ") + linesOf(model, h));
    // Место, куда вернулись по истории, следующий шаг того же вида не затирает.
    h.clear();
    h.recordJump(1, 10, NavKind::LevelJump);
    h.recordJump(20, 30, NavKind::LevelJump);
    CHECK(h.back(30) == 20 && h.back(20) == 10, QStringLiteral("назад к цели первого F8"));
    h.recordJump(10, 40, NavKind::LevelJump);
    CHECK(historyLines(h) == QVector<qint64>({ L(1), L(10), L(40) }),
          QStringLiteral("F8 после возврата — новая запись: ") + linesOf(model, h));
    CHECK(h.back(40) == 10, QStringLiteral("и назад к месту возврата"));

    // ---- Предел -------------------------------------------------------------
    h.clear();
    for (int i = 0; i < 3 * NavHistory::kMaxEntries; ++i)
        h.recordJump(i, i + 200, i % 2 ? NavKind::GoTo : NavKind::Timeline);
    CHECK(h.entries().size() == NavHistory::kMaxEntries
              && h.position() == NavHistory::kMaxEntries - 1,
          QStringLiteral("не больше %1 записей: %2").arg(NavHistory::kMaxEntries).arg(h.entries().size()));
    const int last = 3 * NavHistory::kMaxEntries - 1;
    CHECK(h.entries().constLast().line == L(last + 200) && h.back(last + 200) == last,
          QStringLiteral("выпадают старые"));

    // ---- Фильтр между переходом и возвратом --------------------------------
    h.clear();
    // Место, откуда ушли, — строка уровня, который фильтр скроет.
    int from = -1;
    for (int row = 100; row < 300 && from < 0; ++row)
        if (model.messageAt(row).contains(QStringLiteral(" INFO ")))
            from = row;
    CHECK(from >= 0, QStringLiteral("есть строка Info"));
    int to = -1;
    for (int row = 350; row < model.rowCount() && to < 0; ++row)
        if (model.messageAt(row).contains(QStringLiteral(" ERROR ")))
            to = row;
    CHECK(to >= 0, QStringLiteral("есть строка Error"));
    if (from < 0 || to < 0)
        return;
    const RecordKey fromKey = model.keyAt(from);
    const RecordKey toKey = model.keyAt(to);
    h.recordJump(from, to, NavKind::Search);
    model.setLogLevelFilter({ LogLevel::Error, LogLevel::Fatal });
    CHECK(settle(model), QStringLiteral("фильтр"));
    const int toNow = model.rowForKey(toKey);
    const int expected = model.nearestRowForKey(fromKey);
    CHECK(model.rowForKey(fromKey) < 0 && expected >= 0, QStringLiteral("место скрыто фильтром"));
    CHECK(h.back(toNow) == expected, QStringLiteral("скрытое место — к ближайшей видимой"));
    model.setLogLevelFilter({});
    CHECK(settle(model), QStringLiteral("фильтр снят"));
    CHECK(h.forward(model.rowForKey(fromKey)) == model.rowForKey(toKey), QStringLiteral("вперёд после снятия"));
    CHECK(h.back(model.rowForKey(toKey)) == model.rowForKey(fromKey), QStringLiteral("назад — точно туда"));
}

// ---------------------------------------------------------------------------
// Вкладка на живых файлах
// ---------------------------------------------------------------------------

static QByteArray record(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

// 200 записей: ERROR — каждая 7-я, «needle» — каждая 13-я, продолжение
// многострочной записи после каждой 10-й (i % 10 == 5).
static QByteArray tabFixture(int records = 200, const QByteArray& prefix = "line ")
{
    QByteArray bytes;
    for (int i = 0; i < records; ++i) {
        QByteArray text = prefix + QByteArray::number(i);
        if (i % 13 == 6)
            text += " needle";
        bytes += record(i, i % 7 == 3 ? "ERROR" : "INFO", text);
        if (i % 10 == 5)
            bytes += "    at continuation of " + QByteArray::number(i) + '\n';
    }
    return bytes;
}

static bool settled(LogViewWidget& tab)
{
    return waitFor([&] { return !tab.isLoading(); }, 60000) && settle(*tab.model());
}

static void reload(LogViewWidget& tab)
{
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), QStringLiteral("перезагрузка закончилась"));
    pump(5);
}

static bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

// «Щелчок пользователя»: строка выбрана, история не пишется.
static void select(LogViewWidget& tab, int row)
{
    tab.view()->goToRow(row, NavKind::History);
}

static qint64 selectedLine(LogViewWidget& tab)
{
    const int row = tab.view()->userSelectedRow();
    return row >= 0 ? lineAt(*tab.model(), row) : -1;
}

// Переход jump из строки originLine должен прийти в targetLine; затем Alt+←
// возвращает к originLine, Alt+→ — снова к targetLine, без новых записей.
static void checkRoundTrip(LogViewWidget& tab, const QString& what, qint64 originLine,
                           const std::function<void()>& jump, qint64 targetLine)
{
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();
    h.clear();
    select(tab, rowOfLine(model, originLine));
    CHECK(selectedLine(tab) == originLine, what + QStringLiteral(": выбрана строка %1").arg(originLine));
    CHECK(h.entries().isEmpty(), what + QStringLiteral(": выбор строки историю не пишет"));
    jump();
    CHECK(selectedLine(tab) == targetLine,
          what + QStringLiteral(": переход к %1, а пришли к %2").arg(targetLine).arg(selectedLine(tab)));
    CHECK(historyLines(h) == QVector<qint64>({ originLine, targetLine }),
          what + QStringLiteral(": история ") + linesOf(model, h));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == originLine,
          what + QStringLiteral(": Alt+← — к %1, а не %2").arg(originLine).arg(selectedLine(tab)));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == targetLine,
          what + QStringLiteral(": Alt+→ — к %1, а не %2").arg(targetLine).arg(selectedLine(tab)));
    CHECK(h.entries().size() == 2, what + QStringLiteral(": возврат записей не создаёт"));
}

static void checkTab(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / вкладка");
    const QByteArray bytes = tabFixture();
    const QString path = writeFile(dir, name + "-tab.log", bytes);
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setShowFields(true);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), QStringLiteral("загрузка"));
    pump(100);
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    NavHistory& h = tab.navHistory();
    CHECK(model.isIndexedBackend() == name.startsWith("indexed"), QStringLiteral("бэкенд"));

    // Строка записи i: i + 1 + число продолжений до неё.
    const auto lineOfRecord = [](int i) { return qint64(i + 1 + (i + 4) / 10); };
    CHECK(model.messageAt(rowOfLine(model, lineOfRecord(40))).endsWith("line 40"),
          QStringLiteral("нумерация фикстуры"));

    int historySignals = 0;
    QObject::connect(&tab, &LogViewWidget::navHistoryChanged, &tab, [&] { ++historySignals; });

    // ---- Переходы каждого вида ----------------------------------------------
    const qint64 origin = lineOfRecord(20);
    checkRoundTrip(tab, QStringLiteral("Go to"), origin, [&] {
        view.goToRow(rowOfLine(model, lineOfRecord(150)), NavKind::GoTo);
    }, lineOfRecord(150));
    CHECK(historySignals > 0, QStringLiteral("вкладка сообщает об изменении истории"));
    checkRoundTrip(tab, QStringLiteral("таймлайн"), origin, [&] {
        view.goToRow(rowOfLine(model, lineOfRecord(90)), NavKind::Timeline);
    }, lineOfRecord(90));
    checkRoundTrip(tab, QStringLiteral("статистика / Search"), origin, [&] {
        const int row = rowOfLine(model, lineOfRecord(120));
        CHECK(tab.goToEntry(model.keyAt(row).logicalEntryId, model.keyAt(row).file.get(),
                            NavKind::Search),
              QStringLiteral("goToEntry"));
    }, lineOfRecord(120));
    checkRoundTrip(tab, QStringLiteral("F3"), origin, [&] {
        bool done = false;
        auto c = QObject::connect(&tab, &LogViewWidget::quickSearchFinished, &tab,
                                  [&](const QString&, bool) { done = true; });
        tab.searchTextNext(TextQuery(QStringLiteral("needle"), Qt::CaseInsensitive));
        CHECK(waitFor([&] { return done; }, 10000), QStringLiteral("F3 закончился"));
        QObject::disconnect(c);
    }, lineOfRecord(32));
    checkRoundTrip(tab, QStringLiteral("F8"), origin, [&] {
        bool done = false;
        auto c = QObject::connect(&tab, &LogViewWidget::levelJumpFinished, &tab,
                                  [&](LogLevelMask, LogViewWidget::LevelJumpOutcome) { done = true; });
        tab.goToLevel(levelBit(LogLevel::Error) | levelBit(LogLevel::Fatal), true);
        CHECK(waitFor([&] { return done; }, 10000), QStringLiteral("F8 закончился"));
        QObject::disconnect(c);
    }, lineOfRecord(24));
    const int bookmarkRow = rowOfLine(model, lineOfRecord(60));
    CHECK(tab.toggleBookmark(bookmarkRow), QStringLiteral("закладка"));
    checkRoundTrip(tab, QStringLiteral("F2"), origin, [&] { tab.goToBookmark(true); },
                   lineOfRecord(60));
    checkRoundTrip(tab, QStringLiteral("панель закладок"), origin, [&] {
        CHECK(tab.goToBookmarkId(tab.bookmarks().idAtRow(bookmarkRow)), QStringLiteral("к закладке"));
    }, lineOfRecord(60));
    tab.bookmarks().clear();

    // ---- Склейка на вкладке: F3, F3, F3 — один Alt+← ------------------------
    h.clear();
    select(tab, rowOfLine(model, origin));
    for (int i = 0; i < 3; ++i) {
        bool done = false;
        auto c = QObject::connect(&tab, &LogViewWidget::quickSearchFinished, &tab,
                                  [&](const QString&, bool) { done = true; });
        tab.searchTextNext(TextQuery(QStringLiteral("needle"), Qt::CaseInsensitive));
        CHECK(waitFor([&] { return done; }, 10000), QStringLiteral("F3 закончился"));
        QObject::disconnect(c);
    }
    CHECK(selectedLine(tab) == lineOfRecord(58), QStringLiteral("третья находка"));
    CHECK(h.entries().size() == 2, QStringLiteral("серия F3 — две записи: ") + linesOf(model, h));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == origin, QStringLiteral("один Alt+← после серии"));

    // ---- Стрелки и мышь историю не пишут; боковая кнопка — назад -----------
    h.clear();
    select(tab, rowOfLine(model, origin));
    view.goToRow(rowOfLine(model, lineOfRecord(100)), NavKind::GoTo);
    const int entries = int(h.entries().size());
    view.setFocus();
    for (int i = 0; i < 3; ++i) {
        QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
        QApplication::sendEvent(&view, &down);
    }
    const QRect r1 = view.visualRect(model.index(view.userSelectedRow()));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(r1.center()), view.viewport()->mapToGlobal(QPointF(r1.center())),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(r1.center()), view.viewport()->mapToGlobal(QPointF(r1.center())),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &release);
    view.verticalScrollBar()->setValue(0);
    pump(20);
    CHECK(h.entries().size() == entries, QStringLiteral("стрелки, щелчок и прокрутка историю не пишут"));
    CHECK(selectedLine(tab) != lineOfRecord(100), QStringLiteral("стрелки сдвинули выбор"));
    QMouseEvent back(QEvent::MouseButtonPress, QPointF(10, 10), view.viewport()->mapToGlobal(QPointF(10, 10)),
                     Qt::BackButton, Qt::BackButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &back);
    CHECK(selectedLine(tab) == origin, QStringLiteral("кнопка мыши «назад»"));
    QMouseEvent fwd(QEvent::MouseButtonPress, QPointF(10, 10), view.viewport()->mapToGlobal(QPointF(10, 10)),
                    Qt::ForwardButton, Qt::ForwardButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &fwd);
    CHECK(selectedLine(tab) == lineOfRecord(100), QStringLiteral("кнопка мыши «вперёд»"));

    // ---- Без выбранной строки «откуда» — первая строка экрана ----------------
    h.clear();
    view.clearSelection();
    view.verticalScrollBar()->setValue(0);
    pump(20);
    CHECK(view.userSelectedRow() < 0, QStringLiteral("выбора нет"));
    view.goToRow(rowOfLine(model, lineOfRecord(150)), NavKind::GoTo);
    CHECK(h.entries().size() == 2 && h.entries().constFirst().line == lineAt(model, 0),
          QStringLiteral("откуда — первая строка экрана: ") + linesOf(model, h));

    // ---- Follow-tail снят после возврата -------------------------------------
    view.setFollowTail(true);
    pump(20);
    CHECK(view.followTail(), QStringLiteral("follow-tail включён"));
    CHECK(tab.navigateHistory(false), QStringLiteral("Alt+← при follow-tail"));
    CHECK(!view.followTail() && selectedLine(tab) == lineAt(model, 0),
          QStringLiteral("возврат снимает follow-tail"));

    // ---- Фильтр между переходом и возвратом -----------------------------------
    h.clear();
    select(tab, rowOfLine(model, origin));                 // запись 20 — INFO
    view.goToRow(rowOfLine(model, lineOfRecord(108)), NavKind::GoTo);  // запись 108 — ERROR
    const RecordKey originKey = model.keyAt(rowOfLine(model, origin));
    model.setLogLevelFilter({ LogLevel::Error });
    CHECK(settled(tab), QStringLiteral("фильтр"));
    CHECK(model.rowForKey(originKey) < 0, QStringLiteral("место скрыто"));
    CHECK(tab.navigateHistory(false) && view.userSelectedRow() == model.nearestRowForKey(originKey),
          QStringLiteral("скрытое место — к ближайшей видимой"));
    model.setLogLevelFilter({});
    CHECK(settled(tab), QStringLiteral("фильтр снят"));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == lineOfRecord(108), QStringLiteral("вперёд после фильтра"));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == origin, QStringLiteral("назад после фильтра — точно"));

    // ---- «↓ N новых строк» и дозапись -------------------------------------------
    g_context = name + QStringLiteral(" / дозапись");
    view.setFollowTail(false);
    view.verticalScrollBar()->setValue(0);
    pump(20);
    h.clear();
    select(tab, rowOfLine(model, origin));
    view.verticalScrollBar()->setValue(0);
    pump(20);
    const qint64 linesBefore = lineAt(model, model.rowCount() - 1);
    QByteArray tail;
    for (int i = 200; i < 205; ++i)
        tail += record(i, "INFO", "tail " + QByteArray::number(i));
    CHECK(appendToFile(path, tail), QStringLiteral("дозапись"));
    reload(tab);
    CHECK(h.entries().isEmpty(), QStringLiteral("дозапись историю не пишет"));
    CHECK(view.firstNewLineRow() >= 0, QStringLiteral("новые строки ниже экрана"));
    view.goToNewLines();
    CHECK(selectedLine(tab) == linesBefore + 1, QStringLiteral("к первой новой строке"));
    CHECK(historyLines(h) == QVector<qint64>({ origin, linesBefore + 1 }),
          QStringLiteral("«↓ N новых строк» — в истории: ") + linesOf(model, h));
    CHECK(appendToFile(path, record(206, "INFO", "more")), QStringLiteral("ещё дозапись"));
    reload(tab);
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == origin, QStringLiteral("назад после дозаписи"));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == linesBefore + 1, QStringLiteral("вперёд после дозаписи"));

    // ---- Файл удалён и создан заново с тем же содержимым ----------------------
    g_context = name + QStringLiteral(" / перезагрузка");
    QFile current(path);
    CHECK(current.open(QIODevice::ReadOnly), QStringLiteral("чтение файла"));
    const QByteArray content = current.readAll();
    current.close();
    h.clear();
    select(tab, rowOfLine(model, lineOfRecord(30)));
    view.goToRow(rowOfLine(model, lineOfRecord(70)), NavKind::GoTo);
    view.goToRow(rowOfLine(model, lineOfRecord(140)), NavKind::Timeline);
    CHECK(historyLines(h) == QVector<qint64>({ lineOfRecord(30), lineOfRecord(70), lineOfRecord(140) }),
          QStringLiteral("три места: ") + linesOf(model, h));
    CHECK(QFile::remove(path), QStringLiteral("файл удалён"));
    reload(tab);
    CHECK(model.rowCount() == 0, QStringLiteral("строки удалённого файла сброшены"));
    CHECK(!tab.navigateHistory(false), QStringLiteral("файла нет — идти некуда"));
    CHECK(writeBytes(path, content), QStringLiteral("файл создан заново"));
    reload(tab);
    CHECK(model.rowCount() > 0, QStringLiteral("файл прочитан заново"));
    select(tab, rowOfLine(model, lineOfRecord(140)));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == lineOfRecord(70),
          QStringLiteral("после перезагрузки — назад к 70: %1").arg(selectedLine(tab)));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == lineOfRecord(30), QStringLiteral("и к 30"));
    CHECK(tab.navigateHistory(true) && tab.navigateHistory(true)
              && selectedLine(tab) == lineOfRecord(140),
          QStringLiteral("и вперёд к 140"));

    // ---- Переписанный файл: пропавшая строка пропускается -------------------
    g_context = name + QStringLiteral(" / переписан");
    // Записи до 50 те же, дальше другой текст: место 70 стало чужой строкой.
    QByteArray rewritten = tabFixture(50);
    QByteArray other = tabFixture(200, "other ");
    // хвост «other» начиная с записи 50
    const int cut = other.indexOf(record(50, "INFO", "other 50"));
    CHECK(cut > 0, QStringLiteral("фикстура переписанного файла"));
    rewritten += other.mid(cut);
    CHECK(writeBytes(path, rewritten), QStringLiteral("переписан"));
    reload(tab);
    const int at140 = rowOfLine(model, lineOfRecord(140));
    CHECK(at140 >= 0 && model.messageAt(at140).contains("other"), QStringLiteral("строка 140 теперь другая"));
    select(tab, at140);
    const int before = int(h.entries().size());
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == lineOfRecord(30),
          QStringLiteral("пропавшее место 70 пропущено — к 30, а пришли к %1").arg(selectedLine(tab)));
    CHECK(!tab.navigateHistory(false), QStringLiteral("дальше назад некуда"));
    CHECK(!tab.navigateHistory(true) && selectedLine(tab) == lineOfRecord(30),
          QStringLiteral("вперёд — только пропавшие: остаёмся"));
    CHECK(h.entries().size() == before, QStringLiteral("возврат записей не создаёт"));
    CHECK(!h.canGoBack() && !h.canGoForward(), QStringLiteral("пропавшие места — пункты недоступны"));

    // Прежнее содержимое вернули: пропавшее место снова находится.
    CHECK(writeBytes(path, content), QStringLiteral("содержимое возвращено"));
    reload(tab);
    CHECK(h.canGoForward(), QStringLiteral("после перезагрузки «вперёд» снова доступно"));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == lineOfRecord(70),
          QStringLiteral("место 70 нашлось после возврата содержимого: %1").arg(selectedLine(tab)));
}

// Рост файла за порог: вкладка конвертируется в индексную, история на месте.
static void checkConversion(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("переход порога");
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(1);
    QByteArray start;
    for (int i = 0; i < 2000; ++i)
        start += record(i / 50, "INFO", "line " + QByteArray::number(i));
    const QString path = writeFile(dir, "grows.log", start);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setShowFields(true);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    CHECK(!model.isIndexedBackend(), QStringLiteral("начинает резидентной"));
    select(tab, 7);
    tab.view()->goToRow(999, NavKind::GoTo);
    tab.view()->goToRow(1500, NavKind::Timeline);
    const QVector<qint64> lines = historyLines(tab.navHistory());
    CHECK(lines == QVector<qint64>({ 8, 1000, 1501 }), QStringLiteral("три места"));

    QByteArray more;
    for (int i = 2000; i < 30000; ++i)
        more += record(i / 50, "INFO", "line " + QByteArray::number(i));
    CHECK(appendToFile(path, more), QStringLiteral("дозапись за порог"));
    reload(tab);
    CHECK(model.isIndexedBackend(), QStringLiteral("вкладка стала индексной"));
    CHECK(historyLines(tab.navHistory()) == lines, QStringLiteral("история пережила конверсию"));
    select(tab, rowOfLine(model, 1501));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == 1000, QStringLiteral("назад к 1000"));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == 8, QStringLiteral("назад к 8"));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == 1000, QStringLiteral("вперёд к 1000"));
    settings.setIndexedThresholdMB(512);
}

// Смена кодировки (F-1, шаг 4): фрагменты записей истории переписываются
// новым текстом — после полной перезагрузки тем же выбранным декодером
// места не пропадают.
static void checkEncodingChange(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / смена кодировки");
    QString text;
    for (int i = 0; i < 60; ++i)
        text += QString::fromLatin1(record(i, "INFO", QByteArray()).chopped(1))
              + QStringLiteral("Запись номер %1: ошибка диска\n").arg(i);
    QByteArray bytes = encodeText(text, TextEncoding::Windows1251);
    const QString path = writeFile(dir, name + "-cp1251.log", bytes);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();
    const LogFilePtr file = tab.loadedFiles().value(0);
    select(tab, rowOfLine(model, 5));
    tab.view()->goToRow(rowOfLine(model, 40), NavKind::GoTo);
    CHECK(historyLines(h) == QVector<qint64>({ 5, 40 }), QStringLiteral("два места"));

    tab.setFileEncoding(file, TextEncoding::Koi8R);
    CHECK(settled(tab), QStringLiteral("смена на KOI8-R"));
    for (const NavEntry& e : h.entries()) {
        const int row = rowOfLine(model, e.line);
        CHECK(row >= 0 && e.preview == model.messageAt(row).left(NavHistory::kPreviewLength),
              QStringLiteral("фрагмент строки %1 — новый текст: %2").arg(e.line).arg(e.preview));
    }

    bytes[bytes.size() - 2] = 'X';
    CHECK(writeBytes(path, bytes), QStringLiteral("переписан"));
    reload(tab);
    select(tab, rowOfLine(model, 40));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == 5,
          QStringLiteral("после перезагрузки Alt+← — к строке 5, а не %1").arg(selectedLine(tab)));
    bool anyLost = false;
    for (const NavEntry& e : h.entries())
        anyLost = anyLost || e.lost;
    CHECK(!anyLost, QStringLiteral("ни одно место не пропало"));
}

// Закрытие вкладки отбрасывает историю: она у модели, модель — у вкладки.
static void checkTabClose(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("закрытие вкладки");
    const QString path = writeFile(dir, "close.log", tabFixture(50));
    auto tab = std::make_unique<LogViewWidget>();
    tab->setParserPattern(schema);
    tab->addLogFile(path);
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    select(*tab, 3);
    tab->view()->goToRow(30, NavKind::GoTo);
    QPointer<NavHistory> history(&tab->navHistory());
    CHECK(history && history->canGoBack(), QStringLiteral("история есть"));
    tab.reset();
    CHECK(!history, QStringLiteral("история ушла вместе с вкладкой"));
}

// Общая обвязка мест (M-8, LocatedPlaces): закладки и история одной вкладки
// стоят на одних строках и делят RecordLocator модели. Ответ локатора
// доходит до своего владельца и только до него: после удаления и возврата
// файла, переписывания и возврата содержимого статус закладки и запись
// истории одной строки совпадают; снятая закладка и очищенная история
// чужие места не трогают.
static void checkSharedPlaces(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / закладки и история");
    const QByteArray content = tabFixture();
    const QString path = writeFile(dir, name + "-shared.log", content);
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();
    BookmarkStore& bm = tab.bookmarks();
    const auto lineOfRecord = [](int i) { return qint64(i + 1 + (i + 4) / 10); };
    const qint64 lines[] = { lineOfRecord(30), lineOfRecord(70), lineOfRecord(140) };

    select(tab, rowOfLine(model, lines[0]));
    tab.view()->goToRow(rowOfLine(model, lines[1]), NavKind::GoTo);
    tab.view()->goToRow(rowOfLine(model, lines[2]), NavKind::Timeline);
    int ids[3];
    for (int i = 0; i < 3; ++i)
        ids[i] = bm.add(rowOfLine(model, lines[i]));
    CHECK(historyLines(h) == QVector<qint64>({ lines[0], lines[1], lines[2] }),
          QStringLiteral("три места истории: ") + linesOf(model, h));

    // Состояние места истории на строке line: 1 — сверено, 0 — ищется,
    // -1 — пропало.
    const auto historyState = [&](qint64 line) {
        for (const NavEntry& e : h.entries())
            if (e.line == line)
                return e.lost ? -1 : e.verified ? 1 : 0;
        return -2;
    };
    const auto bookmarkState = [&](int id) {
        switch (bm.status(id)) {
        case BookmarkStore::Status::Visible:
        case BookmarkStore::Status::Hidden:
            return 1;
        case BookmarkStore::Status::Pending:
            return 0;
        case BookmarkStore::Status::Missing:
            return -1;
        }
        return -2;
    };
    const auto expectStates = [&](const QString& what, const int (&expected)[3]) {
        for (int i = 0; i < 3; ++i) {
            CHECK(historyState(lines[i]) == expected[i],
                  what + QStringLiteral(": история, строка %1 — %2, а не %3")
                             .arg(lines[i]).arg(expected[i]).arg(historyState(lines[i])));
            CHECK(bookmarkState(ids[i]) == expected[i],
                  what + QStringLiteral(": закладка, строка %1 — %2, а не %3")
                             .arg(lines[i]).arg(expected[i]).arg(bookmarkState(ids[i])));
            const int row = bm.visibleRow(ids[i]);
            if (expected[i] == 1 && row >= 0) {
                const Bookmark* b = bm.find(ids[i]);
                CHECK(b && b->line == lines[i]
                          && b->preview == model.messageAt(row).left(BookmarkStore::kPreviewLength),
                      what + QStringLiteral(": закладка на своей строке %1").arg(lines[i]));
            }
        }
    };
    expectStates(QStringLiteral("поставлены"), { 1, 1, 1 });

    CHECK(QFile::remove(path), QStringLiteral("файл удалён"));
    reload(tab);
    expectStates(QStringLiteral("файла нет"), { 0, 0, 0 });
    CHECK(writeBytes(path, content), QStringLiteral("файл создан заново"));
    reload(tab);
    expectStates(QStringLiteral("файл вернули"), { 1, 1, 1 });

    // Записи до 50 те же, дальше другой текст: места 70 и 140 — чужие строки.
    QByteArray rewritten = tabFixture(50);
    const QByteArray other = tabFixture(200, "other ");
    const int cut = other.indexOf(record(50, "INFO", "other 50"));
    CHECK(cut > 0, QStringLiteral("фикстура переписанного файла"));
    rewritten += other.mid(cut);
    CHECK(writeBytes(path, rewritten), QStringLiteral("переписан"));
    reload(tab);
    expectStates(QStringLiteral("переписан"), { 1, -1, -1 });

    // Закладку 140 сняли, историю не трогали: место истории 140 по-прежнему
    // ищется при возврате содержимого и находится.
    CHECK(bm.remove(ids[2]), QStringLiteral("закладка снята"));
    CHECK(writeBytes(path, content), QStringLiteral("содержимое возвращено"));
    reload(tab);
    CHECK(historyState(lines[1]) == 1 && historyState(lines[2]) == 1,
          QStringLiteral("места истории нашлись после возврата содержимого"));
    CHECK(bookmarkState(ids[1]) == 1 && bm.find(ids[2]) == nullptr,
          QStringLiteral("закладка 70 нашлась, снятая не вернулась"));

    // История очищена — закладки живут дальше и после следующего сброса.
    h.clear();
    CHECK(QFile::remove(path) && writeBytes(path, content), QStringLiteral("файл пересоздан"));
    reload(tab);
    CHECK(bookmarkState(ids[0]) == 1 && bookmarkState(ids[1]) == 1 && bm.count() == 2,
          QStringLiteral("закладки после очистки истории"));
}

// Переписанный файл сдвинул границы записей (ключи строк другие), а место
// истории скрыто фильтром, когда загрузка кончилась. Строку находит
// RecordLocator — по номеру и тексту среди ВСЕХ строк порции, — и возврат
// ведёт к ближайшей видимой, а не пропускает место.
static void checkHiddenAtReload(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / скрыто фильтром при перезагрузке");
    const auto lineOfRecord = [](int i) { return qint64(i + 1 + (i + 4) / 10); };
    const QByteArray before = tabFixture(60);
    QByteArray after = before;
    after.replace("    at continuation of 5\n", record(5, "INFO", "inserted record"));
    CHECK(after.size() != before.size() && after.count('\n') == before.count('\n'),
          QStringLiteral("фикстура: строк столько же, запись вставлена"));
    const QString path = writeFile(dir, name + "-hidden.log", before);
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();
    const qint64 hiddenLine = lineOfRecord(31); // ERROR
    select(tab, rowOfLine(model, lineOfRecord(20)));
    tab.view()->goToRow(rowOfLine(model, hiddenLine), NavKind::GoTo);
    tab.view()->goToRow(rowOfLine(model, lineOfRecord(40)), NavKind::Timeline);
    CHECK(historyLines(h) == QVector<qint64>({ lineOfRecord(20), hiddenLine, lineOfRecord(40) }),
          QStringLiteral("три места: ") + linesOf(model, h));
    const RecordKey oldKey = h.entries().value(1).key;

    model.setLogLevelFilter({ LogLevel::Info });
    CHECK(settled(tab), QStringLiteral("фильтр"));
    CHECK(writeBytes(path, after), QStringLiteral("переписан"));
    reload(tab);
    const NavEntry hidden = h.entries().value(1);
    CHECK(hidden.verified && !hidden.lost && hidden.key.line == hiddenLine,
          QStringLiteral("место найдено по номеру и тексту, хотя скрыто"));
    CHECK(hidden.key.logicalEntryId != oldKey.logicalEntryId,
          QStringLiteral("сценарий: ключ строки сменился"));
    CHECK(model.rowForKey(hidden.key) < 0, QStringLiteral("строка скрыта фильтром"));
    select(tab, rowOfLine(model, lineOfRecord(40)));
    const int expected = model.nearestRowForKey(hidden.key);
    CHECK(tab.navigateHistory(false) && tab.view()->userSelectedRow() == expected && expected >= 0,
          QStringLiteral("Alt+← — к ближайшей видимой у скрытого места, а пришли к строке %1")
              .arg(selectedLine(tab)));
    model.setLogLevelFilter({});
    CHECK(settled(tab), QStringLiteral("фильтр снят"));
    CHECK(tab.navigateHistory(true) && selectedLine(tab) == lineOfRecord(40), QStringLiteral("вперёд"));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == hiddenLine,
          QStringLiteral("без фильтра — точно на место: %1").arg(selectedLine(tab)));
}

// Место истории на строке, скрытой фильтром, когда сменилась кодировка: после
// полной перезагрузки его находит отпечаток строки, а не фрагмент.
static void checkHiddenEncodingChange(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / скрыто фильтром при смене кодировки");
    QString text;
    for (int i = 0; i < 60; ++i)
        text += QString::fromLatin1(record(i, i % 5 ? "INFO" : "ERROR", QByteArray()).chopped(1))
              + QStringLiteral("Запись номер %1: ошибка диска\n").arg(i);
    QByteArray bytes = encodeText(text, TextEncoding::Windows1251);
    const QString path = writeFile(dir, name + "-hidden-cp1251.log", bytes);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();
    const LogFilePtr file = tab.loadedFiles().value(0);
    select(tab, rowOfLine(model, 7));                      // INFO — будет скрыта
    tab.view()->goToRow(rowOfLine(model, 41), NavKind::GoTo); // ERROR
    CHECK(historyLines(h) == QVector<qint64>({ 7, 41 }), QStringLiteral("два места"));
    model.setLogLevelFilter({ LogLevel::Error });
    CHECK(settled(tab), QStringLiteral("фильтр"));
    tab.setFileEncoding(file, TextEncoding::Koi8R);
    CHECK(settled(tab), QStringLiteral("смена на KOI8-R"));
    bytes[bytes.size() - 2] = 'X';
    CHECK(writeBytes(path, bytes), QStringLiteral("переписан"));
    reload(tab);
    const NavEntry hidden = h.entries().value(0);
    CHECK(hidden.verified && !hidden.lost && hidden.key.line == 7,
          QStringLiteral("скрытое место найдено после перезагрузки"));
    model.setLogLevelFilter({});
    CHECK(settled(tab), QStringLiteral("фильтр снят"));
    select(tab, rowOfLine(model, 41));
    CHECK(tab.navigateHistory(false) && selectedLine(tab) == 7,
          QStringLiteral("Alt+← — к строке 7, а не %1").arg(selectedLine(tab)));
}

// Выдача Search перестраивается сама (фильтр вкладки, дозапись) и на reset
// восстанавливает своё выделение — это не выбор пользователя: переход во
// вкладке и запись в историю делать нельзя (SearchResultsNavigator).
static void checkSearchResultsQuiet(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("выдача Search не пишет историю");
    const QString path = writeFile(dir, "search-quiet.log", tabFixture(200));
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    NavHistory& h = tab.navHistory();

    SearchResultsController search;
    search.setSource(&model);
    search.setLive(true);
    LogListView results;
    results.resize(600, 300);
    SearchResultsNavigator nav(search.model(), &results);
    nav.setTarget([&tab]() { return &tab; });
    results.show();
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = QStringLiteral("needle");
    rules.rules.append(rule);
    rules.bindFields({});
    search.search(rules);
    CHECK(waitFor([&] { return search.model()->rowCount() >= 10 && !search.model()->isFiltering(); }, 10000),
          QStringLiteral("выдача готова"));
    CHECK(nav.navigations() == 0 && h.entries().isEmpty(), QStringLiteral("поиск сам никуда не ведёт"));

    // Пользователь выбрал совпадение — переход во вкладке, запись в историю.
    results.goToRow(2, NavKind::History);
    pump(20);
    CHECK(nav.navigations() == 1, QStringLiteral("выбор в выдаче — переход"));
    const int mainRow = tab.view()->userSelectedRow();
    CHECK(mainRow >= 0 && model.messageAt(mainRow).contains(QStringLiteral("needle")),
          QStringLiteral("во вкладке выбрано совпадение"));
    const RecordKey mainKey = model.keyAt(mainRow);
    const QVector<qint64> before = historyLines(h);
    CHECK(!before.isEmpty(), QStringLiteral("переход записан в историю"));
    const QString chosen = search.model()->messageAt(results.userSelectedRow());

    // Выдача перестраивается полным поиском (фильтр вкладки сменился) и
    // восстанавливает своё выделение.
    int resets = 0;
    int restored = 0;
    QObject relay;
    QObject::connect(search.model(), &QAbstractItemModel::modelReset, &relay, [&] { ++resets; });
    QObject::connect(results.selectionModel(), &QItemSelectionModel::currentRowChanged, &relay,
                     [&](const QModelIndex& cur, const QModelIndex&) { if (cur.isValid()) ++restored; });
    model.setLogLevelFilter({ LogLevel::Info });
    CHECK(settled(tab), QStringLiteral("фильтр вкладки"));
    CHECK(waitFor([&] { return resets > 0 && !search.model()->isFiltering() && !search.isStale(); }, 10000),
          QStringLiteral("выдача перестроена"));
    pump(SearchResultsController::kRefreshDelayMs + 100);
    CHECK(results.userSelectedRow() >= 0
              && search.model()->messageAt(results.userSelectedRow()) == chosen,
          QStringLiteral("выдача восстановила выделение"));
    CHECK(restored > 0, QStringLiteral("сценарий: восстановление дёрнуло currentRowChanged"));
    CHECK(nav.navigations() == 1, QStringLiteral("перестройка выдачи — не переход (%1)").arg(nav.navigations()));
    CHECK(historyLines(h) == before, QStringLiteral("история не тронута: ") + linesOf(model, h));
    CHECK(tab.view()->userSelectedRow() >= 0 && model.keyAt(tab.view()->userSelectedRow()) == mainKey,
          QStringLiteral("основной вид не сдвинут"));
}

static void checkShortcuts()
{
    g_context = QStringLiteral("сочетания");
    ShortcutManager& sm = ShortcutManager::instance();
    CHECK(sm.sequence(QStringLiteral("navigateBack")) == QKeySequence(Qt::ALT | Qt::Key_Left),
          QStringLiteral("Alt+← — назад"));
    CHECK(sm.sequence(QStringLiteral("navigateForward")) == QKeySequence(Qt::ALT | Qt::Key_Right),
          QStringLiteral("Alt+→ — вперёд"));
    CHECK(sm.sequence(QStringLiteral("reopenTab")) == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T),
          QStringLiteral("Ctrl+Shift+T — вернуть закрытую вкладку (R-5)"));
    QSet<QString> seen;
    for (const ShortcutManager::Command& c : sm.commands()) {
        if (c.defaultSeq.isEmpty())
            continue;
        const QString s = c.defaultSeq.toString(QKeySequence::PortableText);
        CHECK(!seen.contains(s), QStringLiteral("сочетание по умолчанию %1 у двух команд").arg(s));
        seen.insert(s);
    }
    // Пользователь отдал Alt+← другой команде: новая команда клавишу уступает.
    sm.setSequence(QStringLiteral("goTo"), QKeySequence(Qt::ALT | Qt::Key_Left));
    CHECK(sm.sequence(QStringLiteral("navigateBack")).isEmpty()
              && sm.sequence(QStringLiteral("goTo")) == QKeySequence(Qt::ALT | Qt::Key_Left),
          QStringLiteral("переназначенное пользователем сочетание остаётся у его команды"));
    sm.setSequence(QStringLiteral("goTo"), sm.defaultSequence(QStringLiteral("goTo")));
    CHECK(sm.sequence(QStringLiteral("navigateBack")) == QKeySequence(Qt::ALT | Qt::Key_Left),
          QStringLiteral("переназначение снято — сочетание вернулось"));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "нет временного каталога\n");
        return 2;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();
    const QDateTime base = fixtureBaseTime();
    const QString one = writeFile(dir, QStringLiteral("one.log"),
        makeLogBytes(1500, 41u, "\n", base, true, true, true));
    const QString two = writeFile(dir, QStringLiteral("two.log"),
        makeLogBytes(700, 42u, "\r\n", base.addSecs(200), false, true, false));

    runCore(QStringLiteral("резидентный"), buildResident({ one }, schema));
    runCore(QStringLiteral("индексный"), buildIndexed({ one }, schema));
    runCore(QStringLiteral("резидентный, два файла"), buildResident({ one, two }, schema));
    runCore(QStringLiteral("индексный, два файла"),
            buildIndexed({ one, two }, schema, IndexedLoad::Concurrent));

    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(512);
    checkTab(dir, QStringLiteral("resident"), schema);
    checkEncodingChange(dir, QStringLiteral("resident"), schema);
    checkHiddenAtReload(dir, QStringLiteral("resident"), schema);
    checkHiddenEncodingChange(dir, QStringLiteral("resident"), schema);
    checkSearchResultsQuiet(dir, schema);
    checkSharedPlaces(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkTab(dir, QStringLiteral("indexed"), schema);
    checkEncodingChange(dir, QStringLiteral("indexed"), schema);
    checkHiddenAtReload(dir, QStringLiteral("indexed"), schema);
    checkHiddenEncodingChange(dir, QStringLiteral("indexed"), schema);
    checkSharedPlaces(dir, QStringLiteral("indexed"), schema);
    settings.setIndexedThresholdMB(512);
    checkConversion(dir, schema);
    checkTabClose(dir, schema);
    checkShortcuts();

    std::fprintf(failures ? stderr : stdout, "nav_history: %d проверок, %d нарушений\n",
                 checks, failures);
    return failures ? 1 : 0;
}
