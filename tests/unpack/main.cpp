// Тест ядра сжатых источников (F-2, шаг 1): GzipDevice, SourceUnpacker,
// SpoolDir / SpoolItem. Без виджетов и без окна.
//
// Фикстуры настоящего gzip (утилита gzip 1.12) зашиты массивами байт: двоичные
// файлы в репозитории на Windows рискуют пострадать от core.autocrlf. Большие
// входы тест сжимает сам через miniz.
//
// Режимы:
//   unpack                              — все проверки;
//   unpack --hold-spool <root>          — служебный: дочерний процесс держит
//                                         каталог экземпляра (печатает путь и
//                                         ждёт, пока его не убьют);
//   unpack --bench <file.gz> <out-dir>  — замер скорости распаковки.

#include "gzipdevice.h"
#include "logentry.h"
#include "logfile.h"
#include "sourceunpacker.h"
#include "spooldir.h"

#include <miniz.h>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        const auto _a = (a);                                                           \
        const auto _b = (b);                                                           \
        if (!(_a == _b)) {                                                             \
            ++g_failures;                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__,    \
                         __LINE__, #a, #b, (long long)(_a), (long long)(_b));          \
        }                                                                              \
    } while (0)

bool waitFor(const std::function<bool()>& ready, int timeoutMs = 120000)
{
    QElapsedTimer t;
    t.start();
    while (!ready()) {
        if (t.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Фикстуры: тексты и их gzip от настоящей утилиты.
//   single.gz — gzip -9 -n app.log       (без имени)
//   named.gz  — gzip app.log             (FNAME = app.log, MTIME)
//   concat.gz — gzip -n app.log; gzip -n part2.log; cat — два потока
//   empty.gz  — gzip -n пустого файла
// ---------------------------------------------------------------------------
const char kAppLog[] =
    "2026-11-19 10:00:00.000 INFO  [main] Service started\n"
    "2026-11-19 10:00:00.125 DEBUG [worker-1] Loaded 42 rules from config\n"
    "2026-11-19 10:00:01.500 WARN  [worker-2] Slow response: 1500 ms\n"
    "2026-11-19 10:00:02.000 ERROR [worker-1] Connection refused\n"
    "    at net.Client.connect(Client.java:120)\n"
    "    at net.Pool.acquire(Pool.java:57)\n"
    "2026-11-19 10:00:03.000 INFO  [main] Retrying\n";
const char kPart2Log[] =
    "2026-11-19 10:05:00.000 INFO  [main] Second part begins\n"
    "2026-11-19 10:05:01.000 ERROR [db] Deadlock detected\n";

// single.gz — 237 байт
const unsigned char kSingleGz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x6d, 0x50, 0x4d, 0x4b, 0x03, 0x31,
    0x10, 0xbd, 0xf7, 0x57, 0xcc, 0xb1, 0x3d, 0x6c, 0x48, 0xa2, 0xab, 0xb8, 0x37, 0xad, 0x55, 0x04,
    0x69, 0x25, 0x45, 0x3c, 0x94, 0x1e, 0x42, 0x76, 0xb6, 0x44, 0x77, 0x67, 0x34, 0xc9, 0xb6, 0xf8,
    0xef, 0x4d, 0xb7, 0x22, 0x05, 0x77, 0x98, 0xcb, 0x83, 0xf7, 0x35, 0xa3, 0xa5, 0xbe, 0x2a, 0x94,
    0x2a, 0xd4, 0x0d, 0x28, 0x59, 0xc9, 0xe3, 0x0a, 0x29, 0x25, 0x3c, 0x2d, 0x1f, 0x56, 0x00, 0x9b,
    0xce, 0x7a, 0xda, 0xc2, 0x1a, 0xc3, 0xde, 0x3b, 0x84, 0x98, 0x6c, 0x48, 0x58, 0x4f, 0xf4, 0x88,
    0x48, 0xe9, 0x12, 0xee, 0x17, 0x77, 0xaf, 0x8f, 0xb0, 0x39, 0x70, 0xf8, 0xc0, 0x50, 0xa8, 0x2d,
    0x3c, 0xb3, 0xad, 0xb1, 0x86, 0x4b, 0x0d, 0xa1, 0x6f, 0x31, 0x42, 0x13, 0xb8, 0x03, 0xc7, 0xd4,
    0xf8, 0xdd, 0x88, 0x89, 0x12, 0x65, 0x4e, 0x7e, 0xbb, 0x35, 0x4b, 0xf8, 0x33, 0xd1, 0x39, 0xbd,
    0xe5, 0x03, 0x04, 0x8c, 0x9f, 0x4c, 0x11, 0x2b, 0x50, 0x47, 0x52, 0x17, 0x47, 0xf4, 0x7a, 0x68,
    0xbe, 0x30, 0x66, 0x65, 0xce, 0x4b, 0xcc, 0x99, 0x08, 0x5d, 0xf2, 0x4c, 0xd9, 0xa5, 0xe9, 0x63,
    0x3e, 0x00, 0xf2, 0xd8, 0x04, 0x84, 0x49, 0xcc, 0x5b, 0x8f, 0x94, 0x84, 0x3b, 0x91, 0xa6, 0xbf,
    0xf0, 0xdd, 0xee, 0x6d, 0xa5, 0xb4, 0x9c, 0x9d, 0x53, 0x5f, 0x98, 0x5b, 0x61, 0xdd, 0x57, 0xef,
    0x03, 0x4e, 0x07, 0x30, 0xd0, 0xca, 0xeb, 0xd9, 0x48, 0x99, 0x8b, 0xff, 0x6f, 0x34, 0x98, 0xc2,
    0xb7, 0xa7, 0xdd, 0xe4, 0x07, 0x8c, 0x6e, 0xc3, 0x24, 0x75, 0x01, 0x00, 0x00,
};

// named.gz — 245 байт
const unsigned char kNamedGz[] = {
    0x1f, 0x8b, 0x08, 0x08, 0x91, 0xd4, 0xfe, 0x6a, 0x00, 0x03, 0x61, 0x70, 0x70, 0x2e, 0x6c, 0x6f,
    0x67, 0x00, 0x6d, 0x50, 0x4d, 0x4b, 0x03, 0x31, 0x10, 0xbd, 0xf7, 0x57, 0xcc, 0xb1, 0x3d, 0x6c,
    0x48, 0xa2, 0xab, 0xb8, 0x37, 0xad, 0x55, 0x04, 0x69, 0x25, 0x45, 0x3c, 0x94, 0x1e, 0x42, 0x76,
    0xb6, 0x44, 0x77, 0x67, 0x34, 0xc9, 0xb6, 0xf8, 0xef, 0x4d, 0xb7, 0x22, 0x05, 0x77, 0x98, 0xcb,
    0x83, 0xf7, 0x35, 0xa3, 0xa5, 0xbe, 0x2a, 0x94, 0x2a, 0xd4, 0x0d, 0x28, 0x59, 0xc9, 0xe3, 0x0a,
    0x29, 0x25, 0x3c, 0x2d, 0x1f, 0x56, 0x00, 0x9b, 0xce, 0x7a, 0xda, 0xc2, 0x1a, 0xc3, 0xde, 0x3b,
    0x84, 0x98, 0x6c, 0x48, 0x58, 0x4f, 0xf4, 0x88, 0x48, 0xe9, 0x12, 0xee, 0x17, 0x77, 0xaf, 0x8f,
    0xb0, 0x39, 0x70, 0xf8, 0xc0, 0x50, 0xa8, 0x2d, 0x3c, 0xb3, 0xad, 0xb1, 0x86, 0x4b, 0x0d, 0xa1,
    0x6f, 0x31, 0x42, 0x13, 0xb8, 0x03, 0xc7, 0xd4, 0xf8, 0xdd, 0x88, 0x89, 0x12, 0x65, 0x4e, 0x7e,
    0xbb, 0x35, 0x4b, 0xf8, 0x33, 0xd1, 0x39, 0xbd, 0xe5, 0x03, 0x04, 0x8c, 0x9f, 0x4c, 0x11, 0x2b,
    0x50, 0x47, 0x52, 0x17, 0x47, 0xf4, 0x7a, 0x68, 0xbe, 0x30, 0x66, 0x65, 0xce, 0x4b, 0xcc, 0x99,
    0x08, 0x5d, 0xf2, 0x4c, 0xd9, 0xa5, 0xe9, 0x63, 0x3e, 0x00, 0xf2, 0xd8, 0x04, 0x84, 0x49, 0xcc,
    0x5b, 0x8f, 0x94, 0x84, 0x3b, 0x91, 0xa6, 0xbf, 0xf0, 0xdd, 0xee, 0x6d, 0xa5, 0xb4, 0x9c, 0x9d,
    0x53, 0x5f, 0x98, 0x5b, 0x61, 0xdd, 0x57, 0xef, 0x03, 0x4e, 0x07, 0x30, 0xd0, 0xca, 0xeb, 0xd9,
    0x48, 0x99, 0x8b, 0xff, 0x6f, 0x34, 0x98, 0xc2, 0xb7, 0xa7, 0xdd, 0xe4, 0x07, 0x8c, 0x6e, 0xc3,
    0x24, 0x75, 0x01, 0x00, 0x00,
};

// concat.gz — 348 байт
const unsigned char kConcatGz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x6d, 0x50, 0x4d, 0x4b, 0x03, 0x31,
    0x10, 0xbd, 0xf7, 0x57, 0xcc, 0xb1, 0x3d, 0x6c, 0x48, 0xa2, 0xab, 0xb8, 0x37, 0xad, 0x55, 0x04,
    0x69, 0x25, 0x45, 0x3c, 0x94, 0x1e, 0x42, 0x76, 0xb6, 0x44, 0x77, 0x67, 0x34, 0xc9, 0xb6, 0xf8,
    0xef, 0x4d, 0xb7, 0x22, 0x05, 0x77, 0x98, 0xcb, 0x83, 0xf7, 0x35, 0xa3, 0xa5, 0xbe, 0x2a, 0x94,
    0x2a, 0xd4, 0x0d, 0x28, 0x59, 0xc9, 0xe3, 0x0a, 0x29, 0x25, 0x3c, 0x2d, 0x1f, 0x56, 0x00, 0x9b,
    0xce, 0x7a, 0xda, 0xc2, 0x1a, 0xc3, 0xde, 0x3b, 0x84, 0x98, 0x6c, 0x48, 0x58, 0x4f, 0xf4, 0x88,
    0x48, 0xe9, 0x12, 0xee, 0x17, 0x77, 0xaf, 0x8f, 0xb0, 0x39, 0x70, 0xf8, 0xc0, 0x50, 0xa8, 0x2d,
    0x3c, 0xb3, 0xad, 0xb1, 0x86, 0x4b, 0x0d, 0xa1, 0x6f, 0x31, 0x42, 0x13, 0xb8, 0x03, 0xc7, 0xd4,
    0xf8, 0xdd, 0x88, 0x89, 0x12, 0x65, 0x4e, 0x7e, 0xbb, 0x35, 0x4b, 0xf8, 0x33, 0xd1, 0x39, 0xbd,
    0xe5, 0x03, 0x04, 0x8c, 0x9f, 0x4c, 0x11, 0x2b, 0x50, 0x47, 0x52, 0x17, 0x47, 0xf4, 0x7a, 0x68,
    0xbe, 0x30, 0x66, 0x65, 0xce, 0x4b, 0xcc, 0x99, 0x08, 0x5d, 0xf2, 0x4c, 0xd9, 0xa5, 0xe9, 0x63,
    0x3e, 0x00, 0xf2, 0xd8, 0x04, 0x84, 0x49, 0xcc, 0x5b, 0x8f, 0x94, 0x84, 0x3b, 0x91, 0xa6, 0xbf,
    0xf0, 0xdd, 0xee, 0x6d, 0xa5, 0xb4, 0x9c, 0x9d, 0x53, 0x5f, 0x98, 0x5b, 0x61, 0xdd, 0x57, 0xef,
    0x03, 0x4e, 0x07, 0x30, 0xd0, 0xca, 0xeb, 0xd9, 0x48, 0x99, 0x8b, 0xff, 0x6f, 0x34, 0x98, 0xc2,
    0xb7, 0xa7, 0xdd, 0xe4, 0x07, 0x8c, 0x6e, 0xc3, 0x24, 0x75, 0x01, 0x00, 0x00, 0x1f, 0x8b, 0x08,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x33, 0x32, 0x30, 0x32, 0xd3, 0x35, 0x34, 0xd4, 0x35,
    0xb4, 0x54, 0x30, 0x34, 0xb0, 0x32, 0x30, 0xb5, 0x32, 0x30, 0xd0, 0x33, 0x30, 0x30, 0x50, 0xf0,
    0xf4, 0x73, 0xf3, 0x57, 0x50, 0x88, 0xce, 0x4d, 0xcc, 0xcc, 0x8b, 0x55, 0x08, 0x4e, 0x4d, 0xce,
    0xcf, 0x4b, 0x51, 0x28, 0x48, 0x2c, 0x2a, 0x51, 0x48, 0x4a, 0x4d, 0xcf, 0xcc, 0x2b, 0xe6, 0x32,
    0xc2, 0xd0, 0x67, 0x08, 0xd6, 0xe7, 0x1a, 0x14, 0xe4, 0x1f, 0xa4, 0x10, 0x9d, 0x92, 0x14, 0xab,
    0xe0, 0x92, 0x9a, 0x98, 0x92, 0x93, 0x9f, 0x9c, 0xad, 0x90, 0x92, 0x5a, 0x92, 0x9a, 0x5c, 0x92,
    0x9a, 0xc2, 0x05, 0x00, 0xce, 0x1c, 0x62, 0xc9, 0x6d, 0x00, 0x00, 0x00,
};

// empty.gz — 20 байт
const unsigned char kEmptyGz[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

template <size_t N>
QByteArray bytes(const unsigned char (&a)[N])
{
    return QByteArray(reinterpret_cast<const char*>(a), int(N));
}

QByteArray appLog() { return QByteArray(kAppLog); }
QByteArray part2Log() { return QByteArray(kPart2Log); }

// ---------------------------------------------------------------------------
// Сжатие через miniz: raw deflate + свой заголовок и хвост gzip.
// ---------------------------------------------------------------------------
QByteArray rawDeflate(const QByteArray& data, int level)
{
    mz_stream s;
    std::memset(&s, 0, sizeof(s));
    if (mz_deflateInit2(&s, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY) != MZ_OK)
        return {};
    QByteArray out;
    QByteArray chunk(256 * 1024, Qt::Uninitialized);
    const char* in = data.constData();
    qint64 left = data.size();
    for (;;) {
        const unsigned feed = unsigned(qMin<qint64>(left, 1 << 24));
        s.next_in = reinterpret_cast<const unsigned char*>(in);
        s.avail_in = feed;
        const bool last = left == feed;
        int status = MZ_OK;
        do {
            s.next_out = reinterpret_cast<unsigned char*>(chunk.data());
            s.avail_out = unsigned(chunk.size());
            status = mz_deflate(&s, last ? MZ_FINISH : MZ_NO_FLUSH);
            out.append(chunk.constData(), chunk.size() - int(s.avail_out));
        } while (s.avail_out == 0 || (last && status == MZ_OK));
        in += feed - s.avail_in;
        left -= feed - s.avail_in;
        if (last && status == MZ_STREAM_END)
            break;
        if (status != MZ_OK && status != MZ_BUF_ERROR) {
            out.clear();
            break;
        }
    }
    mz_deflateEnd(&s);
    return out;
}

void appendLe32(QByteArray& out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

quint32 crc32Of(const QByteArray& data)
{
    return quint32(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.constData()),
                            size_t(data.size())));
}

struct HeaderOptions {
    QByteArray extra;          // FEXTRA
    QByteArray name;           // FNAME
    QByteArray comment;        // FCOMMENT
    bool headerCrc = false;    // FHCRC
    bool badHeaderCrc = false;
    int method = 8;
};

QByteArray gzipMember(const QByteArray& data, int level = 1, const HeaderOptions& o = {})
{
    QByteArray h;
    int flags = 0;
    if (!o.extra.isEmpty()) flags |= 0x04;
    if (!o.name.isEmpty()) flags |= 0x08;
    if (!o.comment.isEmpty()) flags |= 0x10;
    if (o.headerCrc) flags |= 0x02;
    h.append(char(0x1F));
    h.append(char(0x8B));
    h.append(char(o.method));
    h.append(char(flags));
    appendLe32(h, 0x5F000000u); // MTIME
    h.append(char(0));          // XFL
    h.append(char(3));          // OS = Unix
    if (!o.extra.isEmpty()) {
        h.append(char(o.extra.size() & 0xFF));
        h.append(char(o.extra.size() >> 8));
        h.append(o.extra);
    }
    if (!o.name.isEmpty()) { h.append(o.name); h.append(char(0)); }
    if (!o.comment.isEmpty()) { h.append(o.comment); h.append(char(0)); }
    if (o.headerCrc) {
        quint32 c = crc32Of(h) & 0xFFFF;
        if (o.badHeaderCrc)
            c ^= 0x5A5A;
        h.append(char(c & 0xFF));
        h.append(char(c >> 8));
    }
    QByteArray out = h + rawDeflate(data, level);
    appendLe32(out, crc32Of(data));
    appendLe32(out, quint32(data.size()));
    return out;
}

// Детерминированный «лог» заданного размера.
QByteArray makeLog(qint64 bytes, quint32 seed)
{
    static const char* const kWords[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
                                         "request", "response", "timeout", "user", "session", "cache"};
    static const char* const kLevels[] = {"INFO ", "DEBUG", "WARN ", "ERROR"};
    std::mt19937 rng(seed);
    QByteArray out;
    out.reserve(int(bytes + 256));
    qint64 n = 0;
    while (out.size() < bytes) {
        out.append(QByteArray("2026-11-19 10:") + QByteArray::number(10 + (n / 6000) % 50) + ':'
                   + QByteArray::number(10 + (n / 100) % 50) + '.' + QByteArray::number(100 + n % 900)
                   + ' ' + kLevels[rng() % 4] + " [t" + QByteArray::number(rng() % 16) + "] ");
        const int words = 3 + int(rng() % 12);
        for (int i = 0; i < words; ++i) {
            out.append(kWords[rng() % 12]);
            out.append(i + 1 < words ? ' ' : '\n');
        }
        ++n;
    }
    out.truncate(int(bytes));
    return out;
}

// ---------------------------------------------------------------------------
// Чтение GzipDevice целиком.
// ---------------------------------------------------------------------------
struct Inflated {
    QByteArray data;
    GzipDevice::Error error = GzipDevice::Error::None;
    bool failed = false;   // read() вернул -1
    int members = 0;
    QString name;
};

Inflated inflateAll(QIODevice* source, qint64 readSize = 64 * 1024)
{
    Inflated r;
    GzipDevice gz(source);
    if (!gz.open(QIODevice::ReadOnly)) {
        r.error = gz.error();
        r.failed = true;
        return r;
    }
    QByteArray buf(int(readSize), Qt::Uninitialized);
    for (;;) {
        const qint64 n = gz.read(buf.data(), buf.size());
        if (n < 0) {
            r.failed = true;
            break;
        }
        if (n == 0)
            break;
        r.data.append(buf.constData(), int(n));
    }
    CHECK(gz.atEnd());
    r.error = gz.error();
    r.members = gz.memberCount();
    r.name = gz.originalName();
    if (r.failed)
        CHECK(r.error != GzipDevice::Error::None);
    else
        CHECK(r.error == GzipDevice::Error::None);
    return r;
}

Inflated inflateBytes(const QByteArray& gz, qint64 readSize = 64 * 1024)
{
    QBuffer buf;
    buf.setData(gz);
    buf.open(QIODevice::ReadOnly);
    return inflateAll(&buf, readSize);
}

QString writeBytesRaw(const QDir& dir, const QString& name, const QByteArray& data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
    return path;
}

// Источник, который отдаёт не больше chunk байт за чтение: заголовок, хвост
// и граница потоков приходят по кускам.
class TrickleDevice : public QIODevice {
public:
    TrickleDevice(QByteArray data, int chunk) : m_data(std::move(data)), m_chunk(chunk) {}
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* out, qint64 max) override
    {
        const qint64 n = qMin<qint64>(qMin<qint64>(max, m_chunk), m_data.size() - m_pos);
        std::memcpy(out, m_data.constData() + m_pos, size_t(n));
        m_pos += n;
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray m_data;
    int m_chunk;
    qint64 m_pos = 0;
};

// ===========================================================================
// GzipDevice
// ===========================================================================
void testRealFixtures()
{
    {
        const Inflated r = inflateBytes(bytes(kSingleGz));
        CHECK(!r.failed);
        CHECK(r.data == appLog());
        CHECK_EQ(r.members, 1);
        CHECK(r.name.isEmpty());
    }
    {
        const Inflated r = inflateBytes(bytes(kNamedGz));
        CHECK(!r.failed);
        CHECK(r.data == appLog());
        CHECK(r.name == QStringLiteral("app.log"));
    }
    {
        const Inflated r = inflateBytes(bytes(kConcatGz));
        CHECK(!r.failed);
        CHECK(r.data == appLog() + part2Log());
        CHECK_EQ(r.members, 2);
    }
    {
        const Inflated r = inflateBytes(bytes(kEmptyGz));
        CHECK(!r.failed);
        CHECK(r.data.isEmpty());
        CHECK_EQ(r.members, 1);
    }
    // Мелкие чтения и источник по байту: состояние переживает любую нарезку.
    for (const qint64 readSize : {qint64(1), qint64(7), qint64(4096)}) {
        const Inflated r = inflateBytes(bytes(kConcatGz), readSize);
        CHECK(!r.failed);
        CHECK(r.data == appLog() + part2Log());
    }
    for (const int chunk : {1, 3, 11}) {
        TrickleDevice dev(bytes(kNamedGz) + bytes(kConcatGz), chunk);
        dev.open(QIODevice::ReadOnly);
        const Inflated r = inflateAll(&dev, 5);
        CHECK(!r.failed);
        CHECK(r.data == appLog() + appLog() + part2Log());
        CHECK_EQ(r.members, 3);
        CHECK(r.name == QStringLiteral("app.log"));
    }
}

void testBrokenInputs()
{
    const QByteArray single = bytes(kSingleGz);
    const auto errorOf = [](const QByteArray& gz) {
        const Inflated r = inflateBytes(gz);
        CHECK(r.failed);
        return r.error;
    };

    // Не gzip: пустой файл, текст, одна половина сигнатуры.
    CHECK(errorOf(QByteArray()) == GzipDevice::Error::NotGzip);
    CHECK(errorOf(appLog()) == GzipDevice::Error::NotGzip);
    CHECK(errorOf(QByteArray("\x1f", 1)) == GzipDevice::Error::NotGzip);

    // Обрыв: в заголовке, посреди данных, в хвосте.
    CHECK(errorOf(single.left(5)) == GzipDevice::Error::Truncated);
    CHECK(errorOf(single.left(single.size() / 2)) == GzipDevice::Error::Truncated);
    CHECK(errorOf(single.left(single.size() - 3)) == GzipDevice::Error::Truncated);
    // Обрыв второго потока склейки.
    const QByteArray concat = bytes(kConcatGz);
    CHECK(errorOf(concat.left(concat.size() - 20)) == GzipDevice::Error::Truncated);

    // Неверная CRC и длина в хвосте.
    {
        QByteArray bad = single;
        bad[bad.size() - 8] = char(bad[bad.size() - 8] ^ 0x01);
        CHECK(errorOf(bad) == GzipDevice::Error::CrcMismatch);
    }
    {
        QByteArray bad = single;
        bad[bad.size() - 4] = char(bad[bad.size() - 4] ^ 0x01);
        CHECK(errorOf(bad) == GzipDevice::Error::SizeMismatch);
    }
    // Испорченные данные deflate: ошибка обязана быть (какая именно — зависит
    // от того, где поток разошёлся).
    {
        QByteArray bad = single;
        bad[40] = char(bad[40] ^ 0xFF);
        const GzipDevice::Error e = errorOf(bad);
        CHECK(e == GzipDevice::Error::CorruptData || e == GzipDevice::Error::CrcMismatch
              || e == GzipDevice::Error::SizeMismatch || e == GzipDevice::Error::Truncated
              || e == GzipDevice::Error::TrailingGarbage);
    }
    // Мусор после потока — ошибка; нули — нет (как gzip -d).
    CHECK(errorOf(single + QByteArray("garbage")) == GzipDevice::Error::TrailingGarbage);
    CHECK(errorOf(single + QByteArray("\x1f", 1) + QByteArray("x")) == GzipDevice::Error::TrailingGarbage);
    CHECK(errorOf(single + QByteArray(100, '\0') + QByteArray("x")) == GzipDevice::Error::TrailingGarbage);
    {
        const Inflated r = inflateBytes(single + QByteArray(1000, '\0'));
        CHECK(!r.failed);
        CHECK(r.data == appLog());
    }
    // Метод не deflate, зарезервированный флаг, неверная CRC заголовка.
    {
        HeaderOptions o;
        o.method = 7;
        CHECK(errorOf(gzipMember(appLog(), 6, o)) == GzipDevice::Error::BadHeader);
    }
    {
        QByteArray bad = single;
        bad[3] = char(0x20);
        CHECK(errorOf(bad) == GzipDevice::Error::BadHeader);
    }
    {
        HeaderOptions o;
        o.headerCrc = true;
        o.badHeaderCrc = true;
        CHECK(errorOf(gzipMember(appLog(), 6, o)) == GzipDevice::Error::BadHeader);
    }
}

void testHeaderFields()
{
    // Все необязательные поля заголовка разом (gzip их не пишет — собираем сами).
    HeaderOptions o;
    o.extra = QByteArray("XY\x04\x00" "abcd", 8);
    o.name = QByteArray("service.log");
    o.comment = QByteArray("rotated by logrotate");
    o.headerCrc = true;
    const QByteArray gz = gzipMember(appLog(), 9, o) + gzipMember(part2Log(), 1);
    for (const int chunk : {1, 2, 64 * 1024}) {
        TrickleDevice dev(gz, chunk);
        dev.open(QIODevice::ReadOnly);
        const Inflated r = inflateAll(&dev);
        CHECK(!r.failed);
        CHECK(r.data == appLog() + part2Log());
        CHECK(r.name == QStringLiteral("service.log"));
        CHECK_EQ(r.members, 2);
    }
}

void testLargeStreams()
{
    // Несколько мегабайт в трёх потоках, разные уровни: поток пересекает
    // границы буфера источника (256 КБ) и словаря inflate (32 КБ).
    const QByteArray a = makeLog(5 * 1024 * 1024 + 123, 1);
    const QByteArray b = makeLog(300 * 1024, 2);
    const QByteArray c = QByteArray(3 * 1024 * 1024, 'z'); // длинные повторы
    const QByteArray gz = gzipMember(a, 1) + gzipMember(b, 9) + gzipMember(c, 6);
    for (const qint64 readSize : {qint64(1000), qint64(1024 * 1024)}) {
        const Inflated r = inflateBytes(gz, readSize);
        CHECK(!r.failed);
        CHECK_EQ(r.data.size(), a.size() + b.size() + c.size());
        CHECK(r.data == a + b + c);
        CHECK_EQ(r.members, 3);
    }
    // compressedBytesRead доходит до размера входа.
    QBuffer buf;
    buf.setData(gz);
    buf.open(QIODevice::ReadOnly);
    GzipDevice dev(&buf);
    CHECK(dev.open(QIODevice::ReadOnly));
    QByteArray sink(1 << 20, Qt::Uninitialized);
    while (dev.read(sink.data(), sink.size()) > 0) {
    }
    CHECK(dev.error() == GzipDevice::Error::None);
    CHECK_EQ(dev.compressedBytesRead(), qint64(gz.size()));
    CHECK_EQ(dev.uncompressedBytes(), qint64(a.size() + b.size() + c.size()));
}

// Источник, поднимающий флаг прерывания, когда прочитано больше trigger байт.
class AbortingDevice : public QIODevice {
public:
    AbortingDevice(QByteArray data, qint64 trigger, std::atomic_bool* flag)
        : m_data(std::move(data)), m_trigger(trigger), m_flag(flag) {}
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* out, qint64 max) override
    {
        const qint64 n = qMin<qint64>(max, m_data.size() - m_pos);
        std::memcpy(out, m_data.constData() + m_pos, size_t(n));
        m_pos += n;
        if (m_pos > m_trigger)
            m_flag->store(true);
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray m_data;
    qint64 m_trigger;
    qint64 m_pos = 0;
    std::atomic_bool* m_flag;
};

void testAbortAndLongFields()
{
    // Вход без вывода — 64 МБ нулей после потока (выравнивание): прерывание
    // срабатывает на ближайшем блоке источника, а не после всего файла.
    const QByteArray padded = bytes(kSingleGz) + QByteArray(64 * 1024 * 1024, '\0');
    {
        std::atomic_bool abort{false};
        AbortingDevice src(padded, 1024 * 1024, &abort);
        src.open(QIODevice::ReadOnly);
        GzipDevice gz(&src);
        gz.setAbortFlag(&abort);
        CHECK(gz.open(QIODevice::ReadOnly));
        QByteArray out, buf(1 << 20, Qt::Uninitialized);
        qint64 n = 0;
        while ((n = gz.read(buf.data(), buf.size())) > 0)
            out.append(buf.constData(), int(n));
        CHECK_EQ(n, qint64(-1));
        CHECK(gz.error() == GzipDevice::Error::Aborted);
        CHECK(out == appLog());
        CHECK(gz.compressedBytesRead() < 2 * 1024 * 1024);
    }
    // Без прерывания тот же вход читается до конца без ошибки.
    {
        const Inflated r = inflateBytes(padded, 1 << 20);
        CHECK(!r.failed);
        CHECK(r.data == appLog());
    }
    // Длинный FCOMMENT (8 МБ) и FNAME длиннее предела имени: поток читается,
    // имя обрезано, CRC заголовка сходится (считается на ходу).
    {
        HeaderOptions o;
        o.comment = QByteArray(8 * 1024 * 1024, 'c');
        o.name = QByteArray(5000, 'n');
        o.headerCrc = true;
        const Inflated r = inflateBytes(gzipMember(appLog(), 6, o));
        CHECK(!r.failed);
        CHECK(r.data == appLog());
        CHECK_EQ(r.name.size(), qsizetype(1024));
    }
}

void testExternalSourceReopen(const QDir& dir)
{
    // Закрытый источник устройство открывает само и само закрывает; повторный
    // open() читает с начала.
    const QString path = writeBytesRaw(dir, QStringLiteral("reopen.gz"), bytes(kConcatGz));
    QFile file(path);
    GzipDevice gz(&file);
    for (int round = 0; round < 2; ++round) {
        CHECK(gz.open(QIODevice::ReadOnly));
        CHECK(file.isOpen());
        CHECK(gz.readAll() == appLog() + part2Log());
        CHECK(gz.error() == GzipDevice::Error::None);
        gz.close();
        CHECK(!file.isOpen());
    }
    // Открытый вызывающим — остаётся открытым.
    CHECK(file.open(QIODevice::ReadOnly));
    {
        GzipDevice gz2(&file);
        CHECK(gz2.open(QIODevice::ReadOnly));
        CHECK(gz2.readAll() == appLog() + part2Log());
    }
    CHECK(file.isOpen());
}

void testSignatureHelpers(const QDir& dir)
{
    const QString gzPath = dir.filePath(QStringLiteral("sig.bin"));
    const QString txtPath = dir.filePath(QStringLiteral("sig.gz"));
    {
        QFile f(gzPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(bytes(kNamedGz));
    }
    {
        QFile f(txtPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(appLog());
    }
    // Сигнатура, а не имя.
    CHECK(GzipDevice::fileHasSignature(gzPath));
    CHECK(!GzipDevice::fileHasSignature(txtPath));
    CHECK_EQ(GzipDevice::trailerSize(gzPath), qint64(appLog().size()));
    CHECK_EQ(GzipDevice::trailerSize(dir.filePath(QStringLiteral("missing.gz"))), qint64(-1));
    // Устройство по пути.
    GzipDevice dev(gzPath);
    CHECK(dev.open(QIODevice::ReadOnly));
    CHECK(dev.readAll() == appLog());
    CHECK(dev.error() == GzipDevice::Error::None);
    GzipDevice missing(dir.filePath(QStringLiteral("missing.gz")));
    CHECK(!missing.open(QIODevice::ReadOnly));
    CHECK(missing.error() == GzipDevice::Error::ReadError);
}

// ===========================================================================
// SourceUnpacker
// ===========================================================================
QString writeBytes(const QDir& dir, const QString& name, const QByteArray& data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        std::fprintf(stderr, "cannot write %s\n", qPrintable(path));
    return path;
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

struct RunResult {
    SourceUnpacker::Result result;
    qint64 lastProgress = -1;
    qint64 progressTotal = -1;
    int progressSignals = 0;
};

RunResult runUnpack(SourceUnpacker& unpacker, const QString& src, const QString& dst)
{
    RunResult rr;
    bool done = false;
    QObject ctx;
    QObject::connect(&unpacker, &SourceUnpacker::progress, &ctx, [&rr](qint64 d, qint64 t) {
        ++rr.progressSignals;
        CHECK(d >= rr.lastProgress); // не убывает
        rr.lastProgress = d;
        rr.progressTotal = t;
    });
    QObject::connect(&unpacker, &SourceUnpacker::finished, &ctx,
                     [&rr, &done](const SourceUnpacker::Result& r) {
                         rr.result = r;
                         done = true;
                     });
    CHECK(unpacker.unpackGzip(src, dst));
    // isRunning() здесь не проверяется: воркер снимает занятость сам, сразу
    // после отправки итога, и быстрое задание (ошибка до чтения) может уже
    // закончиться.
    CHECK(waitFor([&done] { return done; }));
    CHECK(!unpacker.isRunning());
    return rr;
}

// Своя «файловая система» для проверки места: свободно = диск − размер
// целевого файла. Никаких настоящих гигабайт.
SourceUnpacker::FreeSpaceFunction fakeDisk(qint64 diskBytes, const QString& target,
                                           std::shared_ptr<std::atomic<int>> calls = {})
{
    return [diskBytes, target, calls](const QString&) {
        if (calls)
            calls->fetch_add(1);
        return diskBytes - QFileInfo(target).size();
    };
}

void testUnpackOk(const QDir& dir)
{
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });

    // Настоящий gzip байт в байт, склейка — тоже.
    for (const auto& [name, gz, expected] :
         {std::make_tuple(QStringLiteral("named.gz"), bytes(kNamedGz), appLog()),
          std::make_tuple(QStringLiteral("concat.gz"), bytes(kConcatGz), appLog() + part2Log()),
          std::make_tuple(QStringLiteral("empty.gz"), bytes(kEmptyGz), QByteArray())}) {
        const QString src = writeBytes(dir, name, gz);
        const QString dst = dir.filePath(name + QStringLiteral(".out"));
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.ok());
        CHECK(rr.result.message.isEmpty());
        CHECK(QFileInfo::exists(dst));
        CHECK(readFile(dst) == expected);
        CHECK_EQ(rr.result.bytesWritten, qint64(expected.size()));
        CHECK_EQ(rr.lastProgress, qint64(gz.size()));
        CHECK_EQ(rr.progressTotal, qint64(gz.size()));
    }

    // Большой многопоточный вход: файл совпадает байт в байт, прогресс доходит
    // до конца, целевой файл перезаписывается.
    const QByteArray a = makeLog(9 * 1024 * 1024, 7);
    const QByteArray b = makeLog(2 * 1024 * 1024 + 1, 8);
    const QByteArray gz = gzipMember(a, 1) + gzipMember(b, 6);
    const QString src = writeBytes(dir, QStringLiteral("big.log.gz"), gz);
    const QString dst = writeBytes(dir, QStringLiteral("big.log"), QByteArray("old content, longer than nothing"));
    unpacker.setCheckIntervalBytes(1024 * 1024);
    const RunResult rr = runUnpack(unpacker, src, dst);
    CHECK(rr.result.ok());
    CHECK(readFile(dst) == a + b);
    CHECK_EQ(rr.lastProgress, qint64(gz.size()));
    CHECK(rr.progressSignals >= 2);

    // Второе задание, пока идёт первое, — отказ, первое не страдает.
    bool done = false;
    QObject ctx;
    QObject::connect(&unpacker, &SourceUnpacker::finished, &ctx,
                     [&done](const SourceUnpacker::Result&) { done = true; });
    const QString dst2 = dir.filePath(QStringLiteral("busy.log"));
    CHECK(unpacker.unpackGzip(src, dst2));
    CHECK(!unpacker.unpackGzip(src, dir.filePath(QStringLiteral("busy2.log"))));
    CHECK(waitFor([&done] { return done; }));
    CHECK(readFile(dst2) == a + b);
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("busy2.log"))));
}

