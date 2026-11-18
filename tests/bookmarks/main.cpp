// ============================================================================
// bookmarks — закладки с заметками (R-2): BookmarkStore, роль BookmarkRole,
// переходы F2 / Shift+F2 и панель Bookmarks, на обоих бэкендах.
//
// Модель (документ, собранный как во вкладке, testdocuments.h):
//   • постановка, снятие, заметка; роль отвечает «да» ровно на строках
//     закладок — сверка с ключами строк перебором;
//   • все 33 конфигурации фильтров logstore_equivalence (filterconfigs.h):
//     роль на видимых строках, статус Visible / Hidden, переход к скрытой —
//     к ближайшей видимой, следующая / предыдущая — против перебора;
//   • дозапись хвоста; слитая вкладка из двух файлов; строка продолжения
//     многострочной записи (закладка — на физической строке).
// Вкладка (LogViewWidget на живых файлах, резидентная и индексная):
//   • Ctrl+F2 по выбранной строке, F2 / Shift+F2 с исходами (дальше, через
//     край, единственная, нет);
//   • полная перезагрузка того же содержимого (файл удалён и создан заново);
//   • переписанный файл: строка с тем же текстом — закладка жива, другая или
//     пропавшая — «строка не найдена», без отметки на чужой строке;
//   • рост файла за порог — вкладка становится индексной, закладки на месте;
//   • панель: карточки, статус «скрыта фильтром», переход щелчком, заметка,
//     удаление, Clear All.
// ============================================================================

#include "appsettings.h"
#include "bookmarkpanelwidget.h"
#include "bookmarkstore.h"
#include "filterconfigs.h"
#include "logviewwidget.h"
#include "testdocuments.h"

#include "LogListView.h"

#include <QApplication>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>

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

using LineId = QPair<QString, qint64>;

static LineId lineOf(const RecordKey& key)
{
    return { key.file ? key.file->filePath : QString(), key.line };
}

// ---------------------------------------------------------------------------
// Сверки
// ---------------------------------------------------------------------------

// Роль на каждой видимой строке против множества строк закладок; статусы
// закладок против того, видна ли их строка.
static void checkRoles(const LogModel& model, const QSet<LineId>& expected,
                       const QString& phase)
{
    const BookmarkStore& store = model.bookmarks();
    int shown = 0;
    for (int row = 0; row < model.rowCount(); ++row) {
        const bool want = expected.contains(lineOf(model.keyAt(row)));
        const bool got = model.data(model.index(row, 0), LogModel::BookmarkRole).toBool();
        CHECK(want == got, QStringLiteral("%1: роль строки %2 — %3, ожидалось %4")
                               .arg(phase).arg(row).arg(got).arg(want));
        shown += got ? 1 : 0;
    }
    int visible = 0;
    for (const Bookmark& b : store.bookmarks()) {
        if (!expected.contains({ b.filePath, b.line }))
            continue; // потерянные (переписанный файл) проверяются отдельно
        const int row = model.rowForKey(b.key);
        const BookmarkStore::Status st = store.status(b.id);
        if (row >= 0) {
            ++visible;
            CHECK(st == BookmarkStore::Status::Visible,
                  QStringLiteral("%1: строка %2 видна, статус %3").arg(phase).arg(b.line).arg(int(st)));
            CHECK(store.visibleRow(b.id) == row && store.targetRow(b.id) == row,
                  QStringLiteral("%1: строка закладки %2").arg(phase).arg(b.line));
        } else {
            CHECK(st == BookmarkStore::Status::Hidden,
                  QStringLiteral("%1: строка %2 скрыта, статус %3").arg(phase).arg(b.line).arg(int(st)));
            // Переход к скрытой — к ближайшей видимой (как у nearestRowForKey).
            CHECK(store.targetRow(b.id) == model.nearestRowForKey(b.key),
                  QStringLiteral("%1: переход к скрытой %2").arg(phase).arg(b.line));
            CHECK(model.rowCount() == 0 || store.targetRow(b.id) >= 0,
                  QStringLiteral("%1: скрытой %2 некуда вести").arg(phase).arg(b.line));
        }
    }
    CHECK(visible == shown, QStringLiteral("%1: отметок %2, видимых закладок %3")
                                .arg(phase).arg(shown).arg(visible));
}

