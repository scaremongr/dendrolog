#include "statisticspanel.h"
#include "appsettings.h"
#include "apptheme.h"
#include "logmodel.h"
#include "toggleswitch.h"

#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLocale>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {

// ---- Пределы сбора ---------------------------------------------------------
constexpr int kBins         = 720;     // корзины времени для темпа/всплесков
constexpr int kMaxTemplates = 100000;  // предел словаря шаблонов сообщений
constexpr int kTopMessages  = 10;
constexpr int kTopErrors    = 5;
constexpr int kMaxInsights  = 8;
constexpr int kMaxFiles     = 12;      // строк в секции Files
constexpr int kNormInputCap = 400;     // шаблон строится по префиксу строки
constexpr int kExampleCap   = 240;     // сохраняемый пример сообщения
constexpr int kCancelMask   = 0x0FFF;  // проверка cancel раз в 4096 строк

// ---- Пороги эвристик -------------------------------------------------------
// Всплеск активности: корзина выше median + 6·MAD (робастно к общему разбросу)
// и не ниже 3× среднего; интервал значим от 50 записей.
constexpr double kBurstMadK        = 6.0;
constexpr double kBurstMeanK       = 3.0;
constexpr double kBurstFloor       = 8.0;
constexpr qint64 kBurstMinTotal    = 50;
// Всплеск ошибок: Пуассоновский порог expected + 6·sqrt(expected), от 5 ошибок
// на корзину; анализ имеет смысл от 20 ошибок с таймстампом на документ.
constexpr double kSpikeSigmaK      = 6.0;
constexpr double kSpikeBinFloor    = 5.0;
constexpr int    kSpikeMinErrors   = 20;
// Спам: шаблон от 2% документа (и не меньше 100 записей), «спамом» считается
// доминирование от 20% документа либо собственная плотность ≥ 5 зап/с и ≥ 8×
// среднего темпа документа.
constexpr double kSpamShare        = 0.20;
constexpr double kSpamDensityAbs   = 5.0;
constexpr double kSpamDensityRel   = 8.0;
// Пауза: от 10 с, от 50× среднего интервала и от 0.5% всей длительности —
// иначе в многосуточных логах каждая ночь становилась бы «аномалией».
constexpr qint64 kGapAbsMinMs      = 10000;
constexpr double kGapMeanK         = 50.0;
constexpr double kGapSpanShare     = 0.005;
// Тренд: сравнение половин интервала; значимо от 500 записей и роста ×2
// (для ошибок — от 30 ошибок и роста ×3).
constexpr int    kTrendMinRecords  = 500;
constexpr double kTrendVolumeK     = 2.0;
constexpr int    kTrendMinErrors   = 30;
constexpr double kTrendErrorsK     = 3.0;

QColor mixColors(const QColor& a, const QColor& b, qreal weightB)
{
    return QColor(int(a.red()   + (b.red()   - a.red())   * weightB),
                  int(a.green() + (b.green() - a.green()) * weightB),
                  int(a.blue()  + (b.blue()  - a.blue())  * weightB));
}

QString colorSpan(const QColor& color, const QString& escapedText, bool bold = false)
{
    return QStringLiteral("<span style=\"color:%1;%2\">%3</span>")
        .arg(color.name(), bold ? QStringLiteral("font-weight:bold;") : QString(),
             escapedText);
}

// «6 d 06 h», «1 h 05 min», «4 min 30 s», «12 s», «350 ms» (как на таймлайне).
QString durationText(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    qint64 s = ms / 1000;
    if (s < 60)
        return QStringLiteral("%1 s").arg(s);
    qint64 m = s / 60;
    s %= 60;
    if (m < 60)
        return QStringLiteral("%1 min %2 s").arg(m).arg(s, 2, 10, QLatin1Char('0'));
    qint64 h = m / 60;
    m %= 60;
    if (h < 24)
        return QStringLiteral("%1 h %2 min").arg(h).arg(m, 2, 10, QLatin1Char('0'));
    const qint64 d = h / 24;
    h %= 24;
    return QStringLiteral("%1 d %2 h").arg(d).arg(h, 2, 10, QLatin1Char('0'));
}

// «12:01:10 – 12:02:20», дата добавляется только на смене дня.
QString shortRangeText(qint64 fromMs, qint64 toMs)
{
    const QDateTime from = QDateTime::fromMSecsSinceEpoch(fromMs);
    const QDateTime to   = QDateTime::fromMSecsSinceEpoch(toMs);
    return from.toString(QStringLiteral("HH:mm:ss")) + QStringLiteral(" – ")
        + to.toString(to.date() == from.date() ? QStringLiteral("HH:mm:ss")
                                               : QStringLiteral("dd.MM HH:mm:ss"));
}

// ---------------------------------------------------------------------------
// Нормализация сообщения в шаблон: каждый алфавитно-цифровой токен, содержащий
// хотя бы одну цифру, заменяется на «#». Это склеивает переменные части —
// числа, таймстампы (#-#-# #:#:#.#), IP (#.#.#.#), hex-идентификаторы, GUID —
// и оставляет неизменным словесный костяк сообщения.
// ---------------------------------------------------------------------------
QString normalizeMessage(const QString& msg)
{
    const int n = int(qMin<qsizetype>(msg.size(), kNormInputCap));
    QString out;
    out.reserve(n);
    int runStart = -1;
    bool runHasDigit = false;
    const auto flushRun = [&](int end) {
        if (runStart < 0)
            return;
        if (runHasDigit)
            out += QLatin1Char('#');
        else
            out += QStringView(msg).mid(runStart, end - runStart);
        runStart = -1;
        runHasDigit = false;
    };
    for (int i = 0; i < n; ++i) {
        const QChar c = msg.at(i);
        if (c.isLetterOrNumber() || c == QLatin1Char('_')) {
            if (runStart < 0)
                runStart = i;
            if (c.isDigit())
                runHasDigit = true;
        } else {
            flushRun(i);
            out += c;
        }
    }
    flushRun(n);
    if (msg.size() > kNormInputCap)
        out += QStringLiteral(" …");
    return out;
}

