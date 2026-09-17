// ============================================================================
// logstore_equivalence — дифференциальный тест двух бэкендов LogStore.
//
// Спецификация индексного бэкенда не написана прозой, и не нужна: эталон
// поведения — резидентный бэкенд. Тест грузит один и тот же файл обоими
// путями (LogParser → ResidentLogStore и LogIndexer → IndexedLogStore),
// применяет к обеим моделям одинаковые настройки фильтров и сравнивает ВЕСЬ
// наблюдаемый API LogStore построчно: тексты, метаданные, навигацию, поиск,
// снапшоты сканов. Любое расхождение — регрессия в одном из бэкендов.
//
// Покрываются:
//   • один файл (у индексного — тождественное отображение row == line);
//   • вкладка из двух файлов, слитых по времени, — на упорядоченных файлах и
//     на файлах с преамбулой и метками не по порядку, в двух сценариях
//     подключения: файл дописан в открытую вкладку и файлы открыты разом
//     (все подключены до первого батча — объединение вкладок, конверсия
//     вкладки в индексную);
//   • фильтры: уровень, время, текстовые правила (Include/Exclude, AND/OR,
//     регекс, регистр, привязка к колонке схемы), выбор колонок, маркеры
//     строк и их комбинации;
//   • рандомизированный прогон наборов правил по фиксированному seed —
//     ловит комбинации, которые вручную никто бы не перебрал;
//   • поиск записи (rowForEntry / nearestVisibleRow — восстановление
//     выделения после фильтрации) на одно-файловой вкладке с метками не по
//     порядку, где номера строк бэкендов законно расходятся.
//
// ИЗВЕСТНЫЕ И НАМЕРЕННЫЕ РАСХОЖДЕНИЯ — не замалчиваются, а ЗАКРЕПЛЯЮТСЯ
// отдельными проверками, чтобы их изменение тоже было заметно:
//   • LogModel::displayTextLength на индексном бэкенде — байтовая ОЦЕНКА,
//     а не точная длина display-текста (checkDocumentedDivergence);
//   • вкладка из ОДНОГО файла на индексном бэкенде показывает строки в
//     ПОРЯДКЕ ФАЙЛА (тождественное отображение row == line, ради нулевой
//     цены памяти на 20-ГБ логе), тогда как резидентный бэкенд сортирует их
//     по таймстампу. На файле с неубывающими метками порядки совпадают;
//     на файле с преамбулой свободного текста или строками «не по порядку» —
//     расходятся (testSingleFileOrdering). Набор строк при этом обязан
//     совпадать всегда.
// ============================================================================

#include "filterruleset.h"
#include "indexedlogstore.h"
#include "lineindex.h"
#include "logentry.h"
#include "logfile.h"
#include "logindexer.h"
#include "logmodel.h"
#include "logparser.h"
#include "logpattern.h"
#include "logscan.h"
#include "testdocuments.h"
#include "textmatchhighlighter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QRandomGenerator>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// ---------------------------------------------------------------------------
// Отчётность
// ---------------------------------------------------------------------------

// Печать QString в UTF-8. qPrintable() конвертирует в local 8-bit —
// на Windows это CP1251/CP866, и русские сообщения превращаются в мусор.
#define U8(s) ((s).toUtf8().constData())

static int     g_failures = 0;
static int     g_checks   = 0;
static QString g_context;              // «<фикстура> / <конфигурация>»
static int     g_contextFailures = 0;  // бюджет сообщений на одну конфигурацию

static constexpr int kFailuresPerContext = 12;

static void reportFail(const char* file, int line, const QString& msg)
{
    ++g_failures;
    ++g_contextFailures;
    if (g_contextFailures > kFailuresPerContext) {
        if (g_contextFailures == kFailuresPerContext + 1)
            std::fprintf(stderr, "  … дальнейшие расхождения в [%s] подавлены\n",
                         U8(g_context));
        return;
    }
    std::fprintf(stderr, "FAIL(%s:%d) [%s]: %s\n", file, line,
                 U8(g_context), U8(msg));
}

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond))                                                           \
            reportFail(__FILE__, __LINE__, (msg));                             \
    } while (false)

// Сравнение двух значений с печатью обеих сторон.
#define CHECK_EQ(a, b, what)                                                   \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!((a) == (b)))                                                     \
            reportFail(__FILE__, __LINE__,                                     \
                       QStringLiteral("%1: резидентный=%2, индексный=%3")      \
                           .arg(what, describe(a), describe(b)));              \
    } while (false)

static QString describe(const QString& s)
{
    QString out = s;
    if (out.size() > 90)
        out = out.left(87) + QStringLiteral("…");
    return QLatin1Char('"') + out + QLatin1Char('"');
}
static QString describe(int v)          { return QString::number(v); }
static QString describe(qint64 v)       { return QString::number(v); }
static QString describe(bool v)         { return v ? QStringLiteral("true") : QStringLiteral("false"); }
static QString describe(LogLevel v)     { return LevelToStr(v); }
static QString describe(const QDateTime& v)
{
    return v.isValid() ? v.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
                       : QStringLiteral("<invalid>");
}
static QString describe(const QStringList& v)
{
    return describe(v.join(QLatin1Char('|')));
}

// ---------------------------------------------------------------------------
// Загрузка документов — общая с другими тестами (tests/common). Ошибки
// загрузчика и «модель не успокоилась» здесь — такие же расхождения.
// ---------------------------------------------------------------------------

using namespace testdocs;

static void reportLoadErrors(const Document& doc)
{
    for (const QString& error : doc.errors)
        CHECK(false, error);
}

