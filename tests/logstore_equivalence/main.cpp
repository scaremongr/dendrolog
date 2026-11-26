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
#include "recordid.h"
#include "filterconfigs.h"
#include "keyedfixtures.h"
#include "testdocuments.h"
#include "textmatchhighlighter.h"
#include "timehistogram.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QRandomGenerator>
#include <QSet>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>
#include <limits>
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

// Конфигурации фильтров (FilterConfig, namedConfigs, randomConfigs) — общие
// с tests/bookmarks, в tests/common/filterconfigs.h.
// Здесь у моделей нет view, а документ загружен: конец фильтра — это
// !isFiltering(), окно тишины settle() ничего не ловит (P-10).
constexpr int kFilterQuietMs = 0;
// Правило по полю решается на записи (F-17, шаг 3): когда все действующие
// правила — по полю (уровни, время и файл — тоже на записи), запись видна
// целиком или не видна вовсе: строки-продолжения следуют за своей записью.
// До шага 3 продолжение давало «не содержит» — стек-трейс прятался от
// Include своей записи и оставался от Exclude скрытой (ожидание изменено
// осознанно, docs/design/F-17.md, «Сделано → шаг 3»).
static void checkRecordsWhole(const LogModel& model, const QString& what)
{
    const FilterRuleSet& rules = model.filterRules();
    bool fieldOnly = rules.isActive();
    for (int i = 0; i < rules.rules.size() && fieldOnly; ++i) {
        const FilterRule& r = rules.rules[i];
        if (r.isActive() && (r.fieldName.isEmpty() || rules.fieldMissing(i)))
            fieldOnly = false;
    }
    if (!fieldOnly)
        return;
    using Key = QPair<const LogFile*, int>;
    QHash<Key, int> total, shown;
    const LogScanSnapshot all = model.scanSnapshot(false);
    for (qint64 row = 0; row < all.rowCount(); ++row) {
        const LogEntryMeta meta = all.metaAt(row);
        ++total[Key(meta.sourceFile, meta.logicalEntryId)];
    }
    for (int row = 0; row < model.rowCount(); ++row) {
        const LogEntryMeta meta = model.visibleMetaAt(row);
        ++shown[Key(meta.sourceFile, meta.logicalEntryId)];
    }
    int broken = 0;
    for (auto it = shown.cbegin(); it != shown.cend(); ++it)
        broken += it.value() == total.value(it.key()) ? 0 : 1;
    CHECK(broken == 0, what + QStringLiteral(": правило по полю — запись видна целиком (%1 записей разорваны)")
                                  .arg(broken));
}

static void applyChecked(LogModel& model, const FilterConfig& cfg,
                         const QStringList& fieldNames)
{
    CHECK(testdocs::applyConfig(model, cfg, fieldNames, kFilterQuietMs),
          QStringLiteral("модель не пришла в покой"));
    checkRecordsWhole(model, cfg.name);
}

// ---------------------------------------------------------------------------
// Сравнение бэкендов
// ---------------------------------------------------------------------------

// Сравнение двух индексных вкладок на одинаковом тексте в разных кодировках
// (F-1): файлы лежат в разных каталогах под одним именем, сверяется имя.
static bool g_fileNameOnly = false;

static QString filePathForCompare(const LogFile* file)
{
    if (!file)
        return QString();
    return g_fileNameOnly ? QFileInfo(file->filePath).fileName() : file->filePath;
}

static QString fileOf(const LogModel::EntryKey& key)
{
    return key.sourceFile ? filePathForCompare(key.sourceFile) : QStringLiteral("<null>");
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

        // Место записи среди видимых строк (старт быстрого поиска): одинаково
        // на обоих бэкендах и согласовано с rowForEntry / nearestVisibleRow —
        // nearest это то же место, прижатое к последней строке.
        bool resVisible = false, idxVisible = false;
        const int resPlace = res.placeAmongVisible(key.logicalEntryId, key.sourceFile, &resVisible);
        const int idxPlace = idx.placeAmongVisible(key.logicalEntryId, idxFile, &idxVisible);
        CHECK_EQ(resPlace, idxPlace, QStringLiteral("placeAmongVisible(%1)").arg(what));
        CHECK_EQ(resVisible, idxVisible, QStringLiteral("placeAmongVisible visible(%1)").arg(what));
        for (const LogModel* m : {&res, &idx}) {
            const LogFile* file = m == &res ? key.sourceFile : idxFile;
            bool visible = false;
            const int place = m->placeAmongVisible(key.logicalEntryId, file, &visible);
            const int exact = m->rowForEntry(key.logicalEntryId, file);
            const int nearest = m->nearestVisibleRow(key.logicalEntryId, file);
            const int rows = m->rowCount();
            const bool consistent = visible
                ? place == exact && exact >= 0
                : exact < 0 && place <= rows
                      && (rows == 0 || place < 0 || nearest == qMin(place, rows - 1));
            CHECK(consistent, QStringLiteral("placeAmongVisible согласовано с rowForEntry/"
                                             "nearestVisibleRow (%1): place=%2 exact=%3 nearest=%4 rows=%5")
                                  .arg(what).arg(place).arg(exact).arg(nearest).arg(rows));
        }
    }
}

// ---------------------------------------------------------------------------
// Ключ строки (R-1): keyAt / rowForKey / nearestRowForKey.
// ---------------------------------------------------------------------------

// Все строки документа без фильтра — снимаются сразу после загрузки, пока
// фильтр не применён: часть из них конфигурации потом скроют.
struct AllLines {
    QVector<RecordKey> keys;   // в порядке вкладки
    QVector<int>       fileIndex; // номер файла ключа в Document::files
};

static AllLines collectAllLines(const Document& doc)
{
    AllLines all;
    const int rows = doc.model->rowCount();
    all.keys.reserve(rows);
    all.fileIndex.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const RecordKey key = doc.model->keyAt(row);
        int fi = -1;
        for (int i = 0; i < doc.files.size(); ++i)
            if (doc.files[i] == key.file)
                fi = i;
        all.keys.append(key);
        all.fileIndex.append(fi);
    }
    return all;
}

static QString describe(const RecordKey& k)
{
    if (!k.isValid())
        return QStringLiteral("<нет ключа>");
    return QStringLiteral("%1:%2 #%3 @%4")
        .arg(QFileInfo(k.file->filePath).fileName())
        .arg(k.line)
        .arg(k.logicalEntryId)
        .arg(k.hasTimestamp() ? QString::number(k.timestampMs) : QStringLiteral("-"));
}

// Ключи одного бэкенда против перебора, без обращения к поиску по ключу:
// видимая строка — по равенству ключей при проходе всех строк вкладки вместе
// с видимыми (видимые — упорядоченная подпоследовательность всех), место
// скрытой — число видимых перед ней. Порядок вкладки у каждого бэкенда свой,
// поэтому сверка годится и для одно-файловой вкладки «не по порядку».
static void checkKeysAgainstScan(const LogModel& m, const AllLines& all, const QString& backend)
{
    const int rows = m.rowCount();
    QVector<RecordKey> visible(rows);
    for (int r = 0; r < rows; ++r) {
        visible[r] = m.keyAt(r);
        CHECK(visible[r].isValid(), QStringLiteral("%1: keyAt(%2) невалиден").arg(backend).arg(r));
        CHECK(m.rowForKey(visible[r]) == r,
              QStringLiteral("%1: rowForKey(keyAt(%2) = %3) = %4")
                  .arg(backend).arg(r).arg(describe(visible[r])).arg(m.rowForKey(visible[r])));
    }
    CHECK(!m.keyAt(-1).isValid() && !m.keyAt(rows).isValid(),
          backend + QStringLiteral(": keyAt за границами обязан быть невалидным"));
    CHECK(m.rowForKey(RecordKey{}) == -1 && m.nearestRowForKey(RecordKey{}) == -1,
          backend + QStringLiteral(": невалидный ключ"));

    int j = 0;
    for (int i = 0; i < all.keys.size(); ++i) {
        if (g_contextFailures > kFailuresPerContext)
            return;
        const RecordKey& key = all.keys[i];
        const bool isVisible = j < rows && visible[j] == key;
        const int expectedExact = isVisible ? j : -1;
        const int expectedNearest = rows == 0 ? -1 : qMin(j, rows - 1);
        const int exact = m.rowForKey(key);
        const int nearest = m.nearestRowForKey(key);
        CHECK(exact == expectedExact,
              QStringLiteral("%1: rowForKey(%2) = %3, перебором %4")
                  .arg(backend, describe(key)).arg(exact).arg(expectedExact));
        CHECK(nearest == expectedNearest,
              QStringLiteral("%1: nearestRowForKey(%2) = %3, перебором %4")
                  .arg(backend, describe(key)).arg(nearest).arg(expectedNearest));

        // Ключ с чужой записью или меткой той же строки — не она (выборочно:
        // каждая пятая строка, цена прогона).
        if (i % 5 != 0) {
            if (isVisible)
                ++j;
            continue;
        }
        RecordKey stale = key;
        stale.logicalEntryId += 1;
        CHECK(m.rowForKey(stale) == -1 && m.nearestRowForKey(stale) == -1,
              QStringLiteral("%1: ключ с другим id записи нашёл строку (%2)")
                  .arg(backend, describe(key)));
        stale = key;
        stale.timestampMs = key.hasTimestamp() ? key.timestampMs + 1 : 0;
        CHECK(m.rowForKey(stale) == -1,
              QStringLiteral("%1: ключ с другой меткой нашёл строку (%2)")
                  .arg(backend, describe(key)));
        if (isVisible)
            ++j;
    }
}

