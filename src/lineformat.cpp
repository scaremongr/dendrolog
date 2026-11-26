#include "lineformat.h"

#include "ansiescape.h"
#include "gzipdevice.h"
#include "lineclassifier.h"
#include "patternheuristics.h"
#include "textencoding.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringDecoder>

namespace {

const QLatin1String kFieldPrefix("dendrolog-line-format:");

QJsonObject keysToJson(const KeyedDefinition& k, bool withColumns)
{
    QJsonObject o;
    o.insert(QStringLiteral("timeKeys"), QJsonArray::fromStringList(k.timeKeys));
    o.insert(QStringLiteral("levelKeys"), QJsonArray::fromStringList(k.levelKeys));
    o.insert(QStringLiteral("messageKeys"), QJsonArray::fromStringList(k.messageKeys));
    if (withColumns)
        o.insert(QStringLiteral("extraColumns"), QJsonArray::fromStringList(k.extraColumns));
    o.insert(QStringLiteral("epochUnit"), KeyedDefinition::epochUnitName(k.epochUnit));
    o.insert(QStringLiteral("numericLevels"), KeyedDefinition::numericLevelsName(k.numericLevels));
    return o;
}

bool stringList(const QJsonValue& v, QStringList* out)
{
    if (v.isUndefined())
        return true;
    if (!v.isArray())
        return false;
    QStringList list;
    for (const QJsonValue& item : v.toArray()) {
        if (!item.isString())
            return false;
        list << item.toString();
    }
    *out = list;
    return true;
}

bool keysFromJson(const QJsonObject& o, KeyedDefinition* k)
{
    if (!stringList(o.value(QStringLiteral("timeKeys")), &k->timeKeys)
        || !stringList(o.value(QStringLiteral("levelKeys")), &k->levelKeys)
        || !stringList(o.value(QStringLiteral("messageKeys")), &k->messageKeys)
        || !stringList(o.value(QStringLiteral("extraColumns")), &k->extraColumns))
        return false;
    const QJsonValue unit = o.value(QStringLiteral("epochUnit"));
    if (!unit.isUndefined()
        && (!unit.isString() || !KeyedDefinition::epochUnitFromName(unit.toString(), &k->epochUnit)))
        return false;
    const QJsonValue levels = o.value(QStringLiteral("numericLevels"));
    if (!levels.isUndefined()
        && (!levels.isString()
            || !KeyedDefinition::numericLevelsFromName(levels.toString(), &k->numericLevels)))
        return false;
    return true;
}

KeyedDefinition::Format keyedFormatOf(LineFormat::Kind kind)
{
    return kind == LineFormat::Kind::Logfmt ? KeyedDefinition::Format::Logfmt
                                            : KeyedDefinition::Format::JsonObject;
}

bool kindFromName(const QString& name, LineFormat::Kind* kind)
{
    for (LineFormat::Kind k : {LineFormat::Kind::Plain, LineFormat::Kind::Json, LineFormat::Kind::Logfmt}) {
        if (LineFormat::kindName(k) == name) {
            *kind = k;
            return true;
        }
    }
    return false;
}

// Ведущее слово уровня: с позиции 0 или 1 за «[».
bool leadingLevel(QStringView line)
{
    if (LineClassifier::levelLengthAt(line, 0) > 0)
        return true;
    return line.size() > 1 && line.front() == QLatin1Char('[')
        && LineClassifier::levelLengthAt(line, 1) > 0;
}

} // namespace

// ============================================================================
// LineFormat
// ============================================================================

LineFormat LineFormat::plain(Origin origin)
{
    LineFormat f;
    f.origin = origin;
    return f;
}

LineFormat LineFormat::structured(Kind kind, KeyedDefinition keys, Origin origin)
{
    LineFormat f;
    f.kind = kind;
    f.keys = std::move(keys);
    f.keys.format = keyedFormatOf(kind);
    f.origin = origin;
    return f;
}