// Начало «словесной» части шаблона для показа: первый двухбуквенный токен.
// Отрезает шумовой префикс из нормализованного таймстампа («#-#-#T#:#:#.# »),
// не трогая счётчики — это чисто отображение.
int displayStart(const QString& tpl)
{
    for (int i = 0; i + 1 < tpl.size(); ++i)
        if (tpl.at(i).isLetter() && tpl.at(i + 1).isLetter())
            return i;
    return 0;
}

double medianOf(QVector<double>& values)
{
    if (values.isEmpty())
        return 0.0;
    const int mid = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + mid, values.end());
    return values[mid];
}

// Интервал подряд идущих корзин выше порога.
struct BinRun {
    int    first = 0, last = 0;   // индексы корзин включительно
    qint64 sum = 0;
};

QVector<BinRun> runsAboveThreshold(const QVector<quint32>& bins, double threshold)
{
    QVector<BinRun> runs;
    int runStart = -1;
    qint64 runSum = 0;
    for (int b = 0; b <= bins.size(); ++b) {
        const bool above = b < bins.size() && bins[b] > threshold;
        if (above) {
            if (runStart < 0) {
                runStart = b;
                runSum = 0;
            }
            runSum += bins[b];
        } else if (runStart >= 0) {
            runs.append({runStart, b - 1, runSum});
            runStart = -1;
        }
    }
    return runs;
}

// ---------------------------------------------------------------------------
// collectDocumentStats — воркер: одна проходка по снапшоту + постобработка.
// Читает только message/timestamp/level/logicalEntryId/sourceFile — поле
// fields может конкурентно переписываться переизвлечением схемы полей.
// ---------------------------------------------------------------------------
struct TemplateAgg {
    int             count = 0;
    LogLevel        level = LogLevel::Unknown;
    qint64          firstMs = -1, lastMs = -1;
    const LogEntry* firstEntry = nullptr; // жив, пока жив снапшот
};

