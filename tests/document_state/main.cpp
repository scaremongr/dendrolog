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
// Последняя сессия (R-5, шаг 2; SessionController с временным файлом, а не
// с настоящим каталогом настроек):
//   • выход: сессия окна (вкладки, схема, колонки) пишется; пустое окно и
//     окно только со stdin прошлую не затирают; файл новой версии не
//     перезаписывается;
//   • вторичный экземпляр не пишет и не читает (ссылки нет, startUp не
//     восстанавливает);
//   • запуск: настройка включена — восстановленные вкладки, затем файлы
//     командной строки; выключена — ссылка на стартовом экране (WelcomeWidget)
//     появляется и пропадает после восстановления; файлы второго экземпляра
//     восстановления не запускают; настройка по умолчанию выключена.
// Ждём состояние (загрузка, ответ локатора, отложенная раскладка), а не время.
// ============================================================================

#include "appsettings.h"
#include "bookmarkpanelwidget.h"
#include "documentstate.h"
#include "fieldschemacontroller.h"
#include "filememorystore.h"
#include "logpattern.h"
#include "schemastore.h"
#include "sessioncontroller.h"
#include "sessionfile.h"
#include "sessionreportdialog.h"
#include "logviewwidget.h"
#include "spooldir.h"
#include "welcomewidget.h"
#include "openrouting.h"
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
#include <QSettings>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <atomic>
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


static int g_rowsA = 0;

// Фильтр по файлу (U-23, часть 4): скрытые файлы — индексы источников в
// снимке вкладки; круговой проход через JSON, применение возвращает их;
// показ полного лога снимает приостановленные; эталон версии 1 без поля —
// ничего не скрыто.
static void checkHiddenFiles(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / скрытые файлы");
    const QString a = writeFile(dir, name + "-hidden-a.log", logBytes(120));
    const QString b = writeFile(dir, name + "-hidden-b.log", logBytes(120, 1, "bee"));
    TabState state;
    TabState pausedState;
    {
        auto tab = makeTab(schema);
        tab->addLogFile(a);
        tab->addLogFile(b);
        CHECK(settled(*tab), QStringLiteral("загрузка"));
        LogModel& model = *tab->model();
        const int all = model.rowCount();
        int rowsA = 0;
        for (int r = 0; r < all; ++r)
            rowsA += model.keyAt(r).file->filePath == a ? 1 : 0;
        g_rowsA = rowsA;
        model.setHiddenFiles({b});
        CHECK(settled(*tab) && model.rowCount() == rowsA && model.hasFilters(),
              QStringLiteral("b скрыт: %1 из %2").arg(model.rowCount()).arg(all));
        state = throughJson(tab->captureState());
        CHECK(state.view.hiddenSources == QVector<int>{1} && state.view.hasFilters(),
              QStringLiteral("снимок помнит скрытый b"));
        CHECK(state.toJson().value(QLatin1String("view")).toObject().contains(QLatin1String("hiddenFiles"))
                  || state.view.toJson().contains(QLatin1String("hiddenFiles")),
              QStringLiteral("поле hiddenFiles в JSON"));
        // Показ полного лога: фильтр по файлу приостановлен и снимается им.
        CHECK(tab->toggleFullLog() && settled(*tab) && model.rowCount() == all && model.hiddenFiles().isEmpty(),
              QStringLiteral("полный лог показывает и b"));
        pausedState = throughJson(tab->captureState());
        CHECK(pausedState.view.hiddenSources == QVector<int>{1},
              QStringLiteral("снимок во время полного лога — приостановленный фильтр"));
        CHECK(tab->toggleFullLog() && settled(*tab) && model.hiddenFiles() == QSet<QString>{b}
                  && model.rowCount() == rowsA,
              QStringLiteral("возврат из полного лога скрывает b снова"));
    }
    auto tab = makeTab(schema);
    tab->applyState(state);
    CHECK(settled(*tab), QStringLiteral("применение"));
    LogModel& model = *tab->model();
    CHECK(model.hiddenFiles() == QSet<QString>{b} && model.rowCount() == g_rowsA,
          QStringLiteral("применённый снимок скрывает b: %1 строк").arg(model.rowCount()));
    for (int r = 0; r < model.rowCount(); r += 7)
        CHECK(model.keyAt(r).file && model.keyAt(r).file->filePath == a, QStringLiteral("строки только из a"));

    // Видимый файл пропал: скрытый — единственный открывшийся, вкладка не
    // пустеет (легенды у одного файла нет, вернуть было бы нечем).
    {
        TabState missing = state;
        missing.sources[0].source.path = dir.filePath(QStringLiteral("%1-hidden-gone.log").arg(name));
        if (missing.view.currentSource)
            missing.view.currentSource->path = missing.sources[0].source.path;
        auto alone = makeTab(schema);
        alone->applyState(missing);
        CHECK(settled(*alone) && alone->model()->hiddenFiles().isEmpty() && alone->model()->rowCount() > 0,
              QStringLiteral("видимый пропал — скрытый показан: %1 строк, скрыто %2")
                  .arg(alone->model()->rowCount()).arg(alone->model()->hiddenFiles().size()));
    }
    // Предложение (память файла): фильтр по файлу не предлагается — плашки
    // «Restore Filters», которая ничего не вернёт, нет.
    {
        TabState onlyFiles = state;
        onlyFiles.view.levels.clear();
        onlyFiles.view.timeFrom = onlyFiles.view.timeTo = QDateTime();
        onlyFiles.view.textRules = FilterRuleSet();
        auto offered = makeTab(schema);
        offered->applyState(onlyFiles, LogViewWidget::StateFilters::Offer);
        CHECK(settled(*offered) && !offered->hasOfferedFilters() && offered->model()->hiddenFiles().isEmpty(),
              QStringLiteral("только фильтр по файлу — не предлагается"));
    }

    // Без поля (снимок версии 1 прежних версий) — ничего не скрыто.
    QJsonObject json = state.toJson();
    QJsonObject view = json.value(QLatin1String("view")).toObject();
    view.remove(QLatin1String("hiddenFiles"));
    json.insert(QLatin1String("view"), view);
    const auto old = TabState::fromJson(json);
    CHECK(old && old->view.hiddenSources.isEmpty() && !old->view.hasFilters(),
          QStringLiteral("снимок без hiddenFiles читается как «ничего не скрыто»"));
    QJsonObject bad = state.toJson();
    QJsonObject badView = bad.value(QLatin1String("view")).toObject();
    badView.insert(QLatin1String("hiddenFiles"), QStringLiteral("b"));
    bad.insert(QLatin1String("view"), badView);
    CHECK(!TabState::fromJson(bad), QStringLiteral("битое hiddenFiles — отказ"));
}

static void checkStdin(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("спул stdin");
    const QString spool = testdocs::streamSpoolPath(dir);
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
    // «schema» с F-5 (PR 3) — схема файла (TabSource.schema), не чужой ключ.
    extra.insert("fromFutureVersion", "из будущей версии");
    CHECK(FileMemory::fromJson(extra).has_value(), QStringLiteral("лишние ключи не мешают"));
}