void testUnpackErrorsLeaveNoFile(const QDir& dir)
{
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
    const QByteArray big = gzipMember(makeLog(4 * 1024 * 1024, 3), 1);

    struct Case {
        const char* name;
        QByteArray data;
        GzipDevice::Error expected;
    };
    QByteArray badCrc = big;
    badCrc[badCrc.size() - 6] = char(badCrc[badCrc.size() - 6] ^ 0x10);
    const Case cases[] = {
        {"truncated.gz", big.left(big.size() / 2), GzipDevice::Error::Truncated},
        {"badcrc.gz", badCrc, GzipDevice::Error::CrcMismatch},
        {"plain.gz", appLog(), GzipDevice::Error::NotGzip},
        {"zero.gz", QByteArray(), GzipDevice::Error::NotGzip},
        {"garbage.gz", bytes(kSingleGz) + "tail", GzipDevice::Error::TrailingGarbage},
    };
    for (const Case& c : cases) {
        const QString src = writeBytes(dir, QString::fromLatin1(c.name), c.data);
        const QString dst = dir.filePath(QString::fromLatin1(c.name) + QStringLiteral(".out"));
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::SourceError);
        if (rr.result.gzipError != c.expected)
            std::fprintf(stderr, "  case %s: gzip error %d\n", c.name, int(rr.result.gzipError));
        CHECK(rr.result.gzipError == c.expected);
        CHECK(!rr.result.message.isEmpty());
        CHECK(!QFileInfo::exists(dst));
    }

    // Источник не существует.
    {
        const QString dst = dir.filePath(QStringLiteral("nosrc.out"));
        const RunResult rr = runUnpack(unpacker, dir.filePath(QStringLiteral("nosuch.gz")), dst);
        CHECK(rr.result.error == SourceUnpacker::Error::SourceError);
        CHECK(rr.result.gzipError == GzipDevice::Error::ReadError);
        CHECK(!QFileInfo::exists(dst));
    }
    // Целевой каталог не существует — ошибка записи.
    {
        const QString src = writeBytes(dir, QStringLiteral("ok.gz"), bytes(kSingleGz));
        const QString dst = dir.filePath(QStringLiteral("no/such/dir/out.log"));
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::WriteFailed);
        CHECK(!QFileInfo::exists(dst));
    }
}

