#include "logpattern.h"
#include "patternheuristics.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// Hard cap on the number of compiled fallback variants so that a very
// long schema cannot turn every unmatched line into dozens of matches.
constexpr int kMaxPrefixVariants = 8;

QString matchKindToString(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::ConstantText:              return QStringLiteral("constant");
    case PatternBlock::MatchKind::TextUntilSeparator:         return QStringLiteral("text");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:   return QStringLiteral("greedy-text");
    case PatternBlock::MatchKind::Timestamp:                  return QStringLiteral("timestamp");
    case PatternBlock::MatchKind::Level:                      return QStringLiteral("level");
    case PatternBlock::MatchKind::HexText:                    return QStringLiteral("hex");
    case PatternBlock::MatchKind::Integer:                    return QStringLiteral("integer");
    case PatternBlock::MatchKind::IpAddress:                  return QStringLiteral("ip");
    case PatternBlock::MatchKind::FilePath:                   return QStringLiteral("path");
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
    } else if (normalized == QLatin1String("text")
               || normalized == QLatin1String("optional-text")) {
        // "optional-text" is a legacy kind: Text allows empty values now.
        *kind = PatternBlock::MatchKind::TextUntilSeparator;
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
    } else if (normalized == QLatin1String("ip")) {
        *kind = PatternBlock::MatchKind::IpAddress;
    } else if (normalized == QLatin1String("path")
               || normalized == QLatin1String("optional-path")) {
        // "optional-path" is a legacy kind folded into File path.
        *kind = PatternBlock::MatchKind::FilePath;
    } else if (normalized == QLatin1String("regex")) {
        *kind = PatternBlock::MatchKind::CustomRegex;
    } else if (normalized == QLatin1String("ignore")) {
        // Forward compatibility: older experimental kind maps to ignored text.
        *kind = PatternBlock::MatchKind::TextUntilSeparator;
    } else if (normalized == QLatin1String("remainder")) {
        *kind = PatternBlock::MatchKind::Remainder;
    } else {
        return false;
    }

    return true;
}

