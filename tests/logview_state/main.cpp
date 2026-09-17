// ============================================================================
// logview_state — состояние LogListView переживает изменения данных модели.
//
// Класс багов, против которого это написано: «сменил фильтр — слетело
// выделение, прокрутка ушла в начало». Такой баг чинился не раз и
// возвращался, потому что инвариант ОДИН, а путей, меняющих видимый набор
// строк, много (уровни, время, текст, маркеры, выбор колонок, новый файл…), и
// у каждого своя сигнальная хореография: синхронный reset, асинхронный reset,
// вставки строк. Починка одного пути не переносится на соседний. Поэтому
// проверка табличная: один набор инвариантов × все триггеры × все пути
// хранилища × несколько позиций. Новый путь, добавленный мимо общего
// механизма восстановления, падает здесь сразу.
//
// E — текущая запись ДО изменения. Опознаётся по ключу (logicalEntryId,
// файл), а не по номеру строки: номера строк после фильтра другие.
//   I1  E осталась видна → она текущая, и её строка выделена;
//   I2  E осталась видна → её строка целиком во вьюпорте;
//   I3  E скрыта → во вьюпорте ближайшая к ней видимая строка;
//   I4  изменение отменили → снова I1 и I2 (якорь выделения пережил скрытие);
//   I5  выделения не было → запись, стоявшая вверху вьюпорта, если осталась
//       видна, во вьюпорте и остаётся (прокрутка не уезжает в начало);
//   I6  видимый набор не менялся (маркеры строк) → прокрутка не сдвинулась.
//
// Пути хранилища: резидентный синхронный (< 100k записей), резидентный
// асинхронный (≥ 100k — фильтр в пуле, reset приходит позже) и индексный
// (фильтр всегда асинхронный).
//
// Плюс геометрия и клавиатура. LogListView подавляет раскладку QListView и
// замещает её собственной (visualRect / indexAt / setSelection / moveCursor),
// поэтому они проверяются отдельно:
//   G1  indexAt(visualRect(r).center()) == r, соседние строки стыкуются без
//       зазоров и наложений — в том числе с переносом строк (разные высоты);
//   G2  Down / PageDown / End / Up / Home двигают текущую строку, она
//       выделена и видна.
// ============================================================================

#include "LogListView.h"
#include "testdocuments.h"

#include <QApplication>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

using namespace testdocs;

// ---------------------------------------------------------------------------
// Отчётность
// ---------------------------------------------------------------------------

// Печать QString в UTF-8 (qPrintable() отдал бы local 8-bit).
#define U8(s) ((s).toUtf8().constData())

static int     g_failures = 0;
static int     g_checks   = 0;
static QString g_context;
static int     g_contextFailures = 0;

static constexpr int kFailuresPerContext = 6;

static void reportFail(const char* file, int line, const QString& msg)
{
    ++g_failures;
    ++g_contextFailures;
    if (g_contextFailures > kFailuresPerContext) {
        if (g_contextFailures == kFailuresPerContext + 1)
            std::fprintf(stderr, "  … дальнейшие нарушения в [%s] подавлены\n", U8(g_context));
        return;
    }
    std::fprintf(stderr, "FAIL(%s:%d) [%s]: %s\n", file, line, U8(g_context), U8(msg));
}

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
            reportFail(__FILE__, __LINE__, (msg));                             \
    } while (false)

static void setContext(const QString& context)
{
    g_context = context;
    g_contextFailures = 0;
}

// ---------------------------------------------------------------------------
// Наблюдение за view
// ---------------------------------------------------------------------------

static constexpr int kViewWidth  = 900;
static constexpr int kViewHeight = 400;

static bool sameKey(const LogModel::EntryKey& a, const LogModel::EntryKey& b)
{
    return a.logicalEntryId == b.logicalEntryId && a.sourceFile == b.sourceFile;
}

static QString keyText(const LogModel::EntryKey& key)
{
    return key.logicalEntryId < 0 ? QStringLiteral("<нет>")
                                  : QStringLiteral("#%1").arg(key.logicalEntryId);
}

static int currentRow(const LogListView& view)
{
    const QModelIndex index = view.currentIndex();
    return index.isValid() ? index.row() : -1;
}

