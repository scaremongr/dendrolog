#include "recordid.h"
#include "ansiescape.h"
#include "tararchive.h"

#include "gzipdevice.h"
#include "spooldir.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>

namespace {

quint64 first8(const QByteArray& digest)
{
    quint64 v = 0;
    for (int i = 0; i < 8 && i < digest.size(); ++i)
        v = (v << 8) | quint8(digest.at(i));
    return v;
}

// ASCII-скелет строки: единицы < 0x80 как есть, серия единиц ≥ 0x80 — один
// заполнитель 0x80. Единица — символ UTF-16 (текст) или байт (сырые байты):
// у ASCII-совместимой тотальной кодировки скелеты совпадают (recordid.h).
template <typename Unit>
quint64 skeletonFingerprint(const Unit* data, qsizetype len)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    char buf[4096];
    int used = 0;
    bool inRun = false;
    for (qsizetype i = 0; i < len; ++i) {
        const auto u = static_cast<std::make_unsigned_t<Unit>>(data[i]);
        if (u < 0x80) {
            buf[used++] = char(u);
            inRun = false;
        } else if (!inRun) {
            buf[used++] = char(0x80);
            inRun = true;
        } else {
            continue;
        }
        if (used == int(sizeof buf)) {
            hash.addData(QByteArrayView(buf, used));
            used = 0;
        }
    }
    hash.addData(QByteArrayView(buf, used));
    return first8(hash.result());
}

void setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
}

// Целое из JSON: число без дробной части в [min, 2^53]. nullopt — не так.
std::optional<qint64> integerField(const QJsonObject& json, const char* key, qint64 min)
{
    const QJsonValue v = json.value(QLatin1String(key));
    if (!v.isDouble())
        return std::nullopt;
    constexpr qint64 kMaxExact = qint64(1) << 53;
    const qint64 sentinel = std::numeric_limits<qint64>::min();
    const qint64 n = v.toInteger(sentinel);
    if (n == sentinel || n < min || n > kMaxExact)
        return std::nullopt;
    return n;
}

// Заголовок формата: версия и имя алгоритма. false — чужой формат.
bool checkFormat(const QJsonObject& json, int version, const char* algorithm, QString* error)
{
    const std::optional<qint64> got = integerField(json, "version", 0);
    if (!got || *got != version) {
        setError(error, QStringLiteral("unsupported format version %1")
                            .arg(json.value(QLatin1String("version")).toVariant().toString()));
        return false;
    }
    const QString hash = json.value(QLatin1String("hash")).toString();
    if (hash != QLatin1String(algorithm)) {
        setError(error, QStringLiteral("unsupported hash algorithm \"%1\"").arg(hash));
        return false;
    }
    return true;
}

std::optional<quint64> hexField(const QJsonObject& json, const char* key)
{
    const QJsonValue v = json.value(QLatin1String(key));
    if (!v.isString())
        return std::nullopt;
    return RecordFingerprint::fromHex(v.toString());
}

} // namespace

// ---------------------------------------------------------------------------
// RecordFingerprint
// ---------------------------------------------------------------------------

quint64 RecordFingerprint::ofBytes(QByteArrayView bytes)
{
    return first8(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1));
}

std::optional<quint64> RecordFingerprint::ofFileRegion(QFile& file, qint64 from, qint64 len)
{
    if (from < 0 || len < 0 || !file.seek(from))
        return std::nullopt;
    const QByteArray bytes = file.read(len);
    if (bytes.size() != len)
        return std::nullopt;
    return ofBytes(bytes);
}

// Последовательности ANSI (F-3) в отпечаток не входят ни в тексте, ни в
// байтах: текст строки их не содержит (декодер снимает), а сырые байты
// индексного пути — содержат. Без ESC — без копии.
quint64 RecordFingerprint::ofLineText(QStringView text)
{
    if (Q_UNLIKELY(AnsiEscape::hasEscape(text))) {
        QString stripped = text.toString();
        AnsiEscape::stripInPlace(stripped);
        return skeletonFingerprint(stripped.utf16(), stripped.size());
    }
    return skeletonFingerprint(text.utf16(), text.size());
}

quint64 RecordFingerprint::ofLineBytes(QByteArrayView bytes)
{
    if (Q_UNLIKELY(AnsiEscape::hasEscape(bytes))) {
        const QByteArray stripped = AnsiEscape::strip(bytes);
        return skeletonFingerprint(stripped.data(), stripped.size());
    }
    return skeletonFingerprint(bytes.data(), bytes.size());
}

QString RecordFingerprint::toHex(quint64 fingerprint)
{
    return QStringLiteral("%1").arg(fingerprint, 16, 16, QLatin1Char('0'));
}

