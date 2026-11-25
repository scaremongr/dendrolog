#include "tararchive.h"

#include "gzipdevice.h"
#include "ziparchive.h"

#include <QFile>
#include <QIODevice>
#include <QStringConverter>

#include <cstring>

namespace {

constexpr qint64 kBlock = 512;
constexpr qint64 kChunkBytes = 256 * 1024;
constexpr qint64 kMaxMetaBytes = 1024 * 1024; // длинное имя GNU, запись pax

// Числовое поле заголовка: восьмеричное (пробелы и NUL по краям) или
// base-256 (GNU: старший бит первого байта; отрицательные — ошибка).
bool parseNumber(const char* p, int len, qint64& out)
{
    const auto byte = [p](int i) { return static_cast<unsigned char>(p[i]); };
    if (byte(0) & 0x80) {
        if (byte(0) & 0x40)
            return false;
        quint64 v = byte(0) & 0x3f;
        for (int i = 1; i < len; ++i) {
            if (v > (quint64(1) << 54))
                return false; // за 2^62 — не размер файла
            v = (v << 8) | byte(i);
        }
        out = qint64(v);
        return true;
    }
    int i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\0'))
        ++i;
    quint64 v = 0;
    bool digits = false;
    for (; i < len && p[i] >= '0' && p[i] <= '7'; ++i) {
        if (v > (quint64(1) << 59))
            return false;
        v = v * 8 + quint64(p[i] - '0');
        digits = true;
    }
    for (; i < len; ++i)
        if (p[i] != ' ' && p[i] != '\0')
            return false;
    out = digits ? qint64(v) : 0;
    return true;
}

QByteArray field(const char* p, int len)
{
    int n = 0;
    while (n < len && p[n] != '\0')
        ++n;
    return QByteArray(p, n);
}

QString decodeName(const QByteArray& raw)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8.decode(raw);
    if (!utf8.hasError())
        return text;
    return QString::fromLatin1(raw);
}

bool allZero(const char* p, qint64 n)
{
    for (qint64 i = 0; i < n; ++i)
        if (p[i] != '\0')
            return false;
    return true;
}

// Записи pax: «<длина> <ключ>=<значение>\n», длина — всей записи.
void parsePax(const QByteArray& data, QByteArray& path, qint64& size, bool& hasSize)
{
    qsizetype pos = 0;
    while (pos < data.size()) {
        const qsizetype space = data.indexOf(' ', pos);
        if (space < 0)
            return;
        bool ok = false;
        const qsizetype len = data.mid(pos, space - pos).toLongLong(&ok);
        if (!ok || len <= space - pos + 1 || pos + len > data.size() || data.at(pos + len - 1) != '\n')
            return; // битая запись — остальное не читается
        const QByteArray kv = data.mid(space + 1, pos + len - space - 2);
        const qsizetype eq = kv.indexOf('=');
        if (eq > 0) {
            const QByteArray key = kv.left(eq);
            const QByteArray value = kv.mid(eq + 1);
            if (key == "path") {
                path = value;
            } else if (key == "size") {
                bool sizeOk = false;
                const qint64 v = value.toLongLong(&sizeOk);
                if (sizeOk && v >= 0) {
                    size = v;
                    hasSize = true;
                }
            }
        }
        pos += len;
    }
}

} // namespace

struct TarArchive::Impl {
    QFile file;
    std::unique_ptr<GzipDevice> gz;
    QIODevice* in = nullptr;
    const std::atomic_bool* abort = nullptr;
    qint64 remaining = 0;  // непрочитанные данные текущего члена
    qint64 padding = 0;    // и их выравнивание до блока
    bool ended = false;
    bool sawHeader = false;
};

TarArchive::TarArchive(const QString& path)
    : m_path(path), m(std::make_unique<Impl>())
{
}

TarArchive::~TarArchive()
{
    close();
}

void TarArchive::close()
{
    if (m->gz)
        m->gz->close();
    m->gz.reset();
    m->file.close();
    m->in = nullptr;
}

bool TarArchive::fail(Error error, const QString& detail)
{
    m_error = error;
    m_errorString = detail.isEmpty() ? errorText(error) : errorText(error) + QStringLiteral(": ") + detail;
    return false;
}

bool TarArchive::isCompressed() const
{
    return m->gz != nullptr;
}

qint64 TarArchive::archiveSize() const
{
    return m->file.size();
}

qint64 TarArchive::bytesRead() const
{
    return m->gz ? m->gz->compressedBytesRead() : m->file.pos();
}

bool TarArchive::open(const std::atomic_bool* abort)
{
    close();
    m_error = Error::None;
    m_errorString.clear();
    m->abort = abort;
    m->remaining = m->padding = 0;
    m->ended = m->sawHeader = false;
    m->file.setFileName(m_path);
    if (!m->file.open(QIODevice::ReadOnly))
        return fail(Error::ReadError, m->file.errorString());
    if (GzipDevice::hasSignature(m->file.peek(2))) {
        m->gz = std::make_unique<GzipDevice>(&m->file);
        m->gz->setAbortFlag(abort);
        if (!m->gz->open(QIODevice::ReadOnly))
            return fail(Error::Corrupt, m->gz->errorString());
        m->in = m->gz.get();
    } else {
        m->in = &m->file;
    }
    return true;
}

