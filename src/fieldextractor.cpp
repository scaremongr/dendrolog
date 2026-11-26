#include "fieldextractor.h"

#include "logpattern.h"

namespace {

void clearFields(LogEntryFields* out, int columns)
{
    if (!out)
        return;
    if (out->spans.size() != columns)
        out->spans.resize(columns);
    for (FieldSpan& span : out->spans)
        span = FieldSpan();
}

} // namespace

// ============================================================================
// KeyedExtractor
// ============================================================================

KeyedExtractor::KeyedExtractor(const KeyedDefinition& definition)
    : m_definition(definition)
    , m_lookup(definition)
    , m_columns(definition.columnNames())
{
}

FieldExtractor::Outcome KeyedExtractor::extract(QStringView line, LogEntryFields* out, int upTo,
                                                Stop* stop) const
{
    if (stop)
        *stop = Stop();
    const int columns = int(m_columns.size());
    clearFields(out, columns);
    KeyedLine::Hits hits;
    const int needed = upTo >= 0 ? upTo + 1 : -1;
    if (!m_lookup.scan(line, &hits, needed)) {
        if (stop)
            stop->offset = 0;
        return Outcome::Rejected;
    }
    if (out) {
        for (int c = 0; c < hits.size() && c < columns; ++c) {
            if (hits[c].isValid())
                out->spans[c].set(hits[c].start, hits[c].length, hits[c].escaped);
        }
    }
    return Outcome::Full;
}

MatchReport KeyedExtractor::explain(QStringView line) const
{
    MatchReport report;
    KeyedLine::Hits hits;
    if (!m_lookup.scan(line, &hits)) {
        report.expect = MatchReport::Expect::Record;
        report.found = line.left(24).toString();
    }
    return report;
}

// ============================================================================
// LegacyPatternExtractor
// ============================================================================

LegacyPatternExtractor::LegacyPatternExtractor(std::shared_ptr<const LogPattern> pattern)
    : m_pattern(pattern ? std::move(pattern) : std::make_shared<const LogPattern>())
    , m_fields(m_pattern->fieldNames())
{
    const PatternDefinition& def = m_pattern->definition();
    if (!def.keyed) {
        for (const PatternBlock& b : def.blocks) {
            if (b.matchKind == PatternBlock::MatchKind::CustomRegex
                || (b.separatorIsRegex && !b.separator.isEmpty()))
                m_cost = Cost::Background;
        }
    }
}

LegacyPatternExtractor::LegacyPatternExtractor(const LogPattern& pattern)
    : LegacyPatternExtractor(std::make_shared<const LogPattern>(pattern))
{
}

bool LegacyPatternExtractor::isValid() const
{
    return m_pattern->isValid();
}

int LegacyPatternExtractor::tailField() const
{
    return m_pattern->tailFieldIndex();
}

bool LegacyPatternExtractor::isKeyed() const
{
    return m_pattern->isKeyed();
}

FieldExtractor::Outcome LegacyPatternExtractor::extract(QStringView line, LogEntryFields* out,
                                                        int upTo, Stop* stop) const
{
    Q_UNUSED(upTo);
    return extractWithMatch(line, out, nullptr, stop);
}

