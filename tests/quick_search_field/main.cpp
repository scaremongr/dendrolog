// ============================================================================
// quick_search_field — поле быстрого поиска в тулбаре (QuickSearchField):
//   • запрос собирается из текста и переключателей «Aa» / слово / «.*»;
//   • невалидный регекс — красная рамка и сообщение с позицией в подсказке,
//     queryError() не пуст; правка или выключенный «.*» снимают ошибку;
//   • «не найдено» снимается правкой запроса и переключением флагов;
//   • история: remember() — свежий первым, без дублей; стрелка вниз открывает
//     список, выбор пункта ставит запрос и сигналит historyActivated, пункт
//     «Clear search history» очищает список;
//   • переключатели и история переживают перезапуск (AppSettings, ini).
// ============================================================================

#include "appsettings.h"
#include "quicksearchfield.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QMenu>
#include <QStandardPaths>
#include <QStyleFactory>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static void pressDown(QWidget* widget, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Down, mods);
    QCoreApplication::sendEvent(widget, &press);
}

static QStringList menuEntries(const QMenu* menu)
{
    QStringList out;
    for (QAction* a : menu->actions())
        if (a->data().isValid())
            out << a->data().toString();
    return out;
}

static QAction* clearAction(const QMenu* menu)
{
    for (QAction* a : menu->actions())
        if (a->objectName() == QLatin1String("quickSearchClearHistory"))
            return a;
    return nullptr;
}

static void checkQuery()
{
    QuickSearchField field;
    field.setText(QStringLiteral("a(b"));
    TextQuery q = field.query();
    CHECK(q.text == QLatin1String("a(b") && !q.caseSensitive && !q.wholeWord && !q.regex,
          "plain query by default");
    CHECK(field.queryError().isEmpty() && field.styleSheet().isEmpty(), "a literal is never an error");

    field.matchCaseAction()->setChecked(true);
    field.wholeWordAction()->setChecked(true);
    q = field.query();
    CHECK(q.caseSensitive && q.wholeWord && !q.regex, "toggles go into the query");
    CHECK(field.queryError().isEmpty(), "whole word escapes a literal");

    field.regexAction()->setChecked(true);
    CHECK(field.query().regex, "regex toggle goes into the query");
    CHECK(!field.queryError().isEmpty() && field.queryError().contains(QLatin1String("position 3")),
          "invalid regex reports its position: " + field.queryError());
    CHECK(field.styleSheet().contains(QLatin1String("border")), "invalid regex marks the field");
    CHECK(field.toolTip() == field.queryError(), "the tooltip explains the error");

    field.setText(QStringLiteral("a(b)"));
    CHECK(field.queryError().isEmpty() && field.styleSheet().isEmpty(), "a fixed regex clears the error");
    field.setText(QStringLiteral("a[b"));
    CHECK(!field.queryError().isEmpty(), "broken again");
    field.regexAction()->setChecked(false);
    CHECK(field.queryError().isEmpty(), "regex off clears the error");

    field.setBaseToolTip(QStringLiteral("base tip"));
    CHECK(field.toolTip() == QLatin1String("base tip"), "base tooltip without an error");

    // «Не найдено» — до правки или флагов.
    field.setNotFound(true);
    CHECK(field.isNotFound() && field.styleSheet().contains(QLatin1String("background")),
          "not found tints the field");
    field.setText(QStringLiteral("a[bc"));
    CHECK(!field.isNotFound() && field.styleSheet().isEmpty(), "editing clears not found");
    field.setNotFound(true);
    field.matchCaseAction()->setChecked(false);
    CHECK(!field.isNotFound(), "toggling a flag clears not found");

    int optionSignals = 0;
    QObject::connect(&field, &QuickSearchField::optionsChanged, [&] { ++optionSignals; });
    field.wholeWordAction()->toggle();
    field.regexAction()->toggle();
    CHECK(optionSignals == 2, "each toggle reports a change");
}

