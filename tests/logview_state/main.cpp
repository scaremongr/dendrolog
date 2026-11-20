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
//
// И восстановление позиции из сохранённого состояния вкладки (R-4,
// LogViewWidget::applyState): позиция ставится, когда RecordLocator найдёт
// строку, — и НЕ ставится, если пользователь до того выбрал строку или
// прокрутил вид сам (инвариант 10); без его действий — ставится.
// ============================================================================

#include "LogListView.h"
#include "appsettings.h"
#include "bookmarkstore.h"
#include "fulllogcontroller.h"
#include "indexedlogstore.h"
#include "logviewwidget.h"
#include "navhistory.h"
#include "testdocuments.h"

#include <QApplication>
#include <QFrame>
#include <QItemSelectionModel>
#include <QPushButton>
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
    // View сценария и показ полного лога над ним (триггеры R-3): контроллер
    // живёт, пока жив view, — сценарий снимает его по окончании.
    LogListView* view = nullptr;
    std::unique_ptr<FullLogController> fullLog;
};

static FullLogController& fullLogOf(Backend& b)
{
    if (!b.fullLog)
        b.fullLog = std::make_unique<FullLogController>(*b.doc.model, *b.view);
    return *b.fullLog;
}

// Сценарий кончился — view уходит, контроллер с ним.
struct ScenarioView {
    Backend& b;
    ScenarioView(Backend& backend, LogListView* view) : b(backend)
    {
        b.fullLog.reset();
        b.view = view;
    }
    ~ScenarioView()
    {
        b.fullLog.reset();
        b.view = nullptr;
    }
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

// Состояние кодировки файла документа — как LogViewWidget::fileEncoding.
static FileEncoding documentEncoding(const Document& doc, const LogFilePtr& lf)
{
    if (const IndexedLogStore* store = doc.model->indexedOrNull())
        return store->fileEncoding(lf);
    return doc.encodings.value(lf->filePath);
}

struct Trigger {
    QString name;
    std::function<void(Backend&, const EntryInfo&)> apply;
    // Пусто — триггер без отмены.
    std::function<void(Backend&)> undo;
    // I6: видимый набор не меняется — прокрутка обязана остаться на месте.
    bool visibleSetUnchanged = false;
    // Видимый набор тот же, но текст строк другой (смена кодировки): у
    // резидентного бэкенда с переносом высоты строк законно меняются, и
    // значение скроллбара тоже. Инвариант — место текущей строки на экране
    // (visualRect), а не пиксели прокрутки.
    bool rowPlaceUnchanged = false;
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

    // Закладки (R-2): отметка в жёлобе — роль без смены текста и видимого
    // набора; ни выделение, ни прокрутка от неё не двигаются.
    {
        Trigger t{ QStringLiteral("закладки: поставить на запись и соседние строки"),
                   [](Backend& b, const EntryInfo& e) {
                       LogModel& m = *b.doc.model;
                       const int row = m.rowForEntry(e.key.logicalEntryId, e.key.sourceFile);
                       for (const int r : { row - 1, row, row + 1, 0, m.rowCount() - 1 })
                           if (r >= 0 && r < m.rowCount() && !m.bookmarks().isBookmarkedRow(r))
                               m.bookmarks().toggle(r);
                   },
                   [](Backend& b) { b.doc.model->bookmarks().clear(); } };
        t.visibleSetUnchanged = true;
        list.append(t);
    }