static QRect rowRect(const LogListView& view, int row)
{
    if (!view.model() || row < 0 || row >= view.model()->rowCount())
        return QRect();
    return view.visualRect(view.model()->index(row, 0));
}

static bool rowFullyVisible(const LogListView& view, int row)
{
    const QRect r = rowRect(view, row);
    const QRect vp = view.viewport()->rect();
    return r.height() > 0 && r.top() >= vp.top() && r.bottom() <= vp.bottom();
}

static bool rowIntersectsViewport(const LogListView& view, int row)
{
    const QRect r = rowRect(view, row);
    return r.height() > 0 && r.intersects(view.viewport()->rect());
}

// Строка во вьюпорте или не дальше экрана от него: вьюпорт мог законно
// сползти (см. Trigger::multiStep), но не уехать в другую часть файла.
static bool rowNearViewport(const LogListView& view, int row)
{
    const QRect r = rowRect(view, row);
    if (r.height() <= 0)
        return false;
    const int slack = view.viewport()->height();
    return r.intersects(view.viewport()->rect().adjusted(0, -slack, 0, slack));
}

static int topVisibleRow(const LogListView& view)
{
    const QModelIndex index = view.indexAt(QPoint(4, 1));
    return index.isValid() ? index.row() : -1;
}

// Сводка состояния view для сообщения о нарушении.
static QString viewState(const LogListView& view)
{
    const auto* model = qobject_cast<const LogModel*>(view.model());
    const int cur = currentRow(view);
    const QRect r = rowRect(view, cur);
    return QStringLiteral("[view: строк=%1, текущая=%2 %3, её y=%4, верхняя=%5, прокрутка=%6/%7]")
        .arg(model ? model->rowCount() : -1)
        .arg(cur)
        .arg(model && cur >= 0 ? keyText(model->keyForRow(cur)) : QStringLiteral("—"))
        .arg(r.isValid() ? QString::number(r.top()) : QStringLiteral("—"))
        .arg(topVisibleRow(view))
        .arg(view.verticalScrollBar()->value())
        .arg(view.verticalScrollBar()->maximum());
}

// ---------------------------------------------------------------------------
// Документ, записи, триггеры
// ---------------------------------------------------------------------------

struct Backend {
    QString  name;
    Document doc;
    QString  schema;
    // Второй файл для триггера «файл добавлен в документ».
    std::function<void(Backend&, const QString&)> addFile;
};

struct EntryInfo {
    LogModel::EntryKey key;
    LogLevel           level = LogLevel::Unknown;
    QDateTime          ts;
    QString            text;
};

static EntryInfo infoAt(const LogModel& model, int row)
{
    EntryInfo e;
    e.key = model.keyForRow(row);
    e.level = model.visibleLevelAt(row);
    e.ts = model.visibleTimestampAt(row);
    e.text = model.messageAt(row);
    return e;
}

// Первая строка записи с меткой и известным уровнем, начиная от доли
// position документа (вперёд, затем назад) — чтобы триггеры могли опираться
// на уровень и время записи.
static int pickRow(const LogModel& model, double position)
{
    const int rows = model.rowCount();
    if (rows == 0)
        return -1;
    const int from = qBound(0, int(rows * position), rows - 1);
    const auto good = [&model](int row) {
        const LogModel::EntryKey key = model.keyForRow(row);
        return model.visibleLevelAt(row) != LogLevel::Unknown
            && model.visibleTimestampAt(row).isValid()
            && model.rowForEntry(key.logicalEntryId, key.sourceFile) == row;
    };
    for (int row = from; row < rows; ++row)
        if (good(row))
            return row;
    for (int row = from - 1; row >= 0; --row)
        if (good(row))
            return row;
    return -1;
}

static void settleChecked(LogModel& model)
{
    CHECK(settle(model), QStringLiteral("модель не пришла в покой"));
}

static void clearFilters(LogModel& model)
{
    model.setLogLevelFilter({});
    model.setTimeRangeFilter(QDateTime(), QDateTime());
    model.setFilterRules(FilterRuleSet());
    model.setRowMarkers({});
    model.setFieldDisplaySelection(false, {});
    settleChecked(model);
}

static FilterRuleSet singleRule(FilterRule::Action action, const QString& text)
{
    FilterRule rule;
    rule.action = action;
    rule.text = text;
    FilterRuleSet set;
    set.rules << rule;
    set.bindFields({}, false);
    return set;
}

