// Тест ядра сжатых источников (F-2, шаги 1, 4 и 5): GzipDevice, ZipArchive,
// TarArchive, SourceUnpacker (gzip, zip и tar), SpoolDir / SpoolItem. Без виджетов и без окна.
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
#include "recordid.h"
#include "tararchive.h"
#include "ziparchive.h"

#include "../common/tarbytes.h"
#include "../common/zipbytes.h"
#include "tarfixtures.h"

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
// zip (F-2, шаг 4)
// ===========================================================================

// Настоящий zip — Info-ZIP 3.0 (`zip -X -r`, затем `zip -0` для
// logs/sub/debug.log): каталоги, deflate и stored.
const unsigned char kInfoZip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79, 0x5d, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x67, 0x73, 0x2f, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79,
    0x5d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
    0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x73, 0x75, 0x62, 0x2f, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79, 0x5d, 0x8b, 0xd7, 0xf4, 0x75, 0x12, 0x00, 0x00, 0x00,
    0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x73, 0x75, 0x62,
    0x2f, 0x64, 0x65, 0x62, 0x75, 0x67, 0x2e, 0x6c, 0x6f, 0x67, 0x6c, 0x69, 0x6e, 0x65, 0x20, 0x6f,
    0x6e, 0x65, 0x0a, 0x6c, 0x69, 0x6e, 0x65, 0x20, 0x74, 0x77, 0x6f, 0x0a, 0x50, 0x4b, 0x03, 0x04,
    0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x21, 0xa0, 0x79, 0x5d, 0xc6, 0x3c, 0x54, 0x43, 0x2d, 0x00,
    0x00, 0x00, 0x3e, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x61,
    0x70, 0x70, 0x2e, 0x6c, 0x6f, 0x67, 0x33, 0x32, 0x30, 0x32, 0xd3, 0x35, 0x30, 0x04, 0x22, 0x05,
    0x43, 0x03, 0x2b, 0x03, 0x10, 0x52, 0xf0, 0xf4, 0x73, 0xf3, 0x57, 0x28, 0x2e, 0x49, 0x2c, 0x2a,
    0xe1, 0x32, 0xc2, 0x90, 0x36, 0x54, 0x70, 0x0d, 0x0a, 0xf2, 0x0f, 0x52, 0x48, 0xca, 0xcf, 0xcf,
    0xe5, 0x02, 0x00, 0x50, 0x4b, 0x03, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79,
    0x5d, 0x3e, 0xd2, 0xd8, 0xd6, 0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00,
    0x00, 0x72, 0x65, 0x61, 0x64, 0x6d, 0x65, 0x2e, 0x6d, 0x64, 0x6e, 0x6f, 0x74, 0x65, 0x73, 0x0a,
    0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79, 0x5d,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xed, 0x41, 0x00, 0x00, 0x00, 0x00, 0x6c, 0x6f,
    0x67, 0x73, 0x2f, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21,
    0xa0, 0x79, 0x5d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0xed, 0x41, 0x23, 0x00, 0x00,
    0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x73, 0x75, 0x62, 0x2f, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03,
    0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0xa0, 0x79, 0x5d, 0x8b, 0xd7, 0xf4, 0x75, 0x12, 0x00,
    0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xa4, 0x81, 0x4a, 0x00, 0x00, 0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x73, 0x75, 0x62,
    0x2f, 0x64, 0x65, 0x62, 0x75, 0x67, 0x2e, 0x6c, 0x6f, 0x67, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03,
    0x14, 0x00, 0x00, 0x00, 0x08, 0x00, 0x21, 0xa0, 0x79, 0x5d, 0xc6, 0x3c, 0x54, 0x43, 0x2d, 0x00,
    0x00, 0x00, 0x3e, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x00, 0xa4, 0x81, 0x8c, 0x00, 0x00, 0x00, 0x6c, 0x6f, 0x67, 0x73, 0x2f, 0x61, 0x70, 0x70,
    0x2e, 0x6c, 0x6f, 0x67, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x21, 0xa0, 0x79, 0x5d, 0x3e, 0xd2, 0xd8, 0xd6, 0x06, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0xe3, 0x00,
    0x00, 0x00, 0x72, 0x65, 0x61, 0x64, 0x6d, 0x65, 0x2e, 0x6d, 0x64, 0x50, 0x4b, 0x05, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x05, 0x00, 0x1b, 0x01, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00,
    0x00,
};

const QByteArray kInfoAppLog("2026-01-01 10:00:00 INFO start\n2026-01-01 10:00:01 ERROR boom\n");
const QByteArray kInfoDebugLog("line one\nline two\n");

QByteArray infoZip() { return QByteArray(reinterpret_cast<const char*>(kInfoZip), int(sizeof(kInfoZip))); }

struct ZipRun {
    SourceUnpacker::Result result;
    qint64 lastProgress = -1;
    qint64 progressTotal = -1;
};

ZipRun runJob(SourceUnpacker& unpacker, const std::function<quint64()>& startJob)
{
    ZipRun zr;
    bool done = false;
    QObject ctx;
    QObject::connect(&unpacker, &SourceUnpacker::progress, &ctx, [&zr](qint64 d, qint64 t) {
        CHECK(d >= zr.lastProgress);
        zr.lastProgress = d;
        zr.progressTotal = t;
    });
    QObject::connect(&unpacker, &SourceUnpacker::finished, &ctx,
                     [&zr, &done](const SourceUnpacker::Result& r) {
                         zr.result = r;
                         done = true;
                     });
    CHECK(startJob() != 0);
    CHECK(waitFor([&done] { return done; }));
    return zr;
}

bool logOrGz(const QString& member)
{
    const QString name = QFileInfo(member).fileName().toLower();
    return name.endsWith(QLatin1String(".log")) || name.endsWith(QLatin1String(".log.gz"));
}

QStringList filesUnder(const QString& root)
{
    QStringList out;
    QDir base(root);
    std::function<void(const QString&)> walk = [&](const QString& dir) {
        const QFileInfoList list = QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& fi : list) {
            if (fi.isDir() && !fi.isSymLink())
                walk(fi.absoluteFilePath());
            else
                out << base.relativeFilePath(fi.absoluteFilePath());
        }
    };
    walk(root);
    out.sort();
    return out;
}

