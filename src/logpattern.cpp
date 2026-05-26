#include "logpattern.h"

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LogPattern::LogPattern(const QString& conversionPattern)
{
    setPattern(conversionPattern);
}

bool LogPattern::setPattern(const QString& conversionPattern)
{
    m_patternString = conversionPattern;
    m_valid         = false;
    m_segments.clear();

    if (conversionPattern.isEmpty())
        return false;

    parsePattern(conversionPattern);
    return m_valid;
}

// ---------------------------------------------------------------------------
// parsePattern
//
// Converts the ConversionPattern string into a flat list of PatternSegment
// items (alternating Literal / Field).  Alignment/padding modifiers such as
// "%-10" or "%20" are consumed and discarded — they affect visual formatting
// only and carry no value for field extraction.
// ---------------------------------------------------------------------------

void LogPattern::parsePattern(const QString& pattern)
{
    m_segments.clear();

    const int n = pattern.size();
    int       i = 0;
    QString   currentLiteral;

    // Helper: flush any accumulated literal text as a Literal segment.
    auto flushLiteral = [&]() {
        if (!currentLiteral.isEmpty()) {
            PatternSegment seg;
            seg.kind    = PatternSegment::Kind::Literal;
            seg.literal = std::move(currentLiteral);
            m_segments.push_back(std::move(seg));
            currentLiteral.clear();
        }
    };

    // Helper: emit a Field segment (flushing any pending literal first).
    auto addField = [&](LogField field) {
        flushLiteral();
        PatternSegment seg;
        seg.kind  = PatternSegment::Kind::Field;
        seg.field = field;
        m_segments.push_back(seg);
    };

    while (i < n) {
        const QChar ch = pattern[i];

        if (ch != QLatin1Char('%')) {
            currentLiteral += ch;
            ++i;
            continue;
        }

        // '%' found — advance past it
        ++i;
        if (i >= n) break;

        // Skip optional alignment/padding modifier: -, digits
        while (i < n && (pattern[i] == QLatin1Char('-') || pattern[i].isDigit()))
            ++i;

        if (i >= n) break;

        const QChar spec = pattern[i];

        if (spec == QLatin1Char('d')) {
            // %d  or  %d{dateformat}
            addField(LogField::Timestamp);
            ++i;
            if (i < n && pattern[i] == QLatin1Char('{')) {
                while (i < n && pattern[i] != QLatin1Char('}'))
                    ++i;
                if (i < n) ++i; // consume '}'
            }
        } else if (spec == QLatin1Char('t')) {
            addField(LogField::ThreadId);
            ++i;
        } else if (spec == QLatin1Char('c')) {
            // %c  or  %c{precision}
            addField(LogField::LoggerName);
            ++i;
            if (i < n && pattern[i] == QLatin1Char('{')) {
                while (i < n && pattern[i] != QLatin1Char('}'))
                    ++i;
                if (i < n) ++i;
            }
        } else if (spec == QLatin1Char('p')) {
            addField(LogField::Level);
            ++i;
        } else if (spec == QLatin1Char('m')) {
            addField(LogField::Message);
            ++i;
        } else if (spec == QLatin1Char('x')) {
            addField(LogField::Ndc);
            ++i;
        } else if (spec == QLatin1Char('F')) {
            addField(LogField::SourceFile);
            ++i;
        } else if (spec == QLatin1Char('L')) {
            addField(LogField::SourceLine);
            ++i;
        } else if (spec == QLatin1Char('n')) {
            // %n = end-of-line — stop pattern parsing here
            ++i;
            break;
        } else if (spec == QLatin1Char('%')) {
            // %% = literal percent sign
            currentLiteral += QLatin1Char('%');
            ++i;
        } else {
            // Unknown / unsupported specifier — skip
            ++i;
        }
    }

    flushLiteral();
    m_valid = !m_segments.isEmpty();
}

