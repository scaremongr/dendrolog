#include "fieldextractor.h"


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