void testCancel(const QDir& dir)
{
    const QByteArray data = makeLog(12 * 1024 * 1024, 11);
    const QString src = writeBytes(dir, QStringLiteral("cancel.log.gz"), gzipMember(data, 1));

    // Отмена посреди распаковки — детерминированно: функцию места зовут на
    // каждом интервале (1 МБ), на третьем вызове (уже записано 2 МБ) она
    // отменяет задание из воркера.
    {
        SourceUnpacker unpacker;
        unpacker.setCheckIntervalBytes(1024 * 1024);
        const QString dst = dir.filePath(QStringLiteral("cancel.log"));
        auto calls = std::make_shared<std::atomic<int>>(0);
        unpacker.setFreeSpaceFunction([calls, &unpacker](const QString&) {
            if (calls->fetch_add(1) == 3)
                unpacker.cancel();
            return qint64(1) << 50;
        });
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::Cancelled);
        CHECK(rr.result.bytesWritten > 0);
        CHECK(rr.result.bytesWritten < data.size());
        CHECK(!QFileInfo::exists(dst));
    }
    // Отмена с GUI-потока по первому прогрессу.
    {
        SourceUnpacker unpacker;
        unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
        const QString dst = dir.filePath(QStringLiteral("cancel2.log"));
        QObject ctx;
        QObject::connect(&unpacker, &SourceUnpacker::progress, &ctx,
                         [&unpacker](qint64, qint64) { unpacker.cancel(); });
        const RunResult rr = runUnpack(unpacker, src, dst);
        // Вход мог успеть распаковаться целиком — тогда файл обязан быть
        // полным; иначе — отмена без файла.
        if (rr.result.ok()) {
            CHECK(readFile(dst) == data);
        } else {
            CHECK(rr.result.error == SourceUnpacker::Error::Cancelled);
            CHECK(!QFileInfo::exists(dst));
        }
    }
    // Уничтожение посреди распаковки: деструктор отменяет и ждёт воркер,
    // частичного файла не остаётся.
    {
        const QString dst = dir.filePath(QStringLiteral("destroyed.log"));
        auto started = std::make_shared<std::atomic<bool>>(false);
        auto unpacker = std::make_unique<SourceUnpacker>();
        unpacker->setCheckIntervalBytes(1024 * 1024);
        unpacker->setFreeSpaceFunction([started](const QString&) {
            started->store(true);
            QThread::msleep(5); // пусть уничтожение застанет воркер в работе
            return qint64(1) << 50;
        });
        CHECK(unpacker->unpackGzip(src, dst));
        CHECK(waitFor([&started] { return started->load(); }));
        unpacker.reset();
        CHECK(!QFileInfo::exists(dst));
    }
    // Перезапуск после отмены (F5 в шаге 2): cancel + waitForFinished — и
    // новое задание стартует сразу; запоздавший итог прежнего — со своим
    // номером, файл нового — полный.
    {
        SourceUnpacker unpacker;
        unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
        const QString dst = dir.filePath(QStringLiteral("restart.log"));
        QVector<SourceUnpacker::Result> results;
        QObject ctx;
        QObject::connect(&unpacker, &SourceUnpacker::finished, &ctx,
                         [&results](const SourceUnpacker::Result& r) { results.append(r); });
        const quint64 first = unpacker.unpackGzip(src, dst);
        CHECK(first != 0);
        unpacker.cancel();
        unpacker.waitForFinished();
        CHECK(!unpacker.isRunning());
        const quint64 second = unpacker.unpackGzip(src, dst);
        CHECK(second != 0);
        CHECK(second != first);
        CHECK(waitFor([&results] { return results.size() == 2; }));
        if (results.size() == 2) {
            CHECK(results[0].job == first);
            CHECK(results[0].error == SourceUnpacker::Error::Cancelled || results[0].ok());
            CHECK(results[1].job == second);
            CHECK(results[1].ok());
        }
        CHECK(readFile(dst) == data);
    }
    // Отмена посреди входа без вывода (gzip + 48 МБ нулей): прерывание
    // приходит из GzipDevice, а не после конца файла.
    {
        const QString padded = writeBytes(dir, QStringLiteral("padded.gz"),
                                          bytes(kSingleGz) + QByteArray(48 * 1024 * 1024, '\0'));
        SourceUnpacker unpacker;
        const QString dst = dir.filePath(QStringLiteral("padded.log"));
        // Функция места зовётся до начала — из воркера; отмена в ней
        // поднимает флаг ДО первого чтения, и цикл обязан выйти по нему.
        unpacker.setFreeSpaceFunction([&unpacker](const QString&) {
            unpacker.cancel();
            return qint64(1) << 50;
        });
        const RunResult rr = runUnpack(unpacker, padded, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::Cancelled);
        CHECK(!QFileInfo::exists(dst));
    }
}