// Сверка ключей между бэкендами. Строки сопоставляются по (файл, номер
// строки), а не по номеру ряда: так сверка годится и там, где порядок
// бэкендов законно расходится (sameOrder = false), — тогда сравниваются
// только видимость строки и сам ключ, ряды — нет.
static void compareRecordKeys(const Document& res, const Document& idx,
                              const AllLines& resAll, const AllLines& idxAll,
                              bool sameOrder)
{
    const LogModel& rm = *res.model;
    const LogModel& im = *idx.model;

    checkKeysAgainstScan(rm, resAll, QStringLiteral("резидентный"));
    checkKeysAgainstScan(im, idxAll, QStringLiteral("индексный"));

    QHash<QPair<int, qint64>, int> idxByLine;
    for (int i = 0; i < idxAll.keys.size(); ++i)
        idxByLine.insert(qMakePair(idxAll.fileIndex[i], idxAll.keys[i].line), i);
    CHECK_EQ(resAll.keys.size(), idxAll.keys.size(), QStringLiteral("число строк документа"));

    for (int i = 0; i < resAll.keys.size(); ++i) {
        if (g_contextFailures > kFailuresPerContext)
            return;
        const RecordKey& rk = resAll.keys[i];
        const auto found = idxByLine.constFind(qMakePair(resAll.fileIndex[i], rk.line));
        CHECK(found != idxByLine.constEnd(),
              QStringLiteral("строки %1 нет на индексном бэкенде").arg(describe(rk)));
        if (found == idxByLine.constEnd())
            continue;
        const RecordKey& ik = idxAll.keys[found.value()];
        const QString what = describe(rk);

        CHECK_EQ(rk.logicalEntryId, ik.logicalEntryId, what + QStringLiteral(": id записи"));
        CHECK_EQ(rk.timestampMs, ik.timestampMs, what + QStringLiteral(": метка"));
        CHECK_EQ(rk.file->filePath, ik.file->filePath, what + QStringLiteral(": файл"));

        const int rr = rm.rowForKey(rk);
        const int ir = im.rowForKey(ik);
        CHECK_EQ(rr >= 0, ir >= 0, what + QStringLiteral(": строка видна"));
        if (rr >= 0 && ir >= 0) {
            CHECK_EQ(rm.messageAt(rr), im.messageAt(ir), what + QStringLiteral(": текст строки"));
            CHECK_EQ(rm.keyAt(rr).line, im.keyAt(ir).line, what + QStringLiteral(": найденная строка"));
        }
        if (sameOrder) {
            CHECK_EQ(rr, ir, QStringLiteral("rowForKey(%1)").arg(what));
            CHECK_EQ(rm.nearestRowForKey(rk), im.nearestRowForKey(ik),
                     QStringLiteral("nearestRowForKey(%1)").arg(what));
        }
    }

    // Видимые ряды: ключ один и тот же на обоих бэкендах.
    if (sameOrder && rm.rowCount() == im.rowCount()) {
        for (int row = 0; row < rm.rowCount(); ++row) {
            if (g_contextFailures > kFailuresPerContext)
                return;
            const RecordKey rk = rm.keyAt(row);
            const RecordKey ik = im.keyAt(row);
            const QString what = QStringLiteral("keyAt(%1)").arg(row);
            CHECK_EQ(rk.line, ik.line, what + QStringLiteral(".line"));
            CHECK_EQ(rk.logicalEntryId, ik.logicalEntryId, what + QStringLiteral(".logicalEntryId"));
            CHECK_EQ(rk.timestampMs, ik.timestampMs, what + QStringLiteral(".timestampMs"));
            CHECK_EQ(rk.file ? rk.file->filePath : QString(), ik.file ? ik.file->filePath : QString(),
                     what + QStringLiteral(".file"));
        }
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
        // Кириллица в регистре, отличном от текста (F-1: фикстуры кодировок).
        { QStringLiteral("ОШИБКА"),         Qt::CaseInsensitive },
        { QStringLiteral("ошибка"),         Qt::CaseSensitive   },
        { QStringLiteral("пользователь"),   Qt::CaseInsensitive },
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

// Переход по уровню (F8 / Shift+F8): оба бэкенда против эталона, а не друг
// против друга — общую ошибку семантики сравнение бэкендов не увидело бы.
// Эталон задан иначе, чем реализация: «кандидаты в порядке обхода, первый
// подходящий». Подходящая строка — нужного уровня и первая показанная строка
// своей записи (продолжения наследуют уровень и целью не служат).
static int expectedRowWithLevel(const QVector<bool>& target, int startRow, bool forward,
                                bool wrap)
{
    const int n = int(target.size());
    QVector<int> order;
    const bool inside = startRow >= 0 && startRow < n;
    if (forward) {
        for (int r = 0; r < n; ++r)
            if (r > startRow) order << r;
        if (wrap)
            for (int r = 0; r < n; ++r)
                if (r <= startRow) order << r;
    } else if (!inside) {
        for (int r = n - 1; r >= 0; --r)
            order << r;               // без опорной строки — с конца, всё подряд
    } else {
        for (int r = n - 1; r >= 0; --r)
            if (r < startRow) order << r;
        if (wrap)
            for (int r = n - 1; r >= 0; --r)
                if (r >= startRow) order << r;
    }
    for (const int r : std::as_const(order))
        if (target[r]) return r;
    return -1;
}

static void compareLevelSearch(const LogModel& res, const LogModel& idx)
{
    const int rows = res.rowCount();
    if (rows != idx.rowCount())
        return; // уже сообщено в compareVisibleRows
    // Уровни и записи строк — один раз (их равенство между бэкендами сверено
    // там же).
    QVector<LogLevel> rowLevels(rows);
    QVector<bool> startsRecord(rows);
    LogModel::EntryKey previous;
    for (int row = 0; row < rows; ++row) {
        rowLevels[row] = res.visibleLevelAt(row);
        const LogModel::EntryKey key = res.keyForRow(row);
        startsRecord[row] = row == 0 || key.logicalEntryId != previous.logicalEntryId
                            || key.sourceFile != previous.sourceFile;
        previous = key;
    }
    // Ошибки (F8), предупреждения, уровень без строк в части конфигураций,
    // пустая маска и «все уровни».
    const QVector<LogLevelMask> masks = {
        levelBit(LogLevel::Error) | levelBit(LogLevel::Fatal),
        levelBit(LogLevel::Warn),
        levelBit(LogLevel::Fatal),
        levelBit(LogLevel::Unknown),
        0,
        ~LogLevelMask(0),
    };
    const QVector<int> starts = { -1, 0, 1, rows / 3, rows / 2, qMax(0, rows - 2),
                                  qMax(0, rows - 1), rows, rows + 7 };
    for (const LogLevelMask mask : masks) {
        QVector<bool> target(rows);
        for (int row = 0; row < rows; ++row)
            target[row] = startsRecord[row] && (levelBit(rowLevels[row]) & mask);
        for (const int start : starts) {
            for (const bool forward : { true, false }) {
                for (const bool wrap : { true, false }) {
                    const QString what =
                        QStringLiteral("findRowWithLevel(маска %1, от %2, %3, wrap=%4)")
                            .arg(mask, 0, 16).arg(start)
                            .arg(forward ? QStringLiteral("вперёд") : QStringLiteral("назад"))
                            .arg(wrap ? QStringLiteral("да") : QStringLiteral("нет"));
                    const int expected = expectedRowWithLevel(target, start, forward, wrap);
                    CHECK_EQ(res.findRowWithLevel(mask, start, forward, wrap), expected,
                             what + QStringLiteral(": резидентный против эталона"));
                    CHECK_EQ(idx.findRowWithLevel(mask, start, forward, wrap), expected,
                             what + QStringLiteral(": индексный против эталона"));
                }
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
            CHECK_EQ(filePathForCompare(rm.sourceFile), filePathForCompare(im.sourceFile),
                     what + QStringLiteral(": sourceFile"));
        }

        // Диапазон меток (S-13) — настоящие минимум и максимум, одинаковые
        // на обоих бэкендах.
        qint64 rMin = 0, rMax = 0, iMin = 0, iMax = 0;
        const bool rHas = rs.timeRange(&rMin, &rMax);
        const bool iHas = is.timeRange(&iMin, &iMax);
        CHECK_EQ(rHas, iHas, tag + QStringLiteral(": timeRange — есть метки"));
        if (rHas && iHas) {
            CHECK_EQ(rMin, iMin, tag + QStringLiteral(": timeRange — минимум"));
            CHECK_EQ(rMax, iMax, tag + QStringLiteral(": timeRange — максимум"));
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
           records = 0, divergence = 0, apply = 0, load = 0, levelSearch = 0,
           keys = 0;
};
static StageTiming g_timing;

// Накопленный расклад по стадиям — печатается после каждой фикстуры, чтобы
// замедление было видно сразу, а не по таймауту CTest.
static void printTiming(const QString& tag)
{
    std::fprintf(stdout,
                 "  [%s] накопленно, мс: загрузка=%lld фильтры=%lld строки=%lld "
                 "навигация=%lld поиск=%lld уровни=%lld снапшоты=%lld записи=%lld оценки=%lld "
                 "ключи=%lld\n",
                 U8(tag),
                 static_cast<long long>(g_timing.load),
                 static_cast<long long>(g_timing.apply),
                 static_cast<long long>(g_timing.rows),
                 static_cast<long long>(g_timing.navigation),
                 static_cast<long long>(g_timing.search),
                 static_cast<long long>(g_timing.levelSearch),
                 static_cast<long long>(g_timing.snapshots),
                 static_cast<long long>(g_timing.records),
                 static_cast<long long>(g_timing.divergence),
                 static_cast<long long>(g_timing.keys));
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
    compareLevelSearch(*res.model, *idx.model);
    g_timing.levelSearch += t.restart();
    compareScanSnapshots(*res.model, *idx.model);
    g_timing.snapshots += t.restart();
    compareLogicalRecords(*res.model, *idx.model);
    g_timing.records += t.restart();
    checkDocumentedDivergence(*res.model, *idx.model);
    g_timing.divergence += t.restart();
}

// ---------------------------------------------------------------------------
// Прогон одной фикстуры
// ---------------------------------------------------------------------------

// Прогон загруженной пары документов во всех конфигурациях фильтров (плюс
// extra); afterEach — дополнительные проверки после каждой конфигурации.
static void runLoaded(const QString& fixtureName, Document& res, Document& idx,
                      const QStringList& fieldNames, const QVector<FilterConfig>& extra = {},
                      const std::function<void(const FilterConfig&)>& afterEach = {})
{
    std::fprintf(stdout, "фикстура «%s»: %d файл(ов), %d строк(и)\n",
                 U8(fixtureName), int(res.files.size()), res.model->rowCount());
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

    const AllLines resAll = collectAllLines(res);
    const AllLines idxAll = collectAllLines(idx);

    const QPair<QDateTime, QDateTime> range = res.model->fullTimeRange();

    QVector<FilterConfig> configs = namedConfigs(range.first, range.second);
    configs += randomConfigs(kRandomConfigCount, kRandomConfigSeed, range.first, range.second);
    configs += extra;

    for (const FilterConfig& cfg : configs) {
        g_context = fixtureName + QStringLiteral(" / ") + cfg.name;
        g_contextFailures = 0;

        QElapsedTimer t;
        t.start();
        applyChecked(*res.model, cfg, fieldNames);
        applyChecked(*idx.model, cfg, fieldNames);
        g_timing.apply += t.elapsed();

        compareAll(res, idx);
        compareEntryLookup(*res.model, *idx.model, lookupProbes, res.files, idx.files);
        t.restart();
        compareRecordKeys(res, idx, resAll, idxAll, /*sameOrder=*/true);
        g_timing.keys += t.elapsed();
        if (afterEach)
            afterEach(cfg);
    }

    printTiming(fixtureName);
}

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
    // Одна схема на всё: колонки вкладки — колонки схемы, columnOf
    // тождественен (F-5) — результат прежний.
    CHECK_EQ(res.layout.columns, fieldNames, QStringLiteral("колонки вкладки = колонки схемы"));
    CHECK_EQ(idx.layout.columns, fieldNames, QStringLiteral("колонки индексной = колонки схемы"));
    runLoaded(fixtureName, res, idx, fieldNames);
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
        applyChecked(*res.model, none, fieldNames);
        applyChecked(*idx.model, none, fieldNames);

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
        applyChecked(*res.model, none, fieldNames);
        applyChecked(*idx.model, none, fieldNames);

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
// S-13. Диапазон таймлайна и статистики на файле с метками не по порядку.
//
// Одно-файловая индексная вкладка держит порядок файла (testSingleFileOrdering),
// и диапазон меток раньше брался по первой и последней строке с меткой — записи
// вне него прижимались к крайним корзинам. Теперь у обоих бэкендов (и у одной,
// и у слитой вкладки, с фильтром и без, после дозаписи с новым минимумом и
// максимумом) диапазон — настоящие минимум и максимум, а корзины таймлайна
// (TimeHistogram) — те, что даёт перебор метаданных снапшота. Эталон —
// перебор, не другой бэкенд: общую ошибку сравнение бэкендов не увидело бы.
// Заодно — арифметика корзин при времени фильтра вне годов 0001–9999.
// ---------------------------------------------------------------------------
struct BruteRange {
    bool   has = false;
    qint64 lo = (std::numeric_limits<qint64>::max)();
    qint64 hi = kNoTimestampMs;
};

static BruteRange bruteRange(const LogScanSnapshot& snap)
{
    BruteRange r;
    for (qint64 row = 0; row < snap.rowCount(); ++row) {
        const qint64 ts = snap.metaAt(row).timestampMs;
        if (!hasTimestampMs(ts))
            continue;
        r.has = true;
        r.lo = qMin(r.lo, ts);
        r.hi = qMax(r.hi, ts);
    }
    return r;
}

// Корзины перебором: логическая запись — серия строк одной записи одного файла.
static QVector<quint32> bruteBins(const LogScanSnapshot& snap, qint64 tMin, qint64 tMax,
                                  int bins, bool errorsOnly)
{
    QVector<quint32> out(bins, 0);
    const qint64 span = qMax<qint64>(1, tMax - tMin);
    int prevId = 0;
    const LogFile* prevFile = nullptr;
    for (qint64 row = 0; row < snap.rowCount(); ++row) {
        const LogEntryMeta m = snap.metaAt(row);
        const bool continuation = row > 0 && m.logicalEntryId == prevId && m.sourceFile == prevFile;
        prevId = m.logicalEntryId;
        prevFile = m.sourceFile;
        if (continuation || !hasTimestampMs(m.timestampMs))
            continue;
        if (errorsOnly && m.level != LogLevel::Error)
            continue;
        const qint64 ts = qBound(tMin, m.timestampMs, tMax);
        ++out[int(qMin<qint64>(bins - 1, (ts - tMin) * bins / span))];
    }
    return out;
}

static QVector<quint32> binsOf(const QVector<quint32>& prefix)
{
    QVector<quint32> out;
    for (int b = 0; b + 1 < prefix.size(); ++b)
        out << prefix[b + 1] - prefix[b];
    return out;
}

static QString describe(const QVector<quint32>& v)
{
    QStringList parts;
    for (quint32 x : v)
        parts << QString::number(x);
    return parts.join(QLatin1Char(','));
}

static void checkTimeRangeAgainstScan(const QString& what, const LogModel& m)
{
    constexpr int kTestBins = 64;
    for (const bool filteredOnly : { true, false }) {
        const QString tag = what + (filteredOnly ? QStringLiteral(" / видимые")
                                                 : QStringLiteral(" / все строки"));
        const LogScanSnapshot snap = m.scanSnapshot(filteredOnly);
        const BruteRange want = bruteRange(snap);
        qint64 lo = 0, hi = 0;
        const bool has = snap.timeRange(&lo, &hi);
        CHECK_EQ(has, want.has, tag + QStringLiteral(": timeRange — есть метки"));
        if (!has || !want.has)
            continue;
        CHECK_EQ(lo, want.lo, tag + QStringLiteral(": timeRange — минимум"));
        CHECK_EQ(hi, want.hi, tag + QStringLiteral(": timeRange — максимум"));

        if (!filteredOnly) {
            const QPair<QDateTime, QDateTime> full = m.fullTimeRange();
            CHECK_EQ(full.first.toMSecsSinceEpoch(), want.lo, tag + QStringLiteral(": fullTimeRange — от"));
            CHECK_EQ(full.second.toMSecsSinceEpoch(), want.hi, tag + QStringLiteral(": fullTimeRange — до"));
            continue;
        }

        // Таймлайн без фильтра времени: шкала — диапазон данных, корзины — перебор.
        const TimeHistogram h = TimeHistogram::build(snap, kTestBins, 0, 0, false);
        CHECK(h.hasData && h.tMin == want.lo && h.tMax == want.hi,
              tag + QStringLiteral(": шкала таймлайна %1 — %2, ожидалось %3 — %4")
                        .arg(h.tMin).arg(h.tMax).arg(want.lo).arg(want.hi));
        CHECK_EQ(describe(binsOf(h.totalPrefix)), describe(bruteBins(snap, want.lo, want.hi, kTestBins, false)),
                 tag + QStringLiteral(": корзины таймлайна"));
        CHECK_EQ(describe(binsOf(h.errorPrefix)), describe(bruteBins(snap, want.lo, want.hi, kTestBins, true)),
                 tag + QStringLiteral(": корзины ошибок"));

        // Фильтр времени вне годов 0001–9999 (его задаёт пользователь, QDateTime
        // — сотни миллионов лет): шкала прижата к поддерживаемым годам, ни
        // одна запись не потеряна, корзины — перебор на той же шкале (UBSan
        // ловит переполнение в санитайзерной сборке).
        const qint64 farPast = QDateTime(QDate(-200000, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const qint64 farFuture = QDateTime(QDate(200000, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        const struct { qint64 from, to; const char* name; } filters[] = {
            { farPast, farFuture, "годы -200000…200000" },
            { (std::numeric_limits<qint64>::min)() + 1, (std::numeric_limits<qint64>::max)(), "края qint64" },
            { farPast, want.lo + 1, "только начало вне диапазона" },
        };
        for (const auto& f : filters) {
            const TimeHistogram fh = TimeHistogram::build(snap, kTestBins, f.from, f.to, true);
            const qint64 expMin = TimeBins::clampMs(qMin(f.from, want.lo));
            const qint64 expMax = TimeBins::clampMs(qMax(f.to, want.hi));
            CHECK(fh.hasData && fh.tMin == expMin && fh.tMax == expMax,
                  tag + QStringLiteral(": фильтр %1 — шкала %2 — %3, ожидалось %4 — %5")
                            .arg(QString::fromUtf8(f.name)).arg(fh.tMin).arg(fh.tMax)
                            .arg(expMin).arg(expMax));
            CHECK_EQ(describe(binsOf(fh.totalPrefix)), describe(bruteBins(snap, expMin, expMax, kTestBins, false)),
                     tag + QStringLiteral(": фильтр ") + QString::fromUtf8(f.name) + QStringLiteral(": корзины"));
        }
    }
}

// Лог «не по порядку»: преамбула, записи через эпоху с шагами назад, минимум
// (1901 год) и максимум (2031 год) посреди файла, свободный текст в конце.
static QByteArray unorderedTimeBytes(quint32 seed)
{
    const QDateTime base(QDate(1969, 12, 31), QTime(23, 58));
    QByteArray out = makeLogBytes(140, seed, "\n", base, /*withPreamble*/ true,
                                  /*trailingNewline*/ true, /*monotonic*/ false);
    out += "1901-03-05 10:00:00.000 [main] ERROR - far minimum in the middle\n"
           "    at com.example.Deep.call(Deep.java:1)\n";
    out += makeLogBytes(120, seed + 1, "\n", base.addSecs(30), false, true, false);
    out += "2031-01-02 03:04:05.006 [main] WARN - far maximum in the middle\n";
    out += makeLogBytes(100, seed + 2, "\n", base.addSecs(-30), false, true, false);
    out += "free text at the end, no stamp\n";
    return out;
}

static void testUnorderedTimeRange(const QDir& dir, const QString& schema,
                                   const QStringList& fieldNames, const QString& otherPath)
{
    const QByteArray bytes = unorderedTimeBytes(31u);
    const QString resPath = writeFile(dir, QStringLiteral("s13-res.log"), bytes);
    const QString idxPath = writeFile(dir, QStringLiteral("s13-idx.log"), bytes);
    const QString sharedPath = writeFile(dir, QStringLiteral("s13-shared.log"), bytes);

    // Первая и последняя строка с меткой — не минимум и не максимум: иначе
    // проверка ничего не ловит.
    {
        Document idx = loadIndexed({ sharedPath }, schema);
        const int rows = idx.model->rowCount();
        qint64 first = kNoTimestampMs, last = kNoTimestampMs;
        for (int row = 0; row < rows; ++row) {
            const QDateTime t = idx.model->visibleTimestampAt(row);
            if (!t.isValid())
                continue;
            if (!hasTimestampMs(first))
                first = t.toMSecsSinceEpoch();
            last = t.toMSecsSinceEpoch();
        }
        const BruteRange all = bruteRange(idx.model->scanSnapshot(false));
        g_context = QStringLiteral("S-13 / фикстура");
        CHECK(all.has && first != all.lo && last != all.hi && all.lo < 0,
              QStringLiteral("фикстура: первая/последняя метка %1/%2 против минимума/максимума %3/%4")
                  .arg(first).arg(last).arg(all.lo).arg(all.hi));
    }

    const auto runConfigs = [&](const QString& name, Document& res, Document& idx) {
        const QPair<QDateTime, QDateTime> range = res.model->fullTimeRange();
        for (const FilterConfig& cfg : allConfigs(range.first, range.second)) {
            g_context = QStringLiteral("S-13 / ") + name + QStringLiteral(" / ") + cfg.name;
            g_contextFailures = 0;
            applyChecked(*res.model, cfg, fieldNames);
            applyChecked(*idx.model, cfg, fieldNames);
            checkTimeRangeAgainstScan(QStringLiteral("резидентный"), *res.model);
            checkTimeRangeAgainstScan(QStringLiteral("индексный"), *idx.model);
            // Шкала и корзины от порядка вкладки не зависят — бэкенды равны.
            const TimeHistogram rh = TimeHistogram::build(res.model->scanSnapshot(true), 64, 0, 0, false);
            const TimeHistogram ih = TimeHistogram::build(idx.model->scanSnapshot(true), 64, 0, 0, false);
            CHECK(rh.tMin == ih.tMin && rh.tMax == ih.tMax && rh.totalPrefix == ih.totalPrefix
                      && rh.errorPrefix == ih.errorPrefix && rh.warnPrefix == ih.warnPrefix,
                  QStringLiteral("таймлайн резидентной и индексной вкладки различается"));
        }
    };

    {
        Document res = loadResident({ sharedPath }, schema);
        Document idx = loadIndexed({ sharedPath }, schema);
        runConfigs(QStringLiteral("один файл"), res, idx);
    }
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent }) {
        Document res = loadResident({ sharedPath, otherPath }, schema);
        Document idx = loadIndexed({ sharedPath, otherPath }, schema, mode);
        runConfigs(QStringLiteral("слитая вкладка, ") + describe(mode), res, idx);
    }

    // Дозапись хвоста: новый минимум и новый максимум посреди дописанного,
    // у индексной — сперва оборванной последней строкой (предварительная,
    // при следующей дозаписи переиндексируется — truncateFrom).
    g_context = QStringLiteral("S-13 / дозапись");
    g_contextFailures = 0;
    Document res = loadResident({ resPath }, schema);
    Document idx = loadIndexed({ idxPath }, schema);
    const QByteArray tail1 =
        "2026-03-05 10:00:00.000 [main] INFO - appended\n"
        "1900-06-01 00:00:00.000 [main] ERROR - new minimum\n"
        "2026-03-05 10:00:01.000 [main] INFO - appended again\n";
    const QByteArray tail2a = "2045-07-08 09:10:11.012 [main] WARN - new max";
    const QByteArray tail2b = "imum, completed\n2026-03-05 10:00:02.000 [main] INFO - last\n";
    CHECK(appendResidentTail(res, 0, tail1, schema), QStringLiteral("резидентная дозапись 1"));
    CHECK(appendIndexedTail(idx, 0, tail1, schema), QStringLiteral("индексная дозапись 1"));
    checkTimeRangeAgainstScan(QStringLiteral("резидентный после дозаписи"), *res.model);
    checkTimeRangeAgainstScan(QStringLiteral("индексный после дозаписи"), *idx.model);
    qint64 lo = 0, hi = 0;
    CHECK(idx.model->scanSnapshot(false).timeRange(&lo, &hi)
              && lo == QDateTime(QDate(1900, 6, 1), QTime(0, 0)).toMSecsSinceEpoch(),
          QStringLiteral("индексный: новый минимум из дозаписи"));

    CHECK(appendIndexedTail(idx, 0, tail2a, schema), QStringLiteral("индексная дозапись 2a (оборванная)"));
    checkTimeRangeAgainstScan(QStringLiteral("индексный, оборванная строка"), *idx.model);
    CHECK(appendIndexedTail(idx, 0, tail2b, schema), QStringLiteral("индексная дозапись 2b"));
    CHECK(appendResidentTail(res, 0, tail2a + tail2b, schema), QStringLiteral("резидентная дозапись 2"));
    checkTimeRangeAgainstScan(QStringLiteral("резидентный после дозаписи 2"), *res.model);
    checkTimeRangeAgainstScan(QStringLiteral("индексный после дозаписи 2"), *idx.model);
    const qint64 newMax = QDateTime(QDate(2045, 7, 8), QTime(9, 10, 11, 12)).toMSecsSinceEpoch();
    qint64 rlo = 0, rhi = 0;
    CHECK(idx.model->scanSnapshot(false).timeRange(&lo, &hi) && hi == newMax
              && res.model->scanSnapshot(false).timeRange(&rlo, &rhi) && rhi == newMax && rlo == lo,
          QStringLiteral("новый максимум из дозаписи: индексный %1, резидентный %2, ожидалось %3")
              .arg(hi).arg(rhi).arg(newMax));
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

    const AllLines resAll = collectAllLines(res);
    const AllLines idxAll = collectAllLines(idx);

    const QPair<QDateTime, QDateTime> range = res.model->fullTimeRange();
    for (const FilterConfig& cfg : namedConfigs(range.first, range.second)) {
        g_context = QStringLiteral("поиск записи / один файл, метки не по порядку / ") + cfg.name;
        g_contextFailures = 0;

        applyChecked(*res.model, cfg, fieldNames);
        applyChecked(*idx.model, cfg, fieldNames);

        // Ключ строки: каждый бэкенд в своём порядке, сверка — по строкам.
        compareRecordKeys(res, idx, resAll, idxAll, /*sameOrder=*/false);

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
// Файлы из разных часовых поясов в одной вкладке сливаются по МОМЕНТУ:
// 12:00:01.500+02:00 — это 10:00:01.500 UTC, между 10:00:00Z и 10:00:02Z.
// Эталон задан заранее, а не резидентным бэкендом: общая ошибка
// классификатора в обоих бэкендах иначе прошла бы незамеченной.
// ---------------------------------------------------------------------------
static void testZonedMerge(const QDir& dir, const QString& schema)
{
    QByteArray utc, plus2;
    QStringList expected;
    for (int i = 0; i < 10; ++i) {
        utc += QStringLiteral("2026-09-20T10:00:%1.000Z [u] INFO - utc %2\n")
                   .arg(2 * i, 2, 10, QLatin1Char('0')).arg(i).toUtf8();
        plus2 += QStringLiteral("2026-09-20T12:00:%1.500+02:00 [p] INFO - plus2 %2\n")
                     .arg(2 * i + 1, 2, 10, QLatin1Char('0')).arg(i).toUtf8();
        expected << QStringLiteral("utc %1").arg(i) << QStringLiteral("plus2 %1").arg(i);
    }
    const QString utcPath = writeFile(dir, QStringLiteral("zone-utc.log"), utc);
    const QString plus2Path = writeFile(dir, QStringLiteral("zone-plus2.log"), plus2);
    const qint64 firstMs = QDateTime(QDate(2026, 9, 20), QTime(10, 0), QTimeZone::UTC)
                               .toMSecsSinceEpoch();

    const auto check = [&](const QString& name, const Document& doc) {
        g_context = QStringLiteral("часовые пояса / ") + name;
        g_contextFailures = 0;
        const LogModel& m = *doc.model;
        CHECK(m.rowCount() == expected.size(),
              QStringLiteral("строк %1, ожидается %2").arg(m.rowCount()).arg(expected.size()));
        for (int row = 0; row < qMin(m.rowCount(), int(expected.size())); ++row) {
            CHECK(m.messageAt(row).endsWith(expected[row]),
                  QStringLiteral("строка %1: %2, ожидается …%3")
                      .arg(row).arg(describe(m.messageAt(row)), expected[row]));
            // Строка row — секунда row после 10:00:00 UTC, у нечётных ещё 500 мс.
            const qint64 wantMs = firstMs + row * 1000 + (row % 2 ? 500 : 0);
            CHECK(m.visibleTimestampAt(row).toMSecsSinceEpoch() == wantMs,
                  QStringLiteral("строка %1: момент %2, ожидается %3")
                      .arg(row).arg(describe(m.visibleTimestampAt(row)),
                                    describe(QDateTime::fromMSecsSinceEpoch(wantMs))));
        }
    };
    check(QStringLiteral("резидентный"), loadResident({ utcPath, plus2Path }, schema));
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        check(QStringLiteral("индексный, ") + describe(mode),
              loadIndexed({ utcPath, plus2Path }, schema, mode));
}

// ---------------------------------------------------------------------------
// F-13. Метки до 1970 года и на краях диапазона: 0 мс (ровно эпоха), −1 мс,
// момент, отрицательный из-за зоны, годы 0001 и 9999 — законные метки, а
// «метки нет» (kNoTimestampMs) не совпадает ни с одной из них. Слитая
// вкладка из такого файла и обычного: порядок, моменты, ключи строк,
// снапшот, фильтр времени, переход ко времени и диапазон меток сверяются с
// заданным заранее эталоном — общую ошибку сравнение бэкендов не увидит.
// ---------------------------------------------------------------------------
static void testPre1970Merge(const QDir& dir, const QString& schema)
{
    const auto utc = [](int y, int mo, int d, int h, int mi, int s, int ms) {
        return QDateTime(QDate(y, mo, d), QTime(h, mi, s, ms), QTimeZone::UTC)
            .toMSecsSinceEpoch();
    };
    const qint64 year1 = utc(1, 1, 1, 0, 0, 0, 0);
    const qint64 year9999 = utc(9999, 12, 31, 23, 59, 59, 999);
    const qint64 now = utc(2026, 9, 20, 10, 0, 0, 0);

    // Файл «a» нарочно не по порядку и с преамбулой без метки; «b» —
    // обычный, с совпадающим моментом −1 мс (при равенстве — порядок путей).
    const QString aPath = writeFile(dir, QStringLiteral("a-pre1970.log"),
        "a preamble without a stamp\n"
        "0001-01-01T00:00:00.000Z INFO a year one\n"
        "1970-01-01T00:00:00.000Z INFO a epoch\n"
        "    at continuation of the epoch record\n"
        "1969-12-31T23:59:59.999Z INFO a minus one\n"
        "1970-01-01T01:00:00.000+02:00 INFO a minus hour\n"
        "9999-12-31T23:59:59.999Z INFO a year 9999\n"
        "1900-01-01T00:00:00.000Z ERROR a far past\n");
    const QString bPath = writeFile(dir, QStringLiteral("b-now.log"),
        "2026-09-20T10:00:00.000Z INFO b now\n"
        "1969-12-31T23:59:59.999Z WARN b minus one\n");

    struct Row { const char* text; qint64 ms; };
    const QVector<Row> expected = {
        { "a year one", year1 },
        { "a far past", utc(1900, 1, 1, 0, 0, 0, 0) },
        { "a minus hour", -3600000 },
        { "a minus one", -1 },
        { "b minus one", -1 },
        { "a epoch", 0 },
        { "at continuation of the epoch record", 0 },
        { "b now", now },
        { "a year 9999", year9999 },
        { "a preamble without a stamp", kNoTimestampMs },
    };

    const auto check = [&](const QString& name, const Document& doc) {
        g_context = QStringLiteral("до 1970 / ") + name;
        g_contextFailures = 0;
        LogModel& m = *doc.model;
        const auto visibleTexts = [&m]() {
            QStringList out;
            for (int row = 0; row < m.rowCount(); ++row)
                out << m.messageAt(row);
            return out;
        };

        CHECK(m.rowCount() == expected.size(),
              QStringLiteral("строк %1, ожидается %2").arg(m.rowCount()).arg(expected.size()));
        const LogScanSnapshot snap = m.scanSnapshot(false);
        for (int row = 0; row < qMin(m.rowCount(), int(expected.size())); ++row) {
            const Row& want = expected[row];
            CHECK(m.messageAt(row).endsWith(QLatin1String(want.text)),
                  QStringLiteral("строка %1: %2, ожидается …%3")
                      .arg(row).arg(describe(m.messageAt(row)), QLatin1String(want.text)));
            const QDateTime ts = m.visibleTimestampAt(row);
            const qint64 gotMs = ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs;
            CHECK(gotMs == want.ms,
                  QStringLiteral("строка %1: момент %2, ожидается %3")
                      .arg(row).arg(gotMs).arg(want.ms));
            CHECK(snap.metaAt(row).timestampMs == want.ms,
                  QStringLiteral("строка %1: снапшот %2, ожидается %3")
                      .arg(row).arg(snap.metaAt(row).timestampMs).arg(want.ms));
            const RecordKey key = m.keyAt(row);
            CHECK(key.timestampMs == want.ms && key.hasTimestamp() == (want.ms != kNoTimestampMs),
                  QStringLiteral("строка %1: ключ %2, ожидается %3")
                      .arg(row).arg(key.timestampMs).arg(want.ms));
            CHECK(m.rowForKey(key) == row,
                  QStringLiteral("строка %1: rowForKey → %2").arg(row).arg(m.rowForKey(key)));
        }

        // Диапазон меток: первая строка и последняя с меткой.
        const QPair<QDateTime, QDateTime> range = m.fullTimeRange();
        CHECK(range.first.isValid() && range.first.toMSecsSinceEpoch() == year1
                  && range.second.isValid() && range.second.toMSecsSinceEpoch() == year9999,
              QStringLiteral("fullTimeRange: %1 — %2")
                  .arg(describe(range.first), describe(range.second)));
        qint64 minMs = 0, maxMs = 0;
        CHECK(snap.timeRange(&minMs, &maxMs) && minMs == year1 && maxMs == year9999,
              QStringLiteral("снапшот: диапазон меток %1 — %2").arg(minMs).arg(maxMs));

        // Переход ко времени: первая строка не раньше момента.
        const struct { qint64 at; int row; } jumps[] = {
            { -1, 3 }, { 0, 5 }, { 1, 7 }, { year1, 0 }, { year1 - 1, 0 },
            { -3600000, 2 }, { year9999, 8 }, { year9999 + 1, 9 },
        };
        for (const auto& j : jumps)
            CHECK(m.firstVisibleRowAtOrAfter(QDateTime::fromMSecsSinceEpoch(j.at)) == j.row,
                  QStringLiteral("firstVisibleRowAtOrAfter(%1) = %2, ожидается %3")
                      .arg(j.at).arg(m.firstVisibleRowAtOrAfter(QDateTime::fromMSecsSinceEpoch(j.at)))
                      .arg(j.row));

        // Фильтр времени: границы включительно, строка без метки при заданном
        // начале скрыта.
        const struct { qint64 from, to; QStringList rows; } windows[] = {
            { -1, 0, { QStringLiteral("a minus one"), QStringLiteral("b minus one"),
                       QStringLiteral("a epoch"),
                       QStringLiteral("at continuation of the epoch record") } },
            { year1, -3600000, { QStringLiteral("a year one"), QStringLiteral("a far past"),
                                 QStringLiteral("a minus hour") } },
            { 1, year9999, { QStringLiteral("b now"), QStringLiteral("a year 9999") } },
            { -3599999, -2, {} },
        };
        for (const auto& w : windows) {
            FilterConfig cfg;
            cfg.name = QStringLiteral("время %1 — %2").arg(w.from).arg(w.to);
            cfg.start = QDateTime::fromMSecsSinceEpoch(w.from);
            cfg.end = QDateTime::fromMSecsSinceEpoch(w.to);
            applyChecked(m, cfg, {});
            const QStringList got = visibleTexts();
            bool same = got.size() == w.rows.size();
            for (int i = 0; same && i < got.size(); ++i)
                same = got[i].endsWith(w.rows[i]);
            CHECK(same, cfg.name + QStringLiteral(": ") + got.join(QStringLiteral(" | ")));
        }
        applyChecked(m, FilterConfig{}, {});
        CHECK(m.rowCount() == expected.size(), QStringLiteral("сброс фильтра времени"));
    };
    check(QStringLiteral("резидентный"), loadResident({ aPath, bPath }, schema));
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        check(QStringLiteral("индексный, ") + describe(mode),
              loadIndexed({ aPath, bPath }, schema, mode));
}

// ---------------------------------------------------------------------------
// Второй файл дописан во вкладку с файлом «не по порядку» при АКТИВНОМ
// фильтре: переход «один файл → несколько» переупорядочивает и видимые
// строки (materializeAllRefs). Эталон — резидентная вкладка с теми же
// файлами и тем же фильтром.
// ---------------------------------------------------------------------------
static void testAttachWithActiveFilter(const QString& unsortedPath, const QString& secondPath,
                                       const QString& schema, const QStringList& fieldNames)
{
    FilterConfig cfg;
    cfg.name = QStringLiteral("уровни: Error+Warn");
    cfg.levels = { LogLevel::Error, LogLevel::Warn };
    g_context = QStringLiteral("второй файл при активном фильтре / загрузка");
    g_contextFailures = 0;

    Document idx = loadIndexed({ unsortedPath }, schema);
    applyChecked(*idx.model, cfg, fieldNames);
    addIndexedFiles(idx, { secondPath }, schema);
    settleChecked(*idx.model);

    Document res = loadResident({ unsortedPath, secondPath }, schema);
    applyChecked(*res.model, cfg, fieldNames);

    g_context = QStringLiteral("второй файл при активном фильтре / ") + cfg.name;
    g_contextFailures = 0;
    CHECK(idx.model->rowCount() > 0, QStringLiteral("фильтр оставил строки"));
    compareVisibleRows(*res.model, *idx.model);
}

// ---------------------------------------------------------------------------
// Номера физических строк (R-1): на номере строки стоит идентификатор, поэтому
// оба бэкенда обязаны нумеровать строки одинаково — и так, как их видит
// файл: строка — то, что кончается '\n' ('\r' перед ним снимается), плюс
// хвост без '\n'; пустая строка — тоже строка, в том числе последняя; за
// завершающим '\n' строки нет. Эталон — разбиение байтов
// (testdocs::expectedPhysicalLines), а не один из бэкендов: общую ошибку
// сверка бэкендов не увидит.
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Кодировки на индексном бэкенде (F-1, шаг 2). Эталон — та же индексная
// вкладка на ТОМ ЖЕ тексте в UTF-8: файл в однобайтной кодировке обязан дать
// строка в строку тот же текст, поля, фильтры (кириллица без учёта регистра,
// регекс, правило по полю), поиск, переходы по уровню, снапшоты и Find All.
// Сверка резидентного бэкенда с индексным на тех же файлах — шаг 3, ниже
// (testResidentEncodings).
// ---------------------------------------------------------------------------

static QVector<FilterConfig> encodingConfigs()
{
    QVector<FilterConfig> cfgs;
    const auto add = [&](const QString& name, QVector<FilterRule> rules,
                         QSet<LogLevel> levels = {}) {
        FilterConfig c;
        c.name = name;
        c.levels = levels;
        c.rules.rules = std::move(rules);
        c.fieldScopeActive = true;
        cfgs << c;
    };
    add(QStringLiteral("без фильтров"), {});
    add(QStringLiteral("кириллица без учёта регистра"),
        { rule(FilterRule::Action::Include, QStringLiteral("ОШИБКА")) });
    add(QStringLiteral("кириллица с учётом регистра"),
        { rule(FilterRule::Action::Include, QStringLiteral("Ошибка"), FilterRule::Connector::And,
               QString(), false, true) });
    add(QStringLiteral("кириллица, регекс"),
        { rule(FilterRule::Action::Include, QStringLiteral("пользовател[а-я]+\\s+[а-я]+ов"),
               FilterRule::Connector::And, QString(), true) });
    add(QStringLiteral("кириллица, исключение + ИЛИ"),
        { rule(FilterRule::Action::Include, QStringLiteral("диск")),
          rule(FilterRule::Action::Include, QStringLiteral("повторная"), FilterRule::Connector::Or),
          rule(FilterRule::Action::Exclude, QStringLiteral("ascii")) });
    add(QStringLiteral("кириллица в поле Message"),
        { rule(FilterRule::Action::Include, QStringLiteral("сервер"), FilterRule::Connector::And,
               QStringLiteral("Message")) });
    add(QStringLiteral("уровень Error + кириллица"),
        { rule(FilterRule::Action::Include, QStringLiteral("соединения")) },
        { LogLevel::Error });
    add(QStringLiteral("кириллица, ничего не найдено"),
        { rule(FilterRule::Action::Include, QStringLiteral("нет такой строки")) });
    return cfgs;
}

static void runEncodingFixture(const QString& name, const QStringList& utf8Paths,
                               const QStringList& encodedPaths, const QString& schema,
                               const QStringList& fieldNames,
                               IndexedLoad mode = IndexedLoad::Sequential)
{
    g_context = name + QStringLiteral(" / загрузка");
    g_contextFailures = 0;
    g_fileNameOnly = true;

    Document ref = loadIndexed(utf8Paths, schema, mode);
    Document enc = loadIndexed(encodedPaths, schema, mode);
    std::fprintf(stdout, "фикстура «%s»: %d файл(ов), %d строк(и)\n",
                 U8(name), int(encodedPaths.size()), enc.model->rowCount());
    std::fflush(stdout);
    CHECK(enc.model->rowCount() > 0, QStringLiteral("вкладка пуста"));

    for (const FilterConfig& cfg : encodingConfigs()) {
        g_context = name + QStringLiteral(" / ") + cfg.name;
        g_contextFailures = 0;
        applyConfig(*ref.model, cfg, fieldNames, kFilterQuietMs);
        checkRecordsWhole(*ref.model, cfg.name);
        applyConfig(*enc.model, cfg, fieldNames, kFilterQuietMs);
        checkRecordsWhole(*enc.model, cfg.name);
        if (!cfg.rules.rules.isEmpty() && cfg.name != QStringLiteral("кириллица, ничего не найдено"))
            CHECK(enc.model->rowCount() > 0,
                  QStringLiteral("правило с кириллицей ничего не нашло — фикстура не проверяет фильтр"));
        compareAll(ref, enc);

        // Find All (панель Search): база из видимых строк и тот же запрос.
        if (!cfg.rules.rules.isEmpty()) {
            FilterRuleSet query;
            query.rules = { rule(FilterRule::Action::Include, QStringLiteral("журнал")) };
            query.bindFields(fieldNames);
            LogModel refResults;
            LogModel encResults;
            refResults.searchVisible(*ref.model, query);
            encResults.searchVisible(*enc.model, query);
            settleChecked(refResults);
            settleChecked(encResults);
            g_context = name + QStringLiteral(" / ") + cfg.name + QStringLiteral(" / Find All");
            compareVisibleRows(refResults, encResults);
        }
    }
    g_fileNameOnly = false;
}

static void testEncodings(const QDir& dir, const QString& schema, const QStringList& fieldNames)
{
    // Файл A — ASCII-голова перед кириллицей (решение в середине файла),
    // файл B — кириллица с первых строк, метки перекрываются с A.
    const QString textA = cyrillicLogText(40);
    const QString textB = cyrillicLogText(0, 7);
    QDir().mkpath(dir.filePath(QStringLiteral("enc-utf8")));
    const QDir utf8Dir(dir.filePath(QStringLiteral("enc-utf8")));
    const QString utf8A = writeFile(utf8Dir, QStringLiteral("a.log"), encodeText(textA, TextEncoding::Utf8));
    const QString utf8B = writeFile(utf8Dir, QStringLiteral("b.log"), encodeText(textB, TextEncoding::Utf8));

    for (const TextEncoding enc : { TextEncoding::Windows1251, TextEncoding::Ibm866,
                                    TextEncoding::Koi8R }) {
        const QString encName = TextEncodings::displayName(enc);
        QDir().mkpath(dir.filePath(encName));
        const QDir encDir(dir.filePath(encName));
        const QString encA = writeFile(encDir, QStringLiteral("a.log"), encodeText(textA, enc));
        const QString encB = writeFile(encDir, QStringLiteral("b.log"), encodeText(textB, enc));

        runEncodingFixture(QStringLiteral("кодировка %1, один файл").arg(encName),
                           { utf8A }, { encA }, schema, fieldNames);

        // Кодировка определена сама и записана на файл.
        Document doc = loadIndexed({ encA }, schema);
        const FileEncoding state = doc.model->indexedOrNull()->fileEncoding(doc.files.value(0));
        CHECK(state.source == FileEncoding::Source::Auto && state.decoder.encoding() == enc,
              QStringLiteral("%1: стор не запомнил определённую кодировку (%2)")
                  .arg(encName, TextEncodings::displayName(state.decoder.encoding())));
    }

    // Слитая вкладка: файлы в разных кодировках, каждый читается своим
    // декодером — «кодировки вкладки» нет.
    QDir().mkpath(dir.filePath(QStringLiteral("enc-mixed")));
    const QDir mixedDir(dir.filePath(QStringLiteral("enc-mixed")));
    const QString mixedA = writeFile(mixedDir, QStringLiteral("a.log"), encodeText(textA, TextEncoding::Utf8));
    const QString mixedB = writeFile(mixedDir, QStringLiteral("b.log"),
                                     encodeText(textB, TextEncoding::Windows1251));
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        runEncodingFixture(QStringLiteral("слитая вкладка UTF-8 + Windows-1251, ") + describe(mode),
                           { utf8A, utf8B }, { mixedA, mixedB }, schema, fieldNames, mode);

    // Кириллица пришла дозаписью в файл, начавшийся ASCII: «не решено»
    // переживает загрузку, решение — на первой не-ASCII строке хвоста.
    {
        g_context = QStringLiteral("кодировка в дозаписи");
        g_contextFailures = 0;
        QString head;
        for (int i = 0; i < 30; ++i)
            head += QStringLiteral("2026-03-05 09:%1:00.000 [main] INFO - ascii only %2\n")
                        .arg(i, 2, 10, QChar('0')).arg(i);
        const QString path = writeFile(dir, QStringLiteral("enc-tail.log"),
                                       encodeText(head, TextEncoding::Koi8R));
        Document doc = loadIndexed({ path }, schema);
        IndexedLogStore* store = doc.model->indexedOrNull();
        CHECK(!store->fileEncoding(doc.files.value(0)).decided(),
              QStringLiteral("ASCII-файл не должен быть «решён»"));
        const QString tail = cyrillicLogText(0, 4000);
        CHECK(appendIndexedTail(doc, 0, encodeText(tail, TextEncoding::Koi8R), schema),
              QStringLiteral("дозапись не прочитана"));
        settleChecked(*doc.model);
        const FileEncoding state = store->fileEncoding(doc.files.value(0));
        CHECK(state.decoder.encoding() == TextEncoding::Koi8R,
              QStringLiteral("дозапись не решила KOI8-R: %1")
                  .arg(TextEncodings::displayName(state.decoder.encoding())));
        QStringList want = (head + tail).split(QLatin1Char('\n'));
        want.removeLast();
        QStringList got;
        for (int row = 0; row < doc.model->rowCount(); ++row)
            got << doc.model->messageAt(row);
        CHECK_EQ(got.join(QLatin1Char('\n')), want.join(QLatin1Char('\n')),
                 QStringLiteral("текст после дозаписи"));
    }
}

// ---------------------------------------------------------------------------
// Кодировки на резидентном бэкенде (F-1, шаг 3): один и тот же файл ниже и
// выше порога обязан дать один текст, один порядок и одни ключи строк.
// Эталон — индексный бэкенд на ТЕХ ЖЕ файлах (он сверен с UTF-8-текстом в
// testEncodings), а текст резидентного — ещё и напрямую с исходным текстом:
// общую ошибку декодера сравнение бэкендов не увидело бы.
// ---------------------------------------------------------------------------

static void runResidentEncodingFixture(const QString& name, const QStringList& paths,
                                       const QString& schema, const QStringList& fieldNames,
                                       IndexedLoad mode = IndexedLoad::Sequential)
{
    // Весь API на обычных конфигурациях (ASCII-правила, случайные).
    runFixture(name, paths, schema, fieldNames, mode);

    // Правила с кириллицей и Find All — как в testEncodings, но резидентный
    // против индексного.
    g_context = name + QStringLiteral(" / кириллица / загрузка");
    g_contextFailures = 0;
    Document res = loadResident(paths, schema);
    Document idx = loadIndexed(paths, schema, mode);
    const AllLines resAll = collectAllLines(res);
    const AllLines idxAll = collectAllLines(idx);
    for (const FilterConfig& cfg : encodingConfigs()) {
        g_context = name + QStringLiteral(" / ") + cfg.name;
        g_contextFailures = 0;
        applyConfig(*res.model, cfg, fieldNames, kFilterQuietMs);
        checkRecordsWhole(*res.model, cfg.name);
        applyConfig(*idx.model, cfg, fieldNames, kFilterQuietMs);
        checkRecordsWhole(*idx.model, cfg.name);
        if (!cfg.rules.rules.isEmpty() && cfg.name != QStringLiteral("кириллица, ничего не найдено"))
            CHECK(res.model->rowCount() > 0,
                  QStringLiteral("правило с кириллицей ничего не нашло на резидентном бэкенде"));
        compareAll(res, idx);
        compareRecordKeys(res, idx, resAll, idxAll, /*sameOrder=*/true);
        if (!cfg.rules.rules.isEmpty()) {
            FilterRuleSet query;
            query.rules = { rule(FilterRule::Action::Include, QStringLiteral("ЖУРНАЛ")) };
            query.bindFields(fieldNames);
            LogModel resResults;
            LogModel idxResults;
            resResults.searchVisible(*res.model, query);
            idxResults.searchVisible(*idx.model, query);
            settleChecked(resResults);
            settleChecked(idxResults);
            g_context = name + QStringLiteral(" / ") + cfg.name + QStringLiteral(" / Find All");
            compareVisibleRows(resResults, idxResults);
        }
    }
}

// Текст строк одно-файловой вкладки по порядку — против исходного текста.
static void checkResidentText(const QString& what, const Document& doc, const QString& text)
{
    QStringList want = text.split(QLatin1Char('\n'));
    if (!want.isEmpty() && want.constLast().isEmpty())
        want.removeLast();
    QStringList got;
    for (int row = 0; row < doc.model->rowCount(); ++row)
        got << doc.model->messageAt(row);
    CHECK_EQ(got.join(QLatin1Char('\n')), want.join(QLatin1Char('\n')), what);
}

static void testResidentEncodings(const QDir& dir, const QString& schema,
                                  const QStringList& fieldNames)
{
    const QString textA = cyrillicLogText(40);
    const QString textB = cyrillicLogText(0, 7);
    QDir().mkpath(dir.filePath(QStringLiteral("res-enc")));
    const QDir encDir(dir.filePath(QStringLiteral("res-enc")));

    for (const TextEncoding enc : { TextEncoding::Windows1251, TextEncoding::Ibm866,
                                    TextEncoding::Koi8R }) {
        const QString encName = TextEncodings::displayName(enc);
        const QString path = writeFile(encDir, encName + QStringLiteral(".log"),
                                       encodeText(textA, enc));
        // KOI8-R — только текст и решение: полный прогон на двух кодировках
        // из задачи (cp1251, cp866) уже держит механизм.
        if (enc != TextEncoding::Koi8R)
            runResidentEncodingFixture(QStringLiteral("резидентный, %1, один файл").arg(encName),
                                       { path }, schema, fieldNames);

        g_context = QStringLiteral("резидентный, %1, текст").arg(encName);
        g_contextFailures = 0;
        Document doc = loadResident({ path }, schema);
        checkResidentText(QStringLiteral("текст резидентной вкладки"), doc, textA);
        const FileEncoding state = doc.encodings.value(path);
        CHECK(state.source == FileEncoding::Source::Auto && state.decoder.encoding() == enc,
              QStringLiteral("%1: парсер решил %2")
                  .arg(encName, TextEncodings::displayName(state.decoder.encoding())));
        // То же решение, что у индексатора: правило одно.
        Document idx = loadIndexed({ path }, schema);
        const FileEncoding idxState = idx.model->indexedOrNull()->fileEncoding(idx.files.value(0));
        CHECK(idxState.decoder == state.decoder && idxState.source == state.source,
              QStringLiteral("%1: бэкенды решили по-разному").arg(encName));
    }

    // Слитая вкладка: UTF-8 + Windows-1251 — каждый файл своим декодером.
    const QString mixedA = writeFile(encDir, QStringLiteral("mixed-a.log"),
                                     encodeText(textA, TextEncoding::Utf8));
    const QString mixedB = writeFile(encDir, QStringLiteral("mixed-b.log"),
                                     encodeText(textB, TextEncoding::Windows1251));
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        runResidentEncodingFixture(
            QStringLiteral("резидентный, слитая вкладка UTF-8 + Windows-1251, ") + describe(mode),
            { mixedA, mixedB }, schema, fieldNames, mode);

    // UTF-8 с BOM: решение Bom, текст без BOM — на обоих бэкендах.
    {
        g_context = QStringLiteral("резидентный, UTF-8 с BOM");
        g_contextFailures = 0;
        const QString path = writeFile(encDir, QStringLiteral("bom.log"),
                                       QByteArray("\xEF\xBB\xBF") + textA.toUtf8());
        Document doc = loadResident({ path }, schema);
        checkResidentText(QStringLiteral("текст UTF-8 с BOM"), doc, textA);
        CHECK(doc.encodings.value(path).source == FileEncoding::Source::Bom,
              QStringLiteral("BOM UTF-8 не дал решения Bom"));
        runFixture(QStringLiteral("резидентный, UTF-8 с BOM"), { path }, schema, fieldNames);
    }

    // Кириллица пришла дозаписью в ASCII-файл: «не решено» переживает
    // загрузку, решает первая не-ASCII строка хвоста — на обоих бэкендах.
    {
        g_context = QStringLiteral("резидентный, кодировка в дозаписи");
        g_contextFailures = 0;
        QString head;
        for (int i = 0; i < 30; ++i)
            head += QStringLiteral("2026-03-05 09:%1:00.000 [main] INFO - ascii only %2\n")
                        .arg(i, 2, 10, QChar('0')).arg(i);
        const QString tail = cyrillicLogText(0, 4000);
        // Один файл на бэкенд (дозапись меняет его), одно имя — для сравнения.
        QDir().mkpath(encDir.filePath(QStringLiteral("tail-res")));
        QDir().mkpath(encDir.filePath(QStringLiteral("tail-idx")));
        const QString resPath = writeFile(QDir(encDir.filePath(QStringLiteral("tail-res"))),
                                          QStringLiteral("tail.log"),
                                          encodeText(head, TextEncoding::Koi8R));
        const QString idxPath = writeFile(QDir(encDir.filePath(QStringLiteral("tail-idx"))),
                                          QStringLiteral("tail.log"),
                                          encodeText(head, TextEncoding::Koi8R));
        Document res = loadResident({ resPath }, schema);
        Document idx = loadIndexed({ idxPath }, schema);
        CHECK(!res.encodings.value(resPath).decided(),
              QStringLiteral("ASCII-файл не должен быть «решён»"));
        CHECK(appendResidentTail(res, 0, encodeText(tail, TextEncoding::Koi8R), schema),
              QStringLiteral("резидентная дозапись не прочитана"));
        CHECK(appendIndexedTail(idx, 0, encodeText(tail, TextEncoding::Koi8R), schema),
              QStringLiteral("индексная дозапись не прочитана"));
        settleChecked(*res.model);
        settleChecked(*idx.model);
        CHECK(res.encodings.value(resPath).decoder.encoding() == TextEncoding::Koi8R,
              QStringLiteral("резидентная дозапись не решила KOI8-R"));
        checkResidentText(QStringLiteral("текст после резидентной дозаписи"), res, head + tail);
        g_fileNameOnly = true; // файлы разные — сравниваются по имени
        compareVisibleRows(*res.model, *idx.model);
        g_fileNameOnly = false;

        // Решённый файл не решается заново: ещё одна порция декодируется KOI8-R.
        const QString more = cyrillicLogText(0, 8000);
        CHECK(appendResidentTail(res, 0, encodeText(more, TextEncoding::Koi8R), schema),
              QStringLiteral("вторая дозапись не прочитана"));
        settleChecked(*res.model);
        checkResidentText(QStringLiteral("текст после второй дозаписи"), res, head + tail + more);
    }
}

// Шаг 3а F-1: предварительное решение кодировки. Строки лога с
// возрастающими метками от second; в строке word (если ≥ 0) — слово text.
static QString asciiRecords(int count, int second, int wordLine = -1,
                            const QString& word = QString())
{
    const QDateTime base = fixtureBaseTime();
    QString out;
    for (int i = 0; i < count; ++i)
        out += QStringLiteral("%1 [main] INFO - ascii filler line %2%3\n")
                   .arg(base.addSecs(second + i).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
                   .arg(i)
                   .arg(i == wordLine ? QStringLiteral(" ") + word : QString());
    return out;
}

// Видимые строки по тексту и уровню — против эталона, у которого нет
// отметок «новых» строк (он не дописывался).
static void compareTextWithReference(const LogModel& got, const LogModel& ref, const QString& what)
{
    QStringList a, b;
    for (int row = 0; row < got.rowCount(); ++row)
        a << QStringLiteral("%1 %2").arg(int(got.visibleLevelAt(row))).arg(got.messageAt(row));
    for (int row = 0; row < ref.rowCount(); ++row)
        b << QStringLiteral("%1 %2").arg(int(ref.visibleLevelAt(row))).arg(ref.messageAt(row));
    CHECK_EQ(a.join(QLatin1Char('\n')), b.join(QLatin1Char('\n')), what);
}

static bool sameAutoState(const FileEncoding& a, const FileEncoding& b)
{
    return a.decoder == b.decoder && a.source == b.source && a.provisional == b.provisional;
}

static QString describeState(const FileEncoding& e)
{
    return QStringLiteral("%1%2").arg(TextEncodings::displayName(e.decoder.encoding()),
                                      e.provisional ? QStringLiteral(" (предварительно)") : QString());
}

static void testProvisionalEncodings(const QDir& dir, const QString& schema,
                                     const QStringList& fieldNames)
{
    QDir().mkpath(dir.filePath(QStringLiteral("prov-enc")));
    const QDir encDir(dir.filePath(QStringLiteral("prov-enc")));
    const int kFiller = 1100; // строк ASCII: больше окна 64 КБ

    struct Case {
        QString name;
        QByteArray bytes;
        TextEncoding want;
        bool provisional;
        bool fullRun; // весь API (runResidentEncodingFixture)
    };
    // 1. Мусорный байт в начале UTF-8-лога, кириллица за окном.
    QByteArray garbageUtf8 = (asciiRecords(kFiller, 0) + cyrillicLogText(0, kFiller)).toUtf8();
    garbageUtf8.insert(garbageUtf8.indexOf(" line 0") + 7, " \xFF");
    // 3. Одно короткое слово в Windows-1251.
    const QByteArray oneWord = encodeText(asciiRecords(40, 0, 7, QStringLiteral("ошибка")),
                                          TextEncoding::Windows1251);
    // 4. Короткое слово, затем русский текст за окном — Windows-1251.
    const QByteArray late1251 = encodeText(asciiRecords(kFiller, 0, 3, QStringLiteral("старт"))
                                               + cyrillicLogText(0, kFiller),
                                           TextEncoding::Windows1251);
    // 5. Окончательное UTF-8, затем блок мусора и текст в Windows-1251.
    QByteArray garbage;
    for (int i = 0; i < 600; ++i)
        garbage += char(0x80 + (i * 7) % 0x80);
    const QByteArray finalUtf8 = cyrillicLogText(0).toUtf8()
        + asciiRecords(kFiller, 100).toUtf8()
        + "2026-03-05 10:30:00.000 [main] WARN - binary " + garbage + "\n"
        + encodeText(cyrillicLogText(0, 2000), TextEncoding::Windows1251);
    const QVector<Case> cases = {
        { QStringLiteral("мусорный байт + UTF-8"), garbageUtf8, TextEncoding::Utf8, false, true },
        { QStringLiteral("одно слово Windows-1251"), oneWord, TextEncoding::Windows1251, true, false },
        { QStringLiteral("слово, затем текст Windows-1251"), late1251, TextEncoding::Windows1251, false, false },
        { QStringLiteral("окончательное UTF-8 и мусор"), finalUtf8, TextEncoding::Utf8, false, false },
    };
    for (const Case& c : cases) {
        const QString path = writeFile(encDir, c.name + QStringLiteral(".log"), c.bytes);
        if (c.fullRun)
            runResidentEncodingFixture(QStringLiteral("предварительное, ") + c.name, { path },
                                       schema, fieldNames);
        g_context = QStringLiteral("предварительное, %1").arg(c.name);
        g_contextFailures = 0;
        Document res = loadResident({ path }, schema);
        Document idx = loadIndexed({ path }, schema);
        const FileEncoding resState = res.encodings.value(path);
        const FileEncoding idxState = idx.model->indexedOrNull()->fileEncoding(idx.files.value(0));
        CHECK(resState.source == FileEncoding::Source::Auto && resState.decoder.encoding() == c.want
                  && resState.provisional == c.provisional,
              QStringLiteral("резидентный решил %1").arg(describeState(resState)));
        CHECK(sameAutoState(resState, idxState),
              QStringLiteral("бэкенды решили по-разному: %1 против %2")
                  .arg(describeState(resState), describeState(idxState)));
        const QString text = TextDecoder(c.want).decode(c.bytes);
        checkResidentText(QStringLiteral("текст резидентной вкладки"), res, text);
        compareVisibleRows(*res.model, *idx.model);
    }

    // 2. Кириллица UTF-8 дозаписью в лог, решённый по мусорному байту
    // (предварительно Windows-1251): пересмотр на обоих бэкендах, текст —
    // как у файла, сразу открытого в UTF-8; правило с кириллицей и Find All —
    // как на нём же.
    {
        g_context = QStringLiteral("предварительное, пересмотр дозаписью");
        g_contextFailures = 0;
        QByteArray head = asciiRecords(60, 0).toUtf8();
        head.insert(head.indexOf(" line 0") + 7, " \xFF");
        const QByteArray tail1 = cyrillicLogText(0, 100).toUtf8();
        const QByteArray tail2 = cyrillicLogText(0, 200).toUtf8();
        QDir().mkpath(encDir.filePath(QStringLiteral("tail-res")));
        QDir().mkpath(encDir.filePath(QStringLiteral("tail-idx")));
        QDir().mkpath(encDir.filePath(QStringLiteral("tail-ref")));
        const QString resPath = writeFile(QDir(encDir.filePath(QStringLiteral("tail-res"))),
                                          QStringLiteral("tail.log"), head);
        const QString idxPath = writeFile(QDir(encDir.filePath(QStringLiteral("tail-idx"))),
                                          QStringLiteral("tail.log"), head);
        Document res = loadResident({ resPath }, schema);
        Document idx = loadIndexed({ idxPath }, schema);
        const FileEncoding before = res.encodings.value(resPath);
        CHECK(before.decoder.encoding() == TextEncoding::Windows1251 && before.provisional,
              QStringLiteral("мусорный байт должен дать предварительное Windows-1251: ")
                  + describeState(before));
        for (const QByteArray& tail : { tail1, tail2 }) {
            CHECK(appendResidentTail(res, 0, tail, schema),
                  QStringLiteral("резидентная дозапись не прочитана"));
            CHECK(appendIndexedTail(idx, 0, tail, schema),
                  QStringLiteral("индексная дозапись не прочитана"));
            // После каждой порции — в том числе той, что пересмотрела
            // решение: текст и отметки «новых» строк хвоста у бэкендов одни.
            settleChecked(*res.model);
            settleChecked(*idx.model);
            g_fileNameOnly = true;
            compareVisibleRows(*res.model, *idx.model);
            g_fileNameOnly = false;
        }
        const FileEncoding resState = res.encodings.value(resPath);
        const FileEncoding idxState = idx.model->indexedOrNull()->fileEncoding(idx.files.value(0));
        CHECK(resState.decoder.isUtf8() && resState.source == FileEncoding::Source::Auto
                  && !resState.provisional,
              QStringLiteral("дозапись не пересмотрела решение: ") + describeState(resState));
        CHECK(sameAutoState(resState, idxState),
              QStringLiteral("бэкенды после дозаписи решили по-разному: %1 против %2")
                  .arg(describeState(resState), describeState(idxState)));
        const QByteArray all = head + tail1 + tail2;
        checkResidentText(QStringLiteral("текст после пересмотра"), res, QString::fromUtf8(all));
        g_fileNameOnly = true; // файлы разные — сравниваются по имени
        compareVisibleRows(*res.model, *idx.model);

        // Эталон — тот же файл, сразу открытый (решение UTF-8 с первой порции
        // кириллицы невозможно: мусорный байт раньше — поэтому эталон читает
        // его выбором пользователя UTF-8).
        const QString refPath = writeFile(QDir(encDir.filePath(QStringLiteral("tail-ref"))),
                                          QStringLiteral("tail.log"), all);
        Document ref = loadIndexed({ refPath }, schema);
        ref.model->indexedOrNull()->setFileEncoding(ref.files.value(0), TextDecoder(TextEncoding::Utf8));
        settleChecked(*ref.model);
        compareTextWithReference(*res.model, *ref.model, QStringLiteral("текст против эталона"));
        for (const FilterConfig& cfg : encodingConfigs()) {
            g_context = QStringLiteral("предварительное, пересмотр дозаписью / ") + cfg.name;
            g_contextFailures = 0;
            applyConfig(*res.model, cfg, fieldNames, kFilterQuietMs);
            checkRecordsWhole(*res.model, cfg.name);
            applyConfig(*idx.model, cfg, fieldNames, kFilterQuietMs);
            checkRecordsWhole(*idx.model, cfg.name);
            applyConfig(*ref.model, cfg, fieldNames, kFilterQuietMs);
            checkRecordsWhole(*ref.model, cfg.name);
            compareVisibleRows(*res.model, *idx.model);
            compareTextWithReference(*res.model, *ref.model, QStringLiteral("резидентный против эталона"));
            if (!cfg.rules.rules.isEmpty()) {
                FilterRuleSet query;
                query.rules = { rule(FilterRule::Action::Include, QStringLiteral("ЖУРНАЛ")) };
                query.bindFields(fieldNames);
                LogModel resResults, idxResults, refResults;
                resResults.searchVisible(*res.model, query);
                idxResults.searchVisible(*idx.model, query);
                refResults.searchVisible(*ref.model, query);
                settleChecked(resResults);
                settleChecked(idxResults);
                settleChecked(refResults);
                compareVisibleRows(resResults, idxResults);
                compareTextWithReference(resResults, refResults, QStringLiteral("Find All против эталона"));
            }
        }
        g_fileNameOnly = false;
    }
}

static void checkLineNumbers(const QString& what, const Document& res, const Document& idx,
                             const QByteArray& bytes)
{
    const QString expected = expectedPhysicalLines(bytes).join(QStringLiteral(" | "));
    const QString resLines = numberedLines(*res.model).join(QStringLiteral(" | "));
    const QString idxLines = numberedLines(*idx.model).join(QStringLiteral(" | "));
    CHECK(resLines == expected,
          QStringLiteral("%1: резидентный бэкенд нумерует строки не так, как файл:\n"
                         "    файл:        %2\n    резидентный: %3").arg(what, expected, resLines));
    CHECK(idxLines == expected,
          QStringLiteral("%1: индексный бэкенд нумерует строки не так, как файл:\n"
                         "    файл:      %2\n    индексный: %3").arg(what, expected, idxLines));
}

static void testLineNumbers(const QDir& dir, const QString& schema)
{
    const QByteArray a = "2026-03-05 10:00:00.000 [w] INFO - first\n";
    const QByteArray b = "2026-03-05 10:00:01.000 [w] ERROR - second\n";
    const QByteArray cont = "    at continuation\n";
    const QByteArray c = "2026-03-05 10:00:02.000 [w] WARN - third\n";

    struct Case {
        const char* name;
        QByteArray initial;
        QVector<QByteArray> appends; // дозаписи; файл перед каждой кончается '\n'
    };
    auto crlf = [](QByteArray s) { return s.replace("\n", "\r\n"); };
    const QVector<Case> cases = {
        { "LF", a + b + cont + c, { c } },
        { "пустая строка в хвосте", a + b + "\n", { c, "\n", "\n\n" + a } },
        { "несколько пустых строк в хвосте", a + "\n\n\n", { b } },
        { "пустые строки в середине", a + "\n" + cont + "\n\n" + b, { "\n" + c } },
        { "CRLF", crlf(a + b + cont + c), { crlf(c) } },
        { "CRLF, пустая строка в хвосте", crlf(a + b + "\n"), { crlf(c) } },
        { "оборванная последняя строка", a + b + "    at tail without newline", {} },
        { "CRLF, оборванная последняя строка", crlf(a + b) + "2026-03-05 10:00:03.000 [w] INFO - cut", {} },
        { "только перевод строки", "\n", { a } },
        { "только пустые строки", "\n\n", { a } },
        { "UTF-8 BOM", "\xEF\xBB\xBF" + a + b + "\n", { c } },
        { "только UTF-8 BOM", "\xEF\xBB\xBF", { a } },
        { "пустой файл", "", { a } },
        { "одиночный \\r внутри строки", a + "    left\rright\n" + b, {} },
        // Оборванный хвост с одиночным '\r' на конце: '\r' не текст на обоих
        // бэкендах (до R-1 шага 2 индексный его оставлял).
        { "оборванная строка с одиночным \\r на конце", a + b + "    tail\r", {} },
        { "CRLF, оборванная запись с одиночным \\r", crlf(a + b) + "2026-03-05 10:00:03.000 [w] INFO - cut\r", {} },
        { "файл из одного \\r", "\r", {} },
        { "пустая строка и одиночный \\r", a + "\n\r", {} },
    };

    for (int n = 0; n < cases.size(); ++n) {
        const Case& tc = cases[n];
        g_context = QStringLiteral("номера строк / ") + QString::fromUtf8(tc.name);
        g_contextFailures = 0;

        QByteArray bytes = tc.initial;
        const QString resPath = writeFile(dir, QStringLiteral("lines-%1-res.log").arg(n), bytes);
        const QString idxPath = writeFile(dir, QStringLiteral("lines-%1-idx.log").arg(n), bytes);
        Document res = loadResident({ resPath }, schema);
        Document idx = loadIndexed({ idxPath }, schema);
        checkLineNumbers(QStringLiteral("загрузка"), res, idx, bytes);

        for (int k = 0; k < tc.appends.size(); ++k) {
            const QByteArray& chunk = tc.appends[k];
            bytes += chunk;
            CHECK(appendResidentTail(res, 0, chunk, schema), QStringLiteral("резидентная дозапись"));
            CHECK(appendIndexedTail(idx, 0, chunk, schema), QStringLiteral("индексная дозапись"));
            settleChecked(*res.model);
            settleChecked(*idx.model);
            checkLineNumbers(QStringLiteral("дозапись %1").arg(k + 1), res, idx, bytes);
        }
    }
}

// F-12. Лог без слов уровня: запись начинает ведущая метка (позиция 0,
// «[» на позиции 0, access за «[»), тело с метками в середине и под отступом
// — продолжение. Метки неубывающие, чтобы одно-файловая вкладка сравнивалась
// строго. form выбирает вид первичной строки по номеру записи: 0 — по кругу
// все виды, 1 — только access-лог. Преамбула без метки — только для слитой
// вкладки: одно-файловая индексная не сортирует (testSingleFileOrdering).
static QByteArray makeNoLevelBytes(int records, int form, const QDateTime& base,
                                   bool withPreamble)
{
    static const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    QByteArray b;
    if (withPreamble)
        b += "service banner without a timestamp\n";
    for (int i = 0; i < records; ++i) {
        const QDateTime t = base.addSecs(i * 7);
        const QByteArray iso = t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")).toUtf8();
        const QByteArray access = (t.toString(QStringLiteral("dd/")) + kMonths[t.date().month() - 1]
                                   + t.toString(QStringLiteral("/yyyy:HH:mm:ss"))).toUtf8();
        const QByteArray syslog = (QByteArray(kMonths[t.date().month() - 1]) + ' '
                                   + t.toString(QStringLiteral("dd HH:mm:ss")).toUtf8());
        const int kind = form == 1 ? 2 : i % 4;
        switch (kind) {
        case 0: b += iso + " job " + QByteArray::number(i) + " started\n"; break;
        case 1: b += "[" + iso + "] cache step " + QByteArray::number(i) + "\n"; break;
        case 2: b += "10.0.0." + QByteArray::number(i % 250) + " - - [" + access
                     + " +0000] \"GET /item/" + QByteArray::number(i) + " HTTP/1.1\" 200 512\n"; break;
        default: b += syslog + " web01 cron[" + QByteArray::number(i) + "]: tick\n"; break;
        }
        if (i % 5 == 0) {
            b += "{\n";
            b += "  \"ts\": \"" + iso + "\",\n";
            b += "Response: {\"at\":\"" + iso + "\"}\n";
            b += "}\n";
        }
    }
    return b;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// F-4, PR 2. Ключевая схема на загрузке: момент, уровень и граница записи из
// ключей — одним правилом (LineClassifier::classifyLine) у парсера и
// индексатора. Фикстуры JSON Lines и logfmt (tests/common/keyedfixtures.h)
// прогоняются во всех конфигурациях фильтров (правила по колонкам Level,
// Message, Thread — с раскодированными значениями), моменты, уровни, записи
// и значение Message каждой строки сверяются с заданными в фикстуре; слитая
// вкладка с обычным логом — против заданного порядка моментов; дозапись —
// с объектом, оборванным посреди строки, \u-последовательности и вложенного.
// ---------------------------------------------------------------------------
struct KeyedFile {
    QString path;
    keyedfx::Fixture fixture;
};

static void checkKeyedExpectations(const QString& what, const LogModel& m,
                                   const QVector<KeyedFile>& files, const QStringList& fieldNames)
{
    const int messageColumn = fieldNames.indexOf(QStringLiteral("Message"));
    QHash<QString, int> idOfRecord;      // «путь#запись» → id записи
    QHash<QString, QString> recordOfId;  // «путь#id» → «путь#запись»
    qint64 previousMs = (std::numeric_limits<qint64>::min)();
    for (int row = 0; row < m.rowCount(); ++row) {
        if (g_contextFailures > kFailuresPerContext)
            return;
        const std::shared_ptr<LogEntry> e = m.entryAt(row);
        if (!e || !e->sourceFile()) {
            CHECK(false, what + QStringLiteral(": строка %1 без записи").arg(row));
            continue;
        }
        const QString path = e->sourceFile()->filePath;
        const KeyedFile* file = nullptr;
        for (const KeyedFile& f : files)
            if (f.path == path)
                file = &f;
        if (!file)
            continue;   // обычный лог слитой вкладки — его проверяет порядок
        const int line = e->originalLineNumber() - 1;
        const QString at = what + QStringLiteral(": %1:%2").arg(QFileInfo(path).fileName()).arg(line + 1);
        if (line < 0 || line >= file->fixture.lineRecord.size()) {
            CHECK(false, at + QStringLiteral(" — строки нет в фикстуре"));
            continue;
        }
        const int rec = file->fixture.lineRecord[line];
        const keyedfx::Record expected = rec >= 0 ? file->fixture.records[rec] : keyedfx::Record{};
        const QDateTime ts = m.visibleTimestampAt(row);
        const qint64 ms = ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs;
        CHECK(ms == expected.ms, at + QStringLiteral(": момент %1, ожидается %2")
                                      .arg(describe(ts), describe(QDateTime::fromMSecsSinceEpoch(expected.ms))));
        CHECK(m.visibleLevelAt(row) == expected.level,
              at + QStringLiteral(": уровень %1, ожидается %2")
                       .arg(describe(m.visibleLevelAt(row)), describe(expected.level)));
        // Слитая вкладка — по времени; одно-файловая фикстура и так монотонна.
        if (ms != kNoTimestampMs) {
            CHECK(ms >= previousMs, at + QStringLiteral(": момент меньше предыдущей строки"));
            previousMs = ms;
        }
        const QString recordKey = path + u'#' + QString::number(rec);
        const QString idKey = path + u'#' + QString::number(e->logicalEntryId());
        if (idOfRecord.contains(recordKey))
            CHECK(idOfRecord.value(recordKey) == e->logicalEntryId(),
                  at + QStringLiteral(": строка не в своей записи"));
        else
            idOfRecord.insert(recordKey, e->logicalEntryId());
        if (recordOfId.contains(idKey))
            CHECK(recordOfId.value(idKey) == recordKey,
                  at + QStringLiteral(": две записи фикстуры слиплись в одну"));
        else
            recordOfId.insert(idKey, recordKey);
        const bool firstOfRecord = line == 0 || file->fixture.lineRecord[line - 1] != rec;
        if (firstOfRecord && !expected.message.isEmpty() && messageColumn >= 0) {
            const QString got = m.fieldsOf(*e).value(messageColumn, e->message());
            CHECK(got == expected.message,
                  at + QStringLiteral(": Message %1, ожидается %2")
                           .arg(describe(got), describe(expected.message)));
        }
    }
}

// Обычный лог с явной зоной: строки встают между записями ключевой фикстуры
// (момент base + i секунд + 500 мс).
static QByteArray keyedNeighbourLog(int records)
{
    QByteArray b;
    for (int i = 0; i < records; ++i)
        b += keyedfx::iso(i, 500) + " [p] INFO - plain neighbour " + QByteArray::number(i) + '\n';
    return b;
}

// Слитая вкладка ключевого и обычного лога: заданный порядок — строки по
// моменту; у равных моментов разных файлов порядок дал бы путь, здесь их нет.
static void checkKeyedMergeOrder(const QString& what, const LogModel& m,
                                 const keyedfx::Fixture& fx, const QString& keyedPath,
                                 int neighbourRecords)
{
    // Ожидаемая последовательность первичных строк: (момент, путь, строка).
    struct Primary { qint64 ms; bool keyed; int index; };
    QVector<Primary> expected;
    for (int r = 0; r < fx.records.size(); ++r)
        expected.append({fx.records[r].ms, true, r});
    for (int i = 0; i < neighbourRecords; ++i)
        expected.append({keyedfx::baseMs() + i * 1000LL + 500, false, i});
    std::stable_sort(expected.begin(), expected.end(),
                     [](const Primary& a, const Primary& b) { return a.ms < b.ms; });
    QVector<Primary> got;
    int lastId = -1;
    const LogFile* lastFile = nullptr;
    for (int row = 0; row < m.rowCount(); ++row) {
        const std::shared_ptr<LogEntry> e = m.entryAt(row);
        if (!e)
            continue;
        if (e->logicalEntryId() == lastId && e->sourceFile().get() == lastFile)
            continue;
        lastId = e->logicalEntryId();
        lastFile = e->sourceFile().get();
        const bool keyed = e->sourceFile()->filePath == keyedPath;
        const QDateTime ts = m.visibleTimestampAt(row);
        got.append({ts.isValid() ? ts.toMSecsSinceEpoch() : kNoTimestampMs, keyed,
                    keyed ? fx.lineRecord.value(e->originalLineNumber() - 1, -1)
                          : e->originalLineNumber() - 1});
    }
    CHECK(got.size() == expected.size(),
          what + QStringLiteral(": записей %1, ожидается %2").arg(got.size()).arg(expected.size()));
    for (int k = 0; k < qMin(got.size(), expected.size()); ++k) {
        if (got[k].ms != expected[k].ms || got[k].keyed != expected[k].keyed
            || got[k].index != expected[k].index) {
            CHECK(false, what + QStringLiteral(": запись %1 — %2 #%3, ожидается %4 #%5")
                                    .arg(k)
                                    .arg(got[k].keyed ? "ключевая" : "обычная").arg(got[k].index)
                                    .arg(expected[k].keyed ? "ключевая" : "обычная")
                                    .arg(expected[k].index));
            return;
        }
    }
}

// Без фильтров: весь API и заданные значения.
static void compareKeyedUnfiltered(const QString& what, Document& res, Document& idx,
                                   const QVector<KeyedFile>& files, const QStringList& fieldNames)
{
    g_context = what;
    g_contextFailures = 0;
    FilterConfig none;
    none.name = QStringLiteral("без фильтров");
    applyChecked(*res.model, none, fieldNames);
    applyChecked(*idx.model, none, fieldNames);
    compareAll(res, idx);
    checkKeyedExpectations(what + QStringLiteral(" / резидентный"), *res.model, files, fieldNames);
    checkKeyedExpectations(what + QStringLiteral(" / индексный"), *idx.model, files, fieldNames);
}

static QString writeKeyedIn(const QDir& dir, const QString& sub, const QString& name,
                            const QByteArray& bytes)
{
    dir.mkpath(sub);
    return writeFile(QDir(dir.filePath(sub)), name, bytes);
}

static void testKeyedSchemas(const QDir& dir)
{
    using Format = KeyedDefinition::Format;
    for (const Format format : {Format::JsonObject, Format::Logfmt}) {
        const bool json = format == Format::JsonObject;
        const QString tag = json ? QStringLiteral("JSON Lines") : QStringLiteral("logfmt");
        const QString ext = json ? QStringLiteral(".jsonl") : QStringLiteral(".logfmt");
        const QString keyedSchema = keyedfx::schema(format);
        const LogPattern keyedPattern(keyedSchema);
        CHECK(keyedPattern.isValid() && keyedPattern.isKeyed(), tag + QStringLiteral(": схема"));
        const QStringList fieldNames = keyedPattern.fieldNames();

        const keyedfx::Fixture fx = keyedfx::make(format, 96);
        const QString path = writeFile(dir, QStringLiteral("keyed") + ext, fx.bytes);
        const QVector<KeyedFile> files = {{path, fx}};

        // Все конфигурации фильтров, весь API.
        runFixture(tag + QStringLiteral(", один файл"), {path}, keyedSchema, fieldNames);
        {
            Document res = loadResident({path}, keyedSchema);
            Document idx = loadIndexed({path}, keyedSchema);
            compareKeyedUnfiltered(tag + QStringLiteral(" / заданные значения"), res, idx, files,
                                   fieldNames);
        }

        // Слитая вкладка с обычным логом.
        const QString neighbour = writeFile(dir, QStringLiteral("keyed-neighbour-%1.log").arg(json),
                                            keyedNeighbourLog(60));
        for (const IndexedLoad mode : {IndexedLoad::Sequential, IndexedLoad::Concurrent}) {
            runFixture(tag + QStringLiteral(" + обычный лог, ") + describe(mode), {path, neighbour},
                       keyedSchema, fieldNames, mode);
            Document res = loadResident({path, neighbour}, keyedSchema);
            Document idx = loadIndexed({path, neighbour}, keyedSchema, mode);
            const QString what = tag + QStringLiteral(" + обычный лог / порядок, ") + describe(mode);
            compareKeyedUnfiltered(what, res, idx, files, fieldNames);
            checkKeyedMergeOrder(what + QStringLiteral(" / резидентный"), *res.model, fx, path, 60);
            checkKeyedMergeOrder(what + QStringLiteral(" / индексный"), *idx.model, fx, path, 60);
        }

        // Дозапись целыми строками на обоих бэкендах: продолжение в начале
        // порции остаётся в записи перед ним. Файлы бэкендов — под одним
        // именем в разных каталогах, сверяется имя (g_fileNameOnly).
        {
            const keyedfx::Fixture whole = keyedfx::make(format, 40);
            // Голова — до первого продолжения многострочной записи 36 (форма 4).
            int cut = 0;
            for (int line = 0; line < whole.lineRecord.size(); ++line) {
                if (line > 0 && whole.lineRecord[line] == 36 && whole.lineRecord[line - 1] == 36)
                    break;
                cut = int(whole.bytes.indexOf('\n', cut)) + 1;
            }
            const QByteArray head = whole.bytes.left(cut);
            const QString name = QStringLiteral("keyed-append") + ext;
            const QString resPath = writeKeyedIn(dir, QStringLiteral("res"), name, head);
            const QString idxPath = writeKeyedIn(dir, QStringLiteral("idx"), name, head);
            Document res = loadResident({resPath}, keyedSchema);
            Document idx = loadIndexed({idxPath}, keyedSchema);
            const QByteArray rest = whole.bytes.mid(cut);
            CHECK(appendResidentTail(res, 0, rest, keyedSchema), tag + QStringLiteral(": резидентная дозапись"));
            CHECK(appendIndexedTail(idx, 0, rest, keyedSchema), tag + QStringLiteral(": индексная дозапись"));
            settleChecked(*res.model);
            settleChecked(*idx.model);
            g_fileNameOnly = true;
            compareKeyedUnfiltered(tag + QStringLiteral(" / дозапись строками"), res, idx,
                                   {{resPath, whole}, {idxPath, whole}}, fieldNames);
            g_fileNameOnly = false;
        }

        // Хвост без '\n', дописанный частями: обрыв посреди вложенного
        // объекта, посреди строки в кавычках и посреди \u-последовательности.
        // Индексная вкладка после каждой порции = резидентная, загрузившая
        // те же байты разом; в конце — заданные значения.
        {
            keyedfx::Fixture whole = keyedfx::make(format, 24);
            const QByteArray last = json
                ? "{\"ts\":\"" + keyedfx::iso(30, 0) + "\",\"ctx\":{\"a\":{\"b\":\"x\\\"y\"}},"
                  "\"level\":\"fatal\",\"msg\":\"\\u041a\\u044d\\u0448 end\",\"Thread\":\"worker-1\"}"
                : "ts=" + keyedfx::iso(30, 0) + " level=fatal msg=\"Кэш \\\"end\\\"\" Thread=worker-1";
            const QString lastMessage = json ? QStringLiteral("Кэш end") : QStringLiteral("Кэш \"end\"");
            QVector<int> cuts;
            if (json) {
                cuts << int(last.indexOf("\"b\"")) + 2         // внутри вложенного
                     << int(last.indexOf("x\\\"")) + 2        // внутри строки, за «\»
                     << int(last.indexOf("\\u041a")) + 4;      // внутри \u
            } else {
                cuts << int(last.indexOf("level=")) + 3
                     << int(last.indexOf("\\\"end")) + 1;     // внутри строки, за «\»
            }
            const QString name = QStringLiteral("keyed-tail") + ext;
            QByteArray bytes = whole.bytes;
            const QString idxPath = writeKeyedIn(dir, QStringLiteral("idx"), name, bytes);
            Document idx = loadIndexed({idxPath}, keyedSchema);
            Document res;
            QString resPath;
            int done = 0;
            for (int k = 0; k <= cuts.size(); ++k) {
                const int to = k < cuts.size() ? cuts[k] : int(last.size());
                QByteArray part = last.mid(done, to - done);
                if (k == cuts.size())
                    part += '\n';
                done = to;
                bytes += part;
                CHECK(appendIndexedTail(idx, 0, part, keyedSchema, /*markNew=*/false),
                      tag + QStringLiteral(": хвост, порция %1").arg(k));
                settleChecked(*idx.model);
                resPath = writeKeyedIn(dir, QStringLiteral("res%1").arg(k), name, bytes);
                res = loadResident({resPath}, keyedSchema);
                const QString what = tag + QStringLiteral(" / хвост частями, порция %1").arg(k);
                g_context = what;
                g_contextFailures = 0;
                g_fileNameOnly = true;
                compareVisibleRows(*res.model, *idx.model);
                compareLogicalRecords(*res.model, *idx.model);
                g_fileNameOnly = false;
            }
            whole.bytes = bytes;
            whole.records.append(keyedfx::Record{keyedfx::baseMs() + 30 * 1000, LogLevel::Fatal,
                                                 lastMessage});
            whole.lineRecord.append(int(whole.records.size()) - 1);
            g_fileNameOnly = true;
            compareKeyedUnfiltered(tag + QStringLiteral(" / хвост частями, итог"), res, idx,
                                   {{resPath, whole}, {idxPath, whole}}, fieldNames);
            g_fileNameOnly = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Схема на файл (F-5, PR 2): слитая вкладка из двух файлов с разными схемами —
// регексной (Timestamp, Worker, Level, Message) и ключевой JSON Lines F-4
// (Timestamp, Level, Message, Thread). Колонки вкладки — объединение по
// имени в порядке схем файлов; общие — Timestamp, Level, Message, своя у
// каждого — Worker и Thread. Бэкенды согласны во всём API во всех
// конфигурациях фильтров, в обоих сценариях подключения индексной вкладки и
// в обоих порядках файлов (первая схема — тождественная, вторая — с
// переносом колонок). Сверх сравнения бэкендов — заданные значения:
//   • выбор колонок, задевающий только один файл, — у строк другого сырой
//     текст, у своих — значение колонки;
//   • правило по колонке одного файла — «не содержит» на строках другого;
//     по общей колонке — строки обоих; по колонке, которой нет во вкладке, —
//     нейтрально;
//   • моменты, уровни, записи и Message ключевого файла — как в фикстуре.
// ---------------------------------------------------------------------------
static QString workerSchema()
{
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        patternBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Worker"),
                     QStringLiteral("["), QStringLiteral("]")),
        patternBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"), QString(),
                     QString(), QStringLiteral("-")),
        patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
    };
    return LogPattern::serializeDefinition(def);
}

// Конфигурации, которых нет среди общих: колонка одного файла, общая,
// отсутствующая, выбор колонок одного файла.
static QVector<FilterConfig> mixedSchemaConfigs()
{
    QVector<FilterConfig> cfgs;
    const auto ruleCfg = [&](const QString& name, FilterRule::Action action, const QString& text,
                             const QString& field) {
        FilterConfig c;
        c.name = name;
        c.fieldScopeActive = true;
        c.rules.rules << rule(action, text, FilterRule::Connector::And, field);
        cfgs << c;
    };
    ruleCfg(QStringLiteral("схемы: Worker (только регексная) содержит worker-1"),
            FilterRule::Action::Include, QStringLiteral("worker-1"), QStringLiteral("Worker"));
    ruleCfg(QStringLiteral("схемы: Thread (только ключевая) содержит worker"),
            FilterRule::Action::Include, QStringLiteral("worker"), QStringLiteral("Thread"));
    ruleCfg(QStringLiteral("схемы: Exclude Thread содержит worker-2"),
            FilterRule::Action::Exclude, QStringLiteral("worker-2"), QStringLiteral("Thread"));
    ruleCfg(QStringLiteral("схемы: общая Level содержит ERROR"),
            FilterRule::Action::Include, QStringLiteral("ERROR"), QStringLiteral("Level"));
    ruleCfg(QStringLiteral("схемы: колонки Span нет во вкладке — нейтрально"),
            FilterRule::Action::Include, QStringLiteral("zzz"), QStringLiteral("Span"));
    const auto shown = [&](const QString& name, const QStringList& columns) {
        FilterConfig c;
        c.name = name;
        c.fieldFilterEnabled = true;
        c.visibleFields.clear();
        // Индексы — по именам при прогоне (порядок колонок зависит от порядка
        // файлов): имена кладутся в name после «:», см. visibleByName.
        c.name += QStringLiteral(" [") + columns.join(u',') + u']';
        cfgs << c;
    };
    shown(QStringLiteral("схемы: показана только Worker"), {QStringLiteral("Worker")});
    shown(QStringLiteral("схемы: показана только Thread"), {QStringLiteral("Thread")});
    shown(QStringLiteral("схемы: показаны Thread и Message"),
          {QStringLiteral("Thread"), QStringLiteral("Message")});
    // Сняты все галочки: строка с полями пуста, как у вкладки с одной схемой
    // (правило сырого текста — только при непустом выборе).
    shown(QStringLiteral("схемы: выбор колонок пуст"), {});
    return cfgs;
}

// Колонки «показаны» из имени конфигурации (mixedSchemaConfigs) → индексы.
static QStringList shownColumnNames(const FilterConfig& cfg)
{
    const qsizetype open = cfg.name.lastIndexOf(u'[');
    if (!cfg.fieldFilterEnabled || open < 0 || !cfg.name.endsWith(u']')
        || cfg.name.endsWith(QStringLiteral("[]")))
        return {};
    return cfg.name.mid(open + 1, cfg.name.size() - open - 2).split(u',');
}

// Заданные значения на одной модели после конфигурации cfg.
static void checkMixedSchemaRows(const QString& what, const LogModel& m, const FilterConfig& cfg,
                                 const QString& keyedPath, int unfilteredRows,
                                 const QStringList& columns)
{
    const QStringList shownNames = shownColumnNames(cfg);
    const bool emptySelection = cfg.fieldFilterEnabled && cfg.name.endsWith(QStringLiteral("[]"));
    const QString onlyField = cfg.fieldScopeActive && cfg.rules.rules.size() == 1
        ? cfg.rules.rules.first().fieldName : QString();
    bool sawKeyed = false, sawRegex = false;
    for (int row = 0; row < m.rowCount(); ++row) {
        if (g_contextFailures > kFailuresPerContext)
            return;
        const std::shared_ptr<LogEntry> e = m.entryAt(row);
        if (!e)
            continue;
        const bool keyed = e->sourceFile()->filePath == keyedPath;
        (keyed ? sawKeyed : sawRegex) = true;
        const QString at = what + QStringLiteral(": строка %1 (%2:%3)")
                                      .arg(row).arg(keyed ? "ключевой" : "регексный")
                                      .arg(e->originalLineNumber());
        if (emptySelection && !m.fieldsOf(*e).isEmpty()) {
            const QString got = m.data(m.index(row, 0), Qt::DisplayRole).toString();
            CHECK(got.isEmpty(), at + QStringLiteral(": пустой выбор колонок — показ %1, ожидается пусто")
                                      .arg(describe(got)));
        }
        if (!shownNames.isEmpty() && !emptySelection && !m.fieldsOf(*e).isEmpty()) {
            // Своя колонка у файла есть — значение; нет ни одной — сырой текст.
            QStringList parts;
            bool schemaHasShown = false;
            for (const QString& name : shownNames) {
                const int column = int(columns.indexOf(name));
                const bool own = keyed ? name != QStringLiteral("Worker")
                                       : name != QStringLiteral("Thread");
                schemaHasShown = schemaHasShown || own;
                const QString v = m.fieldsOf(*e).value(column, e->message(), true);
                if (own && !v.isEmpty())
                    parts << v;
            }
            const QString expected = schemaHasShown ? parts.join(u' ') : e->message();
            const QString got = m.data(m.index(row, 0), Qt::DisplayRole).toString();
            CHECK(got == expected, at + QStringLiteral(": показ %1, ожидается %2")
                                        .arg(describe(got), describe(expected)));
        }
        if (onlyField == QStringLiteral("Worker") || onlyField == QStringLiteral("Thread")) {
            const bool own = keyed == (onlyField == QStringLiteral("Thread"));
            const bool include = cfg.rules.rules.first().action == FilterRule::Action::Include;
            // Include по чужой колонке не пропускает ни одной строки файла.
            if (include)
                CHECK(own, at + QStringLiteral(": правило по колонке ") + onlyField
                               + QStringLiteral(" пропустило строку файла без неё"));
        }
    }
    if (onlyField == QStringLiteral("Span"))
        CHECK(m.rowCount() == unfilteredRows,
              what + QStringLiteral(": правило по отсутствующей колонке не нейтрально (%1 из %2)")
                         .arg(m.rowCount()).arg(unfilteredRows));
    if (onlyField == QStringLiteral("Level"))
        CHECK(sawKeyed && sawRegex, what + QStringLiteral(": общая колонка Level — строки обоих файлов"));
    if (onlyField == QStringLiteral("Thread")
        && cfg.rules.rules.first().action == FilterRule::Action::Exclude)
        CHECK(sawRegex, what + QStringLiteral(": Exclude по чужой колонке оставляет строки файла без неё"));
}

static void testMixedSchemas(const QDir& dir)
{
    const QString regexSchema = workerSchema();
    const QString keyedSchema = keyedfx::schema(KeyedDefinition::Format::JsonObject);
    const keyedfx::Fixture fx = keyedfx::make(KeyedDefinition::Format::JsonObject, 120);
    const QString keyedPath = writeFile(dir, QStringLiteral("mixed-keyed.jsonl"), fx.bytes);
    // Регексный лог в те же минуты: строки обоих файлов перемежаются.
    const QString regexPath = writeFile(
        dir, QStringLiteral("mixed-regex.log"),
        makeLogBytes(200, 21u, "\n", QDateTime::fromMSecsSinceEpoch(keyedfx::baseMs()),
                     /*withPreamble*/ true, /*trailingNewline*/ true, /*monotonic*/ false));
    const QVector<KeyedFile> keyedFiles = {{keyedPath, fx}};

    struct Order { QString name; QStringList paths; QStringList schemas; };
    const QVector<Order> orders = {
        {QStringLiteral("регексная, затем ключевая"), {regexPath, keyedPath}, {regexSchema, keyedSchema}},
        {QStringLiteral("ключевая, затем регексная"), {keyedPath, regexPath}, {keyedSchema, regexSchema}},
    };
    for (const Order& order : orders) {
        for (const IndexedLoad mode : {IndexedLoad::Sequential, IndexedLoad::Concurrent}) {
            const QString fixtureName = QStringLiteral("схема на файл, ") + order.name
                                      + QStringLiteral(", ") + describe(mode);
            g_context = fixtureName + QStringLiteral(" / загрузка");
            g_contextFailures = 0;
            // Резидентная: файлы дописываются во вкладку по одному, каждый
            // своей схемой; индексная — по одному или разом.
            Document res;
            res.model = std::make_unique<LogModel>();
            for (int i = 0; i < order.paths.size(); ++i)
                addResidentFiles(res, {order.paths[i]}, order.schemas[i]);
            reportLoadErrors(res);
            CHECK(settle(*res.model), QStringLiteral("резидентная модель не пришла в покой"));
            Document idx;
            idx.model = std::make_unique<LogModel>();
            addIndexedFiles(idx, order.paths, QString(), mode, order.schemas);
            reportLoadErrors(idx);
            CHECK(settle(*idx.model), QStringLiteral("индексная модель не пришла в покой"));

            const QStringList columns = res.layout.columns;
            CHECK_EQ(columns, idx.layout.columns, QStringLiteral("колонки вкладки"));
            const QStringList expectedColumns = order.paths.first() == regexPath
                ? QStringList{"Timestamp", "Worker", "Level", "Message", "Thread"}
                : QStringList{"Timestamp", "Level", "Message", "Thread", "Worker"};
            CHECK_EQ(columns, expectedColumns, QStringLiteral("объединение колонок по имени"));
            const FileSchemaPtr first = res.model->fileSchemaFor(order.paths.first());
            const FileSchemaPtr second = res.model->fileSchemaFor(order.paths.last());
            CHECK(first && first->identity && second && !second->identity,
                  QStringLiteral("первая схема тождественна, вторая — с переносом колонок"));

            compareKeyedUnfiltered(fixtureName + QStringLiteral(" / заданные значения"), res, idx,
                                   keyedFiles, columns);
            const int unfiltered = res.model->rowCount();

            QVector<FilterConfig> extra = mixedSchemaConfigs();
            for (FilterConfig& c : extra)
                for (const QString& name : shownColumnNames(c))
                    c.visibleFields << int(columns.indexOf(name));
            runLoaded(fixtureName, res, idx, columns, extra, [&](const FilterConfig& cfg) {
                checkMixedSchemaRows(g_context + QStringLiteral(" / резидентный"), *res.model, cfg,
                                     keyedPath, unfiltered, columns);
                checkMixedSchemaRows(g_context + QStringLiteral(" / индексный"), *idx.model, cfg,
                                     keyedPath, unfiltered, columns);
            });
        }
    }
}

// ---------------------------------------------------------------------------
// ANSI-последовательности (F-3): цветной лог против своего бесцветного
// близнеца. Строки с кодами снимаются на обоих бэкендах одинаково: текст без
// кодов, моменты (метка за кодом — ведущая), уровни (слово уровня в цвете) и
// записи — как у близнеца; все конфигурации фильтров (правила по тексту видят
// текст без кодов); хвост, оборванный посреди последовательности.
// ---------------------------------------------------------------------------

static const char* const kLevelColors[] = {"\x1b[2m", "\x1b[36m", "\x1b[32m", "\x1b[1;33m",
                                           "\x1b[31m", "\x1b[1;37;41m"};

// Раскраска строки лога makeLogBytes: метка, поток и уровень — в своих цветах,
// продолжения — серым, иногда заголовок окна (OSC), стирание до конца строки и
// сброс без параметров.
static QByteArray colorizeLine(const QByteArray& line, int n)
{
    if (line.isEmpty())
        return n % 3 == 0 ? QByteArray("\x1b[0m") : line;
    if (line.startsWith("    at "))
        return "\x1b[90m" + line + "\x1b[39m";
    const int tsEnd = line.indexOf(" [");
    const int thEnd = line.indexOf("] ", tsEnd);
    if (tsEnd < 0 || thEnd < 0)
        return "\x1b[1m" + line + "\x1b[22m";
    const int lvEnd = line.indexOf(' ', thEnd + 2);
    const QByteArray ts = line.left(tsEnd);
    const QByteArray thread = line.mid(tsEnd + 2, thEnd - tsEnd - 2);
    const QByteArray level = line.mid(thEnd + 2, lvEnd - thEnd - 2);
    const QByteArray rest = line.mid(lvEnd);
    QByteArray out;
    if (n % 7 == 0)
        out += "\x1b]0;app - " + QByteArray::number(n) + "\x07";
    out += (n % 2 ? "\x1b[38;5;244m" : "\x1b[38;2;120;120;120m") + ts + "\x1b[0m";
    out += " [\x1b[38;5;208m" + thread + "\x1b[m] ";
    out += QByteArray(kLevelColors[n % 6]) + level + "\x1b[0m" + rest;
    if (n % 5 == 0)
        out += "\x1b[K";
    return out;
}

static QByteArray colorizeLog(const QByteArray& plain)
{
    QByteArray out;
    const QList<QByteArray> lines = plain.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        const bool cr = line.endsWith('\r');
        if (cr)
            line.chop(1);
        out += colorizeLine(line, i);
        if (cr)
            out += '\r';
        if (i + 1 < lines.size())
            out += '\n';
    }
    return out;
}

// Видимые строки вкладки — те же, что у близнеца: текст без кодов, момент,
// уровень, запись.
static void checkSameAsTwin(const QString& what, const LogModel& colored, const LogModel& twin)
{
    g_context = what;
    g_contextFailures = 0;
    CHECK_EQ(colored.rowCount(), twin.rowCount(), QStringLiteral("число строк"));
    const int rows = qMin(colored.rowCount(), twin.rowCount());
    for (int row = 0; row < rows; ++row) {
        const QString text = colored.messageAt(row);
        CHECK(!text.contains(QChar(0x1b)), QStringLiteral("строка %1: ESC в тексте").arg(row));
        CHECK_EQ(text, twin.messageAt(row), QStringLiteral("строка %1: текст").arg(row));
        CHECK_EQ(colored.visibleTimestampAt(row), twin.visibleTimestampAt(row),
                 QStringLiteral("строка %1: момент").arg(row));
        CHECK_EQ(colored.visibleLevelAt(row), twin.visibleLevelAt(row),
                 QStringLiteral("строка %1: уровень").arg(row));
        CHECK_EQ(colored.keyForRow(row).logicalEntryId, twin.keyForRow(row).logicalEntryId,
                 QStringLiteral("строка %1: запись").arg(row));
    }
}

static void testAnsiSequences(const QDir& dir, const QString& schema, const QStringList& fieldNames)
{
    const QDateTime base = fixtureBaseTime();
    // Одно-файловая индексная вкладка не сортирует (testSingleFileOrdering):
    // первый файл монотонный и без преамбулы, второй — с ней.
    const QByteArray plainA = makeLogBytes(300, 41u, "\n", base, false, true, true);
    const QByteArray plainB = makeLogBytes(220, 42u, "\r\n", base.addSecs(60), true, false, false);
    dir.mkpath(QStringLiteral("ansi"));
    dir.mkpath(QStringLiteral("ansi-twin"));
    const QDir colorDir(dir.filePath(QStringLiteral("ansi")));
    const QDir twinDir(dir.filePath(QStringLiteral("ansi-twin")));
    const QString colorA = writeFile(colorDir, QStringLiteral("app.log"), colorizeLog(plainA));
    const QString colorB = writeFile(colorDir, QStringLiteral("web.log"), colorizeLog(plainB));
    const QString twinA = writeFile(twinDir, QStringLiteral("app.log"), plainA);
    const QString twinB = writeFile(twinDir, QStringLiteral("web.log"), plainB);

    runFixture(QStringLiteral("ANSI, один файл"), {colorA}, schema, fieldNames);
    for (const IndexedLoad mode : {IndexedLoad::Sequential, IndexedLoad::Concurrent})
        runFixture(QStringLiteral("ANSI, два файла, ") + describe(mode), {colorA, colorB}, schema,
                   fieldNames, mode);

    // Против близнеца: те же строки (файлы — под одними именами в разных
    // каталогах, слияние по времени и имени совпадает).
    for (const QStringList& paths : {QStringList{colorA}, QStringList{colorA, colorB}}) {
        const QStringList twins = paths.size() == 1 ? QStringList{twinA} : QStringList{twinA, twinB};
        const QString tag = QStringLiteral("ANSI против близнеца, %1 файл(а)").arg(paths.size());
        Document res = loadResident(paths, schema);
        Document idx = loadIndexed(paths, schema);
        Document resTwin = loadResident(twins, schema);
        Document idxTwin = loadIndexed(twins, schema);
        checkSameAsTwin(tag + QStringLiteral(" / резидентный"), *res.model, *resTwin.model);
        checkSameAsTwin(tag + QStringLiteral(" / индексный"), *idx.model, *idxTwin.model);
        // Отпечатки строк (R-1) — как у близнеца и равны между бэкендами.
        g_context = tag + QStringLiteral(" / отпечатки");
        for (int row = 0; row < res.model->rowCount() && row < idx.model->rowCount(); ++row) {
            const quint64 r = RecordFingerprint::ofLineText(res.model->messageAt(row));
            CHECK(r == RecordFingerprint::ofLineText(resTwin.model->messageAt(row))
                      && r == RecordFingerprint::ofLineText(idx.model->messageAt(row)),
                  QStringLiteral("строка %1").arg(row));
        }
        // Стиль ANSI есть у раскрашенных строк на обоих бэкендах, и он один.
        g_context = tag + QStringLiteral(" / стиль");
        int styled = 0;
        for (int row = 0; row < res.model->rowCount() && row < idx.model->rowCount(); ++row) {
            const QVariant rv = res.model->data(res.model->index(row), LogModel::AnsiStyleRole);
            const QVariant iv = idx.model->data(idx.model->index(row), LogModel::AnsiStyleRole);
            CHECK(rv.isValid() == iv.isValid()
                      && rv.value<AnsiStyles>() == iv.value<AnsiStyles>(),
                  QStringLiteral("строка %1: стиль бэкендов различается").arg(row));
            styled += rv.isValid() ? 1 : 0;
        }
        CHECK(styled > res.model->rowCount() / 2,
              QStringLiteral("раскрашенных строк %1 из %2").arg(styled).arg(res.model->rowCount()));
    }

    // Хвост, оборванный посреди последовательности: индексная — двумя
    // порциями (первая без '\n', кончается внутри CSI), резидентная — разом;
    // обе = вкладка, открытая сразу на всех байтах.
    {
        const QByteArray head = colorizeLog(makeLogBytes(80, 43u, "\n", base, false, true, true));
        const QByteArray tailA = "\x1b[38;5;244m" + base.addSecs(3600).toString(
                                     QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8()
                               + "\x1b[0m [main] \x1b[3";
        const QByteArray tailB = "1mERROR\x1b[0m - tail \x1b]0;t\x07" "done\n"
                                 "\x1b[90m    at com.example.Tail.call(Tail.java:1)\x1b[0m\n"
                                 "\x1b[32m" + base.addSecs(3601).toString(
                                     QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8()
                               + "\x1b[0m [main] \x1b[32mINFO\x1b[0m - last\n";
        dir.mkpath(QStringLiteral("ansi-tail-res"));
        dir.mkpath(QStringLiteral("ansi-tail-idx"));
        dir.mkpath(QStringLiteral("ansi-tail-whole"));
        const QString resPath = writeFile(QDir(dir.filePath(QStringLiteral("ansi-tail-res"))),
                                          QStringLiteral("tail.log"), head);
        const QString idxPath = writeFile(QDir(dir.filePath(QStringLiteral("ansi-tail-idx"))),
                                          QStringLiteral("tail.log"), head);
        const QString wholePath = writeFile(QDir(dir.filePath(QStringLiteral("ansi-tail-whole"))),
                                            QStringLiteral("tail.log"), head + tailA + tailB);
        Document res = loadResident({resPath}, schema);
        Document idx = loadIndexed({idxPath}, schema);
        g_context = QStringLiteral("ANSI, хвост посреди последовательности");
        g_contextFailures = 0;
        CHECK(appendIndexedTail(idx, 0, tailA, schema),
              QStringLiteral("индексная дозапись (оборвана внутри CSI)"));
        settleChecked(*idx.model);
        const QString provisional = idx.model->messageAt(idx.model->rowCount() - 1);
        CHECK(!provisional.contains(QChar(0x1b)) && provisional.endsWith(QStringLiteral("[main] ")),
              QStringLiteral("предварительная строка: %1").arg(describe(provisional)));
        CHECK(appendIndexedTail(idx, 0, tailB, schema), QStringLiteral("индексная дозапись"));
        CHECK(appendResidentTail(res, 0, tailA + tailB, schema), QStringLiteral("резидентная дозапись"));
        settleChecked(*res.model);
        settleChecked(*idx.model);
        Document whole = loadIndexed({wholePath}, schema);
        Document wholeRes = loadResident({wholePath}, schema);
        g_fileNameOnly = true;
        FilterConfig none;
        none.name = QStringLiteral("без фильтров");
        applyChecked(*res.model, none, fieldNames);
        applyChecked(*idx.model, none, fieldNames);
        // Пометка «новых» у перечитанной предварительной строки у бэкендов
        // своя (не про ANSI) — сверяется содержимое строк.
        checkSameAsTwin(QStringLiteral("ANSI, хвост / резидентный против индексного"), *res.model, *idx.model);
        checkSameAsTwin(QStringLiteral("ANSI, хвост / индексный против целого"), *idx.model, *whole.model);
        checkSameAsTwin(QStringLiteral("ANSI, хвост / резидентный против целого"), *res.model, *wholeRes.model);
        g_fileNameOnly = false;
        const int last = idx.model->rowCount() - 3;
        g_context = QStringLiteral("ANSI, хвост / строка, дописанная частями");
        CHECK_EQ(idx.model->visibleLevelAt(last), LogLevel::Error, QStringLiteral("уровень"));
        CHECK(idx.model->messageAt(last).endsWith(QStringLiteral("ERROR - tail done")),
              QStringLiteral("текст: %1").arg(describe(idx.model->messageAt(last))));
    }
}

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

    // F-12: логи без слов уровня (смешанные формы с телом JSON и чистый
    // access-лог, перекрывающийся по времени с первым).
    const QString fileE = writeFile(dir, QStringLiteral("nolevel-mixed.log"),
                                    makeNoLevelBytes(240, 0, baseA, false));
    const QString fileF = writeFile(dir, QStringLiteral("nolevel-access.log"),
                                    makeNoLevelBytes(200, 1, baseA.addSecs(30), true));

    // F-13: метки до 1970 года. Монотонный файл, переходящий эпоху (строгое
    // сравнение одно-файловой вкладки), и файл из далёкого прошлого с
    // преамбулой и метками не по порядку — в слитой вкладке с обычным.
    const QString filePre = writeFile(
        dir, QStringLiteral("pre-epoch.log"),
        makeLogBytes(320, 11u, "\n", QDateTime::fromMSecsSinceEpoch(-120000), false, true, true));
    const QString filePast = writeFile(
        dir, QStringLiteral("far-past.log"),
        makeLogBytes(260, 12u, "\r\n", QDateTime(QDate(1901, 3, 5), QTime(10, 0)),
                     true, false, false));

    for (const QString& path : { fileA, fileB, fileC, fileD, fileE, fileF, filePre, filePast }) {
        if (path.isEmpty()) {
            std::fprintf(stderr, "не удалось записать фикстуру\n");
            return 2;
        }
    }

    if (qEnvironmentVariableIsSet("EQ_ONLY_KEYED")) { testKeyedSchemas(dir); return g_failures ? 1 : 0; }
    if (qEnvironmentVariableIsSet("EQ_ONLY_MIXED")) { testMixedSchemas(dir); return g_failures ? 1 : 0; }
    if (qEnvironmentVariableIsSet("EQ_ONLY_ANSI")) { testAnsiSequences(dir, schema, fieldNames); return g_failures ? 1 : 0; }
    if (qEnvironmentVariableIsSet("EQ_ONLY_PROV")) { testProvisionalEncodings(dir, schema, fieldNames); return g_failures ? 1 : 0; }
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

    runFixture(QStringLiteral("без слов уровня, один файл"), { fileE }, schema, fieldNames);
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        runFixture(QStringLiteral("без слов уровня, два файла, ") + describe(mode),
                   { fileE, fileF }, schema, fieldNames, mode);

    runFixture(QStringLiteral("до 1970, один файл через эпоху"), { filePre }, schema, fieldNames);
    for (const IndexedLoad mode : { IndexedLoad::Sequential, IndexedLoad::Concurrent })
        runFixture(QStringLiteral("до 1970 и после, три файла, ") + describe(mode),
                   { filePre, filePast, fileA }, schema, fieldNames, mode);

    testSingleFileOrdering(fileA, fileC, schema, fieldNames);
    testSingleFileUnsortedLookups(fileC, schema, fieldNames);
    testUnorderedTimeRange(dir, schema, fieldNames, fileA);
    testZonedMerge(dir, schema);
    testPre1970Merge(dir, schema);
    testAttachWithActiveFilter(fileC, fileD, schema, fieldNames);
    testLineNumbers(dir, schema);
    testEncodings(dir, schema, fieldNames);
    testResidentEncodings(dir, schema, fieldNames);
    testProvisionalEncodings(dir, schema, fieldNames);
    testKeyedSchemas(dir);
    testMixedSchemas(dir);
    testAnsiSequences(dir, schema, fieldNames);

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