QString captureNameForIndex(int index)
{
    return QStringLiteral("lv%1").arg(index);
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

// Literal prefix before the value. Whitespace around the literal collapses:
// "] " and "]" behave identically, padding never matters.
QString leadingBoundaryRegex(const QString& boundary)
{
    if (boundary.isEmpty())
        return QString();

    return isWhitespaceOnly(boundary)
        ? QStringLiteral("[ \\t]+")
        : QRegularExpression::escape(boundary.trimmed()) + QStringLiteral("[ \\t]*");
}

// Literal suffix after the value, with optional whitespace before it so
// that "ERROR - msg" matches a separator entered as "-" or " - ".
QString trailingBoundaryRegex(const QString& boundary)
{
    if (boundary.isEmpty())
        return QString();

    return isWhitespaceOnly(boundary)
        ? QStringLiteral("[ \\t]+")
        : QStringLiteral("[ \\t]*") + QRegularExpression::escape(boundary.trimmed());
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

PatternBlock::MatchKind defaultKindForLegacySpec(const QChar spec)
{
    switch (spec.toLatin1()) {
    case 'd': return PatternBlock::MatchKind::Timestamp;
    case 'p': return PatternBlock::MatchKind::Level;
    case 'L': return PatternBlock::MatchKind::Integer;
    case 'm': return PatternBlock::MatchKind::GreedyTextUntilSeparator;
    case 'F': return PatternBlock::MatchKind::FilePath;
    case 'x': return PatternBlock::MatchKind::TextUntilSeparator;
    default:  return PatternBlock::MatchKind::TextUntilSeparator;
    }
}

// Kinds that cannot match arbitrary junk on their own; a fallback prefix
// may only end after one of these (or after a block with a closing wrapper
// or explicit glue), otherwise a trailing lazy ".+?" would "match" one
// character of anything.
bool isSolidBlock(const PatternBlock& block)
{
    if (!block.separator.isEmpty() || !block.closingText.isEmpty())
        return true;

    switch (block.matchKind) {
    case PatternBlock::MatchKind::TextUntilSeparator:
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:
    case PatternBlock::MatchKind::Remainder:
        return false;
    default:
        return true;
    }
}

// Distinctive enough that matching it is real evidence of a structured
// line (and not a coincidence on a free-form continuation line).
bool isEvidenceBlock(const PatternBlock& block)
{
    switch (block.matchKind) {
    case PatternBlock::MatchKind::Timestamp:
    case PatternBlock::MatchKind::Level:
    case PatternBlock::MatchKind::IpAddress:
    case PatternBlock::MatchKind::CustomRegex:
        return true;
    case PatternBlock::MatchKind::ConstantText:
        return block.customRegex.trimmed().size() >= 3;
    default:
        return false;
    }
}

QString valueRegexForBlock(const PatternBlock& block, bool isLastAnchored)
{
    const bool greedyToEnd = isLastAnchored
                          && block.separator.isEmpty()
                          && block.closingText.isEmpty();
    switch (block.matchKind) {
    case PatternBlock::MatchKind::ConstantText:
        return QRegularExpression::escape(block.customRegex);
    // Text kinds accept an empty value: a missing field must not kill
    // the parse of the whole line.
    case PatternBlock::MatchKind::TextUntilSeparator:
        return greedyToEnd ? QStringLiteral(".*") : QStringLiteral(".*?");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:
        return QStringLiteral(".*");
    case PatternBlock::MatchKind::Timestamp:
        return PatternHeuristics::timestampRegex();
    case PatternBlock::MatchKind::Level:
        return PatternHeuristics::levelRegex();
    case PatternBlock::MatchKind::HexText:
        return PatternHeuristics::hexRegex();
    case PatternBlock::MatchKind::Integer:
        return PatternHeuristics::integerRegex();
    case PatternBlock::MatchKind::IpAddress:
        return PatternHeuristics::ipAddressRegex();
    case PatternBlock::MatchKind::FilePath:
        return PatternHeuristics::filePathRegex();
    case PatternBlock::MatchKind::CustomRegex:
        // Isolate user alternations and keep our group numbering intact.
        return QStringLiteral("(?:") + block.customRegex.trimmed() + QStringLiteral(")");
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

bool isFreeTextKind(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::TextUntilSeparator:
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:
    case PatternBlock::MatchKind::Remainder:
        return true;
    default:
        return false;
    }
}

} // namespace

// Literals that any line matched by the first \a blockCount blocks must
// contain. Used as a cheap necessary-condition pre-filter before running
// the regex: when a required literal is missing the regex cannot match, so
// we skip it and avoid catastrophic backtracking while it proves non-match.
//
// Soundness rule: only collect literals the generator emits unconditionally
// (see buildRegexSource). Optional glue after a self-delimiting block, a
// regex separator, and whitespace-only boundaries contribute nothing.
QStringList LogPattern::requiredLiteralsForPrefix(const PatternDefinition& def,
                                                  int blockCount)
{
    QStringList literals;

    const QString prefix = def.linePrefix.trimmed();
    if (!prefix.isEmpty())
        literals.append(prefix);

    for (int i = 0; i < blockCount && i < def.blocks.size(); ++i) {
        const PatternBlock& block = def.blocks[i];

        const QString lead = block.leadingText.trimmed();
        if (!lead.isEmpty())
            literals.append(lead);

        if (block.matchKind == PatternBlock::MatchKind::ConstantText
                && !block.customRegex.isEmpty())
            literals.append(block.customRegex);

        const QString close = block.closingText.trimmed();
        if (!close.isEmpty())
            literals.append(close);

        // The glue separator is a mandatory literal only when it is a plain
        // literal and the block does not delimit itself (otherwise the
        // generator wraps it in an optional "(?:…)?" group).
        if (!block.separator.isEmpty()
                && !block.separatorIsRegex
                && !blockIsSelfDelimiting(block)) {
            const QString sep = block.separator.trimmed();
            if (!sep.isEmpty())
                literals.append(sep);
        }
    }

    return literals;
}

LogPattern::LogPattern(const QString& pattern)
{
    setPattern(pattern);
}

bool LogPattern::isAnchorKind(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::Timestamp:
    case PatternBlock::MatchKind::Level:
    case PatternBlock::MatchKind::IpAddress:
        return true;
    default:
        return false;
    }
}

bool LogPattern::isSelfDelimitingKind(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::Timestamp:
    case PatternBlock::MatchKind::Level:
    case PatternBlock::MatchKind::HexText:
    case PatternBlock::MatchKind::Integer:
    case PatternBlock::MatchKind::IpAddress:
        return true;
    default:
        return false;
    }
}

bool LogPattern::blockIsSelfDelimiting(const PatternBlock& block)
{
    return isSelfDelimitingKind(block.matchKind)
        || block.matchKind == PatternBlock::MatchKind::ConstantText
        || !block.closingText.isEmpty();
}

