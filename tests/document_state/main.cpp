// ============================================================================
// document_state — состояние вкладки вне вкладки (R-4, шаг 1;
// docs/design/R-5.md, «Модель», «Применение состояния», «Изменённый или
// пропавший источник»).
//
// Формат (src/documentstate.h):
//   • круговой проход JSON — полный снимок и пустой;
//   • эталонный файл версии 1 (tab-state-v1.json рядом с тестом) читается, и
//     снимок, собранный из известных значений, пишется ровно в него: формат
//     не может измениться незаметно. Корень путей в файле — @ROOT@ (корень
//     файловой системы платформы: абсолютный путь пишется на Windows и Linux
//     по-разному);
//   • отказ на чужом формате и версии, на битых полях и вложенных объектах;
//     лишние ключи не мешают.
// Вкладка (LogViewWidget на живых файлах, оба бэкенда): снять → закрыть →
// открыть новую → применить:
//   • закладки с заметками на своих строках, маркеры, фильтры (сразу и
//     «предложить»: модель без фильтров, щелчок ставит их), текущая строка
//     выбрана и на том же месте экрана (visualRect), авто-обновление,
//     follow-tail (вид у хвоста), выбор кодировки;
//   • файл дописан между снятием и применением — всё на месте;
//   • файл переписан — открыт, закладки «не найдены» с заметкой и
//     фрагментом, позиция не ставится;
//   • файл пропал — не открыт, закладки ждут его, отчёт называет файл;
//   • слитая вкладка, один из файлов пропал — второй с закладками и позицией;
//   • спул stdin в снимок не попадает;
//   • применение посреди загрузки; переход порога после применения.
// Память файла на диске (R-4, шаг 2; FileMemoryStore с временным каталогом):
//   • формат: круговой проход, эталон file-memory-v1.json, отказы (большая
//     версия отличается от битых данных), лишние ключи;
//   • хранилище: по пути (и дописанный), нормализация пути (., .., родные
//     разделители, регистр на Windows), большая версия не применяется и не
//     перезаписывается, битый файл, прерванная запись, переписанный файл
//     (с закладками — отсоединение, без — удаление; прежнее содержимое под
//     другим именем находит отсоединённую), перенос, копия, короткий (< 4 КБ)
//     и пустой файл, лимит и вытеснение (память с закладками остаётся), два
//     хранилища на один каталог (объединение закладок, снятая не воскресает);
//   • вкладка на обоих бэкендах: открыть → закладки, маркер, фильтр, позиция
//     → закрыть → открыть: всё на месте, фильтр предложен плашкой и
//     возвращается щелчком; отложенная запись после правки закладки; отказ от
//     предложения; второй файл вкладки — только закладки; выключенная
//     настройка; дописан; переписан (закладки серые, не на чужих строках);
//     Clear All спрашивает подтверждение.
// Сессия (R-5, шаг 1; SessionState, SessionFile, SessionController):
//   • формат: круговой проход, эталон session-v1.json (относительные пути к
//     файлу сессии), отказы — большая версия отличима от битых данных;
//     лишние ключи не мешают; файл: QSaveFile, битый, новой версии не тронут;
//   • перепривязка: на месте; каталог с сессией и логами перенесён — по
//     относительному пути; пропал; переписан; оба пути целы — абсолютный;
//   • схема: совпавшая по определению, отсутствующая — с пометкой;
//   • сессия из двух вкладок (одна слитая) открывается на другом пути на обоих
//     бэкендах: фильтры, маркеры, закладки с заметками, строка и её место на
//     экране, кодировка, активная вкладка; открытая раньше вкладка не тронута;
//   • пропавший и изменённый источник, отчёт, пересохранение, «указать файл»
//     (в контроллере и в окне отчёта); вкладка stdin не сохраняется.
// Ждём состояние (загрузка, ответ локатора, отложенная раскладка), а не время.
// ============================================================================

#include "appsettings.h"
#include "bookmarkpanelwidget.h"
#include "documentstate.h"
#include "filememorystore.h"
#include "sessioncontroller.h"
#include "sessionfile.h"
#include "sessionreportdialog.h"
#include "logviewwidget.h"
#include "stdinspooler.h"
#include "testdocuments.h"

#include "LogListView.h"

#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QFrame>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

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

// ---------------------------------------------------------------------------
// Формат
// ---------------------------------------------------------------------------

static QString root()
{
    QString r = QDir::rootPath(); // "/" или "C:/"
    if (r.endsWith(QLatin1Char('/')))
        r.chop(1);
    return r;
}

static QJsonObject bytesToObject(const QByteArray& bytes)
{
    return QJsonDocument::fromJson(bytes).object();
}

static QByteArray objectToBytes(const QJsonObject& json)
{
    return QJsonDocument(json).toJson(QJsonDocument::Indented);
}

// Снимок из известных значений — то, что лежит в эталоне версии 1.
static TabState goldenState()
{
    TabState s;
    SourceIdentity app;
    app.path = root() + QStringLiteral("/logs/app.log");
    app.size = 1048576;
    app.head = 0x9f0c11aa22bb33ccull;
    app.tail = 0x3ca1deadbeef0042ull;
    SourceIdentity db;
    db.path = root() + QStringLiteral("/logs/db.log");
    db.size = 2048;
    db.head = 0x0123456789abcdefull;
    db.tail = 0x0123456789abcdefull;
    s.sources.append({ app, std::nullopt });
    s.sources.append({ db, TextEncoding::Windows1251 });

    BookmarkState b;
    b.source = app;
    b.record.line = 185;
    b.record.text = 0xa1b2c3d4e5f60718ull;
    b.record.hasText = true;
    b.note = QStringLiteral("первый таймаут");
    b.preview = QStringLiteral("2026-07-16 09:20:31.554 [db-pool] ERROR timeout");
    b.created = QDateTime::fromString(QStringLiteral("2026-11-17T10:00:00.000Z"), Qt::ISODateWithMs);
    s.bookmarks.append(b);
    BookmarkState partial = b;
    partial.source = db;
    partial.record = RecordId();
    partial.record.line = 7;
    partial.note.clear();
    partial.preview = QStringLiteral("недописанная");
    s.bookmarks.append(partial);

    ViewState& v = s.view;
    v.levels = { LogLevel::Warn, LogLevel::Error, LogLevel::Fatal };
    v.timeFrom = QDateTime::fromString(QStringLiteral("2026-07-16T09:00:00.000Z"), Qt::ISODateWithMs);
    FilterRule rule;
    rule.action = FilterRule::Action::Exclude;
    rule.text = QStringLiteral("heartbeat");
    rule.highlightColor = QColor(QStringLiteral("#ff336699"));
    FilterRule second;
    second.connector = FilterRule::Connector::Or;
    second.text = QStringLiteral("pool-\\d+");
    second.isRegex = true;
    second.fieldName = QStringLiteral("Thread");
    v.textRules.rules = { rule, second };
    HighlightPattern marker;
    marker.text = QStringLiteral("timeout");
    marker.color = QColor(QStringLiteral("#80ffcc00"));
    v.markers = { marker };
    v.currentSource = app;
    v.current = b.record;
    v.viewportOffset = 120;
    v.currentSelected = true;
    v.followTail = false;
    v.autoReload = true;
    return s;
}

static QJsonObject refusedWith(const QJsonObject& json, const QString& what)
{
    QString error;
    const auto s = TabState::fromJson(json, &error);
    CHECK(!s.has_value() && !error.isEmpty(), QStringLiteral("отказ: ") + what);
    return json;
}

