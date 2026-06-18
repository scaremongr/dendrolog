#include "patternheuristics.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace PatternHeuristics {

namespace {

// Full anchored match helper for token classification.
bool fullyMatches(const QString& pattern, const QString& text)
{
    const QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
    return re.isValid() && re.match(text).hasMatch();
}

// Anchored-at-offset match; returns captured length or -1.
int matchAt(const QRegularExpression& re, const QString& text, int offset)
{
    const QRegularExpressionMatch m =
        re.match(text, offset, QRegularExpression::NormalMatch,
                 QRegularExpression::AnchorAtOffsetMatchOption);
    return m.hasMatch() ? m.capturedLength(0) : -1;
}

// Post-pass: field-producing blocks must have unique names.
void ensureUniqueNames(PatternDefinition& definition)
{
    QSet<QString> seen;
    for (PatternBlock& block : definition.blocks) {
        if (!LogPattern::blockCreatesField(block))
            continue;
        if (block.name.trimmed().isEmpty())
            block.name = QStringLiteral("Field");
        QString base = block.name;
        int suffix = 2;
        while (seen.contains(block.name.toLower()))
            block.name = base + QStringLiteral(" %1").arg(suffix++);
        seen.insert(block.name.toLower());
    }
}

PatternBlock makeBlock(PatternBlock::MatchKind kind,
                       const QString& name,
                       const QString& leading = QString(),
                       const QString& closing = QString(),
                       const QString& separator = QString())
{
    PatternBlock block;
    block.matchKind = kind;
    block.name = name;
    block.leadingText = leading;   // opening wrapper
    block.closingText = closing;   // closing wrapper
    block.separator = separator;   // glue to the next block
    return block;
}

} // namespace

// ---------------------------------------------------------------- //
// Canonical token sub-regexes
// ---------------------------------------------------------------- //

QString timestampRegex()
{
    // Optional date (ISO "2024-01-31", EU "31.01.2024", slashed) + time
    // "12:34:56" with optional fraction. Date alone is not a timestamp.
    return QStringLiteral(
        R"((?:\d{4}[-/.]\d{2}[-/.]\d{2}[T ]|\d{2}[-/.]\d{2}[-/.]\d{4}[ ])?\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,9})?)");
}

QString levelRegex()
{
    return QStringLiteral(
        R"(\b(?i:TRACE|DEBUG|INFO|WARN(?:ING)?|ERROR|FATAL|CRITICAL|NOTICE|VERBOSE)\b)");
}

QString hexRegex()
{
    return QStringLiteral(R"((?:0[xX])?[0-9A-Fa-f]+)");
}

QString integerRegex()
{
    return QStringLiteral(R"([+-]?\d+)");
}

QString ipAddressRegex()
{
    return QStringLiteral(R"((?:\d{1,3}\.){3}\d{1,3}(?::\d{1,5})?)");
}