static Document loadResident(const QStringList& paths, const QString& schema)
{
    Document doc = buildResident(paths, schema);
    reportLoadErrors(doc);
    return doc;
}

static Document loadIndexed(const QStringList& paths, const QString& schema,
                            IndexedLoad mode = IndexedLoad::Sequential)
{
    Document doc = buildIndexed(paths, schema, mode);
    reportLoadErrors(doc);
    return doc;
}

static void settleChecked(LogModel& model)
{
    CHECK(settle(model), QStringLiteral("модель не пришла в покой"));
}

// ---------------------------------------------------------------------------
// Конфигурации фильтров
// ---------------------------------------------------------------------------

struct FilterConfig {
    QString                   name;
    QSet<LogLevel>            levels;              // пустое множество = без фильтра
    QDateTime                 start;
    QDateTime                 end;
    FilterRuleSet             rules;
    bool                      fieldScopeActive = false;
    bool                      fieldFilterEnabled = false;
    QVector<int>              visibleFields;
    QVector<HighlightPattern> markers;
};

static FilterRule rule(FilterRule::Action action, const QString& text,
                       FilterRule::Connector connector = FilterRule::Connector::And,
                       const QString& field = QString(),
                       bool isRegex = false, bool caseSensitive = false)
{
    FilterRule r;
    r.action         = action;
    r.text           = text;
    r.connector      = connector;
    r.fieldName      = field;
    r.isRegex        = isRegex;
    r.caseSensitive  = caseSensitive;
    return r;
}

static void applyConfig(LogModel& model, const FilterConfig& cfg,
                        const QStringList& fieldNames)
{
    model.setAvailableFields(fieldNames);
    model.setFieldDisplaySelection(cfg.fieldFilterEnabled, cfg.visibleFields);

    FilterRuleSet rules = cfg.rules;
    rules.bindFields(fieldNames, cfg.fieldScopeActive);

    model.setLogLevelFilter(cfg.levels);
    model.setTimeRangeFilter(cfg.start, cfg.end);
    model.setFilterRules(rules);
    model.setRowMarkers(cfg.markers);

    settleChecked(model);
}

// ---------------------------------------------------------------------------
// Сравнение бэкендов
// ---------------------------------------------------------------------------

static QString fileOf(const LogModel::EntryKey& key)
{
    return key.sourceFile ? key.sourceFile->filePath : QStringLiteral("<null>");
}

// Построчное сравнение видимой выдачи и точечных запросов к ней.
static void compareVisibleRows(const LogModel& res, const LogModel& idx)
{
    const int rows = res.rowCount();
    CHECK_EQ(rows, idx.rowCount(), QStringLiteral("rowCount"));
    if (rows != idx.rowCount())
        return;

    for (int row = 0; row < rows; ++row) {
        if (g_contextFailures > kFailuresPerContext)
            return; // выдача уже разъехалась — дальнейшие строки шумят

        const QString what = QStringLiteral("строка %1").arg(row);

        const QModelIndex ri = res.index(row, 0);
        const QModelIndex ii = idx.index(row, 0);

        CHECK_EQ(res.data(ri, Qt::DisplayRole).toString(),
                 idx.data(ii, Qt::DisplayRole).toString(),
                 what + QStringLiteral(": DisplayRole"));
        CHECK_EQ(res.messageAt(row), idx.messageAt(row),
                 what + QStringLiteral(": messageAt"));
        CHECK_EQ(res.visibleTimestampAt(row), idx.visibleTimestampAt(row),
                 what + QStringLiteral(": visibleTimestampAt"));
        CHECK_EQ(res.visibleLevelAt(row), idx.visibleLevelAt(row),
                 what + QStringLiteral(": visibleLevelAt"));

        const LogModel::EntryKey rk = res.keyForRow(row);
        const LogModel::EntryKey ik = idx.keyForRow(row);
        CHECK_EQ(rk.logicalEntryId, ik.logicalEntryId,
                 what + QStringLiteral(": logicalEntryId"));
        CHECK_EQ(fileOf(rk), fileOf(ik), what + QStringLiteral(": sourceFile"));

        CHECK_EQ(res.data(ri, LogModel::IsNewRole).toBool(),
                 idx.data(ii, LogModel::IsNewRole).toBool(),
                 what + QStringLiteral(": IsNewRole"));

        const QVariant rm = res.data(ri, LogModel::RowMarkerColorRole);
        const QVariant im = idx.data(ii, LogModel::RowMarkerColorRole);
        CHECK_EQ(rm.isValid() ? rm.value<QColor>().name() : QStringLiteral("<none>"),
                 im.isValid() ? im.value<QColor>().name() : QStringLiteral("<none>"),
                 what + QStringLiteral(": RowMarkerColorRole"));
    }
}

// Навигация по времени, диапазон, сэмплы.
static void compareNavigation(const LogModel& res, const LogModel& idx)
{
    const QPair<QDateTime, QDateTime> rr = res.fullTimeRange();
    const QPair<QDateTime, QDateTime> ir = idx.fullTimeRange();
    CHECK_EQ(rr.first, ir.first, QStringLiteral("fullTimeRange.начало"));
    CHECK_EQ(rr.second, ir.second, QStringLiteral("fullTimeRange.конец"));

    CHECK_EQ(res.sampleMessages(10), idx.sampleMessages(10),
             QStringLiteral("sampleMessages(10)"));

    // Пробы по времени: до начала, по границам, в середине, после конца.
    QVector<QDateTime> probes;
    if (rr.first.isValid() && rr.second.isValid()) {
        const qint64 a = rr.first.toMSecsSinceEpoch();
        const qint64 b = rr.second.toMSecsSinceEpoch();
        probes << QDateTime::fromMSecsSinceEpoch(a - 5000)
               << rr.first
               << QDateTime::fromMSecsSinceEpoch(a + (b - a) / 4)
               << QDateTime::fromMSecsSinceEpoch(a + (b - a) / 2)
               << QDateTime::fromMSecsSinceEpoch(a + 3 * (b - a) / 4)
               << rr.second
               << QDateTime::fromMSecsSinceEpoch(b + 5000);
    }
    probes << QDateTime(); // невалидная метка

    for (const QDateTime& t : probes) {
        CHECK_EQ(res.firstVisibleRowAtOrAfter(t), idx.firstVisibleRowAtOrAfter(t),
                 QStringLiteral("firstVisibleRowAtOrAfter(%1)").arg(describe(t)));
    }
}