void testZipReader(const QDir& dir)
{
    const QString path = writeBytes(dir, QStringLiteral("info.zip"), infoZip());
    CHECK(ZipArchive::fileHasSignature(path));
    CHECK(!GzipDevice::fileHasSignature(path));
    CHECK(ZipArchive::hasSignature(testzip::zip({})));          // пустой архив — «PK\5\6»
    CHECK(!ZipArchive::hasSignature(QByteArray("PK")));
    CHECK(!ZipArchive::fileHasSignature(writeBytes(dir, QStringLiteral("plain.zip"), "hello\n")));

    ZipArchive zip(path);
    CHECK(zip.open());
    CHECK_EQ(zip.members().size(), qsizetype(5));
    const int app = zip.find(QStringLiteral("logs/app.log"));
    const int debug = zip.find(QStringLiteral("logs/sub/debug.log"));
    CHECK(app >= 0 && debug >= 0);
    CHECK(zip.find(QStringLiteral("logs/")) >= 0 && zip.members().at(zip.find(QStringLiteral("logs/"))).directory);
    CHECK(zip.find(QStringLiteral("nope.log")) < 0);
    for (const auto& [index, expected] : {std::make_pair(app, kInfoAppLog), std::make_pair(debug, kInfoDebugLog)}) {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        CHECK(zip.extract(zip.members().at(index), buffer));
        CHECK(buffer.data() == expected);
    }

    // Имена: UTF-8 с флагом и без, OEM (IBM866) без флага.
    testzip::Entry utf8 = testzip::file(QStringLiteral("журнал.log").toUtf8(), "a\n");
    testzip::Entry utf8NoFlag = testzip::file(QStringLiteral("отчёт.log").toUtf8(), "b\n");
    utf8NoFlag.utf8Flag = false;
    testzip::Entry oem = testzip::file(QByteArray("\x8B\xAE\xA3.log"), "c\n"); // «Лог» в cp866
    oem.utf8Flag = false;
    const QString names = writeBytes(dir, QStringLiteral("names.zip"), testzip::zip({utf8, utf8NoFlag, oem}));
    ZipArchive named(names);
    CHECK(named.open());
    CHECK(named.find(QStringLiteral("журнал.log")) >= 0);
    CHECK(named.find(QStringLiteral("отчёт.log")) >= 0);
    CHECK(named.find(QStringLiteral("Лог.log")) >= 0);

    // Повреждения: не zip, обрезан, неверный CRC, данные длиннее заявленного.
    ZipArchive notZip(writeBytes(dir, QStringLiteral("text.zip"), "just text\n"));
    CHECK(!notZip.open());
    CHECK(notZip.error() == ZipArchive::Error::NotZip);
    QByteArray cut = infoZip();
    cut.chop(30);
    ZipArchive truncated(writeBytes(dir, QStringLiteral("cut.zip"), cut));
    CHECK(!truncated.open());
    CHECK(truncated.error() == ZipArchive::Error::Corrupt || truncated.error() == ZipArchive::Error::NotZip);

    testzip::Entry badCrc = testzip::file("bad.log", makeLog(100000, 5));
    badCrc.badCrc = true;
    testzip::Entry badCrcStored = testzip::file("bad-stored.log", "stored data\n", false);
    badCrcStored.badCrc = true;
    testzip::Entry bomb = testzip::file("bomb.log", QByteArray(8 * 1024 * 1024, '\0'));
    bomb.declaredSize = 1000; // разворачивается больше заявленного
    ZipArchive broken(writeBytes(dir, QStringLiteral("broken.zip"), testzip::zip({badCrc, badCrcStored, bomb})));
    CHECK(broken.open());
    for (const QString& name : {QStringLiteral("bad.log"), QStringLiteral("bad-stored.log"), QStringLiteral("bomb.log")}) {
        QBuffer buffer;
        buffer.open(QIODevice::WriteOnly);
        CHECK(!broken.extract(broken.members().at(broken.find(name)), buffer));
        CHECK(broken.error() == ZipArchive::Error::Corrupt);
        CHECK(buffer.size() <= 8 * 1024 * 1024);
    }
    // Флаг прерывания.
    std::atomic_bool abort{true};
    ZipArchive info(path);
    CHECK(info.open());
    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    CHECK(!info.extract(info.members().at(info.find(QStringLiteral("logs/app.log"))), sink, &abort));
    CHECK(info.error() == ZipArchive::Error::Aborted);
}

void testArchiveNames()
{
    const auto safe = [](const char* name) { return ArchiveNames::safeRelativePath(QString::fromUtf8(name)); };
    CHECK(safe("app.log") == QStringLiteral("app.log"));
    CHECK(safe("logs/sub/app.log") == QStringLiteral("logs/sub/app.log"));
    CHECK(safe("logs\\win\\app.log") == QStringLiteral("logs/win/app.log"));
    CHECK(safe("./logs//app.log") == QStringLiteral("logs/app.log"));
    // Отбрасываются.
    for (const char* bad : {"", "logs/", "/etc/passwd.log", "\\\\server\\share\\x.log", "C:\\x.log", "c:x.log",
                            "../x.log", "a/../../x.log", "logs/../x.log", "..\\x.log", "a/..", "..", "/"})
        CHECK(safe(bad).isEmpty());
    // Заменяются.
    CHECK(safe("bad:name?.log") == QStringLiteral("bad_name_.log"));
    CHECK(safe("a<b>|\"*.log") == QStringLiteral("a_b___.log") || safe("a<b>|\"*.log") == QStringLiteral("a_b____.log"));
    CHECK(safe("tab\there.log") == QStringLiteral("tab_here.log"));
    CHECK(safe("con.log") == QStringLiteral("_con.log"));
    CHECK(safe("logs/NUL") == QStringLiteral("logs/_NUL"));
    CHECK(safe("Com1.txt") == QStringLiteral("_Com1.txt"));
    CHECK(safe("com10.txt") == QStringLiteral("com10.txt"));
    CHECK(safe("console.log") == QStringLiteral("console.log"));
    CHECK(safe("trailing.log. ") == QStringLiteral("trailing.log"));
    CHECK(safe("dir./x.log") == QStringLiteral("dir/x.log"));
    CHECK(safe(".../x.log") == QStringLiteral("_/x.log"));
}

