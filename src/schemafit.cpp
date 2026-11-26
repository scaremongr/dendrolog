#include "schemafit.h"

#include "lineclassifier.h"
#include "logmodel.h"
#include "logscan.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 kSparseWindowBytes = 64 * 1024;
}

namespace SchemaFit {

namespace {

bool hasLeadingIndent(const QString& line)
{
    return !line.isEmpty() && (line.at(0) == QLatin1Char(' ') || line.at(0) == QLatin1Char('\t'));
}

bool isBlank(const QString& line)
{
    for (const QChar c : line) {
        if (!c.isSpace())
            return false;
    }
    return true;
}

// Правило кандидата без схемы — одно на corpus и startsRecord.
struct CandidateRule {
    LineClassifier classifier;
    ClockDates clock;   // время без даты (F-11): контекст есть, предпросмотра нет

    bool operator()(const QString& line)
    {
        qint64 ms = kNoTimestampMs;
        bool leading = false;
        const bool hasTs = classifier.detectTimestampMs(line, ms, LineClassifier::YearReference(),
                                                        &leading, &clock);
        LogLevel level = LogLevel::Unknown;
        const bool hasLevel = classifier.detectLogLevel(line, level);
        return LineClassifier::isPrimaryLine(false, hasTs, leading, hasLevel);
    }
};

QString formatPercentImpl(double share)
{
    const double pct = share * 100.0;
    // 99,96 % не показываем как 100 %, а 100 % — без «.0».
    if (pct >= 100.0)
        return QStringLiteral("100");
    // Десятые вниз целым числом: 0,29·1000 в double — 289,999…
    const qint64 tenths = qint64(std::floor(pct * 10.0 + 1e-6));
    QString text = QString::number(tenths / 10) + QLatin1Char('.') + QString::number(tenths % 10);
    if (text.endsWith(QLatin1String(".0")))
        text.chop(2);
    return text;
}

} // namespace

bool startsRecord(const QString& line)
{
    if (isBlank(line))
        return false;
    CandidateRule rule;
    return rule(line);
}

Corpus corpus(const QVector<Sample>& samples)
{
    Corpus c;
    c.samples = samples;
    const int n = int(samples.size());
    c.candidate.fill(false, n);
    c.nonEmpty.fill(false, n);
    CandidateRule rule;
    for (int i = 0; i < n; ++i) {
        const QString& text = samples[i].text;
        if (isBlank(text))
            continue;
        c.nonEmpty[i] = true;
        ++c.nonEmptyCount;
        if (rule(text)) {
            c.candidate[i] = true;
            ++c.candidateCount;
        }
    }
    if (c.nonEmptyCount > 0
            && c.candidateCount < kCandidateFallbackShare * c.nonEmptyCount) {
        // Лог без меток (WARNING:root:…, JSON с эпохой числом): начало
        // записи — строка без отступа. Если строк без отступа со словом
        // уровня не меньше той же доли — только они: иначе «Traceback (most
        // recent call last):» и «TimeoutError: …» (без отступа) считались
        // бы промахами любой схемы такого лога.
        c.fallbackRule = true;
        QVector<bool> withLevel(n, false);
        int levelLines = 0;
        for (int i = 0; i < n; ++i) {
            if (!c.nonEmpty[i] || hasLeadingIndent(samples[i].text))
                continue;
            LogLevel level = LogLevel::Unknown;
            if (rule.classifier.detectLogLevel(samples[i].text, level)) {
                withLevel[i] = true;
                ++levelLines;
            }
        }
        const bool byLevel = levelLines >= kCandidateFallbackShare * c.nonEmptyCount;
        if (!byLevel && c.candidateCount > 0) {
            // Записи с метками есть, но их мало: строки трасс между ними
            // (java.lang.X: …, Caused by: …) — продолжения, а не промахи.
            // Остаются кандидаты по основному правилу.
            c.fallbackRule = false;
            return c;
        }
        c.candidateCount = 0;
        for (int i = 0; i < n; ++i) {
            c.candidate[i] = byLevel ? withLevel[i]
                                     : c.nonEmpty[i] && !hasLeadingIndent(samples[i].text);
            if (c.candidate[i])
                ++c.candidateCount;
        }
    }
    return c;
}

Corpus corpus(const QStringList& lines)
{
    QVector<Sample> samples;
    samples.reserve(lines.size());
    for (const QString& line : lines)
        samples.append(Sample{0, line});
    return corpus(samples);
}

double Result::coverage() const
{
    return candidates > 0 ? (full + 0.5 * partial) / double(candidates) : 0.0;
}

double Result::specificity() const
{
    const int acceptedCandidates = full + partial;
    return acceptedCandidates > 0 ? double(fieldsOnCandidates) / acceptedCandidates : 0.0;
}

Result evaluate(const LogPattern& schema, const Corpus& lines, const Budget& budget,
                QVector<LineMatchResult>* matches)
{
    Result r;
    if (matches) {
        matches->clear();
        matches->resize(lines.samples.size());
    }
    r.keyed = schema.isKeyed();
    r.candidates = lines.candidateCount;
    if (!schema.isValid())
        return r;

    const int columns = int(schema.fieldNames().size());
    const int tail = schema.tailFieldIndex();
    r.filled.fill(0, columns);
    QVector<bool> colFilled(columns, false);

    QElapsedTimer timer;
    timer.start();
    const auto now = [&]() -> qint64 { return budget.clock ? budget.clock() : timer.elapsed(); };
    const qint64 started = budget.clock ? budget.clock() : 0;
    const auto overBudget = [&]() {
        if (budget.cancel && budget.cancel->load(std::memory_order_relaxed)) {
            r.cancelled = true;
            return true;
        }
        if (budget.maxMs > 0 && now() - started > budget.maxMs) {
            r.timedOut = true;
            return true;
        }
        return false;
    };
    const int checkEvery = qMax(1, budget.checkEvery);

    const int n = int(lines.samples.size());
    for (int i = 0; i < n; ++i) {
        if (i > 0 && i % checkEvery == 0 && overBudget())
            return r;
        if (!lines.nonEmpty.value(i))
            continue;
        ++r.examined;
        const bool candidate = lines.candidate.value(i);
        const QString& text = lines.samples[i].text;
        const LineMatchResult match = schema.matchLine(text);
        if (matches)
            (*matches)[i] = match;

        const auto addMiss = [&](bool partial) {
            if (r.misses.size() >= kMaxMisses)
                return;
            Miss miss;
            miss.line = lines.samples[i].line;
            miss.text = text;
            miss.partial = partial;
            if (match.ok && match.skippedFrom >= 0) {
                miss.afterBlock = match.skippedFrom - 1;
                miss.stopOffset = qMax(0, match.holeStart);
            } else if (match.ok) {
                miss.afterBlock = match.matchedBlockCount - 1;
                miss.stopOffset = qMax(0, match.unparsedStart);
            }
            r.misses.append(miss);
        };

        if (!match.ok) {
            if (candidate) {
                ++r.missed;
                addMiss(false);
            }
            continue;
        }

        // Непустые колонки: у ключевой — найденные значения по колонке,
        // у регексной — поля, как их получит парсер (extractFields).
        std::fill(colFilled.begin(), colFilled.end(), false);
        if (r.keyed) {
            for (const BlockSpan& span : match.spans) {
                if (span.blockIndex >= 0 && span.blockIndex < columns && span.length > 0)
                    colFilled[span.blockIndex] = true;
            }
        } else {
            const LogEntryFields fields = schema.fieldsFromMatch(text, match);
            for (int c = 0; c < columns && c < fields.size(); ++c)
                colFilled[c] = fields.spans[c].isValid() && fields.spans[c].size() > 0;
        }
        int nonTail = 0;
        for (int c = 0; c < columns; ++c) {
            if (!colFilled[c])
                continue;
            ++r.filled[c];
            if (c != tail)
                ++nonTail;
        }
        if (r.keyed && ((columns > 0 && colFilled[0]) || (columns > 1 && colFilled[1])))
            ++r.keyedRoles;

        if (!candidate) {
            ++r.loose;
            continue;
        }
        const bool partial = !r.keyed && (match.unparsedStart >= 0 || match.skippedFrom >= 0);
        if (partial) {
            ++r.partial;
            addMiss(true);
        } else {
            ++r.full;
        }
        r.fieldsOnCandidates += nonTail;
    }
    // Проход дошёл до конца — итог годен, сколько бы он ни шёл: бюджет
    // только останавливает проход. Отмена после конца — тоже отмена.
    if (budget.cancel && budget.cancel->load(std::memory_order_relaxed)) {
        r.cancelled = true;
        return r;
    }
    r.valid = true;
    return r;
}

Result evaluate(const LogPattern& schema, const QStringList& lines, const Budget& budget)
{
    return evaluate(schema, corpus(lines), budget);
}

Result evaluate(const LogPattern& schema, const QVector<Sample>& lines, const Budget& budget)
{
    return evaluate(schema, corpus(lines), budget);
}

Verdict verdict(const Result& r)
{
    if (!r.valid || r.timedOut || r.cancelled)
        return Verdict::NoFit;
    const double cov = r.coverage();
    const int acceptedCandidates = r.full + r.partial;
    if (r.keyed) {
        const int accepted = r.accepted();
        const bool roles = accepted > 0 && r.keyedRoles >= kKeyedRoleShare * accepted;
        if (cov >= kFitCoverage && roles)
            return Verdict::Fits;
        if (cov >= kDoubtfulCoverage && r.keyedRoles > 0 && r.full >= r.partial)
            return Verdict::Doubtful;
        return Verdict::NoFit;
    }
    if (acceptedCandidates == 0)
        return Verdict::NoFit;
    const double spec = r.specificity();
    if (cov >= kFitCoverage && spec >= kFitSpecificity)
        return Verdict::Fits;
    // Схема, которая на большинстве записей лишь деградирует (метка — якорь,
    // найденный где угодно, остальное — в Message), формат не описывает:
    // покрытие 0,5 из одних частичных — «не подходит».
    if (cov >= kDoubtfulCoverage && spec >= kMinSpecificity && r.full >= r.partial)
        return Verdict::Doubtful;
    return Verdict::NoFit;
}

bool better(const Result& a, const Result& b)
{
    const auto usable = [](const Result& r) { return r.valid && !r.timedOut && !r.cancelled; };
    if (usable(a) != usable(b))
        return usable(a);
    if (!usable(a))
        return false;
    const auto strong = [](const Result& r) {
        return r.specificity() >= kMinSpecificity && r.coverage() > 0.0;
    };
    if (strong(a) != strong(b))
        return strong(a);
    const qint64 ca = qRound64(a.coverage() / kCoverageStep);
    const qint64 cb = qRound64(b.coverage() / kCoverageStep);
    if (ca != cb)
        return ca > cb;
    const double sa = a.specificity();
    const double sb = b.specificity();
    if (std::abs(sa - sb) > 1e-9)
        return sa > sb;
    return a.partial < b.partial;
}

QVector<Ranked> rank(const QVector<LogPattern>& schemas, const Corpus& lines, const Budget& budget)
{
    QVector<Ranked> ranked;
    ranked.reserve(schemas.size());
    for (int i = 0; i < schemas.size(); ++i) {
        if (budget.cancel && budget.cancel->load(std::memory_order_relaxed))
            break;
        ranked.append(Ranked{i, evaluate(schemas[i], lines, budget)});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const Ranked& a, const Ranked& b) { return better(a.result, b.result); });
    return ranked;
}