static void checkFormat()
{
    g_context = QStringLiteral("формат");
    const TabState golden = goldenState();

    // Круговой проход: JSON → байты → JSON → снимок → тот же JSON.
    const QJsonObject json = golden.toJson();
    QString error;
    const auto back = TabState::fromJson(bytesToObject(objectToBytes(json)), &error);
    CHECK(back.has_value(), QStringLiteral("читается: ") + error);
    if (back) {
        CHECK(back->toJson() == json, QStringLiteral("круговой проход — тот же JSON"));
        CHECK(back->sources.size() == 2 && back->sources[0].source == golden.sources[0].source
                  && !back->sources[0].userEncoding
                  && back->sources[1].userEncoding == TextEncoding::Windows1251,
              QStringLiteral("источники и выбор кодировки"));
        CHECK(back->bookmarks.size() == 2 && back->bookmarks[0].note == golden.bookmarks[0].note
                  && back->bookmarks[0].record == golden.bookmarks[0].record
                  && back->bookmarks[0].created == golden.bookmarks[0].created
                  && !back->bookmarks[1].record.hasText,
              QStringLiteral("закладки"));
        const ViewState& v = back->view;
        CHECK(v.levels == golden.view.levels, QStringLiteral("уровни"));
        CHECK(v.timeFrom == golden.view.timeFrom && !v.timeTo.isValid(), QStringLiteral("время"));
        CHECK(v.textRules.toJson() == golden.view.textRules.toJson(), QStringLiteral("правила"));
        CHECK(highlightPatternsToJson(v.markers) == highlightPatternsToJson(golden.view.markers),
              QStringLiteral("маркеры"));
        CHECK(v.hasCurrent() && *v.currentSource == *golden.view.currentSource
                  && v.current == golden.view.current && v.viewportOffset == 120
                  && v.currentSelected,
              QStringLiteral("текущая строка"));
        CHECK(!v.followTail && v.autoReload, QStringLiteral("follow-tail и авто-обновление"));
    }

    // Пустой снимок: ничего лишнего не пишется и читается обратно.
    const TabState empty;
    const QJsonObject emptyJson = empty.toJson();
    const QJsonObject emptyView = emptyJson.value("view").toObject();
    CHECK(!emptyView.contains("levels") && !emptyView.contains("timeFrom")
              && !emptyView.contains("textRules") && !emptyView.contains("markers")
              && !emptyView.contains("current"),
          QStringLiteral("пустое не пишется"));
    const auto emptyBack = TabState::fromJson(emptyJson, &error);
    CHECK(emptyBack && emptyBack->toJson() == emptyJson && !emptyBack->view.hasFilters()
              && !emptyBack->view.hasCurrent(),
          QStringLiteral("пустой снимок: ") + error);

    // Эталон версии 1.
    g_context = QStringLiteral("эталон версии 1");
    QFile file(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/tab-state-v1.json"));
    CHECK(file.open(QIODevice::ReadOnly), QStringLiteral("эталон открывается"));
    QByteArray bytes = file.readAll();
    bytes.replace("@ROOT@", root().toUtf8());
    const QJsonObject stored = bytesToObject(bytes);
    CHECK(!stored.isEmpty(), QStringLiteral("эталон — JSON-объект"));
    const auto parsed = TabState::fromJson(stored, &error);
    CHECK(parsed.has_value(), QStringLiteral("эталон читается: ") + error);
    if (parsed)
        CHECK(parsed->toJson() == stored, QStringLiteral("прочитанный эталон пишется им же"));
    const bool same = json == stored;
    CHECK(same, QStringLiteral("снимок из известных значений пишется ровно в эталон"));
    if (!same)
        std::fprintf(stderr, "--- сейчас пишется:\n%s\n", objectToBytes(json).constData());

    // Отказы.
    g_context = QStringLiteral("отказы");
    QJsonObject bad = json;
    bad.insert("format", "dendrolog-session");
    refusedWith(bad, QStringLiteral("чужой формат"));
    bad = json;
    bad.remove("format");
    refusedWith(bad, QStringLiteral("нет формата"));
    bad = json;
    bad.insert("version", 2);
    refusedWith(bad, QStringLiteral("версия 2"));
    bad = json;
    bad.insert("version", "1");
    refusedWith(bad, QStringLiteral("версия строкой"));
    bad = json;
    bad.insert("sources", QJsonObject());
    refusedWith(bad, QStringLiteral("источники не массив"));
    bad = json;
    bad.remove("view");
    refusedWith(bad, QStringLiteral("нет вида"));

    const auto withView = [&](const std::function<void(QJsonObject&)>& edit) {
        QJsonObject j = json;
        QJsonObject v = j.value("view").toObject();
        edit(v);
        j.insert("view", v);
        return j;
    };
    refusedWith(withView([](QJsonObject& v) { v.insert("version", 2); }), QStringLiteral("версия вида 2"));
    refusedWith(withView([](QJsonObject& v) { v.insert("levels", QJsonArray{ "error", "loud" }); }),
                QStringLiteral("неизвестный уровень"));
    refusedWith(withView([](QJsonObject& v) { v.insert("levels", "error"); }), QStringLiteral("уровни строкой"));
    refusedWith(withView([](QJsonObject& v) { v.insert("timeFrom", "вчера"); }), QStringLiteral("битое время"));
    refusedWith(withView([](QJsonObject& v) { v.insert("timeTo", 5); }), QStringLiteral("время числом"));
    refusedWith(withView([](QJsonObject& v) { v.insert("textRules", QJsonArray()); }),
                QStringLiteral("правила массивом"));
    refusedWith(withView([](QJsonObject& v) { v.insert("markers", QJsonObject()); }),
                QStringLiteral("маркеры объектом"));
    refusedWith(withView([](QJsonObject& v) { v.insert("followTail", "yes"); }),
                QStringLiteral("follow-tail строкой"));
    refusedWith(withView([](QJsonObject& v) {
                    QJsonObject c = v.value("current").toObject();
                    QJsonObject r = c.value("record").toObject();
                    r.insert("version", 9);
                    c.insert("record", r);
                    v.insert("current", c);
                }),
                QStringLiteral("строка текущей — чужая версия"));
    refusedWith(withView([](QJsonObject& v) {
                    QJsonObject c = v.value("current").toObject();
                    QJsonObject src = c.value("source").toObject();
                    src.insert("path", "logs/app.log");
                    c.insert("source", src);
                    v.insert("current", c);
                }),
                QStringLiteral("источник текущей — относительный путь"));
    refusedWith(withView([](QJsonObject& v) {
                    QJsonObject c = v.value("current").toObject();
                    c.insert("selected", 1);
                    v.insert("current", c);
                }),
                QStringLiteral("selected числом"));
    {
        QJsonObject j = json;
        QJsonArray sources = j.value("sources").toArray();
        QJsonObject s1 = sources.at(1).toObject();
        s1.insert("encoding", "EBCDIC");
        sources.replace(1, s1);
        j.insert("sources", sources);
        refusedWith(j, QStringLiteral("неизвестная кодировка"));
        s1.insert("source", QJsonObject()); // Ephemeral пишется пустым объектом
        s1.remove("encoding");
        sources.replace(1, s1);
        j.insert("sources", sources);
        refusedWith(j, QStringLiteral("пустой источник"));
    }
    {
        QJsonObject j = json;
        QJsonArray marks = j.value("bookmarks").toArray();
        QJsonObject m = marks.at(0).toObject();
        m.insert("version", 3);
        marks.replace(0, m);
        j.insert("bookmarks", marks);
        refusedWith(j, QStringLiteral("закладка чужой версии"));
    }
    // Лишние ключи — не отказ (их добавит следующая версия того же формата).
    QJsonObject extra = withView([](QJsonObject& v) { v.insert("schema", "future"); });
    extra.insert("comment", "из будущего");
    CHECK(TabState::fromJson(extra).has_value(), QStringLiteral("лишние ключи не мешают"));
}

// ---------------------------------------------------------------------------
// Вкладка
// ---------------------------------------------------------------------------

static QByteArray record(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

static QByteArray logBytes(int records, int secondBase = 0, const QByteArray& tag = "line")
{
    QByteArray bytes;
    for (int i = 0; i < records; ++i) {
        bytes += record(secondBase + i, i % 7 == 3 ? "ERROR" : (i % 5 == 1 ? "WARN" : "INFO"),
                        tag + ' ' + QByteArray::number(i));
        if (i % 10 == 5)
            bytes += "    at continuation of " + QByteArray::number(i) + '\n';
    }
    return bytes;
}

static bool settled(LogViewWidget& tab)
{
    const bool ok = waitFor([&] { return !tab.isLoading() && !tab.isPositionPending(); }, 60000)
        && settle(*tab.model());
    waitFor([&] { return !tab.view()->hasPendingLayout(); }, 5000);
    pump(30); // отложенная постановка позиции после reset (singleShot 0)
    return ok;
}

static bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

static int rowOfLine(const LogModel& model, const QString& path, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row) {
        const RecordKey key = model.keyAt(row);
        if (key.line == line && key.file && key.file->filePath == path)
            return row;
    }
    return -1;
}

static std::unique_ptr<LogViewWidget> makeTab(const QString& schema)
{
    auto tab = std::make_unique<LogViewWidget>();
    tab->setParserPattern(schema);
    tab->resize(900, 400);
    tab->show();
    return tab;
}

static int topOf(LogViewWidget& tab, int row)
{
    return tab.view()->visualRect(tab.model()->index(row, 0)).top();
}

static QString currentLine(LogViewWidget& tab)
{
    const int row = tab.view()->userSelectedRow();
    if (row < 0)
        return QStringLiteral("<нет выбора>");
    const RecordKey key = tab.model()->keyAt(row);
    return QStringLiteral("%1:%2").arg(key.file ? key.file->shortName() : QString()).arg(key.line);
}

// Снимок через JSON (как его запишет память файла и сессия).
static TabState throughJson(const TabState& state)
{
    QString error;
    const auto back = TabState::fromJson(bytesToObject(objectToBytes(state.toJson())), &error);
    CHECK(back.has_value(), QStringLiteral("снимок вкладки читается: ") + error);
    return back.value_or(TabState());
}

struct Saved {
    TabState state;
    QString path;
    qint64 currentLine = 0;
    int currentTop = 0;
    QHash<qint64, QString> notes; // строка → заметка
};

// Исходная вкладка: закладки, маркеры, фильтры, выбор кодировки, текущая
// строка не по центру, авто-обновление. Снимок — через JSON.
static Saved prepareSource(const QString& path, const QString& schema, bool userEncoding)
{
    Saved saved;
    saved.path = path;
    auto tab = makeTab(schema);
    tab->addLogFile(path);
    CHECK(settled(*tab), QStringLiteral("исходная: загрузка"));
    LogModel& model = *tab->model();
    for (const qint64 line : { qint64(2), qint64(40), qint64(120), qint64(200) }) {
        const int row = rowOfLine(model, path, line);
        CHECK(row >= 0, QStringLiteral("исходная: строка %1").arg(line));
        const QString note = QStringLiteral("заметка к %1").arg(line);
        tab->bookmarks().add(row, note);
        saved.notes.insert(line, note);
    }
    HighlightPattern marker;
    marker.text = QStringLiteral("line 1");
    marker.color = QColor(Qt::yellow);
    model.setRowMarkers({ marker });
    FilterRule rule;
    rule.action = FilterRule::Action::Exclude;
    rule.text = QStringLiteral("line 13");
    FilterRuleSet rules;
    rules.rules = { rule };
    rules.bindFields(tab->parserFieldNames(), false);
    model.setFilters({ LogLevel::Info, LogLevel::Warn, LogLevel::Unknown }, QDateTime(), QDateTime(), rules);
    if (userEncoding)
        tab->setFileEncoding(tab->loadedFiles().first(), TextEncoding::Windows1251);
    tab->setAutoReload(true);
    CHECK(settled(*tab), QStringLiteral("исходная: фильтр и кодировка"));

    // Строка 170 — запись «line 154» (INFO): видна при фильтрах снимка.
    const int row = rowOfLine(model, path, 170);
    CHECK(row >= 0, QStringLiteral("исходная: строка 170 видна"));
    tab->view()->goToRow(row);
    pump(20);
    QScrollBar* bar = tab->view()->verticalScrollBar();
    bar->setValue(bar->value() + bar->singleStep() * 3); // строка выше центра
    pump(20);
    saved.currentLine = 170;
    saved.currentTop = topOf(*tab, row);
    CHECK(tab->view()->userSelectedRow() == row, QStringLiteral("исходная: строка выбрана"));
    saved.state = throughJson(tab->captureState());
    CHECK(saved.state.sources.size() == 1 && saved.state.bookmarks.size() == 4
              && saved.state.view.hasCurrent() && saved.state.view.currentSelected
              && saved.state.view.current.line == 170,
          QStringLiteral("исходная: снимок полный"));
    CHECK(saved.state.sources.value(0).userEncoding.has_value() == userEncoding,
          QStringLiteral("исходная: выбор кодировки в снимке"));
    return saved; // вкладка закрыта
}

static void checkBookmarksOnLines(LogViewWidget& tab, const Saved& saved, const QString& what)
{
    const QVector<Bookmark> list = tab.bookmarks().bookmarks();
    CHECK(list.size() == saved.notes.size(), what + QStringLiteral(": закладок %1").arg(list.size()));
    for (const Bookmark& b : list) {
        CHECK(saved.notes.value(b.line) == b.note, what + QStringLiteral(": заметка строки %1").arg(b.line));
        const BookmarkStore::Status st = tab.bookmarks().status(b.id);
        CHECK(st == BookmarkStore::Status::Visible || st == BookmarkStore::Status::Hidden,
              what + QStringLiteral(": строка %1 найдена (%2)").arg(b.line).arg(int(st)));
        if (st == BookmarkStore::Status::Visible)
            CHECK(tab.bookmarks().idAtRow(rowOfLine(*tab.model(), saved.path, b.line)) == b.id,
                  what + QStringLiteral(": отметка на строке %1").arg(b.line));
    }
}

static void checkPositionRestored(LogViewWidget& tab, const Saved& saved, const QString& what)
{
    const int row = rowOfLine(*tab.model(), saved.path, saved.currentLine);
    CHECK(row >= 0 && tab.view()->userSelectedRow() == row,
          what + QStringLiteral(": выбрана строка %1, а не %2").arg(saved.currentLine).arg(currentLine(tab)));
    if (row >= 0)
        CHECK(topOf(tab, row) == saved.currentTop,
              what + QStringLiteral(": место строки на экране %1, было %2").arg(topOf(tab, row)).arg(saved.currentTop));
    CHECK(!tab.view()->followTail(), what + QStringLiteral(": follow-tail выключен"));
}

static void checkReopen(const QDir& dir, const QString& name, const QString& schema)
{
    const QString path = writeFile(dir, name + "-reopen.log", logBytes(400));

    // ---- Сразу: всё на месте ------------------------------------------------
    g_context = name + QStringLiteral(" / закрыть и открыть");
    {
        const Saved saved = prepareSource(path, schema, true);
        auto tab = makeTab(schema);
        const auto report = tab->applyState(saved.state, LogViewWidget::StateFilters::Apply);
        CHECK(report.missingSources.isEmpty() && report.changedSources.isEmpty(), QStringLiteral("отчёт пуст"));
        CHECK(tab->fileCount() == 1, QStringLiteral("файл открыт"));
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        LogModel& model = *tab->model();
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки"));
        CHECK(highlightPatternsToJson(model.rowMarkers()) == highlightPatternsToJson(saved.state.view.markers),
              QStringLiteral("маркеры"));
        CHECK(model.logLevelFilter() == saved.state.view.levels
                  && model.filterRules().toJson() == saved.state.view.textRules.toJson(),
              QStringLiteral("фильтры применены"));
        // Строка 15 — «line 13» (INFO, скрыта правилом), строка 4 — ERROR.
        CHECK(rowOfLine(model, path, 15) < 0 && rowOfLine(model, path, 4) < 0
                  && rowOfLine(model, path, 14) >= 0,
              QStringLiteral("фильтры действуют (line 13 и ERROR скрыты)"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция"));
        CHECK(tab->autoReload(), QStringLiteral("авто-обновление"));
        const FileEncoding enc = tab->fileEncoding(tab->loadedFiles().first());
        CHECK(enc.source == FileEncoding::Source::User
                  && enc.decoder.encoding() == TextEncoding::Windows1251,
              QStringLiteral("выбор кодировки вернулся"));
        CHECK(model.isIndexedBackend() == (name == QLatin1String("indexed")), QStringLiteral("бэкенд"));

        // Снова снять — тот же снимок (круг замкнут на вкладке).
        const TabState again = throughJson(tab->captureState());
        CHECK(again.toJson() == saved.state.toJson(), QStringLiteral("снимок восстановленной вкладки тот же"));

        // Фильтры «предложить»: модель без фильтров, позиция и закладки есть.
        g_context = name + QStringLiteral(" / фильтры предложены");
        auto offered = makeTab(schema);
        int offeredSignals = 0;
        QObject::connect(offered.get(), &LogViewWidget::offeredFiltersChanged,
                         [&](bool available) { offeredSignals += available ? 1 : 0; });
        offered->applyState(saved.state, LogViewWidget::StateFilters::Offer);
        CHECK(settled(*offered), QStringLiteral("загрузка"));
        LogModel& om = *offered->model();
        CHECK(om.logLevelFilter().isEmpty() && !om.filterRules().isActive(),
              QStringLiteral("фильтры не применены"));
        CHECK(offered->hasOfferedFilters() && offeredSignals == 1, QStringLiteral("предложены"));
        CHECK(highlightPatternsToJson(om.rowMarkers()) == highlightPatternsToJson(saved.state.view.markers),
              QStringLiteral("маркеры — сразу"));
        checkBookmarksOnLines(*offered, saved, QStringLiteral("закладки"));
        checkPositionRestored(*offered, saved, QStringLiteral("позиция"));
        CHECK(throughJson(offered->captureState()).view.textRules.toJson()
                  == saved.state.view.textRules.toJson(),
              QStringLiteral("непринятые фильтры снимаются, а не теряются"));
        const int allRows = om.rowCount();
        offered->applyOfferedFilters();
        CHECK(settled(*offered), QStringLiteral("фильтры приняты"));
        CHECK(!offered->hasOfferedFilters() && om.logLevelFilter() == saved.state.view.levels
                  && om.rowCount() < allRows && om.rowCount() == model.rowCount(),
              QStringLiteral("щелчок ставит фильтры: %1 строк, как у применённых сразу %2")
                  .arg(om.rowCount()).arg(model.rowCount()));
        CHECK(offered->view()->userSelectedRow() == rowOfLine(om, path, saved.currentLine),
              QStringLiteral("выбор пережил фильтр"));
    }

    // ---- Дописан между снятием и применением ---------------------------------
    g_context = name + QStringLiteral(" / дописан");
    {
        const Saved saved = prepareSource(path, schema, false);
        CHECK(appendToFile(path, logBytes(50, 3000, "appended")), QStringLiteral("дописан"));
        auto tab = makeTab(schema);
        const auto report = tab->applyState(saved.state);
        CHECK(report.changedSources.isEmpty(), QStringLiteral("дозапись — не переписан"));
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция"));
    }

    // ---- Переписан ------------------------------------------------------------
    g_context = name + QStringLiteral(" / переписан");
    {
        const QString rewrittenPath = writeFile(dir, name + "-rewritten.log", logBytes(300));
        const Saved saved = prepareSource(rewrittenPath, schema, false);
        CHECK(writeBytes(rewrittenPath, logBytes(300, 7, "other")), QStringLiteral("переписан"));
        auto tab = makeTab(schema);
        int restored = -1;
        QObject::connect(tab.get(), &LogViewWidget::positionRestoreFinished,
                         [&](bool applied) { restored = applied ? 1 : 0; });
        const auto report = tab->applyState(saved.state);
        CHECK(report.changedSources == QStringList{ rewrittenPath }, QStringLiteral("отчёт: переписан"));
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        CHECK(tab->model()->rowCount() > 0, QStringLiteral("вкладка работает"));
        CHECK(restored == 0 && tab->view()->userSelectedRow() < 0,
              QStringLiteral("позиция не ставится: %1").arg(restored));
        const QVector<Bookmark> list = tab->bookmarks().bookmarks();
        CHECK(list.size() == saved.notes.size(), QStringLiteral("закладки на месте"));
        for (const Bookmark& b : list) {
            CHECK(tab->bookmarks().status(b.id) == BookmarkStore::Status::Missing,
                  QStringLiteral("строка %1 — «не найдена»").arg(b.line));
            CHECK(b.note == saved.notes.value(b.line) && !b.preview.isEmpty(),
                  QStringLiteral("заметка и фрагмент строки %1").arg(b.line));
        }
        CHECK(throughJson(tab->captureState()).bookmarks.size() == saved.notes.size(),
              QStringLiteral("серые закладки снимаются снова"));
    }

    // ---- Пропал ---------------------------------------------------------------
    g_context = name + QStringLiteral(" / пропал");
    {
        const QString gonePath = writeFile(dir, name + "-gone.log", logBytes(300));
        const Saved saved = prepareSource(gonePath, schema, false);
        CHECK(QFile::remove(gonePath), QStringLiteral("удалён"));
        auto tab = makeTab(schema);
        const auto report = tab->applyState(saved.state);
        CHECK(report.missingSources == QStringList{ gonePath }, QStringLiteral("отчёт: пропал"));
        CHECK(tab->fileCount() == 0, QStringLiteral("не открыт"));
        CHECK(settled(*tab), QStringLiteral("вкладка спокойна"));
        CHECK(tab->bookmarks().count() == saved.notes.size(), QStringLiteral("закладки на месте"));
        for (const Bookmark& b : tab->bookmarks().bookmarks())
            CHECK(tab->bookmarks().status(b.id) == BookmarkStore::Status::Pending
                      && b.note == saved.notes.value(b.line),
                  QStringLiteral("строка %1 ждёт файл, заметка цела").arg(b.line));
        const TabState again = throughJson(tab->captureState());
        CHECK(again.bookmarks.size() == saved.notes.size() && again.sources.isEmpty(),
              QStringLiteral("снимок без файла хранит закладки"));
        // Файл вернули и открыли — закладки нашли строки.
        CHECK(writeBytes(gonePath, logBytes(300)), QStringLiteral("вернули"));
        tab->addLogFile(gonePath);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки вернувшегося файла"));
    }
}

static void checkFollowTail(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / follow-tail");
    const QString path = writeFile(dir, name + "-tail.log", logBytes(400));
    TabState state;
    {
        auto tab = makeTab(schema);
        tab->addLogFile(path);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        tab->view()->setFollowTail(true);
        state = throughJson(tab->captureState());
    }
    CHECK(state.view.followTail && !state.view.hasCurrent(), QStringLiteral("снимок: хвост, без строки"));
    CHECK(appendToFile(path, logBytes(30, 5000, "tail")), QStringLiteral("дописан"));
    auto tab = makeTab(schema);
    tab->applyState(state);
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    CHECK(tab->view()->followTail(), QStringLiteral("follow-tail включён"));
    const int last = tab->model()->rowCount() - 1;
    CHECK(last >= 0 && tab->view()->visualRect(tab->model()->index(last, 0)).bottom()
                           <= tab->view()->viewport()->height(),
          QStringLiteral("вид у хвоста"));
}

static void checkMerged(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / слитая, один пропал");
    const QString a = writeFile(dir, name + "-merged-a.log", logBytes(200));
    const QString b = writeFile(dir, name + "-merged-b.log", logBytes(200, 1, "bee"));
    TabState state;
    qint64 savedLine = 0;
    int currentTop = 0;
    {
        auto tab = makeTab(schema);
        tab->addLogFile(a);
        tab->addLogFile(b);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        LogModel& model = *tab->model();
        tab->bookmarks().add(rowOfLine(model, a, 10), QStringLiteral("a10"));
        tab->bookmarks().add(rowOfLine(model, b, 20), QStringLiteral("b20"));
        tab->bookmarks().add(rowOfLine(model, b, 90), QStringLiteral("b90"));
        const int row = rowOfLine(model, b, 100);
        tab->view()->goToRow(row);
        pump(20);
        savedLine = 100;
        currentTop = topOf(*tab, row);
        state = throughJson(tab->captureState());
    }
    CHECK(state.sources.size() == 2 && state.bookmarks.size() == 3, QStringLiteral("снимок"));
    CHECK(QFile::remove(a), QStringLiteral("a удалён"));
    auto tab = makeTab(schema);
    const auto report = tab->applyState(state);
    CHECK(report.missingSources == QStringList{ a }, QStringLiteral("отчёт называет a"));
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    LogModel& model = *tab->model();
    CHECK(tab->fileCount() == 1 && model.rowCount() > 0, QStringLiteral("b открыт"));
    for (const Bookmark& bm : tab->bookmarks().bookmarks()) {
        const BookmarkStore::Status st = tab->bookmarks().status(bm.id);
        if (bm.filePath == a)
            CHECK(st == BookmarkStore::Status::Pending && bm.note == QStringLiteral("a10"),
                  QStringLiteral("закладка a ждёт файл"));
        else
            CHECK(st == BookmarkStore::Status::Visible
                      && tab->bookmarks().idAtRow(rowOfLine(model, b, bm.line)) == bm.id,
                  QStringLiteral("закладка b%1 на своей строке").arg(bm.line));
    }
    const int row = rowOfLine(model, b, savedLine);
    CHECK(row >= 0 && tab->view()->userSelectedRow() == row, QStringLiteral("позиция в b: %1").arg(currentLine(*tab)));
    if (row >= 0)
        CHECK(topOf(*tab, row) == currentTop,
              QStringLiteral("место на экране %1, было %2").arg(topOf(*tab, row)).arg(currentTop));
}

static void checkStdin(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("спул stdin");
    const QString spool = StdinSpooler::spoolFilePathFor(QCoreApplication::applicationPid());
    CHECK(writeBytes(spool, logBytes(50, 0, "piped")), QStringLiteral("спул создан"));
    const QString path = writeFile(dir, "stdin-neighbour.log", logBytes(50, 2, "file"));
    auto tab = makeTab(schema);
    tab->addLogFile(spool);
    tab->addLogFile(path);
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    LogModel& model = *tab->model();
    const QString spoolPath = LogViewWidget::normalizedPath(spool);
    tab->bookmarks().add(rowOfLine(model, spoolPath, 5), QStringLiteral("в спуле"));
    tab->bookmarks().add(rowOfLine(model, path, 5), QStringLiteral("в файле"));
    tab->view()->goToRow(rowOfLine(model, spoolPath, 7));
    const TabState state = throughJson(tab->captureState());
    CHECK(state.sources.size() == 1 && state.sources[0].source.path == path,
          QStringLiteral("источник — только файл"));
    CHECK(state.bookmarks.size() == 1 && state.bookmarks[0].note == QStringLiteral("в файле"),
          QStringLiteral("закладка — только файла"));
    CHECK(!state.view.hasCurrent(), QStringLiteral("текущая строка в спуле не снимается"));
    tab.reset();
    QFile::remove(spool);
}

// Применение посреди загрузки и переход порога после применения.
static void checkMidLoadAndThreshold(const QDir& dir, const QString& schema)
{
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(512);
    const QString path = writeFile(dir, "midload.log", logBytes(20000));
    g_context = QStringLiteral("посреди загрузки");
    const Saved saved = prepareSource(path, schema, false);
    {
        auto tab = makeTab(schema);
        tab->addLogFile(path);
        const bool loading = tab->isLoading();
        tab->applyState(saved.state);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        if (!loading)
            std::fprintf(stdout, "  (файл прочитан до применения — поздний проход)\n");
        CHECK(tab->fileCount() == 1, QStringLiteral("файл не задвоен"));
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция"));
        CHECK(tab->model()->logLevelFilter() == saved.state.view.levels, QStringLiteral("фильтры"));
    }

    g_context = QStringLiteral("переход порога после применения");
    settings.setIndexedThresholdMB(2);
    {
        auto tab = makeTab(schema);
        tab->applyState(saved.state);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        CHECK(!tab->model()->isIndexedBackend(), QStringLiteral("сценарий: резидентная до порога"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция до порога"));
        CHECK(appendToFile(path, logBytes(30000, 40000, "grown")), QStringLiteral("дописан за порог"));
        tab->reloadChangedFiles(true);
        CHECK(settled(*tab), QStringLiteral("конверсия"));
        CHECK(tab->model()->isIndexedBackend(), QStringLiteral("сценарий: стала индексной"));
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки после порога"));
        const int row = rowOfLine(*tab->model(), path, saved.currentLine);
        CHECK(row >= 0 && tab->view()->userSelectedRow() == row,
              QStringLiteral("выбор пережил порог: %1").arg(currentLine(*tab)));
        if (row >= 0)
            CHECK(topOf(*tab, row) == saved.currentTop,
                  QStringLiteral("и место на экране: %1, было %2").arg(topOf(*tab, row)).arg(saved.currentTop));
        CHECK(tab->model()->logLevelFilter() == saved.state.view.levels
                  && rowOfLine(*tab->model(), path, 4) < 0,
              QStringLiteral("фильтр действует после порога"));
    }
    settings.setIndexedThresholdMB(512);
}

// ---------------------------------------------------------------------------
// Память файла на диске (R-4, шаг 2): FileMemoryStore с временным каталогом
// ---------------------------------------------------------------------------

static QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static QString memoryFileOf(const QString& memDir, const QString& path)
{
    return memDir + QLatin1Char('/') + FileMemoryStore::fileNameForPath(path);
}

static QStringList memoryFiles(const QString& memDir)
{
    return QDir(memDir).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
}

static BookmarkState fakeBookmark(const SourceIdentity& source, qint64 line)
{
    BookmarkState b;
    b.source = source;
    b.record = RecordId::forLine(line, QStringLiteral("text of %1").arg(line));
    b.note = QStringLiteral("note %1").arg(line);
    b.preview = QStringLiteral("text of %1").arg(line);
    b.created = QDateTime::fromString(QStringLiteral("2026-11-19T10:00:00.000Z"), Qt::ISODateWithMs);
    return b;
}

static FileMemory memoryOf(const QString& path, const QVector<qint64>& lines, bool withView = true)
{
    FileMemory m;
    const auto source = SourceIdentity::capture(path);
    CHECK(source.has_value(), QStringLiteral("идентичность снимается: ") + path);
    m.source.source = source.value_or(SourceIdentity());
    for (const qint64 line : lines)
        m.bookmarks.append(fakeBookmark(m.source.source, line));
    if (withView) {
        ViewState v;
        HighlightPattern marker;
        marker.text = QStringLiteral("line 1");
        marker.color = QColor(Qt::yellow);
        v.markers = { marker };
        v.currentSource = m.source.source;
        v.current = RecordId::forLine(3, QStringLiteral("text of 3"));
        v.currentSelected = true;
        m.view = v;
    }
    return m;
}

static QVector<qint64> linesOf(const std::optional<FileMemory>& m)
{
    QVector<qint64> lines;
    if (m)
        for (const BookmarkState& b : m->bookmarks)
            lines.append(b.record.line);
    std::sort(lines.begin(), lines.end());
    return lines;
}

static bool pathsAre(const FileMemory& m, const QString& path)
{
    bool ok = m.source.source.path == path;
    for (const BookmarkState& b : m.bookmarks)
        ok = ok && b.source.path == path;
    if (m.view && m.view->currentSource)
        ok = ok && m.view->currentSource->path == path;
    return ok;
}

static FileMemory goldenMemory()
{
    FileMemory m;
    const TabState tab = goldenState();
    m.source = tab.sources[0]; // app.log
    m.source.userEncoding = TextEncoding::Windows1251;
    for (const BookmarkState& b : tab.bookmarks)
        if (b.source == m.source.source)
            m.bookmarks.append(b);
    m.view = tab.view;
    m.lastUsed = QDateTime::fromString(QStringLiteral("2026-11-19T10:00:00.000Z"), Qt::ISODateWithMs);
    return m;
}

static void checkFileMemoryFormat()
{
    g_context = QStringLiteral("память файла: формат");
    const FileMemory golden = goldenMemory();
    const QJsonObject json = golden.toJson();
    QString error;
    const auto back = FileMemory::fromJson(bytesToObject(objectToBytes(json)), &error);
    CHECK(back.has_value(), QStringLiteral("читается: ") + error);
    if (back) {
        CHECK(back->toJson() == json, QStringLiteral("круговой проход — тот же JSON"));
        CHECK(back->source.userEncoding == TextEncoding::Windows1251 && back->bookmarks.size() == 1
                  && back->view && back->view->hasCurrent() && back->lastUsed == golden.lastUsed,
              QStringLiteral("поля на месте"));
    }

    QFile file(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/file-memory-v1.json"));
    CHECK(file.open(QIODevice::ReadOnly), QStringLiteral("эталон открывается"));
    QByteArray bytes = file.readAll();
    bytes.replace("@ROOT@", root().toUtf8());
    const QJsonObject stored = bytesToObject(bytes);
    const auto parsed = FileMemory::fromJson(stored, &error);
    CHECK(parsed.has_value(), QStringLiteral("эталон читается: ") + error);
    if (parsed)
        CHECK(parsed->toJson() == stored, QStringLiteral("прочитанный эталон пишется им же"));
    const bool same = json == stored;
    CHECK(same, QStringLiteral("память из известных значений пишется ровно в эталон"));
    if (!same)
        std::fprintf(stderr, "--- сейчас пишется:\n%s\n", objectToBytes(json).constData());

    // Без ViewState (файл был в слитой вкладке) — читается, вида нет.
    FileMemory noView = golden;
    noView.view.reset();
    const auto noViewBack = FileMemory::fromJson(noView.toJson());
    CHECK(noViewBack && !noViewBack->view && !noView.toJson().contains("view"),
          QStringLiteral("без вида"));

    const auto refused = [&](QJsonObject j, const QString& what, bool newer = false) {
        bool isNewer = !newer;
        QString why;
        CHECK(!FileMemory::fromJson(j, &why, &isNewer) && !why.isEmpty() && isNewer == newer,
              QStringLiteral("отказ: ") + what);
    };
    QJsonObject bad = json;
    bad.insert("version", 2);
    refused(bad, QStringLiteral("версия 2 — новее"), true);
    bad = json;
    bad.insert("format", "dendrolog-tab-state");
    refused(bad, QStringLiteral("чужой формат"));
    bad = json;
    bad.insert("version", 0);
    refused(bad, QStringLiteral("версия 0"));
    bad = json;
    bad.insert("lastUsed", "вчера");
    refused(bad, QStringLiteral("битое время"));
    bad = json;
    bad.insert("bookmarks", QJsonObject());
    refused(bad, QStringLiteral("закладки объектом"));
    bad = json;
    bad.insert("view", 5);
    refused(bad, QStringLiteral("вид числом"));
    bad = json;
    bad.insert("source", QJsonObject()); // Ephemeral (спул stdin) не пишется
    refused(bad, QStringLiteral("пустой источник"));
    QJsonObject extra = json;
    extra.insert("schema", "из будущей версии F-5");
    CHECK(FileMemory::fromJson(extra).has_value(), QStringLiteral("лишние ключи не мешают"));
}

static void checkFileMemoryStore(const QDir& dir)
{
    const QDir logs(dir.filePath(QStringLiteral("memory-logs")));
    QDir().mkpath(logs.path());

    // ---- По пути: запись, чтение, дозапись -----------------------------------
    g_context = QStringLiteral("память файла: по пути");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-path"));
        const QString a = writeFile(logs, "a.log", logBytes(60));
        FileMemoryStore store(memDir);
        CHECK(store.open(a).found == FileMemoryStore::Found::None, QStringLiteral("сначала пусто"));
        store.save(memoryOf(a, { 3, 7 }), {});
        store.waitForWrites();
        CHECK(memoryFiles(memDir) == QStringList{ FileMemoryStore::fileNameForPath(a) },
              QStringLiteral("один файл с именем-отпечатком пути"));
        FileMemoryStore other(memDir);
        const auto found = other.open(a);
        CHECK(found.found == FileMemoryStore::Found::ByPath && linesOf(found.memory) == (QVector<qint64>{ 3, 7 })
                  && found.memory->view && found.memory->lastUsed.isValid(),
              QStringLiteral("второе хранилище читает её по пути"));
        CHECK(appendToFile(a, logBytes(5, 900, "more")), QStringLiteral("дописан"));
        CHECK(other.open(a).found == FileMemoryStore::Found::ByPath, QStringLiteral("дописанный — та же память"));
        CHECK(store.clear() == 1 && memoryFiles(memDir).isEmpty(), QStringLiteral("clear удаляет"));
    }

    // ---- Нормализация пути ----------------------------------------------------
    g_context = QStringLiteral("память файла: нормализация пути");
    {
        const QString plain = logs.filePath(QStringLiteral("x/app.log"));
        const QString dotted = logs.path() + QStringLiteral("/x/./y/../app.log");
        CHECK(FileMemoryStore::fileNameForPath(plain) == FileMemoryStore::fileNameForPath(dotted),
              QStringLiteral(". и .. не меняют ключ"));
        CHECK(FileMemoryStore::fileNameForPath(plain)
                  == FileMemoryStore::fileNameForPath(QDir::toNativeSeparators(plain)),
              QStringLiteral("родные разделители не меняют ключ"));
        const QString upper = logs.filePath(QStringLiteral("x/APP.log"));
#ifdef Q_OS_WIN
        CHECK(FileMemoryStore::fileNameForPath(plain) == FileMemoryStore::fileNameForPath(upper),
              QStringLiteral("Windows: регистр не меняет ключ"));
#else
        CHECK(FileMemoryStore::fileNameForPath(plain) != FileMemoryStore::fileNameForPath(upper),
              QStringLiteral("Linux: регистр различается"));
#endif
        CHECK(FileMemoryStore::fileNameForPath(plain).size() == 16 + 5, QStringLiteral("16 hex + .json"));
    }

    // ---- Большая версия: не применяется и не перезаписывается -------------------
    g_context = QStringLiteral("память файла: большая версия");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-newer"));
        const QString b = writeFile(logs, "b.log", logBytes(60));
        QJsonObject future = memoryOf(b, { 1 }).toJson();
        future.insert("version", 2);
        future.insert("somethingNew", true);
        QDir().mkpath(memDir);
        const QByteArray bytes = objectToBytes(future);
        CHECK(writeBytes(memoryFileOf(memDir, b), bytes), QStringLiteral("файл версии 2"));
        FileMemoryStore store(memDir);
        CHECK(store.open(b).found == FileMemoryStore::Found::Refused, QStringLiteral("отказ"));
        store.save(memoryOf(b, { 5 }), {});
        store.waitForWrites();
        CHECK(readBytes(memoryFileOf(memDir, b)) == bytes, QStringLiteral("не перезаписан"));
        // И не вытесняется, и не служит кандидатом поиска по содержимому.
        const QString moved = logs.filePath(QStringLiteral("b-moved.log"));
        CHECK(QFile::copy(b, moved), QStringLiteral("копия"));
        CHECK(store.open(moved).found == FileMemoryStore::Found::None, QStringLiteral("не кандидат"));
        QFile::remove(moved);
    }

    // ---- Битый файл -------------------------------------------------------------
    g_context = QStringLiteral("память файла: битый файл");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-broken"));
        const QString c = writeFile(logs, "c.log", logBytes(60));
        QDir().mkpath(memDir);
        CHECK(writeBytes(memoryFileOf(memDir, c), "{ \"format\": \"dendrolog-file-memory\", "),
              QStringLiteral("битые байты"));
        FileMemoryStore store(memDir);
        CHECK(store.open(c).found == FileMemoryStore::Found::None, QStringLiteral("не читается — нет памяти"));
        store.save(memoryOf(c, { 2 }), {});
        store.waitForWrites();
        FileMemoryStore again(memDir);
        CHECK(linesOf(again.open(c).memory) == (QVector<qint64>{ 2 }), QStringLiteral("следующая запись его заменяет"));
    }

    // ---- Прерванная запись --------------------------------------------------------
    g_context = QStringLiteral("память файла: прерванная запись");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-abort"));
        const QString d = writeFile(logs, "d.log", logBytes(60));
        FileMemoryStore store(memDir);
        store.save(memoryOf(d, { 4 }), {});
        store.waitForWrites();
        const QString target = memoryFileOf(memDir, d);
        const QByteArray before = readBytes(target);
        const QByteArray next = objectToBytes(memoryOf(d, { 4, 9, 11 }).toJson());
        CHECK(!FileMemoryStore::writeFileAtomically(target, next, next.size() / 2),
              QStringLiteral("оборванная запись — неуспех"));
        CHECK(readBytes(target) == before, QStringLiteral("прежний файл цел"));
        CHECK(QDir(memDir).entryList(QDir::Files).size() == 1, QStringLiteral("временных файлов не осталось"));
        CHECK(linesOf(FileMemoryStore(memDir).open(d).memory) == (QVector<qint64>{ 4 }),
              QStringLiteral("память — прежняя"));
    }

    // ---- Источник Changed: с закладками — отсоединить, без — удалить --------------
    g_context = QStringLiteral("память файла: переписан");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-changed"));
        const QString r = writeFile(logs, "r.log", logBytes(60, 0, "first"));
        const QByteArray oldBytes = readBytes(r);
        const QString s = writeFile(logs, "s.log", logBytes(60, 0, "plain"));
        FileMemoryStore store(memDir);
        store.save(memoryOf(r, { 5 }), {});
        store.save(memoryOf(s, {}), {});
        store.waitForWrites();
        CHECK(writeBytes(r, logBytes(60, 3, "second")) && writeBytes(s, logBytes(60, 3, "other")),
              QStringLiteral("переписаны"));
        const auto stale = store.open(r);
        CHECK(stale.found == FileMemoryStore::Found::Stale && linesOf(stale.memory) == (QVector<qint64>{ 5 }),
              QStringLiteral("с закладками: Stale, память прежнего содержимого"));
        CHECK(!QFileInfo::exists(memoryFileOf(memDir, r)), QStringLiteral("по пути её больше нет"));
        const QStringList detached = memoryFiles(memDir).filter(QStringLiteral("~"));
        CHECK(detached.size() == 1, QStringLiteral("отсоединённая лежит в каталоге"));
        CHECK(store.open(s).found == FileMemoryStore::Found::Stale, QStringLiteral("без закладок: Stale"));
        CHECK(!QFileInfo::exists(memoryFileOf(memDir, s)) && memoryFiles(memDir).size() == 1,
              QStringLiteral("без закладок — удалена"));
        CHECK(store.open(r).found == FileMemoryStore::Found::None, QStringLiteral("новое содержимое — чистое"));
        // Прежнее содержимое появилось под другим именем — память находит его.
        const QString old = writeFile(logs, "r.log.1", oldBytes);
        const auto found = store.open(old);
        CHECK(found.found == FileMemoryStore::Found::ByContent && !found.copied
                  && linesOf(found.memory) == (QVector<qint64>{ 5 }) && pathsAre(*found.memory, old)
                  && found.previousPath == r,
              QStringLiteral("отсоединённая переехала к r.log.1"));
        store.waitForWrites();
        CHECK(memoryFiles(memDir) == QStringList{ FileMemoryStore::fileNameForPath(old) },
              QStringLiteral("отсоединённая убрана, память — по новому пути"));
    }

    // ---- По содержимому: перенос, копия, короткий, пустой ---------------------------
    g_context = QStringLiteral("память файла: по содержимому");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-content"));
        FileMemoryStore store(memDir);
        const QString m = writeFile(logs, "m.log", logBytes(80, 0, "moving"));
        const QString c = writeFile(logs, "copied.log", logBytes(80, 0, "copying"));
        const QString shortFile = writeFile(logs, "short.log", logBytes(4, 0, "tiny")); // < 4 КБ
        const QString empty = writeFile(logs, "empty.log", QByteArray());
        store.save(memoryOf(m, { 2, 4 }), {});
        store.save(memoryOf(c, { 1 }), {});
        store.save(memoryOf(shortFile, { 1 }), {});
        store.save(memoryOf(empty, {}), {});
        store.waitForWrites();
        CHECK(QFileInfo(shortFile).size() < 4096, QStringLiteral("сценарий: короткий файл"));

        QDir().mkpath(logs.filePath(QStringLiteral("elsewhere")));
        const QString movedTo = logs.filePath(QStringLiteral("elsewhere/m.log"));
        CHECK(QFile::rename(m, movedTo), QStringLiteral("перенесён в другую папку"));
        const auto moved = store.open(movedTo);
        CHECK(moved.found == FileMemoryStore::Found::ByContent && !moved.copied
                  && linesOf(moved.memory) == (QVector<qint64>{ 2, 4 }) && pathsAre(*moved.memory, movedTo),
              QStringLiteral("перенос: память переехала"));
        store.waitForWrites();
        CHECK(!QFileInfo::exists(memoryFileOf(memDir, m)) && QFileInfo::exists(memoryFileOf(memDir, movedTo)),
              QStringLiteral("перенос: прежняя память убрана"));
        CHECK(FileMemoryStore(memDir).open(movedTo).found == FileMemoryStore::Found::ByPath,
              QStringLiteral("перенос: дальше — по пути"));

        const QString copyTo = logs.filePath(QStringLiteral("copied-again.log"));
        CHECK(QFile::copy(c, copyTo), QStringLiteral("скопирован"));
        const auto copied = store.open(copyTo);
        CHECK(copied.found == FileMemoryStore::Found::ByContent && copied.copied
                  && linesOf(copied.memory) == (QVector<qint64>{ 1 }),
              QStringLiteral("копия: память скопирована"));
        store.waitForWrites();
        CHECK(QFileInfo::exists(memoryFileOf(memDir, c)) && QFileInfo::exists(memoryFileOf(memDir, copyTo)),
              QStringLiteral("копия: обе памяти на месте"));

        const QString shortMoved = logs.filePath(QStringLiteral("short-renamed.log"));
        CHECK(QFile::rename(shortFile, shortMoved) && appendToFile(shortMoved, logBytes(2, 50, "grew")),
              QStringLiteral("короткий переименован и дописан"));
        const auto foundShort = store.open(shortMoved);
        CHECK(foundShort.found == FileMemoryStore::Found::ByContent
                  && linesOf(foundShort.memory) == (QVector<qint64>{ 1 }),
              QStringLiteral("короткий файл (< 4 КБ) находится по содержимому"));
        const QString unrelated = writeFile(logs, "unrelated.log", logBytes(4, 9, "else"));
        CHECK(store.open(unrelated).found == FileMemoryStore::Found::None,
              QStringLiteral("чужой короткий — не найден"));
        const QString otherEmpty = writeFile(logs, "empty-2.log", QByteArray());
        CHECK(store.open(otherEmpty).found == FileMemoryStore::Found::None,
              QStringLiteral("пустой файл по содержимому не ищется"));
    }

    // ---- Лимит и вытеснение -----------------------------------------------------
    for (const int limit : { 4, 2 }) {
        g_context = QStringLiteral("память файла: лимит %1").arg(limit);
        const QString memDir = dir.filePath(QStringLiteral("mem-limit-%1").arg(limit));
        QDir().mkpath(memDir);
        const QDateTime base = QDateTime::fromString(QStringLiteral("2026-11-01T00:00:00.000Z"), Qt::ISODateWithMs);
        QStringList paths;
        for (int i = 0; i < 6; ++i) {
            const QString p = writeFile(logs, QStringLiteral("limit-%1-%2.log").arg(limit).arg(i).toUtf8(),
                                        logBytes(20, i, "limit"));
            paths << p;
            // Давние: l0, l1 — с закладками; l2…l5 — без, по возрастанию времени.
            FileMemory m = memoryOf(p, i < 2 ? QVector<qint64>{ 1 } : QVector<qint64>{});
            m.lastUsed = base.addSecs(60 * i);
            CHECK(FileMemoryStore::writeFileAtomically(memoryFileOf(memDir, p), objectToBytes(m.toJson())),
                  QStringLiteral("память %1").arg(i));
        }
        const QString fresh = writeFile(logs, QStringLiteral("limit-%1-new.log").arg(limit).toUtf8(),
                                        logBytes(20, 99, "fresh"));
        FileMemoryStore store(memDir, limit);
        store.save(memoryOf(fresh, {}), {});
        store.waitForWrites();
        QStringList expected{ FileMemoryStore::fileNameForPath(paths[0]), FileMemoryStore::fileNameForPath(paths[1]),
                              FileMemoryStore::fileNameForPath(fresh) };
        if (limit == 4)
            expected << FileMemoryStore::fileNameForPath(paths[5]); // самая свежая из прежних
        expected.sort();
        CHECK(memoryFiles(memDir) == expected,
              QStringLiteral("остались: %1").arg(memoryFiles(memDir).join(QStringLiteral(", "))));
    }

    // ---- Два экземпляра на один каталог --------------------------------------------
    g_context = QStringLiteral("память файла: два экземпляра");
    {
        const QString memDir = dir.filePath(QStringLiteral("mem-two"));
        const QString p = writeFile(logs, "shared.log", logBytes(60));
        FileMemoryStore first(memDir);
        FileMemoryStore second(memDir);
        CHECK(first.open(p).found == FileMemoryStore::Found::None
                  && second.open(p).found == FileMemoryStore::Found::None,
              QStringLiteral("оба открыли без памяти"));
        // Первое окно поставило 1 и закрыло вкладку; второе — 2 (своя база пуста).
        first.save(memoryOf(p, { 1 }), {});
        first.waitForWrites();
        FileMemory secondMemory = memoryOf(p, { 2 });
        secondMemory.view->viewportOffset = 77;
        second.save(secondMemory, {});
        second.waitForWrites();
        auto now = FileMemoryStore(memDir).open(p);
        CHECK(linesOf(now.memory) == (QVector<qint64>{ 1, 2 }), QStringLiteral("закладки объединены"));
        CHECK(now.memory && now.memory->view && now.memory->view->viewportOffset == 77,
              QStringLiteral("вид — последней записи"));
        // Первое окно добавило 3 (база — его прошлая запись {1}).
        first.save(memoryOf(p, { 1, 3 }), { RecordId::forLine(1, QStringLiteral("text of 1")) });
        first.waitForWrites();
        CHECK(linesOf(FileMemoryStore(memDir).open(p).memory) == (QVector<qint64>{ 1, 2, 3 }),
              QStringLiteral("чужая закладка 2 не потерялась"));
        // Первое окно сняло 1: она не воскресает, 2 остаётся.
        first.save(memoryOf(p, { 3 }), { RecordId::forLine(1, QStringLiteral("text of 1")),
                                         RecordId::forLine(3, QStringLiteral("text of 3")) });
        first.waitForWrites();
        CHECK(linesOf(FileMemoryStore(memDir).open(p).memory) == (QVector<qint64>{ 2, 3 }),
              QStringLiteral("снятая закладка не воскресла"));
        // Запись без вида (файл был в слитой вкладке) оставляет лежащий вид.
        FileMemory withOffset = memoryOf(p, { 2, 3 });
        withOffset.view->viewportOffset = 55;
        first.save(withOffset, { RecordId::forLine(3, QStringLiteral("text of 3")) });
        first.waitForWrites();
        second.save(memoryOf(p, { 2, 3 }, false), { RecordId::forLine(2, QStringLiteral("text of 2")) });
        second.waitForWrites();
        now = FileMemoryStore(memDir).open(p);
        CHECK(now.memory && now.memory->view && now.memory->view->viewportOffset == 55,
              QStringLiteral("запись без вида не стирает вид"));
    }
}

