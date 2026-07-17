#ifndef LOGPATTERN_H
#define LOGPATTERN_H

#include "logfield.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

struct PatternBlock {
    // Note: Text and Greedy text allow an empty value — a missing field
    // must not kill the parse of the whole line. (The former separate
    // "optional" kinds were folded into them.)
    enum class MatchKind {
        ConstantText,
        TextUntilSeparator,
        GreedyTextUntilSeparator,
        Timestamp,
        Level,
        HexText,
        Integer,
        IpAddress,
        FilePath,
        CustomRegex,
        Remainder
    };

    QString   name;
    MatchKind matchKind = MatchKind::TextUntilSeparator;

    // Wrappers — a property of the block itself ("enclosed by [ and ]").
    QString   leadingText;   ///< Opening wrapper literal before the value.
    QString   closingText;   ///< Closing wrapper literal after the value.

    // Glue — an independent separator between this block and the next one.
    // Empty = "auto": any run of spaces/tabs is accepted implicitly.
    QString   separator;
    bool      separatorIsRegex = false; ///< Treat \c separator as a regex, not a literal.

    QString   customRegex;   ///< Literal text (ConstantText) or user regex (CustomRegex).
    bool      ignored = false; ///< "Blind spot": matched and consumed but never shown as a field.
};

struct PatternDefinition {
    QString               linePrefix;
    QVector<PatternBlock> blocks;
};

/// Span of one block inside a matched line. Includes non-field blocks
/// (constants and ignored blocks) so the editor can colour everything.
struct BlockSpan {
    int blockIndex = -1;
    int start      = -1;
    int length     =  0;
};

/// Result of matching one line against the schema.
struct LineMatchResult {
    QVector<BlockSpan> spans;             ///< One entry per block that matched.
    int  matchedBlockCount = 0;           ///< How many blocks matched (leading, or prefix + re-synced tail).
    int  unparsedStart     = -1;          ///< Offset where parsing gave up; -1 = whole line parsed.
    // Re-sync degradation: blocks [skippedFrom..skippedTo) were skipped and
    // [holeStart..holeEnd) is the unparsed text where they should have been
    // (whitespace-trimmed; -1 = no hole / hole was empty).
    int  skippedFrom       = -1;
    int  skippedTo         = -1;
    int  holeStart         = -1;
    int  holeEnd           = -1;
    bool ok                = false;       ///< False = the line was not accepted at all.
};

// ============================================================
// LogPattern
//
// Stores a dynamic field-extraction schema made of ordered blocks and
// compiles it into a small cascade of guarded regular expressions:
//
//   • The full regex (^…$) is the fast path — one match per line.
//   • Prefix regexes (^… without $) implement graceful degradation:
//     when a line deviates mid-way, the longest matching prefix of
//     blocks is kept and the unparsed tail is routed into the last
//     free-text field (typically "Message").
//   • Re-sync regexes handle the opposite failure: a weak block whose
//     structure (separator / wrappers) is absent from the line must not
//     kill the self-detectable blocks after it. Each variant matches a
//     leading prefix of blocks, skips one contiguous run of weak
//     (non-evidence) blocks via a lazy hole, re-synchronizes at the next
//     anchor and requires the whole remaining schema to the end of the
//     line. Skipped fields stay empty; the hole text is routed into the
//     first skipped free-text field.
//   • Anchor kinds (Timestamp / Level / IP address) tolerate leading
//     garbage: the generator emits a lazy skip before them, so they
//     are *found* rather than required at an exact position, and
//     their trailing separator is optional.
//   • Whitespace collapses automatically: blocks may be padded with
//     spaces/tabs without dedicated Constant-text blocks.
//
// A degraded (prefix) match is only accepted when it contains at
// least one distinctive block — Timestamp, Level, IP address, Custom
// regex, or a Constant text of 3+ characters — so that free-form
// continuation lines are not misclassified as structured entries.
// ============================================================

class LogPattern {
public:
    LogPattern() = default;
    explicit LogPattern(const QString& pattern);

    bool setPattern(const QString& pattern);

    bool                     isValid() const noexcept { return m_valid; }
    const QString&           patternString() const noexcept { return m_patternString; }
    const PatternDefinition& definition() const noexcept { return m_definition; }
    QStringList              fieldNames() const;

    /// Best-effort match: full match first, then the prefix cascade.
    LineMatchResult matchLine(const QString& line) const;