void testSameSourceAndTarget(const QDir& dir)
{
    const QByteArray gz = bytes(kNamedGz);
    const QString src = writeBytes(dir, QStringLiteral("self.gz"), gz);
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
    const RunResult rr = runUnpack(unpacker, src, src);
    CHECK(rr.result.error == SourceUnpacker::Error::WriteFailed);
    CHECK(readFile(src) == gz); // источник цел
    const RunResult rr2 = runUnpack(unpacker, src, dir.filePath(QStringLiteral("./self.gz")));
    CHECK(rr2.result.error == SourceUnpacker::Error::WriteFailed);
    CHECK(readFile(src) == gz);
}

void testDiskSpace(const QDir& dir)
{
    // ДО начала: честная ISIZE больше свободного места — отказ, файл не создан.
    const QByteArray zeros(48 * 1024 * 1024, '\0');
    const QByteArray bomb = gzipMember(zeros, 9);
    CHECK(bomb.size() < 256 * 1024); // поток нулей сжимается в сотни раз
    const QString src = writeBytes(dir, QStringLiteral("zeros.gz"), bomb);
    {
        SourceUnpacker unpacker;
        unpacker.setReserveBytes(8 * 1024 * 1024);
        const QString dst = dir.filePath(QStringLiteral("zeros.out"));
        auto calls = std::make_shared<std::atomic<int>>(0);
        unpacker.setFreeSpaceFunction(fakeDisk(40 * 1024 * 1024, dst, calls));
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::NoSpace);
        CHECK_EQ(rr.result.requiredBytes, qint64(zeros.size()) + 8 * 1024 * 1024);
        CHECK_EQ(rr.result.availableBytes, qint64(40 * 1024 * 1024));
        CHECK_EQ(rr.result.bytesWritten, qint64(0));
        CHECK(!rr.result.message.isEmpty());
        CHECK_EQ(calls->load(), 1);
        CHECK(!QFileInfo::exists(dst));
    }
    // ПО ХОДУ: «бомба» с ложной ISIZE (как у потока больше 4 ГБ, где поле
    // переполнилось) проходит проверку до начала, но останавливается, когда
    // свободное место падает ниже резерва; диск не заполняется дальше резерва
    // больше чем на интервал проверки. Частичный файл удалён.
    {
        QByteArray lying = bomb;
        lying[lying.size() - 4] = char(1);
        lying[lying.size() - 3] = lying[lying.size() - 2] = lying[lying.size() - 1] = char(0);
        const QString lyingSrc = writeBytes(dir, QStringLiteral("bomb.gz"), lying);
        CHECK_EQ(GzipDevice::trailerSize(lyingSrc), qint64(1));

        SourceUnpacker unpacker;
        const qint64 disk = 24 * 1024 * 1024, reserve = 8 * 1024 * 1024, interval = 1024 * 1024;
        unpacker.setReserveBytes(reserve);
        unpacker.setCheckIntervalBytes(interval);
        const QString dst = dir.filePath(QStringLiteral("bomb.out"));
        auto maxWritten = std::make_shared<std::atomic<qint64>>(0);
        unpacker.setFreeSpaceFunction([disk, dst, maxWritten](const QString&) {
            const qint64 size = QFileInfo(dst).size();
            if (size > maxWritten->load())
                maxWritten->store(size);
            return disk - size;
        });
        const RunResult rr = runUnpack(unpacker, lyingSrc, dst);
        CHECK(rr.result.error == SourceUnpacker::Error::NoSpace);
        CHECK(rr.result.availableBytes < reserve);
        CHECK(rr.result.bytesWritten <= disk - reserve + interval);
        CHECK(maxWritten->load() <= disk - reserve + interval);
        CHECK(rr.result.bytesWritten < zeros.size());
        CHECK(!QFileInfo::exists(dst));
    }
    // Неизвестное свободное место (-1) проверку не блокирует.
    {
        SourceUnpacker unpacker;
        unpacker.setFreeSpaceFunction([](const QString&) { return qint64(-1); });
        const QString dst = dir.filePath(QStringLiteral("unknown.out"));
        const RunResult rr = runUnpack(unpacker, src, dst);
        CHECK(rr.result.ok());
        CHECK_EQ(QFileInfo(dst).size(), qint64(zeros.size()));
        QFile::remove(dst);
    }
}

