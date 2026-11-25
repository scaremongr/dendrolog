#ifndef TESTS_COMMON_TARBYTES_H
#define TESTS_COMMON_TARBYTES_H

// tar-архив из байтов для тестов (F-2, шаг 5): заголовки ustar / GNU / v7,
// длинные имена GNU ('L') и pax ('x'), размер base-256, любые типы членов.
// Имена — сырые байты: так собираются и опасные имена (`../x`, `/etc/x`),
// которые настоящий tar пишет неохотно. Сжатие — testgzip::gzip. Без
// miniz: тесты виджетов его не линкуют. Настоящие архивы GNU tar —
// tests/unpack/tarfixtures.h.

#include "gzipbytes.h"

#include <QByteArray>
#include <QList>
#include <QtGlobal>

#include <cstring>

namespace testtar {

enum class Format { Ustar, Gnu, V7 };

struct Entry {
    QByteArray name;            // как записано (в поле name, или в 'L' / pax)
    QByteArray data;            // содержимое
    char type = '0';            // '0' файл, '1' ссылка, '2' симв. ссылка, '3' устройство, '5' каталог, 'S'…
    QByteArray prefix;          // ustar: поле prefix
    bool gnuLongName = false;   // имя — членом 'L' (поле name — обрезанное)
    bool paxPath = false;       // имя — записью pax path
    bool base256Size = false;   // размер — в base-256 (GNU)
    bool badChecksum = false;
    qint64 declaredSize = -1;   // заявленный размер (по умолчанию — настоящий; данные — как есть)
};

inline QByteArray octal(qint64 v, int width)
{
    QByteArray s = QByteArray::number(v, 8);
    while (s.size() < width - 1)
        s.prepend('0');
    s.append('\0');
    return s.left(width);
}

inline QByteArray header(const QByteArray& name, qint64 size, char type, Format format,
                         const QByteArray& prefix = {}, bool base256 = false, bool badChecksum = false)
{
    QByteArray h(512, '\0');
    char* p = h.data();
    std::memcpy(p, name.constData(), size_t(qMin<qsizetype>(name.size(), 100)));
    std::memcpy(p + 100, octal(0644, 8).constData(), 8);
    std::memcpy(p + 108, octal(0, 8).constData(), 8);
    std::memcpy(p + 116, octal(0, 8).constData(), 8);
    if (base256) {
        p[124] = char(0x80);
        for (int i = 0; i < 8; ++i)
            p[135 - i] = char((size >> (8 * i)) & 0xFF);
    } else {
        std::memcpy(p + 124, octal(size, 12).constData(), 12);
    }
    std::memcpy(p + 136, octal(1767225600, 12).constData(), 12);
    p[156] = type;
    if (format == Format::Ustar) {
        std::memcpy(p + 257, "ustar\0" "00", 8);
        std::memcpy(p + 345, prefix.constData(), size_t(qMin<qsizetype>(prefix.size(), 155)));
    } else if (format == Format::Gnu) {
        std::memcpy(p + 257, "ustar  \0", 8);
    }
    std::memset(p + 148, ' ', 8);
    quint32 sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += quint8(p[i]);
    if (badChecksum)
        sum += 1;
    const QByteArray cs = octal(sum, 7);
    std::memcpy(p + 148, cs.constData(), 7);
    p[155] = ' ';
    return h;
}

inline void appendPadded(QByteArray& out, const QByteArray& data)
{
    out += data;
    out += QByteArray(int((512 - data.size() % 512) % 512), '\0');
}

inline QByteArray paxRecord(const QByteArray& key, const QByteArray& value)
{
    const QByteArray body = ' ' + key + '=' + value + '\n';
    qsizetype len = body.size() + 1;
    while (QByteArray::number(len).size() + body.size() != len)
        len = QByteArray::number(len).size() + body.size();
    return QByteArray::number(len) + body;
}

// Архив целиком; endBlocks — сколько нулевых блоков в конце (по умолчанию 2).
inline QByteArray tar(const QList<Entry>& entries, Format format = Format::Ustar, int endBlocks = 2)
{
    QByteArray out;
    for (const Entry& e : entries) {
        QByteArray name = e.name;
        if (e.gnuLongName) {
            appendPadded(out += header("././@LongLink", name.size() + 1, 'L', Format::Gnu), name + '\0');
            name = name.left(99);
        }
        if (e.paxPath) {
            const QByteArray rec = paxRecord("path", name);
            appendPadded(out += header("PaxHeader/x", rec.size(), 'x', Format::Ustar), rec);
            name = "pax-placeholder";
        }
        const qint64 size = e.declaredSize >= 0 ? e.declaredSize : e.data.size();
        out += header(name, size, e.type, format, e.prefix, e.base256Size, e.badChecksum);
        appendPadded(out, e.data);
    }
    out += QByteArray(512 * endBlocks, '\0');
    return out;
}

inline Entry file(const QByteArray& name, const QByteArray& data)
{
    Entry e;
    e.name = name;
    e.data = data;
    return e;
}

inline Entry special(const QByteArray& name, char type, const QByteArray& data = {})
{
    Entry e;
    e.name = name;
    e.type = type;
    e.data = data;
    return e;
}

} // namespace testtar

#endif // TESTS_COMMON_TARBYTES_H