    /// Field extraction for the parser; built on matchLine(), so the
    /// editor preview shows exactly what the parser will do.
    LogEntryFields extractFields(const QString& line) const;

    /// True for kinds that are distinctive enough to be searched for
    /// while skipping arbitrary garbage before them.
    static bool isAnchorKind(PatternBlock::MatchKind kind);

    /// True for kinds whose token shape ends the match by itself, so no
    /// separator is required after them (Timestamp, Level, Integer, Hex, IP).
    static bool isSelfDelimitingKind(PatternBlock::MatchKind kind);

    /// Block-level self-delimiting test: a self-delimiting kind, a constant
    /// literal, or any block with a closing wrapper ends the match by
    /// itself — the glue after it is optional.
    static bool blockIsSelfDelimiting(const PatternBlock& block);

    /// True when the block contributes a visible output field.
    /// Constant text becomes a field only when it is given a name;
    /// an unnamed (or hidden) constant is pure line structure.
    static bool blockCreatesField(const PatternBlock& block);

    /// Full structural validation with a human-readable error message.
    /// Includes QRegularExpression::isValid() checks for custom regexes.
    static bool validateDefinition(const PatternDefinition& definition,
                                   QString* errorMessage = nullptr);

    static QString serializeDefinition(const PatternDefinition& definition);
    // strict=true (default): validates with validateDefinition — used when applying a
    //   pattern for real parsing.
    // strict=false: skips validation — used by the editor dialog so that
    //   in-progress / partially-configured schemas can be loaded without data loss.
    static bool deserializeDefinition(const QString& text,
                                      PatternDefinition* definition,
                                      bool allowLegacy = true,
                                      bool strict = true);

private:
    static bool parseLegacyConversionPattern(const QString& legacy,
                                             PatternDefinition* definition);

    void buildExtractRegexes();
    /// Emits blocks [0..prefixCount); with \a resumeAt >= 0 additionally
    /// emits a lazy hole capture and blocks [resumeAt..end) (re-sync variant).
    QString buildRegexSource(int prefixCount, bool anchorEnd, int resumeAt = -1) const;

    /// Literals that any line matched by blocks [0..prefixCount) (plus
    /// [resumeAt..end) for re-sync variants) must contain — a cheap
    /// pre-filter to skip impossible (and possibly pathologically slow)
    /// regex matches.
    static QStringList requiredLiteralsForVariant(const PatternDefinition& def,
                                                  int prefixCount, int resumeAt);

    struct CompiledVariant {
        QRegularExpression regex;
        int  prefixCount = 0;     ///< Leading blocks matched normally.
        int  resumeAt    = -1;    ///< -1 = prefix variant; else blocks
                                  ///< [prefixCount..resumeAt) are skipped and
                                  ///< [resumeAt..end) match to the line end.
        ///< Anchor kind this variant re-syncs at (probe slot, see
        ///< anchorProbeSlot); only meaningful when resumeAt >= 0.
        PatternBlock::MatchKind resumeKind = PatternBlock::MatchKind::TextUntilSeparator;
        ///< Literals that MUST appear in any line this variant can match.
        ///< A cheap substring pre-check skips the (potentially very
        ///< expensive) regex match when one of them is absent.
        QStringList requiredLiterals;
    };

    /// Probe slot for an anchor kind (0..2) or -1. Re-sync variants often
    /// have no required literals (e.g. a bare Level), so an unanchored
    /// search for the anchor token itself — once per line per kind — is the
    /// pre-filter that keeps non-matching lines cheap.
    static int anchorProbeSlot(PatternBlock::MatchKind kind);

    QString                  m_patternString;
    PatternDefinition        m_definition;
    QRegularExpression       m_fullRegex;
    QStringList              m_fullRequiredLiterals; ///< Mandatory literals for m_fullRegex.
    QVector<CompiledVariant> m_variants;  ///< Prefix + re-sync fallbacks, most blocks first.
    QRegularExpression       m_anchorProbes[3]; ///< Token probes per anchor kind slot.
    QVector<int>             m_fieldIndexOfBlock; ///< block index -> output field index (-1 = none).
    int                      m_fieldCount = 0;
    int                      m_tailFieldBlockIndex = -1; ///< Free-text block that receives the unparsed tail.
    bool                     m_valid = false;
};

#endif // LOGPATTERN_H
