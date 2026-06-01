#include "logpattern.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

QString matchKindToString(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::ConstantText:              return QStringLiteral("constant");
    case PatternBlock::MatchKind::TextUntilSeparator:         return QStringLiteral("text");
    case PatternBlock::MatchKind::OptionalTextUntilSeparator: return QStringLiteral("optional-text");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:   return QStringLiteral("greedy-text");
    case PatternBlock::MatchKind::Timestamp:                  return QStringLiteral("timestamp");
    case PatternBlock::MatchKind::Level:                      return QStringLiteral("level");
    case PatternBlock::MatchKind::HexText:                    return QStringLiteral("hex");
    case PatternBlock::MatchKind::Integer:                    return QStringLiteral("integer");
    case PatternBlock::MatchKind::FilePath:                   return QStringLiteral("path");
    case PatternBlock::MatchKind::OptionalFilePath:           return QStringLiteral("optional-path");
    case PatternBlock::MatchKind::CustomRegex:                return QStringLiteral("regex");
    case PatternBlock::MatchKind::Remainder:                  return QStringLiteral("remainder");
    }
    return QStringLiteral("text");
}

bool matchKindFromString(const QString& text, PatternBlock::MatchKind* kind)
{
    if (!kind)
        return false;

    const QString normalized = text.trimmed().toLower();
    if (normalized == QLatin1String("constant")) {
        *kind = PatternBlock::MatchKind::ConstantText;
    } else if (normalized == QLatin1String("text")) {
        *kind = PatternBlock::MatchKind::TextUntilSeparator;
    } else if (normalized == QLatin1String("optional-text")) {
        *kind = PatternBlock::MatchKind::OptionalTextUntilSeparator;
    } else if (normalized == QLatin1String("greedy-text")) {
        *kind = PatternBlock::MatchKind::GreedyTextUntilSeparator;
    } else if (normalized == QLatin1String("timestamp")) {
        *kind = PatternBlock::MatchKind::Timestamp;
    } else if (normalized == QLatin1String("level")) {
        *kind = PatternBlock::MatchKind::Level;
    } else if (normalized == QLatin1String("hex")) {
        *kind = PatternBlock::MatchKind::HexText;
    } else if (normalized == QLatin1String("integer")) {
        *kind = PatternBlock::MatchKind::Integer;
    } else if (normalized == QLatin1String("path")) {
        *kind = PatternBlock::MatchKind::FilePath;
    } else if (normalized == QLatin1String("optional-path")) {
        *kind = PatternBlock::MatchKind::OptionalFilePath;
    } else if (normalized == QLatin1String("regex")) {
        *kind = PatternBlock::MatchKind::CustomRegex;
    } else if (normalized == QLatin1String("remainder")) {
        *kind = PatternBlock::MatchKind::Remainder;
    } else {
        return false;
    }

    return true;
}

QString captureNameForIndex(int index)
{
    return QStringLiteral("f%1").arg(index);
}

bool isWhitespaceOnly(const QString& text)
{
    if (text.isEmpty())
        return false;

    for (const QChar c : text) {
        if (!c.isSpace())
            return false;
    }
    return true;
}

QString separatorRegex(const QString& separator)
{
    if (separator.isEmpty())
        return QString();

    return isWhitespaceOnly(separator)
        ? QStringLiteral("\\s+")
        : QRegularExpression::escape(separator);
}

QString defaultNameForLegacySpec(const QChar spec)
{
    switch (spec.toLatin1()) {
    case 'd': return QStringLiteral("Timestamp");
    case 't': return QStringLiteral("Thread");
    case 'c': return QStringLiteral("Logger");
    case 'p': return QStringLiteral("Level");
    case 'm': return QStringLiteral("Message");
    case 'x': return QStringLiteral("Context");
    case 'F': return QStringLiteral("Source File");
    case 'L': return QStringLiteral("Source Line");
    default:  return QStringLiteral("Field");
    }
}

