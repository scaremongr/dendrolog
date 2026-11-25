// ============================================================================
// logview_lifecycle — жизненный цикл вкладки (LogViewWidget) на живых файлах:
//   • дозапись многострочной записи по частям, в том числе строки, дописанной
//     в два приёма (без '\n' между ними), — на обоих бэкендах;
//   • файл переписан заново (ротация) — полная перезагрузка;
//   • переписано только НАЧАЛО файла — той же длины или с ростом, хвост тот
//     же (Q-19): полная перезагрузка на обоих бэкендах, в слитой вкладке и
//     после перехода порога; авто-обновление и F5; новый текст и номера
//     строк, закладка на переписанной строке — «не найдена», на уцелевшей —
//     на месте;
//   • растущий файл пересёк порог индексного бэкенда — вкладка на лету
//     становится индексной, строки те же;
//   • вкладку закрыли посреди загрузки — без падения;
//   • переходы на растущем логе с включённым Follow Tail (найденная строка
//     быстрого поиска, переход к записи из Search и статистики): дозапись не
//     возвращает вид к хвосту;
//   • быстрый поиск стартует от строки, выбранной пользователем, даже когда
//     её скрыл фильтр, — а не от строки, назначенной Qt по фокусу, и не с
//     начала файла; то же после перестройки модели посреди поиска;
//   • «↓ N новых строк» при выключенном Follow Tail: счёт только видимых
//     дописанных строк, сброс у хвоста, по Follow Tail, фильтру и перезагрузке
//     переписанного файла, переход к первой новой строке.
//   • UTF-16 (LE и BE, с BOM; резидентный путь): дозапись читается в кодировке
//     файла, строка, дописанная частями, — одна строка, даже когда порция
//     обрывается посреди двухбайтного символа или суррогатной пары; файл выше
//     порога индексного бэкенда не перечитывается целиком на каждый тик.
//   • память файла (R-4): ротация app.log → app.log.1 (память находит
//     переименованный файл по содержимому, новый app.log открывается чистым —
//     в обоих порядках открытия) и перенос файла в другую папку; оба бэкенда.
//   • закрыть вкладку и вернуть её (Ctrl+Shift+T, R-5; SessionController на
//     ряду QTabWidget): файлы (и слитая вкладка), фильтры, маркеры, закладки,
//     текущая строка и её место на экране, выбор кодировки, место в ряду;
//     стек из 10; файл за время закрытия дописан, переписан, удалён; вкладка
//     stdin и пустая в стек не идут; вкладка из снимка без файлов не пустая
//     для обычного открытия (isBlank); оба бэкенда.
// ============================================================================

#include "appsettings.h"
#include "filememorystore.h"
#include "logviewwidget.h"
#include "sessioncontroller.h"
#include "stdinspooler.h"
#include "testdocuments.h"
#include "viewidle.h"
#include "fulllogcontroller.h"

#include "LogListView.h"
#include "indexedlogstore.h"
#include "logindexer.h"
#include "recordlocator.h"
#include "bookmarkstore.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QFocusEvent>
#include <QPointer>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStringEncoder>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <cstdio>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static QByteArray record(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

// Покой по состоянию (P-6): загрузка кончилась, модель не фильтрует,
// отложенное доставлено, view доделали раскладку — без окна тишины.
static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); }, 60000)
        && testdocs::settleViews(*tab.model(), [&] { return tab.isLoading(); });
}

static QStringList rows(const LogModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.messageAt(row);
    return out;
}

// Дозапись: новая запись, её продолжение, затем строка в два приёма.
static void checkAppendInParts(const QDir& dir, const QString& name, const QString& schema)
{
    const QString path = testdocs::writeFile(dir, name + ".log",
        record(0, "INFO", "first") + record(1, "INFO", "second"));
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), name + ": initial load");
    LogModel& model = *tab.model();
    CHECK(model.rowCount() == 2, name + ": two lines loaded");

    const auto append = [&](const QByteArray& bytes) {
        CHECK(testdocs::appendToFile(path, bytes), name + ": append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + ": reload settles");
    };

    append(record(2, "ERROR", "failure"));
    CHECK(model.rowCount() == 3 && model.messageAt(2).endsWith("failure"),
          name + ": appended record shown");
    append("    at continuation one\n");
    CHECK(model.rowCount() == 4 && model.messageAt(3) == "    at continuation one",
          name + ": continuation shown");
    CHECK(model.keyForRow(3).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + ": continuation belongs to the record it continues");

    // Строка дописана в два приёма: до перевода строки её видно частично,
    // после — целиком, одной строкой, а не двумя.
    append("    at contin");
    append("uation two\n");
    const QStringList text = rows(model);
    CHECK(model.rowCount() == 5, name + ": a line written in two parts is one row: "
                                     + text.join(" | "));
    CHECK(!text.isEmpty() && text.last() == "    at continuation two",
          name + ": the completed line has its full text: " + (text.isEmpty() ? QString() : text.last()));
    CHECK(model.rowCount() == 5
              && model.keyForRow(4).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + ": the completed continuation stays in its record");

    append(record(3, "INFO", "after"));
    CHECK(model.rowCount() == 6 && model.messageAt(5).endsWith("after"),
          name + ": next record after the completed line");
    CHECK(model.keyForRow(5).logicalEntryId != model.keyForRow(2).logicalEntryId,
          name + ": next record starts a new logical record");

    // Пустая строка в дозаписи — такая же строка, как при первой загрузке.
    append(QByteArray("\n") + record(4, "INFO", "after blank"));
    CHECK(model.rowCount() == 8 && model.messageAt(6).isEmpty()
              && model.messageAt(7).endsWith("after blank"),
          name + ": a blank appended line is kept: " + rows(model).join(" | "));

    // Ротация: файл переписан заново и стал короче — полная перезагрузка.
    QFile rewrite(path);
    CHECK(rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate), name + ": rewrite");
    rewrite.write(record(10, "WARN", "rotated"));
    rewrite.close();
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), name + ": rewrite settles");
    CHECK(model.rowCount() == 1 && model.messageAt(0).endsWith("rotated"),
          name + ": rewritten file is reloaded from scratch: " + rows(model).join(" | "));
}

// Номера физических строк на живой вкладке (R-1): идентификатор строки стоит
// на её номере, поэтому оба бэкенда нумеруют строки как файл — с пустой
// последней строкой, CRLF, строкой, дописанной частями, и хвостом без '\n'.
// Эталон — разбиение байтов файла (testdocs::expectedPhysicalLines).
static void checkLineNumbers(const QDir& dir, const QString& name, const QString& schema)
{
    QByteArray bytes = record(0, "INFO", "first") + record(1, "ERROR", "second") + "\n";
    const QString path = testdocs::writeFile(dir, name + "-lines.log", bytes);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), name + " line numbers: initial load");
    const LogModel& model = *tab.model();

    const auto compare = [&](const QString& step) {
        const QString expected = testdocs::expectedPhysicalLines(bytes).join(" | ");
        const QString actual = testdocs::numberedLines(model).join(" | ");
        CHECK(actual == expected, name + " line numbers, " + step + ":\n    file: " + expected
                                      + "\n    tab:  " + actual);
    };
    const auto append = [&](const QByteArray& chunk, const QString& step) {
        bytes += chunk;
        CHECK(testdocs::appendToFile(path, chunk), name + ": append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + " line numbers: reload settles");
        compare(step);
    };

    compare("blank last line");
    append(record(2, "WARN", "after blank"), "record after the blank last line");
    append("    at part", "a line without '\\n'");
    append("ial\n\n", "the line completed, then a blank line");
    append(QByteArray(record(3, "INFO", "crlf")).replace("\n", "\r\n") + "\r\n",
           "CRLF line and a blank CRLF line");
    append("    left\rright\n", "a lone CR inside a line");
    // Хвост, оборванный на '\r' от "\r\n": '\r' не текст ни до, ни после
    // дозаписи '\n' — на обоих бэкендах (R-1, шаг 2).
    append("    cr tail\r", "a provisional tail ending with a lone CR");
    append("\n", "the CR tail completed by LF");
    append("    cr then\r", "another provisional tail ending with a lone CR");
    append("text\n", "the lone CR became a CR inside the line");
    append("tail without newline", "a provisional tail");
    append("\n" + record(4, "INFO", "last"), "the tail completed");
    CHECK(tab.model()->isIndexedBackend() == (name == QStringLiteral("indexed")),
          name + " line numbers: the tab stayed on its backend");
}

// 400 строк: каждая третья — ERROR, NEEDLE на строках 30, 150 и 300 (все ERROR).
static QByteArray jumpFixture()
{
    QByteArray out;
    for (int i = 0; i < 400; ++i) {
        QByteArray text = "ordinary " + QByteArray::number(i);
        if (i == 30 || i == 150 || i == 300)
            text = "NEEDLE " + QByteArray::number(i);
        out += record(i, i % 3 ? "INFO" : "ERROR", text);
    }
    return out;
}

// Быстрый поиск до итога: текст найденной (текущей) строки или пустая строка.
static QString quickSearch(LogViewWidget& tab, bool forward, const std::function<void()>& meanwhile = {})
{
    bool finished = false, found = false;
    const QMetaObject::Connection connection = QObject::connect(
        &tab, &LogViewWidget::quickSearchFinished,
        [&](const QString&, bool ok) { finished = true; found = ok; });
    if (forward)
        tab.searchTextNext(TextQuery("NEEDLE", Qt::CaseSensitive));
    else
        tab.searchTextPrevious(TextQuery("NEEDLE", Qt::CaseSensitive));
    if (meanwhile)
        meanwhile(); // до возврата в цикл событий: итог скана ещё не доставлен
    const bool done = testdocs::waitFor([&] { return finished; });
    QObject::disconnect(connection);
    if (!done || !found)
        return QString();
    const QModelIndex current = tab.view()->currentIndex();
    return current.isValid() ? tab.model()->messageAt(current.row()) : QString();
}

// Переходы на растущем логе и старт быстрого поиска.
static void checkJumps(const QDir& dir, const QString& name, const QString& schema)
{
    const QString path = testdocs::writeFile(dir, name + "-jumps.log", jumpFixture());
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), name + ": jumps: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    CHECK(model.rowCount() == 400, name + ": jumps: fixture loaded");

    int appended = 0;
    const auto append = [&] {
        CHECK(testdocs::appendToFile(path, record(400 + appended, "ERROR",
                                                  "tail " + QByteArray::number(appended))),
              name + ": jumps: append");
        ++appended;
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + ": jumps: reload settles");
        CHECK(testdocs::idleViews(), "views settle");
    };
    const auto rowOnScreen = [&](int row) {
        return view.viewport()->rect().intersects(view.visualRect(model.index(row, 0)));
    };
    // Вид остался на строке row: Follow Tail снят, дозапись к хвосту не увела.
    const auto staysAt = [&](int row, const QString& what) {
        CHECK(view.currentIndex().row() == row
                  && view.selectionModel()->isRowSelected(row, QModelIndex()),
              name + ": " + what + ": the row is current and selected");
        CHECK(!view.followTail(), name + ": " + what + ": follow-tail is off");
        CHECK(rowOnScreen(row), name + ": " + what + ": the row is on screen");
        append();
        CHECK(rowOnScreen(row) && !view.isScrolledToBottom(),
              name + ": " + what + ": an append does not pull the view back to the tail");
    };
    const auto followTail = [&] {
        view.setFollowTail(true);
        append();
        CHECK(view.isScrolledToBottom(), name + ": jumps: follow-tail shows the tail");
    };

    // --- Найденная строка быстрого поиска ---------------------------------
    followTail();
    CHECK(quickSearch(tab, true).endsWith("NEEDLE 30"), name + ": quick search finds the first match");
    staysAt(30, "quick search match");

    // --- Переход к записи: из Search Results и из статистики --------------
    followTail();
    const LogModel::EntryKey key = model.keyForRow(200);
    CHECK(tab.goToEntry(key.logicalEntryId, key.sourceFile), name + ": jump to a record");
    staysAt(200, "jump to a record");
    CHECK(!tab.goToEntry(1000000, key.sourceFile) && view.currentIndex().row() == 200,
          name + ": a record that is not in the tab is ignored");
    // Запись скрыта фильтром — переход к ближайшей видимой строке.
    const LogModel::EntryKey hidden = model.keyForRow(100);
    model.setLogLevelFilter({LogLevel::Error});
    CHECK(settled(tab), name + ": jumps: filter settles");
    CHECK(tab.goToEntry(hidden.logicalEntryId, hidden.sourceFile)
              && model.messageAt(view.currentIndex().row()).endsWith("ordinary 102"),
          name + ": jump to a hidden record lands on the next visible row");
    model.setLogLevelFilter({});
    CHECK(settled(tab), name + ": jumps: filter reset settles");
    CHECK(testdocs::idleViews(), "views settle");

    // --- Переход в последние полэкрана (Q-14) ------------------------------
    // Центрировать такую строку нельзя — вид встаёт у самого низа. Это не
    // «пользователь читает хвост»: дозапись не должна его двигать.
    followTail();
    {
        const int nearEnd = model.rowCount() - 4;
        view.goToRow(nearEnd);
        testdocs::pump(0);
        CHECK(view.currentIndex().row() == nearEnd && !view.followTail(),
              name + ": jump near the end: row is current, follow-tail is off");
        CHECK(view.isScrolledToBottom(), name + ": jump near the end leaves the view at the bottom");
        const int before = view.verticalScrollBar()->value();
        append();
        append();
        CHECK(view.verticalScrollBar()->value() == before && rowOnScreen(nearEnd),
              name + ": jump near the end: an append does not pull the view to the tail");

        // Пользователь сам докрутил до хвоста — прилипание возвращается.
        QWheelEvent wheel(QPointF(10, 10), view.viewport()->mapToGlobal(QPointF(10, 10)),
                          QPoint(), QPoint(0, -120 * 50), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view.viewport(), &wheel);
        CHECK(view.isScrolledToBottom(), name + ": the wheel reaches the tail");
        append();
        CHECK(view.isScrolledToBottom() && !view.followTail(),
              name + ": a view scrolled to the tail by the user keeps showing the tail");
    }

    // --- Старт быстрого поиска -------------------------------------------
    // Пользователь выбрал строку 100, фильтр её скрыл; Qt при возврате фокуса
    // назначает текущей другую строку, не выделяя её (инвариант 10).
    const auto hideSelected = [&] {
        model.setLogLevelFilter({LogLevel::Error});
        CHECK(settled(tab), name + ": start: filter settles");
        CHECK(testdocs::idleViews(), "views settle");
        QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(&view, &focus);
    };
    const auto showAll = [&] {
        model.setLogLevelFilter({});
        CHECK(settled(tab), name + ": start: filter reset settles");
        CHECK(testdocs::idleViews(), "views settle");
    };
    view.goToRow(100);
    hideSelected();
    CHECK(quickSearch(tab, true).endsWith("NEEDLE 150"),
          name + ": forward search starts from the hidden selected row, not from the top");
    showAll();
    view.goToRow(100);
    hideSelected();
    CHECK(quickSearch(tab, false).endsWith("NEEDLE 30"),
          name + ": backward search starts from the hidden selected row");
    showAll();

    // Выбранная строка видна — поиск идёт от неё, как и раньше.
    view.goToRow(150);
    CHECK(quickSearch(tab, true).endsWith("NEEDLE 300"), name + ": forward from a selected match");
    view.goToRow(150);
    CHECK(quickSearch(tab, false).endsWith("NEEDLE 30"), name + ": backward from a selected match");

    // Модель перестроилась посреди поиска (фильтр скрыл выбранную строку):
    // поиск начинается заново от её места, а не с начала файла. Синхронный
    // reset до доставки итога бывает только у резидентного фильтра.
    if (!model.isIndexedBackend()) {
        view.goToRow(100);
        const QString found = quickSearch(tab, true, [&] { model.setLogLevelFilter({LogLevel::Error}); });
        CHECK(found.endsWith("NEEDLE 150"),
              name + ": a search restarted by a model reset keeps its starting place: " + found);
        showAll();
    }
}