// Следующая / предыдущая закладка — независимый перебор видимых строк.
static int bruteNext(const LogModel& model, int start, bool forward)
{
    QVector<int> rows;
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.data(model.index(row, 0), LogModel::BookmarkRole).toBool())
            rows << row;
    if (rows.isEmpty())
        return -1;
    if (start < 0)
        return forward ? rows.first() : rows.last();
    if (forward) {
        for (const int r : rows)
            if (r > start)
                return r;
        return rows.first();
    }
    for (int i = rows.size() - 1; i >= 0; --i)
        if (rows[i] < start)
            return rows[i];
    return rows.last();
}

static void checkNavigation(const LogModel& model, const QString& phase)
{
    const BookmarkStore& store = model.bookmarks();
    QVector<int> starts = { -1, 0, model.rowCount() / 2, model.rowCount() - 1 };
    starts += store.visibleRows();
    for (const int start : starts) {
        if (start >= model.rowCount())
            continue;
        for (const bool forward : { true, false }) {
            const int want = bruteNext(model, start, forward);
            const int got = store.nextRow(start, forward);
            CHECK(want == got, QStringLiteral("%1: %2 от %3 — %4, ожидалось %5")
                                   .arg(phase, forward ? "вперёд" : "назад")
                                   .arg(start).arg(got).arg(want));
        }
    }
}