bool blockProducesField(PatternBlock::MatchKind kind)
{
    return kind != PatternBlock::MatchKind::ConstantText;
}

PatternBlock::MatchKind defaultKindForLegacySpec(const QChar spec)
{
    switch (spec.toLatin1()) {
    case 'd': return PatternBlock::MatchKind::Timestamp;
    case 'p': return PatternBlock::MatchKind::Level;
    case 'L': return PatternBlock::MatchKind::Integer;
    case 'm': return PatternBlock::MatchKind::GreedyTextUntilSeparator;
    case 'F': return PatternBlock::MatchKind::OptionalFilePath;
    case 'x': return PatternBlock::MatchKind::OptionalTextUntilSeparator;
    default:  return PatternBlock::MatchKind::TextUntilSeparator;
    }
}

QString regexForBlock(const PatternBlock& block, bool isLast)
{
    switch (block.matchKind) {
    case PatternBlock::MatchKind::ConstantText:
        return QRegularExpression::escape(block.customRegex);
    case PatternBlock::MatchKind::TextUntilSeparator:
        return isLast && block.separator.isEmpty() ? QStringLiteral(".+")
                                                   : QStringLiteral(".+?");
    case PatternBlock::MatchKind::OptionalTextUntilSeparator:
        return isLast && block.separator.isEmpty() ? QStringLiteral(".*")
                                                   : QStringLiteral(".*?");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:
        return isLast && block.separator.isEmpty() ? QStringLiteral(".+")
                                                   : QStringLiteral(".+");
    case PatternBlock::MatchKind::Timestamp:
        return QStringLiteral(R"(\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2}(?:[.,]\d+)?)");
    case PatternBlock::MatchKind::Level:
        return QStringLiteral(R"((?i:TRACE|DEBUG|INFO|WARN(?:ING)?|ERROR|FATAL))");
    case PatternBlock::MatchKind::HexText:
        return QStringLiteral(R"((?:0[xX])?[0-9A-Fa-f]+)");
    case PatternBlock::MatchKind::Integer:
        return QStringLiteral(R"(\d+)");
    case PatternBlock::MatchKind::FilePath:
        // Simple non-backtracking path: one or more non-separator chars optionally followed
        // by (slash + non-sep chars) repetitions.  No *? chain — no catastrophic backtrack.
        return QStringLiteral(R"([^\r\n\s<>:"|?*]+(?:[\\/][^\r\n\s<>:"|?*]+)*)");
    case PatternBlock::MatchKind::OptionalFilePath:
        return QStringLiteral(R"((?:[^\r\n\s<>:"|?*]+(?:[\\/][^\r\n\s<>:"|?*]+)*)?)"  );
    case PatternBlock::MatchKind::CustomRegex:
        return block.customRegex.trimmed();
    case PatternBlock::MatchKind::Remainder:
        return QStringLiteral(".*");
    }
    return QString();
}

bool shouldTrimCapture(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::ConstantText:
    case PatternBlock::MatchKind::CustomRegex:
    case PatternBlock::MatchKind::Remainder:
        return false;
    default:
        return true;
    }
}

} // namespace

LogPattern::LogPattern(const QString& pattern)
{
    setPattern(pattern);
}