bool LogPattern::blockCreatesField(const PatternBlock& block)
{
    if (block.ignored)
        return false;
    // A constant is structure by default; naming it turns it into a field.
    if (block.matchKind == PatternBlock::MatchKind::ConstantText)
        return !block.name.trimmed().isEmpty();
    return true;
}

QString LogPattern::serializeDefinition(const PatternDefinition& definition)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("type"), QStringLiteral("dynamic-block-schema"));
    root.insert(QStringLiteral("linePrefix"), definition.linePrefix);

    QJsonArray blocksJson;
    for (const PatternBlock& block : definition.blocks) {
        QJsonObject blockJson;
        blockJson.insert(QStringLiteral("name"), block.name);
        blockJson.insert(QStringLiteral("kind"), matchKindToString(block.matchKind));
        blockJson.insert(QStringLiteral("leadingText"), block.leadingText);
        blockJson.insert(QStringLiteral("closingText"), block.closingText);
        blockJson.insert(QStringLiteral("separator"), block.separator);
        if (block.separatorIsRegex)
            blockJson.insert(QStringLiteral("separatorIsRegex"), true);
        blockJson.insert(QStringLiteral("customRegex"), block.customRegex);
        if (block.ignored)
            blockJson.insert(QStringLiteral("ignored"), true);
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
            const QString kindString = blockJson.value(QStringLiteral("kind")).toString();
            if (!matchKindFromString(kindString, &block.matchKind))
                return false;

            block.leadingText = blockJson.value(QStringLiteral("leadingText")).toString();
            block.closingText = blockJson.value(QStringLiteral("closingText")).toString();
            block.separator = blockJson.value(QStringLiteral("separator")).toString();
            block.separatorIsRegex = blockJson.value(QStringLiteral("separatorIsRegex")).toBool(false);
            block.customRegex = blockJson.value(QStringLiteral("customRegex")).toString();
            block.ignored = blockJson.value(QStringLiteral("ignored")).toBool(false)
                || kindString.trimmed().toLower() == QLatin1String("ignore");

            // Migration from v1/v2 schemas: there the "separator" of an
            // enclosed block doubled as its closing bracket. In the new
            // model wrappers belong to the block and the separator is
            // independent glue between blocks.
            if (!blockJson.contains(QStringLiteral("closingText"))
                    && !block.leadingText.isEmpty()) {
                block.closingText = block.separator;
                block.separator.clear();
            }

            parsed.blocks.push_back(block);
        }

        if (strict && !validateDefinition(parsed))
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

    if (!validateDefinition(parsed))
        return false;

    *definition = std::move(parsed);
    return true;
}

bool LogPattern::validateDefinition(const PatternDefinition& definition,
                                    QString* errorMessage)
{
    auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };

    if (definition.blocks.isEmpty())
        return fail(QObject::tr("Schema has no blocks."));

    QSet<QString> seenNames;
    for (int i = 0; i < definition.blocks.size(); ++i) {
        const PatternBlock& block = definition.blocks[i];
        const bool isLast = (i == definition.blocks.size() - 1);
        const QString position = QObject::tr("Block %1").arg(i + 1);

        if (block.matchKind == PatternBlock::MatchKind::ConstantText
                && block.customRegex.isEmpty())
            return fail(QObject::tr("%1: constant text block has no text.").arg(position));

        if (block.matchKind == PatternBlock::MatchKind::CustomRegex) {
            const QString rx = block.customRegex.trimmed();
            if (rx.isEmpty())
                return fail(QObject::tr("%1: custom regex is empty.").arg(position));
            const QRegularExpression probe(QStringLiteral("(?:") + rx + QStringLiteral(")"));
            if (!probe.isValid())
                return fail(QObject::tr("%1: invalid regex — %2")
                                .arg(position, probe.errorString()));
        }

        if (block.matchKind == PatternBlock::MatchKind::Remainder && !isLast)
            return fail(QObject::tr("%1: 'Remainder of line' must be the last block.").arg(position));

        if (block.separatorIsRegex && !block.separator.trimmed().isEmpty()) {
            const QRegularExpression probe(
                QStringLiteral("(?:") + block.separator.trimmed() + QStringLiteral(")"));
            if (!probe.isValid())
                return fail(QObject::tr("%1: invalid separator regex — %2")
                                .arg(position, probe.errorString()));
        }

        if (blockCreatesField(block)) {
            const QString name = block.name.trimmed();
            if (name.isEmpty())
                return fail(QObject::tr("%1: field name is empty.").arg(position));
            const QString key = name.toLower();
            if (seenNames.contains(key))
                return fail(QObject::tr("%1: duplicate field name '%2'.").arg(position, name));
            seenNames.insert(key);
        }
    }

    return true;
}