void testUnpackZip(const QDir& dir)
{
    // Набор: вложенные каталоги, опасные имена, ссылка, пароль,
    // зарезервированное и недопустимое имя, повтор по регистру, .gz внутри,
    // не-лог (отсеивает фильтр).
    const QByteArray nested = makeLog(300000, 21);
    QList<testzip::Entry> entries = {
        testzip::file("logs/app.log", kInfoAppLog),
        testzip::file("logs/deep/er/x.log", nested),
        testzip::file("stored.log", "stored\n", false),
        testzip::file("../evil.log", "evil\n"),
        testzip::file("/abs.log", "abs\n"),
        testzip::file("C:\\win.log", "win\n"),
        testzip::file("..\\back.log", "back\n"),
        testzip::file("a/../../up.log", "up\n"),
        testzip::file("con.log", "reserved\n"),
        testzip::file("bad:name?.log", "bad name\n"),
        testzip::file("Case.log", "upper\n"),
        testzip::file("case.log", "lower\n"),
        testzip::file("inner/app.log.gz", gzipMember(kInfoAppLog, 6)),
        testzip::file("readme.md", "not a log\n"),
    };
    testzip::Entry link = testzip::file("link.log", "/etc/passwd", false);
    link.symlink = true;
    testzip::Entry secret = testzip::file("secret.log", "xxxx", false);
    secret.encrypted = true;
    testzip::Entry folder;
    folder.name = "logs/";
    folder.directory = true;
    entries << link << secret << folder;
    const QString zipPath = writeBytes(dir, QStringLiteral("bundle.zip"), testzip::zip(entries));
    const QString outer = dir.filePath(QStringLiteral("zipout"));
    QDir().mkpath(outer);
    const QString target = outer + QStringLiteral("/bundle");

    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
    const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackZip(zipPath, target, &logOrGz); });
    CHECK(zr.result.ok());
    if (!zr.result.ok())
        std::fprintf(stderr, "  unpackZip: %s\n", qPrintable(zr.result.message));
    CHECK_EQ(zr.result.skippedUnsafe, 6); // 5 имён + ссылка
    CHECK_EQ(zr.result.skippedEncrypted, 1);
    // Ничего не вышло за каталог извлечения.
    CHECK(filesUnder(outer).size() == filesUnder(target).size());
    const QStringList expected = {
        QStringLiteral("Case.log"), QStringLiteral("_con.log"), QStringLiteral("bad_name_.log"),
        QStringLiteral("case~2.log"), QStringLiteral("inner/app.log.gz"), QStringLiteral("logs/app.log"),
        QStringLiteral("logs/deep/er/x.log"), QStringLiteral("stored.log"),
    };
    QStringList sortedExpected = expected;
    sortedExpected.sort();
    CHECK(filesUnder(target) == sortedExpected);
    if (filesUnder(target) != sortedExpected)
        std::fprintf(stderr, "  files: %s\n", qPrintable(filesUnder(target).join(QStringLiteral(", "))));
    CHECK(readFile(target + QStringLiteral("/logs/deep/er/x.log")) == nested);
    CHECK(readFile(target + QStringLiteral("/stored.log")) == QByteArray("stored\n"));
    CHECK(readFile(target + QStringLiteral("/case~2.log")) == QByteArray("lower\n"));
    CHECK(readFile(target + QStringLiteral("/inner/app.log.gz")) == gzipMember(kInfoAppLog, 6)); // .gz остаётся файлом
    // Извлечённые: имя в архиве → путь в каталоге.
    QHash<QString, QString> map;
    for (const auto& e : zr.result.extracted)
        map.insert(e.member, e.relativePath);
    CHECK_EQ(map.size(), qsizetype(8));
    CHECK(map.value(QStringLiteral("con.log")) == QStringLiteral("_con.log"));
    CHECK(map.value(QStringLiteral("case.log")) == QStringLiteral("case~2.log"));
    CHECK(map.value(QStringLiteral("logs/app.log")) == QStringLiteral("logs/app.log"));
    CHECK_EQ(zr.lastProgress, zr.progressTotal);
    CHECK(QFileInfo::exists(zipPath)); // источник цел

    // Два члена с одним именем: извлекается первый, второй — пропуск (член
    // ищется по имени, второй открылся бы первым).
    const QString dupZip = writeBytes(dir, QStringLiteral("dup.zip"),
        testzip::zip({testzip::file("app.log", "first\n"), testzip::file("app.log", "second\n")}));
    const QString dupTarget = outer + QStringLiteral("/dup");
    ZipRun dr = runJob(unpacker, [&] { return unpacker.unpackZip(dupZip, dupTarget, &logOrGz); });
    CHECK(dr.result.ok() && dr.result.extracted.size() == 1 && dr.result.skippedUnsafe == 1);
    CHECK(filesUnder(dupTarget) == QStringList{QStringLiteral("app.log")});
    CHECK(readFile(dupTarget + QStringLiteral("/app.log")) == QByteArray("first\n"));
    // Не zip — каталог, созданный заранее вызывающим, тоже убран.
    const QString preTarget = outer + QStringLiteral("/pre");
    QDir().mkpath(preTarget);
    dr = runJob(unpacker, [&] { return unpacker.unpackZip(writeBytes(dir, QStringLiteral("nz.zip"), "text\n"),
                                                          preTarget, &logOrGz); });
    CHECK(dr.result.error == SourceUnpacker::Error::SourceError && !QFileInfo::exists(preTarget));

    // Повреждённый член — каталог удалён целиком.
    testzip::Entry bad = testzip::file("z-bad.log", "data\n");
    bad.badCrc = true;
    const QString badZip = writeBytes(dir, QStringLiteral("badcrc.zip"),
                                      testzip::zip({testzip::file("a.log", "ok\n"), bad}));
    const QString badTarget = outer + QStringLiteral("/bad");
    ZipRun br = runJob(unpacker, [&] { return unpacker.unpackZip(badZip, badTarget, &logOrGz); });
    CHECK(br.result.error == SourceUnpacker::Error::SourceError);
    CHECK(br.result.message.contains(QStringLiteral("z-bad.log")));
    CHECK(!QFileInfo::exists(badTarget));
    CHECK(br.result.extracted.isEmpty());
    // Не zip.
    const QString textZip = writeBytes(dir, QStringLiteral("text2.zip"), "plain text\n");
    br = runJob(unpacker, [&] { return unpacker.unpackZip(textZip, outer + QStringLiteral("/t"), &logOrGz); });
    CHECK(br.result.error == SourceUnpacker::Error::SourceError);
    CHECK(!QFileInfo::exists(outer + QStringLiteral("/t")));
    // Пустой архив — успех без файлов.
    const QString emptyZip = writeBytes(dir, QStringLiteral("empty.zip"), testzip::zip({}));
    br = runJob(unpacker, [&] { return unpacker.unpackZip(emptyZip, outer + QStringLiteral("/e"), &logOrGz); });
    CHECK(br.result.ok());
    CHECK(br.result.extracted.isEmpty());
    // Член с паролем — только он: архив открывается пустым, пропуск посчитан.
    const QString onlySecret = writeBytes(dir, QStringLiteral("secret.zip"), testzip::zip({secret}));
    br = runJob(unpacker, [&] { return unpacker.unpackZip(onlySecret, outer + QStringLiteral("/s"), &logOrGz); });
    CHECK(br.result.ok());
    CHECK_EQ(br.result.skippedEncrypted, 1);
}

void testZipSpaceAndCancel(const QDir& dir)
{
    const QByteArray a = makeLog(3 * 1024 * 1024, 31);
    const QByteArray b = makeLog(5 * 1024 * 1024, 32);
    const QString zipPath = writeBytes(dir, QStringLiteral("space.zip"),
        testzip::zip({testzip::file("a.log", a), testzip::file("b.log", b), testzip::file("skip.md", makeLog(1 << 20, 1))}));
    const QString outer = dir.filePath(QStringLiteral("zipspace"));
    QDir().mkpath(outer);

    // ДО начала: сумма отобранных (не всех) членов + резерв.
    {
        SourceUnpacker unpacker;
        unpacker.setReserveBytes(1024 * 1024);
        auto calls = std::make_shared<std::atomic<int>>(0);
        unpacker.setFreeSpaceFunction([calls](const QString&) {
            calls->fetch_add(1);
            return qint64(8 * 1024 * 1024);
        });
        const QString target = outer + QStringLiteral("/before");
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackZip(zipPath, target, &logOrGz); });
        CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
        CHECK_EQ(zr.result.requiredBytes, qint64(a.size() + b.size()) + 1024 * 1024);
        CHECK_EQ(zr.result.availableBytes, qint64(8 * 1024 * 1024));
        CHECK_EQ(calls->load(), 1);
        CHECK(!QFileInfo::exists(target));
    }
    // ПО ХОДУ: диск «заполняется» распаковкой; остановка у резерва.
    {
        SourceUnpacker unpacker;
        const qint64 reserve = 1024 * 1024;
        unpacker.setReserveBytes(reserve);
        unpacker.setCheckIntervalBytes(1024 * 1024);
        const QString target = outer + QStringLiteral("/during");
        auto calls = std::make_shared<std::atomic<int>>(0);
        unpacker.setFreeSpaceFunction([calls, target](const QString&) {
            // Первый вызов — до начала: места хватает; дальше — «чужая
            // программа» съела диск.
            return calls->fetch_add(1) == 0 ? qint64(1) << 40 : qint64(512 * 1024);
        });
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackZip(zipPath, target, &logOrGz); });
        CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
        CHECK(zr.result.bytesWritten < a.size() + b.size());
        CHECK(!QFileInfo::exists(target));
    }
    // Отмена посреди — каталог удалён.
    {
        SourceUnpacker unpacker;
        unpacker.setCheckIntervalBytes(1024 * 1024);
        auto calls = std::make_shared<std::atomic<int>>(0);
        SourceUnpacker* self = &unpacker;
        unpacker.setFreeSpaceFunction([calls, self](const QString&) {
            if (calls->fetch_add(1) == 2)
                self->cancel(); // атомарный флаг: можно из воркера
            return qint64(1) << 40;
        });
        const QString target = outer + QStringLiteral("/cancel");
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackZip(zipPath, target, &logOrGz); });
        CHECK(zr.result.error == SourceUnpacker::Error::Cancelled);
        CHECK(!QFileInfo::exists(target));
    }
}