// «↓ N новых строк» при выключенном Follow Tail: счёт дописанных видимых строк
// ниже экрана, сброс у хвоста и по Follow Tail, переход к первой новой строке.
static void checkNewLinesIndicator(const QDir& dir, const QString& name, const QString& schema)
{
    const QString path = testdocs::writeFile(dir, name + "-newlines.log", jumpFixture());
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), name + ": new lines: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    QScrollBar* bar = view.verticalScrollBar();
    auto* indicator = view.findChild<QAbstractButton*>(QStringLiteral("newLinesIndicator"));
    CHECK(indicator, name + ": new lines: the indicator exists");
    if (!indicator)
        return;
    const auto shown = [&] { return indicator->isVisible(); };

    CHECK(view.newLinesBelow() == 0 && !shown(),
          name + ": the initial load is not counted as new lines");

    int appended = 0;
    const auto append = [&](int count, const char* level = "INFO") {
        QByteArray bytes;
        for (int i = 0; i < count; ++i, ++appended)
            bytes += record(400 + appended, level, "tail " + QByteArray::number(appended));
        CHECK(testdocs::appendToFile(path, bytes), name + ": new lines: append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + ": new lines: reload settles");
        CHECK(testdocs::idleViews(), "views settle");
    };
    const auto toTop = [&] {
        bar->setValue(0);
        CHECK(testdocs::idleViews(), "views settle");
    };

    // --- Дозапись при выключенном Follow Tail увеличивает счёт ------------
    toTop();
    CHECK(!view.followTail() && !view.isScrolledToBottom(), name + ": new lines: reading the top");
    append(3);
    CHECK(view.newLinesBelow() == 3 && shown(),
          name + QStringLiteral(": three appended lines are counted: %1").arg(view.newLinesBelow()));
    CHECK(indicator->text().contains(QStringLiteral("3 new lines")),
          name + ": the indicator says how many: " + indicator->text());
    CHECK(view.firstNewLineRow() == model.rowCount() - 3, name + ": the first new line is known");
    append(2);
    CHECK(view.newLinesBelow() == 5, name + ": the next append adds to the count");
    CHECK(view.firstNewLineRow() == model.rowCount() - 5, name + ": the first new line stays");

    // --- Прокрутка к низу сбрасывает ----------------------------------------
    bar->setValue(bar->maximum());
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": reaching the bottom clears the count");
    append(2);
    CHECK(view.newLinesBelow() == 0 && !shown() && view.isScrolledToBottom(),
          name + ": at the bottom the view sticks to the tail, nothing to count");

    // --- Follow Tail сбрасывает ----------------------------------------------
    toTop();
    append(4);
    CHECK(view.newLinesBelow() == 4, name + ": counting again after leaving the bottom");
    view.setFollowTail(true);
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": Follow Tail clears the count");
    append(2);
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": Follow Tail shows the lines itself");
    view.setFollowTail(false);

    // --- Переход: к первой новой строке, Follow Tail не включается -----------
    toTop();
    append(60);
    CHECK(view.newLinesBelow() == 60, name + ": sixty new lines");
    const int firstNew = model.rowCount() - 60;
    CHECK(view.firstNewLineRow() == firstNew, name + ": jump target is the first new line");
    indicator->click();
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(view.currentIndex().row() == firstNew
              && view.selectionModel()->isRowSelected(firstNew, QModelIndex()),
          name + ": the click goes to the first new line and selects it");
    CHECK(view.viewport()->rect().intersects(view.visualRect(model.index(firstNew, 0))),
          name + ": the first new line is on screen");
    CHECK(!view.followTail(), name + ": the jump does not turn Follow Tail on");
    const int left = view.newLinesBelow();
    CHECK(left > 0 && left < 60 && shown(),
          name + QStringLiteral(": lines seen on screen are no longer counted: %1 left").arg(left));
    CHECK(view.firstNewLineRow() > firstNew, name + ": the next unseen line is below the screen");
    view.goToNewLines(); // команда (шорткат) — тот же путь
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(view.newLinesBelow() < left, name + ": the command pages through the rest");
    while (view.newLinesBelow() > 0 && view.firstNewLineRow() >= 0) {
        const int before = view.newLinesBelow();
        view.goToNewLines();
        CHECK(testdocs::idleViews(), "views settle");
        if (view.newLinesBelow() >= before)
            break;
    }
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": paging through reaches the end");

    // --- Фильтр: считаются только видимые строки ---------------------------
    model.setLogLevelFilter({LogLevel::Error});
    CHECK(settled(tab), name + ": new lines: filter settles");
    CHECK(testdocs::idleViews(), "views settle");
    toTop();
    CHECK(view.newLinesBelow() == 0, name + ": a filter change starts the count afresh");
    append(3, "INFO");
    append(2, "ERROR");
    CHECK(view.newLinesBelow() == 2,
          name + QStringLiteral(": only lines the filter shows are counted: %1").arg(view.newLinesBelow()));
    CHECK(view.firstNewLineRow() == model.rowCount() - 2
              && model.messageAt(view.firstNewLineRow()).contains("ERROR"),
          name + ": the first new visible line is the first new ERROR");
    model.setLogLevelFilter({});
    CHECK(settled(tab), name + ": new lines: filter reset settles");
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": a model reset leaves no stale count");

    // --- Файл переписан: полная перезагрузка, ложного счёта нет --------------
    toTop();
    append(5);
    CHECK(view.newLinesBelow() == 5, name + ": counted before the rewrite");
    QByteArray rewritten;
    for (int i = 0; i < 300; ++i)
        rewritten += record(i, "INFO", "rewritten " + QByteArray::number(i));
    testdocs::writeFile(dir, name + "-newlines.log", rewritten);
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), name + ": new lines: rewrite settles");
    CHECK(testdocs::idleViews(), "views settle");
    CHECK(model.rowCount() == 300, name + ": the rewritten file is reloaded");
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": a rewritten file leaves no stale count");
    toTop();
    appended = 0;
    append(2);
    CHECK(view.newLinesBelow() == 2, name + ": counting works after the rewrite");
}

// Текст в UTF-16/UTF-32 без BOM (BOM пишется один раз — в начале файла).
static QByteArray encoded(QStringConverter::Encoding encoding, const QString& text)
{
    QStringEncoder encoder(encoding, QStringConverter::Flag::ConvertInitialBom);
    return encoder.encode(text);
}

static QString recordText(int second, const char* level, const QString& text)
{
    return QString::fromUtf8(record(second, level, text.toUtf8()));
}

static int lineNumberAt(const LogModel& model, int row)
{
    const std::shared_ptr<LogEntry> entry = model.entryAt(row);
    return entry ? entry->originalLineNumber() : -1;
}

// Дозапись в файл UTF-16 / UTF-32 (резидентный путь): то же, что
// checkAppendInParts, плюс порции, обрывающиеся посреди символа.
static void checkWideTail(const QDir& dir, const QString& name, const QString& schema,
                          QStringConverter::Encoding encoding, const QByteArray& bom)
{
    const auto enc = [&](const QString& text) { return encoded(encoding, text); };
    const int unit = bom.size(); // байт на кодовую единицу: 2 или 4
    const QString path = testdocs::writeFile(dir, name + ".log",
        bom + enc(recordText(0, "INFO", QStringLiteral("первая"))
                  + recordText(1, "INFO", QStringLiteral("вторая"))));
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), name + ": initial load");
    LogModel& model = *tab.model();
    CHECK(!model.isIndexedBackend(), name + ": the tab is resident");
    CHECK(model.rowCount() == 2 && model.messageAt(1).endsWith(QStringLiteral("вторая")),
          name + ": two lines loaded: " + rows(model).join(" | "));

    QStringList expected = rows(model);
    const auto append = [&](const QByteArray& bytes) {
        CHECK(testdocs::appendToFile(path, bytes), name + ": append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + ": reload settles");
    };
    const auto sameRows = [&](const QString& what) {
        const QStringList actual = rows(model);
        CHECK(actual == expected, name + ": " + what + ": " + actual.join(" | "));
        bool numbered = true;
        for (int row = 0; row < model.rowCount(); ++row)
            numbered = numbered && lineNumberAt(model, row) == row + 1;
        CHECK(numbered, name + ": " + what + ": line numbers continue");
    };
    // Полная строка одной порцией.
    const auto appendLine = [&](const QString& line, const QString& what) {
        append(enc(line + '\n'));
        expected << line;
        sameRows(what);
    };
    // Строка двумя порциями, первая — cut байт: между ними её видно частично.
    const auto appendSplit = [&](const QString& line, qsizetype cut, const QString& what) {
        const QByteArray bytes = enc(line + '\n');
        append(bytes.left(cut));
        CHECK(model.rowCount() == expected.size() + 1,
              name + ": " + what + ": the unfinished line is shown: " + rows(model).join(" | "));
        append(bytes.mid(cut));
        expected << line;
        sameRows(what);
    };

    appendLine(recordText(2, "ERROR", QStringLiteral("сбой")).chopped(1), "appended record");
    CHECK(model.rowCount() == 3 && model.keyForRow(2).logicalEntryId != model.keyForRow(1).logicalEntryId,
          name + ": the appended record is a new logical record");
    appendLine(QStringLiteral("    at продолжение один"), "continuation");
    CHECK(model.rowCount() == 4
              && model.keyForRow(3).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + ": continuation belongs to the record it continues");

    // Порция кончается на границе символа, посреди кодовой единицы и между
    // половинами суррогатной пары (U+1F600 — две единицы UTF-16).
    const QString second = QStringLiteral("    at продолжение два");
    appendSplit(second, 10 * unit, "a line written in two parts");
    const QString third = QStringLiteral("    at продолжение три");
    appendSplit(third, 10 * unit + 1, "a part ending inside a code unit");
    const QString emoji = QStringLiteral("    at смайл ") + QString::fromUcs4(U"\U0001F600")
                        + QStringLiteral(" конец");
    if (unit == 2) {
        const qsizetype high = emoji.indexOf(QChar(0xD83D));
        appendSplit(emoji, (high + 1) * 2, "a part ending inside a surrogate pair");
        appendSplit(emoji, (high + 1) * 2 + 1, "a part ending inside the low surrogate");
    } else {
        appendSplit(emoji, 13 * unit + 3, "a part ending inside a code unit of a wide character");
    }
    for (int row = 3; row < model.rowCount(); ++row)
        CHECK(model.keyForRow(row).logicalEntryId == model.keyForRow(2).logicalEntryId,
              name + ": completed continuations stay in their record");

    // Запись, дописанная частями: недописанная строка начинает запись.
    appendSplit(recordText(3, "WARN", QStringLiteral("после")).chopped(1), 31 * unit + 1,
                "a record written in two parts");
    CHECK(model.keyForRow(model.rowCount() - 1).logicalEntryId
              != model.keyForRow(2).logicalEntryId,
          name + ": the completed record is a new logical record");
    // CRLF и пустая строка.
    append(enc(QStringLiteral("\r\n") + recordText(4, "INFO", QStringLiteral("после пустой")).chopped(1)
               + QStringLiteral("\r\n")));
    expected << QString() << recordText(4, "INFO", QStringLiteral("после пустой")).chopped(1);
    sameRows("CRLF and a blank line");
}