    // Фильтр по файлу (U-23): документы таблицы — один файл, скрывается
    // чужой (как щелчок по чипу легенды слитой вкладки, где запись в другом
    // файле): фильтр применяется — у индексного бэкенда фоновым заданием с
    // reset, — а видимый набор тот же. Скрытие строк соседнего файла на
    // слитой вкладке — tests/row_chrome.
    {
        Trigger t{ QStringLiteral("файл: скрыть другой файл вкладки"),
                   [](Backend& b, const EntryInfo&) {
                       b.doc.model->setHiddenFiles({ QStringLiteral("/no/such/other-file.log") });
                   },
                   [](Backend& b) { b.doc.model->setHiddenFiles({}); } };
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

    // Смена кодировки файла записи (F-1, шаг 4): выбор пользователя, затем
    // «Определить автоматически». Строки те же, текст другой — reset модели.
    {
        Trigger t{ QStringLiteral("кодировка: файл записи — KOI8-R вручную"),
                   [](Backend& b, const EntryInfo& e) {
                       for (const LogFilePtr& lf : b.doc.files)
                           if (lf.get() == e.key.sourceFile)
                               setDocumentEncoding(b.doc, lf,
                                   documentEncoding(b.doc, lf).withUserChoice(
                                       TextDecoder(TextEncoding::Koi8R)),
                                   b.schema);
                   },
                   [](Backend& b) {
                       for (const LogFilePtr& lf : b.doc.files)
                           setDocumentEncoding(b.doc, lf, documentEncoding(b.doc, lf).automatic(),
                                               b.schema);
                   } };
        t.rowPlaceUnchanged = true;
        list.append(t);
    }

    // Полный лог (R-3): фильтр вкладки приостановлен и возвращён. Видимый
    // набор меняется дважды (фильтр, затем без фильтра), запись в полном логе
    // видна всегда.
    list.append({ QStringLiteral("полный лог: показать из фильтра уровней (запись видна)"),
                  [](Backend& b, const EntryInfo& e) {
                      b.doc.model->setLogLevelFilter({ e.level, LogLevel::Error });
                      settle(*b.doc.model);
                      fullLogOf(b).show();
                  },
                  [](Backend& b) {
                      fullLogOf(b).returnToFiltered();
                      settle(*b.doc.model);
                      b.doc.model->setLogLevelFilter({});
                  } });

    // Без выделения верхняя строка экрана после фильтра — уже соседняя с
    // записью; показ держит на месте её, запись встаёт рядом (multiStep).
    {
        Trigger t{ QStringLiteral("полный лог: показать, когда запись скрыта (Exclude)"),
                   [](Backend& b, const EntryInfo& e) {
                       b.doc.model->setFilterRules(singleRule(FilterRule::Action::Exclude, e.text));
                       settle(*b.doc.model);
                       fullLogOf(b).show();
                   },
                   [](Backend& b) {
                       fullLogOf(b).returnToFiltered();
                       settle(*b.doc.model);
                       b.doc.model->setFilterRules(FilterRuleSet());
                   } };
        t.multiStep = true;
        list.append(t);
    }

    // Нажатия подряд, не дожидаясь фонового фильтра: показ, возврат, показ.
    {
        Trigger t{ QStringLiteral("полный лог: показ, возврат и показ до конца фильтра"),
                   [](Backend& b, const EntryInfo& e) {
                       b.doc.model->setLogLevelFilter({ e.level, LogLevel::Warn });
                       settle(*b.doc.model);
                       FullLogController& fl = fullLogOf(b);
                       fl.show();
                       fl.returnToFiltered();
                       fl.show();
                   },
                   [](Backend& b) {
                       fullLogOf(b).returnToFiltered();
                       b.doc.model->setLogLevelFilter({});
                   } };
        t.multiStep = true;
        list.append(t);
    }

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
    const ScenarioView scenarioView(b, &view);

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

    const QRect placeBefore = rowRect(view, start);
    t.apply(b, e);
    settleChecked(model);
    if (t.rowPlaceUnchanged)
        pump(kViewTimersMs); // отложенное уточнение высот после reset
    checkEntryPreserved(view, model, e, QStringLiteral("после изменения"));
    if (t.rowPlaceUnchanged) {
        const int cur = currentRow(view);
        CHECK(cur >= 0 && rowRect(view, cur).top() == placeBefore.top(),
              QStringLiteral("текущая строка осталась на том же месте экрана (верх %1 → %2) %3")
                  .arg(placeBefore.top()).arg(cur >= 0 ? rowRect(view, cur).top() : -1)
                  .arg(viewState(view)));
    }
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
    const ScenarioView scenarioView(b, view.get());
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

// ---------------------------------------------------------------------------
// Полный лог (R-3): та же запись на том же месте экрана в обе стороны
// ---------------------------------------------------------------------------

static bool tabSettled(LogViewWidget& tab);

static int entryRow(const LogModel& model, const EntryInfo& e)
{
    return model.rowForEntry(e.key.logicalEntryId, e.key.sourceFile);
}

// Строка рядом с row — первая строка другой записи, чей уровень не из levels:
// контекст, скрытый фильтром.
static int contextRowNear(const LogModel& model, int row, const QSet<LogLevel>& levels)
{
    const int entry = model.keyForRow(row).logicalEntryId;
    for (int d = 1; d < 400; ++d)
        for (const int r : { row + d, row - d }) {
            if (r < 0 || r >= model.rowCount())
                continue;
            const LogModel::EntryKey key = model.keyForRow(r);
            const LogLevel level = model.visibleLevelAt(r);
            if (key.logicalEntryId != entry && level != LogLevel::Unknown && !levels.contains(level)
                && model.rowForEntry(key.logicalEntryId, key.sourceFile) == r)
                return r;
        }
    return -1;
}

static void settleView(LogModel& model, LogListView& view)
{
    settleChecked(model);
    waitFor([&] { return !view.hasPendingLayout(); }, 5000);
    pump(30);
}

// Выбранная запись e и верх её строки на экране.
static QString placeText(const LogListView& view, const LogModel& model, const EntryInfo& e)
{
    const int row = entryRow(model, e);
    return QStringLiteral("запись %1: строка %2, выбрана %3, верх %4 %5")
        .arg(keyText(e.key)).arg(row).arg(view.userSelectedRow())
        .arg(row >= 0 ? rowRect(view, row).top() : -1).arg(viewState(view));
}

static bool atPlace(const LogListView& view, const LogModel& model, const EntryInfo& e, int top)
{
    const int row = entryRow(model, e);
    return row >= 0 && view.userSelectedRow() == row && rowRect(view, row).top() == top;
}

static void runFullLogChecks(Backend& b, double position)
{
    LogModel& model = *b.doc.model;
    const QString prefix = QStringLiteral("%1 / полный лог / позиция %2% / ")
                               .arg(b.name).arg(int(position * 100));
    clearFilters(model);
    const int allRows = model.rowCount();
    const auto view = makeView(model);
    FullLogController fl(model, *view);
    const int start = pickRow(model, position);
    if (start < 0)
        return;
    const EntryInfo e = infoAt(model, start);
    const QSet<LogLevel> levels = { e.level, LogLevel::Error };
    const int rowH = rowRect(*view, start).height();

    // 1. Запись выбрана не по центру экрана; показ и возврат держат её на месте.
    setContext(prefix + QStringLiteral("туда и обратно — то же место"));
    model.setLogLevelFilter(levels);
    settleView(model, *view);
    CHECK(fl.canShow(), QStringLiteral("фильтр скрывает строки — показывать есть что"));
    view->goToRow(entryRow(model, e));
    view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() + 3 * rowH);
    pump(30);
    const int top0 = rowRect(*view, entryRow(model, e)).top();
    CHECK(view->userSelectedRow() == entryRow(model, e) && top0 >= 0
              && top0 + rowH <= view->viewport()->height(),
          QStringLiteral("предусловие: %1").arg(placeText(*view, model, e)));
    CHECK(fl.show(), QStringLiteral("показ начат"));
    settleView(model, *view);
    CHECK(fl.isActive() && !model.hasFilters() && model.rowCount() == allRows,
          QStringLiteral("полный лог: %1 строк из %2, фильтры %3")
              .arg(model.rowCount()).arg(allRows).arg(model.hasFilters()));
    CHECK(atPlace(*view, model, e, top0),
          QStringLiteral("показ: запись выбрана на прежнем месте (верх %1) — %2")
              .arg(top0).arg(placeText(*view, model, e)));
    CHECK(!fl.canShow() && !fl.show(), QStringLiteral("повторный показ во время показа — нет"));
    CHECK(fl.returnToFiltered(), QStringLiteral("возврат"));
    settleView(model, *view);
    CHECK(!fl.isActive() && model.logLevelFilter() == levels,
          QStringLiteral("возврат вернул ровно прежние фильтры"));
    CHECK(atPlace(*view, model, e, top0),
          QStringLiteral("возврат: запись выбрана на прежнем месте (верх %1) — %2")
              .arg(top0).arg(placeText(*view, model, e)));

    // 2. В полном логе выбрана строка контекста (её фильтр скрывает): возврат —
    //    к записи, с которой уходили, на её прежнее место; это переход
    //    истории, Alt+← — к ближайшей видимой к строке контекста.
    setContext(prefix + QStringLiteral("возврат со строки контекста"));
    fl.show();
    settleView(model, *view);
    const int ctx = contextRowNear(model, entryRow(model, e), levels);
    CHECK(ctx >= 0, QStringLiteral("предусловие: есть строка контекста"));
    if (ctx >= 0) {
        const RecordKey ctxKey = model.keyAt(ctx);
        view->goToRow(ctx);
        pump(30);
        fl.returnToFiltered();
        settleView(model, *view);
        CHECK(model.logLevelFilter() == levels, QStringLiteral("фильтры вернулись"));
        CHECK(atPlace(*view, model, e, top0),
              QStringLiteral("выбрана исходная запись на прежнем месте (верх %1) — %2")
                  .arg(top0).arg(placeText(*view, model, e)));
        const NavHistory& history = model.navHistory();
        CHECK(!history.entries().isEmpty() && history.entries().last().kind == NavKind::FullLog,
              QStringLiteral("возврат к исходной записи — переход FullLog"));
        const int back = model.navHistory().back(view->userSelectedRow());
        CHECK(back >= 0 && back == model.nearestRowForKey(ctxKey),
              QStringLiteral("Alt+← — к ближайшей видимой к строке контекста: %1, ждали %2")
                  .arg(back).arg(model.nearestRowForKey(ctxKey)));
        // Не уходили со строки — восстановление места, не переход.
        const qsizetype entries = history.entries().size();
        view->goToRow(entryRow(model, e));
        pump(30);
        const qsizetype afterGo = history.entries().size();
        fl.show();
        settleView(model, *view);
        fl.returnToFiltered();
        settleView(model, *view);
        CHECK(history.entries().size() == afterGo,
              QStringLiteral("показ и возврат без смены строки историю не пишут (%1 → %2, до %3)")
                  .arg(afterGo).arg(history.entries().size()).arg(entries));
    }

    // 3. Правка фильтра во время показа: новый фильтр — правда.
    setContext(prefix + QStringLiteral("правка фильтра во время показа"));
    {
        int changes = 0;
        const auto c = QObject::connect(&fl, &FullLogController::activeChanged,
                                        [&](bool) { ++changes; });
        fl.show();
        settleView(model, *view);
        model.setLogLevelFilter({ LogLevel::Warn });
        settleView(model, *view);
        CHECK(!fl.isActive() && model.logLevelFilter() == QSet<LogLevel>{ LogLevel::Warn },
              QStringLiteral("показ закончен, фильтр — новый"));
        CHECK(changes == 2, QStringLiteral("activeChanged: показ и конец (%1)").arg(changes));
        CHECK(!fl.returnToFiltered() && !fl.suspendedFilters(),
              QStringLiteral("приостановленные фильтры отброшены"));
        QObject::disconnect(c);
        // Reset всех фильтров во время показа: полный лог остаётся.
        fl.show();
        settleView(model, *view);
        fl.discard();
        CHECK(!fl.isActive() && !model.hasFilters(), QStringLiteral("discard оставляет полный лог"));
    }

    // 4. Показ посреди фонового фильтра: запись остаётся выбранной там, где
    //    пользователь её видел.
    setContext(prefix + QStringLiteral("показ посреди фильтра"));
    {
        clearFilters(model);
        view->goToRow(entryRow(model, e));
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->value() - 2 * rowH);
        pump(30);
        int top1 = rowRect(*view, entryRow(model, e)).top();
        model.setLogLevelFilter(levels);
        const bool filtering = model.isFiltering();
        if (!filtering) {
            // Синхронный фильтр уже применён, а место строки view ставит
            // отложенно: момента «посреди фильтра» у него нет — пользователь
            // видит отфильтрованный вид.
            settleView(model, *view);
            top1 = rowRect(*view, entryRow(model, e)).top();
        }
        CHECK(fl.show(), QStringLiteral("показ начат посреди фильтра (фоновый: %1)").arg(filtering));
        settleView(model, *view);
        CHECK(fl.isActive() && !model.hasFilters(), QStringLiteral("полный лог"));
        CHECK(atPlace(*view, model, e, top1),
              QStringLiteral("запись на месте (верх %1) — %2").arg(top1).arg(placeText(*view, model, e)));
        // Возврат и снова показ, не дожидаясь фильтров.
        fl.returnToFiltered();
        fl.show();
        fl.returnToFiltered();
        settleView(model, *view);
        CHECK(!fl.isActive() && model.logLevelFilter() == levels,
              QStringLiteral("нечётное число нажатий — отфильтрованный вид"));
        CHECK(atPlace(*view, model, e, top1),
              QStringLiteral("запись на месте после серии нажатий (верх %1) — %2")
                  .arg(top1).arg(placeText(*view, model, e)));
    }