static void checkHistory()
{
    QuickSearchField field;
    QStringList saved;
    int changes = 0;
    QObject::connect(&field, &QuickSearchField::historyChanged, [&](const QStringList& e) {
        saved = e;
        ++changes;
    });
    QString activated;
    QObject::connect(&field, &QuickSearchField::historyActivated,
                     [&](const QString& t) { activated = t; });

    field.setHistory({QStringLiteral("old"), QStringLiteral("older")});
    CHECK(changes == 0, "loading does not report a change");
    field.remember(QStringLiteral("fresh"));
    field.remember(QStringLiteral("old"));
    field.remember(QString());
    CHECK(saved == QStringList({"old", "fresh", "older"}) && changes == 2,
          "remember: newest first, no duplicates, empty ignored");

    // Стрелка вниз — в непустом поле тоже.
    field.setText(QStringLiteral("typed"));
    pressDown(&field);
    QMenu* menu = field.historyMenu();
    CHECK(menu && menu->isVisible(), "Down opens the history");
    CHECK(menuEntries(menu) == QStringList({"old", "fresh", "older"}), "the menu lists the history");
    QAction* clear = clearAction(menu);
    CHECK(clear && clear->isEnabled(), "the menu offers to clear the history");
    menu->hide();
    pressDown(&field, Qt::AltModifier);
    CHECK(menu->isVisible(), "Alt+Down opens it too");
    menu->hide();

    // Выбор пункта.
    for (QAction* a : menu->actions())
        if (a->data().toString() == QLatin1String("fresh"))
            a->trigger();
    CHECK(field.text() == QLatin1String("fresh") && activated == QLatin1String("fresh"),
          "choosing an entry puts it into the field and runs it");

    // Длинный запрос в меню — сокращён, полный — в данных пункта.
    const QString longQuery(400, u'w');
    field.remember(longQuery);
    field.showHistory();
    CHECK(menuEntries(menu).first() == longQuery, "a long entry keeps its full text");
    CHECK(menu->actions().first()->text().size() < longQuery.size(), "and is elided in the menu");
    menu->hide();

    // Очистка.
    field.historyAction()->trigger();
    CHECK(menu->isVisible(), "the arrow button opens the history");
    clearAction(menu)->trigger();
    menu->hide();
    CHECK(field.history().isEmpty() && saved.isEmpty(), "clear empties the history and reports it");
    field.showHistory();
    CHECK(menuEntries(menu).isEmpty() && clearAction(menu) && !clearAction(menu)->isEnabled(),
          "an empty history shows nothing to clear");
    menu->hide();
}

static void checkPersistence()
{
    AppSettings& s = AppSettings::instance();
    s.setQuickSearchMatchCase(true);
    s.setQuickSearchWholeWord(false);
    s.setQuickSearchRegex(true);
    s.setQuickSearchHistory({QStringLiteral("first"), QStringLiteral("se,cond"), QStringLiteral("third")});
    s.save();
    s.setQuickSearchMatchCase(false);
    s.setQuickSearchRegex(false);
    s.setQuickSearchHistory({});
    s.load();
    CHECK(s.quickSearchMatchCase() && !s.quickSearchWholeWord() && s.quickSearchRegex(),
          "toggles survive a restart");
    CHECK(s.quickSearchHistory() == QStringList({"first", "se,cond", "third"}),
          "history survives a restart in order");
    s.setQuickSearchHistory({QStringLiteral("only")});
    s.save();
    s.load();
    CHECK(s.quickSearchHistory() == QStringList({"only"}), "a single entry round-trips");
    // Сохранение одной группы — сразу после изменения, без save().
    s.setQuickSearchHistory({QStringLiteral("now")});
    s.setQuickSearchWholeWord(true);
    s.saveQuickSearch();
    s.setQuickSearchHistory({});
    s.setQuickSearchWholeWord(false);
    s.load();
    CHECK(s.quickSearchHistory() == QStringList({"now"}) && s.quickSearchWholeWord(),
          "saveQuickSearch writes the group at once");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DendroLogTest"));
    QApplication::setApplicationName(QStringLiteral("quick_search_field"));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QFile::remove(AppSettings::iniFilePath());

    checkQuery();
    checkHistory();
    checkPersistence();

    QFile::remove(AppSettings::iniFilePath());
    std::fprintf(stdout, "Quick search field: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