// ---------------------------------------------------------------------------
// Память файла во вкладке: открыть → закладки, маркер, позиция, фильтр →
// закрыть → открыть снова
// ---------------------------------------------------------------------------

static std::unique_ptr<LogViewWidget> openWithMemory(FileMemoryStore& store, const QString& path,
                                                     const QString& schema, QStringList* messages = nullptr)
{
    auto tab = makeTab(schema);
    tab->setFileMemory(&store);
    tab->setFileMemorySaveDelay(30);
    if (messages)
        QObject::connect(tab.get(), &LogViewWidget::statusMessage,
                         [messages](const QString& text) { messages->append(text); });
    tab->addLogFile(path);
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    return tab;
}

static bool offerBarShown(LogViewWidget& tab)
{
    auto* bar = tab.findChild<QFrame*>(QStringLiteral("filtersOfferBar"));
    return bar && !bar->isHidden();
}

// Ответить на ближайший QMessageBox (вопрос панели) кнопкой answer; вопрос
// появится внутри click() — во вложенном цикле событий.
static void clickQuestion(QMessageBox::StandardButton answer)
{
    if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
        if (QAbstractButton* button = box->button(answer))
            button->click();
        return;
    }
    QTimer::singleShot(10, [answer]() { clickQuestion(answer); });
}