// Файл UTF-16 выше порога индексного бэкенда: индексатор откатывает вкладку
// в резидентный путь один раз, дальше она в нём и остаётся.
static void checkWideAboveThreshold(const QDir& dir, const QString& schema)
{
    const QStringConverter::Encoding encoding = QStringConverter::Utf16LE;
    QString text;
    for (int i = 0; i < 12000; ++i)
        text += recordText(i / 50, "INFO", QStringLiteral("строка %1").arg(i));
    const QString path = testdocs::writeFile(dir, "utf16-big.log",
                                             QByteArray("\xFF\xFE", 2) + encoded(encoding, text));
    CHECK(QFileInfo(path).size() > 1024 * 1024, "utf16 threshold: the fixture is above the threshold");

    LogViewWidget tab;
    int fullReads = 0; // каждая полная загрузка (индексатор или парсер) сигналит о старте
    QObject::connect(&tab, &LogViewWidget::fileParsingStarted, [&] { ++fullReads; });
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), "utf16 threshold: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    CHECK(!model.isIndexedBackend(), "utf16 threshold: the tab fell back to resident");
    CHECK(model.rowCount() == 12000
              && model.messageAt(11999).endsWith(QStringLiteral("строка 11999")),
          "utf16 threshold: every line loaded");
    const int readsAfterLoad = fullReads;

    // Тики авто-обновления и F5 без изменений на диске.
    for (int tick = 0; tick < 5; ++tick) {
        tab.reloadChangedFiles(/*force=*/tick % 2 == 1);
        CHECK(settled(tab), "utf16 threshold: tick settles");
        CHECK(testdocs::idleViews(), "views settle");
    }
    CHECK(fullReads == readsAfterLoad,
          QStringLiteral("utf16 threshold: idle ticks do not reread the file (%1 extra full reads)")
              .arg(fullReads - readsAfterLoad));
    CHECK(!model.isIndexedBackend() && model.rowCount() == 12000,
          "utf16 threshold: idle ticks leave the tab as it was");

    // Дозапись: читается только хвост.
    for (int i = 0; i < 3; ++i) {
        CHECK(testdocs::appendToFile(path, encoded(encoding,
                  recordText(300 + i, "ERROR", QStringLiteral("хвост %1").arg(i)))),
              "utf16 threshold: append");
        tab.reloadChangedFiles(/*force=*/false);
        CHECK(settled(tab), "utf16 threshold: append settles");
        CHECK(testdocs::idleViews(), "views settle");
    }
    CHECK(fullReads == readsAfterLoad,
          QStringLiteral("utf16 threshold: appends read only the tail (%1 extra full reads)")
              .arg(fullReads - readsAfterLoad));
    CHECK(model.rowCount() == 12003
              && model.messageAt(12002).endsWith(QStringLiteral("хвост 2"))
              && lineNumberAt(model, 12002) == 12003,
          "utf16 threshold: appended lines shown: "
              + (model.rowCount() ? model.messageAt(model.rowCount() - 1) : QString()));

    // Файл переписан в UTF-8 и остался выше порога: «только резидентный»
    // относился к прежнему содержимому — вкладка становится индексной.
    const QByteArray utf8 = text.toUtf8() + text.toUtf8();
    CHECK(utf8.size() > 1024 * 1024, "utf16 threshold: the rewritten file is above the threshold");
    testdocs::writeFile(dir, "utf16-big.log", utf8);
    tab.reloadChangedFiles(/*force=*/false);
    CHECK(settled(tab), "utf16 threshold: rewrite settles");
    tab.reloadChangedFiles(/*force=*/false);
    CHECK(settled(tab), "utf16 threshold: rewrite settles again");
    CHECK(model.isIndexedBackend() && model.rowCount() == 24000
              && model.messageAt(23999).endsWith(QStringLiteral("строка 11999")),
          QStringLiteral("utf16 threshold: a file rewritten as UTF-8 is indexed (%1 rows)")
              .arg(model.rowCount()));
    CHECK(fullReads == readsAfterLoad + 1,
          QStringLiteral("utf16 threshold: the rewritten file is read once (%1 full reads)")
              .arg(fullReads - readsAfterLoad));
}


// Смена кодировки на индексной вкладке (F-1, шаг 2): индексатор не
// запускается, текущая запись, выделение и позиция вьюпорта остаются, текст
// читается новым декодером; при активном текстовом правиле видимый набор
// пересчитывается. Кодировка определяется сама: файл в Windows-1251 с
// ASCII-головой открывается читаемым.
static void checkEncodingSwitch(const QDir& dir, const QString& schema)
{
    QString text;
    for (int i = 0; i < 1500; ++i) {
        const QString second = QString::number(i % 60).rightJustified(2, QLatin1Char('0'));
        const QString minute = QString::number(i / 60).rightJustified(2, QLatin1Char('0'));
        if (i < 20)
            text += QStringLiteral("2026-03-05 10:%1:%2.000 [w] INFO - ascii head %3\n").arg(minute, second).arg(i);
        else
            text += QStringLiteral("2026-03-05 10:%1:%2.000 [worker-%3] %4 - Запись номер %5: ошибка диска\n")
                        .arg(minute, second).arg(i % 3).arg(QLatin1String(i % 2 ? "ERROR" : "INFO")).arg(i);
    }
    const QString path = testdocs::writeFile(dir, "encoding-switch.log",
                                             testdocs::encodeText(text, TextEncoding::Windows1251));
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), "encoding: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    IndexedLogStore* store = model.indexedOrNull();
    CHECK(store != nullptr, "encoding: the tab is indexed");
    if (!store)
        return;
    const auto first = model.entryAt(0);
    const LogFilePtr file = first ? first->sourceFile() : LogFilePtr();
    CHECK(file && store->fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251,
          "encoding: Windows-1251 detected on load");
    CHECK(model.messageAt(700).endsWith(QStringLiteral("Запись номер 700: ошибка диска")),
          "encoding: text is readable after detection: " + model.messageAt(700));

    // Шпионы: ни один индексатор вкладки не стартует и не публикует строк.
    int indexerEvents = 0;
    QObject spies;
    for (LogIndexer* indexer : tab.findChildren<LogIndexer*>()) {
        QObject::connect(indexer, &LogIndexer::indexingStarted, &spies, [&] { ++indexerEvents; });
        QObject::connect(indexer, &LogIndexer::indexBatchReady, &spies, [&] { ++indexerEvents; });
    }
    int startedSignals = 0;
    QObject::connect(&tab, &LogViewWidget::fileParsingStarted, &spies, [&] { ++startedSignals; });

    const auto switchTo = [&](TextEncoding encoding, const QString& what) {
        const int row = 700;
        view.goToRow(row);
        CHECK(testdocs::idleViews(), "views settle");
        const auto entry = model.entryAt(view.currentIndex().row());
        const qint64 lineNumber = entry ? entry->originalLineNumber() : -1;
        const int scroll = view.verticalScrollBar()->value();
        const QRect rect = view.visualRect(view.currentIndex());
        store->setFileEncoding(file, TextDecoder(encoding));
        CHECK(settled(tab), what + ": settles");
        CHECK(testdocs::idleViews(), "views settle");
        const QModelIndex cur = view.currentIndex();
        const auto now = cur.isValid() ? model.entryAt(cur.row()) : nullptr;
        CHECK(now && lineNumber == 701 && now->originalLineNumber() == lineNumber,
              what + ": the current line is kept");
        CHECK(cur.isValid() && view.selectionModel()->isRowSelected(cur.row(), QModelIndex()),
              what + ": the current row stays selected");
        CHECK(view.verticalScrollBar()->value() == scroll && view.visualRect(cur) == rect,
              what + QStringLiteral(": the viewport stays put (scroll %1 → %2)")
                         .arg(scroll).arg(view.verticalScrollBar()->value()));
        CHECK(store->fileEncoding(file).source == FileEncoding::Source::User
                  && store->fileEncoding(file).decoder.encoding() == encoding,
              what + ": the store records the user's choice");
        return cur.row();
    };

    // Неверная кодировка: текст другой, строки и места те же.
    int row = switchTo(TextEncoding::Koi8R, "encoding KOI8-R");
    const QString koi = model.messageAt(row);
    CHECK(!koi.contains(QStringLiteral("Запись")) && koi.contains(QStringLiteral(": ")),
          "encoding KOI8-R: the text is read with the new decoder: " + koi);
    // Обратно: исходный текст.
    row = switchTo(TextEncoding::Windows1251, "encoding back to Windows-1251");
    CHECK(model.messageAt(row).endsWith(QStringLiteral("Запись номер 700: ошибка диска")),
          "encoding back: original text: " + model.messageAt(row));

    // Активное текстовое правило с кириллицей: при смене кодировки видимый
    // набор пересчитывается (под KOI8-R «ошибка» не находится нигде).
    FilterRuleSet rules;
    FilterRule rule;
    rule.text = QStringLiteral("ОШИБКА");
    rules.rules.append(rule);
    rules.bindFields({}, false);
    model.setFilterRules(rules);
    CHECK(settled(tab), "encoding filter: settles");
    const int filteredRows = model.rowCount();
    CHECK(filteredRows == 1480, QStringLiteral("encoding filter: Cyrillic rule matches (%1 rows)").arg(filteredRows));
    store->setFileEncoding(file, TextDecoder(TextEncoding::Koi8R));
    CHECK(settled(tab), "encoding filter: switch settles");
    CHECK(model.rowCount() == 0, QStringLiteral("encoding filter: rows re-filtered with the new decoder (%1)")
                                     .arg(model.rowCount()));
    store->setFileEncoding(file, TextDecoder(TextEncoding::Windows1251));
    CHECK(settled(tab), "encoding filter: switch back settles");
    CHECK(model.rowCount() == filteredRows, "encoding filter: rows come back");
    model.setFilterRules(FilterRuleSet());
    CHECK(settled(tab), "encoding filter: cleared");

    CHECK(indexerEvents == 0 && startedSignals == 0,
          QStringLiteral("encoding: switching never runs the indexer (%1 indexer event(s), %2 start(s))")
              .arg(indexerEvents).arg(startedSignals));

    // Выбор пользователя переживает дозапись: хвост читается им же, автоматика
    // его не перерешает.
    store->setFileEncoding(file, TextDecoder(TextEncoding::Ibm866));
    CHECK(settled(tab), "encoding tail: switch settles");
    CHECK(testdocs::appendToFile(path, testdocs::encodeText(
              QStringLiteral("2026-03-05 11:00:00.000 [w] INFO - хвост после выбора\n"), TextEncoding::Ibm866)),
          "encoding tail: append");
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), "encoding tail: reload settles");
    CHECK(model.messageAt(model.rowCount() - 1).endsWith(QStringLiteral("хвост после выбора")),
          "encoding tail: the tail is read with the user's encoding: " + model.messageAt(model.rowCount() - 1));
    CHECK(store->fileEncoding(file).source == FileEncoding::Source::User
              && store->fileEncoding(file).decoder.encoding() == TextEncoding::Ibm866,
          "encoding tail: the user's choice stands");
}