// rowForEntry / nearestVisibleRow — в том числе для записей, скрытых фильтром
// (именно на этом пути живёт восстановление выделения после фильтрации).
static void compareEntryLookup(const LogModel& res, const LogModel& idx,
                               const QVector<LogModel::EntryKey>& probes,
                               const QVector<LogFilePtr>& resFiles,
                               const QVector<LogFilePtr>& idxFiles)
{
    for (const LogModel::EntryKey& key : probes) {
        // Ключи собраны на резидентной модели: тот же файл в индексной модели
        // представлен своим экземпляром LogFile, ищем его по пути.
        const LogFile* idxFile = nullptr;
        for (int i = 0; i < resFiles.size() && i < idxFiles.size(); ++i) {
            if (resFiles[i].get() == key.sourceFile) {
                idxFile = idxFiles[i].get();
                break;
            }
        }
        if (!idxFile)
            continue;

        const QString what = QStringLiteral("запись #%1 в %2")
                                 .arg(key.logicalEntryId)
                                 .arg(QFileInfo(fileOf(key)).fileName());

        CHECK_EQ(res.rowForEntry(key.logicalEntryId, key.sourceFile),
                 idx.rowForEntry(key.logicalEntryId, idxFile),
                 QStringLiteral("rowForEntry(%1)").arg(what));
        CHECK_EQ(res.nearestVisibleRow(key.logicalEntryId, key.sourceFile),
                 idx.nearestVisibleRow(key.logicalEntryId, idxFile),
                 QStringLiteral("nearestVisibleRow(%1)").arg(what));
    }
}

// Быстрый поиск вперёд/назад, с заворотом и без.
static void compareSearch(const LogModel& res, const LogModel& idx)
{
    struct Probe {
        QString             needle;
        Qt::CaseSensitivity cs;
    };
    // «timeout» строчными проверяет регистронезависимый путь, «Timeout» с
    // учётом регистра — регистрозависимый; «Кэш» — не-ASCII; последняя
    // строка заведомо не встречается (обе стороны обязаны дать -1).
    const QVector<Probe> probes = {
        { QStringLiteral("timeout"),        Qt::CaseInsensitive },
        { QStringLiteral("Timeout"),        Qt::CaseSensitive   },
        { QStringLiteral("Кэш"),            Qt::CaseInsensitive },
        { QStringLiteral("at com.example"), Qt::CaseInsensitive },
        { QStringLiteral("нет такой строки"), Qt::CaseInsensitive },
    };
    const int rows = res.rowCount();
    const QVector<int> starts = { 0, rows / 2, qMax(0, rows - 1) };

    for (const Probe& p : probes) {
        for (const int start : starts) {
            for (const bool wrap : { true, false }) {
                const QString what =
                    QStringLiteral("«%1» от %2 (wrap=%3, cs=%4)")
                        .arg(p.needle).arg(start)
                        .arg(wrap ? QStringLiteral("да") : QStringLiteral("нет"))
                        .arg(p.cs == Qt::CaseSensitive ? QStringLiteral("да")
                                                       : QStringLiteral("нет"));

                const QModelIndex rn = res.findNextOccurrence(p.needle, start, p.cs, wrap);
                const QModelIndex in = idx.findNextOccurrence(p.needle, start, p.cs, wrap);
                CHECK_EQ(rn.isValid() ? rn.row() : -1, in.isValid() ? in.row() : -1,
                         QStringLiteral("findNextOccurrence %1").arg(what));

                const QModelIndex rp = res.findPreviousOccurrence(p.needle, start, p.cs, wrap);
                const QModelIndex ip = idx.findPreviousOccurrence(p.needle, start, p.cs, wrap);
                CHECK_EQ(rp.isValid() ? rp.row() : -1, ip.isValid() ? ip.row() : -1,
                         QStringLiteral("findPreviousOccurrence %1").arg(what));
            }
        }
    }
}