static void answerNextQuestion(QMessageBox::StandardButton answer)
{
    QTimer::singleShot(0, [answer]() { clickQuestion(answer); });
}

static void checkTabMemory(const QDir& dir, const QString& name, const QString& schema)
{
    const QString memDir = dir.filePath(name + QStringLiteral("-tab-memory"));
    FileMemoryStore store(memDir);
    const QString path = writeFile(dir, name + "-memory.log", logBytes(400));
    const QString neighbour = writeFile(dir, name + "-memory-neighbour.log", logBytes(50, 5000, "neighbour"));

    // ---- Исходная вкладка ---------------------------------------------------------
    g_context = name + QStringLiteral(" / память: исходная");
    Saved saved;
    saved.path = path;
    {
        auto tab = openWithMemory(store, path, schema);
        LogModel& model = *tab->model();
        for (const qint64 line : { qint64(2), qint64(40), qint64(120), qint64(200) }) {
            const QString note = QStringLiteral("заметка к %1").arg(line);
            tab->bookmarks().add(rowOfLine(model, path, line), note);
            saved.notes.insert(line, note);
        }
        HighlightPattern marker;
        marker.text = QStringLiteral("line 1");
        marker.color = QColor(Qt::yellow);
        model.setRowMarkers({ marker });
        FilterRule rule;
        rule.action = FilterRule::Action::Exclude;
        rule.text = QStringLiteral("line 13");
        FilterRuleSet rules;
        rules.rules = { rule };
        rules.bindFields(tab->parserFieldNames(), false);
        model.setFilters({ LogLevel::Info, LogLevel::Warn, LogLevel::Unknown }, QDateTime(), QDateTime(), rules);
        CHECK(settled(*tab), QStringLiteral("фильтр"));
        const int row = rowOfLine(model, path, 170);
        tab->view()->goToRow(row);
        pump(20);
        QScrollBar* bar = tab->view()->verticalScrollBar();
        bar->setValue(bar->value() + bar->singleStep() * 3);
        pump(20);
        saved.currentLine = 170;
        saved.currentTop = topOf(*tab, row);
        saved.state = tab->captureState();
        tab->saveFileMemory(); // как closeTab
    }
    store.waitForWrites();
    CHECK(QFileInfo::exists(memoryFileOf(memDir, path)), QStringLiteral("память записана"));

    // ---- Открыть снова: всё на месте, фильтр предложен ---------------------------
    g_context = name + QStringLiteral(" / память: открыть снова");
    {
        auto tab = openWithMemory(store, path, schema);
        LogModel& model = *tab->model();
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки"));
        CHECK(highlightPatternsToJson(model.rowMarkers()) == highlightPatternsToJson(saved.state.view.markers),
              QStringLiteral("маркеры — сразу"));
        CHECK(model.logLevelFilter().isEmpty() && !model.filterRules().isActive(),
              QStringLiteral("фильтры не применены сами"));
        CHECK(tab->hasOfferedFilters() && offerBarShown(*tab), QStringLiteral("фильтры предложены плашкой"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция"));
        const int allRows = model.rowCount();
        auto* restore = tab->findChild<QPushButton*>(QStringLiteral("restoreFiltersButton"));
        CHECK(restore != nullptr, QStringLiteral("кнопка «вернуть фильтры»"));
        if (restore)
            restore->click();
        CHECK(settled(*tab), QStringLiteral("фильтры возвращены"));
        CHECK(model.logLevelFilter() == saved.state.view.levels && model.rowCount() < allRows
                  && rowOfLine(model, path, 15) < 0 && !offerBarShown(*tab) && !tab->hasOfferedFilters(),
              QStringLiteral("щелчок вернул фильтры, плашка ушла"));
        CHECK(tab->view()->userSelectedRow() == rowOfLine(model, path, saved.currentLine),
              QStringLiteral("выбор пережил фильтр"));

        // Правка закладки — запись сама, через задержку (ждём состояние).
        g_context = name + QStringLiteral(" / память: отложенная запись");
        CHECK(!tab->isFileMemorySavePending(), QStringLiteral("сверка закладок записи не просит"));
        const int row = rowOfLine(model, path, 300);
        CHECK(row >= 0 && tab->bookmarks().add(row, QStringLiteral("новая")) >= 0, QStringLiteral("поставлена"));
        CHECK(tab->isFileMemorySavePending(), QStringLiteral("запись запланирована"));
        CHECK(waitFor([&] { return !tab->isFileMemorySavePending(); }, 10000), QStringLiteral("запись прошла"));
        store.waitForWrites();
        const auto onDisk = FileMemoryStore(memDir).open(path);
        CHECK(linesOf(onDisk.memory).contains(300), QStringLiteral("новая закладка на диске до закрытия"));
        const Bookmark* added = nullptr;
        for (const Bookmark& b : tab->bookmarks().bookmarks())
            if (b.line == 300)
                added = tab->bookmarks().find(b.id);
        CHECK(added && tab->bookmarks().remove(added->id), QStringLiteral("снята"));
        CHECK(tab->isFileMemorySavePending(), QStringLiteral("снятие тоже пишется"));
        tab->saveFileMemory();
    }
    store.waitForWrites();

    // ---- Отказ от предложения: до закрытия вкладки, фильтры не теряются ------------
    g_context = name + QStringLiteral(" / память: отказ от фильтров");
    {
        // Фильтры возвращены щелчком и записаны — теперь они предлагаются снова.
        auto tab = openWithMemory(store, path, schema);
        CHECK(offerBarShown(*tab), QStringLiteral("предложены"));
        auto* dismiss = tab->findChild<QToolButton*>(QStringLiteral("dismissFiltersOfferButton"));
        if (dismiss)
            dismiss->click();
        CHECK(!offerBarShown(*tab) && tab->model()->logLevelFilter().isEmpty(),
              QStringLiteral("плашка закрыта, фильтров нет"));
        tab->saveFileMemory();
        store.waitForWrites();
        auto again = openWithMemory(store, path, schema);
        CHECK(again->hasOfferedFilters() && offerBarShown(*again),
              QStringLiteral("при следующем открытии предложены снова"));
    }

    // ---- Второй файл вкладки — только закладки -------------------------------------
    g_context = name + QStringLiteral(" / память: второй файл");
    {
        auto tab = openWithMemory(store, neighbour, schema);
        tab->addLogFile(path);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        CHECK(tab->bookmarks().count() == saved.notes.size(), QStringLiteral("закладки файла пришли"));
        CHECK(!tab->hasOfferedFilters() && tab->model()->rowMarkers().isEmpty(),
              QStringLiteral("вид (фильтры, маркеры) — нет"));
        CHECK(tab->view()->userSelectedRow() < 0, QStringLiteral("позиция — нет"));
        tab->saveFileMemory();
        store.waitForWrites();
        const auto memory = FileMemoryStore(memDir).open(path);
        CHECK(memory.memory && memory.memory->view && memory.memory->bookmarks.size() == saved.notes.size(),
              QStringLiteral("слитая вкладка не стёрла вид файла"));
    }

    // ---- Выключенная настройка: не читает и не пишет ---------------------------------
    g_context = name + QStringLiteral(" / память: выключена");
    {
        AppSettings::instance().setRememberFileState(false);
        const QByteArray before = readBytes(memoryFileOf(memDir, path));
        auto tab = openWithMemory(store, path, schema);
        CHECK(tab->bookmarks().isEmpty() && !tab->hasOfferedFilters() && tab->model()->rowMarkers().isEmpty(),
              QStringLiteral("не читает"));
        tab->bookmarks().add(rowOfLine(*tab->model(), path, 10), QStringLiteral("не запишется"));
        CHECK(!tab->isFileMemorySavePending(), QStringLiteral("запись не планируется"));
        tab->saveFileMemory();
        store.waitForWrites();
        CHECK(readBytes(memoryFileOf(memDir, path)) == before, QStringLiteral("не пишет"));
        AppSettings::instance().setRememberFileState(true);
    }

    // ---- Снимок (сессия R-5) главнее памяти, закладки объединяются -----------------------
    g_context = name + QStringLiteral(" / память: снимок главнее памяти");
    {
        auto tab = makeTab(schema);
        tab->setFileMemory(&store);
        TabState session = saved.state;
        session.bookmarks.resize(1);
        tab->applyState(session, LogViewWidget::StateFilters::Apply);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        CHECK(!tab->hasOfferedFilters() && tab->model()->logLevelFilter() == saved.state.view.levels,
              QStringLiteral("фильтры снимка применены, а не предложены"));
        CHECK(tab->bookmarks().count() == saved.notes.size(),
              QStringLiteral("закладки памяти дополнили снимок: %1").arg(tab->bookmarks().count()));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция снимка"));
    }

    // ---- Дописан ------------------------------------------------------------------------
    g_context = name + QStringLiteral(" / память: дописан");
    {
        CHECK(appendToFile(path, logBytes(50, 3000, "appended")), QStringLiteral("дописан"));
        auto tab = openWithMemory(store, path, schema);
        checkBookmarksOnLines(*tab, saved, QStringLiteral("закладки"));
        checkPositionRestored(*tab, saved, QStringLiteral("позиция"));
    }

    // ---- Переписан: память отсоединена, закладки не цепляются к чужим строкам ----------
    g_context = name + QStringLiteral(" / память: переписан");
    {
        CHECK(writeBytes(path, logBytes(400, 7, "other")), QStringLiteral("переписан"));
        QStringList messages;
        auto tab = openWithMemory(store, path, schema, &messages);
        CHECK(messages.size() == 1 && messages.value(0).contains(QStringLiteral("changed")),
              QStringLiteral("сообщение: %1").arg(messages.join(QStringLiteral(" | "))));
        CHECK(tab->bookmarks().count() == saved.notes.size(), QStringLiteral("прежние закладки — в панели"));
        for (const Bookmark& b : tab->bookmarks().bookmarks())
            CHECK(tab->bookmarks().status(b.id) == BookmarkStore::Status::Missing
                      && b.note == saved.notes.value(b.line),
                  QStringLiteral("строка %1 — серая, заметка цела").arg(b.line));
        CHECK(tab->bookmarks().visibleRows().isEmpty(), QStringLiteral("к чужим строкам не прицепились"));
        CHECK(!tab->hasOfferedFilters() && tab->model()->rowMarkers().isEmpty()
                  && tab->view()->userSelectedRow() < 0,
              QStringLiteral("открыт чистым"));
        CHECK(memoryFiles(memDir).filter(QStringLiteral("~")).size() == 1, QStringLiteral("память отсоединена"));
        tab->saveFileMemory();
        store.waitForWrites();
        const auto fresh = FileMemoryStore(memDir).open(path);
        CHECK(fresh.found == FileMemoryStore::Found::ByPath && fresh.memory->bookmarks.isEmpty(),
              QStringLiteral("память нового содержимого — без серых закладок"));
    }

    // ---- Перенос вкладки (объединение): снятая после переноса закладка не воскресает ----
    g_context = name + QStringLiteral(" / память: объединение вкладок");
    {
        const QString merged = writeFile(dir, name + "-memory-merge.log", logBytes(100, 0, "merge"));
        {
            auto first = openWithMemory(store, merged, schema);
            first->bookmarks().add(rowOfLine(*first->model(), merged, 5), QStringLiteral("пять"));
            first->bookmarks().add(rowOfLine(*first->model(), merged, 9), QStringLiteral("девять"));
            first->saveFileMemory();
        }
        store.waitForWrites();
        auto source = openWithMemory(store, merged, schema);
        auto target = makeTab(schema);
        target->setFileMemory(&store);
        target->addFilesFrom(*source);
        CHECK(settled(*target), QStringLiteral("загрузка приёмника"));
        source->saveFileMemory(); // closeTab источника
        source.reset();
        for (const Bookmark& b : target->bookmarks().bookmarks())
            if (b.line == 5)
                target->bookmarks().remove(b.id);
        target->saveFileMemory();
        store.waitForWrites();
        CHECK(linesOf(FileMemoryStore(memDir).open(merged).memory) == (QVector<qint64>{ 9 }),
              QStringLiteral("снятая в приёмнике закладка не вернулась"));
    }

    // ---- Clear All — с подтверждением -----------------------------------------------------
    if (name == QLatin1String("resident")) {
        g_context = QStringLiteral("Clear All с подтверждением");
        auto tab = openWithMemory(store, neighbour, schema);
        tab->bookmarks().add(0, QStringLiteral("одна"));
        tab->bookmarks().add(1, QStringLiteral("две"));
        BookmarkPanelWidget panel;
        panel.setLogView(tab.get());
        auto* clear = panel.findChild<QToolButton*>(QStringLiteral("bookmarkClearAll"));
        CHECK(clear != nullptr, QStringLiteral("кнопка"));
        if (clear) {
            answerNextQuestion(QMessageBox::No);
            clear->click();
            CHECK(tab->bookmarks().count() == 2, QStringLiteral("«нет» — закладки на месте"));
            answerNextQuestion(QMessageBox::Yes);
            clear->click();
            CHECK(tab->bookmarks().isEmpty(), QStringLiteral("«да» — сняты"));
        }
    }
}

// ---------------------------------------------------------------------------
// Сессия (R-5, шаг 1): формат, файл, перепривязка, открытие, отчёт
// ---------------------------------------------------------------------------

static const QString kGoldenSchema = QStringLiteral(
    "{\"blocks\":[{\"name\":\"Time\",\"type\":\"timestamp\"},{\"name\":\"Level\",\"type\":\"level\"}],\"version\":1}");

static QString goldenSessionPath()
{
    return root() + QStringLiteral("/work/incident.dlsession");
}

static SessionState goldenSession()
{
    SessionState s;
    s.schemaName = QStringLiteral("Spring Boot");
    s.schema = kGoldenSchema;
    s.fieldExtraction = true;
    s.visibleFields = QStringList{ QStringLiteral("Level"), QStringLiteral("Message") };
    s.tabs.append(goldenState());
    TabState second;
    SourceIdentity other;
    other.path = root() + QStringLiteral("/work/other.log");
    other.size = 10;
    other.head = 1;
    other.tail = 2;
    second.sources.append({ other, std::nullopt });
    second.view.followTail = true;
    s.tabs.append(second);
    s.activeTab = 1;
    return s;
}

static void refusedSession(const QJsonObject& json, const QString& what, bool newer = false)
{
    QString error;
    bool newerVersion = !newer;
    const auto s = SessionState::fromJson(json, &error, &newerVersion);
    CHECK(!s.has_value() && !error.isEmpty() && newerVersion == newer, QStringLiteral("отказ: ") + what);
}

static void checkSessionFormat()
{
    g_context = QStringLiteral("сессия: формат");
    const SessionState golden = goldenSession();
    const QJsonObject json = golden.toJson(goldenSessionPath());
    QString error;
    const auto back = SessionState::fromJson(bytesToObject(objectToBytes(json)), &error);
    CHECK(back.has_value(), QStringLiteral("читается: ") + error);
    if (back) {
        CHECK(back->toJson(goldenSessionPath()) == json, QStringLiteral("круговой проход — тот же JSON"));
        CHECK(back->schemaName == golden.schemaName && back->schema == golden.schema
                  && back->fieldExtraction && back->visibleFields == golden.visibleFields
                  && back->activeTab == 1 && back->tabs.size() == 2,
              QStringLiteral("схема, колонки, активная вкладка"));
        CHECK(back->tabs.value(0).toJson() == golden.tabs[0].toJson()
                  && back->tabs.value(1).toJson() == golden.tabs[1].toJson(),
              QStringLiteral("вкладки целиком"));
        CHECK(back->relativePaths.value(root() + QStringLiteral("/logs/app.log")) == QStringLiteral("../logs/app.log")
                  && back->relativePaths.value(root() + QStringLiteral("/work/other.log")) == QStringLiteral("other.log"),
              QStringLiteral("относительные пути к каталогу файла сессии"));
    }
    CHECK(golden.paths().size() == 3, QStringLiteral("пути вкладок: файлы и источники закладок"));

    // Без схемы и без файла сессии: ничего лишнего.
    SessionState bare;
    bare.tabs.append(TabState());
    const QJsonObject bareJson = bare.toJson();
    CHECK(!bareJson.contains("schema") && !bareJson.contains("visibleFields")
              && bareJson.value("files").toArray().isEmpty(),
          QStringLiteral("пустое не пишется"));
    const auto bareBack = SessionState::fromJson(bareJson, &error);
    CHECK(bareBack && !bareBack->hasSchema() && bareBack->activeTab == -1 && bareBack->tabs.size() == 1,
          QStringLiteral("без схемы читается: ") + error);

    // Эталон версии 1.
    g_context = QStringLiteral("сессия: эталон версии 1");
    QFile file(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/session-v1.json"));
    CHECK(file.open(QIODevice::ReadOnly), QStringLiteral("эталон открывается"));
    QByteArray bytes = file.readAll();
    bytes.replace("@ROOT@", root().toUtf8());
    const QJsonObject stored = bytesToObject(bytes);
    const auto parsed = SessionState::fromJson(stored, &error);
    CHECK(parsed.has_value(), QStringLiteral("эталон читается: ") + error);
    if (parsed)
        CHECK(parsed->toJson(goldenSessionPath()) == stored, QStringLiteral("прочитанный эталон пишется им же"));
    const bool same = json == stored;
    CHECK(same, QStringLiteral("сессия из известных значений пишется ровно в эталон"));
    if (!same)
        std::fprintf(stderr, "--- сейчас пишется:\n%s\n", objectToBytes(json).constData());

    // Отказы: большая версия отличается от битых данных.
    g_context = QStringLiteral("сессия: отказы");
    QJsonObject bad = json;
    bad.insert("version", 2);
    refusedSession(bad, QStringLiteral("версия 2"), true);
    bad.insert("version", "1");
    refusedSession(bad, QStringLiteral("версия строкой"));
    bad = json;
    bad.insert("format", "dendrolog-tab-state");
    refusedSession(bad, QStringLiteral("чужой формат"));
    bad = json;
    bad.remove("tabs");
    refusedSession(bad, QStringLiteral("нет вкладок"));
    bad = json;
    bad.insert("tabs", QJsonArray{ 5 });
    refusedSession(bad, QStringLiteral("вкладка не объект"));
    {
        bad = json;
        QJsonArray tabs = bad.value("tabs").toArray();
        QJsonObject t = tabs.at(0).toObject();
        t.insert("version", 7);
        tabs.replace(0, t);
        bad.insert("tabs", tabs);
        refusedSession(bad, QStringLiteral("вкладка чужой версии"));
    }
    bad = json;
    bad.insert("activeTab", 2);
    refusedSession(bad, QStringLiteral("активная вкладка за концом"));
    bad = json;
    bad.insert("schema", "Spring Boot");
    refusedSession(bad, QStringLiteral("схема строкой"));
    bad = json;
    bad.insert("schema", QJsonObject{ { "name", "x" } });
    refusedSession(bad, QStringLiteral("схема без определения"));
    bad = json;
    bad.insert("visibleFields", QJsonArray{ 1 });
    refusedSession(bad, QStringLiteral("колонки не строки"));
    bad = json;
    bad.insert("files", QJsonArray{ QJsonObject{ { "relative", "a.log" } } });
    refusedSession(bad, QStringLiteral("файл без пути"));
    bad = json;
    bad.insert("fieldExtraction", "yes");
    refusedSession(bad, QStringLiteral("извлечение полей строкой"));
    QJsonObject extra = json;
    extra.insert("comment", "из будущего");
    extra.insert("windowLayout", QJsonObject());
    CHECK(SessionState::fromJson(extra).has_value(), QStringLiteral("лишние ключи не мешают"));
}

static void checkSessionFile(const QDir& dir)
{
    g_context = QStringLiteral("сессия: файл");
    const QString path = dir.filePath(QStringLiteral("file-check.dlsession"));
    QString error;
    const SessionState golden = goldenSession();
    CHECK(SessionFile::save(path, golden, &error), QStringLiteral("запись: ") + error);
    const SessionFile::LoadResult loaded = SessionFile::load(path);
    CHECK(loaded.state && loaded.state->toJson(path) == golden.toJson(path), QStringLiteral("чтение: ") + loaded.error);
    CHECK(!QFileInfo::exists(path + QStringLiteral(".tmp")), QStringLiteral("без хвостов записи"));

    const QString garbage = writeFile(dir, QStringLiteral("garbage.dlsession"), "{ not json");
    SessionFile::LoadResult r = SessionFile::load(garbage);
    CHECK(!r.state && !r.error.isEmpty() && !r.newerVersion, QStringLiteral("битый JSON — отказ"));
    QJsonObject newer = golden.toJson(path);
    newer.insert("version", 5);
    const QByteArray newerBytes = objectToBytes(newer);
    const QString newerPath = writeFile(dir, QStringLiteral("newer.dlsession"), newerBytes);
    r = SessionFile::load(newerPath);
    CHECK(!r.state && r.newerVersion, QStringLiteral("новая версия — отказ, отличимый от битых данных"));
    CHECK(readBytes(newerPath) == newerBytes, QStringLiteral("файл новой версии не тронут"));
    r = SessionFile::load(dir.filePath(QStringLiteral("absent.dlsession")));
    CHECK(!r.state && !r.error.isEmpty(), QStringLiteral("нет файла — отказ"));

    CHECK(SessionFile::withSuffix(QStringLiteral("/x/a")) == QStringLiteral("/x/a.dlsession")
              && SessionFile::withSuffix(QStringLiteral("/x/a.DLSession")) == QStringLiteral("/x/a.DLSession")
              && SessionFile::isSessionFile(QStringLiteral("C:/x/b.dlsession"))
              && !SessionFile::isSessionFile(QStringLiteral("/x/b.log")),
          QStringLiteral("расширение"));
}

static SourceIdentity identityOf(const QString& path)
{
    return SourceIdentity::capture(path).value_or(SourceIdentity());
}

static void checkSessionRebind(const QDir& dir)
{
    g_context = QStringLiteral("сессия: перепривязка");
    const QString oldRoot = dir.filePath(QStringLiteral("rebind-old"));
    const QString newRoot = dir.filePath(QStringLiteral("rebind-new"));
    QDir().mkpath(oldRoot + QStringLiteral("/logs"));
    const QString a = writeFile(QDir(oldRoot + QStringLiteral("/logs")), QStringLiteral("a.log"), logBytes(60));
    const QString b = writeFile(QDir(oldRoot + QStringLiteral("/logs")), QStringLiteral("b.log"), logBytes(60, 3, "bee"));
    const QString c = writeFile(QDir(oldRoot), QStringLiteral("c.log"), logBytes(60, 5, "sea"));
    const QString d = writeFile(QDir(oldRoot), QStringLiteral("d.log"), logBytes(60, 7, "dee"));
    // Файл вне перенесённого каталога: относительный путь выведет мимо.
    const QString outside = writeFile(dir, QStringLiteral("rebind-outside.log"), logBytes(30, 9, "out"));
    SessionState s;
    TabState t1;
    t1.sources = { { identityOf(a), std::nullopt }, { identityOf(b), std::nullopt } };
    BookmarkState bm;
    bm.source = identityOf(b);
    bm.record = RecordId::forLine(3, QStringLiteral("x"));
    t1.bookmarks.append(bm);
    t1.view.currentSource = identityOf(a);
    t1.view.current = RecordId::forLine(5, QStringLiteral("y"));
    TabState t2;
    t2.sources = { { identityOf(c), std::nullopt }, { identityOf(d), std::nullopt },
                   { identityOf(outside), std::nullopt } };
    s.tabs = { t1, t2 };
    const QString sessionAt = oldRoot + QStringLiteral("/session/incident.dlsession");
    QDir().mkpath(QFileInfo(sessionAt).absolutePath());
    QString error;
    CHECK(SessionFile::save(sessionAt, s, &error), QStringLiteral("запись: ") + error);

    {   // На месте: всё по абсолютным путям.
        auto loaded = SessionFile::load(sessionAt);
        CHECK(loaded.state.has_value(), QStringLiteral("читается"));
        SessionState state = loaded.state.value_or(SessionState());
        const auto res = SessionFile::rebind(state, sessionAt);
        CHECK(res.size() == 5 && std::all_of(res.cbegin(), res.cend(), [](const SessionFile::Resolution& r) {
                  return r.kind == SessionFile::Resolution::Kind::Valid && r.path == r.savedPath; }),
              QStringLiteral("на месте — все Valid"));
    }

    // Перенос каталога вместе с сессией; c удалён, d переписан.
    CHECK(QDir().rename(oldRoot, newRoot), QStringLiteral("каталог перенесён"));
    const QString newSession = newRoot + QStringLiteral("/session/incident.dlsession");
    CHECK(QFile::remove(newRoot + QStringLiteral("/c.log")), QStringLiteral("c удалён"));
    CHECK(writeBytes(newRoot + QStringLiteral("/d.log"), logBytes(60, 11, "other")), QStringLiteral("d переписан"));
    auto loaded = SessionFile::load(newSession);
    CHECK(loaded.state.has_value(), QStringLiteral("перенесённая читается"));
    SessionState state = loaded.state.value_or(SessionState());
    const QVector<SessionFile::Resolution> res = SessionFile::rebind(state, newSession);
    const auto kindOf = [&res](const QString& saved) {
        for (const auto& r : res)
            if (r.savedPath == saved)
                return std::make_pair(r.kind, r.path);
        return std::make_pair(SessionFile::Resolution::Kind::Missing, QString());
    };
    using Kind = SessionFile::Resolution::Kind;
    CHECK(kindOf(a) == std::make_pair(Kind::Relocated, newRoot + QStringLiteral("/logs/a.log")),
          QStringLiteral("a найден по относительному пути"));
    CHECK(kindOf(b) == std::make_pair(Kind::Relocated, newRoot + QStringLiteral("/logs/b.log")),
          QStringLiteral("b найден по относительному пути"));
    CHECK(kindOf(c).first == Kind::Missing, QStringLiteral("c — пропал"));
    CHECK(kindOf(d) == std::make_pair(Kind::Changed, newRoot + QStringLiteral("/d.log")),
          QStringLiteral("d — изменён, путь новый"));
    CHECK(kindOf(outside) == std::make_pair(Kind::Valid, outside), QStringLiteral("файл вне каталога — на месте"));
    const TabState& r1 = state.tabs.value(0);
    CHECK(r1.sources.value(0).source.path == newRoot + QStringLiteral("/logs/a.log")
              && r1.bookmarks.value(0).source.path == newRoot + QStringLiteral("/logs/b.log")
              && r1.view.currentSource && r1.view.currentSource->path == newRoot + QStringLiteral("/logs/a.log"),
          QStringLiteral("пути подменены в файлах, закладках и текущей строке"));
    CHECK(state.tabs.value(1).sources.value(0).source.path == c, QStringLiteral("пропавший — с прежним путём"));

    // И старый, и новый на месте: абсолютный главнее.
    QDir().mkpath(oldRoot + QStringLiteral("/logs"));
    writeFile(QDir(oldRoot + QStringLiteral("/logs")), QStringLiteral("a.log"), logBytes(60));
    SessionState again = SessionFile::load(newSession).state.value_or(SessionState());
    const auto both = SessionFile::rebind(again, newSession);
    CHECK(!both.isEmpty() && both[0].kind == Kind::Valid && both[0].path == a,
          QStringLiteral("оба на месте — абсолютный"));
}

static void checkSessionSchema()
{
    g_context = QStringLiteral("сессия: схема");
    SchemaStore::SchemaList list;
    SchemaStore::SchemaEntry e1;
    e1.name = QStringLiteral("Spring Boot");
    e1.schema = QStringLiteral("{\"blocks\":[],\"version\":1}");
    SchemaStore::SchemaEntry e2;
    e2.name = QStringLiteral("Mine");
    e2.schema = kGoldenSchema;
    list = { e1, e2 };
    auto c = SessionController::chooseSchema(list, QStringLiteral("Renamed"), kGoldenSchema);
    CHECK(c.index == 1 && !c.added, QStringLiteral("совпало определение — её, имя не важно"));
    // То же определение с другим порядком ключей и пробелами.
    c = SessionController::chooseSchema(list, QStringLiteral("x"),
        QStringLiteral("{ \"version\": 1, \"blocks\": [ {\"type\":\"timestamp\",\"name\":\"Time\"}, {\"name\":\"Level\",\"type\":\"level\"} ] }"));
    CHECK(c.index == 1 && !c.added, QStringLiteral("совпадение по содержимому, не по байтам"));
    const QString otherSchema = QStringLiteral("{\"blocks\":[{\"name\":\"Msg\",\"type\":\"rest\"}],\"version\":1}");
    c = SessionController::chooseSchema(list, QStringLiteral("Spring Boot"), otherSchema);
    CHECK(c.added && c.index == 2 && c.newEntry.name == QStringLiteral("Spring Boot (from session)")
              && c.newEntry.schema == otherSchema,
          QStringLiteral("нет в каталоге — добавить с пометкой: ") + c.newEntry.name);
    list.append(c.newEntry);
    const QString third = QStringLiteral("{\"blocks\":[{\"name\":\"X\",\"type\":\"rest\"}],\"version\":1}");
    c = SessionController::chooseSchema(list, QStringLiteral("Spring Boot"), third);
    CHECK(c.added && c.newEntry.name == QStringLiteral("Spring Boot (from session) 2"),
          QStringLiteral("занятое имя — с номером: ") + c.newEntry.name);
    c = SessionController::chooseSchema(list, QString(), QString());
    CHECK(c.index == -1 && !c.added, QStringLiteral("без схемы"));
}

// Вкладки, созданные контроллером: владеет тест.
struct TabPool {
    QString schema;
    std::vector<std::unique_ptr<LogViewWidget>> tabs;
    LogViewWidget* make()
    {
        tabs.push_back(makeTab(schema));
        return tabs.back().get();
    }
};

static void checkSessionOpen(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / сессия: две вкладки на другом пути");
    const QString oldRoot = dir.filePath(name + QStringLiteral("-session-old"));
    const QString newRoot = dir.filePath(name + QStringLiteral("-session-new"));
    const QDir logs(oldRoot + QStringLiteral("/logs"));
    QDir().mkpath(logs.path());
    const QString one = writeFile(logs, QStringLiteral("one.log"), logBytes(400));
    const QString ma = writeFile(logs, QStringLiteral("merged-a.log"), logBytes(200));
    const QString mb = writeFile(logs, QStringLiteral("merged-b.log"), logBytes(200, 1, "bee"));
    const QString sessionPath = oldRoot + QStringLiteral("/incident.dlsession");

    // Вкладка 1: закладки, маркер, фильтры, кодировка, выбранная строка.
    const Saved saved = prepareSource(one, schema, true);
    QString error;
    {
        auto tab1 = makeTab(schema);
        tab1->applyState(saved.state);
        CHECK(settled(*tab1), QStringLiteral("вкладка 1"));
        // Вкладка 2: слитая из двух файлов, закладки с заметками, позиция в b.
        auto tab2 = makeTab(schema);
        tab2->addLogFile(ma);
        tab2->addLogFile(mb);
        CHECK(settled(*tab2), QStringLiteral("вкладка 2"));
        LogModel& m2 = *tab2->model();
        tab2->bookmarks().add(rowOfLine(m2, ma, 10), QStringLiteral("a10"));
        tab2->bookmarks().add(rowOfLine(m2, mb, 20), QStringLiteral("b20"));
        tab2->view()->goToRow(rowOfLine(m2, mb, 100));
        pump(20);
        SessionController controller;
        SessionState session = controller.capture({ tab1.get(), tab2.get() }, tab2.get());
        CHECK(session.tabs.size() == 2 && session.activeTab == 1, QStringLiteral("снята: две вкладки, активна вторая"));
        CHECK(session.tabs.value(1).sources.size() == 2, QStringLiteral("слитая — оба файла"));
        session.schemaName = QStringLiteral("test schema");
        session.schema = schema;
        CHECK(SessionFile::save(sessionPath, session, &error), QStringLiteral("запись: ") + error);
    }

    CHECK(QDir().rename(oldRoot, newRoot), QStringLiteral("каталог с сессией и логами перенесён"));
    const QString newOne = newRoot + QStringLiteral("/logs/one.log");
    const QString newMa = newRoot + QStringLiteral("/logs/merged-a.log");
    const QString newMb = newRoot + QStringLiteral("/logs/merged-b.log");

    // Уже открытая вкладка — сессия к ней добавляется, её не трогает.
    const QString keep = writeFile(dir, name + QStringLiteral("-session-keep.log"), logBytes(30, 50, "keep"));
    auto existing = makeTab(schema);
    existing->addLogFile(keep);
    CHECK(settled(*existing), QStringLiteral("открытая вкладка"));
    const int existingRows = existing->model()->rowCount();

    SessionFile::LoadResult loaded = SessionFile::load(newRoot + QStringLiteral("/incident.dlsession"));
    CHECK(loaded.state.has_value(), QStringLiteral("читается: ") + loaded.error);
    if (!loaded.state)
        return;
    CHECK(loaded.state->schema == schema && loaded.state->schemaName == QStringLiteral("test schema"),
          QStringLiteral("схема в файле"));
    TabPool pool{ schema, {} };
    SessionController controller;
    const auto result = controller.open(*loaded.state, newRoot + QStringLiteral("/incident.dlsession"),
                                        [&pool]() { return pool.make(); });
    CHECK(result.tabs.size() == 2 && !result.hasProblems() && result.relocated == 3,
          QStringLiteral("две вкладки, три файла найдены рядом с сессией (%1)").arg(result.relocated));
    CHECK(result.active == result.tabs.value(1), QStringLiteral("активная вкладка — вторая"));
    CHECK(existing->fileCount() == 1 && existing->model()->rowCount() == existingRows
              && !existing->model()->filterRules().isActive() && existing->bookmarks().isEmpty(),
          QStringLiteral("открытая раньше вкладка не тронута"));
    CHECK(controller.report().size() == 3 && !SessionController::summary(result).isEmpty(),
          QStringLiteral("отчёт: найдены по относительному пути"));
    if (result.tabs.size() != 2)
        return;

    LogViewWidget& t1 = *result.tabs[0];
    CHECK(settled(t1), QStringLiteral("вкладка 1: загрузка"));
    CHECK(t1.fileCount() == 1 && t1.loadedFiles().first()->filePath == newOne, QStringLiteral("вкладка 1: файл по новому пути"));
    Saved moved = saved;
    moved.path = newOne;
    checkBookmarksOnLines(t1, moved, QStringLiteral("вкладка 1: закладки с заметками"));
    LogModel& m1 = *t1.model();
    CHECK(highlightPatternsToJson(m1.rowMarkers()) == highlightPatternsToJson(saved.state.view.markers),
          QStringLiteral("вкладка 1: маркеры"));
    CHECK(m1.logLevelFilter() == saved.state.view.levels
              && m1.filterRules().toJson() == saved.state.view.textRules.toJson(),
          QStringLiteral("вкладка 1: фильтры"));
    checkPositionRestored(t1, moved, QStringLiteral("вкладка 1: строка и место на экране"));
    CHECK(t1.autoReload(), QStringLiteral("вкладка 1: авто-обновление"));
    const FileEncoding enc = t1.fileEncoding(t1.loadedFiles().first());
    CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("вкладка 1: выбор кодировки"));
    CHECK(m1.isIndexedBackend() == (name == QLatin1String("indexed")), QStringLiteral("бэкенд"));

    LogViewWidget& t2 = *result.tabs[1];
    CHECK(settled(t2), QStringLiteral("вкладка 2: загрузка"));
    LogModel& m2 = *t2.model();
    CHECK(t2.fileCount() == 2, QStringLiteral("вкладка 2: оба файла"));
    int found = 0;
    for (const Bookmark& b : t2.bookmarks().bookmarks()) {
        const QString path = b.note == QStringLiteral("a10") ? newMa : newMb;
        CHECK(b.filePath == path && t2.bookmarks().status(b.id) == BookmarkStore::Status::Visible
                  && t2.bookmarks().idAtRow(rowOfLine(m2, path, b.line)) == b.id,
              QStringLiteral("вкладка 2: закладка «%1» на своей строке").arg(b.note));
        ++found;
    }
    CHECK(found == 2, QStringLiteral("вкладка 2: обе закладки"));
    const int row = rowOfLine(m2, newMb, 100);
    CHECK(row >= 0 && t2.view()->userSelectedRow() == row,
          QStringLiteral("вкладка 2: текущая строка %1").arg(currentLine(t2)));

    // Снятая снова сессия — те же вкладки (пути новые).
    const SessionState again = controller.capture({ &t1, &t2 }, &t2);
    CHECK(again.tabs.size() == 2 && again.tabs[0].bookmarks.size() == saved.state.bookmarks.size()
              && again.tabs[1].bookmarks.size() == 2 && again.activeTab == 1,
          QStringLiteral("снятая заново — те же вкладки"));
}

static void checkSessionProblems(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("сессия: пропавший и изменённый источник");
    const QDir sub(dir.filePath(QStringLiteral("problems")));
    QDir().mkpath(sub.path());
    const QByteArray aBytes = logBytes(200);
    const QString a = writeFile(sub, QStringLiteral("a.log"), aBytes);
    const QString b = writeFile(sub, QStringLiteral("b.log"), logBytes(200, 1, "bee"));
    const QString c = writeFile(sub, QStringLiteral("c.log"), logBytes(100, 2, "sea"));
    const QString sessionPath = sub.filePath(QStringLiteral("p.dlsession"));
    {
        auto tab = makeTab(schema);
        tab->addLogFile(a);
        tab->addLogFile(b);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        tab->bookmarks().add(rowOfLine(*tab->model(), a, 10), QStringLiteral("a10"));
        tab->bookmarks().add(rowOfLine(*tab->model(), b, 20), QStringLiteral("b20"));
        auto tabC = makeTab(schema);
        tabC->addLogFile(c);
        CHECK(settled(*tabC), QStringLiteral("загрузка c"));
        tabC->bookmarks().add(rowOfLine(*tabC->model(), c, 5), QStringLiteral("c5"));
        SessionController controller;
        QString error;
        CHECK(SessionFile::save(sessionPath, controller.capture({ tab.get(), tabC.get() }, tab.get()), &error),
              QStringLiteral("запись: ") + error);
    }
    const QString aMoved = writeFile(dir, QStringLiteral("problems-a-elsewhere.log"), aBytes);
    const QString cMoved = dir.filePath(QStringLiteral("problems-c-elsewhere.log"));
    CHECK(QFile::copy(c, cMoved), QStringLiteral("копия c"));
    CHECK(QFile::remove(a) && QFile::remove(c), QStringLiteral("a и c удалены"));
    CHECK(writeBytes(b, logBytes(200, 9, "rewritten")), QStringLiteral("b переписан"));

    TabPool pool{ schema, {} };
    SessionController controller;
    int reportSignals = 0;
    QObject::connect(&controller, &SessionController::reportChanged, [&] { ++reportSignals; });
    const auto loaded = SessionFile::load(sessionPath);
    CHECK(loaded.state.has_value(), QStringLiteral("читается"));
    const auto result = controller.open(loaded.state.value_or(SessionState()), sessionPath,
                                        [&pool]() { return pool.make(); });
    CHECK(result.tabs.size() == 2 && result.missing == 2 && result.changed == 1 && result.hasProblems(),
          QStringLiteral("пропали два, изменён один (%1 / %2)").arg(result.missing).arg(result.changed));
    CHECK(SessionController::summary(result).contains(QStringLiteral("not found")), QStringLiteral("строка состояния"));
    CHECK(reportSignals >= 1, QStringLiteral("отчёт сообщил о себе"));
    const auto report = controller.report();
    CHECK(report.size() == 3, QStringLiteral("в отчёте три записи"));
    int missingA = -1, missingC = -1;
    for (int i = 0; i < report.size(); ++i) {
        if (report[i].savedPath == a)
            missingA = i;
        if (report[i].savedPath == c)
            missingC = i;
        CHECK(!SessionController::describe(report[i]).isEmpty() && !report[i].tabTitle.isEmpty(),
              QStringLiteral("запись отчёта: вкладка и что случилось"));
    }
    CHECK(missingA >= 0 && report[missingA].kind == SessionController::ReportEntry::Kind::Missing,
          QStringLiteral("a — пропал"));
    if (result.tabs.size() != 2 || missingA < 0 || missingC < 0)
        return;
    LogViewWidget& tab = *result.tabs[0];
    CHECK(settled(tab), QStringLiteral("загрузка"));
    CHECK(tab.fileCount() == 1 && tab.loadedFiles().first()->filePath == b, QStringLiteral("открыт только b"));
    for (const Bookmark& bm : tab.bookmarks().bookmarks()) {
        const BookmarkStore::Status st = tab.bookmarks().status(bm.id);
        if (bm.note == QStringLiteral("a10"))
            CHECK(st == BookmarkStore::Status::Pending, QStringLiteral("закладка a ждёт файл"));
        else
            CHECK(st == BookmarkStore::Status::Missing, QStringLiteral("закладка b — серая (файл изменён)"));
    }
    LogViewWidget& tabC = *result.tabs[1];
    CHECK(settled(tabC) && tabC.fileCount() == 0, QStringLiteral("вкладка c открыта пустой"));

    // Пересохранение до «указать файл» пропавший источник не теряет.
    const SessionState resaved = controller.capture({ &tab, &tabC }, &tab);
    CHECK(resaved.tabs.size() == 2 && resaved.tabs[0].sources.size() == 2
              && resaved.tabs[1].sources.size() == 1 && resaved.tabs[1].sources[0].source.path == c,
          QStringLiteral("пересохранённая сессия хранит пропавшие файлы"));

    // «Указать файл»: чужой файл не принимается, свой — открывается.
    QString error;
    CHECK(!controller.locate(missingA, cMoved, &error) && !error.isEmpty(),
          QStringLiteral("чужой файл не принят: ") + error);
    CHECK(tab.fileCount() == 1, QStringLiteral("после отказа вкладка та же"));
    CHECK(controller.locate(missingA, aMoved, &error), QStringLiteral("свой файл принят: ") + error);
    CHECK(settled(tab), QStringLiteral("загрузка указанного"));
    CHECK(tab.fileCount() == 2, QStringLiteral("указанный файл во вкладке"));
    for (const Bookmark& bm : tab.bookmarks().bookmarks())
        if (bm.note == QStringLiteral("a10"))
            CHECK(bm.filePath == aMoved && tab.bookmarks().status(bm.id) == BookmarkStore::Status::Visible
                      && tab.bookmarks().idAtRow(rowOfLine(*tab.model(), aMoved, 10)) == bm.id,
                  QStringLiteral("закладка a переехала на указанный файл"));
    CHECK(controller.report().value(missingA).kind == SessionController::ReportEntry::Kind::Located,
          QStringLiteral("запись отчёта — «открыт оттуда»"));
    CHECK(!controller.locate(missingA, aMoved), QStringLiteral("второй раз указывать нечего"));

    // То же через окно отчёта.
    g_context = QStringLiteral("сессия: окно отчёта");
    SessionReportDialog dialog(&controller);
    QString asked;
    dialog.setFileChooser([&](QWidget*, const QString& savedPath) { asked = savedPath; return cMoved; });
    dialog.show();
    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("sessionReportList"));
    auto* locate = dialog.findChild<QPushButton*>(QStringLiteral("sessionReportLocate"));
    CHECK(list && locate && list->topLevelItemCount() == 3, QStringLiteral("список: три записи"));
    if (!list || !locate)
        return;
    list->setCurrentItem(list->topLevelItem(missingA));
    CHECK(!locate->isEnabled(), QStringLiteral("найденному «указать» не нужно"));
    list->setCurrentItem(list->topLevelItem(missingC));
    CHECK(locate->isEnabled(), QStringLiteral("пропавшему — можно"));
    locate->click();
    CHECK(asked == c, QStringLiteral("выбор файла спросили по сохранённому пути"));
    CHECK(settled(tabC) && tabC.fileCount() == 1 && tabC.bookmarks().count() == 1
              && tabC.bookmarks().status(tabC.bookmarks().bookmarks().first().id) == BookmarkStore::Status::Visible,
          QStringLiteral("вкладка c получила файл и закладку"));
    CHECK(list->topLevelItemCount() == 3 && !locate->isEnabled(), QStringLiteral("список обновился"));

    // Вкладку закрыли — её записи ушли из отчёта.
    pool.tabs[1].reset();
    pump(10);
    CHECK(controller.report().size() == 2 && list->topLevelItemCount() == 2,
          QStringLiteral("закрытая вкладка ушла из отчёта"));
}

static void checkSessionStdin(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("сессия: stdin");
    const QString spool = StdinSpooler::spoolFilePathFor(QCoreApplication::applicationPid());
    CHECK(writeBytes(spool, logBytes(40, 0, "piped")), QStringLiteral("спул создан"));
    const QString path = writeFile(dir, QStringLiteral("session-stdin-neighbour.log"), logBytes(40, 2, "file"));
    auto onlyStdin = makeTab(schema);
    onlyStdin->addLogFile(spool);
    auto merged = makeTab(schema);
    merged->addLogFile(spool);
    merged->addLogFile(path);
    CHECK(settled(*onlyStdin) && settled(*merged), QStringLiteral("загрузка"));
    SessionController controller;
    SessionState s = controller.capture({ onlyStdin.get(), merged.get() }, onlyStdin.get());
    CHECK(s.tabs.size() == 1 && s.tabs[0].sources.size() == 1 && s.tabs[0].sources[0].source.path == path,
          QStringLiteral("вкладка stdin не попала, слитая сохранила файл"));
    CHECK(s.activeTab == -1, QStringLiteral("активной была вкладка stdin — активная не задана"));
    s = controller.capture({ onlyStdin.get(), merged.get() }, merged.get());
    CHECK(s.activeTab == 0, QStringLiteral("активная — по номеру среди сохранённых"));
    onlyStdin.reset();
    merged.reset();
    QFile::remove(spool);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    checkFormat();
    checkFileMemoryFormat();
    checkSessionFormat();
    checkSessionSchema();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "нет временного каталога\n");
        return 2;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();
    AppSettings& settings = AppSettings::instance();
    for (const bool indexed : { false, true }) {
        const QString name = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
        settings.setIndexedThresholdMB(indexed ? 0 : 512);
        checkReopen(dir, name, schema);
        checkFollowTail(dir, name, schema);
        checkMerged(dir, name, schema);
        checkTabMemory(dir, name, schema);
        checkSessionOpen(dir, name, schema);
    }
    settings.setIndexedThresholdMB(512);
    checkStdin(dir, schema);
    checkMidLoadAndThreshold(dir, schema);
    checkFileMemoryStore(dir);
    checkSessionFile(dir);
    checkSessionRebind(dir);
    checkSessionProblems(dir, schema);
    checkSessionStdin(dir, schema);

    std::fprintf(failures ? stderr : stdout, "document_state: %d проверок, %d нарушений\n",
                 checks, failures);
    return failures ? 1 : 0;
}
