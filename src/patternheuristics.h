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

// Optional date part (ISO / EU / US) followed by a mandatory time part,
// optional fraction and an optional zone glued to it (Z, ±hh, ±hh:mm, ±hhmm);
// or an access log "18/Nov/2026:13:55:36 +0000" (zone included), or a syslog
// "Nov 18 13:55:36".
QString timestampRegex();
// Any word of the level vocabulary (kLevelWords in logentry.h) with word
// boundaries; case-insensitive for ASCII letters only.
QString levelRegex();
QString hexRegex();
QString integerRegex();
QString ipAddressRegex();
QString filePathRegex();

// Reference regexes for LineClassifier's hand-written scanners (the
// equivalence is pinned by tests/lineindex_smoke).
//
// What follows the seconds: fraction and time zone. Capture layout:
// (1) "[.,]digits", (2) "Z"/"z", (3)(4) "±hh" and optional "mm" glued to the
// time, (5)(6) "±hh" and "mm" after one space, (7) "UTC"/"GMT" after one
// space. A zone must end at a word boundary; the offset range (mm <= 59,
// at most ±14:00) is checked by the consumer, not by the regex.
QString timestampSuffixDetectPattern();
// (1) = "YYYY-MM-DD[ T]HH:MM:SS" or "YYYY/MM/DD[ T]HH:MM:SS" (nginx error),
// then the suffix layout shifted by one. Searched anywhere in the line.
QString isoTimestampDetectPattern();
// Access log "DD/Mon/YYYY:HH:MM:SS": (1) day, (2) month name, (3) year,
// (4)(5)(6) time, then the suffix layout from (7). Searched anywhere.
QString accessTimestampDetectPattern();
// Syslog RFC 3164 "Mon DD HH:MM:SS" at the start of the line (no year; the
// day may be " 8" or "8"): (1) month name, (2) day, (3)(4)(5) time, then the
// suffix layout from (6).
QString syslogTimestampDetectPattern();
// Time without a date (F-11) "HH:MM:SS" at position 0 or right after "[" at
// position 0: (1)(2)(3) time — not followed by a digit or ":", (4) optional
// "[.,]digits". No zone. Only file readers with date state take it as a
// timestamp (LineClassifier::ClockDates).
QString clockTimestampDetectPattern();
// A timestamp of any form LineClassifier reads (ISO and YYYY/MM/DD, access,
// DD/MM/YYYY and DD.MM.YYYY, syslog, time without a date) with its fraction
// and zone — no anchor: match it with AnchorAtOffsetMatchOption; the match
// length is LineClassifier::timestampLengthAt (shape only, no date check;
// pinned by tests/lineindex_smoke). The "time" kind of a line template field
// (F-17, linetemplate.h).
QString timestampAtDetectPattern();
// English month abbreviation Jan…Dec, ASCII case folded, no capture.
QString monthNameRegex();
// (1) = level word.
QString levelDetectPattern();

// ---- Auto-detect ---------------------------------------------- //

// Builds a draft schema from a single real log line.
// Never fails: worst case is a single Remainder("Message") block.
// A JSON-object or logfmt line (detectLineFormat) gets a keyed schema
// (suggestKeyedSchema on that one line).
PatternDefinition suggestSchema(const QString& sampleLine);

// ---- Ключевая схема по образцам (F-4, PR 4) --------------------- //

// Единственное место правила «как выглядит строка JSON / logfmt»:
//   • Json — «{» в позиции 0 и хотя бы одна полная пара «ключ — значение»;
//   • Logfmt — с позиции 0 пары key=value (KeyedLine::PairReader), и их не
//     меньше двух: одна пара в начале — это и обычный текст «user=bob
//     logged in»;
//   • иначе Blocks (отступ, пустая строка, обычный лог).
// Только ASCII-структура строки; ключи ролей не требуются — формат строки,
// а не «распознана ли схемой» (это KeyedLine::Lookup::scan).
enum class LineFormat { Blocks, Json, Logfmt };
LineFormat detectLineFormat(QStringView line);

// Ключевая схема по образцам: формат — у большинства непустых строк
// (иначе false, *definition не тронут); ключи ролей — умолчания плюс ключи
// образцов, совпавшие с ключом роли без учёта регистра («LogLevel» .NET);
// колонки — ключи, встреченные больше чем в половине строк этого формата
// (keyedSampleKeys). Блоки — запасной «Message».
bool suggestKeyedSchema(const QStringList& sampleLines, PatternDefinition* definition);

// Ключи верхнего уровня строк образца в формате definition по порядку
// первой встречи, кроме ключей ролей, колонок definition и имён, которые
// совпали бы с именем колонки без учёта регистра. minShare — доля строк
// этого формата, где ключ встретился (0 — хоть раз; suggestKeyedSchema
// берёт > 0,5). Кнопка редактора «Add keys from sample».
QStringList keyedSampleKeys(const QStringList& sampleLines, const KeyedDefinition& definition,
                            double minShare = 0.0);

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
