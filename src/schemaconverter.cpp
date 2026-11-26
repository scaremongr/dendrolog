#include "schemaconverter.h"

#include "linetemplate.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <algorithm>
#include <optional>

// ---------------------------------------------------------------------------
// Прежний формат схемы (версия 3 и строка-паттерн) — только чтение для
// перевода. Код чтения, сериализации и проверки перенесён из LogPattern без
// изменений: каноническая сериализация обязана совпадать буква в букву
// (отпечатки манифеста встроенных схем прежних выпусков).
// ---------------------------------------------------------------------------
namespace v3 {

struct Block {
    enum class MatchKind {
        ConstantText, TextUntilSeparator, GreedyTextUntilSeparator, Timestamp, Level,
        HexText, Integer, IpAddress, FilePath, CustomRegex, Remainder
    };
    QString   name;
    MatchKind matchKind = MatchKind::TextUntilSeparator;
    QString   leadingText;
    QString   closingText;
    QString   separator;
    bool      separatorIsRegex = false;
    QString   customRegex;
    bool      ignored = false;
};

struct Definition {
    QString linePrefix;
    QVector<Block> blocks;
};

bool validateDefinition(const Definition& definition, QString* errorMessage = nullptr);
bool parseLegacyConversionPattern(const QString& legacy, Definition* definition);

QString matchKindToString(Block::MatchKind kind)
{
    switch (kind) {
    case Block::MatchKind::ConstantText:              return QStringLiteral("constant");
    case Block::MatchKind::TextUntilSeparator:         return QStringLiteral("text");
    case Block::MatchKind::GreedyTextUntilSeparator:   return QStringLiteral("greedy-text");
    case Block::MatchKind::Timestamp:                  return QStringLiteral("timestamp");
    case Block::MatchKind::Level:                      return QStringLiteral("level");
    case Block::MatchKind::HexText:                    return QStringLiteral("hex");
    case Block::MatchKind::Integer:                    return QStringLiteral("integer");
    case Block::MatchKind::IpAddress:                  return QStringLiteral("ip");
    case Block::MatchKind::FilePath:                   return QStringLiteral("path");
    case Block::MatchKind::CustomRegex:                return QStringLiteral("regex");
    case Block::MatchKind::Remainder:                  return QStringLiteral("remainder");
    }
    return QStringLiteral("text");
}

bool matchKindFromString(const QString& text, Block::MatchKind* kind)
{
    if (!kind)
        return false;

    const QString normalized = text.trimmed().toLower();
    if (normalized == QLatin1String("constant")) {
        *kind = Block::MatchKind::ConstantText;
    } else if (normalized == QLatin1String("text")
               || normalized == QLatin1String("optional-text")) {
        // "optional-text" is a legacy kind: Text allows empty values now.
        *kind = Block::MatchKind::TextUntilSeparator;
    } else if (normalized == QLatin1String("greedy-text")) {
        *kind = Block::MatchKind::GreedyTextUntilSeparator;
    } else if (normalized == QLatin1String("timestamp")) {
        *kind = Block::MatchKind::Timestamp;
    } else if (normalized == QLatin1String("level")) {
        *kind = Block::MatchKind::Level;
    } else if (normalized == QLatin1String("hex")) {
        *kind = Block::MatchKind::HexText;
    } else if (normalized == QLatin1String("integer")) {
        *kind = Block::MatchKind::Integer;
    } else if (normalized == QLatin1String("ip")) {
        *kind = Block::MatchKind::IpAddress;
    } else if (normalized == QLatin1String("path")
               || normalized == QLatin1String("optional-path")) {
        // "optional-path" is a legacy kind folded into File path.
        *kind = Block::MatchKind::FilePath;
    } else if (normalized == QLatin1String("regex")) {
        *kind = Block::MatchKind::CustomRegex;
    } else if (normalized == QLatin1String("ignore")) {
        // Forward compatibility: older experimental kind maps to ignored text.
        *kind = Block::MatchKind::TextUntilSeparator;
    } else if (normalized == QLatin1String("remainder")) {
        *kind = Block::MatchKind::Remainder;
    } else {
        return false;
    }

    return true;
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

Block::MatchKind defaultKindForLegacySpec(const QChar spec)
{
    switch (spec.toLatin1()) {
    case 'd': return Block::MatchKind::Timestamp;
    case 'p': return Block::MatchKind::Level;
    case 'L': return Block::MatchKind::Integer;
    case 'm': return Block::MatchKind::GreedyTextUntilSeparator;
    case 'F': return Block::MatchKind::FilePath;
    case 'x': return Block::MatchKind::TextUntilSeparator;
    default:  return Block::MatchKind::TextUntilSeparator;
    }
}

// Kinds that cannot match arbitrary junk on their own; a fallback prefix
// may only end after one of these (or after a block with a closing wrapper
// or explicit glue), otherwise a trailing lazy ".+?" would "match" one

bool isSelfDelimitingKind(Block::MatchKind kind)
{
    switch (kind) {
    case Block::MatchKind::Timestamp:
    case Block::MatchKind::Level:
    case Block::MatchKind::HexText:
    case Block::MatchKind::Integer:
    case Block::MatchKind::IpAddress:
        return true;
    default:
        return false;
    }
}

bool blockIsSelfDelimiting(const Block& block)
{
    return isSelfDelimitingKind(block.matchKind)
        || block.matchKind == Block::MatchKind::ConstantText
        || !block.closingText.isEmpty();
}

bool blockCreatesField(const Block& block)
{
    if (block.ignored)
        return false;
    // A constant is structure by default; naming it turns it into a field.
    if (block.matchKind == Block::MatchKind::ConstantText)
        return !block.name.trimmed().isEmpty();
    return true;
}


QString serializeDefinition(const Definition& definition)
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("type"), QStringLiteral("dynamic-block-schema"));
    root.insert(QStringLiteral("linePrefix"), definition.linePrefix);

