#ifndef TESTS_COMMON_GZIPBYTES_H
#define TESTS_COMMON_GZIPBYTES_H

// gzip-файл из байтов для тестов вкладки и окна (F-2): deflate — qCompress
// (zlib-поток без заголовка и Adler-32 — это raw deflate), заголовок и хвост
// gzip (CRC-32, ISIZE) — свои. Без miniz: тесты виджетов его не линкуют.

#include <QByteArray>
#include <QtGlobal>

namespace testgzip {

inline quint32 crc32(const QByteArray& data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : data) {
        crc ^= quint8(c);
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void appendLe32(QByteArray& out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

// Один поток gzip с содержимым data.
inline QByteArray gzip(const QByteArray& data, int level = 6)
{
    const QByteArray zlib = qCompress(data, level); // 4 байта длины + zlib
    QByteArray out("\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\x03", 10);
    out += zlib.mid(4 + 2, zlib.size() - 4 - 2 - 4);
    appendLe32(out, crc32(data));
    appendLe32(out, quint32(data.size() & 0xFFFFFFFFu));
    return out;
}

} // namespace testgzip

#endif // TESTS_COMMON_GZIPBYTES_H