DocumentStats collectDocumentStats(const QVector<std::shared_ptr<LogEntry>>& snapshot,
                                   const std::shared_ptr<std::atomic_bool>& cancel,
                                   int generation)
{
    QElapsedTimer timer;
    timer.start();

    DocumentStats s;
    s.generation = generation;
    s.valid = true;
    s.rows = int(snapshot.size());

    // Диапазон времени: список отсортирован по времени, записи без валидной
    // метки — в конце (как на таймлайне).
    int validEnd = int(snapshot.size());
    while (validEnd > 0) {
        const auto& e = snapshot[validEnd - 1];
        if (e && e->timestamp.isValid())
            break;
        --validEnd;
    }
    const bool hasTime = validEnd > 0 && snapshot.first()
                      && snapshot.first()->timestamp.isValid();
    if (hasTime) {
        s.tMinMs = snapshot.first()->timestamp.toMSecsSinceEpoch();
        s.tMaxMs = snapshot[validEnd - 1]->timestamp.toMSecsSinceEpoch();
    }
    const qint64 spanMs   = hasTime ? qMax<qint64>(0, s.tMaxMs - s.tMinMs) : 0;
    const double bucketMs = spanMs > 0 ? double(spanMs) / kBins : 0.0;

    QVector<quint32> totalBins(kBins, 0);
    QVector<quint32> errorBins(kBins, 0);
    QHash<QString, TemplateAgg> templates;
    templates.reserve(4096);
    QHash<const LogFile*, DocumentStats::FileStat> fileStats;

    struct GapInfo { qint64 ms = 0, fromMs = 0, toMs = 0; };
    GapInfo topGaps[3];

    int prevId = 0;
    const LogFile* prevFile = nullptr;
    bool firstRow = true;
    qint64 prevTsMs = -1;

    for (int i = 0; i < snapshot.size(); ++i) {
        if ((i & kCancelMask) == 0 && cancel
            && cancel->load(std::memory_order_relaxed)) {
            s.cancelled = true;
            return s;
        }
        const LogEntry* e = snapshot[i].get();
        if (!e)
            continue;

        const LogFile* file = e->sourceFile.get();
        // Свободный текст (не-лог файл, преамбула) слипается в запись #0
        // номинально — единицей статистики там служит каждая строка.
        const bool newRecord = e->isPlainText() || firstRow
                            || e->logicalEntryId != prevId || file != prevFile;
        firstRow = false;
        prevId = e->logicalEntryId;
        prevFile = file;

        DocumentStats::FileStat& fs = fileStats[file];
        if (!fs.file && e->sourceFile)
            fs.file = e->sourceFile;
        ++fs.lines;

        if (!newRecord)
            continue; // continuation-строка: только счётчик строк

        ++s.records;
        ++fs.records;
        ++s.levelCounts[qBound(0, int(e->level), 6)];
        switch (e->level) {
        case LogLevel::Warn:  ++fs.warn;  break;
        case LogLevel::Error: ++fs.error; break;
        case LogLevel::Fatal: ++fs.fatal; break;
        default: break;
        }

        qint64 tsMs = -1;
        if (e->timestamp.isValid()) {
            tsMs = e->timestamp.toMSecsSinceEpoch();
            ++s.recordsWithTs;
            if (fs.firstMs < 0)
                fs.firstMs = tsMs;
            fs.lastMs = tsMs;

            if (bucketMs > 0) {
                const int b = qBound(0, int((tsMs - s.tMinMs) / bucketMs), kBins - 1);
                ++totalBins[b];
                if (e->level == LogLevel::Error || e->level == LogLevel::Fatal)
                    ++errorBins[b];
            }

            if (prevTsMs >= 0) {
                const qint64 gap = tsMs - prevTsMs;
                if (gap > topGaps[2].ms) {
                    const GapInfo g{gap, prevTsMs, tsMs};
                    if (g.ms > topGaps[0].ms) {
                        topGaps[2] = topGaps[1];
                        topGaps[1] = topGaps[0];
                        topGaps[0] = g;
                    } else if (g.ms > topGaps[1].ms) {
                        topGaps[2] = topGaps[1];
                        topGaps[1] = g;
                    } else {
                        topGaps[2] = g;
                    }
                }
            }
            prevTsMs = tsMs;
        }

        const QString key = normalizeMessage(e->message);
        const auto it = templates.find(key);
        if (it != templates.end()) {
            ++it->count;
            if (tsMs >= 0) {
                if (it->firstMs < 0)
                    it->firstMs = tsMs;
                it->lastMs = tsMs;
            }
        } else if (templates.size() >= kMaxTemplates) {
            s.templatesCapped = true; // топ приблизительный: словарь переполнен
        } else {
            TemplateAgg agg;
            agg.count = 1;
            agg.level = e->level;
            agg.firstMs = agg.lastMs = tsMs;
            agg.firstEntry = e;
            templates.insert(key, agg);
        }
    }

    if (cancel && cancel->load(std::memory_order_relaxed)) {
        s.cancelled = true;
        return s;
    }

    // ---- Темп записей -------------------------------------------------------
    if (spanMs > 0 && s.recordsWithTs > 0) {
        s.avgPerSec = s.recordsWithTs * 1000.0 / spanMs;
        int peakBin = 0;
        quint32 peak = 0;
        for (int b = 0; b < kBins; ++b) {
            if (totalBins[b] > peak) {
                peak = totalBins[b];
                peakBin = b;
            }
        }
        if (peak > 0) {
            s.peakPerSec = peak * 1000.0 / bucketMs;
            s.peakFromMs = s.tMinMs + qint64(peakBin * bucketMs);
            s.peakToMs   = s.tMinMs + qint64((peakBin + 1) * bucketMs);
        }
    }

    // ---- Топ шаблонов -------------------------------------------------------
    struct TemplRef {
        const QString* key;
        const TemplateAgg* agg;
    };
    QVector<TemplRef> all;
    all.reserve(templates.size());
    QVector<TemplRef> errs;
    for (auto it = templates.constBegin(); it != templates.constEnd(); ++it) {
        all.append({&it.key(), &it.value()});
        if (it.value().level == LogLevel::Error || it.value().level == LogLevel::Fatal)
            errs.append({&it.key(), &it.value()});
    }
    const auto byCountDesc = [](const TemplRef& a, const TemplRef& b) {
        return a.agg->count > b.agg->count;
    };
    const auto makeTemplate = [](const TemplRef& r) {
        DocumentStats::Template t;
        t.text = *r.key;
        t.count = r.agg->count;
        t.level = r.agg->level;
        t.firstMs = r.agg->firstMs;
        t.lastMs = r.agg->lastMs;
        if (const LogEntry* fe = r.agg->firstEntry) {
            t.example = fe->message.left(kExampleCap);
            t.firstEntryId = fe->logicalEntryId;
            t.firstFile = fe->sourceFile;
        }
        return t;
    };
    {
        const int topN = int(qMin<qsizetype>(kTopMessages, all.size()));
        std::partial_sort(all.begin(), all.begin() + topN, all.end(), byCountDesc);
        for (int i = 0; i < topN; ++i)
            s.topMessages.append(makeTemplate(all[i]));

        const int topE = int(qMin<qsizetype>(kTopErrors, errs.size()));
        std::partial_sort(errs.begin(), errs.begin() + topE, errs.end(), byCountDesc);
        for (int i = 0; i < topE; ++i)
            s.topErrors.append(makeTemplate(errs[i]));
    }

    // ---- Эвристики ----------------------------------------------------------
    QVector<DocumentStats::Insight> insights;
    const auto binStartMs = [&](int b) { return s.tMinMs + qint64(b * bucketMs); };
    const auto binEndMs   = [&](int b) { return s.tMinMs + qint64((b + 1) * bucketMs); };

    // Всплески общей активности: робастный порог median + k·MAD.
    if (bucketMs > 0 && s.recordsWithTs >= kBurstMinTotal * 4) {
        QVector<double> vals(kBins);
        for (int b = 0; b < kBins; ++b)
            vals[b] = totalBins[b];
        const double med = medianOf(vals);
        for (int b = 0; b < kBins; ++b)
            vals[b] = std::abs(double(totalBins[b]) - med);
        const double mad = medianOf(vals);
        const double mean = double(s.recordsWithTs) / kBins;
        const double threshold = std::max({med + kBurstMadK * 1.4826 * mad,
                                           mean * kBurstMeanK, kBurstFloor});
        QVector<BinRun> runs = runsAboveThreshold(totalBins, threshold);
        std::sort(runs.begin(), runs.end(),
                  [](const BinRun& a, const BinRun& b) { return a.sum > b.sum; });
        int added = 0;
        for (const BinRun& r : runs) {
            if (added >= 2 || r.sum < kBurstMinTotal)
                break;
            const int len = r.last - r.first + 1;
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::Burst;
            in.fromMs = binStartMs(r.first);
            in.toMs = binEndMs(r.last);
            in.count = r.sum;
            in.factor = (double(r.sum) / len) / qMax(1.0, med);
            in.magnitude = double(r.sum);
            insights.append(in);
            ++added;
        }
    }

    // Всплески ошибок: Пуассоновский порог над средней плотностью ошибок.
    qint64 errWithTs = 0;
    for (int b = 0; b < kBins; ++b)
        errWithTs += errorBins[b];
    if (bucketMs > 0 && errWithTs >= kSpikeMinErrors) {
        const double expected = double(errWithTs) / kBins;
        const double threshold = std::max(kSpikeBinFloor,
                                          expected + kSpikeSigmaK
                                              * std::sqrt(expected + 0.5));
        QVector<BinRun> runs = runsAboveThreshold(errorBins, threshold);
        std::sort(runs.begin(), runs.end(),
                  [](const BinRun& a, const BinRun& b) { return a.sum > b.sum; });
        int added = 0;
        for (const BinRun& r : runs) {
            if (added >= 2)
                break;
            const int len = r.last - r.first + 1;
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::ErrorSpike;
            in.fromMs = binStartMs(r.first);
            in.toMs = binEndMs(r.last);
            in.count = r.sum;
            in.factor = double(r.sum) / qMax(1e-9, expected * len);
            in.preferErrors = true;
            in.magnitude = double(r.sum);
            insights.append(in);
            ++added;
        }
    }

    // Спам: доминирующий либо аномально плотный шаблон.
    if (s.records > 0) {
        const double overallRate = spanMs > 0 ? s.recordsWithTs * 1000.0 / spanMs
                                              : 0.0;
        QVector<TemplRef> spam;
        for (const TemplRef& r : all) {
            if (r.agg->count < qMax(100, s.records / 50))
                continue;
            const double share = double(r.agg->count) / s.records;
            double density = 0.0;
            if (r.agg->firstMs >= 0 && r.agg->lastMs > r.agg->firstMs)
                density = r.agg->count * 1000.0 / double(r.agg->lastMs - r.agg->firstMs);
            const bool dominant = share >= kSpamShare;
            const bool dense = density >= kSpamDensityAbs
                            && (overallRate <= 0.0
                                || density >= kSpamDensityRel * overallRate);
            if (dominant || dense)
                spam.append(r);
        }
        std::sort(spam.begin(), spam.end(), byCountDesc);
        for (int i = 0; i < spam.size() && i < 2; ++i) {
            const TemplateAgg* agg = spam[i].agg;
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::Spam;
            in.fromMs = agg->firstMs;
            in.toMs = agg->lastMs;
            in.count = agg->count;
            in.share = double(agg->count) / s.records;
            const QString& tpl = *spam[i].key;
            in.snippet = tpl.mid(displayStart(tpl));
            if (const LogEntry* fe = agg->firstEntry) {
                in.entryId = fe->logicalEntryId;
                in.file = fe->sourceFile;
            }
            in.magnitude = double(agg->count);
            insights.append(in);
        }
    }

    // Паузы: крупнейшие промежутки без записей.
    if (spanMs > 0 && s.recordsWithTs >= 2) {
        const double meanGapMs = double(spanMs) / s.recordsWithTs;
        const qint64 minReport = std::max<qint64>(
            {kGapAbsMinMs, qint64(meanGapMs * kGapMeanK),
             qint64(double(spanMs) * kGapSpanShare)});
        int added = 0;
        for (const GapInfo& g : topGaps) {
            if (added >= 2 || g.ms < minReport)
                break;
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::Gap;
            in.fromMs = g.fromMs;
            in.toMs = g.toMs;
            in.magnitude = double(g.ms);
            insights.append(in);
            ++added;
        }
    }

    // Тренды: сравнение половин интервала по корзинам.
    if (bucketMs > 0 && s.recordsWithTs >= kTrendMinRecords) {
        qint64 firstHalf = 0, secondHalf = 0, errFirst = 0, errSecond = 0;
        for (int b = 0; b < kBins; ++b) {
            (b < kBins / 2 ? firstHalf : secondHalf) += totalBins[b];
            (b < kBins / 2 ? errFirst : errSecond) += errorBins[b];
        }
        if (firstHalf > 0 && double(secondHalf) >= kTrendVolumeK * firstHalf) {
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::RisingVolume;
            in.factor = double(secondHalf) / firstHalf;
            in.count = secondHalf;
            in.magnitude = in.factor;
            insights.append(in);
        }
        if (errFirst + errSecond >= kTrendMinErrors
            && double(errSecond) >= kTrendErrorsK * qMax<qint64>(1, errFirst)) {
            DocumentStats::Insight in;
            in.kind = DocumentStats::Insight::RisingErrors;
            in.factor = double(errSecond) / qMax<qint64>(1, errFirst);
            in.count = errSecond;
            in.magnitude = in.factor;
            insights.append(in);
        }
    }

    // Важность: по виду (ErrorSpike первым), внутри вида — по величине.
    std::stable_sort(insights.begin(), insights.end(),
                     [](const DocumentStats::Insight& a,
                        const DocumentStats::Insight& b) {
                         if (a.kind != b.kind)
                             return a.kind < b.kind;
                         return a.magnitude > b.magnitude;
                     });
    if (insights.size() > kMaxInsights)
        insights.resize(kMaxInsights);
    s.insights = insights;

    // ---- Разбивка по файлам -------------------------------------------------
    for (auto it = fileStats.constBegin(); it != fileStats.constEnd(); ++it)
        s.files.append(it.value());
    std::sort(s.files.begin(), s.files.end(),
              [](const DocumentStats::FileStat& a, const DocumentStats::FileStat& b) {
                  return a.lines > b.lines;
              });

    s.collectMs = timer.elapsed();
    return s;
}

} // namespace