std::optional<quint64> RecordFingerprint::fromHex(QStringView hex)
{
    if (hex.size() != 16)
        return std::nullopt;
    quint64 v = 0;
    for (const QChar c : hex) {
        const char16_t u = c.unicode();
        int digit;
        if (u >= u'0' && u <= u'9')
            digit = u - u'0';
        else if (u >= u'a' && u <= u'f')
            digit = u - u'a' + 10;
        else if (u >= u'A' && u <= u'F')
            digit = u - u'A' + 10;
        else
            return std::nullopt;
        v = (v << 4) | quint64(digit);
    }
    return v;
}

// ---------------------------------------------------------------------------
// RecordId
// ---------------------------------------------------------------------------

RecordId RecordId::forLine(qint64 line, QStringView lineText, bool complete)
{
    RecordId id;
    id.line = line;
    id.hasText = complete;
    if (complete)
        id.text = RecordFingerprint::ofLineText(lineText);
    return id;
}

bool RecordId::matchesText(QStringView lineText) const
{
    return !hasText || text == RecordFingerprint::ofLineText(lineText);
}

bool RecordId::matchesBytes(QByteArrayView lineBytes) const
{
    return !hasText || text == RecordFingerprint::ofLineBytes(lineBytes);
}

QJsonObject RecordId::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("version"), kFormatVersion);
    json.insert(QStringLiteral("hash"), QLatin1String(RecordFingerprint::kLineAlgorithm));
    json.insert(QStringLiteral("line"), line);
    if (hasText)
        json.insert(QStringLiteral("text"), RecordFingerprint::toHex(text));
    else
        json.insert(QStringLiteral("partial"), true);
    return json;
}

std::optional<RecordId> RecordId::fromJson(const QJsonObject& json, QString* error)
{
    if (!checkFormat(json, kFormatVersion, RecordFingerprint::kLineAlgorithm, error))
        return std::nullopt;
    RecordId id;
    const std::optional<qint64> line = integerField(json, "line", 1);
    if (!line) {
        setError(error, QStringLiteral("bad line number"));
        return std::nullopt;
    }
    id.line = *line;
    // Недописанная строка помечена явно; пропавший отпечаток без метки —
    // битые данные: иначе такой идентификатор совпал бы с любым текстом.
    const QJsonValue partial = json.value(QLatin1String("partial"));
    if (!partial.isUndefined() && !partial.isBool()) {
        setError(error, QStringLiteral("bad partial flag"));
        return std::nullopt;
    }
    if (partial.toBool()) {
        if (json.contains(QLatin1String("text"))) {
            setError(error, QStringLiteral("partial line with a text fingerprint"));
            return std::nullopt;
        }
        return id;
    }
    const std::optional<quint64> text = hexField(json, "text");
    if (!text) {
        setError(error, QStringLiteral("bad text fingerprint"));
        return std::nullopt;
    }
    id.text = *text;
    id.hasText = true;
    return id;
}

// ---------------------------------------------------------------------------
// SavedPath
// ---------------------------------------------------------------------------

namespace SavedPath {

namespace {

bool isSeparator(QChar c)
{
    return c == QLatin1Char('/') || c == QLatin1Char('\\');
}

bool isDriveRoot(QStringView p)
{
    const char16_t letter = p.size() >= 3 ? p[0].unicode() : 0;
    return ((letter >= u'A' && letter <= u'Z') || (letter >= u'a' && letter <= u'z'))
        && p[1] == QLatin1Char(':') && isSeparator(p[2]);
}

// Длина префикса «\\?\» / «\\.\» (и «\\?\UNC\»); 0 — префикса нет.
// *unc — после префикса идёт «server\share».
qsizetype longPrefix(QStringView p, bool* unc)
{
    *unc = false;
    if (p.size() < 4 || !isSeparator(p[0]) || !isSeparator(p[1])
        || (p[2] != QLatin1Char('?') && p[2] != QLatin1Char('.')) || !isSeparator(p[3]))
        return 0;
    const QStringView rest = p.mid(4);
    if (rest.size() >= 4 && rest.first(3).compare(QLatin1String("UNC"), Qt::CaseInsensitive) == 0
        && isSeparator(rest[3])) {
        *unc = true;
        return 8;
    }
    return 4;
}

} // namespace

Style style(QStringView p)
{
    bool unc = false;
    if (const qsizetype prefix = longPrefix(p, &unc)) {
        const QStringView rest = p.mid(prefix);
        if (unc)
            return !rest.isEmpty() && !isSeparator(rest[0]) ? Style::WindowsUnc : Style::None;
        return isDriveRoot(rest) ? Style::WindowsDrive : Style::None;
    }
    if (isDriveRoot(p))
        return Style::WindowsDrive;
    if (p.size() >= 3 && isSeparator(p[0]) && isSeparator(p[1]) && !isSeparator(p[2]))
        return Style::WindowsUnc;
    if (!p.isEmpty() && p[0] == QLatin1Char('/'))
        return Style::Posix;
    return Style::None;
}

bool isNative(Style s)
{
#ifdef Q_OS_WIN
    return isWindows(s);
#else
    return s == Style::Posix;
#endif
}

QString normalized(const QString& path)
{
    if (!isWindows(style(path)))
        return path;
    bool unc = false;
    const qsizetype prefix = longPrefix(path, &unc);
    QString out = unc ? QStringLiteral("//") + path.mid(prefix) : path.mid(prefix);
    out.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return out;
}

QString toDisplay(const QString& path)
{
    if (!isWindows(style(path)))
        return path;
    QString out = path;
    out.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return out;
}

bool same(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    constexpr bool windowsHost = true;
#else
    constexpr bool windowsHost = false;
#endif
    const bool insensitive = windowsHost || isWindows(style(a)) || isWindows(style(b));
    return a.compare(b, insensitive ? Qt::CaseInsensitive : Qt::CaseSensitive) == 0;
}

} // namespace SavedPath