// Записи памяти — по одной и в потоке записи (Q-21). Ожидание записей не
// должно выполнять ещё не начатую запись в ждущем потоке: тогда следующую
// взял бы освободившийся поток записи, две записи одного файла шли бы
// одновременно, и прежняя ложилась на диск после новой (под нагрузкой TSan
// так воскресала снятая в приёмнике закладка, «объединение вкладок»).
// Поток записи занят задачей мимо очереди, пока не начнётся первая запись;
// первая ждёт начала второй. Оба ожидания — с запасом (1 с): с исправлением
// они истекают (первая стоит за занятой, вторая — за первой), без него
// кончаются сразу, и запас нужен лишь медленному потоку под нагрузкой.
static void checkWriteOrder(const QDir& dir)
{
    g_context = QStringLiteral("память файла: порядок записей");
    FileMemoryStore store(dir.filePath(QStringLiteral("write-order")));
    QThread* const waiting = QThread::currentThread();
    std::atomic<bool> firstStarted{ false };
    std::atomic<bool> secondStarted{ false };
    std::atomic<int> active{ 0 };
    std::atomic<int> maxActive{ 0 };
    std::atomic<int> onWaitingThread{ 0 };
    const auto enter = [&] {
        const int now = ++active;
        int seen = maxActive.load();
        while (now > seen && !maxActive.compare_exchange_weak(seen, now)) {
        }
        if (QThread::currentThread() == waiting)
            ++onWaitingThread;
    };
    const auto waitUntil = [](const std::atomic<bool>& flag, int ms) {
        QElapsedTimer t;
        t.start();
        while (!flag.load() && t.elapsed() < ms)
            QThread::msleep(1);
    };
    store.writerPoolForTesting().start([&] { waitUntil(firstStarted, 1000); });
    store.enqueueForTesting([&] {
        enter();
        firstStarted = true;
        waitUntil(secondStarted, 1000);
        --active;
    });
    store.enqueueForTesting([&] {
        enter();
        secondStarted = true;
        --active;
    });
    store.waitForWrites();
    CHECK(maxActive.load() == 1,
          QStringLiteral("записи не идут одновременно (сразу шло %1)").arg(maxActive.load()));
    CHECK(onWaitingThread.load() == 0, QStringLiteral("запись не выполняется в ждущем потоке"));
    CHECK(firstStarted.load() && secondStarted.load(), QStringLiteral("обе записи выполнены"));
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
        const QVector<qint64> mergedLines = linesOf(FileMemoryStore(memDir).open(merged).memory);
        QStringList shownLines;
        for (const qint64 line : mergedLines)
            shownLines << QString::number(line);
        CHECK(mergedLines == (QVector<qint64>{ 9 }),
              QStringLiteral("снятая в приёмнике закладка не вернулась (в памяти: %1)")
                  .arg(shownLines.join(QStringLiteral(", "))));
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

// ---------------------------------------------------------------------------
// Схема на вкладку (F-5, PR 1)
// ---------------------------------------------------------------------------

static const QString kPlainSchema = QStringLiteral(
    "{\"blocks\":[{\"name\":\"Msg\",\"type\":\"rest\"}],\"version\":1}");

static void checkSchemasFormat()
{
    g_context = QStringLiteral("сессия: схема на вкладку, формат");
    SessionState golden = goldenSession();
    golden.tabs[0].view.fields = FieldSettings{ QStringLiteral("Spring Boot"), kGoldenSchema, true,
                                                { QStringLiteral("Level") } };
    golden.tabs[1].view.fields = FieldSettings{ QStringLiteral("Plain"), kPlainSchema, false, {} };
    // Верхний уровень — схема активной вкладки (вторая).
    golden.schemaName = QStringLiteral("Plain");
    golden.schema = kPlainSchema;
    golden.fieldExtraction = false;
    golden.visibleFields.clear();
    const QJsonObject json = golden.toJson(goldenSessionPath());

    QFile file(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/session-schemas.json"));
    CHECK(file.open(QIODevice::ReadOnly), QStringLiteral("эталон открывается"));
    QByteArray bytes = file.readAll();
    bytes.replace("@ROOT@", root().toUtf8());
    const QJsonObject stored = bytesToObject(bytes);
    CHECK(json == stored, QStringLiteral("сессия со схемами вкладок пишется ровно в эталон"));
    QString error;
    const auto parsed = SessionState::fromJson(stored, &error);
    CHECK(parsed.has_value(), QStringLiteral("эталон читается: ") + error);
    if (parsed) {
        CHECK(parsed->tabs.size() == 2 && parsed->tabs[0].view.fields == golden.tabs[0].view.fields
                  && parsed->tabs[1].view.fields == golden.tabs[1].view.fields,
              QStringLiteral("схема каждой вкладки"));
        CHECK(parsed->toJson(goldenSessionPath()) == stored, QStringLiteral("круговой проход — тот же JSON"));
    }

    // Эталоны версии 1 — без схем вкладок: их получает схема верхнего уровня
    // (SessionController::open, проверка на живых вкладках ниже).
    QFile v1(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/session-v1.json"));
    CHECK(v1.open(QIODevice::ReadOnly), QStringLiteral("эталон версии 1"));
    QByteArray v1Bytes = v1.readAll();
    v1Bytes.replace("@ROOT@", root().toUtf8());
    const auto old = SessionState::fromJson(bytesToObject(v1Bytes), &error);
    CHECK(old && old->tabs.size() == 2 && !old->tabs[0].view.fields && !old->tabs[1].view.fields,
          QStringLiteral("вкладки версии 1 без схемы"));
    if (old) {
        const FieldSettings top = old->topLevelFields();
        CHECK(top.schema == kGoldenSchema && top.schemaName == QStringLiteral("Spring Boot") && top.extraction
                  && top.visibleFields == (QStringList{ QStringLiteral("Level"), QStringLiteral("Message") }),
              QStringLiteral("схема верхнего уровня"));
    }

    // ViewState.fields: круговой проход и отказ на битом.
    g_context = QStringLiteral("ViewState.fields");
    for (const FieldSettings& f : { FieldSettings{ QStringLiteral("A"), kGoldenSchema, true, { QStringLiteral("Time") } },
                                    FieldSettings{ QString(), QString(), true, {} },
                                    FieldSettings{ QString(), QStringLiteral("not json"), false, {} } }) {
        ViewState v;
        v.fields = f;
        const QJsonObject vj = v.toJson();
        const auto back = ViewState::fromJson(bytesToObject(objectToBytes(vj)), &error);
        CHECK(back && back->fields == f, QStringLiteral("круговой проход fields: ") + error);
        CHECK(f.hasSchema() == vj.value("fields").toObject().contains("schema"),
              QStringLiteral("нет схемы — нет ключа schema"));
    }
    CHECK(!ViewState::fromJson(ViewState().toJson()).value_or(ViewState()).fields,
          QStringLiteral("без поля — nullopt"));
    const auto refusedView = [&](const QJsonValue& fields, const QString& what) {
        QJsonObject vj = ViewState().toJson();
        vj.insert(QStringLiteral("fields"), fields);
        CHECK(!ViewState::fromJson(vj), QStringLiteral("отказ: ") + what);
    };
    refusedView(5, QStringLiteral("fields не объект"));
    refusedView(QJsonObject{ { "schema", 3 } }, QStringLiteral("schema не объект"));
    refusedView(QJsonObject{ { "schema", QJsonObject{ { "name", "x" } } } }, QStringLiteral("schema без определения"));
    refusedView(QJsonObject{ { "extraction", "yes" } }, QStringLiteral("extraction не bool"));
    refusedView(QJsonObject{ { "visibleFields", QJsonArray{ 1 } } }, QStringLiteral("visibleFields не строки"));
}


// Схема файла слитой вкладки (F-5, PR 3): TabSource.schema — необязательное
// поле, версия формата не растёт. Эталон session-file-schemas.json: у первой
// вкладки второй файл со своей схемой и первый — без схемы (null).
static SessionState fileSchemasSession()
{
    SessionState golden = goldenSession();
    golden.tabs[0].view.fields = FieldSettings{ QStringLiteral("Spring Boot"), kGoldenSchema, true,
                                                { QStringLiteral("Level") } };
    golden.tabs[0].sources[0].schema = FieldSettings();
    golden.tabs[0].sources[1].schema = FieldSettings{ QStringLiteral("Plain"), kPlainSchema, false, {} };
    return golden;
}

static void checkFileSchemasFormat()
{
    g_context = QStringLiteral("сессия: схема файла, формат");
    const SessionState golden = fileSchemasSession();
    const QJsonObject json = golden.toJson(goldenSessionPath());
    if (qEnvironmentVariableIsSet("DOCUMENT_STATE_PRINT_FILE_SCHEMAS")) {
        QByteArray out = QJsonDocument(json).toJson(QJsonDocument::Indented);
        if (!root().isEmpty())
            out.replace(root().toUtf8(), "@ROOT@");
        std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    }
    QFile file(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/session-file-schemas.json"));
    CHECK(file.open(QIODevice::ReadOnly), QStringLiteral("эталон открывается"));
    QByteArray bytes = file.readAll();
    bytes.replace("@ROOT@", root().toUtf8());
    const QJsonObject stored = bytesToObject(bytes);
    CHECK(json == stored, QStringLiteral("сессия со схемами файлов пишется ровно в эталон"));
    QString error;
    const auto parsed = SessionState::fromJson(stored, &error);
    CHECK(parsed.has_value(), QStringLiteral("эталон читается: ") + error);
    if (parsed) {
        const TabState& tab = parsed->tabs.value(0);
        CHECK(tab.sources.size() == 2 && tab.sources[0].schema && !tab.sources[0].schema->hasSchema()
                  && tab.sources[1].schema
                  && tab.sources[1].schema->schema == kPlainSchema
                  && tab.sources[1].schema->schemaName == QStringLiteral("Plain"),
              QStringLiteral("схемы файлов: null — без схемы, объект — своя"));
        CHECK(!parsed->tabs.value(1).sources.value(0).schema,
              QStringLiteral("файл без ключа — схема вкладки"));
        CHECK(parsed->toJson(goldenSessionPath()) == stored, QStringLiteral("круговой проход — тот же JSON"));
    }
    // Прежние эталоны: схемы файлов нет ни у одного источника.
    for (const char* name : { "session-v1.json", "session-schemas.json" }) {
        QFile old(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/") + QLatin1String(name));
        CHECK(old.open(QIODevice::ReadOnly), QStringLiteral("эталон ") + QLatin1String(name));
        QByteArray oldBytes = old.readAll();
        oldBytes.replace("@ROOT@", root().toUtf8());
        const auto o = SessionState::fromJson(bytesToObject(oldBytes), &error);
        bool none = o.has_value();
        if (o)
            for (const TabState& t : o->tabs)
                for (const TabSource& src : t.sources)
                    none = none && !src.schema;
        CHECK(none, QStringLiteral("прежний эталон без схем файлов: ") + QLatin1String(name));
    }
    // Отказ на битом поле.
    TabSource src = golden.tabs[0].sources[1];
    for (const QJsonValue& bad : { QJsonValue(5), QJsonValue(QJsonObject{ { "name", "x" } }),
                                   QJsonValue(QJsonObject{ { "definition", "{}" } }) }) {
        QJsonObject sj = src.toJson();
        sj.insert(QStringLiteral("schema"), bad);
        CHECK(!TabSource::fromJson(sj), QStringLiteral("отказ: битая схема файла"));
    }
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
    const QString spool = testdocs::streamSpoolPath(dir);
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

// ---- Последняя сессия (R-5, шаг 2) -------------------------------------------------

struct Window {
    TabPool pool;
    SessionController controller;
    explicit Window(const QString& schema) : pool{ schema, {} }
    {
        controller.setWindow({
            [this]() {
                QList<LogViewWidget*> tabs;
                for (const auto& t : pool.tabs)
                    if (t)
                        tabs << t.get();
                return tabs;
            },
            [this]() { return pool.tabs.empty() ? nullptr : pool.tabs.front().get(); },
            [this]() {
                SessionController::WindowSettings w;
                w.schemaName = QStringLiteral("window schema");
                w.schema = pool.schema;
                w.fieldExtraction = true;
                w.visibleFields = { QStringLiteral("Level") };
                return w;
            } });
    }
    LogViewWidget* openFile(const QString& path)
    {
        LogViewWidget* tab = pool.make();
        tab->addLogFile(path);
        CHECK(settled(*tab), QStringLiteral("загрузка ") + path);
        return tab;
    }
};

static QStringList tabFiles(const std::vector<std::unique_ptr<LogViewWidget>>& tabs)
{
    QStringList out;
    for (const auto& t : tabs) {
        QStringList names;
        for (const LogFilePtr& f : t->loadedFiles())
            names << QFileInfo(f->filePath).fileName();
        out << names.join(QLatin1Char('+'));
    }
    return out;
}

static void checkLastSession(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("последняя сессия");
    CHECK(!AppSettings::instance().restoreSessionOnStartup(), QStringLiteral("настройка по умолчанию выключена"));
    QDir().mkpath(dir.filePath(QStringLiteral("config")));
    const QString last = dir.filePath(QStringLiteral("config/last-session.json"));
    const QString one = writeFile(dir, QStringLiteral("last-one.log"), logBytes(200));
    const QString two = writeFile(dir, QStringLiteral("last-two.log"), logBytes(100, 3, "two"));
    const QString cmd = writeFile(dir, QStringLiteral("last-cmd.log"), logBytes(50, 7, "cmd"));

    // ---- Выход: пустое окно ничего не пишет ------------------------------------------
    {
        Window w(schema);
        w.controller.setLastSessionFile(last, true);
        CHECK(!w.controller.canRestoreLastSession(), QStringLiteral("файла нет — восстанавливать нечего"));
        CHECK(w.controller.saveLastSession() == SessionController::SaveOutcome::KeptEmpty && !QFile::exists(last),
              QStringLiteral("пустое окно файл не создаёт"));
    }
    // ---- Выход: две вкладки, закладка, фильтр ---------------------------------------
    {
        Window w(schema);
        w.controller.setLastSessionFile(last, true);
        LogViewWidget* t1 = w.openFile(one);
        t1->bookmarks().add(rowOfLine(*t1->model(), one, 30), QStringLiteral("тридцать"));
        FilterRule rule;
        rule.action = FilterRule::Action::Exclude;
        rule.text = QStringLiteral("line 13");
        FilterRuleSet rules;
        rules.rules = { rule };
        rules.bindFields(t1->parserFieldNames(), false);
        t1->model()->setFilters({}, QDateTime(), QDateTime(), rules);
        CHECK(settled(*t1), QStringLiteral("фильтр"));
        w.openFile(two);
        QString error;
        CHECK(w.controller.saveLastSession(&error) == SessionController::SaveOutcome::Saved,
              QStringLiteral("выход пишет сессию: ") + error);
        const SessionFile::LoadResult loaded = SessionFile::load(last);
        CHECK(loaded.state && loaded.state->tabs.size() == 2 && loaded.state->activeTab == 0
                  && loaded.state->schema == schema && loaded.state->schemaName == QStringLiteral("window schema")
                  && loaded.state->fieldExtraction
                  && loaded.state->visibleFields == QStringList{ QStringLiteral("Level") },
              QStringLiteral("в файле — вкладки, активная, схема и колонки"));
        CHECK(loaded.state && loaded.state->tabs[0].bookmarks.size() == 1
                  && loaded.state->tabs[0].view.textRules.isActive(),
              QStringLiteral("в файле — закладка и фильтр первой вкладки"));
    }
    const QByteArray saved = readBytes(last);

    // ---- Пустое окно и окно только со stdin не затирают ------------------------------
    {
        Window w(schema);
        w.controller.setLastSessionFile(last, true);
        CHECK(w.controller.canRestoreLastSession(), QStringLiteral("есть что восстанавливать"));
        CHECK(w.controller.saveLastSession() == SessionController::SaveOutcome::KeptEmpty && readBytes(last) == saved,
              QStringLiteral("пустое окно прошлую сессию не затирает"));
        const QString spool = testdocs::streamSpoolPath(dir);
        CHECK(writeBytes(spool, logBytes(20, 0, "piped")), QStringLiteral("спул"));
        w.openFile(spool);
        CHECK(w.controller.saveLastSession() == SessionController::SaveOutcome::KeptEmpty && readBytes(last) == saved,
              QStringLiteral("окно только со stdin прошлую сессию не затирает"));
        w.pool.tabs.clear();
        QFile::remove(spool);
    }

    // ---- Вторичный экземпляр: не читает и не пишет -------------------------------------
    {
        Window w(schema);
        w.controller.setLastSessionFile(last, false);
        WelcomeWidget welcome;
        QObject::connect(&w.controller, &SessionController::lastSessionAvailabilityChanged,
                         &welcome, &WelcomeWidget::setRestoreSessionAvailable);
        welcome.setRestoreSessionAvailable(w.controller.canRestoreLastSession());
        auto* link = welcome.findChild<QLabel*>(QStringLiteral("restoreSessionLink"));
        CHECK(link && link->isHidden() && !w.controller.canRestoreLastSession(),
              QStringLiteral("вторичный: ссылки нет"));
        bool restored = false, commandLine = false;
        w.controller.startUp(true, [&](const SessionState&, const QString&) { restored = true; },
                             [&]() { commandLine = true; w.openFile(cmd); });
        CHECK(!restored && commandLine, QStringLiteral("вторичный: при включённой настройке не восстанавливает"));
        CHECK(!w.controller.takeLastSession().has_value(), QStringLiteral("вторичный: не читает"));
        CHECK(w.controller.saveLastSession() == SessionController::SaveOutcome::NotOwner && readBytes(last) == saved,
              QStringLiteral("вторичный: не пишет"));
        // Кто первичный (держит ключ) — SingleInstance, таблица в tests/open_routing.
    }

    // ---- Запуск, настройка выключена: ссылка появляется и пропадает --------------------
    {
        Window w(schema);
        WelcomeWidget welcome;
        QObject::connect(&w.controller, &SessionController::lastSessionAvailabilityChanged,
                         &welcome, &WelcomeWidget::setRestoreSessionAvailable);
        auto* link = welcome.findChild<QLabel*>(QStringLiteral("restoreSessionLink"));
        CHECK(link && link->isHidden(), QStringLiteral("ссылки нет до запуска"));
        w.controller.setLastSessionFile(last, true);
        bool restored = false;
        w.controller.startUp(false, [&](const SessionState&, const QString&) { restored = true; },
                             [&]() { w.openFile(cmd); });
        CHECK(!restored && w.pool.tabs.size() == 1, QStringLiteral("выключена: не восстанавливает, командная строка открыта"));
        CHECK(link && !link->isHidden() && welcome.isRestoreSessionAvailable(),
              QStringLiteral("выключена: ссылка на стартовом экране"));
        // Файлы второго экземпляра — мимо startUp: восстановления нет, ссылка остаётся.
        w.openFile(two);
        CHECK(w.pool.tabs.size() == 2 && w.controller.canRestoreLastSession() && link && !link->isHidden(),
              QStringLiteral("файлы второго экземпляра восстановления не запускают"));
        // Щелчок по ссылке: сессия читается, ссылка пропадает.
        int clicked = 0;
        QObject::connect(&welcome, &WelcomeWidget::restoreSessionRequested, [&]() { ++clicked; });
        if (link)
            emit link->linkActivated(QStringLiteral("restore"));
        CHECK(clicked == 1, QStringLiteral("ссылка просит восстановить"));
        const auto session = w.controller.takeLastSession();
        CHECK(session && session->tabs.size() == 2, QStringLiteral("прочитано две вкладки"));
        CHECK(link && link->isHidden() && !w.controller.canRestoreLastSession(),
              QStringLiteral("после восстановления ссылка пропала"));
        w.pool.tabs.clear();
    }

    // ---- Запуск, настройка включена: прошлые вкладки, затем командная строка ----------
    for (const bool indexed : { false, true }) {
        g_context = QStringLiteral("последняя сессия: запуск (%1)").arg(indexed ? "indexed" : "resident");
        AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
        Window w(schema);
        WelcomeWidget welcome;
        QObject::connect(&w.controller, &SessionController::lastSessionAvailabilityChanged,
                         &welcome, &WelcomeWidget::setRestoreSessionAvailable);
        w.controller.setLastSessionFile(last, true);
        SessionController::OpenResult opened;
        w.controller.startUp(true,
            [&](const SessionState& session, const QString& path) {
                opened = w.controller.open(session, path, [&w]() { return w.pool.make(); });
            },
            [&]() { w.openFile(cmd); });
        CHECK(tabFiles(w.pool.tabs) == (QStringList{ QStringLiteral("last-one.log"), QStringLiteral("last-two.log"),
                                                    QStringLiteral("last-cmd.log") }),
              QStringLiteral("порядок: восстановленные, затем командная строка: ") + tabFiles(w.pool.tabs).join(", "));
        CHECK(!opened.hasProblems() && opened.tabs.size() == 2, QStringLiteral("восстановлено без проблем"));
        auto* link = welcome.findChild<QLabel*>(QStringLiteral("restoreSessionLink"));
        CHECK(link && link->isHidden() && !w.controller.canRestoreLastSession(), QStringLiteral("ссылки нет"));
        if (w.pool.tabs.size() == 3) {
            LogViewWidget& t1 = *w.pool.tabs[0];
            CHECK(settled(t1), QStringLiteral("загрузка"));
            const QVector<Bookmark> list = t1.bookmarks().bookmarks();
            CHECK(list.size() == 1 && list[0].note == QStringLiteral("тридцать")
                      && t1.bookmarks().visibleRow(list[0].id) == rowOfLine(*t1.model(), one, 30),
                  QStringLiteral("закладка на своей строке"));
            CHECK(t1.model()->filterRules().isActive() && rowOfLine(*t1.model(), one, 15) < 0,
                  QStringLiteral("фильтр действует"));
            CHECK(t1.model()->isIndexedBackend() == indexed, QStringLiteral("бэкенд"));
        }
        w.pool.tabs.clear();
    }
    AppSettings::instance().setIndexedThresholdMB(512);

    // ---- Файл новой версии ---------------------------------------------------------
    g_context = QStringLiteral("последняя сессия: новая версия");
    {
        QJsonObject json = bytesToObject(saved);
        json.insert(QStringLiteral("version"), 99);
        const QByteArray newer = objectToBytes(json);
        CHECK(writeBytes(last, newer), QStringLiteral("запись новой версии"));
        Window w(schema);
        w.controller.setLastSessionFile(last, true);
        CHECK(!w.controller.canRestoreLastSession(), QStringLiteral("новую версию не предлагает"));
        w.openFile(one);
        CHECK(w.controller.saveLastSession() == SessionController::SaveOutcome::KeptNewer && readBytes(last) == newer,
              QStringLiteral("новую версию не перезаписывает"));
        w.pool.tabs.clear();
    }
}

// ---------------------------------------------------------------------------
// Сессия с другой ОС (R-8)
// ---------------------------------------------------------------------------
//
// Эталоны session-windows.json и session-posix.json — сессия, сохранённая на
// Windows и на Linux: две вкладки (одна слитая), закладки с заметками,
// фильтры, маркер, кодировка, позиция. Абсолютные пути в них — чужие для
// одной из платформ CI, относительные ведут из «sessions/» в «../логи и
// данные/». Логи сценария детерминированы (logBytes): эталоны хранят их
// отпечатки. Поменялись логи или формат — эталоны пересобрать:
//   DOCUMENT_STATE_WRITE_CROSS_OS=<каталог> document_state
// (пишет оба файла в каталог и выходит).

static const QString kCrossLogs = QStringLiteral("логи и данные");
static const QString kCrossSession = QStringLiteral("инцидент 1.dlsession");

struct CrossLogs {
    QString one, ma, mb;
};

static CrossLogs writeCrossLogs(const QString& base, bool withOne)
{
    const QDir logs(base + QLatin1Char('/') + kCrossLogs);
    QDir().mkpath(logs.path());
    CrossLogs out;
    out.one = logs.filePath(QStringLiteral("one.log"));
    if (withOne)
        writeFile(logs, QStringLiteral("one.log"), logBytes(400));
    out.ma = writeFile(logs, QStringLiteral("merged-a.log"), logBytes(200));
    out.mb = writeFile(logs, QStringLiteral("merged-b.log"), logBytes(200, 1, "bee"));
    return out;
}

// Путь лога сценария на «той» машине: в источниках вкладок и таблице files
// (sourceForm) — как его записала бы Windows-версия с разными написаниями, в
// закладках и текущей строке — с прямыми слэшами (нормализуются в одно).
// Сервер UNC — адрес, а не имя: на Windows CI путь родной и проверяется с
// диска, разрешение имени по сети заняло бы секунды.
static QString foreignPath(const QString& style, const QString& fileName, bool sourceForm)
{
    if (style == QLatin1String("posix"))
        return QStringLiteral("/home/anna/Инцидент/логи и данные/") + fileName;
    QString path;
    if (fileName == QLatin1String("merged-b.log"))
        path = QStringLiteral("//127.0.0.1/incident share/Инцидент/логи и данные/") + fileName;
    else
        path = QStringLiteral("C:/Users/Анна/Инцидент/логи и данные/") + fileName;
    if (!sourceForm)
        return path;
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (fileName == QLatin1String("merged-a.log"))
        path.prepend(QStringLiteral("\\\\?\\"));
    return path;
}

static QJsonObject withForeignPaths(QJsonObject session, const QString& style)
{
    const auto mapped = [&style](const QString& local, bool sourceForm) {
        return foreignPath(style, QFileInfo(local).fileName(), sourceForm);
    };
    const auto mapSource = [&](QJsonObject holder, bool sourceForm) {
        QJsonObject source = holder.value("source").toObject();
        source.insert("path", mapped(source.value("path").toString(), sourceForm));
        holder.insert("source", source);
        return holder;
    };
    QJsonArray files = session.value("files").toArray();
    for (int i = 0; i < files.size(); ++i) {
        QJsonObject f = files[i].toObject();
        f.insert("path", mapped(f.value("path").toString(), true));
        files.replace(i, f);
    }
    session.insert("files", files);
    QJsonArray tabs = session.value("tabs").toArray();
    for (int t = 0; t < tabs.size(); ++t) {
        QJsonObject tab = tabs[t].toObject();
        QJsonArray sources = tab.value("sources").toArray();
        for (int i = 0; i < sources.size(); ++i)
            sources.replace(i, mapSource(sources[i].toObject(), true));
        tab.insert("sources", sources);
        QJsonArray marks = tab.value("bookmarks").toArray();
        for (int i = 0; i < marks.size(); ++i)
            marks.replace(i, mapSource(marks[i].toObject(), false));
        tab.insert("bookmarks", marks);
        QJsonObject view = tab.value("view").toObject();
        if (view.contains("current"))
            view.insert("current", mapSource(view.value("current").toObject(), false));
        tab.insert("view", view);
        tabs.replace(t, tab);
    }
    session.insert("tabs", tabs);
    return session;
}

static int writeCrossOsGoldens(const QString& outDir, const QDir& dir, const QString& schema)
{
    const QString base = dir.filePath(QStringLiteral("cross-os-golden"));
    const CrossLogs logs = writeCrossLogs(base, true);
    QDir().mkpath(base + QStringLiteral("/sessions"));
    const QString sessionPath = base + QStringLiteral("/sessions/") + kCrossSession;
    const Saved saved = prepareSource(logs.one, schema, true);
    auto tab1 = makeTab(schema);
    tab1->applyState(saved.state);
    auto tab2 = makeTab(schema);
    tab2->addLogFile(logs.ma);
    tab2->addLogFile(logs.mb);
    if (!settled(*tab1) || !settled(*tab2))
        return 2;
    LogModel& m2 = *tab2->model();
    tab2->bookmarks().add(rowOfLine(m2, logs.ma, 10), QStringLiteral("a10"));
    tab2->bookmarks().add(rowOfLine(m2, logs.mb, 20), QStringLiteral("b20"));
    tab2->view()->goToRow(rowOfLine(m2, logs.mb, 100));
    pump(20);
    SessionController controller;
    SessionState session = controller.capture({ tab1.get(), tab2.get() }, tab2.get());
    session.schemaName = QStringLiteral("test schema");
    session.schema = schema;
    if (!SessionFile::save(sessionPath, session))
        return 2;
    const QJsonObject local = bytesToObject(readBytes(sessionPath));
    for (const QString& style : { QStringLiteral("windows"), QStringLiteral("posix") }) {
        QFile out(outDir + QStringLiteral("/session-") + style + QStringLiteral(".json"));
        if (!out.open(QIODevice::WriteOnly))
            return 2;
        out.write(objectToBytes(withForeignPaths(local, style)));
    }
    std::fprintf(stdout, "эталоны R-8 записаны в %s\n", U8(outDir));
    return 0;
}

static void checkCrossOsSession(const QDir& dir, const QString& name, const QString& schema,
                                const QString& style)
{
    g_context = name + QStringLiteral(" / сессия с другой ОС (") + style + QStringLiteral("): логи рядом");
    const QByteArray golden = readBytes(QStringLiteral(DOCUMENT_STATE_TEST_DIR "/session-") + style
                                        + QStringLiteral(".json"));
    CHECK(!golden.isEmpty(), QStringLiteral("эталон есть"));

    // ---- Логи лежат рядом с сессией по относительному пути ------------------
    {
        const QString base = dir.filePath(name + QStringLiteral("-cross-") + style);
        const CrossLogs logs = writeCrossLogs(base, true);
        QDir().mkpath(base + QStringLiteral("/sessions"));
        const QString sessionPath = base + QStringLiteral("/sessions/") + kCrossSession;
        CHECK(writeBytes(sessionPath, golden), QStringLiteral("файл сессии"));
        const SessionFile::LoadResult loaded = SessionFile::load(sessionPath);
        CHECK(loaded.state.has_value(), QStringLiteral("читается: ") + loaded.error);
        if (!loaded.state)
            return;
        TabPool pool{ schema, {} };
        SessionController controller;
        const auto result = controller.open(*loaded.state, sessionPath, [&pool]() { return pool.make(); });
        CHECK(result.tabs.size() == 2 && !result.hasProblems() && result.relocated == 3,
              QStringLiteral("две вкладки, три файла найдены по относительному пути (%1 / %2)")
                  .arg(result.relocated).arg(result.missing));
        CHECK(result.active == result.tabs.value(1), QStringLiteral("активная вкладка — вторая"));
        const auto report = controller.report();
        CHECK(report.size() == 3, QStringLiteral("отчёт: три записи"));
        for (const auto& e : report)
            CHECK(e.kind == SessionController::ReportEntry::Kind::Relocated
                      && SavedPath::isAbsolute(e.savedPath)
                      && e.savedPath == SavedPath::normalized(foreignPath(style, QFileInfo(e.path).fileName(), true)),
                  QStringLiteral("отчёт: «%1» найден рядом с сессией").arg(e.savedPath));
        if (result.tabs.size() != 2)
            return;

        LogViewWidget& t1 = *result.tabs[0];
        CHECK(settled(t1), QStringLiteral("вкладка 1: загрузка"));
        CHECK(t1.fileCount() == 1 && t1.loadedFiles().first()->filePath == logs.one,
              QStringLiteral("вкладка 1: файл рядом с сессией"));
        Saved expected;
        expected.path = logs.one;
        expected.currentLine = 170;
        expected.currentTop = loaded.state->tabs[0].view.viewportOffset;
        for (const qint64 line : { qint64(2), qint64(40), qint64(120), qint64(200) })
            expected.notes.insert(line, QStringLiteral("заметка к %1").arg(line));
        checkBookmarksOnLines(t1, expected, QStringLiteral("вкладка 1: закладки с заметками"));
        LogModel& m1 = *t1.model();
        const ViewState& v1 = loaded.state->tabs[0].view;
        CHECK(m1.logLevelFilter() == v1.levels && v1.textRules.isActive()
                  && m1.filterRules().toJson() == v1.textRules.toJson()
                  && highlightPatternsToJson(m1.rowMarkers()) == highlightPatternsToJson(v1.markers)
                  && !v1.markers.isEmpty(),
              QStringLiteral("вкладка 1: фильтры и маркер"));
        checkPositionRestored(t1, expected, QStringLiteral("вкладка 1: строка и место на экране"));
        const FileEncoding enc = t1.fileEncoding(t1.loadedFiles().first());
        CHECK(enc.source == FileEncoding::Source::User && enc.decoder.encoding() == TextEncoding::Windows1251,
              QStringLiteral("вкладка 1: выбор кодировки"));
        CHECK(m1.isIndexedBackend() == (name == QLatin1String("indexed")), QStringLiteral("бэкенд"));

        LogViewWidget& t2 = *result.tabs[1];
        CHECK(settled(t2), QStringLiteral("вкладка 2: загрузка"));
        LogModel& m2 = *t2.model();
        CHECK(t2.fileCount() == 2, QStringLiteral("вкладка 2: оба файла (с \\\\?\\ и UNC)"));
        int found = 0;
        for (const Bookmark& b : t2.bookmarks().bookmarks()) {
            const QString path = b.note == QStringLiteral("a10") ? logs.ma : logs.mb;
            CHECK(b.filePath == path && t2.bookmarks().status(b.id) == BookmarkStore::Status::Visible
                      && t2.bookmarks().idAtRow(rowOfLine(m2, path, b.line)) == b.id,
                  QStringLiteral("вкладка 2: закладка «%1» на своей строке").arg(b.note));
            ++found;
        }
        CHECK(found == 2, QStringLiteral("вкладка 2: обе закладки"));
        const int row = rowOfLine(m2, logs.mb, 100);
        CHECK(row >= 0 && t2.view()->userSelectedRow() == row,
              QStringLiteral("вкладка 2: текущая строка %1").arg(currentLine(t2)));
    }

    // ---- Одного лога рядом нет ---------------------------------------------
    g_context = name + QStringLiteral(" / сессия с другой ОС (") + style + QStringLiteral("): лога нет");
    const QString base = dir.filePath(name + QStringLiteral("-cross-missing-") + style);
    const CrossLogs logs = writeCrossLogs(base, false);
    QDir().mkpath(base + QStringLiteral("/sessions"));
    const QString sessionPath = base + QStringLiteral("/sessions/") + kCrossSession;
    CHECK(writeBytes(sessionPath, golden), QStringLiteral("файл сессии"));
    const SessionFile::LoadResult loaded = SessionFile::load(sessionPath);
    CHECK(loaded.state.has_value(), QStringLiteral("читается: ") + loaded.error);
    if (!loaded.state)
        return;
    TabPool pool{ schema, {} };
    SessionController controller;
    const auto result = controller.open(*loaded.state, sessionPath, [&pool]() { return pool.make(); });
    CHECK(result.tabs.size() == 2 && result.missing == 1 && result.changed == 0 && result.relocated == 2,
          QStringLiteral("пропал один, два найдены (%1 / %2)").arg(result.missing).arg(result.relocated));
    const QString savedOne = SavedPath::normalized(foreignPath(style, QStringLiteral("one.log"), true));
    int missingIndex = -1;
    const auto report = controller.report();
    for (int i = 0; i < report.size(); ++i)
        if (report[i].kind == SessionController::ReportEntry::Kind::Missing)
            missingIndex = i;
    CHECK(missingIndex >= 0 && report[missingIndex].savedPath == savedOne
              && report[missingIndex].path == savedOne
              && report[missingIndex].tabTitle.contains(QStringLiteral("one.log")),
          QStringLiteral("отчёт называет пропавший файл исходным путём"));
    if (result.tabs.size() != 2 || missingIndex < 0)
        return;
    LogViewWidget& t1 = *result.tabs[0];
    LogViewWidget& t2 = *result.tabs[1];
    CHECK(settled(t1) && t1.fileCount() == 0 && t1.bookmarks().count() == 4,
          QStringLiteral("вкладка пропавшего — пустая, закладки ждут"));
    for (const Bookmark& b : t1.bookmarks().bookmarks())
        CHECK(t1.bookmarks().status(b.id) == BookmarkStore::Status::Pending,
              QStringLiteral("закладка %1 ждёт файл").arg(b.line));
    CHECK(settled(t2) && t2.fileCount() == 2 && t2.bookmarks().count() == 2,
          QStringLiteral("остальная вкладка открыта"));

    // Пересохранение — в другой каталог: пропавший не теряется, чужой путь не
    // испорчен, относительный ведёт туда же, куда вёл прежний файл сессии.
    const QString resavedPath = base + QStringLiteral("/elsewhere/deeper/resaved.dlsession");
    QDir().mkpath(QFileInfo(resavedPath).absolutePath());
    QString error;
    CHECK(SessionFile::save(resavedPath, controller.capture({ &t1, &t2 }, &t2), &error),
          QStringLiteral("пересохранение: ") + error);
    const QJsonObject resaved = bytesToObject(readBytes(resavedPath));
    const QJsonArray resavedTabs = resaved.value("tabs").toArray();
    const QJsonArray sources1 = resavedTabs.at(0).toObject().value("sources").toArray();
    CHECK(sources1.size() == 1
              && sources1.at(0).toObject().value("source").toObject().value("path").toString() == savedOne,
          QStringLiteral("пропавший источник с исходным путём"));
    CHECK(resavedTabs.at(0).toObject().value("bookmarks").toArray().size() == 4,
          QStringLiteral("закладки пропавшего на месте"));
    QString relative;
    for (const QJsonValue& f : resaved.value("files").toArray())
        if (f.toObject().value("path").toString() == savedOne)
            relative = f.toObject().value("relative").toString();
    CHECK(relative == QStringLiteral("../../") + kCrossLogs + QStringLiteral("/one.log"),
          QStringLiteral("относительный путь пропавшего: «%1»").arg(relative));

    // Закрыта и возвращена (Ctrl+Shift+T) — пересохранение так же ведёт
    // относительный путь туда, куда вёл файл сессии.
    {
        CHECK(controller.rememberClosedTab(&t1, 0), QStringLiteral("вкладка запомнена"));
        TabPool closedPool{ schema, {} };
        const auto reopened = controller.reopenClosedTab([&closedPool](int) { return closedPool.make(); });
        CHECK(reopened.tabs.size() == 1 && reopened.missing == 1, QStringLiteral("вкладка возвращена, файл пропал"));
        if (reopened.tabs.size() == 1) {
            const QString closedPath = base + QStringLiteral("/closed/reopened.dlsession");
            QDir().mkpath(QFileInfo(closedPath).absolutePath());
            CHECK(SessionFile::save(closedPath, controller.capture({ reopened.tabs[0] }, reopened.tabs[0]), &error),
                  QStringLiteral("запись: ") + error);
            QString rel;
            for (const QJsonValue& f : bytesToObject(readBytes(closedPath)).value("files").toArray())
                if (f.toObject().value("path").toString() == savedOne)
                    rel = f.toObject().value("relative").toString();
            CHECK(rel == QStringLiteral("../") + kCrossLogs + QStringLiteral("/one.log"),
                  QStringLiteral("возвращённая вкладка: относительный путь «%1»").arg(rel));
        }
    }

    // Лог появился — пересохранённая сессия его находит.
    writeCrossLogs(base, true);
    const SessionFile::LoadResult again = SessionFile::load(resavedPath);
    CHECK(again.state.has_value(), QStringLiteral("пересохранённая читается: ") + again.error);
    if (again.state) {
        TabPool pool2{ schema, {} };
        SessionController c2;
        const auto r2 = c2.open(*again.state, resavedPath, [&pool2]() { return pool2.make(); });
        CHECK(r2.tabs.size() == 2 && r2.missing == 0 && !r2.hasProblems(),
              QStringLiteral("пересохранённая: все файлы найдены (%1)").arg(r2.missing));
        if (r2.tabs.size() == 2) {
            CHECK(settled(*r2.tabs[0]) && r2.tabs[0]->fileCount() == 1
                      && r2.tabs[0]->loadedFiles().first()->filePath == logs.one,
                  QStringLiteral("пересохранённая: лог найден"));
        }
    }

    // «Указать файл» для пропавшего работает.
    const QString copy = base + QStringLiteral("/копия one.log");
    CHECK(QFile::copy(logs.one, copy), QStringLiteral("копия лога"));
    CHECK(controller.locate(missingIndex, copy, &error), QStringLiteral("указать файл: ") + error);
    CHECK(settled(t1) && t1.fileCount() == 1 && t1.bookmarks().count() == 4,
          QStringLiteral("указанный файл открыт, закладки на нём"));
    for (const Bookmark& b : t1.bookmarks().bookmarks())
        CHECK(t1.bookmarks().status(b.id) == BookmarkStore::Status::Visible
                  || t1.bookmarks().status(b.id) == BookmarkStore::Status::Hidden,
              QStringLiteral("закладка %1 найдена").arg(b.line));

    // Окно отчёта показывает путь так, как он записан на той ОС.
    SessionReportDialog dialog(&controller);
    auto* list = dialog.findChild<QTreeWidget*>(QStringLiteral("sessionReportList"));
    bool shown = false;
    for (int i = 0; list && i < list->topLevelItemCount(); ++i)
        shown = shown || list->topLevelItem(i)->text(1) == SavedPath::toDisplay(savedOne);
    CHECK(shown, QStringLiteral("окно отчёта: «%1»").arg(SavedPath::toDisplay(savedOne)));
}

// Память файла с другой машины (R-8): чужой путь — «не этот файл». Читается,
// не удаляется как битая, по пути не находится, по содержимому — да.
static void checkForeignMemory(const QDir& dir)
{
    g_context = QStringLiteral("память файла с другой ОС");
    const QString memDir = dir.filePath(QStringLiteral("memory-foreign"));
    QDir().mkpath(memDir);
#ifdef Q_OS_WIN
    const QString foreign = QStringLiteral("/home/anna/logs/app.log");
    const QString foreignOther = QStringLiteral("/home/anna/logs/other.log");
#else
    const QString foreign = QStringLiteral("C:/Users/Анна/logs/app.log");
    const QString foreignOther = QStringLiteral("C:/Users/Анна/logs/other.log");
#endif
    const QString local = writeFile(dir, QStringLiteral("foreign-memory.log"), logBytes(100));
    const QString otherLocal = writeFile(dir, QStringLiteral("foreign-memory-other.log"), logBytes(100, 3, "zz"));
    FileMemory same = memoryOf(local, { 5, 9 });
    same.relocate(foreign);
    FileMemory other = memoryOf(otherLocal, { 7 });
    other.relocate(foreignOther);
    const QString sameFile = memoryFileOf(memDir, foreign);
    const QString otherFile = memoryFileOf(memDir, foreignOther);
    CHECK(FileMemoryStore::writeFileAtomically(sameFile, QJsonDocument(same.toJson()).toJson())
              && FileMemoryStore::writeFileAtomically(otherFile, QJsonDocument(other.toJson()).toJson()),
          QStringLiteral("память записана"));
    CHECK(FileMemory::fromJson(bytesToObject(readBytes(sameFile))).has_value(),
          QStringLiteral("память с чужим путём читается"));
    CHECK(FileMemoryStore::normalizedKeyPath(foreign) != FileMemoryStore::normalizedKeyPath(
              QDir::current().absoluteFilePath(foreign)),
          QStringLiteral("чужой путь не совпадает с локальным «<текущий каталог>/<путь>»"));

    {
        FileMemoryStore store(memDir);
        const auto lookup = store.open(local);
        CHECK(lookup.found == FileMemoryStore::Found::ByContent && lookup.memory
                  && linesOf(lookup.memory) == QVector<qint64>({ 5, 9 }) && pathsAre(*lookup.memory, local)
                  && lookup.previousPath == foreign,
              QStringLiteral("то же содержимое — найдена по содержимому, переехала на локальный путь"));
        store.waitForWrites();
        CHECK(QFileInfo::exists(otherFile), QStringLiteral("память другого файла не удалена"));
        const auto none = store.open(writeFile(dir, QStringLiteral("foreign-memory-third.log"), logBytes(50, 9, "q")));
        CHECK(none.found == FileMemoryStore::Found::None, QStringLiteral("чужой файл — не найдена"));
        store.waitForWrites();
    }
    CHECK(QFileInfo::exists(otherFile) && FileMemory::fromJson(bytesToObject(readBytes(otherFile))).has_value(),
          QStringLiteral("память с чужим путём на месте и цела"));
}

// Вкладки со схемой от контроллера (как у окна: configureView).
struct SchemaTabPool {
    FieldSchemaController* controller = nullptr;
    std::vector<std::unique_ptr<LogViewWidget>> tabs;
    LogViewWidget* make()
    {
        tabs.push_back(makeTab(QString()));
        controller->configureView(tabs.back().get());
        return tabs.back().get();
    }
};

static QString columnSchema(const QString& level, const QString& message)
{
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        patternBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Thread"),
                     QStringLiteral("["), QStringLiteral("]")),
        patternBlock(PatternBlock::MatchKind::Level, level, QString(), QString(), QStringLiteral("-")),
        patternBlock(PatternBlock::MatchKind::Remainder, message),
    };
    return LogPattern::serializeDefinition(def);
}

static bool hasFields(LogViewWidget& tab, const QString& schema, const QVector<int>& visible)
{
    return tab.parserPattern() == schema && tab.model()->fieldDisplayFilterEnabled()
        && tab.model()->visibleFieldIndexes() == visible;
}

static void checkSessionSchemas(const QDir& dir, const QString& name)
{
    g_context = name + QStringLiteral(" / сессия: вкладки с разными схемами (F-5)");
    const QString a = columnSchema(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = columnSchema(QStringLiteral("Severity"), QStringLiteral("Text"));
    const QString foreign = columnSchema(QStringLiteral("Lvl"), QStringLiteral("Body"));
    QString error;
    CHECK(SchemaStore::sync({ { QStringLiteral("A"), a, {}, {} }, { QStringLiteral("B"), b, {}, {} } }, &error),
          QStringLiteral("каталог схем: ") + error);
    FieldSchemaController c;
    {
        QSettings ini(QDir(AppSettings::configDir()).filePath(QStringLiteral("schemas-%1.ini").arg(name)),
                      QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("View"));
        c.load(ini);
    }
    const QString one = writeFile(dir, name + QStringLiteral("-schemas-one.log"), logBytes(300));
    const QString two = writeFile(dir, name + QStringLiteral("-schemas-two.log"), logBytes(200, 7, "two"));
    const QString sessionPath = dir.filePath(name + QStringLiteral("-schemas.dlsession"));

    SessionController sessions;
    {
        SchemaTabPool pool{ &c, {} };
        LogViewWidget* t1 = pool.make();
        LogViewWidget* t2 = pool.make();
        t1->addLogFile(one);
        t2->addLogFile(two);
        c.applyToView(t1, FieldSettings{ QStringLiteral("A"), a, true, { QStringLiteral("Level") } });
        c.applyToView(t2, FieldSettings{ QStringLiteral("B"), b, true, {} });
        CHECK(settled(*t1) && settled(*t2), QStringLiteral("вкладки"));
        CHECK(hasFields(*t1, a, { 2 }) && hasFields(*t2, b, { 0, 1, 2, 3 }),
              QStringLiteral("у каждой вкладки своя схема"));
        SessionState session = sessions.capture({ t1, t2 }, t2);
        CHECK(session.tabs.size() == 2 && session.tabs[0].view.fields && session.tabs[1].view.fields
                  && session.tabs[0].view.fields->schema == a && session.tabs[1].view.fields->schema == b,
              QStringLiteral("снята схема каждой вкладки"));
        session.schemaName = QStringLiteral("B");
        session.schema = b;
        CHECK(SessionFile::save(sessionPath, session, &error), QStringLiteral("запись: ") + error);

        // Закрыть и вернуть (Ctrl+Shift+T) — схема вкладки с ней.
        CHECK(sessions.rememberClosedTab(t1, 0), QStringLiteral("закрытая вкладка запомнена"));
        SchemaTabPool closed{ &c, {} };
        const auto reopened = sessions.reopenClosedTab([&closed](int) { return closed.make(); });
        CHECK(reopened.tabs.size() == 1, QStringLiteral("вкладка возвращена"));
        if (reopened.tabs.size() == 1) {
            CHECK(settled(*reopened.tabs[0]) && hasFields(*reopened.tabs[0], a, { 2 }),
                  QStringLiteral("возвращённая вкладка — со своей схемой и колонками"));
        }
    }

    const SessionFile::LoadResult loaded = SessionFile::load(sessionPath);
    CHECK(loaded.state.has_value(), QStringLiteral("читается: ") + loaded.error);
    if (!loaded.state)
        return;
    {
        SchemaTabPool pool{ &c, {} };
        const auto result = sessions.open(*loaded.state, sessionPath, [&pool]() { return pool.make(); });
        CHECK(result.tabs.size() == 2, QStringLiteral("две вкладки"));
        if (result.tabs.size() == 2) {
            CHECK(settled(*result.tabs[0]) && settled(*result.tabs[1]), QStringLiteral("загрузка"));
            CHECK(hasFields(*result.tabs[0], a, { 2 }) && hasFields(*result.tabs[1], b, { 0, 1, 2, 3 }),
                  QStringLiteral("открытая сессия — у каждой вкладки своя схема"));
            CHECK(result.tabs[0]->model()->isIndexedBackend() == (name == QLatin1String("indexed")),
                  QStringLiteral("бэкенд"));
        }
    }

    // Сессия 0.6 — вкладки без своей схемы: все получают схему верхнего уровня.
    g_context = name + QStringLiteral(" / сессия 0.6: схема верхнего уровня");
    {
        SessionState old = *loaded.state;
        for (TabState& tab : old.tabs)
            tab.view.fields.reset();
        old.fieldExtraction = true;
        old.visibleFields.clear();
        SchemaTabPool pool{ &c, {} };
        const auto result = sessions.open(old, sessionPath, [&pool]() { return pool.make(); });
        CHECK(result.tabs.size() == 2, QStringLiteral("две вкладки"));
        for (LogViewWidget* t : result.tabs)
            CHECK(settled(*t) && hasFields(*t, b, { 0, 1, 2, 3 }), QStringLiteral("вкладка — схема сессии"));
    }

    // Схемы нет в каталоге: сессия добавляет «(from session)» — одну на
    // одинаковые определения.
    g_context = name + QStringLiteral(" / сессия: схема не из каталога");
    {
        SessionState strange = *loaded.state;
        for (TabState& tab : strange.tabs)
            tab.view.fields = FieldSettings{ QStringLiteral("Mine"), foreign, true, {} };
        const int before = int(c.schemas().size());
        SchemaTabPool pool{ &c, {} };
        const auto result = sessions.open(strange, sessionPath, [&pool]() { return pool.make(); });
        int added = 0;
        for (const auto& e : c.schemas())
            added += e.schema == foreign ? 1 : 0;
        CHECK(int(c.schemas().size()) == before + 1 && added == 1, QStringLiteral("добавлена одна запись"));
        for (LogViewWidget* t : result.tabs)
            CHECK(settled(*t) && t->parserPattern() == foreign
                      && t->fieldSettings().schemaName.contains(QLatin1String("from session")),
                  QStringLiteral("вкладка — схема «(from session)»"));
        // Убрать — следующий бэкенд начинает с того же каталога.
        SchemaStore::sync({ { QStringLiteral("A"), a, {}, {} }, { QStringLiteral("B"), b, {}, {} } });
        c.reloadCatalogue();
    }

    // Память файла одно-файловой вкладки — её схема; схемы нет в каталоге —
    // схема по умолчанию и сообщение, каталог не растёт.
    g_context = name + QStringLiteral(" / память файла: схема вкладки");
    {
        FileMemoryStore store(dir.filePath(name + QStringLiteral("-schemas-memory")));
        QStringList messages;
        QObject::connect(&c, &FieldSchemaController::statusMessage,
                         [&messages](const QString& text) { messages << text; });
        const auto openTab = [&](SchemaTabPool& pool, const QString& path) {
            LogViewWidget* tab = pool.make();
            tab->setFileMemory(&store);
            tab->addLogFile(path);
            CHECK(settled(*tab), QStringLiteral("загрузка"));
            return tab;
        };
        {
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* tab = openTab(pool, one);
            CHECK(!tab->parserPattern().size(), QStringLiteral("памяти нет — схема по умолчанию"));
            c.applyToView(tab, FieldSettings{ QStringLiteral("B"), b, true, { QStringLiteral("Severity") } });
            CHECK(settled(*tab), QStringLiteral("схема B"));
            tab->saveFileMemory();
        }
        store.waitForWrites();
        {
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* tab = openTab(pool, one);
            CHECK(hasFields(*tab, b, { 2 }), QStringLiteral("память вернула схему и колонки"));
            // Та же схема, но её больше нет в каталоге.
            c.applyToView(tab, FieldSettings{ QStringLiteral("Mine"), foreign, true, {} });
            CHECK(settled(*tab), QStringLiteral("чужая схема"));
            tab->saveFileMemory();
        }
        store.waitForWrites();
        {
            const int before = int(c.schemas().size());
            messages.clear();
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* tab = openTab(pool, one);
            CHECK(tab->parserPattern() == c.defaults().schema && int(c.schemas().size()) == before,
                  QStringLiteral("схемы нет в каталоге — схема по умолчанию, каталог не растёт"));
            CHECK(messages.size() == 1 && messages[0].contains(QLatin1String("Mine")),
                  QStringLiteral("сообщение: ") + messages.join(QStringLiteral(" | ")));
        }
    }
}


// Схема файла слитой вкладки (F-5, PR 3) на живых вкладках: сессия (своя
// схема файла снимается в TabSource.schema и возвращается, схема не из
// каталога — «(from session)»), память файла (файл слитой вкладки помнит
// свою схему; добавленный вторым — получает её, открытый первым — она
// становится схемой вкладки).
static void checkSessionFileSchemas(const QDir& dir, const QString& name)
{
    g_context = name + QStringLiteral(" / сессия: схема файла (F-5, PR 3)");
    const QString a = columnSchema(QStringLiteral("Level"), QStringLiteral("Message"));
    const QString b = columnSchema(QStringLiteral("Severity"), QStringLiteral("Text"));
    const QString foreign = columnSchema(QStringLiteral("Lvl"), QStringLiteral("Body"));
    QString error;
    CHECK(SchemaStore::sync({ { QStringLiteral("A"), a, {}, {} }, { QStringLiteral("B"), b, {}, {} } }, &error),
          QStringLiteral("каталог схем: ") + error);
    FieldSchemaController c;
    {
        QSettings ini(QDir(AppSettings::configDir()).filePath(QStringLiteral("file-schemas-%1.ini").arg(name)),
                      QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("View"));
        c.load(ini);
    }
    const QString one = writeFile(dir, name + QStringLiteral("-fs-one.log"), logBytes(300));
    const QString two = writeFile(dir, name + QStringLiteral("-fs-two.log"), logBytes(200, 7, "two"));
    const QString sessionPath = dir.filePath(name + QStringLiteral("-file-schemas.dlsession"));
    const auto fileOf = [](LogViewWidget& tab, const QString& path) -> LogFilePtr {
        for (const LogFilePtr& f : tab.loadedFiles())
            if (f && LogViewWidget::samePath(f->filePath, path))
                return f;
        return nullptr;
    };
    const QStringList unionColumns{ QStringLiteral("Timestamp"), QStringLiteral("Thread"),
                                    QStringLiteral("Level"), QStringLiteral("Message"),
                                    QStringLiteral("Severity"), QStringLiteral("Text") };
    // Каждый файл — своей схемой: у строки файла two колонка Severity, у one — Level.
    const auto checkTab = [&](LogViewWidget& tab, const QString& bName, const QString& what) {
        const LogFilePtr f1 = fileOf(tab, one);
        const LogFilePtr f2 = fileOf(tab, two);
        CHECK(f1 && f2 && tab.fileSchema(f1).schema == a && tab.fileSchema(f2).schema == b
                  && tab.parserPattern() == a,
              what + QStringLiteral(": схема вкладки A, у файла two — B"));
        CHECK(f2 && tab.fileSchema(f2).schemaName == bName,
              what + QStringLiteral(": имя схемы файла: ") + (f2 ? tab.fileSchema(f2).schemaName : QString()));
        CHECK(tab.parserFieldNames() == unionColumns,
              what + QStringLiteral(": колонки — объединение: ") + tab.parserFieldNames().join(','));
        const int row2 = rowOfLine(*tab.model(), f2 ? f2->filePath : QString(), 2);
        const auto entry = row2 >= 0 ? tab.model()->entryAt(row2) : nullptr;
        CHECK(entry && entry->fields().value(4, entry->message()).trimmed()
                           == QStringLiteral("WARN"),
              what + QStringLiteral(": строка файла two — поле Severity"));
    };

    SessionController sessions;
    {
        SchemaTabPool pool{ &c, {} };
        LogViewWidget* t = pool.make();
        c.applyToView(t, FieldSettings{ QStringLiteral("A"), a, true, {} });
        t->addLogFile(one);
        t->addLogFile(two);
        CHECK(settled(*t), QStringLiteral("вкладка"));
        c.setCurrentView(t);
        c.setTargetFile(fileOf(*t, two) ? fileOf(*t, two)->filePath : QString());
        CHECK(!c.targetFile().isEmpty(), QStringLiteral("цель — файл two"));
        c.selectSchema(1); // B
        CHECK(settled(*t), QStringLiteral("схема файла"));
        checkTab(*t, QStringLiteral("B"), QStringLiteral("живая вкладка"));
        CHECK(c.shownSchema() == b && c.activeSchema() == a,
              QStringLiteral("док показывает схему файла, схема вкладки — прежняя"));
        c.setTargetFile(QString());
        CHECK(c.isMixed() && c.activeIndex() == -1, QStringLiteral("все файлы — «(mixed)»"));
        // Колонка только схемы файла (Severity) показана, Message — нет.
        c.setVisibleFieldNames({ QStringLiteral("Level"), QStringLiteral("Severity") });
        CHECK(settled(*t) && t->fieldSettings().visibleFields
                                 == (QStringList{ QStringLiteral("Level"), QStringLiteral("Severity") }),
              QStringLiteral("выбор колонок вкладки"));
        const SessionState session = sessions.capture({ t }, t);
        CHECK(session.tabs.size() == 1 && session.tabs[0].sources.size() == 2
                  && !session.tabs[0].sources[0].schema && session.tabs[0].sources[1].schema
                  && session.tabs[0].sources[1].schema->schema == b,
              QStringLiteral("снята своя схема только второго файла"));
        CHECK(SessionFile::save(sessionPath, session, &error), QStringLiteral("запись: ") + error);
        c.setCurrentView(nullptr);
    }
    const SessionFile::LoadResult loaded = SessionFile::load(sessionPath);
    CHECK(loaded.state.has_value(), QStringLiteral("читается: ") + loaded.error);
    if (!loaded.state)
        return;
    {
        SchemaTabPool pool{ &c, {} };
        const auto result = sessions.open(*loaded.state, sessionPath, [&pool]() { return pool.make(); });
        CHECK(result.tabs.size() == 1, QStringLiteral("вкладка сессии"));
        if (result.tabs.size() == 1) {
            CHECK(settled(*result.tabs[0]), QStringLiteral("загрузка"));
            checkTab(*result.tabs[0], QStringLiteral("B"), QStringLiteral("открытая сессия"));
            CHECK(result.tabs[0]->fieldSettings().visibleFields
                      == (QStringList{ QStringLiteral("Level"), QStringLiteral("Severity") }),
                  QStringLiteral("выбор колонок — как в сессии, с колонкой схемы файла: ")
                      + result.tabs[0]->fieldSettings().visibleFields.join(','));
            CHECK(result.tabs[0]->model()->isIndexedBackend() == (name == QLatin1String("indexed")),
                  QStringLiteral("бэкенд"));
        }
    }
    {
        // Схемы файла нет в каталоге — сессия добавляет «(from session)».
        SessionState strange = *loaded.state;
        strange.tabs[0].sources[1].schema = FieldSettings{ QStringLiteral("Mine"), foreign, false, {} };
        const int before = int(c.schemas().size());
        SchemaTabPool pool{ &c, {} };
        const auto result = sessions.open(strange, sessionPath, [&pool]() { return pool.make(); });
        CHECK(int(c.schemas().size()) == before + 1, QStringLiteral("схема файла добавлена в каталог"));
        if (result.tabs.size() == 1) {
            CHECK(settled(*result.tabs[0]), QStringLiteral("загрузка"));
            const LogFilePtr f2 = fileOf(*result.tabs[0], two);
            CHECK(f2 && result.tabs[0]->fileSchema(f2).schema == foreign
                      && result.tabs[0]->fileSchema(f2).schemaName.contains(QLatin1String("from session")),
                  QStringLiteral("файл — схема «(from session)»"));
        }
        SchemaStore::sync({ { QStringLiteral("A"), a, {}, {} }, { QStringLiteral("B"), b, {}, {} } });
        c.reloadCatalogue();
    }

    g_context = name + QStringLiteral(" / память файла: схема файла слитой вкладки");
    {
        FileMemoryStore store(dir.filePath(name + QStringLiteral("-file-schemas-memory")));
        {
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* t = pool.make();
            t->setFileMemory(&store);
            c.applyToView(t, FieldSettings{ QStringLiteral("A"), a, true, {} });
            t->addLogFile(one);
            t->addLogFile(two);
            CHECK(settled(*t), QStringLiteral("вкладка"));
            c.applyToFile(t, fileOf(*t, two), FieldSettings{ QStringLiteral("B"), b, false, {} });
            CHECK(settled(*t), QStringLiteral("схема файла"));
            checkTab(*t, QStringLiteral("B"), QStringLiteral("до закрытия"));
            t->saveFileMemory();
        }
        store.waitForWrites();
        {
            // one, затем two: two — вторым, со своей схемой из памяти.
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* t = pool.make();
            t->setFileMemory(&store);
            c.applyToView(t, FieldSettings{ QStringLiteral("A"), a, true, {} });
            t->addLogFile(one);
            t->addLogFile(two);
            CHECK(settled(*t), QStringLiteral("загрузка"));
            checkTab(*t, QStringLiteral("B"), QStringLiteral("память: второй файл"));
        }
        {
            // two один: его схема — схема вкладки.
            SchemaTabPool pool{ &c, {} };
            LogViewWidget* t = pool.make();
            t->setFileMemory(&store);
            t->addLogFile(two);
            CHECK(settled(*t), QStringLiteral("загрузка"));
            CHECK(t->parserPattern() == b && !t->hasFileSchemas(),
                  QStringLiteral("память: первый файл — схема вкладки B"));
        }
    }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);

    checkFormat();
    checkFileMemoryFormat();
    checkSessionFormat();
    checkSchemasFormat();
    checkFileSchemasFormat();
    checkSessionSchema();

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "нет временного каталога\n");
        return 2;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();
    const QByteArray goldenOut = qgetenv("DOCUMENT_STATE_WRITE_CROSS_OS");
    if (!goldenOut.isEmpty())
        return writeCrossOsGoldens(QString::fromLocal8Bit(goldenOut), dir, schema);
    AppSettings& settings = AppSettings::instance();
    for (const bool indexed : { false, true }) {
        const QString name = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
        settings.setIndexedThresholdMB(indexed ? 0 : 512);
        checkReopen(dir, name, schema);
        checkFollowTail(dir, name, schema);
        checkMerged(dir, name, schema);
        checkHiddenFiles(dir, name, schema);
        checkTabMemory(dir, name, schema);
        checkSessionOpen(dir, name, schema);
        checkCrossOsSession(dir, name, schema, QStringLiteral("windows"));
        checkCrossOsSession(dir, name, schema, QStringLiteral("posix"));
    }
    settings.setIndexedThresholdMB(512);
    checkStdin(dir, schema);
    checkMidLoadAndThreshold(dir, schema);
    checkFileMemoryStore(dir);
    checkWriteOrder(dir);
    checkSessionFile(dir);
    checkSessionRebind(dir);
    checkForeignMemory(dir);
    checkSessionProblems(dir, schema);
    checkSessionStdin(dir, schema);
    checkLastSession(dir, schema);
    // Схема на вкладку (F-5) — с каталогом схем во временном каталоге настроек.
    AppSettings::overrideConfigDirForTesting(dir.filePath(QStringLiteral("config")));
    for (const bool indexed : { false, true }) {
        settings.setIndexedThresholdMB(indexed ? 0 : 512);
        checkSessionSchemas(dir, indexed ? QStringLiteral("indexed") : QStringLiteral("resident"));
        checkSessionFileSchemas(dir, indexed ? QStringLiteral("indexed") : QStringLiteral("resident"));
    }
    settings.setIndexedThresholdMB(512);

    std::fprintf(failures ? stderr : stdout, "document_state: %d проверок, %d нарушений\n",
                 checks, failures);
    return failures ? 1 : 0;
}