// Растущий лог в Windows-1251 (F-1, шаг 3) на обоих бэкендах: файл начался
// ASCII («не решено»), кириллица пришла дозаписью — частями, с продолжением
// записи и строкой, дописанной в два приёма. Кодировку решает первая
// не-ASCII строка хвоста, дальше хвост читается ею же.
static void checkGrowingSingleByte(const QDir& dir, const QString& name, const QString& schema)
{
    const auto cp = [](const QString& text) {
        return testdocs::encodeText(text, TextEncoding::Windows1251);
    };
    const QString path = testdocs::writeFile(dir, name + "-cp1251-grows.log",
        record(0, "INFO", "ascii head") + record(1, "INFO", "still ascii"));
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    CHECK(settled(tab), name + " cp1251: initial load");
    LogModel& model = *tab.model();
    const LogFilePtr file = tab.loadedFiles().value(0);
    CHECK(!tab.fileEncoding(file).decided(), name + " cp1251: an ASCII file is undecided");

    const auto append = [&](const QByteArray& bytes) {
        CHECK(testdocs::appendToFile(path, bytes), name + " cp1251: append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + " cp1251: reload settles");
    };
    append(cp(recordText(2, "ERROR", QStringLiteral("Ошибка соединения: таймаут"))));
    const FileEncoding decided = tab.fileEncoding(file);
    CHECK(decided.source == FileEncoding::Source::Auto
              && decided.decoder.encoding() == TextEncoding::Windows1251,
          name + " cp1251: the tail decided Windows-1251: "
              + TextEncodings::displayName(decided.decoder.encoding()));
    CHECK(model.rowCount() == 3
              && model.messageAt(2).endsWith(QStringLiteral("Ошибка соединения: таймаут")),
          name + " cp1251: the appended record is readable: " + rows(model).join(" | "));
    append(cp(QStringLiteral("    в модуле Хранилище\n")));
    CHECK(model.rowCount() == 4 && model.messageAt(3) == QStringLiteral("    в модуле Хранилище")
              && model.keyForRow(3).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + " cp1251: a Cyrillic continuation stays in its record: " + rows(model).join(" | "));
    // Строка в два приёма: оборвана посреди слова.
    append(cp(QStringLiteral("    повторная попы")));
    append(cp(QStringLiteral("тка через 5 с\n")));
    CHECK(model.rowCount() == 5
              && model.messageAt(4) == QStringLiteral("    повторная попытка через 5 с"),
          name + " cp1251: a line written in two parts is one readable row: "
              + rows(model).join(" | "));
    append(cp(recordText(3, "WARN", QStringLiteral("ДИСК заполнен"))));
    CHECK(model.rowCount() == 6 && model.messageAt(5).endsWith(QStringLiteral("ДИСК заполнен")),
          name + " cp1251: later tails keep the decided encoding");
    CHECK(tab.fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251,
          name + " cp1251: decided once");
}

// Переход порога сохраняет кодировку (F-1, шаг 3): решение резидентного пути
// и выбор пользователя передаются индексатору, а не определяются заново; и
// обратно — индексная вкладка, откатившаяся в резидентный путь, выбор
// пользователя не теряет. Порог 1 МБ выставляет вызывающий.
static void checkThresholdKeepsEncoding(const QDir& dir, const QString& schema)
{
    QString head;
    for (int i = 0; i < 400; ++i)
        head += recordText(i / 50, i % 2 ? "ERROR" : "INFO",
                           QStringLiteral("Запись номер %1: ошибка диска").arg(i));
    QString more;
    for (int i = 400; i < 24000; ++i)
        more += recordText(i / 50, "INFO", QStringLiteral("Строка %1 после порога").arg(i));
    const QByteArray moreBytes = testdocs::encodeText(more, TextEncoding::Windows1251);
    CHECK(moreBytes.size() > 1024 * 1024, "threshold encoding: the tail crosses 1 MB");

    // 1. Решение автоматики. Запасную кодировку меняем до перехода: если бы
    // индексатор решал заново, окно cp1251 под запасной Windows-1252 дало бы
    // Windows-1252.
    {
        const QString path = testdocs::writeFile(dir, "threshold-auto.log",
            testdocs::encodeText(head, TextEncoding::Windows1251));
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.setFallbackEncoding(TextEncoding::Windows1251);
        tab.addLogFile(path);
        CHECK(settled(tab), "threshold auto: initial load");
        const LogFilePtr file = tab.loadedFiles().value(0);
        CHECK(!tab.model()->isIndexedBackend()
                  && tab.fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251,
              "threshold auto: resident tab detected Windows-1251");
        tab.setFallbackEncoding(TextEncoding::Windows1252);
        CHECK(testdocs::appendToFile(path, moreBytes), "threshold auto: append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold auto: conversion settles");
        LogModel& model = *tab.model();
        IndexedLogStore* store = model.indexedOrNull();
        CHECK(store != nullptr, "threshold auto: converted to indexed");
        if (store) {
            const FileEncoding enc = store->fileEncoding(file);
            CHECK(enc.source == FileEncoding::Source::Auto
                      && enc.decoder.encoding() == TextEncoding::Windows1251,
                  "threshold auto: the decision is carried over: "
                      + TextEncodings::displayName(enc.decoder.encoding()));
        }
        CHECK(model.rowCount() == 24000
                  && model.messageAt(0).endsWith(QStringLiteral("Запись номер 0: ошибка диска"))
                  && model.messageAt(23999).endsWith(QStringLiteral("Строка 23999 после порога")),
              "threshold auto: text readable after conversion: " + model.messageAt(0));
    }

    // 2. Выбор пользователя на резидентной вкладке (заведомо неверный KOI8-R)
    // переживает переход порога — и обратный переход индексной вкладки в
    // резидентный путь (файл переписан в UTF-16), и снова индексный.
    {
        const QString path = testdocs::writeFile(dir, "threshold-user.log",
            testdocs::encodeText(head, TextEncoding::Windows1251));
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.setFallbackEncoding(TextEncoding::Windows1251);
        tab.addLogFile(path);
        CHECK(settled(tab), "threshold user: initial load");
        LogModel& model = *tab.model();
        const LogFilePtr file = tab.loadedFiles().value(0);
        const QString readable = model.messageAt(7);
        tab.setFileEncoding(file, TextEncoding::Koi8R);
        CHECK(settled(tab), "threshold user: resident switch settles");
        CHECK(model.rowCount() == 400 && model.messageAt(7) != readable
                  && !model.messageAt(7).contains(QStringLiteral("Запись")),
              "threshold user: the resident tab rereads the file with KOI8-R: " + model.messageAt(7));
        CHECK(tab.fileEncoding(file).source == FileEncoding::Source::User,
              "threshold user: the choice is recorded");
        tab.setFileEncoding(file, TextEncoding::Windows1251);
        CHECK(settled(tab), "threshold user: switch back settles");
        CHECK(model.messageAt(7) == readable, "threshold user: back to readable text");
        tab.setFileEncoding(file, TextEncoding::Koi8R);
        CHECK(settled(tab), "threshold user: KOI8-R again");

        CHECK(testdocs::appendToFile(path, moreBytes), "threshold user: append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold user: conversion settles");
        IndexedLogStore* store = model.indexedOrNull();
        CHECK(store != nullptr, "threshold user: converted to indexed");
        if (!store)
            return;
        CHECK(store->fileEncoding(file).source == FileEncoding::Source::User
                  && store->fileEncoding(file).decoder.encoding() == TextEncoding::Koi8R,
              "threshold user: the user's choice is carried over");
        CHECK(model.rowCount() == 24000 && !model.messageAt(7).contains(QStringLiteral("Запись")),
              "threshold user: the indexed tab reads KOI8-R: " + model.messageAt(7));

        // Обратно: файл переписан в UTF-16 — индексатор возвращает вкладку
        // в резидентный путь; выбор пользователя переходит вместе с файлом.
        QString wide;
        for (int i = 0; i < 30000; ++i)
            wide += recordText(i / 50, "INFO", QStringLiteral("широкая %1").arg(i));
        testdocs::writeFile(dir, "threshold-user.log",
                            QByteArray("\xFF\xFE", 2) + encoded(QStringConverter::Utf16LE, wide));
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold user: UTF-16 rewrite settles");
        CHECK(!model.isIndexedBackend() && model.rowCount() == 30000
                  && model.messageAt(29999).endsWith(QStringLiteral("широкая 29999")),
              "threshold user: a UTF-16 file falls back to resident");
        CHECK(tab.fileStreamEncoding(file) == TextEncoding::Utf16LE,
              "threshold user: read as UTF-16LE");
        CHECK(tab.fileEncoding(file).source == FileEncoding::Source::User
                  && tab.fileEncoding(file).decoder.encoding() == TextEncoding::Koi8R,
              "threshold user: the choice survives the fallback");

        // И снова однобайтный файл выше порога — индексный, с тем же выбором.
        testdocs::writeFile(dir, "threshold-user.log",
                            testdocs::encodeText(head + more, TextEncoding::Windows1251));
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold user: rewrite settles");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold user: rewrite settles again");
        store = model.indexedOrNull();
        CHECK(store && store->fileEncoding(file).source == FileEncoding::Source::User
                  && store->fileEncoding(file).decoder.encoding() == TextEncoding::Koi8R,
              "threshold user: the choice survives the way back to indexed");
        CHECK(model.rowCount() == 24000 && !model.messageAt(7).contains(QStringLiteral("Запись")),
              "threshold user: read with KOI8-R again: " + model.messageAt(7));
    }
}

// Смена кодировки на резидентной вкладке (F-1, шаг 3 — механизм; триггер в
// logview_state по трём путям — шаг 4): файл перечитывается выбранной
// кодировкой; текущая запись, выделение и позиция остаются.
static void checkResidentEncodingSwitch(const QDir& dir, const QString& schema)
{
    QString text;
    for (int i = 0; i < 1500; ++i)
        text += recordText(i / 50, i % 2 ? "ERROR" : "INFO",
                           QStringLiteral("Запись номер %1: ошибка диска").arg(i));
    const QString path = testdocs::writeFile(dir, "resident-switch.log",
        testdocs::encodeText(text, TextEncoding::Windows1251));
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), "resident switch: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    const LogFilePtr file = tab.loadedFiles().value(0);
    CHECK(!model.isIndexedBackend(), "resident switch: the tab is resident");

    const auto switchTo = [&](TextEncoding encoding, const QString& what) {
        view.goToRow(700);
        CHECK(testdocs::idleViews(), "views settle");
        const auto entry = model.entryAt(view.currentIndex().row());
        const qint64 lineNumber = entry ? entry->originalLineNumber() : -1;
        const int scroll = view.verticalScrollBar()->value();
        const QRect rect = view.visualRect(view.currentIndex());
        tab.setFileEncoding(file, encoding);
        CHECK(settled(tab), what + ": settles");
        CHECK(testdocs::idleViews(), "views settle");
        const QModelIndex cur = view.currentIndex();
        const auto now = cur.isValid() ? model.entryAt(cur.row()) : nullptr;
        CHECK(now && lineNumber == 701 && now->originalLineNumber() == lineNumber,
              what + QStringLiteral(": the current line is kept (%1)")
                         .arg(now ? now->originalLineNumber() : -1));
        CHECK(cur.isValid() && view.selectionModel()->isRowSelected(cur.row(), QModelIndex()),
              what + ": the current row stays selected");
        // Резидентная вкладка ведёт высоты строк по тексту: текст в другой
        // кодировке может рисоваться запасным шрифтом другой высоты (так на
        // Windows), и прокрутка в пикселях законно сдвигается. Инвариант —
        // текущая строка на том же месте экрана.
        const QRect after = view.visualRect(cur);
        CHECK(after.top() == rect.top(),
              what + QStringLiteral(": the current row stays at the same place on screen "
                                    "(top %1 → %2, scroll %3 → %4)")
                         .arg(rect.top()).arg(after.top())
                         .arg(scroll).arg(view.verticalScrollBar()->value()));
        CHECK(tab.fileEncoding(file).source == FileEncoding::Source::User
                  && tab.fileEncoding(file).decoder.encoding() == encoding,
              what + ": the tab records the user's choice");
        return cur.row();
    };
    int row = switchTo(TextEncoding::Koi8R, "resident switch KOI8-R");
    CHECK(model.rowCount() == 1500 && !model.messageAt(row).contains(QStringLiteral("Запись")),
          "resident switch KOI8-R: reread with the new encoding: " + model.messageAt(row));
    row = switchTo(TextEncoding::Windows1251, "resident switch back");
    CHECK(model.messageAt(row).endsWith(QStringLiteral("Запись номер 700: ошибка диска")),
          "resident switch back: original text: " + model.messageAt(row));

    // Выбор посреди первой загрузки: загрузка доходит, затем файл
    // перечитывается выбранной кодировкой; решение автоматики, пришедшее
    // позже выбора, его не заменяет.
    {
        LogViewWidget early;
        early.setParserPattern(schema);
        early.setExtractionEnabled(true);
        early.setFallbackEncoding(TextEncoding::Windows1251);
        early.addLogFile(path);
        const LogFilePtr earlyFile = early.loadedFiles().value(0);
        early.setFileEncoding(earlyFile, TextEncoding::Koi8R);
        CHECK(settled(early), "resident switch during load: settles");
        CHECK(testdocs::idleViews(), "views settle");
        CHECK(settled(early), "resident switch during load: the reread settles");
        const FileEncoding enc = early.fileEncoding(earlyFile);
        CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Koi8R,
              "resident switch during load: the user's choice stands");
        CHECK(early.model()->rowCount() == 1500
                  && !early.model()->messageAt(700).contains(QStringLiteral("Запись")),
              "resident switch during load: the file is read with the choice: "
                  + early.model()->messageAt(700));
    }

    // Выбор переживает дозапись.
    tab.setFileEncoding(file, TextEncoding::Ibm866);
    CHECK(settled(tab), "resident switch tail: settles");
    CHECK(testdocs::appendToFile(path, testdocs::encodeText(
              recordText(3000, "INFO", QStringLiteral("хвост после выбора")), TextEncoding::Ibm866)),
          "resident switch tail: append");
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), "resident switch tail: reload settles");
    CHECK(model.messageAt(model.rowCount() - 1).endsWith(QStringLiteral("хвост после выбора")),
          "resident switch tail: the tail is read with the user's encoding: "
              + model.messageAt(model.rowCount() - 1));
}

// Шаг 3а F-1: предварительное решение кодировки на живой вкладке.
// Растущий UTF-8-лог, первый байт ≥ 0x80 которого — одиночный мусорный байт
// (предварительное Windows-1251). Кириллица UTF-8 приходит дозаписью: сперва
// в предварительную строку без '\n', оборванную посреди символа, затем
// остальным. Решение пересматривается без переиндексации и без полной
// перезагрузки, текущая строка, выделение и её место на экране — на месте;
// итоговый текст — чтение файла как UTF-8.
static void checkProvisionalRevision(const QDir& dir, const QString& name, const QString& schema)
{
    QByteArray head;
    for (int i = 0; i < 1500; ++i)
        head += record(i / 10, "INFO", "ascii head " + QByteArray::number(i));
    head.insert(head.indexOf('\n'), " \xFF");
    const QString path = testdocs::writeFile(dir, name + "-provisional.log", head);
    LogViewWidget tab;
    tab.resize(900, 400);
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    tab.show();
    CHECK(settled(tab), name + " provisional: initial load");
    CHECK(testdocs::idleViews(), "views settle");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    const LogFilePtr file = tab.loadedFiles().value(0);
    CHECK(tab.fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251
              && tab.fileEncoding(file).provisional,
          name + " provisional: a lone garbage byte decides Windows-1251 provisionally");

    int indexingStarts = 0, parsingStarts = 0, resets = 0;
    QObject spies;
    QObject::connect(&tab, &LogViewWidget::fileParsingStarted, &spies, [&] { ++parsingStarts; });
    QObject::connect(&model, &QAbstractItemModel::modelReset, &spies, [&] { ++resets; });
    const auto spyIndexers = [&] {
        for (LogIndexer* indexer : tab.findChildren<LogIndexer*>())
            QObject::connect(indexer, &LogIndexer::indexingStarted, &spies, [&] { ++indexingStarts; },
                             Qt::UniqueConnection);
    };
    spyIndexers();

    view.goToRow(700);
    CHECK(testdocs::idleViews(), "views settle");
    const QModelIndex start = view.currentIndex();
    const auto entry = start.isValid() ? model.entryAt(start.row()) : nullptr;
    const int lineNumber = entry ? entry->originalLineNumber() : -1;
    const QRect rect = view.visualRect(start);
    const auto checkPlace = [&](const QString& what) {
        const QModelIndex cur = view.currentIndex();
        const auto now = cur.isValid() ? model.entryAt(cur.row()) : nullptr;
        CHECK(now && lineNumber == 701 && now->originalLineNumber() == lineNumber,
              what + QStringLiteral(": the current line is kept (%1)").arg(now ? now->originalLineNumber() : -1));
        CHECK(cur.isValid() && view.selectionModel()->isRowSelected(cur.row(), QModelIndex()),
              what + ": the current row stays selected");
        CHECK(cur.isValid() && view.visualRect(cur).top() == rect.top(),
              what + QStringLiteral(": the current row stays at its place on screen (%1 → %2)")
                         .arg(rect.top()).arg(cur.isValid() ? view.visualRect(cur).top() : -1));
    };
    const auto append = [&](const QByteArray& bytes, const QString& what) {
        CHECK(testdocs::appendToFile(path, bytes), what + ": append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), what + ": settles");
        CHECK(testdocs::idleViews(), "views settle");
        spyIndexers();
    };

    // Предварительная строка без '\n', оборванная посреди «к» (UTF-8).
    const QByteArray cyr = QStringLiteral("Ошибка соединения с базой данных, повтор через минуту").toUtf8();
    const QByteArray first = record(200, "ERROR", cyr).chopped(1);
    const qsizetype cut = first.indexOf("\xD0\xBA") + 1; // внутри «к»
    append(first.left(cut), name + " provisional: partial line");
    CHECK(tab.fileEncoding(file).decoder.isUtf8(),
          name + " provisional: UTF-8 in the provisional line revises the decision: "
              + TextEncodings::displayName(tab.fileEncoding(file).decoder.encoding()));
    checkPlace(name + " provisional: after the partial line");
    // Остаток строки и ещё кириллица — частями.
    QByteArray rest = first.mid(cut) + '\n';
    for (int i = 0; i < 6; ++i)
        rest += record(201 + i, i % 2 ? "WARN" : "INFO",
                       QStringLiteral("Запись номер %1: диск заполнен на девяносто процентов").arg(i).toUtf8());
    append(rest.left(rest.size() / 2), name + " provisional: first half");
    append(rest.mid(rest.size() / 2), name + " provisional: second half");
    checkPlace(name + " provisional: after the tail");
    const FileEncoding final = tab.fileEncoding(file);
    CHECK(final.decoder.isUtf8() && final.source == FileEncoding::Source::Auto && !final.provisional,
          name + " provisional: UTF-8, final, still automatic");

    CHECK(indexingStarts == 0 && parsingStarts == 0,
          name + QStringLiteral(" provisional: no reindexing / full reload (%1 indexer start(s), "
                                "%2 parse start(s))").arg(indexingStarts).arg(parsingStarts));
    CHECK(resets >= 1, name + " provisional: the revision rereads shown rows (reset)");

    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly), name + " provisional: reopen");
    QStringList want = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
    if (!want.isEmpty() && want.constLast().isEmpty())
        want.removeLast();
    CHECK(rows(model) == want, name + " provisional: every row reads as the file in UTF-8");

    // Окончательное решение не пересматривается: блок мусора и Windows-1251.
    QByteArray garbage;
    for (int i = 0; i < 400; ++i)
        garbage += char(0x80 + (i * 5) % 0x80);
    append(record(300, "WARN", "binary " + garbage)
               + record(301, "INFO", testdocs::encodeText(QStringLiteral("Запись в чужой кодировке"),
                                                          TextEncoding::Windows1251)),
           name + " provisional: garbage after final");
    CHECK(tab.fileEncoding(file).decoder.isUtf8(),
          name + " provisional: a final decision survives garbage");
}