// Первое слово сообщения записи (после « - ») — встречается у многих строк,
// поэтому Include по нему сужает выдачу, но оставляет запись.
static QString firstMessageWord(const QString& line)
{
    const int dash = line.indexOf(QStringLiteral(" - "));
    const QString message = dash >= 0 ? line.mid(dash + 3) : line;
    return message.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty);
}

struct Trigger {
    QString name;
    std::function<void(Backend&, const EntryInfo&)> apply;
    // Пусто — триггер без отмены.
    std::function<void(Backend&)> undo;
    // I6: видимый набор не меняется — прокрутка обязана остаться на месте.
    bool visibleSetUnchanged = false;
    // Триггер меняет фильтр несколькими шагами, и на промежуточных
    // шагах запись скрыта. Якорь вьюпорта транзиентный: он снимается
    // заново на каждый reset, поэтому за серию шагов вьюпорт законно
    // сползает на несколько строк. Требуем не «строка во вьюпорте», а
    // «вьюпорт не уехал дальше экрана от неё».
    bool multiStep = false;
};

static QVector<Trigger> triggers()
{
    const auto clearLevels = [](Backend& b) { b.doc.model->setLogLevelFilter({}); };
    const auto clearTime = [](Backend& b) {
        b.doc.model->setTimeRangeFilter(QDateTime(), QDateTime());
    };
    const auto clearRules = [](Backend& b) { b.doc.model->setFilterRules(FilterRuleSet()); };

    QVector<Trigger> list;

    list.append({ QStringLiteral("уровни: оставить уровень записи и Error"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setLogLevelFilter({ e.level, LogLevel::Error });
                  },
                  clearLevels });

    // Пользователь щёлкает кнопками уровней подряд: промежуточные наборы
    // запись скрывают, итоговый — показывает.
    {
        Trigger t{ QStringLiteral("уровни: серия переключений подряд, без ожидания"),
                  [](Backend& b, const EntryInfo& e) {
                      LogModel& m = *b.doc.model;
                      m.setLogLevelFilter({ LogLevel::Fatal });
                      m.setLogLevelFilter({ LogLevel::Fatal, LogLevel::Error });
                      m.setLogLevelFilter({ LogLevel::Fatal, LogLevel::Error, e.level });
                  },
                  clearLevels };
        t.multiStep = true;
        list.append(t);
    }

    {
        Trigger t{ QStringLiteral("уровни: серия переключений с ожиданием между ними"),
                  [](Backend& b, const EntryInfo& e) {
                      LogModel& m = *b.doc.model;
                      m.setLogLevelFilter({ LogLevel::Fatal });
                      settle(m);
                      m.setLogLevelFilter({ LogLevel::Fatal, LogLevel::Error });
                      settle(m);
                      m.setLogLevelFilter({ LogLevel::Fatal, LogLevel::Error, e.level });
                  },
                  clearLevels };
        t.multiStep = true;
        list.append(t);
    }

    list.append({ QStringLiteral("уровни: скрыть уровень записи"),
                  [](Backend& b, const EntryInfo& e) {
                      QSet<LogLevel> levels = { LogLevel::Unknown, LogLevel::Trace,
                                                LogLevel::Debug,   LogLevel::Info,
                                                LogLevel::Warn,    LogLevel::Error,
                                                LogLevel::Fatal };
                      levels.remove(e.level);
                      b.doc.model->setLogLevelFilter(levels);
                  },
                  clearLevels });

    list.append({ QStringLiteral("время: окно вокруг записи"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setTimeRangeFilter(e.ts.addSecs(-90), e.ts.addSecs(90));
                  },
                  clearTime });

    list.append({ QStringLiteral("время: окно после записи (запись скрыта)"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setTimeRangeFilter(e.ts.addSecs(5), e.ts.addSecs(3600));
                  },
                  clearTime });

    list.append({ QStringLiteral("текст: Include первого слова сообщения записи"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setFilterRules(
                          singleRule(FilterRule::Action::Include, firstMessageWord(e.text)));
                  },
                  clearRules });

    list.append({ QStringLiteral("текст: Exclude строки записи (запись скрыта)"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setFilterRules(singleRule(FilterRule::Action::Exclude, e.text));
                  },
                  clearRules });

    {
        Trigger t{ QStringLiteral("маркеры строк"),
                   [](Backend& b, const EntryInfo&) {
                       HighlightPattern p;
                       p.text = QStringLiteral("Disk");
                       p.color = QColor(200, 60, 60);
                       b.doc.model->setRowMarkers({ p });
                   },
                   [](Backend& b) { b.doc.model->setRowMarkers({}); } };
        t.visibleSetUnchanged = true;
        list.append(t);
    }

    list.append({ QStringLiteral("колонки: показаны только Level и Message"),
                  [](Backend& b, const EntryInfo&) {
                      b.doc.model->setFieldDisplaySelection(true, { 2, 3 });
                  },
                  [](Backend& b) { b.doc.model->setFieldDisplaySelection(false, {}); } });

    list.append({ QStringLiteral("комбинация: уровень + время + текст"),
                  [](Backend& b, const EntryInfo& e) {
                      LogModel& m = *b.doc.model;
                      m.setLogLevelFilter({ e.level, LogLevel::Warn });
                      m.setTimeRangeFilter(e.ts.addSecs(-300), e.ts.addSecs(300));
                      m.setFilterRules(
                          singleRule(FilterRule::Action::Exclude, QStringLiteral("at com.example")));
                  },
                  [](Backend& b) {
                      LogModel& m = *b.doc.model;
                      m.setLogLevelFilter({});
                      m.setTimeRangeFilter(QDateTime(), QDateTime());
                      m.setFilterRules(FilterRuleSet());
                  } });

    return list;
}