Reason explain(const PatternDefinition& definition, const QString& line, const LineMatchResult& match)
{
    Reason reason;
    if (definition.keyed) {
        if (!match.ok) {
            reason.expect = Reason::Expect::Record;
            reason.found = line.left(24);
        }
        return reason;
    }
    const bool partial = match.ok && (match.unparsedStart >= 0 || match.skippedFrom >= 0);
    if (match.ok && !partial)
        return reason;

    if (match.ok && match.skippedFrom >= 0) {
        reason.afterBlock = match.skippedFrom - 1;
        reason.stopOffset = qMax(0, match.holeStart);
    } else if (match.ok) {
        reason.afterBlock = match.matchedBlockCount - 1;
        reason.stopOffset = qMax(0, match.unparsedStart);
    } else {
        reason.afterBlock = -1;
        reason.stopOffset = 0;
    }
    reason.expectedBlock = reason.afterBlock + 1;
    reason.found = line.mid(reason.stopOffset, 24);

    const QVector<PatternBlock>& blocks = definition.blocks;
    if (reason.afterBlock < 0 && !definition.linePrefix.isEmpty()
            && !line.startsWith(definition.linePrefix)) {
        reason.expect = Reason::Expect::Literal;
        reason.literal = definition.linePrefix;
        return reason;
    }
    if (reason.afterBlock >= 0 && reason.afterBlock < blocks.size()) {
        const PatternBlock& prev = blocks[reason.afterBlock];
        if (!prev.separator.isEmpty() && !prev.separatorIsRegex
                && !QStringView(line).mid(reason.stopOffset).trimmed().startsWith(prev.separator.trimmed())) {
            reason.expect = Reason::Expect::Literal;
            reason.literal = prev.separator;
            return reason;
        }
    }
    if (reason.expectedBlock < 0 || reason.expectedBlock >= blocks.size()) {
        reason.expectedBlock = -1;
        reason.expect = Reason::Expect::Text;
        return reason;
    }
    const PatternBlock& next = blocks[reason.expectedBlock];
    if (!next.leadingText.isEmpty()) {
        reason.expect = Reason::Expect::Literal;
        reason.literal = next.leadingText;
        return reason;
    }
    using K = PatternBlock::MatchKind;
    switch (next.matchKind) {
    case K::ConstantText:
        reason.expect = Reason::Expect::Literal;
        reason.literal = next.customRegex;
        break;
    case K::Timestamp: reason.expect = Reason::Expect::Timestamp; break;
    case K::Level: reason.expect = Reason::Expect::Level; break;
    case K::Integer: reason.expect = Reason::Expect::Number; break;
    case K::HexText: reason.expect = Reason::Expect::Hex; break;
    case K::IpAddress: reason.expect = Reason::Expect::Ip; break;
    case K::FilePath: reason.expect = Reason::Expect::Path; break;
    case K::CustomRegex: reason.expect = Reason::Expect::Regex; break;
    case K::TextUntilSeparator:
    case K::GreedyTextUntilSeparator:
        // Текст до разделителя: разделителя в остатке строки нет — его и
        // не хватило.
        if (!next.separator.isEmpty() && !next.separatorIsRegex
                && !QStringView(line).mid(reason.stopOffset).contains(next.separator)) {
            reason.expect = Reason::Expect::Literal;
            reason.literal = next.separator;
            break;
        }
        reason.expect = Reason::Expect::Text;
        break;
    case K::Remainder:
        reason.expect = Reason::Expect::Text;
        break;
    }
    return reason;
}

