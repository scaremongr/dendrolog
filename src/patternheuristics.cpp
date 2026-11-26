#include "patternheuristics.h"

#include "logentry.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace PatternHeuristics {


// ---------------------------------------------------------------- //
// Canonical token sub-regexes
// ---------------------------------------------------------------- //

QString timestampRegex()
{
    // Optional date (ISO "2024-01-31", EU "31.01.2024", slashed) + time
    // "12:34:56" with optional fraction and a zone glued to it. Date alone
    // is not a timestamp. A zone after a space is left to the next block.
    //
    // Two forms with a month name come first: the access log
    // "18/Nov/2026:13:55:36 +0000" (its zone sits inside the brackets, so it
    // belongs to the block) and the syslog "Nov 18 13:55:36" / "Nov  8 …".
    static const QString re =
        QStringLiteral(R"((?:\d{2}/)") + monthNameRegex()
        + QStringLiteral(R"(/\d{4}:\d{2}:\d{2}:\d{2}(?: [+-]\d{4})?)"
                         R"(|)") + monthNameRegex()
        + QStringLiteral(R"( (?: \d|\d{1,2}) \d{2}:\d{2}:\d{2}(?:[.,]\d{1,9})?)"
                         R"(|(?:\d{4}[-/.]\d{2}[-/.]\d{2}[T ]|\d{2}[-/.]\d{2}[-/.]\d{4}[ ])?\d{1,2}:\d{2}:\d{2}(?:[.,]\d{1,9})?(?:[Zz]|[+-]\d{2}(?::?\d{2})?)?))");
    return re;
}

namespace {

// Альтернативы из словаря kLevelWords: "[Ff][Aa][Tt][Aa][Ll]|…". Регистр
// сворачивается здесь же и только для ASCII — опция CaseInsensitive в
// UTF-режиме приравняла бы «ſ» к S и знак Кельвина к K, а ручной сканер
// LineClassifier считает их разделителями. Порядок альтернатив не важен: с
// обеих сторон стоит \b, поэтому ERR не «съедает» начало ERROR.
const QString& levelAlternation()
{
    static const QString alternation = [] {
        QString alt;
        for (const LevelWord& w : kLevelWords) {
            if (!alt.isEmpty())
                alt += QLatin1Char('|');
            for (const char* c = w.word; *c; ++c) {
                const QChar upper = QLatin1Char(*c);
                alt += QLatin1Char('[');
                alt += upper;
                alt += upper.toLower();
                alt += QLatin1Char(']');
            }
        }
        return alt;
    }();
    return alternation;
}

} // namespace

