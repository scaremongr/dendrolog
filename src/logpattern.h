#ifndef LOGPATTERN_H
#define LOGPATTERN_H

#include "logfield.h"
#include <QString>
#include <QVector>

// ============================================================
// PatternSegment — one element of a parsed ConversionPattern.
// ============================================================

struct PatternSegment {
    enum class Kind { Literal, Field };

    Kind     kind    = Kind::Literal;
    LogField field   = LogField::Count; ///< Valid only when kind == Field
    QString  literal;                   ///< Valid only when kind == Literal
};

// ============================================================
// LogPattern — parses a Log4cxx / Log4j ConversionPattern string
// and provides fast, zero-copy structured-field extraction for
// individual log lines.
//
// Example pattern:
//   "> %d [%-10t] %-20c  %-8p %m ~~ %x {%F:%L}%n"
//
// Supported specifiers:
//   %d{fmt}  — Timestamp     %t  — ThreadId
//   %c{prec} — LoggerName    %p  — Level
//   %m       — Message       %x  — Ndc
//   %F       — SourceFile    %L  — SourceLine
//   %n       — end-of-line (stops parsing)
//   %%       — literal '%'
//   Alignment modifiers (%-10, %20, …) are recognised and ignored.
//
// Thread-safety: the object is read-only after construction and
// may be shared across threads without additional locking.
// ============================================================

class LogPattern {
public:
    LogPattern() = default;
    explicit LogPattern(const QString& conversionPattern);

    /// (Re)set the pattern.  Returns false if the string is empty.
    bool setPattern(const QString& conversionPattern);

    bool           isValid()        const noexcept { return m_valid; }
    const QString& patternString()  const noexcept { return m_patternString; }

    /// Extract structured fields from a single raw log line.
    ///
    /// The returned FieldSpan offsets refer into \a line — the caller
    /// must keep \a line alive for the lifetime of the returned fields.
    ///
    /// Returns an empty LogEntryFields when the line does not match
    /// (e.g. a multiline continuation line without header tokens).
    LogEntryFields extractFields(const QString& line) const;

private:
    void parsePattern(const QString& pattern);

    QString                  m_patternString;
    QVector<PatternSegment>  m_segments;
    bool                     m_valid = false;
};

#endif // LOGPATTERN_H