QString filePathRegex()
{
    // Optional drive letter, then non-separator chunks joined by slashes.
    //
    // The chunk class MUST exclude the path separators \ and / themselves:
    // otherwise [^…]+ can also swallow separators and overlaps the following
    // (?:[\\/]…)* group, so a path with N separators factorises 2^N ways —
    // catastrophic backtracking the moment the overall match has to fail or
    // backtrack. With separators excluded from the chunk the alternation is
    // unambiguous and matching stays linear. A leading [\\/]* handles
    // absolute Unix paths and UNC shares.
    return QStringLiteral(
        R"((?:[A-Za-z]:)?[\\/]*[^\r\n\s<>:"|?*\\/]+(?:[\\/]+[^\r\n\s<>:"|?*\\/]+)*)");
}

QString isoTimestampDetectPattern()
{
    // Capture layout used by LogParser::detectTimestamp:
    // (1) = "YYYY-MM-DD HH:MM:SS" (fixed offsets), (2) = "[.,]fff".
    return QStringLiteral(R"((\d{4}-\d{2}-\d{2}[ T]\d{2}:\d{2}:\d{2})([.,]\d+)?)");
}

QString levelDetectPattern()
{
    return QStringLiteral(R"(\b(INFO|WARN|WARNING|ERROR|DEBUG|TRACE|FATAL)\b)");
}

// ---------------------------------------------------------------- //
// Auto-detect
// ---------------------------------------------------------------- //

PatternDefinition suggestSchema(const QString& sampleLine)
{
    PatternDefinition def;
    const QString line = sampleLine;
    const int n = line.size();
    int pos = 0;

    const QRegularExpression tsRe(timestampRegex());
    const QRegularExpression lvlRe(levelRegex());
    const QRegularExpression ipRe(ipAddressRegex() + QStringLiteral(R"((?![\w.]))"));
    const QRegularExpression hexRe(QStringLiteral(R"((?:0[xX][0-9A-Fa-f]+|[0-9A-Fa-f]{6,})(?!\w))"));
    const QRegularExpression intRe(QStringLiteral(R"([+-]?\d+(?![\w.:]))"));
    const QRegularExpression punctRe(QStringLiteral(R"([-|;:,=>#*~]+)"));
    const QRegularExpression wordRe(QStringLiteral(R"(\S+)"));

    auto skipSpaces = [&]() {
        while (pos < n && (line[pos] == QLatin1Char(' ') || line[pos] == QLatin1Char('\t')))
            ++pos;
    };

    // Classify the inside of a bracketed segment.
    auto classifyInner = [&](const QString& inner, bool firstBracketed) -> PatternBlock {
        const QString t = inner.trimmed();
        if (fullyMatches(timestampRegex(), t))
            return makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp"));
        if (fullyMatches(levelRegex(), t))
            return makeBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"));
        if (fullyMatches(ipAddressRegex(), t))
            return makeBlock(PatternBlock::MatchKind::IpAddress, QStringLiteral("IP"));
        if (fullyMatches(integerRegex(), t))
            return makeBlock(PatternBlock::MatchKind::Integer, QStringLiteral("Id"));
        if (fullyMatches(QStringLiteral(R"(0[xX][0-9A-Fa-f]+|[0-9A-Fa-f]{6,})"), t))
            return makeBlock(PatternBlock::MatchKind::HexText, QStringLiteral("Hex"));
        return makeBlock(PatternBlock::MatchKind::TextUntilSeparator,
                         firstBracketed ? QStringLiteral("Thread") : QStringLiteral("Field"));
    };

    bool sawBracketedText = false;
    bool sawPlainWord = false;
    bool done = false;

    skipSpaces();
    while (pos < n && def.blocks.size() < 10 && !done) {
        const QChar c = line[pos];
        PatternBlock block;

        static const QString openers = QStringLiteral("[({<");
        static const QString closers = QStringLiteral("])}>");
        const int openerIdx = openers.indexOf(c);
        int closePos = -1;
        if (openerIdx >= 0)
            closePos = line.indexOf(closers[openerIdx], pos + 1);

        if (openerIdx >= 0 && closePos > pos) {
            const QString inner = line.mid(pos + 1, closePos - pos - 1);
            block = classifyInner(inner, !sawBracketedText);
            if (block.matchKind == PatternBlock::MatchKind::TextUntilSeparator)
                sawBracketedText = true;
            block.leadingText = openers[openerIdx];
            block.closingText = closers[openerIdx];
            pos = closePos + 1;
        } else {
            int len;
            if ((len = matchAt(tsRe, line, pos)) > 0) {
                block = makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp"));
                pos += len;
            } else if ((len = matchAt(lvlRe, line, pos)) > 0) {
                block = makeBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"));
                pos += len;
            } else if ((len = matchAt(ipRe, line, pos)) > 0) {
                block = makeBlock(PatternBlock::MatchKind::IpAddress, QStringLiteral("IP"));
                pos += len;
            } else if ((len = matchAt(hexRe, line, pos)) > 0) {
                block = makeBlock(PatternBlock::MatchKind::HexText, QStringLiteral("Hex"));
                pos += len;
            } else if ((len = matchAt(intRe, line, pos)) > 0) {
                block = makeBlock(PatternBlock::MatchKind::Integer, QStringLiteral("Number"));
                pos += len;
            } else if (!sawPlainWord && (len = matchAt(wordRe, line, pos)) > 0) {
                // First free word: most likely a logger / source name.
                QString word = line.mid(pos, len);
                QString sep;
                if (word.endsWith(QLatin1Char(':'))) {
                    sep = QStringLiteral(":");
                    word.chop(1);
                    --len;
                }
                block = makeBlock(PatternBlock::MatchKind::TextUntilSeparator,
                                  QStringLiteral("Source"), QString(), QString(),
                                  sep.isEmpty() ? QStringLiteral(" ") : sep);
                sawPlainWord = true;
                pos += len + (sep.isEmpty() ? 0 : 1);
            } else {
                // Second free word (or nothing matched): the rest is the message.
                block = makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message"));
                pos = n;
                done = true;
            }
        }

        // Trailing punctuation run (" - ", ": ", " | ") becomes the glue
        // between this block and the next one. Wrappers are a separate
        // property, so the glue slot is always free here.
        if (!done) {
            const int beforePunct = pos;
            skipSpaces();
            const int punctLen = matchAt(punctRe, line, pos);
            if (punctLen > 0 && block.separator.isEmpty()) {
                block.separator = line.mid(pos, punctLen);
                pos += punctLen;
            } else {
                pos = beforePunct;
            }
        }

        def.blocks.push_back(block);
        skipSpaces();
    }

    if (def.blocks.isEmpty()
            || def.blocks.last().matchKind != PatternBlock::MatchKind::Remainder) {
        def.blocks.push_back(
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")));
    }

    ensureUniqueNames(def);
    return def;
}

// ---------------------------------------------------------------- //
// Grok
// ---------------------------------------------------------------- //

namespace {

PatternBlock::MatchKind grokKind(const QString& patternName, bool* known)
{
    static const QHash<QString, PatternBlock::MatchKind> map = {
        { QStringLiteral("TIMESTAMP_ISO8601"), PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP"),         PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP_RFC2822"), PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP_OTHER"),   PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("HTTPDATE"),          PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("TIME"),              PatternBlock::MatchKind::Timestamp },
        { QStringLiteral("LOGLEVEL"),          PatternBlock::MatchKind::Level },
        { QStringLiteral("INT"),               PatternBlock::MatchKind::Integer },
        { QStringLiteral("POSINT"),            PatternBlock::MatchKind::Integer },
        { QStringLiteral("NONNEGINT"),         PatternBlock::MatchKind::Integer },
        { QStringLiteral("NUMBER"),            PatternBlock::MatchKind::Integer },
        { QStringLiteral("BASE10NUM"),         PatternBlock::MatchKind::Integer },
        { QStringLiteral("BASE16NUM"),         PatternBlock::MatchKind::HexText },
        { QStringLiteral("BASE16FLOAT"),       PatternBlock::MatchKind::HexText },
        { QStringLiteral("IP"),                PatternBlock::MatchKind::IpAddress },
        { QStringLiteral("IPV4"),              PatternBlock::MatchKind::IpAddress },
        { QStringLiteral("HOSTPORT"),          PatternBlock::MatchKind::IpAddress },
        { QStringLiteral("PATH"),              PatternBlock::MatchKind::FilePath },
        { QStringLiteral("UNIXPATH"),          PatternBlock::MatchKind::FilePath },
        { QStringLiteral("WINPATH"),           PatternBlock::MatchKind::FilePath },
        { QStringLiteral("URIPATH"),           PatternBlock::MatchKind::FilePath },
        { QStringLiteral("GREEDYDATA"),        PatternBlock::MatchKind::Remainder },
        { QStringLiteral("DATA"),              PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("WORD"),              PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("NOTSPACE"),          PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("USERNAME"),          PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("USER"),              PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("HOSTNAME"),          PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("IPORHOST"),          PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("PROG"),              PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("JAVACLASS"),         PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("EMAILADDRESS"),      PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("UUID"),              PatternBlock::MatchKind::HexText },
        { QStringLiteral("QUOTEDSTRING"),      PatternBlock::MatchKind::TextUntilSeparator },
        { QStringLiteral("QS"),                PatternBlock::MatchKind::TextUntilSeparator },
    };

    const auto it = map.constFind(patternName);
    if (known)
        *known = (it != map.constEnd());
    return it != map.constEnd() ? it.value()
                                : PatternBlock::MatchKind::TextUntilSeparator;
}

QString defaultGrokFieldName(const QString& patternName, PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::Timestamp: return QStringLiteral("Timestamp");
    case PatternBlock::MatchKind::Level:     return QStringLiteral("Level");
    case PatternBlock::MatchKind::IpAddress: return QStringLiteral("IP");
    case PatternBlock::MatchKind::Integer:   return QStringLiteral("Number");
    case PatternBlock::MatchKind::HexText:   return QStringLiteral("Hex");
    case PatternBlock::MatchKind::FilePath:  return QStringLiteral("Path");
    case PatternBlock::MatchKind::Remainder: return QStringLiteral("Message");
    default: {
        QString name = patternName.toLower();
        if (!name.isEmpty())
            name[0] = name[0].toUpper();
        return name;
    }
    }
}

// Grok literals are written with regex escaping (e.g. "\[") — undo it.
QString unescapeGrokLiteral(const QString& literal)
{
    QString out;
    out.reserve(literal.size());
    for (int i = 0; i < literal.size(); ++i) {
        if (literal[i] == QLatin1Char('\\') && i + 1 < literal.size())
            ++i;
        out += literal[i];
    }
    return out;
}

} // namespace

bool definitionFromGrok(const QString& grokExpression,
                        PatternDefinition* definition,
                        QString* warnings)
{
    if (!definition)
        return false;
    *definition = PatternDefinition();
    if (warnings)
        warnings->clear();

    static const QRegularExpression tokenRe(
        QStringLiteral(R"(%\{(\w+)(?::([\w.\-\[\]@]+))?(?::\w+)?\})"));

    QStringList unknownNames;
    int lastEnd = 0;
    QString pendingLiteral;

    QRegularExpressionMatchIterator it = tokenRe.globalMatch(grokExpression);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        pendingLiteral += unescapeGrokLiteral(
            grokExpression.mid(lastEnd, m.capturedStart(0) - lastEnd));
        lastEnd = m.capturedEnd(0);

        const QString patternName = m.captured(1);
        const QString fieldName = m.captured(2);

        bool known = false;
        PatternBlock block;
        block.matchKind = grokKind(patternName, &known);
        if (!known)
            unknownNames.append(patternName);

        block.name = fieldName.isEmpty()
            ? defaultGrokFieldName(patternName, block.matchKind)
            : fieldName;
        // Unnamed grok tokens are matched but not exported as fields.
        block.ignored = fieldName.isEmpty();
        block.leadingText = pendingLiteral;
        pendingLiteral.clear();

        // Quoted strings: wrap with quote literals so the lazy text knows
        // where to stop.
        if (patternName == QLatin1String("QUOTEDSTRING")
                || patternName == QLatin1String("QS")) {
            block.leadingText += QLatin1Char('"');
            block.closingText = QStringLiteral("\"");
        }

        definition->blocks.push_back(block);
    }

    if (definition->blocks.isEmpty())
        return false;

    const QString tail = unescapeGrokLiteral(grokExpression.mid(lastEnd));
    if (!tail.trimmed().isEmpty())
        definition->blocks.last().separator = tail;

    // Mid-schema GREEDYDATA degrades to greedy text; only the last block
    // may be Remainder.
    for (int i = 0; i < definition->blocks.size() - 1; ++i) {
        if (definition->blocks[i].matchKind == PatternBlock::MatchKind::Remainder)
            definition->blocks[i].matchKind = PatternBlock::MatchKind::GreedyTextUntilSeparator;
    }

    if (warnings && !unknownNames.isEmpty()) {
        unknownNames.removeDuplicates();
        *warnings = QObject::tr("Unknown Grok patterns mapped to plain text: %1")
                        .arg(unknownNames.join(QStringLiteral(", ")));
    }

    ensureUniqueNames(*definition);
    return true;
}