QString levelRegex()
{
    return QStringLiteral(R"(\b(?:)") + levelAlternation() + QStringLiteral(R"()\b)");
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

QString timestampSuffixDetectPattern()
{
    return QStringLiteral(
        R"(([.,]\d+)?)"
        R"((?:([Zz])(?![A-Za-z0-9_]))"
        R"(|([+-]\d{2})(?::?(\d{2}))?(?!\d))"
        R"(| ([+-]\d{2}):?(\d{2})(?!\d))"
        R"(| (UTC|GMT)(?![A-Za-z0-9_+-]))?)");
}

QString monthNameRegex()
{
    // Регистр сворачивается в классах и только для ASCII — как у слов уровня.
    static const QString alternation = [] {
        static const char* const kMonths[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        QString alt;
        for (const char* m : kMonths) {
            if (!alt.isEmpty())
                alt += QLatin1Char('|');
            for (const char* c = m; *c; ++c) {
                const QChar upper = QLatin1Char(*c);
                alt += QLatin1Char('[');
                alt += upper;
                alt += upper.toLower();
                alt += QLatin1Char(']');
            }
        }
        return QStringLiteral("(?:") + alt + QLatin1Char(')');
    }();
    return alternation;
}

QString isoTimestampDetectPattern()
{
    return QStringLiteral(R"((\d{4}(?:-\d{2}-\d{2}|/\d{2}/\d{2})[ T]\d{2}:\d{2}:\d{2}))")
         + timestampSuffixDetectPattern();
}

QString accessTimestampDetectPattern()
{
    return QStringLiteral(R"((\d{2})/()") + monthNameRegex()
         + QStringLiteral(R"()/(\d{4}):(\d{2}):(\d{2}):(\d{2}))")
         + timestampSuffixDetectPattern();
}

QString syslogTimestampDetectPattern()
{
    return QStringLiteral(R"(^()") + monthNameRegex()
         + QStringLiteral(R"() ( ?\d|\d{2}) (\d{2}):(\d{2}):(\d{2}))")
         + timestampSuffixDetectPattern();
}

QString clockTimestampDetectPattern()
{
    return QStringLiteral(R"(^\[?(\d{2}):(\d{2}):(\d{2})(?![\d:])([.,]\d+)?)");
}

QString timestampAtDetectPattern()
{
    // Формы — по порядку; ни две из них не начинаются одинаково (у ISO
    // третий символ — цифра, у access и фолбэк-форм — «/» или «.», у
    // времени без даты — «:», у syslog — буква), поэтому порядок альтернатив
    // не меняет совпадения.
    static const QString re = QStringLiteral(
        R"((?:\d{4}(?:-\d{2}-\d{2}|/\d{2}/\d{2})[ T]\d{2}:\d{2}:\d{2})")
        + timestampSuffixDetectPattern()
        + QStringLiteral(R"(|\d{2}/)") + monthNameRegex()
        + QStringLiteral(R"(/\d{4}:\d{2}:\d{2}:\d{2})") + timestampSuffixDetectPattern()
        + QStringLiteral(R"(|\d{2}(?:/\d{2}/|\.\d{2}\.)\d{4} \d{2}:\d{2}:\d{2})")
        + timestampSuffixDetectPattern()
        + QStringLiteral(R"(|)") + monthNameRegex()
        + QStringLiteral(R"( (?: ?\d|\d{2}) \d{2}:\d{2}:\d{2})") + timestampSuffixDetectPattern()
        + QStringLiteral(R"(|\d{2}:\d{2}:\d{2}(?![\d:])(?:[.,]\d+)?))");
    return re;
}

QString levelDetectPattern()
{
    return QStringLiteral(R"(\b()") + levelAlternation() + QStringLiteral(R"()\b)");
}

// ---------------------------------------------------------------- //
// Ключевая схема по образцам (F-4, PR 4)
// ---------------------------------------------------------------- //

namespace {

QString pairKey(QStringView line, const KeyedLine::Pair& pair)
{
    const QStringView key = line.mid(pair.keyStart, pair.keyLength);
    return pair.keyEscaped ? KeyedLine::unescape(key) : key.toString();
}

// Ключи верхнего уровня строки (повторный — один раз). objects — ключи,
// чьё значение — вложенный объект.
QStringList topLevelKeys(QStringView line, KeyedDefinition::Format format,
                         QSet<QString>* objects = nullptr)
{
    QStringList keys;
    KeyedLine::PairReader reader(line, format);
    KeyedLine::Pair pair;
    while (reader.next(&pair)) {
        const QString key = pairKey(line, pair);
        if (key.isEmpty() || keys.contains(key))
            continue;
        keys.append(key);
        if (objects && pair.kind == KeyedLine::ValueKind::Object)
            objects->insert(key);
    }
    return keys;
}

KeyedDefinition::Format keyedFormat(LineShape format)
{
    return format == LineShape::Logfmt ? KeyedDefinition::Format::Logfmt
                                        : KeyedDefinition::Format::JsonObject;
}

LineShape lineFormatOf(KeyedDefinition::Format format)
{
    return format == KeyedDefinition::Format::Logfmt ? LineShape::Logfmt : LineShape::Json;
}

} // namespace

LineShape detectLineFormat(QStringView line)
{
    if (line.isEmpty())
        return LineShape::Blocks;
    KeyedLine::Pair pair;
    if (line.front() == QLatin1Char('{')) {
        KeyedLine::PairReader reader(line, KeyedDefinition::Format::JsonObject);
        return reader.next(&pair) ? LineShape::Json : LineShape::Blocks;
    }
    KeyedLine::PairReader reader(line, KeyedDefinition::Format::Logfmt);
    if (!reader.structured())
        return LineShape::Blocks;
    int pairs = 0;
    while (pairs < 2 && reader.next(&pair))
        ++pairs;
    return pairs >= 2 ? LineShape::Logfmt : LineShape::Blocks;
}

QStringList keyedSampleKeys(const QStringList& sampleLines, const KeyedDefinition& definition,
                            double minShare)
{
    const LineShape wanted = lineFormatOf(definition.format);
    QSet<QString> excluded;   // точные ключи ролей и колонок
    for (const QStringList* list : {&definition.timeKeys, &definition.levelKeys,
                                    &definition.messageKeys, &definition.extraColumns})
        for (const QString& key : *list)
            excluded.insert(key);
    QSet<QString> takenNames;  // имена колонок без учёта регистра
    for (const QString& name : definition.columnNames())
        takenNames.insert(name.trimmed().toLower());
    // Первый сегмент пути ключа роли («log» у «log.level»): вложенный объект
    // под ним — место роли, колонкой он повторил бы её.
    QSet<QString> rolePathHeads;
    for (const QStringList* list : {&definition.timeKeys, &definition.levelKeys,
                                    &definition.messageKeys})
        for (const QString& key : *list)
            if (key.contains(QLatin1Char('.')))
                rolePathHeads.insert(key.section(QLatin1Char('.'), 0, 0));
    QSet<QString> objectKeys;

    QStringList order;
    QHash<QString, int> counts;
    int lines = 0;
    for (const QString& line : sampleLines) {
        if (detectLineFormat(line) != wanted)
            continue;
        ++lines;
        for (const QString& key : topLevelKeys(line, definition.format, &objectKeys)) {
            if (!counts.contains(key))
                order.append(key);
            ++counts[key];
        }
    }

    QStringList result;
    for (const QString& key : order) {
        // Ключ с запятой или пробелом по краям редактор (список через
        // запятую) не запишет — не предлагать.
        if (excluded.contains(key) || key.trimmed() != key || key.isEmpty()
                || key.contains(QLatin1Char(',')))
            continue;
        if (rolePathHeads.contains(key) && objectKeys.contains(key))
            continue;
        if (counts.value(key) <= minShare * lines)
            continue;
        const QString folded = key.trimmed().toLower();
        if (takenNames.contains(folded))
            continue;
        takenNames.insert(folded);
        result.append(key);
    }
    return result;
}

bool suggestKeyedDefinition(const QStringList& sampleLines, KeyedDefinition* definition)
{
    int nonEmpty = 0, json = 0, logfmt = 0;
    for (const QString& line : sampleLines) {
        if (line.trimmed().isEmpty())
            continue;
        ++nonEmpty;
        switch (detectLineFormat(line)) {
        case LineShape::Json:   ++json; break;
        case LineShape::Logfmt: ++logfmt; break;
        case LineShape::Blocks: break;
        }
    }
    const LineShape format = json * 2 > nonEmpty ? LineShape::Json
        : logfmt * 2 > nonEmpty ? LineShape::Logfmt
        : LineShape::Blocks;
    if (format == LineShape::Blocks)
        return false;

    KeyedDefinition keyed;
    keyed.format = keyedFormat(format);
    // Ключ роли в другом регистре («Timestamp», «LogLevel», «Message» у
    // .NET) — ключ этой роли: колонка с именем роли была бы повтором.
    // Короче трёх символов — не повышается («T» — не «t»).
    for (const QString& line : sampleLines) {
        if (detectLineFormat(line) != format)
            continue;
        for (const QString& key : topLevelKeys(line, keyed.format)) {
            if (key.size() < 3)
                continue;
            for (QStringList* roleKeys : {&keyed.timeKeys, &keyed.levelKeys, &keyed.messageKeys}) {
                if (roleKeys->contains(key))
                    break;
                if (roleKeys->contains(key, Qt::CaseInsensitive)) {
                    roleKeys->append(key);
                    break;
                }
            }
        }
    }
    keyed.extraColumns = keyedSampleKeys(sampleLines, keyed, 0.5);
    *definition = keyed;
    return true;
}

} // namespace PatternHeuristics
