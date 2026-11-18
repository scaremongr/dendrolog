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
//   • UTF-16 (LE и BE, с BOM; резидентный путь): дозапись читается в кодировке
//     файла, строка, дописанная частями, — одна строка, даже когда порция
//     обрывается посреди двухбайтного символа или суррогатной пары; файл выше
//     порога индексного бэкенда не перечитывается целиком на каждый тик.
// ============================================================================

#include "appsettings.h"
#include "logviewwidget.h"
#include "testdocuments.h"

#include "LogListView.h"

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
    checkWideTail(dir, QStringLiteral("utf16le"), schema, QStringConverter::Utf16LE,
                  QByteArray("\xFF\xFE", 2));
    checkWideTail(dir, QStringLiteral("utf16be"), schema, QStringConverter::Utf16BE,
                  QByteArray("\xFE\xFF", 2));
    checkWideTail(dir, QStringLiteral("utf32le"), schema, QStringConverter::Utf32LE,
                  QByteArray("\xFF\xFE\x00\x00", 4));
    settings.setIndexedThresholdMB(1);
    checkWideAboveThreshold(dir, schema);
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