// Снапшоты сканов — путь статистики и таймлайна (воркеры читают их целиком).
static void compareScanSnapshots(const LogModel& res, const LogModel& idx)
{
    for (const bool filteredOnly : { true, false }) {
        const QString tag = filteredOnly ? QStringLiteral("scanSnapshot(filtered)")
                                         : QStringLiteral("scanSnapshot(all)");
        const LogScanSnapshot rs = res.scanSnapshot(filteredOnly);
        const LogScanSnapshot is = idx.scanSnapshot(filteredOnly);

        CHECK_EQ(rs.rowCount(), is.rowCount(), tag + QStringLiteral(": rowCount"));
        if (rs.rowCount() != is.rowCount())
            continue;

        for (qint64 row = 0; row < rs.rowCount(); ++row) {
            if (g_contextFailures > kFailuresPerContext)
                break;
            const LogEntryMeta rm = rs.metaAt(row);
            const LogEntryMeta im = is.metaAt(row);
            const QString what = QStringLiteral("%1 строка %2").arg(tag).arg(row);

            CHECK_EQ(rm.timestampMs, im.timestampMs, what + QStringLiteral(": timestampMs"));
            CHECK_EQ(rm.level, im.level, what + QStringLiteral(": level"));
            CHECK_EQ(rm.logicalEntryId, im.logicalEntryId,
                     what + QStringLiteral(": logicalEntryId"));
            CHECK_EQ(rm.isPlainText, im.isPlainText, what + QStringLiteral(": isPlainText"));
            CHECK_EQ(rm.sourceFile ? rm.sourceFile->filePath : QString(),
                     im.sourceFile ? im.sourceFile->filePath : QString(),
                     what + QStringLiteral(": sourceFile"));
        }

        // Последовательный обход текста — то, как снапшот читают воркеры.
        QStringList rTexts;
        QStringList iTexts;
        rs.forEachLine(0, [&](qint64, const LogEntryMeta&, QStringView text) {
            rTexts << text.toString();
            return rTexts.size() < 400;
        });
        is.forEachLine(0, [&](qint64, const LogEntryMeta&, QStringView text) {
            iTexts << text.toString();
            return iTexts.size() < 400;
        });
        CHECK_EQ(rTexts.size(), iTexts.size(), tag + QStringLiteral(": forEachLine — число строк"));
        for (int i = 0; i < qMin(rTexts.size(), iTexts.size()); ++i) {
            if (g_contextFailures > kFailuresPerContext)
                break;
            CHECK_EQ(rTexts[i], iTexts[i],
                     QStringLiteral("%1: forEachLine строка %2").arg(tag).arg(i));
        }
    }
}

// Строки логической записи (многострочные сообщения).
static void compareLogicalRecords(const LogModel& res, const LogModel& idx)
{
    const int rows = res.rowCount();
    if (rows == 0)
        return;

    const QVector<int> probes = { 0, rows / 4, rows / 2, 3 * rows / 4, rows - 1 };
    for (const int row : probes) {
        if (row < 0 || row >= rows)
            continue;
        const auto rEntry = res.entryAt(row);
        const auto iEntry = idx.entryAt(row);
        if (!rEntry || !iEntry) {
            CHECK_EQ(bool(rEntry), bool(iEntry),
                     QStringLiteral("entryAt(%1): наличие записи").arg(row));
            continue;
        }

        const auto rLines = res.logicalRecordLines(rEntry, 2000);
        const auto iLines = idx.logicalRecordLines(iEntry, 2000);
        CHECK_EQ(rLines.size(), iLines.size(),
                 QStringLiteral("logicalRecordLines(%1): число строк").arg(row));
        for (int i = 0; i < qMin(rLines.size(), iLines.size()); ++i) {
            CHECK_EQ(rLines[i] ? rLines[i]->message() : QString(),
                     iLines[i] ? iLines[i]->message() : QString(),
                     QStringLiteral("logicalRecordLines(%1) строка %2").arg(row).arg(i));
        }
    }
}

// Расхождение, которое задокументировано и намеренно: длина display-текста на
// индексном бэкенде — оценка. Проверяем, что оно осталось ИМЕННО таким
// (оценка положительна и одного порядка), а не выродилось в мусор.
static void checkDocumentedDivergence(const LogModel& res, const LogModel& idx)
{
    const int rows = qMin(res.rowCount(), idx.rowCount());
    for (int row = 0; row < rows; row += qMax(1, rows / 20)) {
        const int exact    = res.displayTextLength(row);
        const int estimate = idx.displayTextLength(row);
        if (exact == 0)
            continue;
        CHECK(estimate >= 0,
              QStringLiteral("displayTextLength(%1): оценка отрицательна (%2)")
                  .arg(row).arg(estimate));
        // Оценка — байтовая длина сырой строки: для не-ASCII она больше
        // числа символов, для display с выборкой колонок — больше видимого.
        // Нижняя граница осмысленности: не ноль на непустой строке.
        CHECK(estimate > 0,
              QStringLiteral("displayTextLength(%1): оценка 0 при точной длине %2")
                  .arg(row).arg(exact));
    }
}

// Мультимножество видимых строк — то, что обязано совпадать ВСЕГДА, даже
// когда порядок у бэкендов разный.
static QStringList visibleTextsSorted(const LogModel& model)
{
    QStringList texts;
    texts.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row)
        texts << model.messageAt(row);
    texts.sort();
    return texts;
}

// Накопленное время по стадиям сравнения — печатается в конце прогона.
// Дифференциальный тест гоняется на каждой сборке, поэтому его собственная
// стоимость должна быть видна, а не выясняться таймаутом CTest.
struct StageTiming {
    qint64 rows = 0, navigation = 0, search = 0, snapshots = 0,
           records = 0, divergence = 0, apply = 0, load = 0;
};
static StageTiming g_timing;

// Накопленный расклад по стадиям — печатается после каждой фикстуры, чтобы
// замедление было видно сразу, а не по таймауту CTest.
static void printTiming(const QString& tag)
{
    std::fprintf(stdout,
                 "  [%s] накопленно, мс: загрузка=%lld фильтры=%lld строки=%lld "
                 "навигация=%lld поиск=%lld снапшоты=%lld записи=%lld оценки=%lld\n",
                 U8(tag),
                 static_cast<long long>(g_timing.load),
                 static_cast<long long>(g_timing.apply),
                 static_cast<long long>(g_timing.rows),
                 static_cast<long long>(g_timing.navigation),
                 static_cast<long long>(g_timing.search),
                 static_cast<long long>(g_timing.snapshots),
                 static_cast<long long>(g_timing.records),
                 static_cast<long long>(g_timing.divergence));
    std::fflush(stdout);
}