// ---------------------------------------------------------------------------
// extractFields
//
// Walks the segment list and locates each field value in the line by
// scanning for surrounding literal delimiters.
//
// The %m (Message) field is treated as greedy: when a delimiter follows it,
// the *last* occurrence of that delimiter is used as the boundary so that
// the message body may itself contain the delimiter text.
//
// Lenient matching: if an expected literal is not found at the current
// position, the algorithm searches forward so that minor format deviations
// (extra spaces, missing optional tokens) degrade gracefully rather than
// silently discarding the rest of the line.
// ---------------------------------------------------------------------------

LogEntryFields LogPattern::extractFields(const QString& line) const
{
    LogEntryFields result;
    if (!m_valid || m_segments.isEmpty())
        return result;

    const QStringView lineView(line);
    const int         lineLen   = lineView.size();
    int               pos       = 0;
    const int         segCount  = m_segments.size();

    for (int i = 0; i < segCount && pos <= lineLen; ++i) {
        const PatternSegment& seg = m_segments[i];

        // ---- Literal segment: skip its text in the line ----
        if (seg.kind == PatternSegment::Kind::Literal) {
            const QStringView litView(seg.literal);
            if (lineView.mid(pos).startsWith(litView)) {
                pos += litView.size();
            } else {
                // Lenient: search forward for the literal
                int found = lineView.indexOf(litView, pos);
                if (found >= 0)
                    pos = found + litView.size();
                // If not found at all, keep pos (continue best-effort)
            }
            continue;
        }

        // ---- Field segment: find its end delimiter ----

        // Locate the next Literal segment to use as the end-delimiter.
        // Two consecutive Field segments without a Literal between them
        // cannot be split reliably — we stop searching at that point.
        QString nextLiteral;
        bool    hasNextLiteral = false;
        for (int j = i + 1; j < segCount; ++j) {
            if (m_segments[j].kind == PatternSegment::Kind::Literal) {
                nextLiteral    = m_segments[j].literal;
                hasNextLiteral = true;
            }
            break; // stop after the immediate next segment
        }

        int fieldEnd;
        if (!hasNextLiteral) {
            // Last field in pattern: consume everything to end of line
            fieldEnd = lineLen;
        } else if (seg.field == LogField::Message || seg.field == LogField::SourceFile) {
            // Greedy — use the LAST occurrence of the delimiter so the field
            // value may itself contain the delimiter character.
            // %m: message body can contain virtually anything.
            // %F: on Windows the path contains ':' (drive letter, e.g. C:\x\f.cpp:32)
            //     which fools a left-to-right search when the delimiter is ':'.
            int found = lineView.lastIndexOf(QStringView(nextLiteral));
            fieldEnd  = (found > pos) ? found : lineLen;
        } else {
            // Non-Message fields: skip any alignment-padding whitespace that may
            // have been left at pos by the previous field (e.g. %-20c pads the
            // logger name to 20 chars — those trailing spaces look identical to
            // a whitespace-only delimiter and confuse a plain indexOf from pos).
            int searchFrom = pos;
            while (searchFrom < lineLen && lineView[searchFrom].isSpace())
                ++searchFrom;

            int found = lineView.indexOf(QStringView(nextLiteral), searchFrom);
            if (found < 0 && searchFrom > pos)
                found = lineView.indexOf(QStringView(nextLiteral), pos); // fallback
            fieldEnd = (found >= pos) ? found : lineLen;
        }

        const int fieldLen = fieldEnd - pos;
        if (fieldLen > 0 && seg.field != LogField::Count) {
            // Trim alignment padding (spaces) from the extracted span
            const QStringView raw     = lineView.mid(pos, fieldLen);
            const QStringView trimmed = raw.trimmed();
            if (!trimmed.isEmpty()) {
                FieldSpan& span = result.spans[static_cast<int>(seg.field)];
                span.start  = static_cast<int>(trimmed.begin() - lineView.begin());
                span.length = static_cast<int>(trimmed.size());
            }
        }

        pos = fieldEnd;
    }

    return result;
}