bool LogPattern::setPattern(const QString& pattern)
{
    m_patternString = pattern;
    m_definition = PatternDefinition();
    m_fullRegex = QRegularExpression();
    m_prefixVariants.clear();
    m_fieldIndexOfBlock.clear();
    m_fieldCount = 0;
    m_tailFieldBlockIndex = -1;
    m_valid = false;

    if (!deserializeDefinition(pattern, &m_definition, true))
        return false;

    buildExtractRegexes();
    return m_valid;
}

QStringList LogPattern::fieldNames() const
{
    QStringList names;
    names.reserve(m_definition.blocks.size());
    for (const PatternBlock& block : m_definition.blocks) {
        if (!blockCreatesField(block))
            continue;
        names.append(block.name);
    }
    return names;
}

QString LogPattern::buildRegexSource(int blockCount, bool anchorEnd) const
{
    const QVector<PatternBlock>& blocks = m_definition.blocks;
    QString regex = QStringLiteral("^");
    if (!m_definition.linePrefix.isEmpty())
        regex += QRegularExpression::escape(m_definition.linePrefix);

    for (int i = 0; i < blockCount; ++i) {
        const PatternBlock& block = blocks[i];
        const bool isLastOfSchema = (i == blocks.size() - 1);

        // Gap before the block: anchors may skip arbitrary garbage so the
        // token is *found*; everything else just collapses extra spaces.
        // After a lazy free-text block the skip is pointless (the text
        // itself absorbs anything) and would only shrink the field value.
        const bool prevIsFreeText = i > 0 && isFreeTextKind(blocks[i - 1].matchKind);
        if (isAnchorKind(block.matchKind) && !prevIsFreeText)
            regex += QStringLiteral(".*?");
        else
            regex += QStringLiteral("[ \\t]*");

        regex += leadingBoundaryRegex(block.leadingText);

        const QString value = valueRegexForBlock(block, isLastOfSchema && anchorEnd);
        if (value.isEmpty())
            return QString();
        regex += QStringLiteral("(?<") + captureNameForIndex(i) + QStringLiteral(">")
               + value + QStringLiteral(")");

        // Closing wrapper is part of the block itself and always required —
        // for anchors the bracket pair is part of what makes them findable.
        regex += trailingBoundaryRegex(block.closingText);

        // Glue to the next block. Self-delimiting blocks (distinctive
        // token shape, constant literal, or a closing wrapper) do not
        // require it: the block already ends the match by itself.
        if (!block.separator.isEmpty()) {
            const QString sep = block.separatorIsRegex
                ? QStringLiteral("[ \\t]*(?:") + block.separator + QStringLiteral(")")
                : trailingBoundaryRegex(block.separator);
            regex += blockIsSelfDelimiting(block)
                ? QStringLiteral("(?:") + sep + QStringLiteral(")?")
                : sep;
        }
    }

    if (anchorEnd)
        regex += QStringLiteral("[ \\t]*$");
    return regex;
}

void LogPattern::buildExtractRegexes()
{
    m_fullRegex = QRegularExpression();
    m_fullRequiredLiterals.clear();
    m_prefixVariants.clear();
    m_fieldIndexOfBlock.clear();
    m_fieldCount = 0;
    m_tailFieldBlockIndex = -1;
    m_valid = false;

    if (!validateDefinition(m_definition))
        return;

    const int blockCount = m_definition.blocks.size();
    m_fieldIndexOfBlock.fill(-1, blockCount);
    for (int i = 0; i < blockCount; ++i) {
        const PatternBlock& block = m_definition.blocks[i];
        if (blockCreatesField(block)) {
            m_fieldIndexOfBlock[i] = m_fieldCount++;
            if (isFreeTextKind(block.matchKind))
                m_tailFieldBlockIndex = i;
        }
    }

    const QString fullSource = buildRegexSource(blockCount, true);
    if (fullSource.isEmpty())
        return;
    m_fullRegex = QRegularExpression(fullSource);
    if (!m_fullRegex.isValid())
        return;
    m_fullRequiredLiterals = requiredLiteralsForPrefix(m_definition, blockCount);

    // Fallback cascade: prefixes ending after a "solid" block, longest first.
    for (int i = blockCount - 2; i >= 0 && m_prefixVariants.size() < kMaxPrefixVariants; --i) {
        if (!isSolidBlock(m_definition.blocks[i]))
            continue;

        bool hasEvidence = false;
        for (int j = 0; j <= i && !hasEvidence; ++j)
            hasEvidence = isEvidenceBlock(m_definition.blocks[j]);
        // Without distinctive evidence a degraded match would accept any
        // free-form line — skip such variants entirely.
        if (!hasEvidence)
            continue;

        CompiledVariant variant;
        variant.regex = QRegularExpression(buildRegexSource(i + 1, false));
        if (!variant.regex.isValid())
            continue;
        variant.blockCount = i + 1;
        variant.hasEvidence = hasEvidence;
        variant.requiredLiterals = requiredLiteralsForPrefix(m_definition, i + 1);
        m_prefixVariants.append(variant);
    }

    m_valid = true;
}