// Строки для закладок: равномерно по документу плюс первая, последняя и
// строка продолжения многострочной записи.
static QVector<int> pickRows(const LogModel& model)
{
    QVector<int> rows;
    const int n = model.rowCount();
    for (int i = 0; i < 14 && n > 0; ++i)
        rows << int((qint64(i) * n) / 14);
    if (n > 0)
        rows << n - 1;
    for (int row = n / 3; row < n; ++row) {
        if (model.messageAt(row).startsWith(QStringLiteral("    at "))) {
            rows << row;
            break;
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

// ---------------------------------------------------------------------------
// Модель: постановка, снятие, 33 конфигурации, дозапись
// ---------------------------------------------------------------------------

static void runModelScenario(const QString& name, Document doc, const QString& schema,
                             const QStringList& fieldNames, bool resident)
{
    g_context = name;
    for (const QString& e : doc.errors)
        CHECK(false, e);
    LogModel& model = *doc.model;
    model.setAvailableFields(fieldNames);
    BookmarkStore& store = model.bookmarks();
    CHECK(model.rowCount() > 100, QStringLiteral("документ загружен"));

    // ---- Постановка, заметка, снятие ------------------------------------------
    QSet<LineId> expected;
    const QVector<int> rows = pickRows(model);
    for (int i = 0; i < rows.size(); ++i) {
        const int id = store.add(rows[i], QStringLiteral("note %1").arg(i));
        CHECK(id >= 0, QStringLiteral("закладка на строке %1").arg(rows[i]));
        expected.insert(lineOf(model.keyAt(rows[i])));
        const Bookmark* b = store.find(id);
        CHECK(b && b->note == QStringLiteral("note %1").arg(i), QStringLiteral("заметка"));
        CHECK(b && b->preview == model.messageAt(rows[i]).left(BookmarkStore::kPreviewLength),
              QStringLiteral("фрагмент строки"));
        CHECK(store.add(rows[i]) == id, QStringLiteral("повторная постановка — та же закладка"));
    }
    CHECK(store.count() == rows.size(), QStringLiteral("число закладок"));
    checkRoles(model, expected, QStringLiteral("после постановки"));

    {
        const int row = rows[3];
        const int id = store.idAtRow(row);
        CHECK(store.setNote(id, QStringLiteral("правка")) && store.find(id)->note == QStringLiteral("правка"),
              QStringLiteral("правка заметки"));
        CHECK(!store.toggle(row), QStringLiteral("toggle снимает"));
        CHECK(!model.data(model.index(row, 0), LogModel::BookmarkRole).toBool(),
              QStringLiteral("снятая — без отметки"));
        CHECK(store.count() == rows.size() - 1, QStringLiteral("снятая ушла из списка"));
        CHECK(store.toggle(row), QStringLiteral("toggle ставит снова"));
        CHECK(store.find(store.idAtRow(row))->note.isEmpty(), QStringLiteral("новая — без заметки"));
    }
    checkNavigation(model, QStringLiteral("без фильтров"));

    // Закладка на строке продолжения — на ней самой, а не на начале записи.
    for (const int row : rows) {
        if (!model.messageAt(row).startsWith(QStringLiteral("    at ")))
            continue;
        CHECK(store.nextRow(row - 1, true) == row,
              QStringLiteral("переход к строке продолжения многострочной записи"));
    }

    // ---- 33 конфигурации фильтров ---------------------------------------------
    const QPair<QDateTime, QDateTime> range = model.fullTimeRange();
    const QVector<FilterConfig> configs = allConfigs(range.first, range.second);
    CHECK(configs.size() == 33, QStringLiteral("конфигураций %1").arg(configs.size()));
    for (const FilterConfig& cfg : configs) {
        g_context = name + QStringLiteral(" / ") + cfg.name;
        CHECK(applyConfig(model, cfg, fieldNames), QStringLiteral("фильтр не закончился"));
        pump(5); // отложенная сверка закладок — после сигналов модели
        checkRoles(model, expected, cfg.name);
        checkNavigation(model, cfg.name);
    }
    g_context = name + QStringLiteral(" / после фильтров");
    CHECK(applyConfig(model, FilterConfig(), fieldNames), QStringLiteral("сброс фильтров"));
    checkRoles(model, expected, QStringLiteral("фильтры сняты"));

    // ---- Дозапись хвоста ------------------------------------------------------
    g_context = name + QStringLiteral(" / дозапись");
    const QByteArray more = makeLogBytes(120, 77u, "\n", fixtureBaseTime().addDays(2),
                                         false, true, true);
    const int before = model.rowCount();
    const bool appended = resident ? appendResidentTail(doc, 0, more, schema)
                                   : appendIndexedTail(doc, 0, more, schema);
    CHECK(appended && settle(model), QStringLiteral("дозапись"));
    CHECK(model.rowCount() > before, QStringLiteral("строки дописаны"));
    checkRoles(model, expected, QStringLiteral("после дозаписи"));
    const int tailRow = model.rowCount() - 1;
    CHECK(store.add(tailRow) >= 0, QStringLiteral("закладка на дописанной строке"));
    expected.insert(lineOf(model.keyAt(tailRow)));
    checkRoles(model, expected, QStringLiteral("закладка на хвосте"));
    checkNavigation(model, QStringLiteral("после дозаписи"));

    // ---- Снять все --------------------------------------------------------------
    store.clear();
    CHECK(store.isEmpty(), QStringLiteral("clear"));
    checkRoles(model, {}, QStringLiteral("после clear"));
    CHECK(store.nextRow(-1, true) < 0, QStringLiteral("без закладок переходить некуда"));
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

static int rowOfLine(const LogModel& model, const QString& path, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (lineOf(model.keyAt(row)) == LineId(path, line))
            return row;
    return -1;
}

static QList<BookmarkCard*> cardsOf(const BookmarkPanelWidget& panel)
{
    QList<BookmarkCard*> cards;
    for (BookmarkCard* c : panel.findChildren<BookmarkCard*>())
        if (!c->isHidden())
            cards << c;
    return cards;
}

static BookmarkCard* cardFor(const BookmarkPanelWidget& panel, int id)
{
    for (BookmarkCard* c : cardsOf(panel))
        if (c->bookmarkId() == id)
            return c;
    return nullptr;
}

static QString statusText(const BookmarkPanelWidget& panel, int id)
{
    BookmarkCard* card = cardFor(panel, id);
    auto* label = card ? card->findChild<QLabel*>(QStringLiteral("bookmarkStatus")) : nullptr;
    return label ? label->text() : QStringLiteral("<нет карточки>");
}

static bool waitStatus(const BookmarkPanelWidget& panel, int id, const QString& text)
{
    return waitFor([&] { return statusText(panel, id) == text; }, 5000);
}

static void selectRow(LogViewWidget& tab, int row)
{
    tab.view()->goToRow(row);
}

static void checkTab(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / вкладка");
    QByteArray bytes;
    for (int i = 0; i < 60; ++i) {
        bytes += record(i, i % 7 == 0 ? "ERROR" : "INFO", "line " + QByteArray::number(i));
        if (i % 10 == 5)
            bytes += "    at continuation of " + QByteArray::number(i) + '\n';
    }
    const QString path = writeFile(dir, name + "-tab.log", bytes);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.view()->resize(800, 300);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    BookmarkStore& store = tab.bookmarks();

    // ---- Ctrl+F2 по выбранной строке; F2 / Shift+F2 -----------------------------
    CHECK(!tab.toggleBookmarkAtSelection(), QStringLiteral("без выбранной строки Ctrl+F2 ничего не делает"));
    LogViewWidget::LevelJumpOutcome outcome = LogViewWidget::LevelJumpOutcome::Interrupted;
    QObject::connect(&tab, &LogViewWidget::bookmarkJumpFinished, &tab,
                     [&](bool, LogViewWidget::LevelJumpOutcome o) { outcome = o; });
    tab.goToBookmark(true);
    CHECK(outcome == LogViewWidget::LevelJumpOutcome::NotFound, QStringLiteral("закладок нет — NotFound"));

    // Строки файла: 3 — запись «line 2», 7 — продолжение записи «line 5»,
    // 41 — запись «line 36», 66 — последняя.
    const QVector<qint64> lines = { 3, 7, 41, 66 };
    for (const qint64 line : lines) {
        const int row = rowOfLine(model, path, line);
        CHECK(row >= 0, QStringLiteral("строка %1 есть").arg(line));
        selectRow(tab, row);
        CHECK(tab.toggleBookmarkAtSelection(), QStringLiteral("Ctrl+F2 на строке %1").arg(line));
    }
    CHECK(store.count() == lines.size(), QStringLiteral("четыре закладки"));
    CHECK(model.messageAt(rowOfLine(model, path, 7)).startsWith("    at"),
          QStringLiteral("закладка на строке продолжения"));

    selectRow(tab, rowOfLine(model, path, 20));
    tab.goToBookmark(true);
    CHECK(outcome == LogViewWidget::LevelJumpOutcome::Moved
              && tab.view()->userSelectedRow() == rowOfLine(model, path, 41),
          QStringLiteral("F2 — к следующей, она выбрана"));
    tab.goToBookmark(false);
    CHECK(tab.view()->userSelectedRow() == rowOfLine(model, path, 7), QStringLiteral("Shift+F2 — к предыдущей"));
    selectRow(tab, rowOfLine(model, path, 66));
    tab.goToBookmark(true);
    CHECK(outcome == LogViewWidget::LevelJumpOutcome::Wrapped
              && tab.view()->userSelectedRow() == rowOfLine(model, path, 3),
          QStringLiteral("F2 с последней — через край к первой"));

    // ---- Панель ----------------------------------------------------------------
    BookmarkPanelWidget panel;
    panel.resize(360, 500);
    panel.setLogView(&tab);
    panel.show();
    pump(20);
    CHECK(cardsOf(panel).size() == lines.size(), QStringLiteral("карточки панели: %1").arg(cardsOf(panel).size()));

    const int id40 = store.idAtRow(rowOfLine(model, path, 41));
    BookmarkCard* card40 = cardFor(panel, id40);
    CHECK(card40 != nullptr, QStringLiteral("карточка строки 41"));
    if (card40) {
        auto* note = card40->findChild<QLineEdit*>(QStringLiteral("bookmarkNote"));
        CHECK(note, QStringLiteral("поле заметки"));
        if (note) {
            note->setText(QStringLiteral("первый таймаут"));
            emit note->editingFinished();
            CHECK(store.find(id40)->note == QStringLiteral("первый таймаут"), QStringLiteral("заметка из панели"));
        }
        auto* preview = card40->findChild<QLabel*>(QStringLiteral("bookmarkPreview"));
        CHECK(preview && preview->text().endsWith(QStringLiteral("line 36")), QStringLiteral("фрагмент в карточке"));
    }

    // Фильтр по времени скрывает строку 41 (запись 36): статус и переход.
    model.setTimeRangeFilter(QDateTime(QDate(2026, 3, 5), QTime(10, 0, 0)),
                             QDateTime(QDate(2026, 3, 5), QTime(10, 0, 30)));
    CHECK(settled(tab), QStringLiteral("фильтр"));
    CHECK(waitStatus(panel, id40, QStringLiteral("hidden by filter")),
          QStringLiteral("скрытая отмечена в панели: «%1»").arg(statusText(panel, id40)));
    CHECK(statusText(panel, store.idAtRow(rowOfLine(model, path, 3))).isEmpty(),
          QStringLiteral("видимая — без пометки"));
    if (BookmarkCard* c = cardFor(panel, id40)) {
        auto* go = c->findChild<QToolButton*>(QStringLiteral("bookmarkGo"));
        CHECK(go && go->isEnabled(), QStringLiteral("к скрытой можно перейти"));
        if (go)
            go->click();
        CHECK(tab.view()->userSelectedRow() == model.nearestRowForKey(store.find(id40)->key)
                  && tab.view()->userSelectedRow() >= 0,
              QStringLiteral("переход к скрытой — к ближайшей видимой"));
    }
    model.setTimeRangeFilter(QDateTime(), QDateTime());
    CHECK(settled(tab), QStringLiteral("фильтр снят"));
    CHECK(waitStatus(panel, id40, QString()), QStringLiteral("после снятия фильтра снова видна"));
    if (BookmarkCard* c = cardFor(panel, id40)) {
        if (auto* go = c->findChild<QToolButton*>(QStringLiteral("bookmarkGo")))
            go->click();
        CHECK(tab.view()->userSelectedRow() == rowOfLine(model, path, 41), QStringLiteral("щелчок — переход"));
    }

    // ---- Полная перезагрузка того же содержимого ------------------------------
    g_context = name + QStringLiteral(" / перезагрузка");
    QSet<LineId> expected;
    for (const qint64 line : lines)
        expected.insert({ path, line });
    CHECK(QFile::remove(path), QStringLiteral("файл удалён"));
    reload(tab);
    CHECK(model.rowCount() == 0, QStringLiteral("строки удалённого файла сброшены"));
    CHECK(waitStatus(panel, id40, QStringLiteral("line not loaded")),
          QStringLiteral("пока файла нет — «не загружена»: «%1»").arg(statusText(panel, id40)));
    CHECK(writeBytes(path, bytes), QStringLiteral("файл создан заново"));
    reload(tab);
    checkRoles(model, expected, QStringLiteral("после полной перезагрузки"));
    CHECK(store.find(id40) && store.find(id40)->note == QStringLiteral("первый таймаут"),
          QStringLiteral("заметка пережила перезагрузку"));
    CHECK(waitStatus(panel, id40, QString()), QStringLiteral("в панели снова видна"));

    // ---- Переписанный файл -----------------------------------------------------
    // Строки 1–3 те же, дальше другое содержимое и файл короче: строка 3 жива,
    // 7 и 41 теперь другие (ключ мог совпасть), 66 пропала.
    g_context = name + QStringLiteral(" / переписан");
    QByteArray rewritten;
    for (int i = 0; i < 3; ++i)
        rewritten += record(i, i % 7 == 0 ? "ERROR" : "INFO", "line " + QByteArray::number(i));
    for (int i = 3; i < 45; ++i)
        rewritten += record(i, "WARN", "other " + QByteArray::number(i));
    CHECK(writeBytes(path, rewritten), QStringLiteral("переписан"));
    reload(tab);
    const int id3 = store.idAtRow(rowOfLine(model, path, 3));
    CHECK(id3 >= 0, QStringLiteral("строка с прежним текстом — закладка жива"));
    for (const Bookmark& b : store.bookmarks()) {
        if (b.line == 3)
            continue;
        CHECK(store.status(b.id) == BookmarkStore::Status::Missing,
              QStringLiteral("строка %1 — «не найдена», статус %2").arg(b.line).arg(int(store.status(b.id))));
        CHECK(waitStatus(panel, b.id, QStringLiteral("line not found")),
              QStringLiteral("панель: строка %1 «%2»").arg(b.line).arg(statusText(panel, b.id)));
    }
    checkRoles(model, { LineId(path, 3) }, QStringLiteral("переписанный файл"));
    CHECK(cardsOf(panel).size() == lines.size(), QStringLiteral("потерянные закладки остаются в панели"));
    CHECK(store.find(id40)->preview.endsWith(QStringLiteral("line 36")), QStringLiteral("фрагмент сохранён"));

    // Поставить закладку заново на строку, где была потерянная: та же
    // закладка привязывается к новой строке и сохраняет заметку.
    const int row40 = rowOfLine(model, path, 41);
    CHECK(store.add(row40) == id40 && store.status(id40) == BookmarkStore::Status::Visible
              && store.find(id40)->note == QStringLiteral("первый таймаут"),
          QStringLiteral("повторная постановка привязывает потерянную"));

    // ---- Удаление и Clear All из панели ---------------------------------------
    if (BookmarkCard* c = cardFor(panel, id3)) {
        if (auto* rm = c->findChild<QToolButton*>(QStringLiteral("bookmarkRemove")))
            rm->click();
    }
    pump(5);
    CHECK(!store.find(id3) && cardsOf(panel).size() == lines.size() - 1, QStringLiteral("✕ снимает закладку"));
    if (auto* clearAll = panel.findChild<QToolButton*>(QStringLiteral("bookmarkClearAll")))
        clearAll->click();
    pump(5);
    CHECK(store.isEmpty() && cardsOf(panel).isEmpty(), QStringLiteral("Clear All"));
}

// Файл без меток переписан строками той же формы: ключи строк (файл, номер,
// запись, «нет метки») совпадают с прежними, отличает только текст. Закладка
// не должна прицепиться к чужой строке.
static void checkPlainRewrite(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / переписан без меток");
    QByteArray before;
    QByteArray after;
    for (int i = 1; i <= 30; ++i) {
        before += "alpha " + QByteArray::number(i) + '\n';
        after += (i == 4 ? QByteArray("alpha 4") : "beta " + QByteArray::number(i)) + '\n';
    }
    const QString path = writeFile(dir, name + "-plain.log", before);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    BookmarkStore& store = tab.bookmarks();
    const int id4 = store.add(rowOfLine(model, path, 4));
    const int id9 = store.add(rowOfLine(model, path, 9));
    const RecordKey oldKey9 = store.find(id9)->key;

    // Тот же размер, другой текст в середине: дописыванием это не назвать.
    CHECK(writeBytes(path, after.left(after.size() - 1) + "X\n"), QStringLiteral("переписан"));
    reload(tab);
    const int row9 = rowOfLine(model, path, 9);
    CHECK(row9 >= 0 && model.messageAt(row9) == QStringLiteral("beta 9"), QStringLiteral("новое содержимое"));
    CHECK(model.keyAt(row9) == oldKey9, QStringLiteral("ключ строки 9 совпал с прежним — сценарий тот"));
    CHECK(store.status(id9) == BookmarkStore::Status::Missing,
          QStringLiteral("строка 9 с другим текстом — «не найдена», статус %1").arg(int(store.status(id9))));
    CHECK(!model.data(model.index(row9, 0), LogModel::BookmarkRole).toBool(),
          QStringLiteral("на чужой строке отметки нет"));
    CHECK(store.status(id4) == BookmarkStore::Status::Visible
              && store.idAtRow(rowOfLine(model, path, 4)) == id4,
          QStringLiteral("строка 4 с прежним текстом — закладка жива"));
    CHECK(store.nextRow(-1, true) == rowOfLine(model, path, 4) && store.visibleRows().size() == 1,
          QStringLiteral("F2 ходит только к живой"));
}

// Смена кодировки (F-1, шаг 4): фрагмент закладки переписывается текстом,
// который показан после смены, — иначе сверка после следующей полной
// загрузки (тем же выбранным декодером) объявила бы строку «не найдена».
static void checkEncodingChange(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / смена кодировки");
    QString text;
    for (int i = 0; i < 40; ++i)
        text += QString::fromLatin1(record(i, i % 5 ? "INFO" : "ERROR", QByteArray()).chopped(1))
              + QStringLiteral("Запись номер %1: ошибка диска\n").arg(i);
    QByteArray bytes = encodeText(text, TextEncoding::Windows1251);
    const QString path = writeFile(dir, name + "-cp1251.log", bytes);
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setFallbackEncoding(TextEncoding::Windows1251);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    BookmarkStore& store = tab.bookmarks();
    const LogFilePtr file = tab.loadedFiles().value(0);
    const int id = store.add(rowOfLine(model, path, 10));
    CHECK(id >= 0 && store.find(id)->preview.contains(QStringLiteral("Запись номер 9")),
          QStringLiteral("фрагмент — текст Windows-1251"));

    tab.setFileEncoding(file, TextEncoding::Koi8R);
    CHECK(settled(tab), QStringLiteral("смена на KOI8-R"));
    const int row = rowOfLine(model, path, 10);
    CHECK(row >= 0 && store.find(id)->preview == model.messageAt(row).left(BookmarkStore::kPreviewLength)
              && !store.find(id)->preview.contains(QStringLiteral("Запись")),
          QStringLiteral("фрагмент — новый текст строки: ") + store.find(id)->preview);

    // Файл переписан (последний байт другой) — полная загрузка тем же
    // выбранным декодером: закладка сверяется по обновлённому фрагменту.
    bytes[bytes.size() - 2] = 'X';
    CHECK(writeBytes(path, bytes), QStringLiteral("переписан"));
    reload(tab);
    CHECK(tab.fileEncoding(file).source == FileEncoding::Source::User,
          QStringLiteral("выбор пережил перечитывание"));
    CHECK(waitFor([&] { return store.status(id) == BookmarkStore::Status::Visible; }, 5000),
          QStringLiteral("после перезагрузки закладка жива, статус %1").arg(int(store.status(id))));

    // Возврат к автоматике — тот же путь: фрагмент снова в Windows-1251.
    tab.restoreAutomaticEncoding(file);
    CHECK(settled(tab), QStringLiteral("«Определить автоматически»"));
    CHECK(store.find(id)->preview.contains(QStringLiteral("Запись номер 9")),
          QStringLiteral("фрагмент после «авто»: ") + store.find(id)->preview);
}

// Панель переключается между вкладками и переживает закрытие вкладки.
static void checkPanelLifecycle(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("панель: смена и закрытие вкладки");
    const QByteArray bytes = record(0, "INFO", "a") + record(1, "ERROR", "b") + record(2, "INFO", "c");
    auto tabA = std::make_unique<LogViewWidget>();
    LogViewWidget tabB;
    for (LogViewWidget* tab : { tabA.get(), &tabB }) {
        tab->setParserPattern(schema);
        tab->addLogFile(writeFile(dir, tab == &tabB ? "panel-b.log" : "panel-a.log", bytes));
        CHECK(settled(*tab), QStringLiteral("загрузка"));
    }
    // id в каждом хранилище начинаются с 1 — у двух вкладок закладки с id 1.
    const int idA = tabA->bookmarks().add(1);
    const int idB = tabB.bookmarks().add(1, QStringLiteral("заметка B"));
    CHECK(idA == idB, QStringLiteral("предусловие: одинаковые id"));

    BookmarkPanelWidget panel;
    panel.setLogView(tabA.get());
    panel.show();
    pump(20);
    BookmarkCard* card = cardFor(panel, idA);
    auto* note = card ? card->findChild<QLineEdit*>(QStringLiteral("bookmarkNote")) : nullptr;
    CHECK(note, QStringLiteral("поле заметки"));
    if (note) {
        note->setFocus();
        note->setText(QStringLiteral("набрано без Enter"));
    }
    panel.setLogView(&tabB); // Ctrl+Tab посреди набора
    pump(20);
    CHECK(tabA->bookmarks().find(idA)->note == QStringLiteral("набрано без Enter"),
          QStringLiteral("недописанная заметка сохранена в своей вкладке"));
    CHECK(tabB.bookmarks().find(idB)->note == QStringLiteral("заметка B"),
          QStringLiteral("заметка другой вкладки не перезаписана"));
    CHECK(cardsOf(panel).size() == 1, QStringLiteral("карточки новой вкладки"));

    panel.setLogView(tabA.get());
    pump(20);
    CHECK(cardsOf(panel).size() == 1, QStringLiteral("снова вкладка A"));
    tabA.reset(); // вкладку закрыли
    pump(20);
    CHECK(panel.logView() == nullptr && cardsOf(panel).isEmpty(),
          QStringLiteral("закрытая вкладка не оставляет карточек"));
}

// Рост файла за порог: вкладка конвертируется в индексную, закладки на месте.
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
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), QStringLiteral("загрузка"));
    LogModel& model = *tab.model();
    CHECK(!model.isIndexedBackend(), QStringLiteral("начинает резидентной"));
    QSet<LineId> expected;
    for (const int row : { 0, 7, 999, 1999 }) {
        CHECK(tab.toggleBookmark(row), QStringLiteral("закладка %1").arg(row));
        expected.insert(lineOf(model.keyAt(row)));
    }
    tab.bookmarks().setNote(tab.bookmarks().idAtRow(7), QStringLiteral("до конверсии"));

    QByteArray more;
    for (int i = 2000; i < 30000; ++i)
        more += record(i / 50, "INFO", "line " + QByteArray::number(i));
    CHECK(appendToFile(path, more), QStringLiteral("дозапись за порог"));
    reload(tab);
    CHECK(model.isIndexedBackend(), QStringLiteral("вкладка стала индексной"));
    checkRoles(model, expected, QStringLiteral("после конверсии"));
    const int id7 = tab.bookmarks().idAtRow(rowOfLine(model, path, 8));
    CHECK(id7 >= 0 && tab.bookmarks().find(id7)->note == QStringLiteral("до конверсии"),
          QStringLiteral("заметка пережила конверсию"));
    checkNavigation(model, QStringLiteral("после конверсии"));
    settings.setIndexedThresholdMB(512);
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
    const QStringList fieldNames = LogPattern(schema).fieldNames();
    const QDateTime base = fixtureBaseTime();

    const QString one = writeFile(dir, QStringLiteral("one.log"),
        makeLogBytes(1500, 31u, "\n", base, true, true, true));
    const QString two = writeFile(dir, QStringLiteral("two.log"),
        makeLogBytes(700, 32u, "\r\n", base.addSecs(200), false, true, false));

    runModelScenario(QStringLiteral("резидентный"), buildResident({ one }, schema), schema,
                     fieldNames, true);
    runModelScenario(QStringLiteral("индексный"), buildIndexed({ one }, schema), schema,
                     fieldNames, false);
    runModelScenario(QStringLiteral("резидентный, два файла"), buildResident({ one, two }, schema),
                     schema, fieldNames, true);
    runModelScenario(QStringLiteral("индексный, два файла"),
                     buildIndexed({ one, two }, schema, IndexedLoad::Concurrent), schema,
                     fieldNames, false);

    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(512);
    checkTab(dir, QStringLiteral("resident"), schema);
    checkPlainRewrite(dir, QStringLiteral("resident"), schema);
    checkEncodingChange(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkTab(dir, QStringLiteral("indexed"), schema);
    checkPlainRewrite(dir, QStringLiteral("indexed"), schema);
    checkEncodingChange(dir, QStringLiteral("indexed"), schema);
    settings.setIndexedThresholdMB(512);
    checkPanelLifecycle(dir, schema);
    checkConversion(dir, schema);

    std::fprintf(failures ? stderr : stdout, "bookmarks: %d проверок, %d нарушений\n",
                 checks, failures);
    return failures ? 1 : 0;
}