// ---------------------------------------------------------------- //
// Presets
// ---------------------------------------------------------------- //

QVector<Preset> builtInPresets()
{
    QVector<Preset> presets;

    {
        Preset p;
        p.name = QObject::tr("Classic: Timestamp [Thread] LEVEL - Message");
        p.definition.blocks = {
            makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Thread"),
                      QStringLiteral("["), QStringLiteral("]")),
            makeBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"),
                      QString(), QString(), QStringLiteral("-")),
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
        };
        presets.push_back(p);
    }
    {
        Preset p;
        p.name = QObject::tr("Bracketed: [Timestamp] [Level] Message");
        p.definition.blocks = {
            makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp"),
                      QStringLiteral("["), QStringLiteral("]")),
            makeBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"),
                      QStringLiteral("["), QStringLiteral("]")),
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
        };
        presets.push_back(p);
    }
    {
        Preset p;
        p.name = QObject::tr("Log4j: Timestamp LEVEL Logger - Message");
        p.definition.blocks = {
            makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
            makeBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Logger"),
                      QString(), QString(), QStringLiteral("-")),
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
        };
        presets.push_back(p);
    }
    {
        Preset p;
        p.name = QObject::tr("Syslog: Timestamp Host Process[PID]: Message");
        p.definition.blocks = {
            makeBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Host"),
                      QString(), QString(), QStringLiteral(" ")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Process")),
            makeBlock(PatternBlock::MatchKind::Integer, QStringLiteral("PID"),
                      QStringLiteral("["), QStringLiteral("]"), QStringLiteral(":")),
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
        };
        presets.push_back(p);
    }
    {
        Preset p;
        p.name = QObject::tr("Pipe-delimited: F1 | F2 | F3 | Message");
        p.definition.blocks = {
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Field 1"),
                      QString(), QString(), QStringLiteral("|")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Field 2"),
                      QString(), QString(), QStringLiteral("|")),
            makeBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Field 3"),
                      QString(), QString(), QStringLiteral("|")),
            makeBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
        };
        presets.push_back(p);
    }

    return presets;
}

} // namespace PatternHeuristics
