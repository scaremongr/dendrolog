#ifndef TESTS_COMMON_GZIPWRITER_H
#define TESTS_COMMON_GZIPWRITER_H

// ============================================================================
// Сжатие в gzip для тестов (F-2): raw deflate miniz + свой заголовок и хвост
// (RFC 1952). Тест, который подключает этот заголовок, линкуется с `miniz`
// (`dendrolog_add_test(... LIBRARIES ... miniz)`). Фикстуры — массивами в
// памяти: двоичные файлы в репозитории под core.autocrlf рискованны.
// ============================================================================

#include <QByteArray>
#include <QList>

#include <cstring>

#include <miniz.h>

namespace testgz {

inline QByteArray rawDeflate(const QByteArray& data, int level = 6)
{
    mz_stream s;
    std::memset(&s, 0, sizeof(s));
    if (mz_deflateInit2(&s, level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY)
        != MZ_OK)
        return {};
    QByteArray out;
    QByteArray chunk(256 * 1024, Qt::Uninitialized);
    s.next_in = reinterpret_cast<const unsigned char*>(data.constData());
    s.avail_in = unsigned(data.size());
    int status = MZ_OK;
    do {
        s.next_out = reinterpret_cast<unsigned char*>(chunk.data());
        s.avail_out = unsigned(chunk.size());
        status = mz_deflate(&s, MZ_FINISH);
        out.append(chunk.constData(), chunk.size() - int(s.avail_out));
    } while (status == MZ_OK || (status == MZ_BUF_ERROR && s.avail_out == 0));
    mz_deflateEnd(&s);
    return status == MZ_STREAM_END ? out : QByteArray();
}

inline void appendLe32(QByteArray& out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

// Один поток gzip (без имени и флагов).
inline QByteArray member(const QByteArray& data, int level = 6)
{
    QByteArray out("\x1F\x8B\x08\x00", 4);
    appendLe32(out, 0);                 // MTIME
    out.append('\x00');                 // XFL
    out.append('\x03');                 // OS = Unix
    out += rawDeflate(data, level);
    appendLe32(out, quint32(mz_crc32(MZ_CRC32_INIT,
                                     reinterpret_cast<const unsigned char*>(data.constData()),
                                     size_t(data.size()))));
    appendLe32(out, quint32(data.size()));
    return out;
}

// Склейка потоков (`cat a.gz b.gz`): распакованное — части подряд.
inline QByteArray members(const QList<QByteArray>& parts, int level = 6)
{
    QByteArray out;
    for (const QByteArray& part : parts)
        out += member(part, level);
    return out;
}

} // namespace testgz

#endif // TESTS_COMMON_GZIPWRITER_H
