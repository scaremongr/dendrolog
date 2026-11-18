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
//     начала файла; то же после перестройки модели посреди поиска.
// ============================================================================

#include "appsettings.h"
#include "logviewwidget.h"
#include "testdocuments.h"

#include "LogListView.h"

#include <QApplication>
#include <QFocusEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>
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
    checkJumps(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkAppendInParts(dir, QStringLiteral("indexed"), schema);
    checkJumps(dir, QStringLiteral("indexed"), schema);

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