    // 5. Пользователь прокрутил, пока фильтр в фоне: позиция не ставится.
    setContext(prefix + QStringLiteral("пользователь прокрутил раньше"));
    {
        fl.show();
        if (fl.isPlacementPending()) {
            QScrollBar* bar = view->verticalScrollBar();
            bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
            settleView(model, *view);
            const int value = bar->value();
            pump(50);
            CHECK(!fl.isPlacementPending() && bar->value() == value,
                  QStringLiteral("вид остался, где его оставил пользователь: %1 против %2")
                      .arg(bar->value()).arg(value));
        } else {
            settleView(model, *view);
        }
        fl.returnToFiltered();
        settleView(model, *view);
    }

    // 5а. Follow-tail включён, строка выбрана: показ снимает follow-tail и
    //     показывает её, а не хвост полного лога.
    setContext(prefix + QStringLiteral("follow-tail и выбранная строка"));
    {
        model.setLogLevelFilter(levels);
        settleView(model, *view);
        view->goToRow(entryRow(model, e));
        view->setFollowTail(true);
        pump(30);
        CHECK(view->followTail() && view->userSelectedRow() == entryRow(model, e),
              QStringLiteral("предусловие: follow-tail, запись выбрана"));
        fl.show();
        settleView(model, *view);
        const int row = entryRow(model, e);
        CHECK(!view->followTail() && view->userSelectedRow() == row && rowFullyVisible(*view, row),
              QStringLiteral("запись выбрана и видна, follow-tail снят: %1").arg(placeText(*view, model, e)));
        fl.returnToFiltered();
        settleView(model, *view);
    }

