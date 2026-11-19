#include "gzipdevice.h"

#include <QFile>

#include <miniz.h>

#include <cstring>

namespace {

constexpr int kInputBufferSize = 256 * 1024;

// Флаги заголовка gzip (RFC 1952, 2.3.1).
constexpr int kFlagHcrc = 0x02;
constexpr int kFlagExtra = 0x04;
constexpr int kFlagName = 0x08;
constexpr int kFlagComment = 0x10;
constexpr int kFlagReserved = 0xE0;

constexpr int kMaxNameLength = 1024; // FNAME длиннее — читается, но обрезается

} // namespace

struct GzipDevice::Inflater {
    mz_stream stream;
    bool initialized = false;

    Inflater()
    {
        std::memset(&stream, 0, sizeof(stream));
        initialized = mz_inflateInit2(&stream, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
    }
    ~Inflater()
    {
        if (initialized)
            mz_inflateEnd(&stream);
    }
    bool reset() { return initialized && mz_inflateReset(&stream) == MZ_OK; }
};

GzipDevice::GzipDevice(const QString& path, QObject* parent)
    : QIODevice(parent)
    , m_ownedSource(std::make_unique<QFile>(path))
    , m_source(m_ownedSource.get())
{
}

GzipDevice::GzipDevice(QIODevice* source, QObject* parent)
    : QIODevice(parent)
    , m_source(source)
{
}

GzipDevice::~GzipDevice()
{
    GzipDevice::close();
}

bool GzipDevice::open(OpenMode mode)
{
    if ((mode & QIODevice::ReadWrite) != QIODevice::ReadOnly) {
        setErrorString(QStringLiteral("GzipDevice is read-only"));
        return false;
    }
    if (isOpen())
        close();

    m_in.resize(kInputBufferSize);
    m_inPos = m_inLen = 0;
    m_sourceEof = m_readFailed = false;
    m_state = State::Header;
    m_error = Error::None;
    m_crc = 0;
    m_memberOut = 0;
    m_totalOut = 0;
    m_compressedRead = 0;
    m_members = 0;
    m_originalName.clear();

    m_aborted = false;
    m_openedSource = false;
    if (m_source && !m_source->isOpen()) {
        if (!m_source->open(QIODevice::ReadOnly)) {
            m_error = Error::ReadError;
            setErrorString(m_source->errorString());
            return false;
        }
        m_openedSource = true; // открыли сами — сами и закроем
    }
    if (!m_source || !m_source->isReadable()) {
        m_error = Error::ReadError;
        setErrorString(m_source ? m_source->errorString() : errorText(Error::ReadError));
        return false;
    }
    m_inflater = std::make_unique<Inflater>();
    if (!m_inflater->initialized) {
        m_error = Error::ReadError;
        setErrorString(QStringLiteral("Cannot initialize the decompressor"));
        return false;
    }
    // Своя буферизация (m_in): буфер QIODevice поверх неё не нужен.
    return QIODevice::open(mode | QIODevice::Unbuffered);
}

void GzipDevice::close()
{
    if (!isOpen())
        return;
    QIODevice::close();
    m_inflater.reset();
    if (m_openedSource && m_source)
        m_source->close();
    m_openedSource = false;
    m_in = QByteArray();
}

bool GzipDevice::atEnd() const
{
    return !isOpen() || m_state == State::Done || m_state == State::Failed;
}

bool GzipDevice::fillInput()
{
    if (m_sourceEof)
        return false;
    // Прерывание проверяется на каждом блоке источника: вход, который не
    // даёт вывода (нули в конце, пустые потоки, длинный FCOMMENT), иначе
    // держал бы один read() до конца файла.
    if (m_abort && m_abort->load(std::memory_order_relaxed)) {
        m_aborted = true;
        m_sourceEof = true;
        return false;
    }
    if (m_inPos > 0) {
        // Сдвиг остатка в начало — только при дочитывании (остаток мал).
        const int rest = m_inLen - m_inPos;
        if (rest > 0)
            std::memmove(m_in.data(), m_in.constData() + m_inPos, size_t(rest));
        m_inLen = rest;
        m_inPos = 0;
    }
    if (m_inLen >= m_in.size())
        return true;
    const qint64 n = m_source->read(m_in.data() + m_inLen, m_in.size() - m_inLen);
    if (n < 0) {
        m_readFailed = true;
        m_sourceEof = true;
        return false;
    }
    if (n == 0) {
        m_sourceEof = true;
        return false;
    }
    m_inLen += int(n);
    m_compressedRead += n;
    return true;
}

int GzipDevice::getByte()
{
    if (m_inPos >= m_inLen && !fillInput())
        return -1;
    if (m_inPos >= m_inLen)
        return -1;
    return quint8(m_in.at(m_inPos++));
}

void GzipDevice::fail(Error error)
{
    // Прерывание и ошибка чтения источника главнее того, как их увидел
    // разбор (обрыв, мусор, «не gzip»).
    if (m_aborted)
        error = Error::Aborted;
    else if (m_readFailed && error == Error::Truncated)
        error = Error::ReadError;
    m_error = error;
    m_state = State::Failed;
    QString text = errorText(error);
    if (error == Error::ReadError && m_source && !m_source->errorString().isEmpty())
        text += QStringLiteral(": ") + m_source->errorString();
    setErrorString(text);
}

bool GzipDevice::parseHeader(bool first)
{
    // Заголовок разбирается побайтно через getByte(): поля переменной длины
    // (FEXTRA, FNAME, FCOMMENT) могут пересекать границу буфера. CRC
    // заголовка (FHCRC) считается на ходу — байты не копятся: FNAME и
    // FCOMMENT ничем не ограничены, и испорченный файл с гигабайтом
    // «комментария» не должен стоить гигабайта памяти.
    quint32 headerCrc = MZ_CRC32_INIT;
    const auto next = [this, &headerCrc]() {
        const int b = getByte();
        if (b >= 0) {
            const unsigned char c = static_cast<unsigned char>(b);
            headerCrc = quint32(mz_crc32(headerCrc, &c, 1));
        }
        return b;
    };
    // Обрыв посреди заголовка — Truncated; нет даже сигнатуры у первого
    // потока — NotGzip (пустой файл в том числе).
    if (first) {
        const int id1 = next();
        const int id2 = id1 < 0 ? -1 : next();
        if (id1 != 0x1F || id2 != 0x8B) {
            fail(m_readFailed ? Error::ReadError : Error::NotGzip);
            return false;
        }
    } else {
        // Сигнатуру следующего потока уже прочитал afterMember().
        static const unsigned char kMagic[2] = {0x1F, 0x8B};
        headerCrc = quint32(mz_crc32(headerCrc, kMagic, 2));
    }

    const int method = next();
    const int flags = next();
    if (method < 0 || flags < 0) {
        fail(Error::Truncated);
        return false;
    }
    if (method != 8 || (flags & kFlagReserved)) {
        fail(Error::BadHeader);
        return false;
    }
    // MTIME (4), XFL, OS.
    for (int i = 0; i < 6; ++i) {
        if (next() < 0) {
            fail(Error::Truncated);
            return false;
        }
    }
    if (flags & kFlagExtra) {
        const int lo = next();
        const int hi = lo < 0 ? -1 : next();
        if (hi < 0) {
            fail(Error::Truncated);
            return false;
        }
        for (int len = lo | (hi << 8); len > 0; --len) {
            if (next() < 0) {
                fail(Error::Truncated);
                return false;
            }
        }
    }
    const auto skipZeroTerminated = [&](QByteArray* keep) {
        for (;;) {
            const int c = next();
            if (c < 0)
                return false;
            if (c == 0)
                return true;
            if (keep && keep->size() < kMaxNameLength)
                keep->append(char(c));
        }
    };
    QByteArray name;
    if ((flags & kFlagName) && !skipZeroTerminated(&name)) {
        fail(Error::Truncated);
        return false;
    }
    if ((flags & kFlagComment) && !skipZeroTerminated(nullptr)) {
        fail(Error::Truncated);
        return false;
    }
    if (flags & kFlagHcrc) {
        const quint32 expected = headerCrc & 0xFFFFu;
        const int lo = getByte();
        const int hi = lo < 0 ? -1 : getByte();
        if (hi < 0) {
            fail(Error::Truncated);
            return false;
        }
        if (quint32(lo | (hi << 8)) != expected) {
            fail(Error::BadHeader);
            return false;
        }
    }

    if (m_members == 0 && !name.isEmpty())
        m_originalName = QString::fromLatin1(name);
    ++m_members;
    m_crc = MZ_CRC32_INIT;
    m_memberOut = 0;
    if (m_members > 1 && !m_inflater->reset()) {
        fail(Error::CorruptData);
        return false;
    }
    m_state = State::Body;
    return true;
}

bool GzipDevice::readTrailer()
{
    quint8 t[8];
    for (quint8& c : t) {
        const int b = getByte();
        if (b < 0) {
            fail(Error::Truncated);
            return false;
        }
        c = quint8(b);
    }
    const quint32 crc = quint32(t[0]) | quint32(t[1]) << 8 | quint32(t[2]) << 16 | quint32(t[3]) << 24;
    const quint32 isize = quint32(t[4]) | quint32(t[5]) << 8 | quint32(t[6]) << 16 | quint32(t[7]) << 24;
    if (crc != m_crc) {
        fail(Error::CrcMismatch);
        return false;
    }
    if (isize != m_memberOut) {
        fail(Error::SizeMismatch);
        return false;
    }
    m_state = State::BetweenMembers;
    return true;
}

bool GzipDevice::afterMember()
{
    const int b1 = getByte();
    if (b1 < 0) {
        if (m_readFailed || m_aborted) {
            fail(Error::ReadError);
            return false;
        }
        m_state = State::Done;
        return true;
    }
    if (b1 == 0x1F) {
        const int b2 = getByte();
        if (b2 == 0x8B) {
            m_state = State::Header;
            return parseHeader(false);
        }
        fail(m_readFailed ? Error::ReadError : Error::TrailingGarbage);
        return false;
    }
    if (b1 == 0) {
        // Нули до конца — выравнивание (gzip -d их тоже пропускает).
        for (;;) {
            if (m_inPos >= m_inLen && !fillInput())
                break;
            const char* p = m_in.constData() + m_inPos;
            const int n = m_inLen - m_inPos;
            for (int i = 0; i < n; ++i) {
                if (p[i] != 0) {
                    fail(Error::TrailingGarbage);
                    return false;
                }
            }
            m_inPos = m_inLen;
        }
        if (m_readFailed || m_aborted) {
            fail(Error::ReadError);
            return false;
        }
        m_state = State::Done;
        return true;
    }
    fail(Error::TrailingGarbage);
    return false;
}

qint64 GzipDevice::readData(char* data, qint64 maxSize)
{
    qint64 produced = 0;
    while (produced < maxSize) {
        switch (m_state) {
        case State::Done:
            return produced;
        case State::Failed:
            return produced > 0 ? produced : -1;
        case State::Header:
            parseHeader(m_members == 0);
            continue;
        case State::Trailer:
            readTrailer();
            continue;
        case State::BetweenMembers:
            afterMember();
            continue;
        case State::Body:
            break;
        }

        // Вход кончился — inflate всё равно зовётся: готовый вывод может
        // лежать в его словаре. Без хода на пустом входе он вернёт BUF_ERROR.
        if (m_inPos >= m_inLen)
            fillInput();
        mz_stream& s = m_inflater->stream;
        s.next_in = reinterpret_cast<const unsigned char*>(m_in.constData() + m_inPos);
        s.avail_in = unsigned(m_inLen - m_inPos);
        unsigned char* out = reinterpret_cast<unsigned char*>(data + produced);
        const qint64 room = qMin<qint64>(maxSize - produced, 1 << 30);
        s.next_out = out;
        s.avail_out = unsigned(room);

        const int status = mz_inflate(&s, MZ_SYNC_FLUSH);
        const int consumed = (m_inLen - m_inPos) - int(s.avail_in);
        m_inPos += consumed;
        const qint64 written = room - qint64(s.avail_out);
        if (written > 0) {
            m_crc = quint32(mz_crc32(m_crc, out, size_t(written)));
            m_memberOut += quint32(written);
            m_totalOut += written;
            produced += written;
        }

        if (status == MZ_STREAM_END) {
            m_state = State::Trailer;
            continue;
        }
        if (status == MZ_OK)
            continue;
        if (status == MZ_BUF_ERROR) {
            // Нет хода без нового входа (miniz отвечает так только на пустой
            // вход). Источник кончился — поток оборван.
            if (m_sourceEof)
                fail(Error::Truncated);
            continue;
        }
        fail(Error::CorruptData);
    }
    return produced;
}

bool GzipDevice::hasSignature(const QByteArray& head)
{
    return head.size() >= 2 && quint8(head.at(0)) == 0x1F && quint8(head.at(1)) == 0x8B;
}

bool GzipDevice::fileHasSignature(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return hasSignature(f.read(2));
}

qint64 GzipDevice::trailerSize(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 18) // минимальный gzip — 18 байт
        return -1;
    if (!f.seek(f.size() - 4))
        return -1;
    const QByteArray t = f.read(4);
    if (t.size() != 4)
        return -1;
    return qint64(quint32(quint8(t[0])) | quint32(quint8(t[1])) << 8 | quint32(quint8(t[2])) << 16
                  | quint32(quint8(t[3])) << 24);
}

QString GzipDevice::errorText(Error error)
{
    switch (error) {
    case Error::None: return QString();
    case Error::NotGzip: return QStringLiteral("Not a gzip file");
    case Error::BadHeader: return QStringLiteral("Unsupported or damaged gzip header");
    case Error::Truncated: return QStringLiteral("The gzip file is truncated");
    case Error::CorruptData: return QStringLiteral("The compressed data is damaged");
    case Error::CrcMismatch: return QStringLiteral("CRC check failed: the file is damaged");
    case Error::SizeMismatch: return QStringLiteral("Length check failed: the file is damaged");
    case Error::TrailingGarbage: return QStringLiteral("Unexpected data after the end of the gzip stream");
    case Error::ReadError: return QStringLiteral("Cannot read the file");
    case Error::Aborted: return QStringLiteral("Reading was interrupted");
    }
    return QString();
}