void testUnpackZipMember(const QDir& dir)
{
    const QByteArray big = makeLog(2 * 1024 * 1024, 41);
    testzip::Entry secret = testzip::file("secret.log", "xxxx", false);
    secret.encrypted = true;
    testzip::Entry link = testzip::file("link.log", "/etc/passwd", false);
    link.symlink = true;
    const QString zipPath = writeBytes(dir, QStringLiteral("members.zip"), testzip::zip({
        testzip::file("logs/app.log", kInfoAppLog),
        testzip::file("logs/big.log", big),
        testzip::file("logs/app.log.gz", gzipMember(big, 1) + gzipMember(kInfoAppLog, 6)),
        testzip::file(QStringLiteral("журнал.log").toUtf8(), "кириллица\n"),
        secret, link}));
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });

    const auto extract = [&](const QString& member, const QString& name) {
        const QString target = dir.filePath(name);
        return std::make_pair(runJob(unpacker, [&] { return unpacker.unpackZipMember(zipPath, member, target); }), target);
    };
    {
        const auto [zr, target] = extract(QStringLiteral("logs/app.log"), QStringLiteral("m-app.log"));
        CHECK(zr.result.ok());
        CHECK(readFile(target) == kInfoAppLog);
    }
    {
        const auto [zr, target] = extract(QStringLiteral("logs/big.log"), QStringLiteral("m-big.log"));
        CHECK(zr.result.ok());
        CHECK(readFile(target) == big);
        CHECK_EQ(zr.lastProgress, zr.progressTotal);
    }
    {
        // .gz внутри zip — текст в файле, промежуточного не осталось.
        const auto [zr, target] = extract(QStringLiteral("logs/app.log.gz"), QStringLiteral("m-gz.log"));
        CHECK(zr.result.ok());
        CHECK(readFile(target) == big + kInfoAppLog);
        CHECK(!QFileInfo::exists(target + QStringLiteral(".gz-part")));
    }
    {
        const auto [zr, target] = extract(QStringLiteral("журнал.log"), QStringLiteral("m-cyr.log"));
        CHECK(zr.result.ok());
        CHECK(readFile(target) == QStringLiteral("кириллица\n").toUtf8());
    }
    // Ошибки — файла нет.
    for (const char* member : {"missing.log", "secret.log", "link.log", "logs/"}) {
        const auto [zr, target] = extract(QString::fromUtf8(member), QStringLiteral("m-err.log"));
        CHECK(zr.result.error == SourceUnpacker::Error::SourceError);
        CHECK(!zr.result.message.isEmpty());
        CHECK(!QFileInfo::exists(target));
    }
    // Место до начала — размер члена.
    {
        unpacker.setReserveBytes(0);
        unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1000); });
        const auto [zr, target] = extract(QStringLiteral("logs/big.log"), QStringLiteral("m-space.log"));
        CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
        CHECK_EQ(zr.result.requiredBytes, qint64(big.size()));
        CHECK(!QFileInfo::exists(target));
    }
}

void testZipIdentity(const QDir& dir)
{
    // Идентичность члена zip (F-2, шаг 4): путь — архив, member — имя,
    // checkAt — сигнатура и отпечатки zip, а не gzip.
    const QString zipPath = writeBytes(dir, QStringLiteral("ident.zip"),
                                       testzip::zip({testzip::file("logs/app.log", kInfoAppLog)}));
    const QString copy = writeBytes(dir, QStringLiteral("ident-copy.log"), kInfoAppLog);
    LogFile file(copy);
    LogSource source;
    source.kind = LogSource::Kind::Archive;
    source.path = zipPath;
    source.member = QStringLiteral("logs/app.log");
    file.setSource(source);
    CHECK(file.displayPath().endsWith(QStringLiteral("ident.zip \u203A logs/app.log")));
    const auto id = SourceIdentity::capture(file);
    CHECK(id.has_value());
    CHECK(id && id->compressed && id->member == QStringLiteral("logs/app.log"));
    CHECK(id && id->check() == SourceIdentity::State::Valid);
    CHECK(id && id->checkContentAt(copy) == SourceIdentity::State::Valid);
    const auto json = id ? SourceIdentity::fromJson(id->toJson()) : std::nullopt;
    CHECK(json && *json == *id);
    // Другой zip по тому же пути — Changed; не zip — Missing.
    writeBytes(dir, QStringLiteral("ident.zip"), testzip::zip({testzip::file("logs/app.log", "other\n")}));
    CHECK(id && id->check() == SourceIdentity::State::Changed);
    writeBytes(dir, QStringLiteral("ident.zip"), kInfoAppLog);
    CHECK(id && id->check() == SourceIdentity::State::Missing);
    // Архив ещё не прочитан (копия пишется): член переживает снимок.
    writeBytes(dir, QStringLiteral("ident.zip"), testzip::zip({testzip::file("logs/app.log", kInfoAppLog)}));
    const auto unread = SourceIdentity::captureUnread(file);
    CHECK(unread && unread->member == QStringLiteral("logs/app.log") && unread->isUnreadArchive());
    CHECK(unread && unread->check() == SourceIdentity::State::Valid);
}

// ===========================================================================
// tar (F-2, шаг 5)
// ===========================================================================
QByteArray fixtureGz(const unsigned char* data, size_t size)
{
    return QByteArray(reinterpret_cast<const char*>(data), int(size));
}

// Распаковать gzip в память (фикстуры хранятся сжатыми).
QByteArray gunzipBytes(const QByteArray& gz)
{
    QBuffer src;
    src.setData(gz);
    GzipDevice dev(&src);
    if (!dev.open(QIODevice::ReadOnly))
        return {};
    return dev.readAll();
}

struct TarFixture {
    const char* name;
    QByteArray gz;
};

QList<TarFixture> tarFixtures()
{
    return {
        {"gnu", fixtureGz(tarfixtures::kGnuTarGz, sizeof(tarfixtures::kGnuTarGz))},
        {"pax", fixtureGz(tarfixtures::kPaxTarGz, sizeof(tarfixtures::kPaxTarGz))},
        {"ustar", fixtureGz(tarfixtures::kUstarTarGz, sizeof(tarfixtures::kUstarTarGz))},
    };
}

// Члены архива: имя → тип и данные (данные — только у файлов).
struct ListedMember {
    TarMember::Type type;
    QByteArray data;
};

QHash<QString, ListedMember> listTar(const QString& path, TarArchive::Error* error = nullptr)
{
    QHash<QString, ListedMember> out;
    TarArchive tar(path);
    if (tar.open()) {
        TarMember m;
        while (tar.next(m)) {
            ListedMember lm{m.type, {}};
            if (m.isFile()) {
                QBuffer buffer;
                buffer.open(QIODevice::WriteOnly);
                if (!tar.extract(buffer))
                    break;
                lm.data = buffer.data();
            }
            out.insert(m.name, lm);
        }
        if (tar.error() == TarArchive::Error::None)
            tar.finish();
    }
    if (error)
        *error = tar.error();
    return out;
}