    // 6. Выделения нет: на месте остаётся верхняя строка экрана.
    setContext(prefix + QStringLiteral("без выделения — верхняя строка"));
    {
        auto plain = makeView(model);
        FullLogController fl2(model, *plain);
        activateWindowFocus(*plain);
        plain->scrollTo(model.index(qMin(entryRow(model, e), model.rowCount() - 1), 0),
                        QAbstractItemView::PositionAtTop);
        pump(30);
        const int top = topVisibleRow(*plain);
        const EntryInfo t = infoAt(model, top);
        const int topY = rowRect(*plain, top).top();
        CHECK(plain->userSelectedRow() < 0, QStringLiteral("предусловие: выделения нет"));
        fl2.show();
        settleView(model, *plain);
        const int row = entryRow(model, t);
        CHECK(row >= 0 && rowRect(*plain, row).top() == topY && plain->userSelectedRow() < 0,
              QStringLiteral("верхняя строка на месте: %1, верх %2 против %3, выбрана %4")
                  .arg(row).arg(row >= 0 ? rowRect(*plain, row).top() : -1).arg(topY)
                  .arg(plain->userSelectedRow()));
        fl2.returnToFiltered();
        settleView(model, *plain);
        const int back = entryRow(model, t);
        CHECK(back >= 0 && rowRect(*plain, back).top() == topY,
              QStringLiteral("возврат: верхняя строка на месте: %1, верх %2 против %3")
                  .arg(back).arg(back >= 0 ? rowRect(*plain, back).top() : -1).arg(topY));
    }
    clearFilters(model);
}

// Вкладка (LogViewWidget): плашка, снимок состояния, дозапись, переход порога,
// возврат кнопкой плашки, закладка, скрытая фильтром.
static void runFullLogTab(const QDir& dir, const QString& schema, bool indexed)
{
    AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
    const QString name = indexed ? QStringLiteral("индексный") : QStringLiteral("резидентный");
    const QDateTime base = fixtureBaseTime();
    const QString path = writeFile(dir, QStringLiteral("fulllog-%1.log").arg(indexed ? 1 : 0),
        makeLogBytes(2400, 21u, "\n", base, false, true, true));
    auto tab = std::make_unique<LogViewWidget>();
    tab->setParserPattern(schema);
    tab->resize(kViewWidth, kViewHeight);
    tab->show();
    tab->addLogFile(path);
    setContext(name + QStringLiteral(" / вкладка: полный лог"));
    CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
    LogModel& model = *tab->model();
    auto* bar = tab->findChild<QFrame*>(QStringLiteral("fullLogBar"));
    auto* returnButton = tab->findChild<QPushButton*>(QStringLiteral("returnToFilteredButton"));
    CHECK(bar && returnButton && !bar->isVisible(), QStringLiteral("плашка есть и скрыта"));
    if (!bar || !returnButton)
        return;

    CHECK(!tab->toggleFullLog(), QStringLiteral("без фильтров показывать нечего"));
    const QSet<LogLevel> levels = { LogLevel::Error, LogLevel::Fatal };
    model.setLogLevelFilter(levels);
    CHECK(tabSettled(*tab), QStringLiteral("фильтр"));
    const int row = model.rowCount() / 2;
    tab->view()->goToRow(row);
    pump(30);
    const RecordKey e = model.keyAt(row);
    const auto selectedLine = [&]() {
        const int r = tab->view()->userSelectedRow();
        return r >= 0 ? model.keyAt(r).line : -1;
    };

    int signals_ = 0;
    QObject::connect(tab.get(), &LogViewWidget::fullLogViewChanged, [&](bool) { ++signals_; });
    CHECK(tab->toggleFullLog(), QStringLiteral("показ"));
    CHECK(tabSettled(*tab), QStringLiteral("показ: фильтр"));
    CHECK(tab->isFullLogShown() && bar->isVisible() && signals_ == 1 && !model.hasFilters(),
          QStringLiteral("показ идёт, плашка видна (%1 сигналов)").arg(signals_));
    CHECK(selectedLine() == e.line, QStringLiteral("запись выбрана: %1 против %2").arg(selectedLine()).arg(e.line));

    // Снимок во время показа — настоящие фильтры.
    const TabState state = tab->captureState();
    CHECK(state.view.levels == levels, QStringLiteral("captureState снимает приостановленные фильтры"));

    // Дозапись во время показа.
    setContext(name + QStringLiteral(" / вкладка: дозапись во время показа"));
    CHECK(appendToFile(path, makeLogBytes(40, 77u, "\n", base.addSecs(86400), false, true, true)),
          QStringLiteral("дозапись в файл"));
    const qint64 before = model.totalLineCount();
    tab->reloadChangedFiles(true);
    CHECK(waitFor([&] { return model.totalLineCount() > before; }) && tabSettled(*tab),
          QStringLiteral("хвост дочитан"));
    CHECK(tab->isFullLogShown() && bar->isVisible() && !model.hasFilters(),
          QStringLiteral("показ пережил дозапись"));
    CHECK(selectedLine() == e.line, QStringLiteral("запись выбрана после дозаписи: %1").arg(selectedLine()));

    // Переход порога во время показа (резидентная вкладка → индексная).
    if (!indexed) {
        setContext(name + QStringLiteral(" / вкладка: переход порога во время показа"));
        AppSettings::instance().setIndexedThresholdMB(0);
        CHECK(appendToFile(path, makeLogBytes(20, 78u, "\n", base.addSecs(2 * 86400), false, true, true)),
              QStringLiteral("дозапись в файл"));
        tab->reloadChangedFiles(true);
        CHECK(waitFor([&] { return model.isIndexedBackend(); }) && tabSettled(*tab),
              QStringLiteral("вкладка стала индексной"));
        CHECK(tab->isFullLogShown() && bar->isVisible() && !model.hasFilters(),
              QStringLiteral("показ пережил переход порога"));
        CHECK(selectedLine() == e.line,
              QStringLiteral("запись выбрана после перехода: %1").arg(selectedLine()));
        AppSettings::instance().setIndexedThresholdMB(512);
    }

    // Возврат кнопкой плашки.
    setContext(name + QStringLiteral(" / вкладка: кнопка плашки"));
    returnButton->click();
    CHECK(tabSettled(*tab), QStringLiteral("возврат: фильтр"));
    CHECK(!tab->isFullLogShown() && !bar->isVisible() && model.logLevelFilter() == levels,
          QStringLiteral("возврат вернул фильтры, плашка ушла"));
    CHECK(selectedLine() == e.line, QStringLiteral("запись выбрана после возврата: %1").arg(selectedLine()));

    // applyState во время показа заканчивает его.
    setContext(name + QStringLiteral(" / вкладка: applyState во время показа"));
    tab->toggleFullLog();
    CHECK(tabSettled(*tab), QStringLiteral("показ"));
    tab->applyState(state);
    CHECK(tabSettled(*tab), QStringLiteral("applyState"));
    CHECK(!tab->isFullLogShown() && !bar->isVisible() && model.logLevelFilter() == levels,
          QStringLiteral("снимок применён, показ закончен"));

    // Закладка, скрытая фильтром: переход к ней — к ближайшей видимой, и
    // следующий показ выбирает её саму.
    setContext(name + QStringLiteral(" / вкладка: закладка, скрытая фильтром"));
    tab->toggleFullLog();
    CHECK(tabSettled(*tab), QStringLiteral("показ"));
    int hidden = -1;
    for (int r = model.rowCount() / 3; r < model.rowCount() && hidden < 0; ++r)
        if (!levels.contains(model.visibleLevelAt(r)))
            hidden = r;
    CHECK(hidden >= 0 && tab->toggleBookmark(hidden), QStringLiteral("закладка на скрываемой строке"));
    const qint64 hiddenLine = hidden >= 0 ? model.keyAt(hidden).line : -1;
    tab->toggleFullLog();
    CHECK(tabSettled(*tab), QStringLiteral("возврат"));
    const int id = tab->bookmarks().bookmarks().isEmpty() ? -1 : tab->bookmarks().bookmarks().last().id;
    QString message;
    QObject::connect(tab.get(), &LogViewWidget::statusMessage, [&](const QString& m) { message = m; });
    CHECK(tab->goToBookmarkId(id), QStringLiteral("переход к закладке"));
    CHECK(selectedLine() != hiddenLine && message.contains(QStringLiteral("Show in Full Log")),
          QStringLiteral("к ближайшей видимой, с подсказкой: «%1»").arg(message));
    CHECK(tab->toggleFullLog(), QStringLiteral("показ"));
    CHECK(tabSettled(*tab), QStringLiteral("показ: фильтр"));
    CHECK(selectedLine() == hiddenLine,
          QStringLiteral("выбрана сама закладка: %1 против %2").arg(selectedLine()).arg(hiddenLine));
    tab->toggleFullLog();
    CHECK(tabSettled(*tab), QStringLiteral("возврат"));
    AppSettings::instance().setIndexedThresholdMB(512);
}

// ---------------------------------------------------------------------------
// Восстановленная позиция против выбора пользователя (R-4)
// ---------------------------------------------------------------------------

static int lineRow(const LogModel& model, qint64 line)
{
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.keyAt(row).line == line)
            return row;
    return -1;
}