    QJsonArray blocksJson;
    for (const Block& block : definition.blocks) {
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

bool deserializeDefinition(const QString& text,
                                       Definition* definition,
                                       bool allowLegacy,
                                       bool strict)
{
    if (!definition)
        return false;

    *definition = Definition();

    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && doc.isObject()) {
        Definition parsed;
        const QJsonObject root = doc.object();

        // Refuse documents from a future format version instead of silently
        // misparsing them (serializeDefinition currently writes version 3).
        if (root.value(QStringLiteral("version")).toInt(0) > 3)
            return false;

        parsed.linePrefix = root.value(QStringLiteral("linePrefix")).toString();

        const QJsonArray blocksJson = root.value(QStringLiteral("blocks")).toArray();
        parsed.blocks.reserve(blocksJson.size());
        for (const QJsonValue& value : blocksJson) {
            if (!value.isObject())
                return false;

            const QJsonObject blockJson = value.toObject();
            Block block;
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

        // «keyed» (F-4, до F-17, шаг 7) не читается: схема — свои запасные
        // блоки («Message»), JSON и logfmt — формат строк файла.

        if (strict && !validateDefinition(parsed))
            return false;

        *definition = std::move(parsed);
        return true;
    }

    return allowLegacy && parseLegacyConversionPattern(trimmed, definition);
}

bool parseLegacyConversionPattern(const QString& legacy,
                                              Definition* definition)
{
    if (!definition)
        return false;

    Definition parsed;
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

        Block block;
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

bool validateDefinition(const Definition& definition,
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
        const Block& block = definition.blocks[i];
        const bool isLast = (i == definition.blocks.size() - 1);
        const QString position = QObject::tr("Block %1").arg(i + 1);

        if (block.matchKind == Block::MatchKind::ConstantText
                && block.customRegex.isEmpty())
            return fail(QObject::tr("%1: constant text block has no text.").arg(position));

        if (block.matchKind == Block::MatchKind::CustomRegex) {
            const QString rx = block.customRegex.trimmed();
            if (rx.isEmpty())
                return fail(QObject::tr("%1: custom regex is empty.").arg(position));
            const QRegularExpression probe(QStringLiteral("(?:") + rx + QStringLiteral(")"));
            if (!probe.isValid())
                return fail(QObject::tr("%1: invalid regex — %2")
                                .arg(position, probe.errorString()));
        }

        if (block.matchKind == Block::MatchKind::Remainder && !isLast)
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

// Post-pass: field-producing blocks must have unique names.
void ensureUniqueNames(Definition& definition)
{
    QSet<QString> seen;
    for (Block& block : definition.blocks) {
        if (!blockCreatesField(block))
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

Block::MatchKind grokKind(const QString& patternName, bool* known)
{
    static const QHash<QString, Block::MatchKind> map = {
        { QStringLiteral("TIMESTAMP_ISO8601"), Block::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP"),         Block::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP_RFC2822"), Block::MatchKind::Timestamp },
        { QStringLiteral("DATESTAMP_OTHER"),   Block::MatchKind::Timestamp },
        { QStringLiteral("HTTPDATE"),          Block::MatchKind::Timestamp },
        { QStringLiteral("TIME"),              Block::MatchKind::Timestamp },
        { QStringLiteral("LOGLEVEL"),          Block::MatchKind::Level },
        { QStringLiteral("INT"),               Block::MatchKind::Integer },
        { QStringLiteral("POSINT"),            Block::MatchKind::Integer },
        { QStringLiteral("NONNEGINT"),         Block::MatchKind::Integer },
        { QStringLiteral("NUMBER"),            Block::MatchKind::Integer },
        { QStringLiteral("BASE10NUM"),         Block::MatchKind::Integer },
        { QStringLiteral("BASE16NUM"),         Block::MatchKind::HexText },
        { QStringLiteral("BASE16FLOAT"),       Block::MatchKind::HexText },
        { QStringLiteral("IP"),                Block::MatchKind::IpAddress },
        { QStringLiteral("IPV4"),              Block::MatchKind::IpAddress },
        { QStringLiteral("HOSTPORT"),          Block::MatchKind::IpAddress },
        { QStringLiteral("PATH"),              Block::MatchKind::FilePath },
        { QStringLiteral("UNIXPATH"),          Block::MatchKind::FilePath },
        { QStringLiteral("WINPATH"),           Block::MatchKind::FilePath },
        { QStringLiteral("URIPATH"),           Block::MatchKind::FilePath },
        { QStringLiteral("GREEDYDATA"),        Block::MatchKind::Remainder },
        { QStringLiteral("DATA"),              Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("WORD"),              Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("NOTSPACE"),          Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("USERNAME"),          Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("USER"),              Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("HOSTNAME"),          Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("IPORHOST"),          Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("PROG"),              Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("JAVACLASS"),         Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("EMAILADDRESS"),      Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("UUID"),              Block::MatchKind::HexText },
        { QStringLiteral("QUOTEDSTRING"),      Block::MatchKind::TextUntilSeparator },
        { QStringLiteral("QS"),                Block::MatchKind::TextUntilSeparator },
    };

    const auto it = map.constFind(patternName);
    if (known)
        *known = (it != map.constEnd());
    return it != map.constEnd() ? it.value()
                                : Block::MatchKind::TextUntilSeparator;
}

QString defaultGrokFieldName(const QString& patternName, Block::MatchKind kind)
{
    switch (kind) {
    case Block::MatchKind::Timestamp: return QStringLiteral("Timestamp");
    case Block::MatchKind::Level:     return QStringLiteral("Level");
    case Block::MatchKind::IpAddress: return QStringLiteral("IP");
    case Block::MatchKind::Integer:   return QStringLiteral("Number");
    case Block::MatchKind::HexText:   return QStringLiteral("Hex");
    case Block::MatchKind::FilePath:  return QStringLiteral("Path");
    case Block::MatchKind::Remainder: return QStringLiteral("Message");
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

bool definitionFromGrok(const QString& grokExpression,
                        Definition* definition,
                        QString* warnings)
{
    if (!definition)
        return false;
    *definition = Definition();
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
        Block block;
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
        if (definition->blocks[i].matchKind == Block::MatchKind::Remainder)
            definition->blocks[i].matchKind = Block::MatchKind::GreedyTextUntilSeparator;
    }

    if (warnings && !unknownNames.isEmpty()) {
        unknownNames.removeDuplicates();
        *warnings = QObject::tr("Unknown Grok patterns mapped to plain text: %1")
                        .arg(unknownNames.join(QStringLiteral(", ")));
    }

    ensureUniqueNames(*definition);
    return true;
}

} // namespace v3

namespace {

// Кусок будущего шаблона. Gap — «{? ?}» (пробелы могут быть, а могут и не
// быть), Space — « » (один или несколько), Opt — «{?…?}» из Lit и Space.
struct Piece {
    enum class Type { Lit, Gap, Space, Opt, Field };
    Type type = Type::Lit;
    QString text;               ///< Lit — литерал; Field — «{…}» целиком
    QVector<Piece> content;     ///< Opt
    int block = -1;             ///< Field — номер блока
    QString name;               ///< Field — имя («_» — без колонки)
    QString kind;               ///< Field — вид (пусто — вид по умолчанию)
};

Piece lit(const QString& t)
{
    Piece p;
    p.type = Piece::Type::Lit;
    p.text = t;
    return p;
}

Piece gap()
{
    Piece p;
    p.type = Piece::Type::Gap;
    return p;
}

Piece space()
{
    Piece p;
    p.type = Piece::Type::Space;
    return p;
}

Piece opt(QVector<Piece> content)
{
    Piece p;
    p.type = Piece::Type::Opt;
    p.content = std::move(content);
    return p;
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

// Регекс в виде поля шаблона: «/» внутри экранируется.
QString regexSpec(const QString& rx)
{
    QString out = QStringLiteral("/");
    for (int i = 0; i < rx.size(); ++i) {
        const QChar c = rx.at(i);
        if (c == QLatin1Char('\\') && i + 1 < rx.size()) {
            out += c;
            out += rx.at(++i);
            continue;
        }
        if (c == QLatin1Char('/'))
            out += QLatin1Char('\\');
        out += c;
    }
    out += QLatin1Char('/');
    return out;
}

// Простой регекс-разделитель → куски: литеральные знаки (и «\»-экранированные
// не буквы), пробельные атомы ([ \t], [\t ], \s, пробел, табуляция) с
// квантификаторами ?, *, + у пробелов и ? у одного знака. Иначе — nullopt.
std::optional<QVector<Piece>> translateRegex(const QString& rx)
{
    QVector<Piece> out;
    const auto appendLiteral = [&out](QChar c) {
        if (!out.isEmpty() && out.constLast().type == Piece::Type::Lit)
            out.last().text += c;
        else
            out.append(lit(QString(c)));
    };
    int i = 0;
    const int n = int(rx.size());
    while (i < n) {
        bool ws = false;
        QChar ch;
        const QChar c = rx.at(i);
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= n)
                return std::nullopt;
            const QChar e = rx.at(i + 1);
            if (e == QLatin1Char('s') || e == QLatin1Char('t')) {
                ws = true;
            } else if (e.isLetterOrNumber()) {
                return std::nullopt;
            } else {
                ch = e;
            }
            i += 2;
        } else if (c == QLatin1Char('[')) {
            const QString cls = rx.mid(i);
            if (cls.startsWith(QLatin1String("[ \\t]")) || cls.startsWith(QLatin1String("[\\t ]"))) {
                ws = true;
                i += 5;
            } else {
                return std::nullopt;
            }
        } else if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
            ws = true;
            ++i;
        } else if (QStringLiteral("().|^$?*+{}]").contains(c)) {
            return std::nullopt;
        } else {
            ch = c;
            ++i;
        }
        QChar quant;
        if (i < n && (rx.at(i) == QLatin1Char('?') || rx.at(i) == QLatin1Char('*')
                      || rx.at(i) == QLatin1Char('+'))) {
            quant = rx.at(i);
            ++i;
            if (i < n && (rx.at(i) == QLatin1Char('?') || rx.at(i) == QLatin1Char('+')))
                return std::nullopt;   // ленивые и захватнические квантификаторы
        }
        if (ws) {
            out.append(quant == QLatin1Char('?') || quant == QLatin1Char('*') ? gap() : space());
        } else if (quant.isNull()) {
            appendLiteral(ch);
        } else if (quant == QLatin1Char('?')) {
            if (ch == QLatin1Char('?'))
                return std::nullopt;   // «?» внутри «{?…?}» не выразить
            out.append(opt({ lit(QString(ch)) }));
        } else {
            return std::nullopt;
        }
    }
    return out;
}

bool hasOptional(const QVector<Piece>& pieces)
{
    for (const Piece& p : pieces) {
        if (p.type == Piece::Type::Opt || p.type == Piece::Type::Gap)
            return true;
    }
    return false;
}

// Gap и Space подряд: Space главнее; Gap в начале и в конце не нужен.
QVector<Piece> normalized(const QVector<Piece>& in, bool top)
{
    QVector<Piece> out;
    for (const Piece& p : in) {
        const bool blank = p.type == Piece::Type::Gap || p.type == Piece::Type::Space;
        if (blank && !out.isEmpty()) {
            Piece& last = out.last();
            if (last.type == Piece::Type::Space)
                continue;
            if (last.type == Piece::Type::Gap) {
                if (p.type == Piece::Type::Space)
                    last = p;
                continue;
            }
        }
        if (p.type == Piece::Type::Lit && !out.isEmpty() && out.constLast().type == Piece::Type::Lit) {
            out.last().text += p.text;
            continue;
        }
        out.append(p);
    }
    if (top) {
        while (!out.isEmpty() && out.constFirst().type == Piece::Type::Gap)
            out.removeFirst();
        while (!out.isEmpty() && (out.constLast().type == Piece::Type::Gap
                                  || out.constLast().type == Piece::Type::Space))
            out.removeLast();
    }
    return out;
}

QString render(const QVector<Piece>& pieces, QVector<std::pair<int, int>>* fieldPositions)
{
    QString out;
    for (const Piece& p : pieces) {
        switch (p.type) {
        case Piece::Type::Lit:
            out += LineTemplate::escapeLiteral(p.text);
            break;
        case Piece::Type::Gap:
            out += QStringLiteral("{? ?}");
            break;
        case Piece::Type::Space:
            out += QLatin1Char(' ');
            break;
        case Piece::Type::Opt:
            out += QStringLiteral("{?") + render(p.content, nullptr) + QStringLiteral("?}");
            break;
        case Piece::Type::Field:
            if (fieldPositions)
                fieldPositions->append({ int(out.size()), p.block });
            out += QLatin1Char('{') + p.name;
            if (!p.kind.isEmpty())
                out += QLatin1Char(':') + p.kind;
            out += QLatin1Char('}');
            break;
        }
    }
    return out;
}

QString escapeOfValue(const QString& v)
{
    QString out;
    for (const QChar c : v) {
        if (c == QLatin1Char('|') || c == QLatin1Char(')') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

} // namespace

// Встроенные схемы прежних выпусков (версия 3) → их шаблоны (F-17, шаг 9):
// сохранённое состояние со встроенной схемой получает ровно тот шаблон, что
// поставляется (resources/patterns), — и находит его в каталоге по
// определению, а не добавляет «(from session)». Ключ — SHA-1 канонической
// сериализации (canonicalBlocks); последние версии и версии до Q-5.
// Совпадение с поставляемыми файлами закреплено в tests/builtin_schemas.
struct BuiltinV3 {
    const char* sha1;
    const char* templateText;
};
constexpr BuiltinV3 kBuiltinV3[] = {
    {"3688bb1071a60fe83e8136eadc6772c4c5f7ef8d", // access-combined
     "{Client:ip} {_:word} {User:word} [{Time:time}] {Request:quoted} {Status:int} {Bytes:word}{? {Referer:quoted} {User agent:quoted}?}"},
    {"c5d03fcdf7d4ef0d0ef623b001b7f4cf780353cb", // access-combined
     "{Client:ip} {_:word} {User:word} [{Time:time}] {Request:quoted} {Status:int} {Bytes:word}{? {Referer:quoted} {User agent:quoted}?}"},
    {"9e418fa196d0f58da4f4e983e1717da69b89d4c6", // logback
     "{Timestamp:time} [{Thread}] {Level:level} {Logger:word} -{?-?} {Message}"},
    {"feae0a5e0029979e82b426b885395d89c2cc81aa", // nginx-error
     "{Timestamp:time} [{Level:of(debug|info|notice|warn|error|crit|alert|emerg)}] {PID:int}#{TID:int}: {?*{Connection:int} ?}{Message}"},
    {"ce3b3fb6f7dce1bf516093e4549135054cb5c7db", // python-basicconfig
     "{Level:of(CRITICAL|ERROR|WARNING|INFO|DEBUG)}:{Logger}:{Message}"},
    {"f30b20be2c4c0c7d42ccc79fc6630dedacf8a81a", // python-logging
     "{Timestamp:time} - {Logger} - {Level:level} - {Message}"},
    {"f6fcf893806b0848d04ac40d04919947c6d1c97a", // serilog-console
     "[{Timestamp:time} {Level:level}] {Message}"},
    {"9ddf04bf5da24cbd9109b016f04f4db6a307b2ad", // serilog-file
     "{Timestamp:time} [{Level:level}] {Message}"},
    {"d10eac992b2acf6ed5fd7469262f9cbff53e2518", // spring-boot
     "{Timestamp:time} {Level:level} {PID:int} --- {?[{_}] ?}[{Thread}] {Logger:word} : {Message}"},
    {"7ba5541fb45ae8d791ed955284c48718da449046", // syslog
     "{Timestamp:time} {Host:word} {Program:word}{?[{PID:int}]?}: {Message}"},
    {"70d32622d37df1273acf450ed09c98cf8c6886e9", // syslog
     "{Timestamp:time} {Host:word} {Program:word}{?[{PID:int}]?}: {Message}"},
};

QString builtinTemplateFor(const QString& definition)
{
    v3::Definition def;
    if (!v3::deserializeDefinition(definition, &def, true, false))
        return QString();
    const QString canonical = v3::serializeDefinition(def);
    const QByteArray hash = QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha1).toHex();
    for (const BuiltinV3& b : kBuiltinV3) {
        if (*b.templateText && hash == b.sha1)
            return QString::fromUtf8(b.templateText);
    }
    return QString();
}

namespace SchemaConverter {

static Result fromDefinition(const v3::Definition& definition)
{
    Result result;
    QString why;
    if (!v3::validateDefinition(definition, &why)) {
        result.error = why;
        return result;
    }

    using K = v3::Block::MatchKind;
    QVector<Piece> pieces;
    if (!definition.linePrefix.isEmpty())
        pieces.append(isWhitespaceOnly(definition.linePrefix) ? space() : lit(definition.linePrefix));

    const int count = int(definition.blocks.size());
    for (int i = 0; i < count; ++i) {
        const v3::Block& b = definition.blocks[i];
        const bool last = i == count - 1;
        if (i > 0 || !definition.linePrefix.isEmpty())
            pieces.append(gap());

        if (!b.leadingText.isEmpty()) {
            if (isWhitespaceOnly(b.leadingText)) {
                pieces.append(space());
            } else {
                pieces.append(lit(b.leadingText.trimmed()));
                pieces.append(gap());
            }
        }

        // Значение.
        const bool field = v3::blockCreatesField(b);
        if (b.matchKind == K::ConstantText && !field) {
            pieces.append(lit(b.customRegex));
        } else {
            Piece f;
            f.type = Piece::Type::Field;
            f.block = i;
            f.name = field ? b.name.trimmed() : QStringLiteral("_");
            if (field && (f.name.contains(QLatin1Char(':')) || f.name.contains(QLatin1Char('{'))
                          || f.name.contains(QLatin1Char('}')) || f.name == QLatin1String("_")
                          || f.name.startsWith(QLatin1Char('?')))) {
                // Имя колонки — контракт с правилами фильтров: молча не меняется.
                result.error = QStringLiteral("Block %1: the field name “%2” cannot be written in a "
                                              "template (“:”, a brace, “_” or a leading “?”).")
                                   .arg(i + 1).arg(f.name);
                return result;
            }
            const bool open = b.separator.isEmpty() && b.closingText.isEmpty();
            switch (b.matchKind) {
            case K::ConstantText:
                f.kind = QStringLiteral("of(") + escapeOfValue(b.customRegex) + QLatin1Char(')');
                break;
            case K::Timestamp: f.kind = QStringLiteral("time"); break;
            case K::Level: f.kind = QStringLiteral("level"); break;
            case K::HexText: f.kind = QStringLiteral("hex"); break;
            case K::Integer: f.kind = QStringLiteral("int"); break;
            case K::IpAddress:
                f.kind = QStringLiteral("ip");
                result.notes.append(QStringLiteral("Field “%1”: an IP address no longer takes a port "
                                                   "after it.").arg(f.name));
                break;
            case K::FilePath: f.kind = QStringLiteral("path"); break;
            case K::CustomRegex: f.kind = regexSpec(b.customRegex.trimmed()); break;
            case K::TextUntilSeparator:
                f.kind = (last && open) ? QStringLiteral("rest") : QStringLiteral("text");
                break;
            case K::GreedyTextUntilSeparator:
                f.kind = (last && open) ? QStringLiteral("rest") : QStringLiteral("greedy");
                break;
            case K::Remainder: f.kind = QStringLiteral("rest"); break;
            }
            pieces.append(f);
        }

        if (!b.closingText.isEmpty()) {
            if (isWhitespaceOnly(b.closingText)) {
                pieces.append(space());
            } else {
                pieces.append(gap());
                pieces.append(lit(b.closingText.trimmed()));
            }
        }

        if (!b.separator.isEmpty()) {
            const bool optional = v3::blockIsSelfDelimiting(b);
            QVector<Piece> sep;
            bool translated = true;
            if (isWhitespaceOnly(b.separator)) {
                // Пробельный разделитель (и регексом « ») — пробел шаблона:
                // [ \t]*(?: ) прежнего генератора требует хотя бы один.
                sep.append(space());
            } else if (b.separatorIsRegex) {
                const std::optional<QVector<Piece>> t = translateRegex(b.separator.trimmed());
                if (t)
                    sep = *t;
                else
                    translated = false;
            } else {
                sep.append(lit(b.separator.trimmed()));
            }
            if (translated && optional && hasOptional(sep))
                translated = false;   // «{?…?}» не вкладываются
            if (!translated) {
                // Тот же регекс полем без имени, как его ставил прежний генератор.
                const QString body = b.separatorIsRegex
                    ? QStringLiteral("[ \\t]*(?:") + b.separator.trimmed() + QLatin1Char(')')
                    : QStringLiteral("[ \\t]*") + QRegularExpression::escape(b.separator.trimmed());
                Piece f;
                f.type = Piece::Type::Field;
                f.name = QStringLiteral("_");
                f.kind = regexSpec(optional ? QStringLiteral("(?:") + body + QStringLiteral(")?") : body);
                pieces.append(f);
            } else {
                pieces.append(gap());
                const QVector<Piece> inner = normalized(sep, false);
                const bool blankOnly = std::all_of(inner.cbegin(), inner.cend(), [](const Piece& p) {
                    return p.type == Piece::Type::Space || p.type == Piece::Type::Gap;
                });
                if (optional && !blankOnly) {
                    // Пробел внутри разделителя (« - ») становится частью участка.
                    pieces.append(opt(inner));
                } else if (optional) {
                    // Необязательный пробельный разделитель — тот же промежуток.
                } else {
                    for (const Piece& p : sep)
                        pieces.append(p);
                }
            }
        }
    }

    pieces = normalized(pieces, true);

    // Text без определённого конца (следом пробел и поле без формы) — word.
    for (int attempt = 0; attempt <= count; ++attempt) {
        QVector<std::pair<int, int>> positions;
        const QString text = render(pieces, &positions);
        const LineTemplate tpl(text);
        if (tpl.isValid()) {
            result.ok = true;
            result.templateText = text;
            return result;
        }
        const LineTemplateError& err = tpl.error();
        int block = -1;
        for (const auto& [pos, b] : positions) {
            if (pos == err.position)
                block = b;
        }
        bool fixed = false;
        if (err.code == LineTemplateError::Code::UndefinedEnd && block >= 0) {
            for (Piece& p : pieces) {
                if (p.type == Piece::Type::Field && p.block == block
                    && (p.kind == QLatin1String("text") || p.kind == QLatin1String("greedy"))) {
                    p.kind = QStringLiteral("word");
                    result.notes.append(QStringLiteral("Field “%1”: its end is not defined by the "
                                                       "next block — it is one word now.").arg(p.name));
                    fixed = true;
                }
            }
        }
        if (!fixed) {
            result.error = err.message;
            result.templateText = text;
            return result;
        }
    }
    result.error = QStringLiteral("The schema could not be converted.");
    return result;
}

bool isBlockDefinition(const QString& definition)
{
    const QString trimmed = definition.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        return root.value(QStringLiteral("version")).toInt(0) <= 3
            && root.value(QStringLiteral("blocks")).isArray();
    }
    if (!doc.isNull() || trimmed.startsWith(QLatin1Char('{')))
        return false;
    // Строка-паттерн самых старых версий: «%d [%t] %p - %m».
    v3::Definition def;
    return v3::parseLegacyConversionPattern(trimmed, &def);
}

Result fromBlocks(const QString& definition)
{
    v3::Definition def;
    if (!isBlockDefinition(definition)
        || !v3::deserializeDefinition(definition, &def, /*allowLegacy=*/true, /*strict=*/false)) {
        Result result;
        result.error = QStringLiteral("Not a schema of the previous format.");
        return result;
    }
    return fromDefinition(def);
}

Result translate(const QString& definition)
{
    const QString builtin = builtinTemplateFor(definition);
    if (!builtin.isEmpty()) {
        Result result;
        result.ok = true;
        result.templateText = builtin;
        return result;
    }
    return fromBlocks(definition);
}

QString upgradeDefinition(const QString& definition, bool* converted)
{
    if (converted)
        *converted = false;
    if (!isBlockDefinition(definition))
        return definition;
    if (converted)
        *converted = true;
    if (isRetiredKeyedDefinition(definition))
        return QString();
    const Result result = translate(definition);
    // Не переводится (битое определение) — как есть: схема без полей, а
    // сохранённое состояние не теряет того, что в нём было.
    return result.ok ? LineTemplateSchema::definition(result.templateText) : definition;
}

bool canonicalBlocks(const QString& definition, QString* canonical, bool strict)
{
    v3::Definition def;
    if (!v3::deserializeDefinition(definition, &def, /*allowLegacy=*/true, strict))
        return false;
    if (canonical)
        *canonical = v3::serializeDefinition(def);
    return true;
}

bool isRetiredKeyedDefinition(const QString& definition)
{
    if (!definition.contains(QLatin1String("\"keyed\"")))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(definition.toUtf8());
    return doc.isObject() && doc.object().contains(QStringLiteral("keyed"));
}

Result fromGrok(const QString& grokExpression)
{
    v3::Definition def;
    QString warnings;
    if (!v3::definitionFromGrok(grokExpression, &def, &warnings)) {
        Result result;
        result.error = QStringLiteral("The expression has no %{…} patterns.");
        return result;
    }
    Result result = fromDefinition(def);
    if (!warnings.isEmpty())
        result.notes.prepend(warnings);
    return result;
}

} // namespace SchemaConverter