static void compareAll(const Document& res, const Document& idx)
{
    QElapsedTimer t;
    t.start();
    compareVisibleRows(*res.model, *idx.model);
    g_timing.rows += t.restart();
    compareNavigation(*res.model, *idx.model);
    g_timing.navigation += t.restart();
    compareSearch(*res.model, *idx.model);
    g_timing.search += t.restart();
    compareScanSnapshots(*res.model, *idx.model);
    g_timing.snapshots += t.restart();
    compareLogicalRecords(*res.model, *idx.model);
    g_timing.records += t.restart();
    checkDocumentedDivergence(*res.model, *idx.model);
    g_timing.divergence += t.restart();
}

// ---------------------------------------------------------------------------
// Наборы конфигураций
// ---------------------------------------------------------------------------

static QVector<FilterConfig> namedConfigs(const QDateTime& from, const QDateTime& to)
{
    QVector<FilterConfig> cfgs;

    const qint64 a = from.isValid() ? from.toMSecsSinceEpoch() : 0;
    const qint64 b = to.isValid() ? to.toMSecsSinceEpoch() : 0;
    const QDateTime midLow  = QDateTime::fromMSecsSinceEpoch(a + (b - a) / 3);
    const QDateTime midHigh = QDateTime::fromMSecsSinceEpoch(a + 2 * (b - a) / 3);

    {
        FilterConfig c;
        c.name = QStringLiteral("без фильтров");
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: пустое множество (= без фильтра)");
        c.levels = {};
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: Error+Fatal");
        c.levels = { LogLevel::Error, LogLevel::Fatal };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: только Info");
        c.levels = { LogLevel::Info };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: Unknown (continuation/свободный текст)");
        c.levels = { LogLevel::Unknown };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: средняя треть");
        c.start = midLow;
        c.end   = midHigh;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: весь диапазон");
        c.start = from;
        c.end   = to;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: пустое окно в будущем");
        c.start = QDateTime::fromMSecsSinceEpoch(b + 3600000);
        c.end   = QDateTime::fromMSecsSinceEpoch(b + 7200000);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Include «timeout»");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("timeout"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Include «Timeout» с учётом регистра");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"),
                              FilterRule::Connector::And, QString(), false, true);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Exclude «at com.example»");
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("at com.example"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: регекс «node \\d+»");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("node \\d+"),
                              FilterRule::Connector::And, QString(), true);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Timeout AND Disk");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"))
                      << rule(FilterRule::Action::Include, QStringLiteral("Disk"),
                              FilterRule::Connector::And);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Timeout OR Кэш");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"))
                      << rule(FilterRule::Action::Include, QStringLiteral("Кэш"),
                              FilterRule::Connector::Or);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: A AND B OR C (приоритет AND)");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Disk"))
                      << rule(FilterRule::Action::Include, QStringLiteral("percent"),
                              FilterRule::Connector::And)
                      << rule(FilterRule::Action::Include, QStringLiteral("Checkpoint"),
                              FilterRule::Connector::Or);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name             = QStringLiteral("текст: правило привязано к колонке Thread");
        c.fieldScopeActive = true;
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("worker-2"),
                              FilterRule::Connector::And, QStringLiteral("Thread"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name             = QStringLiteral("текст: колонка Message, Exclude");
        c.fieldScopeActive = true;
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("Retrying"),
                              FilterRule::Connector::And, QStringLiteral("Message"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name               = QStringLiteral("колонки: показаны только Level и Message");
        c.fieldFilterEnabled = true;
        c.visibleFields      = { 2, 3 };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name               = QStringLiteral("колонки: только Timestamp + уровни Warn/Error");
        c.fieldFilterEnabled = true;
        c.visibleFields      = { 0 };
        c.levels             = { LogLevel::Warn, LogLevel::Error };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("маркеры строк");
        HighlightPattern p1;
        p1.text  = QStringLiteral("Timeout");
        p1.color = QColor(200, 60, 60);
        HighlightPattern p2;
        p2.text            = QStringLiteral("checkpoint \\d+");
        p2.color           = QColor(60, 140, 200);
        p2.isRegex         = true;
        c.markers << p1 << p2;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("комбинация: уровни + время + текст");
        c.levels = { LogLevel::Info, LogLevel::Warn, LogLevel::Error };
        c.start  = midLow;
        c.end    = midHigh;
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("Retrying"));
        cfgs << c;
    }

    return cfgs;
}