QString LogPattern::serializeDefinition(const PatternDefinition& definition)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("type"), QStringLiteral("dynamic-block-schema"));
    root.insert(QStringLiteral("linePrefix"), definition.linePrefix);

    QJsonArray blocksJson;
    for (const PatternBlock& block : definition.blocks) {
        QJsonObject blockJson;
        blockJson.insert(QStringLiteral("name"), block.name);
        blockJson.insert(QStringLiteral("kind"), matchKindToString(block.matchKind));
        blockJson.insert(QStringLiteral("leadingText"), block.leadingText);
        blockJson.insert(QStringLiteral("separator"), block.separator);
        blockJson.insert(QStringLiteral("customRegex"), block.customRegex);
        blocksJson.append(blockJson);
    }
    root.insert(QStringLiteral("blocks"), blocksJson);

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool LogPattern::deserializeDefinition(const QString& text,
                                       PatternDefinition* definition,
                                       bool allowLegacy,
                                       bool strict)
{
    if (!definition)
        return false;

    *definition = PatternDefinition();

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        PatternDefinition parsed;
        const QJsonObject root = doc.object();
        parsed.linePrefix = root.value(QStringLiteral("linePrefix")).toString();

        const QJsonArray blocksJson = root.value(QStringLiteral("blocks")).toArray();
        parsed.blocks.reserve(blocksJson.size());
        for (const QJsonValue& value : blocksJson) {
            if (!value.isObject())
                return false;

            const QJsonObject blockJson = value.toObject();
            PatternBlock block;
            block.name = blockJson.value(QStringLiteral("name")).toString().trimmed();
            if (!matchKindFromString(blockJson.value(QStringLiteral("kind")).toString(), &block.matchKind))
                return false;

            block.leadingText = blockJson.value(QStringLiteral("leadingText")).toString();
            block.separator = blockJson.value(QStringLiteral("separator")).toString();
            block.customRegex = blockJson.value(QStringLiteral("customRegex")).toString();
            parsed.blocks.push_back(block);
        }

        if (strict && !isDefinitionValid(parsed))
            return false;

        *definition = std::move(parsed);
        return true;
    }

    return allowLegacy && parseLegacyConversionPattern(trimmed, definition);
}

bool LogPattern::parseLegacyConversionPattern(const QString& legacy,
                                              PatternDefinition* definition)
{
    if (!definition)
        return false;

    PatternDefinition parsed;
    QString currentLiteral;
    bool sawField = false;

    const int n = legacy.size();
    int i = 0;
    while (i < n) {
        const QChar ch = legacy[i];
        if (ch != QLatin1Char('%')) {
            currentLiteral += ch;
            ++i;
            continue;
        }

        ++i;
        if (i >= n)
            break;

        if (legacy[i] == QLatin1Char('%')) {
            currentLiteral += QLatin1Char('%');
            ++i;
            continue;
        }

        while (i < n && (legacy[i] == QLatin1Char('-') || legacy[i].isDigit()))
            ++i;
        if (i >= n)
            break;

        const QChar spec = legacy[i];
        if (spec == QLatin1Char('n')) {
            ++i;
            break;
        }

        const bool supported = QStringLiteral("dtcpmxFL").contains(spec);
        if (!supported) {
            currentLiteral += QLatin1Char('%');
            currentLiteral += spec;
            ++i;
            continue;
        }

        if (!sawField) {
            parsed.linePrefix = currentLiteral;
            sawField = true;
        } else if (!parsed.blocks.isEmpty()) {
            parsed.blocks.last().separator = currentLiteral;
        }
        currentLiteral.clear();

        PatternBlock block;
        block.name = defaultNameForLegacySpec(spec);
        block.matchKind = defaultKindForLegacySpec(spec);
        parsed.blocks.push_back(block);
        ++i;

        if (i < n && legacy[i] == QLatin1Char('{')) {
            while (i < n && legacy[i] != QLatin1Char('}'))
                ++i;
            if (i < n)
                ++i;
        }
    }

    if (parsed.blocks.isEmpty())
        return false;

    parsed.blocks.last().separator = currentLiteral;

    if (!isDefinitionValid(parsed))
        return false;

    *definition = std::move(parsed);
    return true;
}