// Шаг 3а: решение, которое по ходу загрузки только набирает силу, лишних
// reset и перечитываний не делает. Windows-1251-лог: короткое слово в
// начале (предварительно), русский текст — дальше окна 64 КБ (окончательно,
// та же кодировка). Сравнение — с тем же логом без раннего слова
// (окончательно с первой порции). Ещё: выбор пользователя автоматикой не
// пересматривается.
static void checkProvisionalNoExtraWork(const QDir& dir, const QString& name, const QString& schema)
{
    QString filler;
    for (int i = 0; i < 1300; ++i)
        filler += recordText(i / 10, "INFO", QStringLiteral("ascii filler %1").arg(i));
    QString russian;
    for (int i = 0; i < 300; ++i)
        russian += recordText(200 + i / 10, i % 3 ? "INFO" : "ERROR",
                              QStringLiteral("Запись номер %1: ошибка диска").arg(i));
    QString withWord = filler;
    withWord.replace(QStringLiteral("ascii filler 3\n"), QStringLiteral("ascii filler 3 старт\n"));
    struct Load { int resets = 0; int parsers = 0; int indexerStarts = 0; FileEncoding encoding; QString row; };
    const auto load = [&](const QString& text, const QString& tag) {
        Load out;
        const QString path = testdocs::writeFile(dir, name + "-" + tag + ".log",
                                                 testdocs::encodeText(text, TextEncoding::Windows1251));
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.setFallbackEncoding(TextEncoding::Windows1251);
        QObject spies;
        QObject::connect(tab.model(), &QAbstractItemModel::modelReset, &spies, [&] { ++out.resets; });
        QObject::connect(&tab, &LogViewWidget::fileParsingStarted, &spies, [&] { ++out.indexerStarts; });
        tab.addLogFile(path);
        CHECK(settled(tab), name + " no extra work: load " + tag);
        CHECK(testdocs::idleViews(), "views settle");
        out.parsers = int(tab.findChildren<LogParser*>().size());
        out.encoding = tab.fileEncoding(tab.loadedFiles().value(0));
        out.row = tab.model()->messageAt(tab.model()->rowCount() - 1);
        return out;
    };
    const Load late = load(withWord + russian, "late-word");
    const Load dense = load(filler + russian, "dense");
    CHECK(late.encoding.decoder.encoding() == TextEncoding::Windows1251
              && late.encoding.source == FileEncoding::Source::Auto && !late.encoding.provisional,
          name + " no extra work: stays Windows-1251 and becomes final");
    CHECK(late.resets == dense.resets && late.parsers == dense.parsers
              && late.indexerStarts == dense.indexerStarts,
          name + QStringLiteral(" no extra work: resets %1/%2, parsers %3/%4, starts %5/%6")
                     .arg(late.resets).arg(dense.resets).arg(late.parsers).arg(dense.parsers)
                     .arg(late.indexerStarts).arg(dense.indexerStarts));
    CHECK(late.row.endsWith(QStringLiteral("Запись номер 299: ошибка диска")),
          name + " no extra work: text readable: " + late.row);

    // Выбор пользователя на предварительном решении: дозапись, которая
    // пересмотрела бы автоматику, его не трогает; автоматика под выбором
    // продолжает решать рядом (шаг 4) — для «Определить автоматически».
    {
        const QString path = testdocs::writeFile(dir, name + "-user.log",
                                                 testdocs::encodeText(withWord, TextEncoding::Windows1251));
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.setFallbackEncoding(TextEncoding::Windows1251);
        tab.addLogFile(path);
        CHECK(settled(tab), name + " user: load");
        const LogFilePtr file = tab.loadedFiles().value(0);
        CHECK(tab.fileEncoding(file).provisional, name + " user: provisional before the choice");
        tab.setFileEncoding(file, TextEncoding::Koi8R);
        CHECK(settled(tab), name + " user: switch settles");
        CHECK(testdocs::appendToFile(path, russian.toUtf8()), name + " user: append UTF-8");
        tab.reloadChangedFiles(true);
        CHECK(settled(tab), name + " user: reload settles");
        const FileEncoding enc = tab.fileEncoding(file);
        CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Koi8R
                  && enc.automatic().decoder.encoding() == TextEncoding::Utf8
                  && enc.automatic().source == FileEncoding::Source::Auto,
              name + " user: the choice stands, the automatic decision beside it is revised");
        tab.restoreAutomaticEncoding(file);
        CHECK(settled(tab), name + " user: «detect automatically» settles");
        CHECK(tab.fileEncoding(file).source == FileEncoding::Source::Auto
                  && tab.fileEncoding(file).decoder.isUtf8(),
              name + " user: «detect automatically» returns the automatic decision");
    }
}

// Шаг 3а: переход порога резидентная → индексная сохраняет предварительное
// решение предварительным (и накопленное свидетельство): индексатор
// продолжает копить, кириллица UTF-8 после перехода пересматривает решение.
// Обратного перехода с тем же содержимым не бывает (индексная вкладка
// откатывается в резидентную только на переписанном в UTF-16 файле — решение
// автоматики к новому содержимому не относится); перенос состояния туда же
// проверяет tests/textencoding (два прохода разными «бэкендами»).
static void checkProvisionalThreshold(const QDir& dir, const QString& schema)
{
    QByteArray head;
    for (int i = 0; i < 400; ++i)
        head += record(i / 10, "INFO", "ascii head " + QByteArray::number(i));
    head.insert(head.indexOf('\n'), " \xFF");
    const QString path = testdocs::writeFile(dir, "provisional-threshold.log", head);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    CHECK(settled(tab), "provisional threshold: initial load");
    const LogFilePtr file = tab.loadedFiles().value(0);
    CHECK(!tab.model()->isIndexedBackend() && tab.fileEncoding(file).provisional,
          "provisional threshold: resident, provisional");
    QByteArray more;
    for (int i = 400; i < 24000; ++i)
        more += record(i / 10, "INFO", "ascii more " + QByteArray::number(i));
    CHECK(testdocs::appendToFile(path, more), "provisional threshold: append past 1 MB");
    tab.reloadChangedFiles(true);
    CHECK(settled(tab), "provisional threshold: conversion settles");
    CHECK(tab.model()->isIndexedBackend(), "provisional threshold: converted to indexed");
    const FileEncoding carried = tab.fileEncoding(file);
    CHECK(carried.source == FileEncoding::Source::Auto && carried.provisional
              && carried.decoder.encoding() == TextEncoding::Windows1251,
          "provisional threshold: the decision stays provisional across the threshold");
    QByteArray tail;
    for (int i = 0; i < 6; ++i)
        tail += record(3000 + i, "ERROR", QStringLiteral("Ошибка диска номер %1, повтор через минуту").arg(i).toUtf8());
    CHECK(testdocs::appendToFile(path, tail), "provisional threshold: append UTF-8");
    tab.reloadChangedFiles(true);
    CHECK(settled(tab), "provisional threshold: tail settles");
    const FileEncoding revised = tab.fileEncoding(file);
    CHECK(revised.decoder.isUtf8() && !revised.provisional,
          "provisional threshold: the indexed tab revises to final UTF-8");
    CHECK(tab.model()->messageAt(tab.model()->rowCount() - 1)
              .endsWith(QStringLiteral("Ошибка диска номер 5, повтор через минуту")),
          "provisional threshold: the tail reads as UTF-8");
}

// ---------------------------------------------------------------------------
// Сохранённые идентификаторы строк на живой вкладке (R-1, шаг 3)
// ---------------------------------------------------------------------------

struct SavedId {
    SourceIdentity source;
    RecordId id;
    qint64 line = 0;
    QString text;
};

struct Answers {
    QHash<int, RecordKey> resolved;
    QHash<int, RecordLocator::Refusal> refused;
};

static void listen(LogModel& model, Answers& out, QObject* context)
{
    QObject::connect(&model.recordLocator(), &RecordLocator::resolved, context,
                     [&out](int token, const RecordKey& key) { out.resolved.insert(token, key); });
    QObject::connect(&model.recordLocator(), &RecordLocator::refused, context,
                     [&out](int token, RecordLocator::Refusal r) { out.refused.insert(token, r); });
}

static int rowOfLine(const LogModel& model, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.keyAt(row).line == line)
            return row;
    return -1;
}

static QVector<SavedId> saveLines(LogViewWidget& tab, const QVector<qint64>& lines)
{
    QVector<SavedId> out;
    LogModel& model = *tab.model();
    for (const qint64 line : lines) {
        const int row = rowOfLine(model, line);
        const RecordKey key = model.keyAt(row);
        const auto source = model.recordLocator().sourceFor(key.file);
        CHECK(row >= 0 && source, QStringLiteral("сохранение строки %1").arg(line));
        if (row < 0 || !source)
            continue;
        out.append({ *source, model.recordLocator().idFor(key, model.messageAt(row)), line,
                     model.messageAt(row) });
    }
    return out;
}

// Открыть файл во вкладке, попросив строки до загрузки.
static QHash<int, SavedId> requestAndOpen(LogViewWidget& tab, const QString& path,
                                          const QVector<SavedId>& saved, const QString& schema)
{
    tab.setParserPattern(schema);
    QHash<int, SavedId> tokens;
    for (const SavedId& s : saved)
        tokens.insert(tab.model()->recordLocator().request(s.source, s.id), s);
    tab.addLogFile(path);
    return tokens;
}

