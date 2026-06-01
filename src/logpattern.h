#ifndef LOGPATTERN_H
#define LOGPATTERN_H

#include "logfield.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

struct PatternBlock {
    enum class MatchKind {
        ConstantText,
        TextUntilSeparator,
        OptionalTextUntilSeparator,
        GreedyTextUntilSeparator,
        Timestamp,
        Level,
        HexText,
        Integer,
        FilePath,
        OptionalFilePath,
        CustomRegex,
        Remainder
    };

    QString   name;
    MatchKind matchKind = MatchKind::TextUntilSeparator;
    QString   leadingText;
    QString   separator;
    QString   customRegex;
};

struct PatternDefinition {
    QString               linePrefix;
    QVector<PatternBlock> blocks;
};

// ============================================================
// LogPattern
//
// Stores a dynamic field-extraction schema made of ordered blocks.
// Each block has a name and a rule describing how its content is matched:
// typed matchers (timestamp / level / integer), text-until-separator,
// custom regex, or remainder-of-line.
//
// The schema is compiled once into a single QRegularExpression and then
// reused for every line. This keeps extraction fast enough for very large
// log files while still supporting dynamic user-defined fields.
// ============================================================

class LogPattern {
public:
    LogPattern() = default;
    explicit LogPattern(const QString& pattern);

    bool setPattern(const QString& pattern);

    bool                    isValid() const noexcept { return m_valid; }
    const QString&          patternString() const noexcept { return m_patternString; }
    const PatternDefinition& definition() const noexcept { return m_definition; }
    QStringList             fieldNames() const;

    LogEntryFields extractFields(const QString& line) const;

    static QString serializeDefinition(const PatternDefinition& definition);
    // strict=true (default): validates with isDefinitionValid — used when applying a
    //   pattern for real parsing.
    // strict=false: skips isDefinitionValid — used by the editor dialog so that
    //   in-progress / partially-configured schemas can be loaded without data loss.
    static bool deserializeDefinition(const QString& text,
                                      PatternDefinition* definition,
                                      bool allowLegacy = true,
                                      bool strict = true);

private:
    static bool parseLegacyConversionPattern(const QString& legacy,
                                             PatternDefinition* definition);
    static bool isDefinitionValid(const PatternDefinition& definition);

    void buildExtractRegex();

    QString            m_patternString;
    PatternDefinition  m_definition;
    QRegularExpression m_extractRegex;
    QStringList        m_captureNames;
    QVector<int>       m_captureBlockIndexes;
    QVector<int>       m_captureFieldIndexes;
    bool               m_valid = false;
};

#endif // LOGPATTERN_H