bool LogPattern::isDefinitionValid(const PatternDefinition& definition)
{
    if (definition.blocks.isEmpty())
        return false;

    for (int i = 0; i < definition.blocks.size(); ++i) {
        const PatternBlock& block = definition.blocks[i];
        const bool isLast = (i == definition.blocks.size() - 1);
        if (block.matchKind != PatternBlock::MatchKind::ConstantText && block.name.trimmed().isEmpty())
            return false;
        if (block.matchKind == PatternBlock::MatchKind::ConstantText && block.customRegex.isEmpty())
            return false;
        if (block.matchKind == PatternBlock::MatchKind::CustomRegex && block.customRegex.trimmed().isEmpty())
            return false;
        if (block.matchKind == PatternBlock::MatchKind::Remainder && !isLast)
            return false;
    }

    return true;
}

bool LogPattern::setPattern(const QString& pattern)
{
    m_patternString = pattern;
    m_definition = PatternDefinition();
    m_extractRegex = QRegularExpression();
    m_captureNames.clear();
    m_captureBlockIndexes.clear();
    m_captureFieldIndexes.clear();
    m_valid = false;

    if (!deserializeDefinition(pattern, &m_definition, true))
        return false;

    buildExtractRegex();
    return m_valid;
}

QStringList LogPattern::fieldNames() const
{
    QStringList names;
    names.reserve(m_definition.blocks.size());
    for (const PatternBlock& block : m_definition.blocks) {
        if (!blockProducesField(block.matchKind))
            continue;
        names.append(block.name);
    }
    return names;
}

void LogPattern::buildExtractRegex()
{
    m_extractRegex = QRegularExpression();
    m_captureNames.clear();
    m_captureBlockIndexes.clear();
    m_captureFieldIndexes.clear();
    m_valid = false;

    if (!isDefinitionValid(m_definition))
        return;

    QString regex = QStringLiteral("^");
    if (!m_definition.linePrefix.isEmpty())
        regex += QRegularExpression::escape(m_definition.linePrefix);

    int outputFieldIndex = 0;
    for (int i = 0; i < m_definition.blocks.size(); ++i) {
        const PatternBlock& block = m_definition.blocks[i];
        const bool isLast = (i == m_definition.blocks.size() - 1);
        const QString blockRegex = regexForBlock(block, isLast);
        if (blockRegex.isEmpty())
            return;

        regex += separatorRegex(block.leadingText);

        if (blockProducesField(block.matchKind)) {
            const QString captureName = captureNameForIndex(i);
            m_captureNames.push_back(captureName);
            m_captureBlockIndexes.push_back(i);
            m_captureFieldIndexes.push_back(outputFieldIndex++);
            regex += QStringLiteral("(?<") + captureName + QStringLiteral(">") + blockRegex + QStringLiteral(")");
        } else {
            regex += blockRegex;
        }
        regex += separatorRegex(block.separator);
    }

    regex += QLatin1Char('$');
    m_extractRegex = QRegularExpression(regex);
    m_valid = m_extractRegex.isValid();
}

LogEntryFields LogPattern::extractFields(const QString& line) const
{
    LogEntryFields result;
    if (!m_valid || !m_extractRegex.isValid())
        return result;

    const QRegularExpressionMatch match = m_extractRegex.match(line);
    if (!match.hasMatch())
        return result;

    const QStringView lineView(line);
    result.resize(m_captureFieldIndexes.size());
    for (int i = 0; i < m_captureNames.size(); ++i) {
        const int start = match.capturedStart(m_captureNames[i]);
        const int length = match.capturedLength(m_captureNames[i]);
        if (start < 0 || length <= 0)
            continue;

        QStringView captured = lineView.mid(start, length);
        const int outputFieldIndex = m_captureFieldIndexes[i];
        const int blockIndex = m_captureBlockIndexes.value(i, -1);
        if (blockIndex < 0)
            continue;

        if (shouldTrimCapture(m_definition.blocks[blockIndex].matchKind))
            captured = captured.trimmed();
        if (captured.isEmpty())
            continue;

        FieldSpan& span = result.spans[outputFieldIndex];
        span.start = static_cast<int>(captured.begin() - lineView.begin());
        span.length = static_cast<int>(captured.size());
    }

    return result;
}