// ===========================================================================

StatisticsPanel::StatisticsPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Верхняя строка: обновить, статус, переключатель Filtered only ------
    auto* topBar = new QWidget(this);
    auto* topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(6, 4, 6, 4);
    topLayout->setSpacing(6);

    m_refreshButton = new QToolButton(topBar);
    m_refreshButton->setText(QStringLiteral("⟳"));
    m_refreshButton->setAutoRaise(true);
    m_refreshButton->setToolTip(tr("Recalculate statistics"));
    connect(m_refreshButton, &QToolButton::clicked,
            this, [this]() { startCollection(); });
    topLayout->addWidget(m_refreshButton);

    m_statusLabel = new QLabel(topBar);
    topLayout->addWidget(m_statusLabel, /*stretch=*/1);

    const QString filteredTip =
        tr("On — statistics for the visible (filtered) rows only.\n"
           "Off — statistics for the whole document.");
    m_filteredSwitch = new ToggleSwitch(topBar);
    m_filteredSwitch->setToolTip(filteredTip);
    connect(m_filteredSwitch, &ToggleSwitch::toggled, this, [this]() {
        if (isVisible())
            startCollection();
        else
            m_dirty = true;
    });
    topLayout->addWidget(m_filteredSwitch);
    auto* filteredLabel = new QLabel(tr("Filtered only"), topBar);
    filteredLabel->setToolTip(filteredTip);
    topLayout->addWidget(filteredLabel);

    layout->addWidget(topBar);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->document()->setDocumentMargin(8);
    connect(m_browser, &QTextBrowser::anchorClicked,
            this, &StatisticsPanel::onAnchorClicked);
    layout->addWidget(m_browser, /*stretch=*/1);

    // Дебаунс изменений данных: батчи rowsInserted при загрузке идут сериями.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setSingleShot(true);
    m_refreshTimer->setInterval(500);
    connect(m_refreshTimer, &QTimer::timeout, this, [this]() { startCollection(); });

    connect(&m_watcher, &QFutureWatcher<DocumentStats>::finished,
            this, &StatisticsPanel::onCollectionFinished);

    renderPlaceholder(tr("Open a log file to see document statistics."));
}