void testTarReader(const QDir& dir)
{
    const QString longPath = QString::fromLatin1(tarfixtures::kLongPath);
    const QString cyr = QStringLiteral("logs/журнал.log");
    for (const TarFixture& f : tarFixtures()) {
        const QByteArray plain = gunzipBytes(f.gz);
        CHECK(!plain.isEmpty() && plain.size() % 512 == 0);
        const QString base = QString::fromLatin1(f.name);
        const QString tarPath = writeBytes(dir, base + QStringLiteral(".tar"), plain);
        const QString tgzPath = writeBytes(dir, base + QStringLiteral(".tar.gz"), f.gz);
        for (const QString& path : {tarPath, tgzPath}) {
            CHECK(TarArchive::fileHasSignature(path));
            CHECK(ArchiveFormats::detect(path) == ArchiveFormats::Format::Tar);
            CHECK(TarArchive::fileIsCompressedTar(path) == (path == tgzPath));
            TarArchive::Error error = TarArchive::Error::Corrupt;
            const QHash<QString, ListedMember> members = listTar(path, &error);
            CHECK(error == TarArchive::Error::None);
            if (error != TarArchive::Error::None)
                std::fprintf(stderr, "  %s: error %d\n", qPrintable(path), int(error));
            CHECK(members.value(QStringLiteral("logs/app.log")).data == kInfoAppLog);
            CHECK(members.value(QStringLiteral("logs/sub/debug.log")).data == kInfoDebugLog);
            CHECK(members.value(longPath).data == QByteArray("deep line\n"));
            CHECK(members.value(cyr).data == QStringLiteral("кириллица\n").toUtf8());
            CHECK(members.value(QStringLiteral("readme.md")).data == QByteArray("readme\n"));
            CHECK(members.contains(QStringLiteral("logs/link.log"))
                  && members.value(QStringLiteral("logs/link.log")).type == TarMember::Type::Symlink);
            CHECK(members.contains(QStringLiteral("logs/sub"))
                  && members.value(QStringLiteral("logs/sub")).type == TarMember::Type::Directory);
            if (!members.contains(longPath))
                std::fprintf(stderr, "  %s: long path missing; members: %s\n", qPrintable(path),
                             qPrintable(QStringList(members.keys()).join(QStringLiteral(", "))));
        }
    }
    // Не tar: zip, gzip с логом, текст, пустой файл.
    CHECK(ArchiveFormats::detect(writeBytes(dir, QStringLiteral("i.zip"), infoZip())) == ArchiveFormats::Format::Zip);
    const QString logGz = writeBytes(dir, QStringLiteral("t-app.log.gz"), gzipMember(makeLog(100000, 3), 6));
    CHECK(!TarArchive::fileHasSignature(logGz) && ArchiveFormats::detect(logGz) == ArchiveFormats::Format::None);
    const QString shortGz = writeBytes(dir, QStringLiteral("t-short.gz"), gzipMember("tiny\n", 6));
    CHECK(ArchiveFormats::detect(shortGz) == ArchiveFormats::Format::None);
    CHECK(ArchiveFormats::detect(writeBytes(dir, QStringLiteral("t.txt"), makeLog(5000, 4)))
          == ArchiveFormats::Format::None);
    const QString empty = writeBytes(dir, QStringLiteral("t-empty.tar"), QByteArray());
    CHECK(ArchiveFormats::detect(empty) == ArchiveFormats::Format::None);
    {
        TarArchive t(empty);
        CHECK(t.open());
        TarMember m;
        CHECK(!t.next(m) && t.error() == TarArchive::Error::NotTar);
    }
    // Текст под именем .tar — не tar.
    {
        TarArchive t(writeBytes(dir, QStringLiteral("t-text.tar"), makeLog(5000, 5)));
        CHECK(t.open());
        TarMember m;
        CHECK(!t.next(m) && t.error() == TarArchive::Error::NotTar);
    }

    // Собранные: GNU 'L', pax path, base-256, v7 (без magic), prefix ustar.
    const QByteArray longName = QByteArray("dir/") + QByteArray(150, 'n') + QByteArray(".log");
    testtar::Entry gnuLong = testtar::file(longName, "gnu long\n");
    gnuLong.gnuLongName = true;
    testtar::Entry paxLong = testtar::file(QByteArray("pax/") + QByteArray(120, 'p') + "/имя.log", "pax long\n");
    paxLong.paxPath = true;
    testtar::Entry big = testtar::file("b256.log", "base 256\n");
    big.base256Size = true;
    testtar::Entry prefixed = testtar::file("leaf.log", "prefixed\n");
    prefixed.prefix = "some/prefix";
    const QString built = writeBytes(dir, QStringLiteral("built.tar"),
                                     testtar::tar({gnuLong, paxLong, big, prefixed}));
    {
        TarArchive::Error error = TarArchive::Error::Corrupt;
        const auto members = listTar(built, &error);
        CHECK(error == TarArchive::Error::None);
        CHECK(members.value(QString::fromLatin1(longName)).data == QByteArray("gnu long\n"));
        CHECK(members.value(QString::fromUtf8(paxLong.name)).data == QByteArray("pax long\n"));
        CHECK(members.value(QStringLiteral("b256.log")).data == QByteArray("base 256\n"));
        CHECK(members.value(QStringLiteral("some/prefix/leaf.log")).data == QByteArray("prefixed\n"));
    }
    // GNU: prefix-поля нет — те же байты в нём не склеиваются с именем.
    {
        testtar::Entry e = testtar::file("gnu.log", "x\n");
        e.prefix = "ignored";
        const QString p = writeBytes(dir, QStringLiteral("gnuprefix.tar"), testtar::tar({e}, testtar::Format::Gnu));
        CHECK(listTar(p).contains(QStringLiteral("gnu.log")));
    }
    // v7: без magic — читается, но по содержимому за tar не считается.
    const QString v7 = writeBytes(dir, QStringLiteral("v7.tar"),
                                  testtar::tar({testtar::file("old.log", "v7\n")}, testtar::Format::V7));
    CHECK(listTar(v7).value(QStringLiteral("old.log")).data == QByteArray("v7\n"));
    CHECK(!TarArchive::fileHasSignature(v7));
    // Имя не в UTF-8 — Latin-1 (байт = символ).
    const QString latin = writeBytes(dir, QStringLiteral("latin.tar"),
                                     testtar::tar({testtar::file("caf\xE9.log", "c\n")}));
    CHECK(listTar(latin).contains(QString::fromLatin1("caf\xE9.log")));
    // Без нулевых блоков в конце — конец на границе принимается.
    {
        TarArchive::Error error = TarArchive::Error::Corrupt;
        const auto members = listTar(writeBytes(dir, QStringLiteral("noend.tar"),
            testtar::tar({testtar::file("a.log", "a\n"), testtar::file("b.log", "b\n")}, testtar::Format::Ustar, 0)), &error);
        CHECK(error == TarArchive::Error::None && members.size() == 2);
    }

    // Повреждения: контрольная сумма (первый член — не tar, второй —
    // повреждён), обрыв в данных, в заголовке, длинное имя больше 1 МБ,
    // размер не числом, gzip с неверным CRC.
    testtar::Entry badSum = testtar::file("bad.log", "x\n");
    badSum.badChecksum = true;
    {
        TarArchive::Error error = TarArchive::Error::None;
        listTar(writeBytes(dir, QStringLiteral("badsum1.tar"), testtar::tar({badSum})), &error);
        CHECK(error == TarArchive::Error::NotTar);
        listTar(writeBytes(dir, QStringLiteral("badsum2.tar"), testtar::tar({testtar::file("ok.log", "ok\n"), badSum})), &error);
        CHECK(error == TarArchive::Error::Corrupt);
    }
    const QByteArray whole = testtar::tar({testtar::file("a.log", makeLog(5000, 6)), testtar::file("b.log", "b\n")});
    for (const qsizetype cut : {qsizetype(512 + 1000), qsizetype(512 + 5120 + 100), qsizetype(512 + 4000)}) {
        TarArchive::Error error = TarArchive::Error::None;
        listTar(writeBytes(dir, QStringLiteral("cut.tar"), whole.left(cut)), &error);
        CHECK(error == TarArchive::Error::Corrupt);
        // То же сжатым: поток gzip цел, обрыв — у tar.
        listTar(writeBytes(dir, QStringLiteral("cut.tar.gz"), testgzip::gzip(whole.left(cut))), &error);
        CHECK(error == TarArchive::Error::Corrupt);
    }
    {
        testtar::Entry hugeName = testtar::file(QByteArray(2 * 1024 * 1024, 'x'), "x\n");
        hugeName.gnuLongName = true;
        TarArchive::Error error = TarArchive::Error::None;
        listTar(writeBytes(dir, QStringLiteral("hugename.tar"), testtar::tar({testtar::file("a.log", "a\n"), hugeName})), &error);
        CHECK(error == TarArchive::Error::Corrupt);
    }
    {
        QByteArray garbled = testtar::tar({testtar::file("a.log", "a\n"), testtar::file("b.log", "b\n")});
        garbled.replace(1024 + 124, 12, "zzzzzzzzzzz\0");
        TarArchive::Error error = TarArchive::Error::None;
        listTar(writeBytes(dir, QStringLiteral("garbled.tar"), garbled), &error);
        CHECK(error == TarArchive::Error::Corrupt || error == TarArchive::Error::NotTar);
    }
    {
        QByteArray gz = testgzip::gzip(whole);
        gz[gz.size() - 6] = char(gz[gz.size() - 6] ^ 0x55); // CRC-32 в хвосте
        const QString p = writeBytes(dir, QStringLiteral("badcrc.tar.gz"), gz);
        CHECK(TarArchive::fileHasSignature(p));
        TarArchive::Error error = TarArchive::Error::None;
        listTar(p, &error);
        CHECK(error == TarArchive::Error::Corrupt);
    }
    // Флаг прерывания — у .tar и .tar.gz.
    for (const QString& name : {QStringLiteral("gnu.tar"), QStringLiteral("gnu.tar.gz")}) {
        std::atomic_bool abort{true};
        TarArchive t(dir.filePath(name));
        CHECK(t.open(&abort));
        TarMember m;
        CHECK(!t.next(m));
        CHECK(t.error() == TarArchive::Error::Aborted);
    }
}