bool TarArchive::readExact(char* data, qint64 n, bool& eofAtStart)
{
    eofAtStart = false;
    qint64 got = 0;
    while (got < n) {
        if (m->abort && m->abort->load(std::memory_order_relaxed))
            return fail(Error::Aborted);
        const qint64 r = m->in->read(data + got, n - got);
        if (r < 0) {
            if (m->gz) {
                if (m->gz->error() == GzipDevice::Error::Aborted)
                    return fail(Error::Aborted);
                return fail(Error::Corrupt, m->gz->errorString());
            }
            return fail(Error::ReadError, m->file.errorString());
        }
        if (r == 0) {
            if (m->gz && m->gz->error() != GzipDevice::Error::None) {
                if (m->gz->error() == GzipDevice::Error::Aborted)
                    return fail(Error::Aborted);
                return fail(Error::Corrupt, m->gz->errorString());
            }
            if (got == 0) {
                eofAtStart = true;
                return false;
            }
            return fail(Error::Corrupt, QStringLiteral("the archive is truncated"));
        }
        got += r;
    }
    return true;
}

bool TarArchive::skipData()
{
    qint64 n = m->remaining + m->padding;
    m->remaining = m->padding = 0;
    if (n == 0)
        return true;
    if (!m->in->isSequential()) {
        const qint64 to = m->file.pos() + n;
        if (to > m->file.size())
            return fail(Error::Corrupt, QStringLiteral("the archive is truncated"));
        if (!m->file.seek(to))
            return fail(Error::ReadError, m->file.errorString());
        return true;
    }
    QByteArray buffer(int(qMin(n, kChunkBytes)), Qt::Uninitialized);
    while (n > 0) {
        const qint64 step = qMin<qint64>(n, buffer.size());
        bool eof = false;
        if (!readExact(buffer.data(), step, eof))
            return eof ? fail(Error::Corrupt, QStringLiteral("the archive is truncated")) : false;
        n -= step;
    }
    return true;
}

bool TarArchive::next(TarMember& member)
{
    if (!m->in || m_error != Error::None || m->ended)
        return false;
    if (!skipData())
        return false;
    QByteArray longName;
    QByteArray paxPath;
    qint64 paxSize = 0;
    bool hasPaxSize = false;
    for (;;) {
        char block[kBlock];
        bool eof = false;
        if (!readExact(block, kBlock, eof)) {
            if (!eof)
                return false;
            if (!m->sawHeader)
                return fail(Error::NotTar);
            if (!longName.isEmpty() || !paxPath.isEmpty() || hasPaxSize)
                return fail(Error::Corrupt, QStringLiteral("the archive is truncated"));
            m->ended = true; // конец на границе заголовка без нулевых блоков
            return false;
        }
        if (allZero(block, kBlock)) {
            m->sawHeader = true;
            m->ended = true;
            return false;
        }
        if (!isHeader(QByteArray::fromRawData(block, kBlock), false))
            return m->sawHeader ? fail(Error::Corrupt, QStringLiteral("invalid header"))
                                : fail(Error::NotTar);
        m->sawHeader = true;
        qint64 size = 0;
        if (!parseNumber(block + 124, 12, size))
            return fail(Error::Corrupt, QStringLiteral("invalid member size"));
        const char type = block[156];
        if (type == 'L' || type == 'K' || type == 'x' || type == 'g') {
            if (size > kMaxMetaBytes)
                return fail(Error::Corrupt, QStringLiteral("an extended header is too long"));
            const qint64 padded = (size + kBlock - 1) / kBlock * kBlock;
            QByteArray raw(int(padded), Qt::Uninitialized);
            if (padded > 0 && !readExact(raw.data(), padded, eof))
                return eof ? fail(Error::Corrupt, QStringLiteral("the archive is truncated")) : false;
            const QByteArray data = raw.left(int(size));
            if (type == 'L')
                longName = field(data.constData(), int(data.size()));
            else if (type == 'x')
                parsePax(data, paxPath, paxSize, hasPaxSize);
            continue; // 'K' (длинная цель ссылки) и 'g' не нужны
        }

        QString name;
        if (!paxPath.isEmpty()) {
            name = QString::fromUtf8(paxPath);
        } else if (!longName.isEmpty()) {
            name = decodeName(longName);
        } else {
            QByteArray raw = field(block, 100);
            // POSIX ustar («ustar\0»): prefix + '/' + name. У GNU («ustar  »)
            // на этом месте другие поля.
            if (std::memcmp(block + 257, "ustar\0", 6) == 0) {
                const QByteArray prefix = field(block + 345, 155);
                if (!prefix.isEmpty())
                    raw = prefix + '/' + raw;
            }
            name = decodeName(raw);
        }
        if (hasPaxSize)
            size = paxSize;

        switch (type) {
        case '0': case '\0': case '7': member.type = TarMember::Type::File; break;
        case '1': member.type = TarMember::Type::HardLink; break;
        case '2': member.type = TarMember::Type::Symlink; break;
        case '3': case '4': member.type = TarMember::Type::Device; break;
        case '5': member.type = TarMember::Type::Directory; break;
        case '6': member.type = TarMember::Type::Fifo; break;
        default: member.type = TarMember::Type::Unsupported; break;
        }
        // Каталог в старом формате — по хвостовой '/' у обычного типа.
        if (member.type == TarMember::Type::File && name.endsWith(QLatin1Char('/')))
            member.type = TarMember::Type::Directory;
        while (name.endsWith(QLatin1Char('/')))
            name.chop(1);
        member.name = name;
        member.size = size;
        m->remaining = size;
        m->padding = (kBlock - size % kBlock) % kBlock;
        return true;
    }
}