StatisticsPanel::~StatisticsPanel()
{
    // Воркер держит собственный снапшот и cancel-флаг — дожидаться его не
    // нужно, достаточно попросить остановиться.
    cancelCollection();
}

void StatisticsPanel::setModel(LogModel* model)
{
    if (m_model == model)
        return;

    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);

    m_model = model;
    // Результат джоба от прежней модели не должен применяться, даже если
    // панель скрыта и новый сбор пока не запускался.
    cancelCollection();
    ++m_generation;

    if (m_model) {
        connect(m_model, &QAbstractItemModel::rowsInserted,
                this, [this]() { scheduleRefresh(); });
        connect(m_model, &QAbstractItemModel::rowsRemoved,
                this, [this]() { scheduleRefresh(); });
        connect(m_model, &QAbstractItemModel::modelReset,
                this, [this]() { scheduleRefresh(); });
        // Смена фильтров меняет статистику только в режиме «Filtered only».
        connect(m_model, &LogModel::modelFiltered, this, [this]() {
            if (m_filteredSwitch->isChecked())
                scheduleRefresh();
        });
        // Страховка: вкладка может быть закрыта без setModel(nullptr).
        connect(m_model, &QObject::destroyed, this, [this]() {
            m_model = nullptr;
            cancelCollection();
            ++m_generation;
            m_refreshTimer->stop();
            m_stats = DocumentStats();
            renderPlaceholder(tr("Open a log file to see document statistics."));
        });
    }

    m_refreshTimer->stop();
    if (isVisible())
        startCollection();
    else
        m_dirty = true;
}

void StatisticsPanel::scheduleRefresh()
{
    if (!m_model)
        return;
    if (!isVisible()) {
        m_dirty = true;
        return;
    }
    m_refreshTimer->start();
}

void StatisticsPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_dirty)
        startCollection();
}

void StatisticsPanel::cancelCollection()
{
    if (m_cancelFlag)
        m_cancelFlag->store(true);
}

void StatisticsPanel::startCollection()
{
    m_refreshTimer->stop();
    m_dirty = false;
    cancelCollection();
    ++m_generation;

    if (!m_model) {
        m_stats = DocumentStats();
        renderPlaceholder(tr("Open a log file to see document statistics."));
        return;
    }

    // COW-копия вектора shared_ptr: воркер владеет данными независимо от
    // дальнейшей судьбы модели/вкладки.
    const QVector<std::shared_ptr<LogEntry>> snapshot =
        m_filteredSwitch->isChecked() ? m_model->filteredEntries()
                                      : m_model->allEntries();
    if (snapshot.isEmpty()) {
        m_stats = DocumentStats();
        renderPlaceholder(tr("The document is empty."));
        return;
    }

    m_statusLabel->setText(tr("Collecting statistics…"));
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancelFlag = cancel;
    const int generation = m_generation;
    m_watcher.setFuture(QtConcurrent::run([snapshot, cancel, generation]() {
        return collectDocumentStats(snapshot, cancel, generation);
    }));
}