QString percent(double share)
{
    return formatPercentImpl(share);
}

QString summary(const Result& r)
{
    if (r.timedOut)
        return QCoreApplication::translate("SchemaFit", "The schema is too slow on these lines");
    if (!r.valid)
        return QString();
    if (r.candidates == 0)
        return QCoreApplication::translate("SchemaFit", "No record lines to check");
    QString text = QCoreApplication::translate("SchemaFit", "Parsed %1% of %2 record lines")
                       .arg(percent(r.coverage()))
                       .arg(r.candidates);
    if (r.missed > 0)
        text += QCoreApplication::translate("SchemaFit", " · %1 did not match").arg(r.missed);
    if (r.partial > 0)
        text += QCoreApplication::translate("SchemaFit", " · %1 partly").arg(r.partial);
    return text;
}

} // namespace SchemaFit

// ---------------------------------------------------------------------------
// SchemaFitJob
// ---------------------------------------------------------------------------

SchemaFitJob::SchemaFitJob(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFutureWatcher<Outcome>(this))
{
    connect(m_watcher, &QFutureWatcherBase::finished, this, &SchemaFitJob::onWatcherFinished);
}

SchemaFitJob::~SchemaFitJob()
{
    cancel(/*wait=*/true);
}

void SchemaFitJob::setModel(LogModel* model)
{
    if (m_model == model)
        return;
    if (m_resetConnection)
        disconnect(m_resetConnection);
    if (m_destroyedConnection)
        disconnect(m_destroyedConnection);
    cancel();
    m_model = model;
    m_hasOutcome = false;
    if (model) {
        m_destroyedConnection = connect(model, &QObject::destroyed, this, [this]() {
            cancel();
            m_model = nullptr;
            m_hasOutcome = false;
        });
        m_resetConnection = connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            // Строки снапшота могли уйти (перезагрузка, смена кодировки,
            // конверсия бэкенда): расчёт и прежний итог — не про эти строки.
            cancel();
            m_hasOutcome = false;
            emit invalidated();
        });
    }
}

