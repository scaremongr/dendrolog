#ifndef TESTS_COMMON_ZIPBYTES_H
#define TESTS_COMMON_ZIPBYTES_H

// zip-архив из байтов для тестов (F-2, шаг 4): члены stored или deflate
// (qCompress без заголовка zlib и Adler-32 — raw deflate), центральный
// каталог и конец архива — свои. Имена — сырые байты: так собираются и
// опасные имена (`../x`, `/etc/x`, `C:\x`), которые настоящие архиваторы
// пишут неохотно, и имена в OEM-кодировке. Без miniz: тесты виджетов его не
// линкуют.

#include "gzipbytes.h"

#include <QByteArray>
#include <QList>
#include <QtGlobal>

namespace testzip {

struct Entry {
    QByteArray name;            // как записано в архиве
    QByteArray data;            // распакованное содержимое
    bool deflate = true;
    bool utf8Flag = true;       // бит 11 (имя в UTF-8)
    bool encrypted = false;     // бит 0 (данные — как есть: распаковщик их не читает)
    bool symlink = false;       // Unix-атрибуты S_IFLNK (data — цель ссылки)
    bool directory = false;     // имя с '/' на конце, без данных
    bool badCrc = false;        // записать неверный CRC
    qint64 declaredSize = -1;   // заявленный размер (по умолчанию — настоящий)
};

inline void appendLe16(QByteArray& out, quint16 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
}

inline QByteArray rawDeflate(const QByteArray& data)
{
    const QByteArray zlib = qCompress(data, 6);
    return zlib.mid(4 + 2, zlib.size() - 4 - 2 - 4);
}

inline QByteArray zip(const QList<Entry>& entries)
{
    QByteArray out;
    QByteArray central;
    for (const Entry& e : entries) {
        const QByteArray payload = e.directory ? QByteArray() : (e.deflate ? rawDeflate(e.data) : e.data);
        const quint16 method = (e.directory || !e.deflate) ? 0 : 8;
        quint16 flags = 0;
        if (e.utf8Flag)
            flags |= 0x0800;
        if (e.encrypted)
            flags |= 0x0001;
        quint32 crc = e.directory ? 0 : testgzip::crc32(e.data);
        if (e.badCrc)
            crc ^= 0x5A5A5A5Au;
        const quint32 size = e.declaredSize >= 0 ? quint32(e.declaredSize) : quint32(e.data.size());
        const quint32 offset = quint32(out.size());

        // Локальный заголовок.
        testgzip::appendLe32(out, 0x04034b50u);
        appendLe16(out, 20);         // version needed
        appendLe16(out, flags);
        appendLe16(out, method);
        appendLe16(out, 0);          // time
        appendLe16(out, 0x21);       // date (1980-01-01)
        testgzip::appendLe32(out, crc);
        testgzip::appendLe32(out, quint32(payload.size()));
        testgzip::appendLe32(out, size);
        appendLe16(out, quint16(e.name.size()));
        appendLe16(out, 0);          // extra
        out += e.name;
        out += payload;

        // Запись центрального каталога.
        testgzip::appendLe32(central, 0x02014b50u);
        appendLe16(central, quint16((e.symlink ? 3 : 0) << 8 | 20)); // made by: Unix для ссылки
        appendLe16(central, 20);
        appendLe16(central, flags);
        appendLe16(central, method);
        appendLe16(central, 0);
        appendLe16(central, 0x21);
        testgzip::appendLe32(central, crc);
        testgzip::appendLe32(central, quint32(payload.size()));
        testgzip::appendLe32(central, size);
        appendLe16(central, quint16(e.name.size()));
        appendLe16(central, 0);      // extra
        appendLe16(central, 0);      // comment
        appendLe16(central, 0);      // disk
        appendLe16(central, 0);      // internal attrs
        quint32 external = 0;
        if (e.symlink)
            external = quint32(0120777) << 16;
        else if (e.directory)
            external = 0x10;
        testgzip::appendLe32(central, external);
        testgzip::appendLe32(central, offset);
        central += e.name;
    }
    const quint32 centralOffset = quint32(out.size());
    out += central;
    testgzip::appendLe32(out, 0x06054b50u);
    appendLe16(out, 0);
    appendLe16(out, 0);
    appendLe16(out, quint16(entries.size()));
    appendLe16(out, quint16(entries.size()));
    testgzip::appendLe32(out, quint32(central.size()));
    testgzip::appendLe32(out, centralOffset);
    appendLe16(out, 0);
    return out;
}

inline Entry file(const QByteArray& name, const QByteArray& data, bool deflate = true)
{
    Entry e;
    e.name = name;
    e.data = data;
    e.deflate = deflate;
    return e;
}

} // namespace testzip

#endif // TESTS_COMMON_ZIPBYTES_H
