#ifndef PATTERNHEURISTICS_H
#define PATTERNHEURISTICS_H

#include "logpattern.h"

#include <QString>
#include <QStringList>
#include <QVector>

// ============================================================
// PatternHeuristics
//
// Single home for every "what does this piece of text look like"
// heuristic in the application:
//
//   • Canonical token sub-regexes (timestamp, level, hex, ip, path…)
//     shared by LogPattern's regex generator and by LogParser's
//     line classification, so the two never drift apart.
//   • suggestSchema()  — "Auto-detect" magic: builds a draft
//     PatternDefinition from one real log line.
//   • definitionFromGrok() — translates a Logstash Grok expression
//     (%{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} …) into blocks.
//   • builtInPresets() — ready-made schema presets offered by the
//     editor dialog next to user presets stored in the INI file.
// ============================================================

namespace PatternHeuristics {

// ---- Canonical token sub-regexes (no captures, no anchors) ---- //

// Optional date part (ISO / EU / US) followed by a mandatory time part.
QString timestampRegex();
// TRACE/DEBUG/INFO/WARN(ING)/ERROR/FATAL/… with word boundaries.
QString levelRegex();
QString hexRegex();
QString integerRegex();
QString ipAddressRegex();
QString filePathRegex();

// Regexes with the exact capture layout LogParser::detectTimestamp /
// detectLogLevel rely on: (1) = "YYYY-MM-DD HH:MM:SS", (2) = "[.,]fff".
QString isoTimestampDetectPattern();
// (1) = level word.
QString levelDetectPattern();

// ---- Auto-detect ---------------------------------------------- //

// Builds a draft schema from a single real log line.
// Never fails: worst case is a single Remainder("Message") block.
PatternDefinition suggestSchema(const QString& sampleLine);

// ---- Grok ------------------------------------------------------ //

// Translates a Grok expression into a PatternDefinition.
// Returns false only when the input contains no %{...} tokens at all.
// Unknown pattern names degrade to TextUntilSeparator and are listed
// in *warnings (one line per unknown name) when the pointer is given.
bool definitionFromGrok(const QString& grokExpression,
                        PatternDefinition* definition,
                        QString* warnings = nullptr);

// ---- Presets ---------------------------------------------------- //

struct Preset {
    QString           name;
    PatternDefinition definition;
};

QVector<Preset> builtInPresets();

} // namespace PatternHeuristics

#endif // PATTERNHEURISTICS_H