bool TarArchive::extract(QIODevice& out, const std::function<bool(qint64 written)>& onChunk)
{
    if (!m->in || m_error != Error::None)
        return false;
    QByteArray buffer(int(qMin<qint64>(qMax<qint64>(m->remaining, 1), kChunkBytes)), Qt::Uninitialized);
    qint64 written = 0;
    while (m->remaining > 0) {
        const qint64 step = qMin<qint64>(m->remaining, buffer.size());
        bool eof = false;
        if (!readExact(buffer.data(), step, eof))
            return eof ? fail(Error::Corrupt, QStringLiteral("the archive is truncated")) : false;
        m->remaining -= step;
        if (out.write(buffer.constData(), step) != step)
            return fail(Error::WriteError, out.errorString());
        written += step;
        if (onChunk && !onChunk(written))
            return fail(Error::Stopped);
    }
    return skipData(); // выравнивание
}

bool TarArchive::finish(const std::function<void()>& onChunk)
{
    if (!m->in || m_error != Error::None)
        return false;
    if (!m->gz)
        return true;
    QByteArray buffer(int(kChunkBytes), Qt::Uninitialized);
    for (;;) {
        if (m->abort && m->abort->load(std::memory_order_relaxed))
            return fail(Error::Aborted);
        const qint64 r = m->gz->read(buffer.data(), buffer.size());
        if (r < 0 || (r == 0 && m->gz->error() != GzipDevice::Error::None)) {
            if (m->gz->error() == GzipDevice::Error::Aborted)
                return fail(Error::Aborted);
            return fail(Error::Corrupt, m->gz->errorString());
        }
        if (r == 0)
            return true;
        if (onChunk)
            onChunk();
    }
}

bool TarArchive::isHeader(const QByteArray& block, bool strict)
{
    if (block.size() < kBlock || allZero(block.constData(), kBlock))
        return false;
    const char* p = block.constData();
    qint64 stored = 0;
    if (!parseNumber(p + 148, 8, stored))
        return false;
    qint64 unsignedSum = 0;
    qint64 signedSum = 0;
    for (int i = 0; i < kBlock; ++i) {
        const bool inField = i >= 148 && i < 156;
        unsignedSum += inField ? ' ' : static_cast<unsigned char>(p[i]);
        signedSum += inField ? ' ' : static_cast<signed char>(p[i]);
    }
    if (stored != unsignedSum && stored != signedSum)
        return false;
    return !strict || std::memcmp(p + 257, "ustar", 5) == 0;
}

bool TarArchive::fileIsCompressedTar(const QString& path)
{
    if (!GzipDevice::fileHasSignature(path))
        return false;
    GzipDevice gz(path);
    if (!gz.open(QIODevice::ReadOnly))
        return false;
    QByteArray block(int(kBlock), Qt::Uninitialized);
    qint64 got = 0;
    while (got < kBlock) {
        const qint64 r = gz.read(block.data() + got, kBlock - got);
        if (r <= 0)
            return false;
        got += r;
    }
    return isHeader(block);
}

bool TarArchive::fileHasSignature(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(kBlock);
    if (isHeader(head))
        return true;
    if (!GzipDevice::hasSignature(head))
        return false;
    file.close();
    return fileIsCompressedTar(path);
}

QString TarArchive::errorText(Error error)
{
    switch (error) {
    case Error::None: return QString();
    case Error::NotTar: return QStringLiteral("Not a tar archive");
    case Error::ReadError: return QStringLiteral("Cannot read the archive");
    case Error::Corrupt: return QStringLiteral("The archive is damaged");
    case Error::Aborted: return QStringLiteral("Extraction cancelled");
    case Error::WriteError: return QStringLiteral("Cannot write the extracted file");
    case Error::Stopped: return QStringLiteral("Extraction stopped");
    }
    return QString();
}

namespace ArchiveFormats {

Format detect(const QString& path)
{
    if (ZipArchive::fileHasSignature(path))
        return Format::Zip;
    if (TarArchive::fileHasSignature(path))
        return Format::Tar;
    return Format::None;
}

Format detectKnown(const QString& path)
{
    const Format strict = detect(path);
    if (strict != Format::None)
        return strict;
    TarArchive tar(path);
    TarMember m;
    if (tar.open() && (tar.next(m) || tar.error() == TarArchive::Error::None))
        return Format::Tar;
    return Format::None;
}

} // namespace ArchiveFormats