// ---------------------------------------------------------------------------
// SourceIdentity
// ---------------------------------------------------------------------------

std::optional<SourceIdentity> SourceIdentity::capture(const QString& filePath, qint64 size)
{
    if (filePath.isEmpty())
        return std::nullopt;
    SourceIdentity id;
    id.path = QFileInfo(filePath).absoluteFilePath();
    if (SpoolDir::isStreamFile(filePath)) {
        id.ephemeral = true;
        return id;
    }
    if (!QFileInfo(filePath).isFile())
        return std::nullopt;
    // Без QIODevice::Text: отпечатки — по сырым байтам (инвариант 5).
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const qint64 fileSize = file.size();
    if (size < 0)
        size = fileSize;
    if (size > fileSize)
        return std::nullopt;
    // Файл не длиннее 4 КБ: начало и конец — одна область, читается раз.
    const std::optional<quint64> head = RecordFingerprint::ofFileRegion(file, 0, RecordFingerprint::headRegionLength(size));
    const std::optional<quint64> tail = RecordFingerprint::tailRegionStart(size) == 0
        ? head : RecordFingerprint::ofFileRegion(file, RecordFingerprint::tailRegionStart(size), size - RecordFingerprint::tailRegionStart(size));
    if (!head || !tail)
        return std::nullopt;
    id.size = size;
    id.head = *head;
    id.tail = *tail;
    return id;
}

std::optional<SourceIdentity> SourceIdentity::capture(const LogFile& file, qint64 size)
{
    std::optional<SourceIdentity> id = capture(file.filePath, size);
    if (!id || file.source().kind != LogSource::Kind::Archive)
        return id;
    id->path = QFileInfo(file.source().path).absoluteFilePath();
    id->member = file.source().member;
    id->compressed = true;
    id->ephemeral = false;
    if (const auto archive = capture(id->path)) {
        id->archiveSize = archive->size;
        id->archiveHead = archive->head;
        id->archiveTail = archive->tail;
    }
    return id;
}

std::optional<SourceIdentity> SourceIdentity::captureUnread(const LogFile& file)
{
    if (file.source().kind != LogSource::Kind::Archive)
        return std::nullopt;
    const auto archive = capture(file.source().path);
    if (!archive)
        return std::nullopt;
    SourceIdentity id;
    id.path = archive->path;
    id.member = file.source().member;
    id.compressed = true;
    id.size = 0;
    id.head = id.tail = RecordFingerprint::ofBytes(QByteArrayView());
    id.archiveSize = archive->size;
    id.archiveHead = archive->head;
    id.archiveTail = archive->tail;
    return id;
}

SourceIdentity::State SourceIdentity::checkAt(const QString& filePath) const
{
    if (ephemeral)
        return State::Ephemeral;
    if (!compressed)
        return checkContentAt(filePath);
    // Содержимое без распаковки не проверить — проверяется сам архив.
    if (filePath.isEmpty() || SavedPath::isForeign(filePath) || !QFileInfo(filePath).isFile()
        || !(member.isEmpty() ? GzipDevice::fileHasSignature(filePath)
                              : ArchiveFormats::detectKnown(filePath) != ArchiveFormats::Format::None))
        return State::Missing;
    if (archiveSize < 0)
        return State::Valid; // отпечатков архива нет: «есть и gzip»
    SourceIdentity archive;
    archive.size = archiveSize;
    archive.head = archiveHead;
    archive.tail = archiveTail;
    return archive.checkContentAt(filePath);
}