static void checkLocatorLive(const QDir& dir, const QString& schema)
{
    AppSettings& settings = AppSettings::instance();
    using Refusal = RecordLocator::Refusal;

    // ---- Один файл — один LogFile: другое написание пути того же файла ------
    {
        const QString path = testdocs::writeFile(dir, "loc-same.log", record(1, "INFO", "one"));
        QDir(dir).mkpath("sub");
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.addLogFile(path);
        tab.addLogFile(dir.absolutePath() + "/sub/../loc-same.log");
        tab.addLogFile(dir.absolutePath() + "/./loc-same.log");
        CHECK(settled(tab), "locator/same file: load");
        CHECK(tab.fileCount() == 1 && tab.model()->rowCount() == 1,
              QStringLiteral("другое написание пути — тот же файл: файлов %1, строк %2")
                  .arg(tab.fileCount()).arg(tab.model()->rowCount()));
    }

    // ---- Вкладка переходит порог с фильтром: фильтр действует сразу ----------
    {
        settings.setIndexedThresholdMB(1);
        QByteArray start;
        for (int i = 0; i < 2000; ++i)
            start += record(i / 50, i % 5 == 0 ? "ERROR" : "INFO", "line " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "loc-grows.log", start);
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.addLogFile(path);
        CHECK(settled(tab), "locator/threshold: load");
        tab.model()->setLogLevelFilter({ LogLevel::Error });
        CHECK(settled(tab), "locator/threshold: filter");
        const int idHidden = tab.bookmarks().add(0); // строка 1 — ERROR, видна
        tab.model()->setLogLevelFilter({});
        CHECK(settled(tab), "locator/threshold: filter off");
        const int idInfo = tab.bookmarks().add(rowOfLine(*tab.model(), 2)); // INFO
        tab.model()->setLogLevelFilter({ LogLevel::Error });
        CHECK(settled(tab), "locator/threshold: filter on");
        QByteArray more;
        for (int i = 2000; i < 30000; ++i)
            more += record(i / 50, i % 5 == 0 ? "ERROR" : "INFO", "line " + QByteArray::number(i));
        CHECK(testdocs::appendToFile(path, more), "locator/threshold: append past threshold");
        tab.reloadChangedFiles(true);
        CHECK(settled(tab), "locator/threshold: conversion");
        CHECK(tab.model()->isIndexedBackend(), "locator/threshold: indexed now");
        CHECK(tab.model()->rowCount() == 6000,
              QStringLiteral("locator/threshold: фильтр модели действует после конверсии: %1 строк")
                  .arg(tab.model()->rowCount()));
        CHECK(tab.bookmarks().status(idHidden) == BookmarkStore::Status::Visible
                  && tab.bookmarks().status(idInfo) == BookmarkStore::Status::Hidden,
              "locator/threshold: закладки — видимая и скрытая фильтром");
        settings.setIndexedThresholdMB(512);
    }

    // ---- Индексная → резидентная (UTF-16 — откат индексатора) ---------------
    {
        QString text;
        for (int i = 0; i < 300; ++i)
            text += QString::fromLatin1(record(i, "INFO", "wide " + QByteArray::number(i)));
        QByteArray bytes("\xFF\xFE", 2);
        auto enc = QStringEncoder(QStringConverter::Utf16LE);
        bytes += QByteArray(enc(text));
        const QString path = testdocs::writeFile(dir, "loc-utf16.log", bytes);
        QVector<SavedId> saved;
        {
            LogViewWidget tab;
            tab.setParserPattern(schema);
            tab.addLogFile(path);
            CHECK(settled(tab), "locator/utf16: resident load");
            saved = saveLines(tab, { 1, 150, 300 });
        }
        settings.setIndexedThresholdMB(0);
        LogViewWidget tab;
        QObject context;
        Answers out;
        listen(*tab.model(), out, &context);
        const QHash<int, SavedId> tokens = requestAndOpen(tab, path, saved, schema);
        CHECK(settled(tab), "locator/utf16: indexed → resident fallback");
        CHECK(!tab.model()->isIndexedBackend(), "locator/utf16: resident after fallback");
        CHECK(out.resolved.size() == saved.size() && out.refused.isEmpty(),
              QStringLiteral("locator/utf16: найдено %1 из %2").arg(out.resolved.size()).arg(saved.size()));
        for (auto it = out.resolved.cbegin(); it != out.resolved.cend(); ++it)
            CHECK(it->line == tokens[it.key()].line
                      && tab.model()->messageAt(tab.model()->rowForKey(it.value())) == tokens[it.key()].text,
                  "locator/utf16: та же строка");
        settings.setIndexedThresholdMB(512);
    }

    for (const int threshold : { 512, 0 }) {
        settings.setIndexedThresholdMB(threshold);
        const QString tag = threshold ? QStringLiteral("resident") : QStringLiteral("indexed");
        QByteArray bytes;
        for (int i = 0; i < 400; ++i) {
            bytes += record(i, i % 7 == 0 ? "ERROR" : "INFO", "event " + QByteArray::number(i));
            if (i % 9 == 4)
                bytes += "    at detail " + QByteArray::number(i) + '\n';
        }
        const QString path = testdocs::writeFile(dir, QStringLiteral("loc-%1.log").arg(tag), bytes);

        // ---- Дозапись: сохранённое до и после неё находится -----------------
        QVector<SavedId> saved;
        {
            LogViewWidget tab;
            tab.setParserPattern(schema);
            tab.addLogFile(path);
            CHECK(settled(tab), tag + ": load");
            saved = saveLines(tab, { 1, 6, 200, 444 });
            CHECK(testdocs::appendToFile(path, record(500, "INFO", "appended one")
                                                   + "    appended continuation\n"),
                  tag + ": append");
            tab.reloadChangedFiles(true);
            CHECK(settled(tab), tag + ": append settles");
            const qint64 lastLine = tab.model()->keyAt(tab.model()->rowCount() - 1).line;
            saved += saveLines(tab, { lastLine - 1, lastLine });
            CHECK(saved.constLast().source.size == QFileInfo(path).size(),
                  tag + ": идентичность — по концу прочитанного после дозаписи");
        }
        {
            LogViewWidget tab;
            QObject context;
            Answers out;
            listen(*tab.model(), out, &context);
            const QHash<int, SavedId> tokens = requestAndOpen(tab, path, saved, schema);
            CHECK(settled(tab), tag + ": reopen");
            CHECK(out.resolved.size() == saved.size() && out.refused.isEmpty(),
                  QStringLiteral("%1: после дозаписи найдено %2 из %3").arg(tag).arg(out.resolved.size()).arg(saved.size()));
            for (auto it = out.resolved.cbegin(); it != out.resolved.cend(); ++it) {
                const int row = tab.model()->rowForKey(it.value());
                CHECK(row >= 0 && tab.model()->messageAt(row) == tokens[it.key()].text,
                      QStringLiteral("%1: строка %2 на месте").arg(tag).arg(tokens[it.key()].line));
            }
            // Дозапись в открытую вкладку найденного не трогает.
            CHECK(testdocs::appendToFile(path, record(501, "INFO", "more")), tag + ": append again");
            tab.reloadChangedFiles(true);
            CHECK(settled(tab), tag + ": append again settles");
            for (const RecordKey& key : std::as_const(out.resolved))
                CHECK(tab.model()->rowForKey(key) >= 0, tag + ": ключи пережили дозапись");

            // ---- Полная перезагрузка: файл исчез, сохранённое ждёт его ----------
            const QByteArray content = [&] {
                QFile f(path);
                return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
            }();
            CHECK(QFile::remove(path), tag + ": file removed");
            tab.reloadChangedFiles(true);
            CHECK(settled(tab) && tab.model()->rowCount() == 0, tag + ": rows dropped");
            Answers late;
            QObject lateContext;
            listen(*tab.model(), late, &lateContext);
            QHash<int, SavedId> waiting;
            for (const SavedId& s : saved)
                waiting.insert(tab.model()->recordLocator().request(s.source, s.id), s);
            CHECK(testdocs::idleViews(), "views settle");
            CHECK(late.resolved.isEmpty() && late.refused.isEmpty(),
                  tag + ": пока файла нет — запросы ждут (не SourceMissing)");
            QFile f(path);
            CHECK(f.open(QIODevice::WriteOnly) && f.write(content) == content.size(), tag + ": file back");
            f.close();
            tab.reloadChangedFiles(true);
            CHECK(settled(tab), tag + ": full reload");
            CHECK(late.resolved.size() == saved.size() && late.refused.isEmpty(),
                  QStringLiteral("%1: файл вернули тем же — найдено %2 из %3")
                      .arg(tag).arg(late.resolved.size()).arg(saved.size()));
        }

        // ---- Ротация с переименованием ------------------------------------------
        {
            const QString rotated = path + ".1";
            QFile::remove(rotated);
            CHECK(QFile::rename(path, rotated), tag + ": rotate");
            QByteArray fresh;
            for (int i = 0; i < 50; ++i)
                fresh += record(600 + i, "INFO", "fresh " + QByteArray::number(i));
            testdocs::writeFile(dir, QFileInfo(path).fileName(), fresh);
            LogViewWidget byOldPath;
            QObject context;
            Answers out;
            listen(*byOldPath.model(), out, &context);
            requestAndOpen(byOldPath, path, saved, schema);
            CHECK(settled(byOldPath), tag + ": open new file");
            bool allChanged = out.refused.size() == saved.size() && out.resolved.isEmpty();
            for (const auto r : std::as_const(out.refused))
                allChanged = allChanged && r == Refusal::SourceChanged;
            CHECK(allChanged, tag + ": по старому пути — SourceChanged");

            // Перепривязка к переименованному файлу проходит проверку источника.
            QVector<SavedId> moved = saved;
            for (SavedId& s : moved)
                s.source.path = QFileInfo(rotated).absoluteFilePath();
            LogViewWidget byNewPath;
            Answers out2;
            listen(*byNewPath.model(), out2, &context);
            requestAndOpen(byNewPath, rotated, moved, schema);
            CHECK(settled(byNewPath), tag + ": open rotated");
            CHECK(out2.resolved.size() == saved.size() && out2.refused.isEmpty(),
                  QStringLiteral("%1: переименованный — найдено %2 из %3").arg(tag).arg(out2.resolved.size()).arg(saved.size()));
        }
    }
    settings.setIndexedThresholdMB(512);

    // ---- Закрытие вкладки посреди фоновой проверки -----------------------------
    {
        settings.setIndexedThresholdMB(0);
        QByteArray big;
        for (int i = 0; i < 200000; ++i)
            big += record(i / 1000, "INFO", "row " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "loc-closing.log", big);
        QVector<SavedId> saved;
        {
            LogViewWidget tab;
            tab.setParserPattern(schema);
            tab.addLogFile(path);
            CHECK(settled(tab), "locator/closing: load");
            QVector<qint64> lines;
            for (qint64 l = 1; l <= 200000; l += 40)
                lines.append(l);
            QVector<SavedId> all;
            LogModel& model = *tab.model();
            const auto source = model.recordLocator().sourceFor(tab.loadedFiles().value(0));
            for (const qint64 l : lines) {
                const int row = int(l - 1); // одна индексная вкладка: row == line - 1
                all.append({ *source, model.recordLocator().idFor(model.keyAt(row), model.messageAt(row)), l, {} });
            }
            saved = all;
        }
        bool caughtBusy = false;
        for (int attempt = 0; attempt < 5 && !caughtBusy; ++attempt) {
            auto tab = std::make_unique<LogViewWidget>();
            QPointer<RecordLocator> locator(&tab->model()->recordLocator());
            requestAndOpen(*tab, path, saved, schema);
            testdocs::waitFor([&] { return locator && locator->stats().byteChecks == 0 && locator->isBusy()
                                           && locator->stats().feeds > 0; }, 10000);
            caughtBusy = locator && locator->isBusy() && locator->stats().feeds > 0;
            tab.reset(); // закрыли посреди проверки
            testdocs::waitFor([&] { return !locator; }, 10000);
            CHECK(!locator, "locator/closing: локатор ушёл вместе с вкладкой");
        }
        CHECK(caughtBusy, "locator/closing: вкладку закрыли, пока шла фоновая проверка");
        settings.setIndexedThresholdMB(512);
    }
}

// ---------------------------------------------------------------------------
// Память файла (R-4, шаг 2): ротация (app.log → app.log.1, новый app.log) и
// перенос файла в другую папку. Память находит прежнее содержимое по нему
// самому; новый app.log открывается чистым.
// ---------------------------------------------------------------------------

static QByteArray memoryLog(const QByteArray& tag, int lines)
{
    QByteArray bytes;
    for (int i = 0; i < lines; ++i)
        bytes += record(i, i % 9 == 4 ? "ERROR" : "INFO", tag + ' ' + QByteArray::number(i));
    return bytes;
}

static std::unique_ptr<LogViewWidget> memoryTab(FileMemoryStore& store, const QString& path,
                                                const QString& schema)
{
    auto tab = std::make_unique<LogViewWidget>();
    tab->setParserPattern(schema);
    tab->setFileMemory(&store);
    tab->resize(800, 300);
    tab->show();
    tab->addLogFile(path);
    CHECK(testdocs::waitFor([&] { return !tab->isLoading() && !tab->isPositionPending(); }, 60000)
              && testdocs::settleViews(*tab->model(), [&] { return tab->isLoading() || tab->isPositionPending(); }),
          QStringLiteral("memory: load %1").arg(path));
    return tab;
}

// Закладки 10 и 50 с заметками, выбрана строка 30; память записана.
static void rememberFile(FileMemoryStore& store, const QString& path, const QString& schema)
{
    auto tab = memoryTab(store, path, schema);
    LogModel& model = *tab->model();
    tab->bookmarks().add(rowOfLine(model, 10), QStringLiteral("ten"));
    tab->bookmarks().add(rowOfLine(model, 50), QStringLiteral("fifty"));
    tab->view()->goToRow(rowOfLine(model, 30));
    tab->saveFileMemory();
    store.waitForWrites();
}

static void checkRemembered(LogViewWidget& tab, const QString& what)
{
    LogModel& model = *tab.model();
    QStringList found;
    for (const Bookmark& b : tab.bookmarks().bookmarks())
        if (tab.bookmarks().visibleRow(b.id) == rowOfLine(model, b.line))
            found << QStringLiteral("%1:%2").arg(b.line).arg(b.note);
    CHECK(found == (QStringList{ QStringLiteral("10:ten"), QStringLiteral("50:fifty") }),
          what + QStringLiteral(": закладки на своих строках: ") + found.join(QStringLiteral(", ")));
    CHECK(tab.view()->userSelectedRow() == rowOfLine(model, 30),
          what + QStringLiteral(": выбрана строка 30"));
}

static void checkClean(LogViewWidget& tab, const QString& what)
{
    CHECK(tab.bookmarks().visibleRows().isEmpty(), what + QStringLiteral(": закладок на строках нет"));
    for (const Bookmark& b : tab.bookmarks().bookmarks())
        CHECK(tab.bookmarks().status(b.id) == BookmarkStore::Status::Missing,
              what + QStringLiteral(": прежняя закладка %1 — серая").arg(b.line));
    CHECK(tab.view()->userSelectedRow() < 0 && !tab.hasOfferedFilters(),
          what + QStringLiteral(": ни позиции, ни фильтров"));
}

static void checkFileMemoryRotation(const QDir& dir, const QString& name, const QString& schema)
{
    for (const bool rotatedFirst : { false, true }) {
        const QString label = QStringLiteral("memory/%1/rotation (%2)")
            .arg(name, rotatedFirst ? QStringLiteral("app.log.1 first") : QStringLiteral("app.log first"));
        QDir work(dir.filePath(QStringLiteral("memory-%1-%2").arg(name).arg(int(rotatedFirst))));
        QDir().mkpath(work.path());
        FileMemoryStore store(work.filePath(QStringLiteral("memory")));
        const QString app = testdocs::writeFile(work, "app.log", memoryLog("first", 200));
        rememberFile(store, app, schema);

        // Ротация: app.log → app.log.1, новый app.log с другим содержимым.
        const QString rotated = work.filePath(QStringLiteral("app.log.1"));
        CHECK(QFile::rename(app, rotated), label + QStringLiteral(": rename"));
        testdocs::writeFile(work, "app.log", memoryLog("second", 120));

        if (rotatedFirst) {
            auto old = memoryTab(store, rotated, schema);
            checkRemembered(*old, label + QStringLiteral(": app.log.1"));
            auto fresh = memoryTab(store, app, schema);
            checkClean(*fresh, label + QStringLiteral(": новый app.log"));
            CHECK(fresh->bookmarks().isEmpty(), label + QStringLiteral(": память уехала к app.log.1"));
            CHECK(fresh->model()->rowCount() == 120, label + QStringLiteral(": новое содержимое"));
        } else {
            auto fresh = memoryTab(store, app, schema);
            checkClean(*fresh, label + QStringLiteral(": новый app.log"));
            CHECK(fresh->model()->rowCount() == 120, label + QStringLiteral(": новое содержимое"));
            fresh->saveFileMemory(); // закрыть: память нового содержимого — без серых закладок
            auto old = memoryTab(store, rotated, schema);
            checkRemembered(*old, label + QStringLiteral(": app.log.1"));
        }
        // И ещё раз: обе памяти — по своим путям.
        store.waitForWrites();
        FileMemoryStore probe(store.directory());
        CHECK(probe.open(rotated).found == FileMemoryStore::Found::ByPath,
              label + QStringLiteral(": app.log.1 — память по пути"));
    }

    // Перенос в другую папку.
    const QString label = QStringLiteral("memory/%1/move").arg(name);
    QDir work(dir.filePath(QStringLiteral("memory-%1-move").arg(name)));
    QDir().mkpath(work.filePath(QStringLiteral("archive")));
    FileMemoryStore store(work.filePath(QStringLiteral("memory")));
    const QString path = testdocs::writeFile(work, "service.log", memoryLog("moved", 150));
    rememberFile(store, path, schema);
    const QString moved = work.filePath(QStringLiteral("archive/service.log"));
    CHECK(QFile::rename(path, moved), label + QStringLiteral(": rename"));
    auto tab = memoryTab(store, moved, schema);
    checkRemembered(*tab, label);
    store.waitForWrites();
    CHECK(!QFileInfo::exists(store.directory() + QLatin1Char('/') + FileMemoryStore::fileNameForPath(path))
              && QFileInfo::exists(store.directory() + QLatin1Char('/') + FileMemoryStore::fileNameForPath(moved)),
          label + QStringLiteral(": память переехала"));
}