FieldExtractor::Outcome LegacyPatternExtractor::extractWithMatch(QStringView line, LogEntryFields* out,
                                                                 LineMatchResult* match, Stop* stop) const
{
    if (stop)
        *stop = Stop();
    const int columns = int(m_fields.size());
    if (!m_pattern->isValid()) {
        clearFields(out, columns);
        if (match)
            *match = LineMatchResult();
        return Outcome::Rejected;
    }
    // Без копии: регексу нужен QString, строка живёт дольше разбора.
    const QString text = QString::fromRawData(line.data(), line.size());
    const LineMatchResult m = m_pattern->matchLine(text);
    if (match)
        *match = m;
    if (!m.ok) {
        clearFields(out, columns);
        if (stop)
            stop->offset = 0;
        return Outcome::Rejected;
    }
    if (m_pattern->isKeyed()) {
        if (out) {
            *out = m_pattern->extractFields(text);   // спаны с битом экранирования
            if (out->spans.size() != columns)
                out->spans.resize(columns);
        }
        return Outcome::Full;
    }
    if (out) {
        *out = m_pattern->fieldsFromMatch(text, m);
        if (out->spans.size() != columns)
            out->spans.resize(columns);
    }
    const bool partial = m.unparsedStart >= 0 || m.skippedFrom >= 0;
    if (partial && stop) {
        if (m.skippedFrom >= 0) {
            stop->afterElement = m.skippedFrom - 1;
            stop->offset = qMax(0, m.holeStart);
        } else {
            stop->afterElement = m.matchedBlockCount - 1;
            stop->offset = qMax(0, m.unparsedStart);
        }
    }
    return partial ? Outcome::Partial : Outcome::Full;
}

MatchReport LegacyPatternExtractor::explain(QStringView line) const
{
    const QString text = QString::fromRawData(line.data(), line.size());
    return explainMatch(m_pattern->definition(), text, m_pattern->matchLine(text));
}

MatchReport LegacyPatternExtractor::explainMatch(const PatternDefinition& definition, const QString& line,
                                                 const LineMatchResult& match)
{
    MatchReport reason;
    if (definition.keyed) {
        if (!match.ok) {
            reason.expect = MatchReport::Expect::Record;
            reason.found = line.left(24);
        }
        return reason;
    }
    const bool partial = match.ok && (match.unparsedStart >= 0 || match.skippedFrom >= 0);
    reason.partial = partial;
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
        reason.expect = MatchReport::Expect::Literal;
        reason.literal = definition.linePrefix;
        return reason;
    }
    if (reason.afterBlock >= 0 && reason.afterBlock < blocks.size()) {
        const PatternBlock& prev = blocks[reason.afterBlock];
        if (!prev.separator.isEmpty() && !prev.separatorIsRegex
                && !QStringView(line).mid(reason.stopOffset).trimmed().startsWith(prev.separator.trimmed())) {
            reason.expect = MatchReport::Expect::Literal;
            reason.literal = prev.separator;
            return reason;
        }
    }
    if (reason.expectedBlock < 0 || reason.expectedBlock >= blocks.size()) {
        reason.expectedBlock = -1;
        reason.expect = MatchReport::Expect::Text;
        return reason;
    }
    const PatternBlock& next = blocks[reason.expectedBlock];
    reason.field = next.name;
    if (!next.leadingText.isEmpty()) {
        reason.expect = MatchReport::Expect::Literal;
        reason.literal = next.leadingText;
        return reason;
    }
    using K = PatternBlock::MatchKind;
    using X = MatchReport::Expect;
    switch (next.matchKind) {
    case K::ConstantText:
        reason.expect = X::Literal;
        reason.literal = next.customRegex;
        break;
    case K::Timestamp: reason.expect = X::Timestamp; break;
    case K::Level: reason.expect = X::Level; break;
    case K::Integer: reason.expect = X::Number; break;
    case K::HexText: reason.expect = X::Hex; break;
    case K::IpAddress: reason.expect = X::Ip; break;
    case K::FilePath: reason.expect = X::Path; break;
    case K::CustomRegex: reason.expect = X::Regex; break;
    case K::TextUntilSeparator:
    case K::GreedyTextUntilSeparator:
        // Текст до разделителя: разделителя в остатке строки нет — его и
        // не хватило.
        if (!next.separator.isEmpty() && !next.separatorIsRegex
                && !QStringView(line).mid(reason.stopOffset).contains(next.separator)) {
            reason.expect = X::Literal;
            reason.literal = next.separator;
            break;
        }
        reason.expect = X::Text;
        break;
    case K::Remainder:
        reason.expect = X::Text;
        break;
    }
    return reason;
}