bool tarLogOrGz(const QString& member)
{
    return logOrGz(member);
}

void testUnpackTar(const QDir& dir)
{
    const QString outer = dir.filePath(QStringLiteral("tarout"));
    QDir().mkpath(outer);
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
    const QString longPath = QString::fromLatin1(tarfixtures::kLongPath);

    // Настоящие архивы трёх форматов, сжатые и нет.
    for (const TarFixture& f : tarFixtures()) {
        for (const bool compressed : {false, true}) {
            const QString name = QString::fromLatin1(f.name) + (compressed ? QStringLiteral(".tgz") : QStringLiteral(".tar"));
            const QString path = writeBytes(dir, QStringLiteral("real-") + name, compressed ? f.gz : gunzipBytes(f.gz));
            const QString target = outer + QStringLiteral("/") + name;
            const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &tarLogOrGz); });
            CHECK(zr.result.ok());
            if (!zr.result.ok())
                std::fprintf(stderr, "  %s: %s\n", qPrintable(name), qPrintable(zr.result.message));
            CHECK_EQ(zr.result.skippedUnsafe, 1); // ссылка
            CHECK_EQ(zr.result.skippedEncrypted, 0);
            QStringList expected = {QStringLiteral("logs/app.log"), QStringLiteral("logs/sub/debug.log"), longPath,
                                    QStringLiteral("logs/журнал.log")};
            expected.sort();
            CHECK(filesUnder(target) == expected);
            if (filesUnder(target) != expected)
                std::fprintf(stderr, "  %s files: %s\n", qPrintable(name), qPrintable(filesUnder(target).join(QStringLiteral(", "))));
            CHECK(readFile(target + QStringLiteral("/logs/app.log")) == kInfoAppLog);
            CHECK(readFile(target + QLatin1Char('/') + longPath) == QByteArray("deep line\n"));
            CHECK_EQ(zr.result.extracted.size(), qsizetype(4));
            CHECK_EQ(zr.lastProgress, zr.progressTotal);
            CHECK_EQ(zr.progressTotal, QFileInfo(path).size());
        }
    }

    // Собранный набор: опасные имена, ссылки, устройства, разреженный,
    // повтор имени и регистра, .gz внутри, не-лог.
    const QByteArray nested = makeLog(300000, 22);
    const QList<testtar::Entry> entries = {
        testtar::file("logs/app.log", kInfoAppLog),
        testtar::file("logs/deep/er/x.log", nested),
        testtar::file("../evil.log", "evil\n"),
        testtar::file("/abs.log", "abs\n"),
        testtar::file("C:\\win.log", "win\n"),
        testtar::file("a/../../up.log", "up\n"),
        testtar::file("con.log", "reserved\n"),
        testtar::file("Case.log", "upper\n"),
        testtar::file("case.log", "lower\n"),
        testtar::file("logs/app.log", "second copy\n"),
        testtar::file("inner/app.log.gz", gzipMember(kInfoAppLog, 6)),
        testtar::file("readme.md", "not a log\n"),
        testtar::special("hard.log", '1'),
        testtar::special("sym.log", '2'),
        testtar::special("chr.log", '3'),
        testtar::special("blk.log", '4'),
        testtar::special("fifo.log", '6'),
        testtar::special("sparse.log", 'S', "sparse data\n"),
        testtar::special("logs", '5'),
    };
    const QByteArray bundle = testtar::tar(entries);
    for (const bool compressed : {false, true}) {
        const QString path = writeBytes(dir, compressed ? QStringLiteral("bundle.tar.gz") : QStringLiteral("bundle.tar"),
                                        compressed ? testgzip::gzip(bundle) : bundle);
        const QString target = outer + (compressed ? QStringLiteral("/bundle-gz") : QStringLiteral("/bundle"));
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &tarLogOrGz); });
        CHECK(zr.result.ok());
        // 4 опасных имени + повтор имени + 5 ссылок и устройств.
        CHECK_EQ(zr.result.skippedUnsafe, 10);
        CHECK_EQ(zr.result.skippedEncrypted, 1); // разреженный
        CHECK(filesUnder(outer + QStringLiteral("/..")).filter(QStringLiteral("evil")).isEmpty());
        QStringList expected = {QStringLiteral("Case.log"), QStringLiteral("_con.log"), QStringLiteral("case~2.log"),
                                QStringLiteral("inner/app.log.gz"), QStringLiteral("logs/app.log"),
                                QStringLiteral("logs/deep/er/x.log")};
        expected.sort();
        CHECK(filesUnder(target) == expected);
        if (filesUnder(target) != expected)
            std::fprintf(stderr, "  bundle files: %s\n", qPrintable(filesUnder(target).join(QStringLiteral(", "))));
        CHECK(readFile(target + QStringLiteral("/logs/app.log")) == kInfoAppLog); // первый из двух
        CHECK(readFile(target + QStringLiteral("/logs/deep/er/x.log")) == nested);
        CHECK(readFile(target + QStringLiteral("/inner/app.log.gz")) == gzipMember(kInfoAppLog, 6));
    }
    CHECK(!QFileInfo::exists(dir.filePath(QStringLiteral("evil.log"))));
    CHECK(!QFileInfo::exists(outer + QStringLiteral("/evil.log")));

    // Ошибки — каталог удалён целиком: повреждённый второй член, обрыв,
    // неверный CRC у .tar.gz (данные целы, хвост gzip — нет), не архив.
    testtar::Entry badSum = testtar::file("z-bad.log", "data\n");
    badSum.badChecksum = true;
    QByteArray badCrc = testgzip::gzip(testtar::tar({testtar::file("a.log", "ok\n")}));
    badCrc[badCrc.size() - 6] = char(badCrc[badCrc.size() - 6] ^ 0x55);
    const QByteArray full = testtar::tar({testtar::file("a.log", makeLog(20000, 7)), testtar::file("b.log", "b\n")});
    const QList<std::pair<QString, QByteArray>> broken = {
        {QStringLiteral("badsum.tar"), testtar::tar({testtar::file("a.log", "ok\n"), badSum})},
        {QStringLiteral("cut.tar"), full.left(512 + 9000)},
        {QStringLiteral("cut.tgz"), testgzip::gzip(full.left(512 + 9000))},
        {QStringLiteral("badcrc.tgz"), badCrc},
        {QStringLiteral("text.tar"), QByteArray("plain text\n")},
    };
    for (const auto& [name, bytes] : broken) {
        const QString path = writeBytes(dir, name, bytes);
        const QString target = outer + QStringLiteral("/broken-") + name;
        QDir().mkpath(target); // созданный вызывающим — тоже убран
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &tarLogOrGz); });
        CHECK(zr.result.error == SourceUnpacker::Error::SourceError);
        CHECK(!zr.result.message.isEmpty());
        CHECK(!QFileInfo::exists(target));
        CHECK(zr.result.extracted.isEmpty());
    }
    // Пустой архив (только нулевые блоки, сжатый и нет) — успех без файлов.
    for (const QString& name : {QStringLiteral("empty.tar"), QStringLiteral("empty.tgz")}) {
        const QString emptyTar = writeBytes(dir, name, name.endsWith(QLatin1String(".tgz"))
                                                           ? testgzip::gzip(testtar::tar({}, testtar::Format::Ustar, 20))
                                                           : testtar::tar({}, testtar::Format::Ustar, 20));
        CHECK(ArchiveFormats::detect(emptyTar) == ArchiveFormats::Format::None);
        CHECK(ArchiveFormats::detectKnown(emptyTar) == ArchiveFormats::Format::Tar);
        const ZipRun er = runJob(unpacker, [&] { return unpacker.unpackArchive(emptyTar, outer + QStringLiteral("/e"), &tarLogOrGz); });
        CHECK(er.result.ok() && er.result.extracted.isEmpty());
    }
    // v7 (без magic), открытый по имени: по содержимому не архив, но читается.
    const QString v7 = writeBytes(dir, QStringLiteral("old-v7.tar"),
        testtar::tar({testtar::file("old.log", "v7 log\n"), testtar::file("x.md", "x\n")}, testtar::Format::V7));
    CHECK(ArchiveFormats::detect(v7) == ArchiveFormats::Format::None);
    CHECK(ArchiveFormats::detectKnown(v7) == ArchiveFormats::Format::Tar);
    {
        const ZipRun vr = runJob(unpacker, [&] { return unpacker.unpackArchive(v7, outer + QStringLiteral("/v7"), &tarLogOrGz); });
        CHECK(vr.result.ok() && vr.result.extracted.size() == 1);
        CHECK(readFile(outer + QStringLiteral("/v7/old.log")) == QByteArray("v7 log\n"));
        const QString target = dir.filePath(QStringLiteral("v7-member.log"));
        const ZipRun mr = runJob(unpacker, [&] { return unpacker.unpackArchiveMember(v7, QStringLiteral("old.log"), target); });
        CHECK(mr.result.ok() && readFile(target) == QByteArray("v7 log\n"));
    }
    // Каталог-член с тем же именем, что у файла (дописанный архив): набор
    // извлекает файл — член вкладки обязан найти его же, а не каталог (ревью шагов 4–5).
    {
        const QString dirFirst = writeBytes(dir, QStringLiteral("dir-first.tar"),
            testtar::tar({testtar::special("x/app.log", '5'), testtar::file("x/app.log", "file after dir\n")}));
        const ZipRun sr = runJob(unpacker, [&] { return unpacker.unpackArchive(dirFirst, outer + QStringLiteral("/df"), &tarLogOrGz); });
        CHECK(sr.result.ok() && sr.result.extracted.size() == 1);
        const QString target = dir.filePath(QStringLiteral("dir-first-member.log"));
        const ZipRun mr = runJob(unpacker, [&] { return unpacker.unpackArchiveMember(dirFirst, QStringLiteral("x/app.log"), target); });
        CHECK(mr.result.ok());
        if (!mr.result.ok())
            std::fprintf(stderr, "  dir-first member: %s\n", qPrintable(mr.result.message));
        CHECK(readFile(target) == QByteArray("file after dir\n"));
    }
    // Архива нет — причина чтения, а не «не архив».
    {
        const QString gone = dir.filePath(QStringLiteral("gone.tar"));
        const ZipRun gr = runJob(unpacker, [&] { return unpacker.unpackArchive(gone, outer + QStringLiteral("/gone"), &tarLogOrGz); });
        CHECK(gr.result.error == SourceUnpacker::Error::SourceError);
        CHECK(gr.result.message.startsWith(QStringLiteral("Cannot read the archive")));
        CHECK(!QFileInfo::exists(outer + QStringLiteral("/gone")));
    }
    // unpackArchive выбирает zip так же, как unpackZip.
    const QString zipPath = writeBytes(dir, QStringLiteral("via-archive.zip"), infoZip());
    const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(zipPath, outer + QStringLiteral("/z"), &logOrGz); });
    CHECK(zr.result.ok() && zr.result.extracted.size() == 2);
}