// ---- Закрыть вкладку и вернуть (Ctrl+Shift+T, R-5) ------------------------------

static int rowOfPathLine(const LogModel& model, const QString& path, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const RecordKey key = model.keyAt(row);
        if (key.line == line && key.file && LogViewWidget::samePath(key.file->filePath, path))
            return row;
    }
    return -1;
}

static bool tabSettled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading() && !tab.isPositionPending(); }, 60000)
        && testdocs::settleViews(*tab.model(), [&] {
               return tab.isLoading() || tab.isPositionPending() || tab.fullLog().isPlacementPending();
           });
}

// Ряд вкладок окна: QTabWidget, контроллер, создание вкладки на месте.
struct TabRow {
    QString schema;
    QTabWidget widget;
    SessionController controller;

    explicit TabRow(const QString& s) : schema(s)
    {
        widget.resize(900, 400);
        widget.show();
    }
    LogViewWidget* make(int position = -1)
    {
        auto* tab = new LogViewWidget();
        tab->setParserPattern(schema);
        const int at = position < 0 ? widget.count() : qMin(position, widget.count());
        widget.setCurrentIndex(widget.insertTab(at, tab, QStringLiteral("Logs")));
        return tab;
    }
    LogViewWidget* open(const QStringList& paths)
    {
        LogViewWidget* tab = make();
        for (const QString& path : paths)
            tab->addLogFile(path);
        CHECK(tabSettled(*tab), QStringLiteral("reopen: load %1").arg(paths.join(QLatin1Char(','))));
        return tab;
    }
    LogViewWidget* at(int index) const { return qobject_cast<LogViewWidget*>(widget.widget(index)); }
    // Как MainWindow::closeTab: снимок в стек ДО удаления страницы.
    bool close(int index)
    {
        LogViewWidget* tab = at(index);
        const bool remembered = controller.rememberClosedTab(tab, index);
        widget.removeTab(index);
        delete tab;
        return remembered;
    }
    SessionController::OpenResult reopen()
    {
        const auto result = controller.reopenClosedTab([this](int position) { return make(position); });
        if (result.active)
            widget.setCurrentWidget(result.active);
        return result;
    }
};

static QStringList fileNames(const LogViewWidget& tab)
{
    QStringList names;
    for (const LogFilePtr& f : tab.loadedFiles())
        names << QFileInfo(f->filePath).fileName();
    names.sort();
    return names;
}

static void checkReopenClosedTab(const QDir& dir, const QString& name, const QString& schema)
{
    const QString prefix = QStringLiteral("reopen %1: ").arg(name);
    const QString a = testdocs::writeFile(dir, name + "-reopen-a.log", memoryLog("alpha", 400));
    const QString b1 = testdocs::writeFile(dir, name + "-reopen-b1.log", memoryLog("bee", 120));
    const QString b2 = testdocs::writeFile(dir, name + "-reopen-b2.log", memoryLog("bumble", 120));
    const QString c = testdocs::writeFile(dir, name + "-reopen-c.log", memoryLog("sea", 60));

    TabRow row(schema);
    LogViewWidget* ta = row.open({ a });
    LogViewWidget* tb = row.open({ b1, b2 });
    row.open({ c });

    // ---- Вкладка A: закладки, маркер, фильтры, кодировка, выбранная строка ----
    row.widget.setCurrentWidget(ta);
    LogModel& ma = *ta->model();
    ta->bookmarks().add(rowOfPathLine(ma, a, 10), QStringLiteral("ten"));
    ta->bookmarks().add(rowOfPathLine(ma, a, 200), QStringLiteral("two hundred"));
    HighlightPattern marker;
    marker.text = QStringLiteral("alpha 1");
    marker.color = QColor(Qt::yellow);
    ma.setRowMarkers({ marker });
    FilterRule rule;
    rule.action = FilterRule::Action::Exclude;
    rule.text = QStringLiteral("alpha 13");
    FilterRuleSet rules;
    rules.rules = { rule };
    rules.bindFields(ta->parserFieldNames(), false);
    ma.setFilters({ LogLevel::Info }, QDateTime(), QDateTime(), rules);
    ta->setFileEncoding(ta->loadedFiles().first(), TextEncoding::Windows1251);
    CHECK(tabSettled(*ta), prefix + "A: filters");
    const int currentRow = rowOfPathLine(ma, a, 150);
    CHECK(currentRow >= 0, prefix + "A: line 150 visible");
    ta->view()->goToRow(currentRow);
    CHECK(testdocs::idleViews(), "views settle");
    QScrollBar* bar = ta->view()->verticalScrollBar();
    bar->setValue(bar->value() + bar->singleStep() * 3);
    CHECK(testdocs::idleViews(), "views settle");
    const int savedTop = ta->view()->visualRect(ma.index(rowOfPathLine(ma, a, 150), 0)).top();
    const int rowsA = ma.rowCount();

    // Слитая B: закладка в каждом файле.
    LogModel& mb = *tb->model();
    tb->bookmarks().add(rowOfPathLine(mb, b1, 5), QStringLiteral("b1-5"));
    tb->bookmarks().add(rowOfPathLine(mb, b2, 7), QStringLiteral("b2-7"));

    // ---- Закрыть A (место 0) и вернуть ----------------------------------------
    CHECK(row.close(0) && row.controller.closedTabCount() == 1, prefix + "A remembered");
    auto result = row.reopen();
    CHECK(result.tabs.size() == 1 && !result.hasProblems() && row.controller.report().isEmpty(),
          prefix + "A reopened without problems");
    CHECK(row.controller.closedTabCount() == 0, prefix + "stack empty after reopen");
    LogViewWidget* ra = row.at(0);
    CHECK(ra && result.tabs.value(0) == ra && row.widget.currentWidget() == ra, prefix + "A back at its place 0 and current");
    if (ra) {
        CHECK(tabSettled(*ra), prefix + "A: load");
        LogModel& m = *ra->model();
        CHECK(fileNames(*ra) == QStringList{ QFileInfo(a).fileName() } && m.rowCount() == rowsA,
              prefix + QStringLiteral("A: file and visible rows (%1 vs %2)").arg(m.rowCount()).arg(rowsA));
        QStringList marks;
        for (const Bookmark& bm : ra->bookmarks().bookmarks())
            if (ra->bookmarks().visibleRow(bm.id) == rowOfPathLine(m, a, bm.line))
                marks << QStringLiteral("%1:%2").arg(bm.line).arg(bm.note);
        marks.sort();
        CHECK(marks == (QStringList{ QStringLiteral("10:ten"), QStringLiteral("200:two hundred") }),
              prefix + "A: bookmarks with notes on their lines: " + marks.join(", "));
        CHECK(highlightPatternsToJson(m.rowMarkers()) == highlightPatternsToJson({ marker }), prefix + "A: markers");
        CHECK(m.logLevelFilter() == QSet<LogLevel>{ LogLevel::Info } && m.filterRules().toJson() == rules.toJson(),
              prefix + "A: filters");
        const int r = rowOfPathLine(m, a, 150);
        CHECK(r >= 0 && ra->view()->userSelectedRow() == r, prefix + "A: current line selected");
        if (r >= 0)
            CHECK(ra->view()->visualRect(m.index(r, 0)).top() == savedTop,
                  prefix + QStringLiteral("A: place on screen %1, was %2")
                               .arg(ra->view()->visualRect(m.index(r, 0)).top()).arg(savedTop));
        const FileEncoding enc = ra->fileEncoding(ra->loadedFiles().value(0));
        CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Windows1251,
              prefix + "A: encoding choice");
        CHECK(m.isIndexedBackend() == (name == QLatin1String("indexed")), prefix + "A: backend");
        CHECK(!ra->isBlank(), prefix + "A: not blank");
    }

    // ---- Слитая B (место 1) ---------------------------------------------------------
    CHECK(row.close(1), prefix + "B remembered");
    result = row.reopen();
    LogViewWidget* rb = row.at(1);
    CHECK(rb && result.tabs.value(0) == rb, prefix + "B back at its place 1");
    if (rb) {
        CHECK(tabSettled(*rb), prefix + "B: load");
        CHECK(fileNames(*rb).size() == 2, prefix + "B: both files");
        int found = 0;
        for (const Bookmark& bm : rb->bookmarks().bookmarks()) {
            const QString path = bm.note == QLatin1String("b1-5") ? b1 : b2;
            if (rb->bookmarks().visibleRow(bm.id) == rowOfPathLine(*rb->model(), path, bm.line))
                ++found;
        }
        CHECK(found == 2, prefix + "B: bookmarks of both files on their lines");
    }

    // ---- Места в ряду больше нет: в конец --------------------------------------------
    CHECK(row.widget.count() == 3, prefix + "three tabs");
    CHECK(row.close(2), prefix + "C remembered");
    row.close(1);
    row.close(0); // стек: C(2), B(1), A(0)
    CHECK(row.controller.closedTabCount() == 3, prefix + "three closed");
    result = row.reopen(); // A — на место 0
    result = row.reopen(); // B — на место 1
    result = row.reopen(); // C — на место 2
    CHECK(row.widget.count() == 3 && fileNames(*row.at(0)) == QStringList{ QFileInfo(a).fileName() }
              && fileNames(*row.at(2)) == QStringList{ QFileInfo(c).fileName() },
          prefix + "order restored A, B, C");
    CHECK(row.close(2), prefix + "C closed again at place 2");
    while (row.widget.count() > 0) { // остальные — мимо стека
        QWidget* page = row.widget.widget(0);
        row.widget.removeTab(0);
        delete page;
    }
    row.open({ b1 });
    CHECK(row.controller.closedTabCount() == 1, prefix + "one closed");
    result = row.reopen(); // C на место 2: в ряду одна вкладка — в конец
    CHECK(row.widget.count() == 2 && fileNames(*row.at(1)) == QStringList{ QFileInfo(c).fileName() },
          prefix + "place gone — appended at the end");
    while (row.widget.count() > 0)
        row.close(0);

    // ---- Стек из 10 -------------------------------------------------------------
    {
        TabRow many(schema);
        QStringList paths;
        for (int i = 0; i < 12; ++i)
            paths << testdocs::writeFile(dir, name + QStringLiteral("-many-%1.log").arg(i), memoryLog("m", 5));
        for (const QString& path : paths)
            many.open({ path });
        for (int i = 0; i < 12; ++i)
            many.close(0);
        CHECK(many.controller.closedTabCount() == SessionController::kClosedTabsLimit,
              prefix + QStringLiteral("stack keeps the last 10 (%1)").arg(many.controller.closedTabCount()));
        QStringList back;
        while (many.controller.closedTabCount() > 0) {
            const auto r = many.reopen();
            back << (r.tabs.isEmpty() ? QString() : fileNames(*r.tabs[0]).value(0));
        }
        QStringList expected;
        for (int i = 11; i >= 2; --i)
            expected << QFileInfo(paths[i]).fileName();
        CHECK(back == expected, prefix + "last closed first, the two oldest dropped: " + back.join(", "));
        CHECK(many.reopen().tabs.isEmpty(), prefix + "empty stack — nothing reopened");
    }

    // ---- Файл за время закрытия дописан ----------------------------------------------
    {
        TabRow r(schema);
        const QString path = testdocs::writeFile(dir, name + "-reopen-grow.log", memoryLog("grow", 100));
        LogViewWidget* t = r.open({ path });
        t->bookmarks().add(rowOfPathLine(*t->model(), path, 20), QStringLiteral("twenty"));
        r.close(0);
        CHECK(testdocs::appendToFile(path, memoryLog("more", 30)), prefix + "append while closed");
        const auto res = r.reopen();
        CHECK(!res.hasProblems() && r.controller.report().isEmpty(), prefix + "appended: no problems");
        LogViewWidget* back = r.at(0);
        CHECK(back && tabSettled(*back) && back->model()->rowCount() == 130, prefix + "appended: all lines");
        if (back) {
            const QVector<Bookmark> list = back->bookmarks().bookmarks();
            CHECK(list.size() == 1 && back->bookmarks().visibleRow(list[0].id)
                                          == rowOfPathLine(*back->model(), path, 20),
                  prefix + "appended: bookmark on its line");
        }
    }

    // ---- Переписан -------------------------------------------------------------------
    {
        TabRow r(schema);
        const QString path = testdocs::writeFile(dir, name + "-reopen-rewrite.log", memoryLog("old", 100));
        LogViewWidget* t = r.open({ path });
        t->bookmarks().add(rowOfPathLine(*t->model(), path, 20), QStringLiteral("old twenty"));
        r.close(0);
        testdocs::writeFile(dir, name + "-reopen-rewrite.log", memoryLog("new content", 50));
        const auto res = r.reopen();
        CHECK(res.changed == 1 && res.missing == 0, prefix + "rewritten: reported as changed");
        const auto report = r.controller.report();
        CHECK(report.size() == 1 && report[0].kind == SessionController::ReportEntry::Kind::Changed,
              prefix + "rewritten: report entry");
        CHECK(!SessionController::summary(res, SessionController::Origin::ClosedTab).isEmpty(),
              prefix + "rewritten: summary");
        LogViewWidget* back = r.at(0);
        CHECK(back && tabSettled(*back) && back->fileCount() == 1 && back->model()->rowCount() == 50,
              prefix + "rewritten: opened from scratch");
        if (back) {
            const QVector<Bookmark> list = back->bookmarks().bookmarks();
            CHECK(list.size() == 1 && back->bookmarks().status(list[0].id) == BookmarkStore::Status::Missing,
                  prefix + "rewritten: bookmark grey");
        }
    }

    // ---- Удалён ----------------------------------------------------------------------
    {
        TabRow r(schema);
        const QString path = testdocs::writeFile(dir, name + "-reopen-gone.log", memoryLog("gone", 100));
        LogViewWidget* t = r.open({ path });
        t->bookmarks().add(rowOfPathLine(*t->model(), path, 20), QStringLiteral("gone twenty"));
        r.close(0);
        CHECK(QFile::remove(path), prefix + "delete while closed");
        const auto res = r.reopen();
        CHECK(res.missing == 1 && res.tabs.size() == 1, prefix + "deleted: reported as missing, tab created");
        const auto report = r.controller.report();
        CHECK(report.size() == 1 && report[0].kind == SessionController::ReportEntry::Kind::Missing,
              prefix + "deleted: report entry");
        LogViewWidget* back = r.at(0);
        CHECK(back && back->fileCount() == 0 && back->bookmarks().bookmarks().size() == 1,
              prefix + "deleted: no file, bookmark waits for it");
        // Вкладка из снимка без файлов — не «пустая»: обычное открытие её не займёт.
        CHECK(back && !back->isBlank(), prefix + "deleted: snapshot tab is not reused for the next file");
        // Закрыть снова — в стек (закладки ждут файл), отчёт по закрытой вкладке ушёл.
        CHECK(r.close(0) && r.controller.report().isEmpty(), prefix + "deleted: closed again, report cleared");
    }

    // ---- stdin и пустая вкладка в стек не идут ------------------------------------
    {
        TabRow r(schema);
        LogViewWidget* blank = r.make();
        CHECK(blank->isBlank(), prefix + "new tab is blank");
        CHECK(!r.close(0), prefix + "empty tab is not remembered");
        const QString spool = StdinSpooler::spoolFilePathFor(QCoreApplication::applicationPid());
        CHECK(testdocs::writeFile(QDir(QFileInfo(spool).absolutePath()), QFileInfo(spool).fileName(),
                                  memoryLog("piped", 20)) == spool,
              prefix + "spool written");
        r.open({ spool });
        CHECK(!r.close(0) && r.controller.closedTabCount() == 0, prefix + "stdin tab is not remembered");
        QFile::remove(spool);
    }
}

