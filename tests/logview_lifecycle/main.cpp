// ============================================================================
// logview_lifecycle — жизненный цикл вкладки (LogViewWidget) на живых файлах:
//   • дозапись многострочной записи по частям, в том числе строки, дописанной
//     в два приёма (без '\n' между ними), — на обоих бэкендах;
//   • файл переписан заново (ротация) — полная перезагрузка;
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
// ============================================================================

#include "appsettings.h"
#include "logviewwidget.h"
#include "testdocuments.h"

#include "LogListView.h"
#include "indexedlogstore.h"
#include "logindexer.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFocusEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <cstdio>
#include <functional>

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

static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); }, 60000)
        && testdocs::settle(*tab.model());
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
    testdocs::pump(300);
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
        testdocs::pump(50);
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
    testdocs::pump(50);

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
        testdocs::pump(50);
        QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(&view, &focus);
    };
    const auto showAll = [&] {
        model.setLogLevelFilter({});
        CHECK(settled(tab), name + ": start: filter reset settles");
        testdocs::pump(50);
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
    testdocs::pump(300);
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
        testdocs::pump(50);
    };
    const auto toTop = [&] {
        bar->setValue(0);
        testdocs::pump(20);
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
    testdocs::pump(20);
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": reaching the bottom clears the count");
    append(2);
    CHECK(view.newLinesBelow() == 0 && !shown() && view.isScrolledToBottom(),
          name + ": at the bottom the view sticks to the tail, nothing to count");

    // --- Follow Tail сбрасывает ----------------------------------------------
    toTop();
    append(4);
    CHECK(view.newLinesBelow() == 4, name + ": counting again after leaving the bottom");
    view.setFollowTail(true);
    testdocs::pump(20);
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
    testdocs::pump(50);
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
    testdocs::pump(50);
    CHECK(view.newLinesBelow() < left, name + ": the command pages through the rest");
    while (view.newLinesBelow() > 0 && view.firstNewLineRow() >= 0) {
        const int before = view.newLinesBelow();
        view.goToNewLines();
        testdocs::pump(20);
        if (view.newLinesBelow() >= before)
            break;
    }
    CHECK(view.newLinesBelow() == 0 && !shown(), name + ": paging through reaches the end");

    // --- Фильтр: считаются только видимые строки ---------------------------
    model.setLogLevelFilter({LogLevel::Error});
    CHECK(settled(tab), name + ": new lines: filter settles");
    testdocs::pump(50);
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
    testdocs::pump(50);
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
    testdocs::pump(100);
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
    testdocs::pump(50);
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
        testdocs::pump(20);
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
        testdocs::pump(20);
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
    testdocs::pump(200);
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
        testdocs::pump(100);
        const auto entry = model.entryAt(view.currentIndex().row());
        const qint64 lineNumber = entry ? entry->originalLineNumber() : -1;
        const int scroll = view.verticalScrollBar()->value();
        const QRect rect = view.visualRect(view.currentIndex());
        store->setFileEncoding(file, TextDecoder(encoding));
        CHECK(settled(tab), what + ": settles");
        testdocs::pump(100);
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
    testdocs::pump(200);
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    const LogFilePtr file = tab.loadedFiles().value(0);
    CHECK(!model.isIndexedBackend(), "resident switch: the tab is resident");

    const auto switchTo = [&](TextEncoding encoding, const QString& what) {
        view.goToRow(700);
        testdocs::pump(100);
        const auto entry = model.entryAt(view.currentIndex().row());
        const qint64 lineNumber = entry ? entry->originalLineNumber() : -1;
        const int scroll = view.verticalScrollBar()->value();
        const QRect rect = view.visualRect(view.currentIndex());
        tab.setFileEncoding(file, encoding);
        CHECK(settled(tab), what + ": settles");
        testdocs::pump(200);
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
        testdocs::pump(50);
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
    testdocs::pump(200);
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
    testdocs::pump(100);
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
        testdocs::pump(100);
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
        testdocs::pump(100);
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
    // пересмотрела бы автоматику, его не трогает; решение автоматики
    // хранится рядом (для «Определить автоматически» шага 4).
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
                  && enc.automatic().decoder.encoding() == TextEncoding::Windows1251,
              name + " user: the choice stands, the automatic decision is kept beside it");
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
    checkLineNumbers(dir, QStringLiteral("indexed"), schema);
    checkJumps(dir, QStringLiteral("indexed"), schema);
    checkNewLinesIndicator(dir, QStringLiteral("indexed"), schema);
    checkEncodingSwitch(dir, schema);

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