void StatisticsPanel::onCollectionFinished()
{
    const DocumentStats stats = m_watcher.result();
    if (stats.cancelled || stats.generation != m_generation)
        return; // устаревший результат: актуальный джоб уже в полёте
    m_stats = stats;
    renderStats();
}

void StatisticsPanel::renderPlaceholder(const QString& text)
{
    m_statusLabel->setText(QString());
    m_navTargets.clear();
    const QColor muted = mixColors(m_browser->palette().color(QPalette::Text),
                                   m_browser->palette().color(QPalette::Base), 0.45);
    m_browser->setHtml(QStringLiteral("<div style=\"color:%1;\">%2</div>")
                           .arg(muted.name(), text.toHtmlEscaped()));
}

void StatisticsPanel::renderStats()
{
    const QLocale locale;
    QString status = tr("%1 records").arg(locale.toString(qlonglong(m_stats.records)));
    if (m_filteredSwitch->isChecked())
        status += tr(" (filtered view)");
    status += QStringLiteral(" · ") + tr("collected in %1 ms")
                                          .arg(locale.toString(qlonglong(m_stats.collectMs)));
    m_statusLabel->setText(status);
    m_browser->setHtml(buildHtml());
}

void StatisticsPanel::onAnchorClicked(const QUrl& url)
{
    const QString link = url.toString();
    if (!link.startsWith(QLatin1String("nav:")))
        return;
    bool ok = false;
    const int idx = link.mid(4).toInt(&ok);
    if (!ok || idx < 0 || idx >= m_navTargets.size())
        return;
    const NavTarget& target = m_navTargets[idx];
    if (target.toEntry && target.file)
        emit jumpToEntryRequested(target.entryId, target.file);
    else if (target.timeMs >= 0)
        emit jumpToTimeRequested(QDateTime::fromMSecsSinceEpoch(target.timeMs),
                                 target.preferErrors);
}

