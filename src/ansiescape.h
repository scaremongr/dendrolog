#ifndef ANSIESCAPE_H
#define ANSIESCAPE_H

#include <QByteArray>
#include <QByteArrayView>
#include <QMetaType>
#include <QString>
#include <QStringView>
#include <QVector>

#include <cstring>

class TextDecoder;

// ============================================================================
// ANSI-последовательности в строках лога (F-3, docs/design/F-3.md). Ядро, без
// состояния.
//
//   • Снятие: строка без байта ESC (0x1B) не меняется и не копируется
//     (hasEscape — memchr). Грамматика ECMA-48 — CSI `ESC [ … финальный`,
//     строковые `ESC ] / P / X / ^ / _ … BEL | ESC \`, nF `ESC 0x20–0x2F …
//     0x30–0x7E`, двухсимвольные `ESC 0x30–0x7E`; оборванная (конец строки
//     или посторонний символ внутри) снимается до места обрыва, посторонний
//     символ остаётся текстом — последовательность не переходит через '\n';
//     одиночный ESC снимается один. Одна грамматика для байтов (TextDecoder
//     снимает до декодирования — ESC внутри многобайтного символа UTF-8 не
//     рвёт символ) и для UTF-16 (QTextStream, QStringDecoder).
//   • Цвет: styles — спаны SGR в координатах ТЕКСТА без кодов (то, что
//     покажет view); каждая строка начинается со стиля по умолчанию.
// ============================================================================

// Цвет ANSI: 0 — по умолчанию; kIndexed | n — палитра 0–255; kRgb | 0xRRGGBB.
namespace AnsiColor {
constexpr quint32 kDefault = 0;
constexpr quint32 kIndexed = 0x01000000u;
constexpr quint32 kRgb     = 0x02000000u;
constexpr quint32 kKindMask = 0xFF000000u;
inline bool isDefault(quint32 c) { return c == kDefault; }
inline bool isIndexed(quint32 c) { return (c & kKindMask) == kIndexed; }
inline bool isRgb(quint32 c) { return (c & kKindMask) == kRgb; }
inline quint32 indexed(int n) { return kIndexed | quint32(n & 0xFF); }
inline quint32 rgb(int r, int g, int b)
{
    return kRgb | (quint32(r & 0xFF) << 16) | (quint32(g & 0xFF) << 8) | quint32(b & 0xFF);
}
inline int index(quint32 c) { return int(c & 0xFF); }
} // namespace AnsiColor

struct AnsiSpan {
    enum Attr : quint8 {
        Bold      = 0x01,
        Dim       = 0x02,
        Italic    = 0x04,
        Underline = 0x08,
        Inverse   = 0x10,
    };
    int start = 0;   // в символах текста без кодов
    int length = 0;
    quint32 fg = AnsiColor::kDefault;
    quint32 bg = AnsiColor::kDefault;
    quint8 attrs = 0;

    friend bool operator==(const AnsiSpan& a, const AnsiSpan& b)
    {
        return a.start == b.start && a.length == b.length && a.fg == b.fg && a.bg == b.bg
            && a.attrs == b.attrs;
    }
    friend bool operator!=(const AnsiSpan& a, const AnsiSpan& b) { return !(a == b); }
};

// Отсортированы, не пересекаются, только участки с отличным от умолчания
// стилем; соседние с одинаковым стилем слиты.
using AnsiStyles = QVector<AnsiSpan>;

Q_DECLARE_METATYPE(AnsiStyles)

namespace AnsiEscape {

constexpr char kEsc = '\x1b';

inline bool hasEscape(const char* data, qsizetype len)
{
    return len > 0 && std::memchr(data, kEsc, size_t(len)) != nullptr;
}
inline bool hasEscape(QByteArrayView bytes) { return hasEscape(bytes.data(), bytes.size()); }
inline bool hasEscape(QStringView text) { return text.contains(QChar(u'\x1b')); }

// Байты без последовательностей в out (out не меньше len; можно in == out).
// Возвращает длину результата.
qsizetype stripBytes(const char* in, qsizetype len, char* out);
QByteArray strip(QByteArrayView bytes);
// Тот же разбор над UTF-16 (QTextStream резидентного пути, QStringDecoder
// сканера). Без ESC — ничего не делает.
void stripInPlace(QString& text);

// Конец данных, по которому их можно резать на куски без разрыва
// последовательности: начало последней последовательности, не законченной
// до конца данных (ищется в последних kMaxCutLookBack единицах), иначе len.
// Для сканера, который режет длинную строку на куски (FileContentSearch).
constexpr qsizetype kMaxCutLookBack = 256;
qsizetype completeEnd(QByteArrayView data);
qsizetype completeEnd(QStringView data);

// Спаны SGR строки (байты без EOL) в координатах decoder.decode(bytes) —
// текста без кодов. Пусто — в строке нет ESC или нет видимого стиля.
AnsiStyles styles(QByteArrayView bytes, const TextDecoder& decoder);
// То же для строки, прочитанной текстом (UTF-16/32).
AnsiStyles styles(QStringView text);

} // namespace AnsiEscape

#endif // ANSIESCAPE_H