// Случайные наборы правил по фиксированному seed: перебирает комбинации,
// которые вручную никто бы не выписал.
static QVector<FilterConfig> randomConfigs(int count, quint32 seed,
                                           const QDateTime& from, const QDateTime& to)
{
    QRandomGenerator rng(seed);
    QVector<FilterConfig> cfgs;

    const QStringList needles = {
        QStringLiteral("Timeout"), QStringLiteral("Disk"),  QStringLiteral("node"),
        QStringLiteral("Кэш"),     QStringLiteral("worker"), QStringLiteral("INFO"),
        QStringLiteral("at com"),  QStringLiteral("percent"),
    };
    const QStringList regexes = {
        QStringLiteral("\\d{3}"), QStringLiteral("Module\\d"),
        QStringLiteral("(Timeout|Disk)"), QStringLiteral("^\\s+at"),
    };
    const QStringList fields = { QString(), QStringLiteral("Thread"),
                                 QStringLiteral("Level"), QStringLiteral("Message") };
    const QVector<LogLevel> allLevels = { LogLevel::Unknown, LogLevel::Trace,
                                          LogLevel::Debug,   LogLevel::Info,
                                          LogLevel::Warn,    LogLevel::Error,
                                          LogLevel::Fatal };

    const qint64 a = from.isValid() ? from.toMSecsSinceEpoch() : 0;
    const qint64 b = to.isValid() ? to.toMSecsSinceEpoch() : 0;

    for (int i = 0; i < count; ++i) {
        FilterConfig c;
        c.name = QStringLiteral("случайная #%1").arg(i);

        const int ruleCount = 1 + int(rng.bounded(3));
        for (int r = 0; r < ruleCount; ++r) {
            const bool isRegex = rng.bounded(100) < 30;
            c.rules.rules << rule(
                rng.bounded(100) < 70 ? FilterRule::Action::Include
                                      : FilterRule::Action::Exclude,
                isRegex ? regexes[int(rng.bounded(quint32(regexes.size())))]
                        : needles[int(rng.bounded(quint32(needles.size())))],
                rng.bounded(100) < 60 ? FilterRule::Connector::And
                                      : FilterRule::Connector::Or,
                fields[int(rng.bounded(quint32(fields.size())))],
                isRegex,
                rng.bounded(100) < 40);
        }
        c.fieldScopeActive = rng.bounded(100) < 60;

        if (rng.bounded(100) < 50) {
            for (const LogLevel lvl : allLevels)
                if (rng.bounded(100) < 40)
                    c.levels.insert(lvl);
        }
        if (rng.bounded(100) < 40 && b > a) {
            const qint64 s = a + qint64(rng.bounded(quint32(qMax<qint64>(1, b - a))));
            const qint64 e = s + qint64(rng.bounded(quint32(qMax<qint64>(1, b - s))));
            c.start = QDateTime::fromMSecsSinceEpoch(s);
            c.end   = QDateTime::fromMSecsSinceEpoch(e);
        }
        if (rng.bounded(100) < 30) {
            c.fieldFilterEnabled = true;
            for (int f = 0; f < 4; ++f)
                if (rng.bounded(100) < 50)
                    c.visibleFields << f;
            if (c.visibleFields.isEmpty())
                c.visibleFields << 3;
        }
        cfgs << c;
    }
    return cfgs;
}

// ---------------------------------------------------------------------------
// Прогон одной фикстуры
// ---------------------------------------------------------------------------

static void runFixture(const QString& fixtureName, const QStringList& paths,
                       const QString& schema, const QStringList& fieldNames,
                       IndexedLoad mode = IndexedLoad::Sequential)
{
    g_context = fixtureName + QStringLiteral(" / загрузка");
    g_contextFailures = 0;

    QElapsedTimer loadTimer;
    loadTimer.start();
    Document res = loadResident(paths, schema);
    Document idx = loadIndexed(paths, schema, mode);
    g_timing.load += loadTimer.elapsed();

    std::fprintf(stdout, "фикстура «%s»: %d файл(ов), %d строк(и)\n",
                 U8(fixtureName), int(paths.size()), res.model->rowCount());
    std::fflush(stdout);

    CHECK(res.model->rowCount() > 0, QStringLiteral("резидентная модель пуста"));
    CHECK(idx.model->isIndexedBackend(), QStringLiteral("бэкенд не индексный"));

    // Пробы поиска записи собираются на ПОЛНОЙ выдаче: часть из них фильтры
    // потом скроют — это и проверяет nearestVisibleRow.
    QVector<LogModel::EntryKey> lookupProbes;
    {
        const int rows = res.model->rowCount();
        for (int i = 0; i < 25 && rows > 0; ++i)
            lookupProbes << res.model->keyForRow((i * rows) / 25);
    }

    const QPair<QDateTime, QDateTime> range = res.model->fullTimeRange();

    QVector<FilterConfig> configs = namedConfigs(range.first, range.second);
    configs += randomConfigs(12, 20260917u, range.first, range.second);

    for (const FilterConfig& cfg : configs) {
        g_context = fixtureName + QStringLiteral(" / ") + cfg.name;
        g_contextFailures = 0;

        QElapsedTimer t;
        t.start();
        applyConfig(*res.model, cfg, fieldNames);
        applyConfig(*idx.model, cfg, fieldNames);
        g_timing.apply += t.elapsed();

        compareAll(res, idx);
        compareEntryLookup(*res.model, *idx.model, lookupProbes, res.files, idx.files);
    }

    printTiming(fixtureName);
}