// ---------------------------------------------------------------------------
// Q-19: переписано только начало файла (та же длина или рост, хвост тот же)
// ---------------------------------------------------------------------------

// Файл с узнаваемыми строками: строка 3 (в первых 4 КБ) переписывается,
// строка 200 (между областями отпечатка) и последняя остаются.
static QByteArray headFixture(const QByteArray& tag, const QByteArray& third)
{
    QByteArray out;
    for (int i = 1; i <= 300; ++i)
        out += record(i, "INFO", tag + " line " + QByteArray::number(i).rightJustified(4, '0')
                                     + (i == 3 ? " " + third : QByteArray(" steady")));
    return out;
}

// Переписать файл и сдвинуть mtime явно: настоящая запись всегда её меняет,
// а грубые часы файловой системы могли бы оставить прежнюю (предел, который
// закреплён в record_identity).
static bool rewriteLater(const QString& path, const QByteArray& bytes)
{
    const qint64 before = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(bytes) != bytes.size())
        return false;
    return f.setFileTime(QDateTime::fromMSecsSinceEpoch(before + 2000), QFileDevice::FileModificationTime);
}

enum class HeadScenario { Single, Merged, CrossedThreshold };

static void checkHeadRewrite(const QDir& dir, const QString& name, const QString& schema,
                             HeadScenario scenario)
{
    AppSettings& settings = AppSettings::instance();
    const int savedThreshold = settings.indexedThresholdMB();
    for (const bool grow : { false, true }) {
        for (const bool f5 : { false, true }) {
            const QString label = QStringLiteral("head rewrite/%1/%2/%3")
                .arg(name, grow ? QStringLiteral("grown") : QStringLiteral("same length"),
                     f5 ? QStringLiteral("F5") : QStringLiteral("timer"));
            const QString file = QStringLiteral("head-%1-%2-%3.log").arg(name).arg(int(grow)).arg(int(f5));
            const QByteArray original = headFixture("old", "alpha");
            const QString path = testdocs::writeFile(dir, file, original);
            QString other;
            if (scenario == HeadScenario::Merged) {
                QByteArray bytes;
                for (int i = 1; i <= 50; ++i)
                    bytes += record(i * 6, "WARN", "other " + QByteArray::number(i));
                other = testdocs::writeFile(dir, QStringLiteral("other-") + file, bytes);
            }

            LogViewWidget tab;
            tab.setParserPattern(schema);
            tab.setExtractionEnabled(true);
            tab.setAutoReload(true);
            tab.addLogFile(path);
            if (!other.isEmpty())
                tab.addLogFile(other);
            CHECK(settled(tab), label + ": load");
            LogModel& model = *tab.model();

            if (scenario == HeadScenario::CrossedThreshold) {
                // Резидентная вкладка дорастает за порог и становится индексной.
                CHECK(!model.isIndexedBackend(), label + ": starts resident");
                settings.setIndexedThresholdMB(1);
                QByteArray more;
                for (int i = 301; i <= 30000; ++i)
                    more += record(300 + i / 100, "INFO", "old line " + QByteArray::number(i) + " steady");
                CHECK(testdocs::appendToFile(path, more), label + ": grow past threshold");
                tab.reloadChangedFiles(true);
                CHECK(settled(tab) && model.isIndexedBackend(), label + ": converted to indexed");
            }

            QByteArray current;
            {
                QFile f(path);
                CHECK(f.open(QIODevice::ReadOnly), label + ": read back");
                current = f.readAll();
            }
            const int rowsBefore = model.rowCount();
            const int markRewritten = tab.bookmarks().add(rowOfPathLine(model, path, 3));
            const int markSteady = tab.bookmarks().add(rowOfPathLine(model, path, 200));
            CHECK(markRewritten >= 0 && markSteady >= 0, label + ": bookmarks");

            // Начало переписано той же длиной: «alpha» → «omega», хвост прежний.
            QByteArray rewritten = current;
            const qsizetype at = rewritten.indexOf("old line 0003 alpha");
            CHECK(at >= 0 && at < 4096, label + ": the rewritten line is in the head region");
            rewritten.replace(at, 19, "old line 0003 omega");
            CHECK(rewritten.size() == current.size(), label + ": same length");
            if (grow)
                rewritten += record(3000, "ERROR", "grown after rewrite");
            CHECK(rewriteLater(path, rewritten), label + ": rewrite");

            tab.reloadChangedFiles(f5);
            CHECK(settled(tab), label + ": reload settles");
            CHECK(testdocs::idleViews(), "views settle");

            const int row3 = rowOfPathLine(model, path, 3);
            CHECK(row3 >= 0 && model.messageAt(row3).endsWith("old line 0003 omega"),
                  label + ": the new text of the rewritten line is shown: "
                      + (row3 >= 0 ? model.messageAt(row3) : QStringLiteral("<no row>")));
            const int row200 = rowOfPathLine(model, path, 200);
            CHECK(row200 >= 0 && model.messageAt(row200).endsWith("old line 0200 steady"),
                  label + ": line numbers follow the file");
            CHECK(model.rowCount() == rowsBefore + (grow ? 1 : 0),
                  QStringLiteral("%1: %2 rows, expected %3 (no stale or duplicated lines)")
                      .arg(label).arg(model.rowCount()).arg(rowsBefore + (grow ? 1 : 0)));
            if (grow) {
                const int last = rowOfPathLine(model, path, (scenario == HeadScenario::CrossedThreshold ? 30000 : 300) + 1);
                CHECK(last >= 0 && model.messageAt(last).endsWith("grown after rewrite"),
                      label + ": the grown tail is read once, after the new head");
            }
            if (!other.isEmpty())
                CHECK(rowOfPathLine(model, other, 50) >= 0, label + ": the other file of the merged tab stays");
            CHECK(tab.bookmarks().status(markRewritten) == BookmarkStore::Status::Missing,
                  QStringLiteral("%1: bookmark on the rewritten line — not found (%2)")
                      .arg(label).arg(int(tab.bookmarks().status(markRewritten))));
            CHECK(tab.bookmarks().status(markSteady) == BookmarkStore::Status::Visible
                      && tab.bookmarks().idAtRow(row200) == markSteady,
                  QStringLiteral("%1: bookmark on a surviving line — in place (%2)")
                      .arg(label).arg(int(tab.bookmarks().status(markSteady))));

            // Дальше файл снова дописывается как обычно — якорь новый.
            CHECK(testdocs::appendToFile(path, record(3001, "INFO", "after the reload")),
                  label + ": append after reload");
            tab.reloadChangedFiles(f5);
            CHECK(settled(tab) && model.rowCount() == rowsBefore + (grow ? 2 : 1)
                      && model.messageAt(model.rowCount() - 1).endsWith("after the reload"),
                  label + ": appends follow the reloaded file");
            settings.setIndexedThresholdMB(savedThreshold);
        }
    }
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
    const QString schema = testdocs::buildSchema();
    AppSettings& settings = AppSettings::instance();

    // Резидентная вкладка (порог по умолчанию — сотни МБ) и индексная (порог 0).
    checkAppendInParts(dir, QStringLiteral("resident"), schema);
    checkHeadRewrite(dir, QStringLiteral("resident"), schema, HeadScenario::Single);
    checkHeadRewrite(dir, QStringLiteral("resident merged"), schema, HeadScenario::Merged);
    checkHeadRewrite(dir, QStringLiteral("crossed threshold"), schema, HeadScenario::CrossedThreshold);
    checkLineNumbers(dir, QStringLiteral("resident"), schema);
    checkJumps(dir, QStringLiteral("resident"), schema);
    checkNewLinesIndicator(dir, QStringLiteral("resident"), schema);
    checkWideTail(dir, QStringLiteral("utf16le"), schema, QStringConverter::Utf16LE,
                  QByteArray("\xFF\xFE", 2));
    checkWideTail(dir, QStringLiteral("utf16be"), schema, QStringConverter::Utf16BE,
                  QByteArray("\xFE\xFF", 2));
    checkWideTail(dir, QStringLiteral("utf32le"), schema, QStringConverter::Utf32LE,
                  QByteArray("\xFF\xFE\x00\x00", 4));
    checkGrowingSingleByte(dir, QStringLiteral("resident"), schema);
    checkResidentEncodingSwitch(dir, schema);
    checkProvisionalRevision(dir, QStringLiteral("resident"), schema);
    checkProvisionalNoExtraWork(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(1);
    checkWideAboveThreshold(dir, schema);
    checkThresholdKeepsEncoding(dir, schema);
    checkProvisionalThreshold(dir, schema);
    settings.setIndexedThresholdMB(0);
    checkProvisionalRevision(dir, QStringLiteral("indexed"), schema);
    checkProvisionalNoExtraWork(dir, QStringLiteral("indexed"), schema);
    checkGrowingSingleByte(dir, QStringLiteral("indexed"), schema);
    checkAppendInParts(dir, QStringLiteral("indexed"), schema);
    checkHeadRewrite(dir, QStringLiteral("indexed"), schema, HeadScenario::Single);
    checkHeadRewrite(dir, QStringLiteral("indexed merged"), schema, HeadScenario::Merged);
    checkLineNumbers(dir, QStringLiteral("indexed"), schema);
    checkJumps(dir, QStringLiteral("indexed"), schema);
    checkNewLinesIndicator(dir, QStringLiteral("indexed"), schema);
    checkEncodingSwitch(dir, schema);
    checkLocatorLive(dir, schema);
    settings.setIndexedThresholdMB(512);
    checkFileMemoryRotation(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkFileMemoryRotation(dir, QStringLiteral("indexed"), schema);
    settings.setIndexedThresholdMB(512);
    checkReopenClosedTab(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkReopenClosedTab(dir, QStringLiteral("indexed"), schema);

    // Растущий файл пересекает порог: вкладка становится индексной на лету.
    {
        settings.setIndexedThresholdMB(1);
        QByteArray start;
        for (int i = 0; i < 2000; ++i)
            start += record(i / 50, "INFO", "line " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "grows.log", start);
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.addLogFile(path);
        CHECK(settled(tab), "threshold: initial load");
        CHECK(!tab.model()->isIndexedBackend(), "threshold: small file starts resident");
        QByteArray more;
        for (int i = 2000; i < 30000; ++i)
            more += record(i / 50, "INFO", "line " + QByteArray::number(i));
        CHECK(testdocs::appendToFile(path, more), "threshold: append past the threshold");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold: conversion settles");
        CHECK(tab.model()->isIndexedBackend(), "threshold: tab converted to indexed");
        CHECK(tab.model()->rowCount() == 30000, "threshold: every line present after conversion");
        CHECK(tab.model()->messageAt(29999).endsWith("line 29999")
                  && tab.model()->messageAt(0).endsWith("line 0"),
              "threshold: first and last lines intact");
        CHECK(testdocs::appendToFile(path, record(500, "INFO", "tail after conversion")),
              "threshold: append after conversion");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold: tail after conversion settles");
        CHECK(tab.model()->rowCount() == 30001
                  && tab.model()->messageAt(30000).endsWith("tail after conversion"),
              "threshold: the indexed tab keeps following the tail");
    }

    // Вкладку закрыли посреди загрузки: воркеры останавливаются, падения нет.
    {
        settings.setIndexedThresholdMB(512);
        QByteArray big;
        big.reserve(300000 * 60);
        for (int i = 0; i < 300000; ++i)
            big += record(i / 1000, "INFO", "row " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "closing.log", big);
        for (const int threshold : {512, 0}) {
            settings.setIndexedThresholdMB(threshold);
            auto tab = std::make_unique<LogViewWidget>();
            tab->setParserPattern(schema);
            tab->addLogFile(path);
            testdocs::pump(20);
            const bool wasLoading = tab->isLoading();
            tab.reset();
            testdocs::pump(200);
            CHECK(true, "closing a loading tab does not crash");
            if (!wasLoading)
                std::fprintf(stdout, "note: load finished before closing (threshold %d)\n", threshold);
        }
    }

    std::fprintf(stdout, "LogViewWidget lifecycle: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