QString LineFormat::readingSignature() const
{
    if (kind == Kind::Plain)
        return levelStartsRecord ? QStringLiteral("plain+level") : QString();
    QJsonObject o = keysToJson(keys, /*withColumns=*/false);
    o.insert(QStringLiteral("kind"), kindName(kind));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString LineFormat::displayName() const
{
    switch (kind) {
    case Kind::Json:   return QStringLiteral("JSON Lines");
    case Kind::Logfmt: return QStringLiteral("logfmt");
    case Kind::Plain:  break;
    }
    return QStringLiteral("Plain text");
}

QString LineFormat::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Json:   return QStringLiteral("json");
    case Kind::Logfmt: return QStringLiteral("logfmt");
    case Kind::Plain:  break;
    }
    return QStringLiteral("plain");
}

QString LineFormat::fieldDefinition() const
{
    if (!isStructured())
        return QString();
    QJsonObject o = keysToJson(keys, /*withColumns=*/true);
    o.insert(QStringLiteral("kind"), kindName(kind));
    return kFieldPrefix + QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

bool LineFormat::isFieldDefinition(const QString& definition)
{
    return definition.startsWith(kFieldPrefix);
}

bool LineFormat::keysFromFieldDefinition(const QString& definition, KeyedDefinition* keys)
{
    if (!isFieldDefinition(definition))
        return false;
    const QJsonDocument doc =
        QJsonDocument::fromJson(definition.mid(kFieldPrefix.size()).toUtf8());
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    Kind kind = Kind::Plain;
    if (!kindFromName(o.value(QStringLiteral("kind")).toString(), &kind) || kind == Kind::Plain)
        return false;
    KeyedDefinition k;
    if (!keysFromJson(o, &k))
        return false;
    k.format = keyedFormatOf(kind);
    *keys = k;
    return true;
}

QJsonObject LineFormat::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), kindName(kind));
    if (kind == Kind::Plain) {
        if (levelStartsRecord)
            o.insert(QStringLiteral("levelStartsRecord"), true);
    } else {
        const QJsonObject k = keysToJson(keys, /*withColumns=*/true);
        for (auto it = k.begin(); it != k.end(); ++it)
            o.insert(it.key(), it.value());
    }
    return o;
}

bool LineFormat::fromJson(const QJsonObject& json, LineFormat* out)
{
    Kind kind = Kind::Plain;
    const QJsonValue kv = json.value(QStringLiteral("kind"));
    if (!kv.isString() || !kindFromName(kv.toString(), &kind))
        return false;
    LineFormat f;
    f.kind = kind;
    f.origin = Origin::User;
    if (kind == Kind::Plain) {
        const QJsonValue level = json.value(QStringLiteral("levelStartsRecord"));
        if (!level.isUndefined() && !level.isBool())
            return false;
        f.levelStartsRecord = level.toBool(false);
    } else {
        if (!keysFromJson(json, &f.keys))
            return false;
        f.keys.format = keyedFormatOf(kind);
    }
    *out = f;
    return true;
}

LineFormatReader::LineFormatReader(const LineFormat& f)
    : format(f)
{
    if (format.isStructured()) {
        format.keys.format = keyedFormatOf(format.kind);
        lookup = KeyedLine::Lookup(format.keys);
    }
}

// ============================================================================
// LineFormatDetector
// ============================================================================