static bool tabSettled(LogViewWidget& tab)
{
    const bool ok = waitFor([&] { return !tab.isLoading() && !tab.isPositionPending(); })
        && settle(*tab.model());
    waitFor([&] { return !tab.view()->hasPendingLayout(); }, 5000);
    pump(30);
    return ok;
}

static void runRestoreVsUser(const QString& path, const QString& schema, bool indexed)
{
    AppSettings::instance().setIndexedThresholdMB(indexed ? 0 : 512);
    const QString name = indexed ? QStringLiteral("индексный") : QStringLiteral("резидентный");
    const auto makeTab = [&]() {
        auto tab = std::make_unique<LogViewWidget>();
        tab->setParserPattern(schema);
        tab->resize(kViewWidth, kViewHeight);
        tab->show();
        return tab;
    };
    // Снимок: выбрана строка 900.
    TabState state;
    {
        auto tab = makeTab();
        tab->addLogFile(path);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        tab->view()->goToRow(lineRow(*tab->model(), 900));
        pump(20);
        state = tab->captureState();
    }
    setContext(name + QStringLiteral(" / восстановление: без действий пользователя"));
    CHECK(state.view.hasCurrent() && state.view.current.line == 900, QStringLiteral("снимок со строкой"));
    {
        auto tab = makeTab();
        tab->applyState(state);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        const int row = lineRow(*tab->model(), 900);
        CHECK(row >= 0 && tab->view()->userSelectedRow() == row,
              QStringLiteral("позиция поставлена: выбрана %1").arg(tab->view()->userSelectedRow()));
    }

    // Пользователь выбрал строку раньше, чем локатор ответил: вкладка уже
    // загружена, ответ на применённое состояние — поздним проходом (позже).
    setContext(name + QStringLiteral(" / восстановление: пользователь выбрал строку раньше"));
    {
        auto tab = makeTab();
        tab->addLogFile(path);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        int finished = -1;
        QObject::connect(tab.get(), &LogViewWidget::positionRestoreFinished,
                         [&](bool applied) { finished = applied ? 1 : 0; });
        tab->applyState(state);
        CHECK(tab->isPositionPending(), QStringLiteral("сценарий: ответа ещё нет"));
        const int userRow = lineRow(*tab->model(), 300);
        tab->view()->goToRow(userRow);
        CHECK(tabSettled(*tab), QStringLiteral("ответ локатора"));
        CHECK(finished == 0, QStringLiteral("позиция не поставлена: %1").arg(finished));
        CHECK(tab->view()->userSelectedRow() == userRow,
              QStringLiteral("выбор пользователя на месте: %1, ждали %2")
                  .arg(tab->view()->userSelectedRow()).arg(userRow));
        CHECK(rowFullyVisible(*tab->view(), userRow), QStringLiteral("строка пользователя видна"));
    }

    // Пользователь прокрутил вид раньше ответа (ползунок).
    setContext(name + QStringLiteral(" / восстановление: пользователь прокрутил раньше"));
    {
        auto tab = makeTab();
        tab->addLogFile(path);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        tab->applyState(state);
        QScrollBar* bar = tab->view()->verticalScrollBar();
        bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
        bar->triggerAction(QAbstractSlider::SliderPageStepAdd);
        const int value = bar->value();
        CHECK(value > 0, QStringLiteral("сценарий: вид прокручен"));
        CHECK(tabSettled(*tab), QStringLiteral("ответ локатора"));
        CHECK(tab->view()->userSelectedRow() < 0 && bar->value() == value,
              QStringLiteral("вид остался, где его оставил пользователь: %1 против %2, выбрана %3")
                  .arg(bar->value()).arg(value).arg(tab->view()->userSelectedRow()));
    }

    // Пользователь действовал ещё до первых строк (новая вкладка, файл читается).
    setContext(name + QStringLiteral(" / восстановление: действие до первых строк"));
    {
        auto tab = makeTab();
        tab->applyState(state);
        QKeyEvent down(QEvent::KeyPress, Qt::Key_End, Qt::NoModifier);
        QApplication::sendEvent(tab->view(), &down);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        CHECK(tab->view()->userSelectedRow() != lineRow(*tab->model(), 900),
              QStringLiteral("восстановленная строка не выбрана поверх действия пользователя"));
    }

    // Модификатор сам по себе (начало Shift+F3) и Ctrl+C — не действие с видом.
    setContext(name + QStringLiteral(" / восстановление: модификатор и копирование не мешают"));
    {
        auto tab = makeTab();
        tab->applyState(state);
        QKeyEvent shift(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier);
        QApplication::sendEvent(tab->view(), &shift);
        QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(tab->view(), &copy);
        CHECK(tabSettled(*tab), QStringLiteral("загрузка"));
        const int row = lineRow(*tab->model(), 900);
        CHECK(row >= 0 && tab->view()->userSelectedRow() == row,
              QStringLiteral("позиция поставлена: выбрана %1").arg(tab->view()->userSelectedRow()));
    }
    AppSettings::instance().setIndexedThresholdMB(512);
}

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
        for (const double position : kPositions)
            runFullLogChecks(b, position);
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
        // Скроллбар сразу в самый низ, пока высоты последних строк ещё
        // оценки: после их уточнения вид обязан остаться у конца.
        setContext(b.name + QStringLiteral(", перенос строк / скроллбар до конца"));
        view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        pump(kViewTimersMs);
        CHECK(rowFullyVisible(*view, b.doc.model->rowCount() - 1),
              QStringLiteral("последняя строка не видна %1").arg(viewState(*view)));
        // Высоты строк уточняются лениво, но навигация и прокрутка обязаны
        // держать текущую строку на экране, а последняя строка — достижима.
        checkGeometry(b.name + QStringLiteral(", перенос строк"), *view, kViewTimersMs);
        checkKeyboard(b.name + QStringLiteral(", перенос строк"), *view);
    }

    runRestoreVsUser(smallLog, schema, false);
    runRestoreVsUser(smallLog, schema, true);
    runFullLogTab(dir, schema, false);
    runFullLogTab(dir, schema, true);

    setContext(QStringLiteral("итог"));
    if (g_failures > 0) {
        std::fprintf(stderr, "logview_state: %d проверок, %d НАРУШЕНИЙ\n", g_checks, g_failures);
        return 1;
    }
    std::fprintf(stdout, "logview_state: %d проверок, нарушений нет\n", g_checks);
    return 0;
}