void testTarSpaceAndCancel(const QDir& dir)
{
    const QByteArray a = makeLog(3 * 1024 * 1024, 33);
    const QByteArray b = makeLog(5 * 1024 * 1024, 34);
    const QByteArray bytes = testtar::tar({testtar::file("a.log", a), testtar::file("b.log", b),
                                           testtar::file("skip.md", makeLog(1 << 20, 2))});
    const QString tarPath = writeBytes(dir, QStringLiteral("space.tar"), bytes);
    const QString tgzPath = writeBytes(dir, QStringLiteral("space.tgz"), testgzip::gzip(bytes));
    const QString outer = dir.filePath(QStringLiteral("tarspace"));
    QDir().mkpath(outer);

    // ДО начала: .tar — сумма отобранных (по проходу заголовков), .tar.gz —
    // размер архива на диске; плюс резерв.
    for (const QString& path : {tarPath, tgzPath}) {
        SourceUnpacker unpacker;
        unpacker.setReserveBytes(1024 * 1024);
        auto calls = std::make_shared<std::atomic<int>>(0);
        unpacker.setFreeSpaceFunction([calls](const QString&) {
            calls->fetch_add(1);
            return qint64(1024 * 1024);
        });
        const QString target = outer + QStringLiteral("/before");
        const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &logOrGz); });
        CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
        const qint64 estimate = path == tarPath ? qint64(a.size() + b.size()) : QFileInfo(tgzPath).size();
        CHECK_EQ(zr.result.requiredBytes, estimate + 1024 * 1024);
        CHECK_EQ(calls->load(), 1);
        CHECK(!QFileInfo::exists(target));
    }
    // ПО ХОДУ и отмена — у обоих.
    for (const QString& path : {tarPath, tgzPath}) {
        {
            SourceUnpacker unpacker;
            unpacker.setReserveBytes(1024 * 1024);
            unpacker.setCheckIntervalBytes(1024 * 1024);
            auto calls = std::make_shared<std::atomic<int>>(0);
            unpacker.setFreeSpaceFunction([calls](const QString&) {
                return calls->fetch_add(1) == 0 ? qint64(1) << 40 : qint64(512 * 1024);
            });
            const QString target = outer + QStringLiteral("/during");
            const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &logOrGz); });
            CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
            CHECK(zr.result.bytesWritten < a.size() + b.size());
            CHECK(!QFileInfo::exists(target));
        }
        {
            SourceUnpacker unpacker;
            unpacker.setCheckIntervalBytes(1024 * 1024);
            auto calls = std::make_shared<std::atomic<int>>(0);
            SourceUnpacker* self = &unpacker;
            unpacker.setFreeSpaceFunction([calls, self](const QString&) {
                if (calls->fetch_add(1) == 2)
                    self->cancel();
                return qint64(1) << 40;
            });
            const QString target = outer + QStringLiteral("/cancel");
            const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchive(path, target, &logOrGz); });
            CHECK(zr.result.error == SourceUnpacker::Error::Cancelled);
            CHECK(!QFileInfo::exists(target));
        }
    }
}