namespace LineFormatDetector {

LineFormat decide(const QStringList& lines, bool endOfFile)
{
    int nonEmpty = 0;
    for (const QString& line : lines)
        if (!line.trimmed().isEmpty())
            ++nonEmpty;
    if (endOfFile && nonEmpty < kMinLines)
        return LineFormat();   // не решено: читается как Plain

    KeyedDefinition keys;
    if (PatternHeuristics::suggestKeyedDefinition(lines, &keys)) {
        return LineFormat::structured(keys.format == KeyedDefinition::Format::Logfmt
                                          ? LineFormat::Kind::Logfmt : LineFormat::Kind::Json,
                                      keys, LineFormat::Origin::Auto);
    }

    LineFormat f = LineFormat::plain(LineFormat::Origin::Auto);
    if (nonEmpty > 0) {
        const LineClassifier classifier;
        int stamped = 0, levelled = 0;
        for (const QString& line : lines) {
            if (line.trimmed().isEmpty())
                continue;
            qint64 ms = 0;
            if (classifier.detectTimestampMs(line, ms))
                ++stamped;
            if (leadingLevel(line))
                ++levelled;
        }
        f.levelStartsRecord = stamped * 10 < nonEmpty && levelled * 2 >= nonEmpty;
    }
    return f;
}

QStringList sampleDevice(QIODevice& device, bool* endOfFile, const std::atomic_bool* abort)
{
    QStringList lines;
    bool eof = false;
    QByteArray head = device.peek(4);
    std::optional<TextEncoding> wide;
    if (const auto bom = TextEncodings::detectBom(head)) {
        device.read(bom->length);
        if (bom->encoding != TextEncoding::Utf8)
            wide = bom->encoding;
    }
    QByteArray bytes;
    while (bytes.size() < kSampleBytes) {
        if (abort && abort->load(std::memory_order_relaxed))
            break;
        const QByteArray chunk = device.read(qMin<qint64>(64 * 1024, kSampleBytes - bytes.size()));
        if (chunk.isEmpty()) {
            eof = true;
            break;
        }
        bytes += chunk;
    }
    if (!eof && device.atEnd())
        eof = true;

    QString text;
    if (wide) {
        QStringDecoder decoder(*TextEncodings::toQt(*wide));
        text = decoder.decode(bytes);
    }
    int nonEmpty = 0;
    qsizetype pos = 0;
    const qsizetype size = wide ? text.size() : bytes.size();
    while (pos < size && nonEmpty < kSampleLines) {
        qsizetype end = wide ? text.indexOf(QLatin1Char('\n'), pos) : bytes.indexOf('\n', pos);
        const bool cut = end < 0;
        if (cut) {
            // Строка, оборванная пределом выборки, — не строка (её конец не
            // прочитан); оборванная концом файла — строка.
            if (!eof)
                break;
            end = size;
        }
        QString line;
        if (wide) {
            line = text.mid(pos, end - pos);
            AnsiEscape::stripInPlace(line);
        } else {
            QByteArray raw = bytes.mid(pos, end - pos);
            if (AnsiEscape::hasEscape(raw))
                raw.truncate(AnsiEscape::stripBytes(raw.constData(), raw.size(), raw.data()));
            line = QString::fromUtf8(raw);
        }
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (!line.trimmed().isEmpty())
            ++nonEmpty;
        lines << line;
        pos = end + 1;
    }
    // Выборка кончилась на пределе строк, а не на конце файла.
    if (nonEmpty >= kSampleLines || pos < size)
        eof = false;
    if (endOfFile)
        *endOfFile = eof;
    return lines;
}

QStringList sampleFile(const QString& path, bool* endOfFile, const std::atomic_bool* abort)
{
    if (endOfFile)
        *endOfFile = true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    if (GzipDevice::hasSignature(file.peek(2))) {
        file.close();
        GzipDevice gzip(path);
        if (!gzip.open(QIODevice::ReadOnly))
            return {};
        return sampleDevice(gzip, endOfFile, abort);
    }
    return sampleDevice(file, endOfFile, abort);
}

LineFormat detectInFile(const QString& path, const std::atomic_bool* abort)
{
    bool eof = true;
    const QStringList lines = sampleFile(path, &eof, abort);
    return decide(lines, eof);
}

bool advance(LineFormat& format, const QString& path, const std::atomic_bool* abort)
{
    if (format.isDecided())
        return false;
    const LineFormat decided = detectInFile(path, abort);
    if (!decided.isDecided())
        return false;
    format = decided;
    return true;
}

} // namespace LineFormatDetector
