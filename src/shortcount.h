#pragma once

#include <QLocale>
#include <QString>

// Сокращённое число для плашек и чипов (U-23): «↓ N new lines», «+N»
// переключателя переноса, число строк на чипе легенды. До exactBelow —
// точно (с разделителем разрядов, если groupDigits: «12,345»), дальше —
// «120K», «3.4M», «25M», «1.2G»: одна цифра после точки только у первой
// цифры разряда, без округления вверх (9 999 999 — «9.9M»). Длина
// сокращения — не больше четырёх символов, поэтому «+N» всегда
// помещается в резерв «+9999».
namespace ShortCount {

inline QString format(qint64 count, qint64 exactBelow, bool groupDigits)
{
    // Числа — как в остальном (английском) интерфейсе, независимо от локали
    // системы. У QLocale::c() разделитель разрядов выключен — включаем.
    QLocale c = QLocale::c();
    if (groupDigits)
        c.setNumberOptions(QLocale::DefaultNumberOptions);
    if (count < 0)
        count = 0;
    if (count < exactBelow)
        return c.toString(count);
    struct Unit { qint64 size; char suffix; };
    static constexpr Unit units[] = { {1000000000LL, 'G'}, {1000000LL, 'M'}, {1000LL, 'K'} };
    for (const Unit& u : units) {
        if (count < u.size)
            continue;
        if (count < 10 * u.size) {
            const qint64 tenths = count / (u.size / 10);
            return QString::number(tenths / 10) + QLatin1Char('.')
                 + QString::number(tenths % 10) + QLatin1Char(u.suffix);
        }
        if (count < 1000 * u.size || u.suffix == 'G')
            return QString::number(count / u.size) + QLatin1Char(u.suffix);
    }
    return c.toString(count);
}

} // namespace ShortCount