QString StatisticsPanel::buildHtml()
{
    m_navTargets.clear();

    const QPalette pal = m_browser->palette();
    const QColor text = pal.color(QPalette::Text);
    const QColor base = pal.color(QPalette::Base);
    const QColor muted = mixColors(text, base, 0.45);
    const QColor tileBg = mixColors(base, text, 0.06);
    const AppTheme& t = AppTheme::instance();
    const QLocale locale;
    const QString monoFamily = AppSettings::instance().fontFamily();

    // Разделители групп разрядов делаем неразрывными: обычный пробел в узкой
    // ячейке таблицы позволил бы перенести «33 525» посимвольно в столбик.
    const auto num = [&locale](qint64 v) {
        QString s = locale.toString(qlonglong(v));
        s.replace(QLatin1Char(' '), QChar(0x00A0));
        return s;
    };
    const auto rate = [&locale](double v) {
        if (v <= 0.0)
            return QStringLiteral("—");
        if (v < 1.0)
            return locale.toString(v, 'f', 2);
        if (v < 100.0)
            return locale.toString(v, 'f', 1);
        return locale.toString(qlonglong(std::llround(v)));
    };
    const auto factorText = [&locale](double f) {
        return QStringLiteral("×") + (f < 10.0 ? locale.toString(f, 'f', 1)
                                               : locale.toString(qlonglong(std::llround(f))));
    };
    const auto sectionHeader = [&muted](const QString& title) {
        return QStringLiteral(
                   "<div style=\"color:%1;font-weight:bold;margin-top:14px;\">%2</div>")
            .arg(muted.name(), title);
    };
    // Регистрирует адресата навигации и возвращает ссылку «nav:N».
    const auto navLink = [this](const NavTarget& target, const QString& escapedText) {
        m_navTargets.append(target);
        return QStringLiteral("<a href=\"nav:%1\">%2</a>")
            .arg(m_navTargets.size() - 1)
            .arg(escapedText);
    };
    const auto timeTarget = [](qint64 ms, bool preferErrors) {
        NavTarget nav;
        nav.timeMs = ms;
        nav.preferErrors = preferErrors;
        return nav;
    };
    const auto entryTarget = [](int entryId, const LogFilePtr& file) {
        NavTarget nav;
        nav.toEntry = true;
        nav.entryId = entryId;
        nav.file = file;
        return nav;
    };

    QString html;

    // ---- Сводные плитки -----------------------------------------------------
    const auto tile = [&](const QString& value, const QString& caption) {
        return QStringLiteral(
                   "<td bgcolor=\"%1\" style=\"padding:6px 10px;\">"
                   "<div style=\"font-size:13pt;font-weight:bold;\">%2</div>"
                   "<div style=\"color:%3;font-size:8pt;\">%4</div></td>")
            .arg(tileBg.name(), value, muted.name(), caption);
    };

    const bool hasTime = m_stats.tMinMs >= 0 && m_stats.tMaxMs >= m_stats.tMinMs;
    const qint64 spanMs = hasTime ? m_stats.tMaxMs - m_stats.tMinMs : 0;

    html += QLatin1String("<table width=\"100%\" cellspacing=\"3\" cellpadding=\"0\"><tr>");
    html += tile(num(m_stats.records), tr("log records"));
    html += tile(num(m_stats.rows), tr("lines"));
    html += tile(num(m_stats.files.size()),
                 m_stats.files.size() == 1 ? tr("source file") : tr("source files"));
    html += QLatin1String("</tr><tr>");
    html += tile(hasTime && spanMs > 0 ? durationText(spanMs) : QStringLiteral("—"),
                 tr("time span"));
    html += tile(rate(m_stats.avgPerSec), tr("records / sec, average"));
    QString peakCaption = tr("records / sec, peak");
    if (m_stats.peakFromMs >= 0) {
        const QString at = QDateTime::fromMSecsSinceEpoch(m_stats.peakFromMs)
                               .toString(QStringLiteral("HH:mm:ss"));
        peakCaption += QStringLiteral(" @ ")
                    + navLink(timeTarget(m_stats.peakFromMs, false), at);
    }
    html += tile(rate(m_stats.peakPerSec), peakCaption);
    html += QLatin1String("</tr></table>");

    if (hasTime) {
        const QDateTime from = QDateTime::fromMSecsSinceEpoch(m_stats.tMinMs);
        const QDateTime to = QDateTime::fromMSecsSinceEpoch(m_stats.tMaxMs);
        const QString fmt = QStringLiteral("dd.MM.yyyy HH:mm:ss");
        html += QStringLiteral("<div style=\"color:%1;margin-top:4px;\">%2 – %3</div>")
                    .arg(muted.name(), from.toString(fmt),
                         to.toString(to.date() == from.date()
                                         ? QStringLiteral("HH:mm:ss") : fmt));
    }

    // ---- Уровни -------------------------------------------------------------
    {
        struct LevelRow {
            LogLevel level;
            qint64 count;
        };
        QVector<LevelRow> rows;
        qint64 maxCount = 0;
        const LogLevel order[] = {LogLevel::Fatal, LogLevel::Error, LogLevel::Warn,
                                  LogLevel::Info, LogLevel::Debug, LogLevel::Trace,
                                  LogLevel::Unknown};
        for (LogLevel lvl : order) {
            const qint64 c = m_stats.levelCounts[int(lvl)];
            if (c > 0) {
                rows.append({lvl, c});
                maxCount = qMax(maxCount, c);
            }
        }
        if (!rows.isEmpty()) {
            html += sectionHeader(tr("Levels"));
            html += QLatin1String("<table cellspacing=\"0\" cellpadding=\"2\">");
            for (const LevelRow& r : rows) {
                const QColor color = r.level == LogLevel::Unknown
                                         ? muted : t.forLevel(r.level);
                const QString name = r.level == LogLevel::Unknown
                                         ? tr("NO LEVEL") : LevelToStr(r.level);
                const double share = m_stats.records > 0
                                         ? 100.0 * r.count / m_stats.records : 0.0;
                const int barLen =
                    qMax(1, int(std::lround(22.0 * double(r.count) / double(maxCount))));
                html += QStringLiteral(
                            "<tr><td>%1&nbsp;&nbsp;</td>"
                            "<td align=\"right\">%2&nbsp;&nbsp;</td>"
                            "<td align=\"right\" style=\"color:%3;\">%4%&nbsp;&nbsp;</td>"
                            "<td>%5</td></tr>")
                            .arg(colorSpan(color, name, /*bold=*/true), num(r.count),
                                 muted.name(), locale.toString(share, 'f', 1),
                                 colorSpan(color, QString(barLen, QChar(0x2588))));
            }
            html += QLatin1String("</table>");
        }
    }

    // ---- Insights -----------------------------------------------------------
    html += sectionHeader(tr("Insights"));
    if (m_stats.insights.isEmpty()) {
        html += QStringLiteral("<div style=\"color:%1;\">%2</div>")
                    .arg(muted.name(), tr("No anomalies detected."));
    } else {
        for (const DocumentStats::Insight& in : m_stats.insights) {
            QColor accent = t.logWarn;
            QString glyph = QStringLiteral("▲");
            QString body;
            const QString interval =
                in.fromMs >= 0 && in.toMs >= in.fromMs
                    ? shortRangeText(in.fromMs, in.toMs) : QString();
            switch (in.kind) {
            case DocumentStats::Insight::ErrorSpike:
                accent = t.logError;
                glyph = QStringLiteral("⚠");
                body = tr("Error spike: %1 errors within %2 (%3) — %4 the document average.")
                           .arg(num(in.count), durationText(in.toMs - in.fromMs),
                                navLink(timeTarget(in.fromMs, true), interval),
                                factorText(in.factor));
                break;
            case DocumentStats::Insight::Burst:
                accent = t.logWarn;
                glyph = QStringLiteral("▲");
                body = tr("Activity burst: %1 records within %2 (%3) — %4 the typical rate.")
                           .arg(num(in.count), durationText(in.toMs - in.fromMs),
                                navLink(timeTarget(in.fromMs, false), interval),
                                factorText(in.factor));
                break;
            case DocumentStats::Insight::Spam: {
                accent = t.logWarn;
                glyph = QStringLiteral("≡");
                QString snippet = in.snippet.left(90);
                if (in.snippet.size() > 90)
                    snippet += QStringLiteral("…");
                QString what = QStringLiteral("“") + snippet.toHtmlEscaped()
                             + QStringLiteral("”");
                if (in.file)
                    what = navLink(entryTarget(in.entryId, in.file), what);
                body = tr("Message spam: %1 repeated %2 times (%3% of all records).")
                           .arg(what, num(in.count),
                                locale.toString(in.share * 100.0, 'f', 1));
                break;
            }
            case DocumentStats::Insight::Gap:
                accent = t.logInfo;
                glyph = QStringLiteral("…");
                body = tr("Silence: no records for %1 (%2).")
                           .arg(durationText(in.toMs - in.fromMs),
                                navLink(timeTarget(in.toMs, false), interval));
                break;
            case DocumentStats::Insight::RisingErrors:
                accent = t.logError;
                glyph = QStringLiteral("↗");
                body = tr("Error rate is rising: %1 more errors in the second half of the time span.")
                           .arg(factorText(in.factor));
                break;
            case DocumentStats::Insight::RisingVolume:
                accent = t.logWarn;
                glyph = QStringLiteral("↗");
                body = tr("Log volume is rising: %1 more records in the second half of the time span.")
                           .arg(factorText(in.factor));
                break;
            }
            const QColor cardBg = mixColors(base, accent, 0.10);
            html += QStringLiteral(
                        "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"5\" "
                        "bgcolor=\"%1\" style=\"margin-top:3px;\"><tr>"
                        "<td width=\"16\" style=\"color:%2;font-weight:bold;\">%3</td>"
                        "<td>%4</td></tr></table>")
                        .arg(cardBg.name(), accent.name(), glyph, body);
        }
    }

    // ---- Топы сообщений -----------------------------------------------------
    // Числовым колонкам — запрет переноса и выравнивание по верху; колонке
    // сообщения ширину НЕ задаём: width=100% отжимал числа до нулевой ширины,
    // и «×2 114» рассыпался в столбик по одной цифре.
    const auto templateTable = [&](const QVector<DocumentStats::Template>& list) {
        QString out = QLatin1String("<table cellspacing=\"0\" cellpadding=\"2\">");
        // Подпись колонок: что значат числа слева.
        out += QStringLiteral(
                   "<tr><td align=\"right\" style=\"color:%1;font-size:8pt;"
                   "white-space:nowrap;\">%2&nbsp;&nbsp;</td>"
                   "<td align=\"right\" style=\"color:%1;font-size:8pt;\">%3&nbsp;&nbsp;</td>"
                   "<td></td><td style=\"color:%1;font-size:8pt;\">%4</td></tr>")
                   .arg(muted.name(), tr("count"), tr("share"), tr("message"));
        for (const DocumentStats::Template& tp : list) {
            const double share = m_stats.records > 0
                                     ? 100.0 * tp.count / m_stats.records : 0.0;
            const QColor dot = tp.level == LogLevel::Unknown ? muted
                                                             : t.forLevel(tp.level);
            QString display = tp.text.mid(displayStart(tp.text));
            if (display.size() > 140)
                display = display.left(140) + QStringLiteral("…");
            const QString link =
                tp.firstFile
                    ? navLink(entryTarget(tp.firstEntryId, tp.firstFile),
                              display.toHtmlEscaped())
                    : display.toHtmlEscaped();
            out += QStringLiteral(
                       "<tr><td align=\"right\" valign=\"top\" "
                       "style=\"font-weight:bold;white-space:nowrap;\">%1&nbsp;&nbsp;</td>"
                       "<td align=\"right\" valign=\"top\" "
                       "style=\"color:%2;white-space:nowrap;\">%3%&nbsp;&nbsp;</td>"
                       "<td valign=\"top\" style=\"white-space:nowrap;\">%4&nbsp;</td>"
                       "<td valign=\"top\" style=\"font-family:'%5';\">%6</td></tr>")
                       .arg(QStringLiteral("×") + num(tp.count), muted.name(),
                            locale.toString(share, 'f', 1),
                            colorSpan(dot, QStringLiteral("●")), monoFamily, link);
        }
        out += QLatin1String("</table>");
        return out;
    };

    if (!m_stats.topMessages.isEmpty()) {
        html += sectionHeader(tr("Top messages"));
        html += templateTable(m_stats.topMessages);
        if (m_stats.templatesCapped)
            html += QStringLiteral("<div style=\"color:%1;font-size:8pt;\">%2</div>")
                        .arg(muted.name(),
                             tr("Too many distinct messages — counts are approximate."));
    }

    if (!m_stats.topErrors.isEmpty()) {
        html += sectionHeader(tr("Top errors"));
        html += templateTable(m_stats.topErrors);
    }

    // ---- Файлы --------------------------------------------------------------
    if (m_stats.files.size() > 1) {
        html += sectionHeader(tr("Files"));
        html += QLatin1String("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"2\">");
        const int shown = qMin(int(m_stats.files.size()), kMaxFiles);
        for (int i = 0; i < shown; ++i) {
            const DocumentStats::FileStat& fs = m_stats.files[i];
            const QString name = fs.file ? fs.file->shortName() : tr("(unknown)");
            QStringList badges;
            if (fs.warn > 0)
                badges << colorSpan(t.treeBadgeWarn, QStringLiteral("W ") + num(fs.warn));
            if (fs.error > 0)
                badges << colorSpan(t.treeBadgeError, QStringLiteral("E ") + num(fs.error));
            if (fs.fatal > 0)
                badges << colorSpan(t.treeBadgeFatal, QStringLiteral("F ") + num(fs.fatal));
            const QString range = fs.firstMs >= 0
                                      ? shortRangeText(fs.firstMs, fs.lastMs) : QString();
            html += QStringLiteral(
                        "<tr><td valign=\"top\">%1&nbsp;</td>"
                        "<td align=\"right\" valign=\"top\" "
                        "style=\"white-space:nowrap;\">%2&nbsp;</td>"
                        "<td valign=\"top\" style=\"white-space:nowrap;\">%3&nbsp;</td>"
                        "<td valign=\"top\" style=\"color:%4;white-space:nowrap;\">%5</td></tr>")
                        .arg(name.toHtmlEscaped(), num(fs.records),
                             badges.join(QStringLiteral("&nbsp;")),
                             muted.name(), range);
        }
        if (m_stats.files.size() > shown)
            html += QStringLiteral("<tr><td colspan=\"4\" style=\"color:%1;\">%2</td></tr>")
                        .arg(muted.name(),
                             tr("… and %1 more file(s)").arg(m_stats.files.size() - shown));
        html += QLatin1String("</table>");
    }

    return html;
}