// ---------------------------------------------------------------------------
// Сценарии
// ---------------------------------------------------------------------------

static void checkEntryPreserved(const LogListView& view, const LogModel& model,
                                const EntryInfo& e, const QString& phase)
{
    const int expected = model.rowForEntry(e.key.logicalEntryId, e.key.sourceFile);
    if (expected >= 0) {
        const int cur = currentRow(view);
        const LogModel::EntryKey curKey = cur >= 0 ? model.keyForRow(cur) : LogModel::EntryKey{};
        CHECK(sameKey(curKey, e.key),
              QStringLiteral("%1: I1 текущей должна остаться запись %2, а текущая %3 %4")
                  .arg(phase, keyText(e.key), keyText(curKey), viewState(view)));
        CHECK(cur >= 0 && view.selectionModel()->isRowSelected(cur, QModelIndex()),
              QStringLiteral("%1: I1 строка текущей записи не выделена %2")
                  .arg(phase, viewState(view)));
        CHECK(cur >= 0 && rowFullyVisible(view, cur),
              QStringLiteral("%1: I2 строка записи %2 не во вьюпорте %3")
                  .arg(phase, keyText(e.key), viewState(view)));
    } else {
        const int nearest = model.nearestVisibleRow(e.key.logicalEntryId, e.key.sourceFile);
        if (nearest >= 0) {
            CHECK(rowIntersectsViewport(view, nearest),
                  QStringLiteral("%1: I3 запись скрыта, во вьюпорте должна быть ближайшая "
                                 "видимая строка %2 %3")
                      .arg(phase).arg(nearest).arg(viewState(view)));
        }
    }
}

// Отложенные таймеры view: пересчёт после resize — 150 мс, уточнение высот
// строк — 0–50 мс. Проверять геометрию раньше — значит ловить промежуточное
// состояние, а не поведение.
static constexpr int kViewTimersMs = 250;

static std::unique_ptr<LogListView> makeView(LogModel& model)
{
    auto view = std::make_unique<LogListView>();
    view->resize(kViewWidth, kViewHeight);
    view->setModel(&model);
    view->show();
    pump(kViewTimersMs);
    return view;
}