int SchemaFitJob::start(const QVector<LogPattern>& schemas, const LogFile* file,
                        const SchemaFit::Budget& budget)
{
    cancel();
    const int generation = m_generation;
    auto flag = std::make_shared<std::atomic_bool>(false);
    m_cancel = flag;
    const LogScanSnapshot snapshot = m_model ? m_model->scanSnapshot(/*filteredOnly=*/false)
                                             : LogScanSnapshot();
    SchemaFit::Budget workerBudget = budget;
    workerBudget.cancel = flag.get();
    m_watcher->setFuture(QtConcurrent::run([snapshot, schemas, file, workerBudget, flag, generation]() {
        Outcome outcome;
        outcome.generation = generation;
        const QVector<SchemaFit::Sample> samples = sampleLines(snapshot, file, flag.get());
        outcome.sampled = int(samples.size());
        const SchemaFit::Corpus lines = SchemaFit::corpus(samples);
        outcome.results.reserve(schemas.size());
        for (const LogPattern& schema : schemas) {
            if (flag->load(std::memory_order_relaxed))
                break;
            outcome.results.append(SchemaFit::evaluate(schema, lines, workerBudget));
        }
        outcome.cancelled = flag->load(std::memory_order_relaxed);
        return outcome;
    }));
    return generation;
}