LineMatchResult LogPattern::matchLine(const QString& line) const
{
    LineMatchResult result;
    if (!m_valid)
        return result;

    auto fillSpans = [&result](const QRegularExpressionMatch& match, int blockCount) {
        result.spans.reserve(blockCount);
        for (int i = 0; i < blockCount; ++i) {
            const QString name = captureNameForIndex(i);
            const int start = match.capturedStart(name);
            if (start < 0)
                continue;
            BlockSpan span;
            span.blockIndex = i;
            span.start = start;
            span.length = match.capturedLength(name);
            result.spans.append(span);
        }
        result.matchedBlockCount = blockCount;
    };

    // Cheap necessary-condition gate: a regex whose mandatory literals are
    // not all present in the line cannot match, so skip it without paying
    // for the (possibly catastrophic) backtracking that proves non-match.
    const auto lineHasAllLiterals = [&line](const QStringList& literals) {
        for (const QString& literal : literals) {
            if (!line.contains(literal))
                return false;
        }
        return true;
    };

    if (lineHasAllLiterals(m_fullRequiredLiterals)) {
        const QRegularExpressionMatch full = m_fullRegex.match(line);
        if (full.hasMatch()) {
            fillSpans(full, m_definition.blocks.size());
            result.unparsedStart = -1;
            result.ok = true;
            return result;
        }
    }

    for (const CompiledVariant& variant : m_prefixVariants) {
        if (!lineHasAllLiterals(variant.requiredLiterals))
            continue;
        const QRegularExpressionMatch match = variant.regex.match(line);
        if (!match.hasMatch())
            continue;

        fillSpans(match, variant.blockCount);
        int tail = match.capturedEnd(0);
        while (tail < line.size() && line[tail].isSpace())
            ++tail;
        result.unparsedStart = (tail < line.size()) ? tail : -1;
        result.ok = true;
        return result;
    }

    return result;
}

LogEntryFields LogPattern::extractFields(const QString& line) const
{
    LogEntryFields result;
    if (!m_valid)
        return result;

    const LineMatchResult match = matchLine(line);
    if (!match.ok)
        return result;

    const QStringView lineView(line);
    result.resize(m_fieldCount);
    for (const BlockSpan& blockSpan : match.spans) {
        const int fieldIndex = m_fieldIndexOfBlock.value(blockSpan.blockIndex, -1);
        if (fieldIndex < 0)
            continue;

        QStringView captured = lineView.mid(blockSpan.start, blockSpan.length);
        if (shouldTrimCapture(m_definition.blocks[blockSpan.blockIndex].matchKind))
            captured = captured.trimmed();
        if (captured.isEmpty())
            continue;

        FieldSpan& span = result.spans[fieldIndex];
        span.start = static_cast<int>(captured.begin() - lineView.begin());
        span.length = static_cast<int>(captured.size());
    }

    // Graceful degradation: route the unparsed tail into the last free-text
    // field (typically "Message") when that block was not matched itself.
    if (match.unparsedStart >= 0 && m_tailFieldBlockIndex >= match.matchedBlockCount) {
        const int fieldIndex = m_fieldIndexOfBlock.value(m_tailFieldBlockIndex, -1);
        if (fieldIndex >= 0) {
            const QStringView tail = lineView.mid(match.unparsedStart).trimmed();
            if (!tail.isEmpty()) {
                FieldSpan& span = result.spans[fieldIndex];
                span.start = static_cast<int>(tail.begin() - lineView.begin());
                span.length = static_cast<int>(tail.size());
            }
        }
    }

    return result;
}