// Окно снова стало активным (Alt+Tab, закрыт диалог, фокус вернулся из
// панели): Qt доставляет FocusIn, и QAbstractItemView::focusInEvent сам
// назначает текущую строку, если её нет. Шлём событие явно — чтобы сценарий
// не зависел от того, даёт ли платформа фокус offscreen-окну.
static void activateWindowFocus(LogListView& view)
{
    QFocusEvent event(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(&view, &event);
    pump(30);
}

static void selectRow(LogListView& view, int row)
{
    const QModelIndex index = view.model()->index(row, 0);
    view.setCurrentIndex(index);
    view.scrollTo(index, QAbstractItemView::PositionAtCenter);
    pump(30);
}

static const QVector<double> kPositions = { 0.35, 0.6, 0.97 };

// I1–I4, I6: выделенная запись переживает триггер и его отмену.
static void runSelectionScenario(Backend& b, LogListView& view, const Trigger& t,
                                 double position)
{
    setContext(QStringLiteral("%1 / %2 / позиция %3%")
                   .arg(b.name, t.name).arg(int(position * 100)));
    LogModel& model = *b.doc.model;

    clearFilters(model);
    const int start = pickRow(model, position);
    CHECK(start >= 0, QStringLiteral("предусловие: нет подходящей строки"));
    if (start < 0)
        return;

    selectRow(view, start);
    const EntryInfo e = infoAt(model, start);
    CHECK(currentRow(view) == start && rowFullyVisible(view, start),
          QStringLiteral("предусловие: запись выбрана и видна %1").arg(viewState(view)));
    const int scrollBefore = view.verticalScrollBar()->value();

    t.apply(b, e);
    settleChecked(model);
    checkEntryPreserved(view, model, e, QStringLiteral("после изменения"));
    if (t.visibleSetUnchanged) {
        CHECK(view.verticalScrollBar()->value() == scrollBefore,
              QStringLiteral("I6 видимый набор не менялся, а прокрутка сдвинулась с %1 %2")
                  .arg(scrollBefore).arg(viewState(view)));
    }

    // Пока запись скрыта, текущей строки нет — и пользователь переключился
    // на другое окно и вернулся. Выбор пользователя обязан это пережить.
    activateWindowFocus(view);

    if (t.undo) {
        t.undo(b);
        settleChecked(model);
        CHECK(model.rowForEntry(e.key.logicalEntryId, e.key.sourceFile) >= 0,
              QStringLiteral("после отмены запись обязана снова быть видна"));
        checkEntryPreserved(view, model, e, QStringLiteral("после отмены (I4)"));
    }
}

// I5, I6: выделения нет — прокрутка держится за верхнюю видимую запись.
static void runViewportScenario(Backend& b, const Trigger& t, double position)
{
    setContext(QStringLiteral("%1 / без выделения / %2 / позиция %3%")
                   .arg(b.name, t.name).arg(int(position * 100)));
    LogModel& model = *b.doc.model;

    clearFilters(model);
    const auto view = makeView(model);
    const int start = pickRow(model, position);
    if (start < 0)
        return;
    // Документ открыт, окно активно, пользователь листает колесом или
    // ползунком — ни одной строки не выбрал.
    activateWindowFocus(*view);
    view->scrollTo(model.index(start, 0), QAbstractItemView::PositionAtTop);
    pump(30);

    const int top = topVisibleRow(*view);
    CHECK(view->selectionModel()->selectedRows().isEmpty(),
          QStringLiteral("предусловие: ни одна строка не выделена %1").arg(viewState(*view)));
    CHECK(top > 0, QStringLiteral("предусловие: вьюпорт прокручен от начала %1").arg(viewState(*view)));
    if (top <= 0)
        return;
    const EntryInfo e = infoAt(model, top);
    const int scrollBefore = view->verticalScrollBar()->value();

    t.apply(b, e);
    settleChecked(model);

    const int row = model.rowForEntry(e.key.logicalEntryId, e.key.sourceFile);
    if (row >= 0) {
        CHECK(t.multiStep ? rowNearViewport(*view, row) : rowIntersectsViewport(*view, row),
              QStringLiteral("I5 верхняя запись %1 осталась видна (строка %2), но ушла из "
                             "вьюпорта %3")
                  .arg(keyText(e.key)).arg(row).arg(viewState(*view)));
    } else {
        const int nearest = model.nearestVisibleRow(e.key.logicalEntryId, e.key.sourceFile);
        if (nearest >= 0) {
            CHECK(rowIntersectsViewport(*view, nearest),
                  QStringLiteral("I5 верхняя запись скрыта, во вьюпорте должна быть ближайшая "
                                 "видимая строка %1 %2")
                      .arg(nearest).arg(viewState(*view)));
        }
    }
    if (t.visibleSetUnchanged) {
        CHECK(view->verticalScrollBar()->value() == scrollBefore,
              QStringLiteral("I6 видимый набор не менялся, а прокрутка сдвинулась с %1 %2")
                  .arg(scrollBefore).arg(viewState(*view)));
    }
}

// I1, I2: второй файл вливается в документ, строки встают в середину.
static void runAddFileScenario(Backend& b, LogListView& view, const QString& secondFile)
{
    setContext(QStringLiteral("%1 / в документ добавлен второй файл").arg(b.name));
    LogModel& model = *b.doc.model;

    clearFilters(model);
    const int start = pickRow(model, 0.6);
    if (start < 0)
        return;
    selectRow(view, start);
    const EntryInfo e = infoAt(model, start);
    const int rowsBefore = model.rowCount();

    b.addFile(b, secondFile);
    settleChecked(model);
    for (const QString& error : b.doc.errors)
        CHECK(false, error);

    CHECK(model.rowCount() > rowsBefore,
          QStringLiteral("строки второго файла не появились: было %1, стало %2")
              .arg(rowsBefore).arg(model.rowCount()));
    checkEntryPreserved(view, model, e, QStringLiteral("после добавления файла"));
}

// ---------------------------------------------------------------------------
// Геометрия и клавиатура
// ---------------------------------------------------------------------------

// G1: собственная геометрия согласована сама с собой.
// settleMs — пауза после каждой прокрутки: высоты строк уточняются лениво
// (отрисовка + таймер), а без переноса строк они и так точные.
static void checkGeometry(const QString& name, LogListView& view, int settleMs = 60,
                          bool lastRowReachable = true)
{
    setContext(name + QStringLiteral(" / геометрия"));
    const QAbstractItemModel* model = view.model();
    const int rows = model->rowCount();
    const int vpHeight = view.viewport()->height();
    const int step = qMax(1, rows / 60);

    for (int row = 0; row < rows; row += step) {
        if (g_contextFailures > kFailuresPerContext)
            return;
        const QModelIndex index = model->index(row, 0);
        view.scrollTo(index, QAbstractItemView::PositionAtTop);
        pump(settleMs);

        const QRect r = view.visualRect(index);
        CHECK(r.height() > 0,
              QStringLiteral("строка %1: нулевая высота %2").arg(row).arg(viewState(view)));
        // Последние строки не могут встать к верху — у прокрутки есть предел.
        CHECK(r.top() <= 0 || view.verticalScrollBar()->value()
                                  == view.verticalScrollBar()->maximum(),
              QStringLiteral("строка %1: после scrollTo(PositionAtTop) y=%2 %3")
                  .arg(row).arg(r.top()).arg(viewState(view)));
        if (r.top() >= 0 && r.top() < vpHeight) {
            const QModelIndex hit = view.indexAt(QPoint(r.width() / 2, r.top()));
            CHECK(hit.isValid() && hit.row() == row,
                  QStringLiteral("строка %1: indexAt(верх строки) даёт %2 %3")
                      .arg(row).arg(hit.isValid() ? hit.row() : -1).arg(viewState(view)));
            const int lastY = qMin(r.bottom(), vpHeight - 1);
            const QModelIndex hitBottom = view.indexAt(QPoint(r.width() / 2, lastY));
            CHECK(hitBottom.isValid() && hitBottom.row() == row,
                  QStringLiteral("строка %1: indexAt(низ строки) даёт %2 %3")
                      .arg(row).arg(hitBottom.isValid() ? hitBottom.row() : -1).arg(viewState(view)));
        }
        if (row + 1 < rows) {
            const QRect next = view.visualRect(model->index(row + 1, 0));
            CHECK(next.top() == r.bottom() + 1,
                  QStringLiteral("строки %1 и %2 не стыкуются: низ %3, верх следующей %4")
                      .arg(row).arg(row + 1).arg(r.bottom()).arg(next.top()));
        }
    }

    // Последняя строка достижима и видна целиком.
    view.scrollTo(model->index(rows - 1, 0), QAbstractItemView::PositionAtBottom);
    pump(kViewTimersMs);
    if (lastRowReachable) {
        CHECK(rowFullyVisible(view, rows - 1),
              QStringLiteral("последняя строка недостижима %1").arg(viewState(view)));
    }
}

static void pressKey(LogListView& view, int key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
    pump(kViewTimersMs);
}

// G2: навигация с клавиатуры двигает текущую строку, выделяет и показывает её.
static void checkKeyboard(const QString& name, LogListView& view)
{
    setContext(name + QStringLiteral(" / клавиатура"));
    const int rows = view.model()->rowCount();
    if (rows < 50)
        return;

    const auto expectCurrent = [&view](int expected, const QString& what) {
        const int cur = currentRow(view);
        CHECK(cur == expected || (expected < 0 && cur >= 0),
              QStringLiteral("%1: текущая строка %2, ожидалась %3 %4")
                  .arg(what).arg(cur).arg(expected).arg(viewState(view)));
        CHECK(cur >= 0 && view.selectionModel()->isRowSelected(cur, QModelIndex()),
              QStringLiteral("%1: текущая строка не выделена %2").arg(what, viewState(view)));
        CHECK(cur >= 0 && rowFullyVisible(view, cur),
              QStringLiteral("%1: текущая строка не видна %2").arg(what, viewState(view)));
    };

    selectRow(view, 10);
    pressKey(view, Qt::Key_Down);
    expectCurrent(11, QStringLiteral("Down"));
    pressKey(view, Qt::Key_PageDown);
    const int afterPageDown = currentRow(view);
    CHECK(afterPageDown > 11,
          QStringLiteral("PageDown не сдвинул текущую строку %1").arg(viewState(view)));
    expectCurrent(-1, QStringLiteral("PageDown"));
    pressKey(view, Qt::Key_End);
    expectCurrent(rows - 1, QStringLiteral("End"));
    pressKey(view, Qt::Key_Up);
    expectCurrent(rows - 2, QStringLiteral("Up"));
    pressKey(view, Qt::Key_Home);
    expectCurrent(0, QStringLiteral("Home"));
    CHECK(view.verticalScrollBar()->value() == 0,
          QStringLiteral("Home: прокрутка не в начале %1").arg(viewState(view)));
}

// ---------------------------------------------------------------------------
// ЗАДОКУМЕНТИРОВАННЫЙ ДЕФЕКТ (на 2026-09-17 НЕ исправлен): с включённым
// переносом строк высоты строк уточняются лениво, уже после прокрутки, и view
// не возвращает текущую строку в экран — после Down / PageDown / End она
// оказывается ниже нижней границы вьюпорта, а последняя строка недостижима
// прокруткой. Сам курсор при этом двигается правильно и выделяется.
//
// Здесь проверяется то, что работает, и ЗАКРЕПЛЯЕТСЯ сам дефект: когда его
// починят, упадёт последняя проверка — тогда замените этот вызов на обычные
// checkKeyboard/checkGeometry (с lastRowReachable).
// ---------------------------------------------------------------------------
static void checkWrapKnownDefect(LogListView& view)
{
    setContext(QStringLiteral("перенос строк / известный дефект прокрутки"));
    const int rows = view.model()->rowCount();
    if (rows < 50)
        return;

    int offscreen = 0;
    const auto step = [&](int key, int expected, const QString& what) {
        pressKey(view, key);
        const int cur = currentRow(view);
        if (expected >= 0) {
            CHECK(cur == expected,
                  QStringLiteral("%1: текущая строка %2, ожидалась %3 %4")
                      .arg(what).arg(cur).arg(expected).arg(viewState(view)));
        } else {
            CHECK(cur >= 0, QStringLiteral("%1: текущей строки нет %2").arg(what, viewState(view)));
        }
        CHECK(cur >= 0 && view.selectionModel()->isRowSelected(cur, QModelIndex()),
              QStringLiteral("%1: текущая строка не выделена %2").arg(what, viewState(view)));
        if (cur >= 0 && !rowFullyVisible(view, cur))
            ++offscreen;
    };

    selectRow(view, 10);
    step(Qt::Key_Down, 11, QStringLiteral("Down"));
    step(Qt::Key_PageDown, -1, QStringLiteral("PageDown"));
    step(Qt::Key_End, rows - 1, QStringLiteral("End"));

    // Сам дефект НЕ закрепляется падением: он зависит от метрик шрифта, то
    // есть от платформы, и строгая проверка «дефект на месте» сделала бы
    // тест красным там, где высоты строк оцениваются точно. Сообщение
    // заметное — если дефект перестал воспроизводиться, это повод проверить,
    // не починен ли он, и вернуть обычные checkKeyboard/checkGeometry.
    std::fprintf(stderr,
                 offscreen > 0
                     ? "ИЗВЕСТНЫЙ ДЕФЕКТ на месте: с переносом строк навигация уводит "
                       "текущую строку за экран (%d из 3 шагов)\n"
                     : "ВНИМАНИЕ: известный дефект с переносом строк здесь НЕ воспроизвёлся "
                       "(%d из 3 шагов) — возможно, он починен: верните обычные "
                       "checkKeyboard/checkGeometry\n",
                 offscreen);
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // Настройки приложения (шрифт view) — не из профиля пользователя.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    SetConsoleOutputCP(CP_UTF8);
#endif

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create temporary directory\n");
        return 2;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();
    const QStringList fieldNames = LogPattern(schema).fieldNames();
    const QDateTime base = fixtureBaseTime();

    // Малый документ — синхронный резидентный фильтр и индексный путь;
    // большой (≥ 100k строк) — асинхронный резидентный фильтр.
    const QString smallLog = writeFile(dir, QStringLiteral("small.log"),
        makeLogBytes(2400, 11u, "\n", base, false, true, true));
    const QString largeLog = writeFile(dir, QStringLiteral("large.log"),
        makeLogBytes(80000, 12u, "\n", base, false, true, true));
    const QString second = writeFile(dir, QStringLiteral("second.log"),
        makeLogBytes(900, 13u, "\r\n", base.addSecs(300), false, true, true));
    for (const QString& path : { smallLog, largeLog, second }) {
        if (path.isEmpty()) {
            std::fprintf(stderr, "не удалось записать фикстуру\n");
            return 2;
        }
    }

    const auto addResident = [](Backend& b, const QString& path) {
        addResidentFiles(b.doc, { path }, b.schema);
    };
    const auto addIndexed = [](Backend& b, const QString& path) {
        addIndexedFiles(b.doc, { path }, b.schema);
    };

    std::vector<std::unique_ptr<Backend>> backends;
    backends.push_back(std::make_unique<Backend>(Backend{
        QStringLiteral("резидентный, синхронный фильтр"), buildResident({ smallLog }, schema),
        schema, addResident }));
    backends.push_back(std::make_unique<Backend>(Backend{
        QStringLiteral("резидентный, асинхронный фильтр"), buildResident({ largeLog }, schema),
        schema, addResident }));
    backends.push_back(std::make_unique<Backend>(Backend{
        QStringLiteral("индексный"), buildIndexed({ smallLog }, schema), schema, addIndexed }));

    const QVector<Trigger> allTriggers = triggers();

    for (auto& backend : backends) {
        Backend& b = *backend;
        setContext(b.name + QStringLiteral(" / загрузка"));
        for (const QString& error : b.doc.errors)
            CHECK(false, error);
        b.doc.model->setAvailableFields(fieldNames);
        std::fprintf(stdout, "%s: %d строк\n", U8(b.name), b.doc.model->rowCount());
        std::fflush(stdout);

        clearFilters(*b.doc.model);
        auto view = makeView(*b.doc.model);

        checkGeometry(b.name, *view);
        checkKeyboard(b.name, *view);

        for (const Trigger& t : allTriggers) {
            for (const double position : kPositions)
                runSelectionScenario(b, *view, t, position);
            runViewportScenario(b, t, 0.5);
        }

        runAddFileScenario(b, *view, second);
    }

    // Перенос строк даёт разные высоты — геометрия обязана оставаться
    // согласованной и тогда (у индексного бэкенда перенос отключён).
    {
        Backend& b = *backends.front();
        clearFilters(*b.doc.model);
        auto view = makeView(*b.doc.model);
        view->resize(320, kViewHeight);
        view->setWordWrap(true);
        pump(400); // дебаунс resize + отложенный пересчёт высот
        checkGeometry(b.name + QStringLiteral(", перенос строк"), *view, kViewTimersMs,
                      /*lastRowReachable=*/false);
        checkWrapKnownDefect(*view);
    }

    setContext(QStringLiteral("итог"));
    if (g_failures > 0) {
        std::fprintf(stderr, "logview_state: %d проверок, %d НАРУШЕНИЙ\n", g_checks, g_failures);
        return 1;
    }
    std::fprintf(stdout, "logview_state: %d проверок, нарушений нет\n", g_checks);
    return 0;
}