// ===========================================================================
// SpoolDir / SpoolItem
// ===========================================================================
int holdSpool(const QString& root)
{
    const std::shared_ptr<SpoolDir> dir = SpoolDir::create(root);
    if (!dir)
        return 2;
    const std::shared_ptr<SpoolItem> item = dir->createFile(QStringLiteral("held.log"));
    QFile f(item->path());
    if (!f.open(QIODevice::WriteOnly))
        return 3;
    f.write("held\n");
    f.close();
    std::printf("%s\n", qPrintable(dir->path()));
    std::fflush(stdout);
    // Ждать, пока не убьют (или пока родитель не закроет stdin).
    char c = 0;
    while (std::fread(&c, 1, 1, stdin) == 1) {
    }
    return 0;
}

void testSpoolLifetime(const QString& root)
{
    QString err;
    std::shared_ptr<SpoolDir> a = SpoolDir::create(root, &err);
    std::shared_ptr<SpoolDir> b = SpoolDir::create(root, &err);
    CHECK(a && b);
    if (!a || !b)
        return;
    CHECK(a->path() != b->path());
    CHECK(QFileInfo(a->path()).isDir());
    CHECK(QFileInfo::exists(a->path() + QStringLiteral(".lock")));
    CHECK(QFileInfo(a->path()).absolutePath() == QFileInfo(root).absoluteFilePath());

    // Уникальные имена внутри, недопустимые символы заменены.
    std::shared_ptr<SpoolItem> f1 = a->createFile(QStringLiteral("app.log"));
    std::shared_ptr<SpoolItem> f2 = a->createFile(QStringLiteral("app.log"));
    std::shared_ptr<SpoolItem> f3 = a->createFile(QStringLiteral("../evil:name?.log"));
    CHECK(f1 && f2 && f3);
    CHECK(f1->path() != f2->path());
    CHECK(QFileInfo(f3->path()).absolutePath() == a->path());
    CHECK(!QFileInfo(f3->path()).fileName().contains(QLatin1Char(':')));
    for (const auto& item : {f1, f2, f3}) {
        QFile f(item->path());
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    std::shared_ptr<SpoolItem> d1 = a->createDirectory(QStringLiteral("bundle.zip"));
    CHECK(d1 && d1->isDirectory() && QFileInfo(d1->path()).isDir());
    {
        QFile f(QDir(d1->path()).filePath(QStringLiteral("inner.log")));
        CHECK(f.open(QIODevice::WriteOnly));
    }

    // Последняя ссылка на элемент — файл удаляется (в фоне).
    const QString p1 = f1->path(), pd = d1->path();
    std::shared_ptr<SpoolItem> f1copy = f1; // как LogFile в Search Results
    f1.reset();
    CHECK(QFileInfo::exists(p1));
    f1copy.reset();
    d1.reset();
    CHECK(waitFor([&] { return !QFileInfo::exists(p1) && !QFileInfo::exists(pd); }));
    CHECK(QFileInfo::exists(f2->path()));

    // Элементы держат каталог: владелец отпустил его — каталог жив, пока
    // жив элемент; последний элемент уносит и каталог, и блокировку.
    const QString aPath = a->path();
    a.reset();
    CHECK(QFileInfo(aPath).isDir());
    f2.reset();
    f3.reset();
    CHECK(waitFor([&] { return !QFileInfo::exists(aPath) && !QFileInfo::exists(aPath + QStringLiteral(".lock")); }));

    // Каталог без элементов уходит сразу с владельцем.
    const QString bPath = b->path();
    b.reset();
    CHECK(!QFileInfo::exists(bPath));
    CHECK(!QFileInfo::exists(bPath + QStringLiteral(".lock")));

    // Корень, который нельзя создать.
    CHECK(!SpoolDir::create(QString(), &err));
    CHECK(!err.isEmpty());
}

void testOrphans(const QString& root)
{
    std::shared_ptr<SpoolDir> own = SpoolDir::create(root);
    CHECK(own != nullptr);
    if (!own)
        return;

    // Чужие имена уборщик не трогает никогда.
    const QDir rootDir(root);
    CHECK(rootDir.mkdir(QStringLiteral("not-a-spool")));
    // Каталог экземпляра без блокировки — сирота (упали до lock или файл
    // блокировки уже удалён).
    const QString lockless = rootDir.filePath(QStringLiteral("999999-0123456789abcdef"));
    CHECK(QDir().mkpath(lockless + QStringLiteral("/sub")));

    // Живой чужой экземпляр — дочерний процесс.
    QProcess child;
    child.setProcessChannelMode(QProcess::SeparateChannels);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--hold-spool"), root});
    CHECK(child.waitForStarted(30000));
    CHECK(waitFor([&child] { return child.canReadLine() || child.state() == QProcess::NotRunning; }, 60000)
          || child.waitForReadyRead(60000));
    const QString childDir = QString::fromLocal8Bit(child.readLine()).trimmed();
    CHECK(!childDir.isEmpty());
    CHECK(QFileInfo(childDir).isDir());

    CHECK_EQ(SpoolDir::removeOrphans(root), 1); // только каталог без блокировки
    CHECK(!QFileInfo::exists(lockless));
    CHECK(QFileInfo(childDir).isDir());           // живой — на месте
    CHECK(QFileInfo(own->path()).isDir());        // свой — на месте
    CHECK(QFileInfo(rootDir.filePath(QStringLiteral("not-a-spool"))).isDir());

    // Падение: процесс убит, блокировка осталась с мёртвым PID.
    child.kill();
    CHECK(child.waitForFinished(30000));
    CHECK(QFileInfo(childDir).isDir());
    CHECK(QFileInfo::exists(childDir + QStringLiteral(".lock")));

    CHECK_EQ(SpoolDir::removeOrphans(root), 1);
    CHECK(!QFileInfo::exists(childDir));
    CHECK(!QFileInfo::exists(childDir + QStringLiteral(".lock")));
    CHECK(QFileInfo(own->path()).isDir());
    CHECK(QFileInfo::exists(own->path() + QStringLiteral(".lock")));
    CHECK(QFileInfo(rootDir.filePath(QStringLiteral("not-a-spool"))).isDir());

    // Повтор ничего не находит; свой каталог по-прежнему жив.
    CHECK_EQ(SpoolDir::removeOrphans(root), 0);
    CHECK(QFileInfo(own->path()).isDir());

    // Блокировка без каталога от мёртвого экземпляра — убирается.
    const QString strayLock = rootDir.filePath(QStringLiteral("999998-fedcba9876543210.lock"));
    {
        QFile f(strayLock);
        CHECK(f.open(QIODevice::WriteOnly));
        // Формат QLockFile: PID, имя программы, имя машины. Машина — эта,
        // иначе блокировка считается чужой (с другого хоста) и не трогается.
        f.write("999998\nunpack\n" + QSysInfo::machineHostName().toUtf8() + "\n");
    }
    SpoolDir::removeOrphans(root);
    CHECK(!QFileInfo::exists(strayLock));
}

// ===========================================================================
// LogSource и orderKey
// ===========================================================================
void testLogSource()
{
    LogFile plain(QStringLiteral("/logs/b.log"));
    CHECK(plain.source().kind == LogSource::Kind::File);
    CHECK(plain.source().path == plain.filePath);
    CHECK(plain.displayPath() == plain.filePath);
    CHECK(&plain.orderKey() == &plain.filePath); // без копий на сравнение

    // Сжатый файл: читают копию, показывают и упорядочивают по архиву.
    LogFile gz(QStringLiteral("/cache/spool/123-abc/1-a.log"));
    gz.setSource({LogSource::Kind::Archive, QStringLiteral("/logs/a.log.gz"), QString(), QString()});
    CHECK(gz.displayPath() == QStringLiteral("/logs/a.log.gz"));
    CHECK(gz.orderKey() == QStringLiteral("/logs/a.log.gz"));
    CHECK(gz.filePath == QStringLiteral("/cache/spool/123-abc/1-a.log"));

    LogFile member(QStringLiteral("/cache/spool/123-abc/2-x/logs/app.log"));
    member.setSource({LogSource::Kind::Archive, QStringLiteral("/logs/bundle.zip"), QStringLiteral("logs/app.log"),
                      QString()});
    CHECK(member.displayPath() == QStringLiteral("/logs/bundle.zip › logs/app.log"));
    CHECK(member.orderKey().startsWith(QStringLiteral("/logs/bundle.zip")));

    LogFile stream(QStringLiteral("/tmp/DendroLog-stdin-1.log"));
    stream.setSource({LogSource::Kind::Stream, QString(), QString(), QStringLiteral("stdin")});
    CHECK(stream.displayPath() == QStringLiteral("stdin"));
    CHECK(stream.orderKey() == stream.filePath);

    // Порядок при равном времени — по ключу источника, не по пути копии:
    // копия «/cache/…» < «/logs/b.log», но архив «/logs/a.log.gz» тоже меньше,
    // а архив «/logs/c.log.gz» — больше.
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
    auto filePlain = std::make_shared<LogFile>(plain);
    auto fileArchiveA = std::make_shared<LogFile>(gz);
    auto fileArchiveC = std::make_shared<LogFile>(QStringLiteral("/cache/spool/123-abc/3-c.log"));
    fileArchiveC->setSource({LogSource::Kind::Archive, QStringLiteral("/logs/c.log.gz"), QString(), QString()});
    const auto entry = [&t](const LogFilePtr& f) {
        return LogEntry(1, 1, t, LogLevel::Info, QStringLiteral("same"), f);
    };
    CHECK(entry(fileArchiveA) < entry(filePlain));
    CHECK(!(entry(filePlain) < entry(fileArchiveA)));
    CHECK(entry(filePlain) < entry(fileArchiveC));
    CHECK(!(entry(fileArchiveC) < entry(filePlain)));
}

int runBench(const QString& gzPath, const QString& outDir)
{
    const QString target = QDir(outDir).filePath(QStringLiteral("bench-unpacked.log"));
    // 1. Только распаковка (без записи): цена inflate + CRC.
    {
        QElapsedTimer t;
        t.start();
        GzipDevice gz(gzPath);
        if (!gz.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot open %s\n", qPrintable(gzPath));
            return 1;
        }
        QByteArray buf(1 << 20, Qt::Uninitialized);
        qint64 total = 0, n = 0;
        while ((n = gz.read(buf.data(), buf.size())) > 0)
            total += n;
        const double s = t.elapsed() / 1000.0;
        std::printf("inflate only: %lld bytes in %.2f s = %.0f MB/s (error: %s)\n", (long long)total, s,
                    total / 1048576.0 / s, qPrintable(GzipDevice::errorText(gz.error())));
    }
    // 2. SourceUnpacker целиком: чтение, inflate, CRC, запись на диск.
    SourceUnpacker unpacker;
    QElapsedTimer t;
    t.start();
    bool done = false;
    SourceUnpacker::Result result;
    QObject::connect(&unpacker, &SourceUnpacker::finished, [&](const SourceUnpacker::Result& r) {
        result = r;
        done = true;
    });
    unpacker.unpackGzip(gzPath, target);
    waitFor([&done] { return done; }, 3600000);
    const double s = t.elapsed() / 1000.0;
    std::printf("SourceUnpacker: %lld bytes in %.2f s = %.0f MB/s (%s)\n", (long long)result.bytesWritten, s,
                result.bytesWritten / 1048576.0 / s, result.ok() ? "ok" : qPrintable(result.message));
    QFile::remove(target);
    return result.ok() ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args.at(1) == QStringLiteral("--hold-spool"))
        return holdSpool(args.at(2));
    if (args.size() >= 4 && args.at(1) == QStringLiteral("--bench"))
        return runBench(args.at(2), args.at(3));

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "no temporary directory\n");
        return 1;
    }
    const QDir dir(tmp.path());
    CHECK(dir.mkdir(QStringLiteral("files")));
    const QDir files(dir.filePath(QStringLiteral("files")));

    const auto stage = [](const char* name, const std::function<void()>& fn) {
        QElapsedTimer t;
        t.start();
        const int before = g_failures;
        fn();
        std::printf("%-28s %s (%lld ms)\n", name, g_failures == before ? "ok" : "FAILED", (long long)t.elapsed());
        std::fflush(stdout);
    };
    stage("gzip: real fixtures", testRealFixtures);
    stage("gzip: broken inputs", testBrokenInputs);
    stage("gzip: header fields", testHeaderFields);
    stage("gzip: large streams", testLargeStreams);
    stage("gzip: abort, long fields", testAbortAndLongFields);
    stage("gzip: external source", [&] { testExternalSourceReopen(files); });
    stage("gzip: signature helpers", [&] { testSignatureHelpers(files); });
    stage("unpack: ok", [&] { testUnpackOk(files); });
    stage("unpack: errors", [&] { testUnpackErrorsLeaveNoFile(files); });
    stage("unpack: cancel", [&] { testCancel(files); });
    stage("unpack: disk space", [&] { testDiskSpace(files); });
    stage("unpack: source == target", [&] { testSameSourceAndTarget(files); });
    stage("spool: lifetime", [&] { testSpoolLifetime(dir.filePath(QStringLiteral("spool1"))); });
    stage("spool: orphans", [&] { testOrphans(dir.filePath(QStringLiteral("spool2"))); });
    stage("logfile: source", testLogSource);

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all unpack checks passed\n");
    return 0;
}