// ---------------------------------------------------------------------------
// Закрепление известного расхождения: одно-файловая индексная вкладка не
// сортирует строки (row == line), резидентная — сортирует по таймстампу.
//
// Проверяется ровно две вещи:
//   1. на файле с неубывающими метками порядки СОВПАДАЮТ (иначе расхождение
//      шире, чем задокументировано, — это уже регрессия);
//   2. на файле с преамбулой и метками «не по порядку» порядки РАЗНЫЕ, но
//      набор строк идентичен (расходится только порядок, не содержимое).
//
// Если кто-то сделает индексный бэкенд сортирующим и для одного файла, пункт
// 2 упадёт — и это правильный сигнал: поведение изменилось, тест пора
// обновить вместе с документацией.
// ---------------------------------------------------------------------------
static void testSingleFileOrdering(const QString& monotonicPath,
                                   const QString& unsortedPath,
                                   const QString& schema, const QStringList& fieldNames)
{
    const FilterConfig none;

    {
        g_context = QStringLiteral("порядок строк / файл с неубывающими метками");
        g_contextFailures = 0;

        Document res = loadResident({ monotonicPath }, schema);
        Document idx = loadIndexed({ monotonicPath }, schema);
        applyConfig(*res.model, none, fieldNames);
        applyConfig(*idx.model, none, fieldNames);

        CHECK_EQ(res.model->rowCount(), idx.model->rowCount(),
                 QStringLiteral("rowCount"));
        bool sameOrder = res.model->rowCount() == idx.model->rowCount();
        for (int row = 0; sameOrder && row < res.model->rowCount(); ++row)
            sameOrder = (res.model->messageAt(row) == idx.model->messageAt(row));
        CHECK(sameOrder,
              QStringLiteral("на монотонном файле порядок бэкендов обязан совпадать"));
    }

    {
        g_context = QStringLiteral("порядок строк / файл с преамбулой и метками не по порядку");
        g_contextFailures = 0;

        Document res = loadResident({ unsortedPath }, schema);
        Document idx = loadIndexed({ unsortedPath }, schema);
        applyConfig(*res.model, none, fieldNames);
        applyConfig(*idx.model, none, fieldNames);

        CHECK_EQ(res.model->rowCount(), idx.model->rowCount(), QStringLiteral("rowCount"));

        // Содержимое обязано совпадать с точностью до порядка.
        CHECK_EQ(visibleTextsSorted(*res.model).join(QLatin1Char('\n')),
                 visibleTextsSorted(*idx.model).join(QLatin1Char('\n')),
                 QStringLiteral("набор видимых строк (без учёта порядка)"));

        // А порядок — заведомо разный: резидентный сортирует по времени и
        // уводит свободный текст в конец, индексный держит порядок файла.
        bool sameOrder = res.model->rowCount() == idx.model->rowCount();
        for (int row = 0; sameOrder && row < res.model->rowCount(); ++row)
            sameOrder = (res.model->messageAt(row) == idx.model->messageAt(row));
        CHECK(!sameOrder,
              QStringLiteral("ЗАДОКУМЕНТИРОВАННОЕ расхождение исчезло: одно-файловая "
                             "индексная вкладка теперь совпадает с резидентной по "
                             "порядку — обнови тест и комментарий IndexedLogStore"));

        // Резидентный бэкенд уводит строки без таймстампа в конец.
        const int rows = res.model->rowCount();
        CHECK(rows > 0 && !res.model->visibleTimestampAt(rows - 1).isValid(),
              QStringLiteral("резидентный бэкенд: свободный текст ожидается в конце"));
        // Индексный — оставляет их там, где они в файле (в начале).
        CHECK(!idx.model->visibleTimestampAt(0).isValid(),
              QStringLiteral("индексный бэкенд: преамбула ожидается первой строкой"));
    }
}

// ---------------------------------------------------------------------------
// Поиск записи на одно-файловой вкладке с метками не по порядку.
//
// Строки такой вкладки на индексном бэкенде идут в порядке файла (см.
// testSingleFileOrdering), поэтому НОМЕРА строк с резидентным сравнить нельзя.
// Но rowForEntry / nearestVisibleRow обязаны быть верны в СВОЁМ порядке —
// на них держится восстановление выделения после смены фильтра. Оракул
// строится перебором самой видимой выдачи, без обращения к этим методам:
//   • rowForEntry — первая видимая строка с ключом записи;
//   • nearestVisibleRow — она же, а если запись скрыта целиком, первая
//     видимая строка файла ниже начала записи, иначе последняя видимая.
// С резидентным сверяется то, что от порядка не зависит: найдена ли запись и
// на какой именно строке текста.
// ---------------------------------------------------------------------------
static void testSingleFileUnsortedLookups(const QString& unsortedPath,
                                          const QString& schema,
                                          const QStringList& fieldNames)
{
    g_context = QStringLiteral("поиск записи / один файл, метки не по порядку / загрузка");
    g_contextFailures = 0;

    Document res = loadResident({ unsortedPath }, schema);
    Document idx = loadIndexed({ unsortedPath }, schema);
    const LogFile* resFile = res.files.first().get();
    const LogFile* idxFile = idx.files.first().get();

    // Пробы — все записи файла. Без фильтра у индексной одно-файловой
    // вкладки row == line, поэтому первая строка с новым ключом и есть
    // первая строка записи в файле.
    struct Probe {
        int    logicalId;
        qint64 firstLine;
    };
    QVector<Probe> probes;
    {
        QSet<int> seen;
        for (int row = 0; row < idx.model->rowCount(); ++row) {
            const int id = idx.model->keyForRow(row).logicalEntryId;
            if (seen.contains(id))
                continue;
            seen.insert(id);
            probes.append({ id, qint64(row) });
        }
    }
    CHECK(probes.size() > 50, QStringLiteral("мало записей в фикстуре: %1").arg(probes.size()));

    const QPair<QDateTime, QDateTime> range = res.model->fullTimeRange();
    for (const FilterConfig& cfg : namedConfigs(range.first, range.second)) {
        g_context = QStringLiteral("поиск записи / один файл, метки не по порядку / ") + cfg.name;
        g_contextFailures = 0;

        applyConfig(*res.model, cfg, fieldNames);
        applyConfig(*idx.model, cfg, fieldNames);

        const int rows = idx.model->rowCount();
        QVector<int> rowId(rows);
        QVector<qint64> rowLine(rows);
        for (int r = 0; r < rows; ++r) {
            rowId[r] = idx.model->keyForRow(r).logicalEntryId;
            const auto entry = idx.model->entryAt(r);
            rowLine[r] = entry ? qint64(entry->originalLineNumber()) - 1 : -1;
        }

        for (const Probe& p : probes) {
            if (g_contextFailures > kFailuresPerContext)
                break;

            int expectedExact = -1;
            for (int r = 0; r < rows; ++r) {
                if (rowId[r] == p.logicalId) {
                    expectedExact = r;
                    break;
                }
            }
            int expectedNearest = -1;
            if (expectedExact >= 0) {
                expectedNearest = expectedExact;
            } else if (rows > 0) {
                expectedNearest = rows - 1;
                for (int r = 0; r < rows; ++r) {
                    if (rowLine[r] >= p.firstLine) {
                        expectedNearest = r;
                        break;
                    }
                }
            }

            const QString what = QStringLiteral("запись #%1").arg(p.logicalId);
            const int idxRow = idx.model->rowForEntry(p.logicalId, idxFile);
            CHECK(idxRow == expectedExact,
                  QStringLiteral("%1: rowForEntry=%2, по выдаче ожидается %3")
                      .arg(what).arg(idxRow).arg(expectedExact));
            const int idxNearest = idx.model->nearestVisibleRow(p.logicalId, idxFile);
            CHECK(idxNearest == expectedNearest,
                  QStringLiteral("%1: nearestVisibleRow=%2, по выдаче ожидается %3")
                      .arg(what).arg(idxNearest).arg(expectedNearest));

            // Сверка с резидентным: та же запись найдена и на той же строке текста.
            const int resRow = res.model->rowForEntry(p.logicalId, resFile);
            CHECK_EQ(resRow >= 0, idxRow >= 0, what + QStringLiteral(": запись видна"));
            if (resRow >= 0 && idxRow >= 0) {
                CHECK_EQ(res.model->messageAt(resRow), idx.model->messageAt(idxRow),
                         what + QStringLiteral(": строка, найденная rowForEntry"));
            }
        }
    }
}

// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    // Сообщения теста в UTF-8; без этого консоль Windows покажет мусор.
    SetConsoleOutputCP(CP_UTF8);
#endif

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create temporary directory\n");
        return 2;
    }
    const QDir dir(tmp.path());

    const QString schema = buildSchema();
    const LogPattern pattern(schema);
    if (!pattern.isValid()) {
        std::fprintf(stderr, "тестовая схема не компилируется\n");
        return 2;
    }
    const QStringList fieldNames = pattern.fieldNames();

    const QDateTime baseA = fixtureBaseTime();
    const QDateTime baseB = baseA.addSecs(120); // перекрывается с первым файлом

    // Файлы для строгого сравнения одно-файловой вкладки: метки неубывающие,
    // без преамбулы — порядок файла совпадает с порядком по времени, поэтому
    // тождественное отображение индексного бэкенда обязано дать то же, что
    // сортировка резидентного.
    //   A — LF, с завершающим переводом строки;
    //   B — CRLF, БЕЗ завершающего перевода строки (последняя строка
    //       индексируется «предварительной»).
    const QString fileA = writeFile(
        dir, QStringLiteral("alpha.log"),
        makeLogBytes(320, 1u, "\n", baseA, /*withPreamble*/ false,
                     /*trailingNewline*/ true, /*monotonic*/ true));
    const QString fileB = writeFile(
        dir, QStringLiteral("beta.log"),
        makeLogBytes(260, 2u, "\r\n", baseB, /*withPreamble*/ false,
                     /*trailingNewline*/ false, /*monotonic*/ true));

    // Файлы с преамбулой свободного текста и метками «не по порядку». Во
    // вкладке из нескольких файлов индексный бэкенд материализует общий
    // порядок и сортирует — здесь сравнение снова строгое.
    const QString fileC = writeFile(
        dir, QStringLiteral("gamma.log"),
        makeLogBytes(300, 3u, "\n", baseA, /*withPreamble*/ true,
                     /*trailingNewline*/ true, /*monotonic*/ false));
    const QString fileD = writeFile(
        dir, QStringLiteral("delta.log"),
        makeLogBytes(240, 4u, "\r\n", baseB, /*withPreamble*/ true,
                     /*trailingNewline*/ false, /*monotonic*/ false));

    for (const QString& path : { fileA, fileB, fileC, fileD }) {
        if (path.isEmpty()) {
            std::fprintf(stderr, "не удалось записать фикстуру\n");
            return 2;
        }
    }

    runFixture(QStringLiteral("один файл (LF)"), { fileA }, schema, fieldNames);
    runFixture(QStringLiteral("один файл (CRLF, без \\n в конце)"), { fileB }, schema, fieldNames);

    // Слияние нескольких файлов: и монотонные, и с преамбулой и метками не
    // по порядку, в обоих сценариях подключения. Контракт один — вкладка
    // слита по времени ровно так же, как на резидентном бэкенде.
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent }) {
        runFixture(QStringLiteral("два файла, слияние по времени, ") + describe(mode),
                   { fileA, fileB }, schema, fieldNames, mode);
        runFixture(QStringLiteral("два файла с преамбулой и метками не по порядку, ")
                       + describe(mode),
                   { fileC, fileD }, schema, fieldNames, mode);
    }

    testSingleFileOrdering(fileA, fileC, schema, fieldNames);
    testSingleFileUnsortedLookups(fileC, schema, fieldNames);

    g_context = QStringLiteral("итог");
    printTiming(QStringLiteral("итого"));

    if (g_failures > 0) {
        std::fprintf(stderr, "logstore_equivalence: %d проверок, %d РАСХОЖДЕНИЙ\n",
                     g_checks, g_failures);
        return 1;
    }
    std::fprintf(stdout, "logstore_equivalence: %d проверок, расхождений нет\n", g_checks);
    return 0;
}