void testUnpackTarMember(const QDir& dir)
{
    const QByteArray big = makeLog(2 * 1024 * 1024, 42);
    const QByteArray bytes = testtar::tar({
        testtar::file("logs/app.log", kInfoAppLog),
        testtar::file("logs/big.log", big),
        testtar::file("logs/app.log.gz", gzipMember(big, 1) + gzipMember(kInfoAppLog, 6)),
        testtar::file(QStringLiteral("журнал.log").toUtf8(), "кириллица\n"),
        testtar::file("logs/app.log", "second copy\n"),
        testtar::special("link.log", '2'),
        testtar::special("dev.log", '3'),
        testtar::special("logs", '5'),
    });
    SourceUnpacker unpacker;
    unpacker.setFreeSpaceFunction([](const QString&) { return qint64(1) << 50; });
    for (const bool compressed : {false, true}) {
        const QString archive = writeBytes(dir, compressed ? QStringLiteral("members.tgz") : QStringLiteral("members.tar"),
                                           compressed ? testgzip::gzip(bytes) : bytes);
        const auto extract = [&](const QString& member, const QString& name) {
            const QString target = dir.filePath(name);
            return std::make_pair(runJob(unpacker, [&] { return unpacker.unpackArchiveMember(archive, member, target); }),
                                  target);
        };
        {
            const auto [zr, target] = extract(QStringLiteral("logs/app.log"), QStringLiteral("t-app.log"));
            CHECK(zr.result.ok());
            CHECK(readFile(target) == kInfoAppLog); // первый из двух
        }
        {
            const auto [zr, target] = extract(QStringLiteral("logs/big.log"), QStringLiteral("t-big.log"));
            CHECK(zr.result.ok());
            CHECK(readFile(target) == big);
            CHECK_EQ(zr.lastProgress, zr.progressTotal);
            CHECK_EQ(zr.progressTotal, QFileInfo(archive).size()); // по байтам архива на всех стадиях
        }
        {
            const auto [zr, target] = extract(QStringLiteral("logs/app.log.gz"), QStringLiteral("t-gz.log"));
            CHECK(zr.result.ok());
            CHECK(readFile(target) == big + kInfoAppLog);
            CHECK(!QFileInfo::exists(target + QStringLiteral(".gz-part")));
        }
        {
            const auto [zr, target] = extract(QStringLiteral("журнал.log"), QStringLiteral("t-cyr.log"));
            CHECK(zr.result.ok());
            CHECK(readFile(target) == QStringLiteral("кириллица\n").toUtf8());
        }
        for (const char* member : {"missing.log", "link.log", "dev.log", "logs"}) {
            const auto [zr, target] = extract(QString::fromUtf8(member), QStringLiteral("t-err.log"));
            CHECK(zr.result.error == SourceUnpacker::Error::SourceError);
            CHECK(!zr.result.message.isEmpty());
            CHECK(!QFileInfo::exists(target));
        }
        {
            SourceUnpacker tight;
            tight.setReserveBytes(0);
            tight.setFreeSpaceFunction([](const QString&) { return qint64(1000); });
            const QString target = dir.filePath(QStringLiteral("t-space.log"));
            const ZipRun zr = runJob(tight, [&] { return tight.unpackArchiveMember(archive, QStringLiteral("logs/big.log"), target); });
            CHECK(zr.result.error == SourceUnpacker::Error::NoSpace);
            CHECK_EQ(zr.result.requiredBytes, qint64(big.size()));
            CHECK(!QFileInfo::exists(target));
        }
    }
    // .tar.gz с неверным CRC gzip: член цел, но поток — нет; файла нет.
    QByteArray gz = testgzip::gzip(bytes);
    gz[gz.size() - 6] = char(gz[gz.size() - 6] ^ 0x55);
    const QString badArchive = writeBytes(dir, QStringLiteral("members-badcrc.tgz"), gz);
    const QString target = dir.filePath(QStringLiteral("t-badcrc.log"));
    const ZipRun zr = runJob(unpacker, [&] { return unpacker.unpackArchiveMember(badArchive, QStringLiteral("logs/app.log"), target); });
    CHECK(zr.result.error == SourceUnpacker::Error::SourceError);
    CHECK(!QFileInfo::exists(target));
    // Не архив.
    const QString notArchive = writeBytes(dir, QStringLiteral("members-text.tar"), "text\n");
    const ZipRun nr = runJob(unpacker, [&] { return unpacker.unpackArchiveMember(notArchive, QStringLiteral("a.log"), target); });
    CHECK(nr.result.error == SourceUnpacker::Error::SourceError && !QFileInfo::exists(target));
}

void testTarIdentity(const QDir& dir)
{
    // Член tar.gz: путь — архив, checkAt — по содержимому архива (tar), а не
    // по сигнатуре gzip или zip.
    const QByteArray bytes = testtar::tar({testtar::file("logs/app.log", kInfoAppLog)});
    const QString tgz = writeBytes(dir, QStringLiteral("ident.tgz"), testgzip::gzip(bytes));
    const QString copy = writeBytes(dir, QStringLiteral("ident-tar-copy.log"), kInfoAppLog);
    LogFile file(copy);
    LogSource source;
    source.kind = LogSource::Kind::Archive;
    source.path = tgz;
    source.member = QStringLiteral("logs/app.log");
    file.setSource(source);
    CHECK(file.displayPath().endsWith(QStringLiteral("ident.tgz \u203A logs/app.log")));
    const auto id = SourceIdentity::capture(file);
    CHECK(id && id->compressed && id->member == QStringLiteral("logs/app.log"));
    CHECK(id && id->check() == SourceIdentity::State::Valid);
    // Другой tar.gz — Changed; gzip без tar внутри и текст — Missing.
    writeBytes(dir, QStringLiteral("ident.tgz"), testgzip::gzip(testtar::tar({testtar::file("logs/app.log", "other\n")})));
    CHECK(id && id->check() == SourceIdentity::State::Changed);
    writeBytes(dir, QStringLiteral("ident.tgz"), testgzip::gzip(kInfoAppLog));
    CHECK(id && id->check() == SourceIdentity::State::Missing);
    writeBytes(dir, QStringLiteral("ident.tgz"), kInfoAppLog);
    CHECK(id && id->check() == SourceIdentity::State::Missing);
    // v7 (без magic): член узнаётся (архив известен), не Missing.
    const QString v7 = writeBytes(dir, QStringLiteral("ident-v7.tar"),
                                  testtar::tar({testtar::file("logs/app.log", kInfoAppLog)}, testtar::Format::V7));
    source.path = v7;
    file.setSource(source);
    const auto old = SourceIdentity::capture(file);
    CHECK(old && old->check() == SourceIdentity::State::Valid);
    // Несжатый .tar — тоже архив.
    const QString tarPath = writeBytes(dir, QStringLiteral("ident.tar"), bytes);
    source.path = tarPath;
    file.setSource(source);
    const auto plain = SourceIdentity::capture(file);
    CHECK(plain && plain->check() == SourceIdentity::State::Valid);
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
    stage("zip: reader", [&] { testZipReader(files); });
    stage("zip: names", testArchiveNames);
    stage("zip: unpack to folder", [&] { testUnpackZip(files); });
    stage("zip: space, cancel", [&] { testZipSpaceAndCancel(files); });
    stage("zip: one member", [&] { testUnpackZipMember(files); });
    stage("zip: identity", [&] { testZipIdentity(files); });
    stage("tar: reader", [&] { testTarReader(files); });
    stage("tar: unpack to folder", [&] { testUnpackTar(files); });
    stage("tar: space, cancel", [&] { testTarSpaceAndCancel(files); });
    stage("tar: one member", [&] { testUnpackTarMember(files); });
    stage("tar: identity", [&] { testTarIdentity(files); });
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