SourceIdentity::State SourceIdentity::checkContentAt(const QString& filePath) const
{
    if (ephemeral)
        return State::Ephemeral;
    // Путь чужой ОС не читается с диска: QFileInfo понял бы его как
    // относительный (или как путь на текущем диске Windows).
    if (filePath.isEmpty() || SavedPath::isForeign(filePath) || !QFileInfo(filePath).isFile())
        return State::Missing;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return State::Missing;
    if (size < 0 || file.size() < size)
        return State::Changed;
    const std::optional<quint64> h = RecordFingerprint::ofFileRegion(file, 0, RecordFingerprint::headRegionLength(size));
    if (!h || *h != head)
        return State::Changed;
    const std::optional<quint64> t = RecordFingerprint::tailRegionStart(size) == 0
        ? h : RecordFingerprint::ofFileRegion(file, RecordFingerprint::tailRegionStart(size), size - RecordFingerprint::tailRegionStart(size));
    if (!t || *t != tail)
        return State::Changed;
    return State::Valid;
}

QJsonObject SourceIdentity::toJson() const
{
    if (ephemeral || !isValid())
        return {};
    QJsonObject json;
    json.insert(QStringLiteral("version"), kFormatVersion);
    json.insert(QStringLiteral("hash"), QLatin1String(RecordFingerprint::kRegionAlgorithm));
    json.insert(QStringLiteral("path"), path);
    json.insert(QStringLiteral("size"), size);
    json.insert(QStringLiteral("head"), RecordFingerprint::toHex(head));
    json.insert(QStringLiteral("tail"), RecordFingerprint::toHex(tail));
    if (compressed)
        json.insert(QStringLiteral("compressed"), true);
    if (compressed && archiveSize >= 0) {
        QJsonObject archive;
        archive.insert(QStringLiteral("size"), archiveSize);
        archive.insert(QStringLiteral("head"), RecordFingerprint::toHex(archiveHead));
        archive.insert(QStringLiteral("tail"), RecordFingerprint::toHex(archiveTail));
        json.insert(QStringLiteral("archive"), archive);
    }
    if (!member.isEmpty())
        json.insert(QStringLiteral("member"), member);
    return json;
}

std::optional<SourceIdentity> SourceIdentity::fromJson(const QJsonObject& json, QString* error)
{
    if (!checkFormat(json, kFormatVersion, RecordFingerprint::kRegionAlgorithm, error))
        return std::nullopt;
    SourceIdentity id;
    const QJsonValue path = json.value(QLatin1String("path"));
    // Абсолютный — по правилу SavedPath, а не этой платформы: файл мог
    // приехать с другой ОС (R-8).
    if (!path.isString() || !SavedPath::isAbsolute(path.toString())) {
        setError(error, QStringLiteral("bad path (must be absolute)"));
        return std::nullopt;
    }
    id.path = SavedPath::normalized(path.toString());
    const std::optional<qint64> size = integerField(json, "size", 0);
    if (!size) {
        setError(error, QStringLiteral("bad size"));
        return std::nullopt;
    }
    id.size = *size;
    const std::optional<quint64> head = hexField(json, "head");
    const std::optional<quint64> tail = hexField(json, "tail");
    if (!head || !tail) {
        setError(error, QStringLiteral("bad region fingerprint"));
        return std::nullopt;
    }
    id.head = *head;
    id.tail = *tail;
    // F-2: необязательные поля сжатого источника (версия формата та же).
    const QJsonValue compressed = json.value(QLatin1String("compressed"));
    if (!compressed.isUndefined() && !compressed.isBool()) {
        setError(error, QStringLiteral("bad compressed flag"));
        return std::nullopt;
    }
    id.compressed = compressed.toBool(false);
    const QJsonValue member = json.value(QLatin1String("member"));
    if (!member.isUndefined() && !member.isString()) {
        setError(error, QStringLiteral("bad member"));
        return std::nullopt;
    }
    id.member = member.toString();
    if (!id.member.isEmpty() && !id.compressed) {
        setError(error, QStringLiteral("member without compressed"));
        return std::nullopt;
    }
    const QJsonValue archive = json.value(QLatin1String("archive"));
    if (!archive.isUndefined()) {
        const QJsonObject a = archive.toObject();
        const std::optional<qint64> aSize = archive.isObject() ? integerField(a, "size", 0) : std::nullopt;
        const std::optional<quint64> aHead = archive.isObject() ? hexField(a, "head") : std::nullopt;
        const std::optional<quint64> aTail = archive.isObject() ? hexField(a, "tail") : std::nullopt;
        if (!id.compressed || !aSize || !aHead || !aTail) {
            setError(error, QStringLiteral("bad archive fingerprint"));
            return std::nullopt;
        }
        id.archiveSize = *aSize;
        id.archiveHead = *aHead;
        id.archiveTail = *aTail;
    }
    return id;
}