void SchemaFitJob::cancel(bool wait)
{
    // Итог, посчитанный до отмены, тоже не доставляется: поколение — новое.
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    if (wait)
        m_watcher->waitForFinished();
}

bool SchemaFitJob::isRunning() const
{
    return m_watcher->isRunning();
}

void SchemaFitJob::onWatcherFinished()
{
    const QFuture<Outcome> future = m_watcher->future();
    if (future.resultCount() == 0)
        return;
    Outcome outcome = future.result();
    if (outcome.generation != m_generation || outcome.cancelled)
        return;
    m_outcome = std::move(outcome);
    m_hasOutcome = true;
    emit finished();
}

QVector<SchemaFit::Sample> SchemaFitJob::sampleLines(const LogScanSnapshot& snapshot,
                                                     const LogFile* file,
                                                     const std::atomic_bool* cancel,
                                                     int headLines, int spreadLines)
{
    QVector<SchemaFit::Sample> samples;
    const qint64 n = snapshot.rowCount();
    if (n <= 0)
        return samples;
    const auto cancelled = [cancel]() {
        return cancel && cancel->load(std::memory_order_relaxed);
    };
    const LogFile* target = file;
    if (!target)
        snapshot.lineAt(0, &target);
    const auto ofTarget = [&](qint64 row) {
        const LogFile* f = nullptr;
        snapshot.lineAt(row, &f);
        return f == target;
    };

    QVector<qint64> rows;
    rows.reserve(headLines + spreadLines);
    // Начало файла. В слитой вкладке строки другого файла пропускаются,
    // но просмотр ограничен: файл в меньшинстве не стоит прохода по всем.
    qint64 row = 0;
    const qint64 headScanEnd = qMin<qint64>(n, qint64(headLines) * 16);
    for (; row < headScanEnd && rows.size() < headLines; ++row) {
        if ((row & 255) == 0 && cancelled())
            return {};
        if (ofTarget(row))
            rows.append(row);
    }
    const int headCount = int(rows.size());
    // Равномерно по остальному: по строке файла из каждого из spreadLines
    // окон (не дальше 64 строк от начала окна).
    const qint64 rest = n - row;
    if (rest > 0 && spreadLines > 0) {
        const qint64 count = qMin<qint64>(spreadLines, rest);
        for (qint64 k = 0; k < count; ++k) {
            if ((k & 63) == 0 && cancelled())
                return {};
            const qint64 from = row + (rest * k) / count;
            const qint64 to = qMin(row + (rest * (k + 1)) / count, from + 64);
            for (qint64 r = from; r < to; ++r) {
                if (ofTarget(r)) {
                    rows.append(r);
                    break;
                }
            }
        }
    }

    // Начало — подряд, обычным окном; разреженные строки — малым окном
    // (64 КБ): окно 8 МБ на каждую из 2000 строк — гигабайты чтения на
    // большом файле.
    const std::unique_ptr<LogScanSnapshot::LineCursor> headCursor = snapshot.lineCursor();
    const std::unique_ptr<LogScanSnapshot::LineCursor> sparseCursor =
        snapshot.lineCursor(kSparseWindowBytes);
    if (!headCursor || !sparseCursor)
        return samples;
    samples.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        if (cancelled())
            return {};
        const qint64 r = rows[i];
        LogScanSnapshot::LineCursor* cursor = i < headCount ? headCursor.get() : sparseCursor.get();
        const LogFile* f = nullptr;
        SchemaFit::Sample s;
        s.line = snapshot.lineAt(r, &f);
        s.text = cursor->lineAt(r).toString();
        samples.append(s);
    }
    return samples;
}
