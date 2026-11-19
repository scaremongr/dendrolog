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
// Ждём состояние (загрузка, ответ локатора, отложенная раскладка), а не время.
// ============================================================================

#include "appsettings.h"
#include "documentstate.h"
#include "logviewwidget.h"
#include "stdinspooler.h"
#include "testdocuments.h"

#include "LogListView.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>

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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    checkFormat();

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
    }
    settings.setIndexedThresholdMB(512);
    checkStdin(dir, schema);
    checkMidLoadAndThreshold(dir, schema);

    std::fprintf(failures ? stderr : stdout, "document_state: %d проверок, %d нарушений\n",
                 checks, failures);
    return failures ? 1 : 0;
}
