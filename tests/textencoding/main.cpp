// textencoding — модуль кодировок (F-1, шаг 1): таблицы однобайтных
// кодировок против эталона стандартов, TextDecoder, EncodingDetector
// (подсчёт UTF-8, decide, выбор кириллической кодировки), фаззинг decide и
// проверка вывода дизайн-документа «классификатор смотрит только на ASCII».
//
// Эталон таблиц записан здесь НЕЗАВИСИМО от src/textencoding.cpp и в другой
// форме: правильные участки (буквы А–я подряд) — правилом, остальное — парами
// «байт → кодовая точка». QStringDecoder эталоном не служит: однобайтных
// кодировок в нём может не быть (Qt без ICU).
//
// Без окна и без QApplication: модуль ядра.

#include "lineclassifier.h"
#include "textencoding.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QString>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <random>
#include <vector>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

using Table = std::array<char16_t, 128>;   // байт 0x80 + i → кодовая точка

// ---------------------------------------------------------------------------
// Эталон
// ---------------------------------------------------------------------------

struct Pair {
    unsigned char byte;
    char16_t code;
};

static Table fromPairs(std::initializer_list<Pair> pairs, Table table = {})
{
    for (const Pair& p : pairs)
        table[p.byte - 0x80] = p.code;
    return table;
}

// Windows-1251: 0xC0–0xFF — А…я подряд; 0x98 в стандарте не определён (C1).
static Table referenceWindows1251()
{
    Table t{};
    for (int b = 0xC0; b <= 0xFF; ++b)
        t[b - 0x80] = char16_t(0x0410 + (b - 0xC0));
    return fromPairs({
        {0x80, 0x0402}, {0x81, 0x0403}, {0x82, 0x201A}, {0x83, 0x0453}, // Ђ Ѓ ‚ ѓ
        {0x84, 0x201E}, {0x85, 0x2026}, {0x86, 0x2020}, {0x87, 0x2021}, // „ … † ‡
        {0x88, 0x20AC}, {0x89, 0x2030}, {0x8A, 0x0409}, {0x8B, 0x2039}, // € ‰ Љ ‹
        {0x8C, 0x040A}, {0x8D, 0x040C}, {0x8E, 0x040B}, {0x8F, 0x040F}, // Њ Ќ Ћ Џ
        {0x90, 0x0452}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201C}, // ђ ' ' "
        {0x94, 0x201D}, {0x95, 0x2022}, {0x96, 0x2013}, {0x97, 0x2014}, // " • – —
        {0x98, 0x0098}, {0x99, 0x2122}, {0x9A, 0x0459}, {0x9B, 0x203A}, // —  ™ љ ›
        {0x9C, 0x045A}, {0x9D, 0x045C}, {0x9E, 0x045B}, {0x9F, 0x045F}, // њ ќ ћ џ
        {0xA0, 0x00A0}, {0xA1, 0x040E}, {0xA2, 0x045E}, {0xA3, 0x0408}, // nbsp Ў ў Ј
        {0xA4, 0x00A4}, {0xA5, 0x0490}, {0xA6, 0x00A6}, {0xA7, 0x00A7}, // ¤ Ґ ¦ §
        {0xA8, 0x0401}, {0xA9, 0x00A9}, {0xAA, 0x0404}, {0xAB, 0x00AB}, // Ё © Є «
        {0xAC, 0x00AC}, {0xAD, 0x00AD}, {0xAE, 0x00AE}, {0xAF, 0x0407}, // ¬ shy ® Ї
        {0xB0, 0x00B0}, {0xB1, 0x00B1}, {0xB2, 0x0406}, {0xB3, 0x0456}, // ° ± І і
        {0xB4, 0x0491}, {0xB5, 0x00B5}, {0xB6, 0x00B6}, {0xB7, 0x00B7}, // ґ µ ¶ ·
        {0xB8, 0x0451}, {0xB9, 0x2116}, {0xBA, 0x0454}, {0xBB, 0x00BB}, // ё № є »
        {0xBC, 0x0458}, {0xBD, 0x0405}, {0xBE, 0x0455}, {0xBF, 0x0457}, // ј Ѕ ѕ ї
    }, t);
}

// IBM866: 0x80–0xAF — А…п, 0xE0–0xEF — р…я; между ними — псевдографика.
static Table referenceIbm866()
{
    Table t{};
    for (int b = 0x80; b <= 0xAF; ++b)
        t[b - 0x80] = char16_t(0x0410 + (b - 0x80));
    for (int b = 0xE0; b <= 0xEF; ++b)
        t[b - 0x80] = char16_t(0x0440 + (b - 0xE0));
    return fromPairs({
        {0xB0, 0x2591}, {0xB1, 0x2592}, {0xB2, 0x2593}, {0xB3, 0x2502},
        {0xB4, 0x2524}, {0xB5, 0x2561}, {0xB6, 0x2562}, {0xB7, 0x2556},
        {0xB8, 0x2555}, {0xB9, 0x2563}, {0xBA, 0x2551}, {0xBB, 0x2557},
        {0xBC, 0x255D}, {0xBD, 0x255C}, {0xBE, 0x255B}, {0xBF, 0x2510},
        {0xC0, 0x2514}, {0xC1, 0x2534}, {0xC2, 0x252C}, {0xC3, 0x251C},
        {0xC4, 0x2500}, {0xC5, 0x253C}, {0xC6, 0x255E}, {0xC7, 0x255F},
        {0xC8, 0x255A}, {0xC9, 0x2554}, {0xCA, 0x2569}, {0xCB, 0x2566},
        {0xCC, 0x2560}, {0xCD, 0x2550}, {0xCE, 0x256C}, {0xCF, 0x2567},
        {0xD0, 0x2568}, {0xD1, 0x2564}, {0xD2, 0x2565}, {0xD3, 0x2559},
        {0xD4, 0x2558}, {0xD5, 0x2552}, {0xD6, 0x2553}, {0xD7, 0x256B},
        {0xD8, 0x256A}, {0xD9, 0x2518}, {0xDA, 0x250C}, {0xDB, 0x2588},
        {0xDC, 0x2584}, {0xDD, 0x258C}, {0xDE, 0x2590}, {0xDF, 0x2580},
        {0xF0, 0x0401}, {0xF1, 0x0451}, {0xF2, 0x0404}, {0xF3, 0x0454}, // Ё ё Є є
        {0xF4, 0x0407}, {0xF5, 0x0457}, {0xF6, 0x040E}, {0xF7, 0x045E}, // Ї ї Ў ў
        {0xF8, 0x00B0}, {0xF9, 0x2219}, {0xFA, 0x00B7}, {0xFB, 0x221A}, // ° ∙ · √
        {0xFC, 0x2116}, {0xFD, 0x00A4}, {0xFE, 0x25A0}, {0xFF, 0x00A0}, // № ¤ ■ nbsp
    }, t);
}

// KOI8-R (RFC 1489): 0xC0–0xDF — строчные, 0xE0–0xFF — заглавные в порядке
// «юабцдефгхийклмнопярстужвьызшэщчъ» (номер буквы в алфавите а=0…я=31).
static Table referenceKoi8R()
{
    static const int kOrder[32] = {
        30, 0, 1, 22, 4, 5, 20, 3, 21, 8, 9, 10, 11, 12, 13, 14,
        15, 31, 16, 17, 18, 19, 6, 2, 28, 27, 7, 24, 29, 25, 23, 26,
    };
    Table t{};
    for (int i = 0; i < 32; ++i) {
        t[0xC0 + i - 0x80] = char16_t(0x0430 + kOrder[i]);
        t[0xE0 + i - 0x80] = char16_t(0x0410 + kOrder[i]);
    }
    return fromPairs({
        {0x80, 0x2500}, {0x81, 0x2502}, {0x82, 0x250C}, {0x83, 0x2510},
        {0x84, 0x2514}, {0x85, 0x2518}, {0x86, 0x251C}, {0x87, 0x2524},
        {0x88, 0x252C}, {0x89, 0x2534}, {0x8A, 0x253C}, {0x8B, 0x2580},
        {0x8C, 0x2584}, {0x8D, 0x2588}, {0x8E, 0x258C}, {0x8F, 0x2590},
        {0x90, 0x2591}, {0x91, 0x2592}, {0x92, 0x2593}, {0x93, 0x2320}, // ░ ▒ ▓ ⌠
        {0x94, 0x25A0}, {0x95, 0x2219}, {0x96, 0x221A}, {0x97, 0x2248}, // ■ ∙ √ ≈
        {0x98, 0x2264}, {0x99, 0x2265}, {0x9A, 0x00A0}, {0x9B, 0x2321}, // ≤ ≥ nbsp ⌡
        {0x9C, 0x00B0}, {0x9D, 0x00B2}, {0x9E, 0x00B7}, {0x9F, 0x00F7}, // ° ² · ÷
        {0xA0, 0x2550}, {0xA1, 0x2551}, {0xA2, 0x2552}, {0xA3, 0x0451}, // ═ ║ ╒ ё
        {0xA4, 0x2553}, {0xA5, 0x2554}, {0xA6, 0x2555}, {0xA7, 0x2556},
        {0xA8, 0x2557}, {0xA9, 0x2558}, {0xAA, 0x2559}, {0xAB, 0x255A},
        {0xAC, 0x255B}, {0xAD, 0x255C}, {0xAE, 0x255D}, {0xAF, 0x255E},
        {0xB0, 0x255F}, {0xB1, 0x2560}, {0xB2, 0x2561}, {0xB3, 0x0401}, // ╟ ╠ ╡ Ё
        {0xB4, 0x2562}, {0xB5, 0x2563}, {0xB6, 0x2564}, {0xB7, 0x2565},
        {0xB8, 0x2566}, {0xB9, 0x2567}, {0xBA, 0x2568}, {0xBB, 0x2569},
        {0xBC, 0x256A}, {0xBD, 0x256B}, {0xBE, 0x256C}, {0xBF, 0x00A9}, // ╪ ╫ ╬ ©
    }, t);
}

// Windows-1252: 0xA0–0xFF совпадают с Latin-1; 0x81, 0x8D, 0x8F, 0x90, 0x9D
// не определены (C1).
static Table referenceWindows1252()
{
    Table t{};
    for (int b = 0xA0; b <= 0xFF; ++b)
        t[b - 0x80] = char16_t(b);
    return fromPairs({
        {0x80, 0x20AC}, {0x81, 0x0081}, {0x82, 0x201A}, {0x83, 0x0192}, // € — ‚ ƒ
        {0x84, 0x201E}, {0x85, 0x2026}, {0x86, 0x2020}, {0x87, 0x2021}, // „ … † ‡
        {0x88, 0x02C6}, {0x89, 0x2030}, {0x8A, 0x0160}, {0x8B, 0x2039}, // ˆ ‰ Š ‹
        {0x8C, 0x0152}, {0x8D, 0x008D}, {0x8E, 0x017D}, {0x8F, 0x008F}, // Œ — Ž —
        {0x90, 0x0090}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201C}, // — ' ' "
        {0x94, 0x201D}, {0x95, 0x2022}, {0x96, 0x2013}, {0x97, 0x2014}, // " • – —
        {0x98, 0x02DC}, {0x99, 0x2122}, {0x9A, 0x0161}, {0x9B, 0x203A}, // ˜ ™ š ›
        {0x9C, 0x0153}, {0x9D, 0x009D}, {0x9E, 0x017E}, {0x9F, 0x0178}, // œ — ž Ÿ
    }, t);
}

struct Encoding {
    TextEncoding id;
    const char* name;
    Table table;
};

static const std::vector<Encoding>& singleByteEncodings()
{
    static const std::vector<Encoding> all = {
        {TextEncoding::Windows1251, "Windows-1251", referenceWindows1251()},
        {TextEncoding::Ibm866,      "IBM866",       referenceIbm866()},
        {TextEncoding::Koi8R,       "KOI8-R",       referenceKoi8R()},
        {TextEncoding::Windows1252, "Windows-1252", referenceWindows1252()},
    };
    return all;
}

static const Encoding& encodingOf(TextEncoding id)
{
    for (const Encoding& e : singleByteEncodings())
        if (e.id == id)
            return e;
    std::abort();
}

// Кодирование по эталону: символ, которого в кодировке нет, — nullopt.
static std::optional<QByteArray> encode(const QString& text, const Encoding& enc)
{
    QByteArray out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        if (c < 0x80) {
            out.append(char(c));
            continue;
        }
        bool found = false;
        for (int i = 0; i < 128; ++i) {
            if (enc.table[i] == c) {
                out.append(char(0x80 + i));
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;
    }
    return out;
}

static QByteArray bytes(std::initializer_list<int> list)
{
    QByteArray out;
    for (const int b : list)
        out.append(char(b));
    return out;
}

static QString hex(QStringView s)
{
    QString out;
    for (const QChar ch : s)
        out += QString::asprintf("%04X ", ch.unicode());
    return out;
}

// ---------------------------------------------------------------------------
// Таблицы и декодер
// ---------------------------------------------------------------------------

static void testTables()
{
    for (const Encoding& enc : singleByteEncodings()) {
        const TextDecoder decoder(enc.id);
        CHECK(decoder.encoding() == enc.id, QString("encoding() of ") + enc.name);
        CHECK(!decoder.isUtf8(), QString("isUtf8() of ") + enc.name);
        // Ни одного пропуска в эталоне.
        for (int i = 0; i < 128; ++i)
            CHECK(enc.table[i] != 0, QString("%1: reference gap at 0x%2").arg(enc.name).arg(0x80 + i, 0, 16));
        // Каждый старший байт по отдельности.
        int mismatches = 0;
        for (int b = 0x80; b <= 0xFF; ++b) {
            const char byte = char(b);
            const QString got = decoder.decode(&byte, 1);
            if (got.size() != 1 || got[0].unicode() != enc.table[b - 0x80]) {
                ++mismatches;
                std::fprintf(stderr, "  %s 0x%02X: got %s want %04X\n", enc.name, b,
                             qPrintable(hex(got)), unsigned(enc.table[b - 0x80]));
            }
        }
        CHECK(mismatches == 0, QString("%1: %2 of 128 bytes differ from the standard").arg(enc.name).arg(mismatches));

        // Все 256 байтов одной строкой: ASCII — как есть, старшие — по таблице.
        QByteArray all;
        for (int b = 0; b < 256; ++b)
            all.append(char(b));
        const QString text = decoder.decode(all);
        bool same = text.size() == 256;
        for (int b = 0; same && b < 256; ++b)
            same = text[b].unicode() == (b < 0x80 ? char16_t(b) : enc.table[b - 0x80]);
        CHECK(same, QString("%1: decode of all 256 bytes").arg(enc.name));

        // remapLatin1 — тот же результат из Latin-1-строки.
        QString latin = QString::fromLatin1(all);
        decoder.remapLatin1(latin);
        CHECK(latin == text, QString("%1: remapLatin1 == decode").arg(enc.name));

        // Таблица — биекция байтов на символы (кодирование обратно однозначно).
        std::map<char16_t, int> seen;
        for (int i = 0; i < 128; ++i)
            ++seen[enc.table[i]];
        CHECK(seen.size() == 128, QString("%1: table is not one-to-one").arg(enc.name));
    }
}

static void testDecoderBasics()
{
    // По умолчанию — UTF-8, ровно QString::fromUtf8 (в том числе на битых байтах).
    const TextDecoder utf8;
    CHECK(utf8.isUtf8() && utf8.encoding() == TextEncoding::Utf8, "default decoder is UTF-8");
    CHECK(TextDecoder(TextEncoding::Utf8) == utf8, "TextDecoder(Utf8) == default");
    const QByteArray samples[] = {
        QByteArray("plain ascii line"),
        bytes({0xD0, 0x9F, 0xD1, 0x80, 0xD0, 0xB8, 'x'}),
        bytes({'a', 0xCF, 0xF0, 0xE8, 'b', 0xFF, 0xE2, 0x82}),
        bytes({0xF0, 0x9F, 0x98, 0x80, ' ', 0xED, 0xA0, 0x80}),
        QByteArray(),
    };
    for (const QByteArray& s : samples)
        CHECK(utf8.decode(s) == QString::fromUtf8(s), "UTF-8 decode == fromUtf8: " + hex(QString::fromLatin1(s)));

    // remapLatin1 у UTF-8 ничего не делает.
    QString untouched = QString::fromLatin1(bytes({'a', 0xE9, 0xFF}));
    const QString before = untouched;
    utf8.remapLatin1(untouched);
    CHECK(untouched == before, "UTF-8 remapLatin1 is a no-op");

    // ASCII не меняется ни в одной кодировке; старшие байты в середине и
    // на краях длинной строки (проход по 8 байт и хвост).
    const TextDecoder cp1251(TextEncoding::Windows1251);
    const QByteArray ascii("2026-11-18 12:00:00.123 ERROR [main] connection refused: 10.0.0.1");
    for (const Encoding& enc : singleByteEncodings())
        CHECK(TextDecoder(enc.id).decode(ascii) == QString::fromLatin1(ascii), QString("ASCII unchanged in ") + enc.name);
    for (int pos = 0; pos < ascii.size(); ++pos) {
        QByteArray line = ascii;
        line[pos] = char(0xC0);   // А в Windows-1251
        QString want = QString::fromLatin1(ascii);
        want[pos] = QChar(0x0410);
        CHECK(cp1251.decode(line) == want, QString("cp1251 high byte at %1").arg(pos));
    }

    // remapLatin1 не трогает символы выше U+00FF и не отцепляет ASCII-строку.
    QString mixed = QString::fromLatin1(bytes({'a', 0xC0})) + QChar(0x20AC);
    cp1251.remapLatin1(mixed);
    CHECK(mixed.size() == 3 && mixed[1].unicode() == 0x0410 && mixed[2].unicode() == 0x20AC,
          "remapLatin1 leaves chars above U+00FF alone: " + hex(mixed));
    QString shared = QStringLiteral("only ascii");
    const QString copy = shared;
    cp1251.remapLatin1(shared);
    CHECK(shared.constData() == copy.constData(), "remapLatin1 does not detach an ASCII string");

    // Декодер — значение: копия равна оригиналу, кодировки различаются.
    const TextDecoder copyDecoder = cp1251;
    CHECK(copyDecoder == cp1251 && copyDecoder != utf8, "decoder copy compares equal");
}

static void testNamesAndBom()
{
    using namespace TextEncodings;
    const TextEncoding all[] = {
        TextEncoding::Utf8, TextEncoding::Windows1251, TextEncoding::Ibm866, TextEncoding::Koi8R,
        TextEncoding::Windows1252, TextEncoding::Utf16LE, TextEncoding::Utf16BE,
        TextEncoding::Utf32LE, TextEncoding::Utf32BE,
    };
    for (const TextEncoding e : all) {
        const QString name = displayName(e);
        CHECK(!name.isEmpty(), "displayName is empty");
        // Имя декодируемой кодировки читается обратно; UTF-16/32 вручную не выбираются.
        CHECK(fromName(name) == (isDecodable(e) ? std::optional<TextEncoding>(e) : std::nullopt),
              "fromName(displayName) round trip: " + name);
        if (const auto qt = toQt(e))
            CHECK(fromQt(*qt) == e, "toQt/fromQt round trip: " + name);
        else
            CHECK(isSingleByte(e), "only single-byte encodings lack a Qt converter: " + name);
    }
    CHECK(fromName(u"cp1251") == TextEncoding::Windows1251, "alias cp1251");
    CHECK(fromName(u"CP866") == TextEncoding::Ibm866, "alias CP866");
    CHECK(fromName(u"koi8_r") == TextEncoding::Koi8R, "alias koi8_r");
    CHECK(fromName(u"utf8") == TextEncoding::Utf8, "alias utf8");
    CHECK(!fromName(u"latin1").has_value() && !fromName(u"utf-16").has_value(), "unknown names");
    CHECK(isCyrillic(TextEncoding::Koi8R) && !isCyrillic(TextEncoding::Windows1252)
          && !isCyrillic(TextEncoding::Utf8), "isCyrillic");

    const auto bom = [](std::initializer_list<int> b) { return detectBom(bytes(b)); };
    CHECK(bom({0xEF, 0xBB, 0xBF, 'a'})->encoding == TextEncoding::Utf8 && bom({0xEF, 0xBB, 0xBF})->length == 3, "UTF-8 BOM");
    CHECK(bom({0xFF, 0xFE, 'a', 0})->encoding == TextEncoding::Utf16LE && bom({0xFF, 0xFE})->length == 2, "UTF-16LE BOM");
    CHECK(bom({0xFE, 0xFF, 0, 'a'})->encoding == TextEncoding::Utf16BE, "UTF-16BE BOM");
    CHECK(bom({0xFF, 0xFE, 0, 0})->encoding == TextEncoding::Utf32LE && bom({0xFF, 0xFE, 0, 0})->length == 4, "UTF-32LE BOM");
    CHECK(bom({0, 0, 0xFE, 0xFF})->encoding == TextEncoding::Utf32BE, "UTF-32BE BOM");
    CHECK(!bom({0xEF, 0xBB}).has_value() && !bom({'a', 'b', 'c'}).has_value() && !bom({}).has_value(), "no BOM");
    // Согласие с автоопределением Qt (QTextStream и Q-15 берут его).
    const QByteArray heads[] = {
        bytes({0xEF, 0xBB, 0xBF, 'x'}), bytes({0xFF, 0xFE, 'x', 0}), bytes({0xFE, 0xFF, 0, 'x'}),
        bytes({0xFF, 0xFE, 0, 0}), bytes({0, 0, 0xFE, 0xFF}), QByteArray("abcd"),
    };
    for (const QByteArray& head : heads) {
        const auto ours = detectBom(head);
        const auto qt = QStringConverter::encodingForData(head);
        CHECK(ours.has_value() == qt.has_value()
              && (!ours || fromQt(*qt) == ours->encoding), "detectBom agrees with Qt: " + hex(QString::fromLatin1(head)));
    }
}

// ---------------------------------------------------------------------------
// EncodingDetector
// ---------------------------------------------------------------------------

static void testFirstHighByte()
{
    using EncodingDetector::firstHighByte;
    CHECK(firstHighByte(nullptr, 0) == -1, "empty");
    QByteArray ascii(100, 'a');
    CHECK(firstHighByte(ascii) == -1, "all ASCII");
    for (int pos = 0; pos < 100; ++pos) {
        QByteArray s = ascii;
        s[pos] = char(0x80);
        if (pos + 5 < 100)
            s[pos + 5] = char(0xFF);
        CHECK(firstHighByte(s) == pos, QString("first high byte at %1").arg(pos));
        // Не дальше длины: байт за концом не виден.
        CHECK(firstHighByte(s.constData(), pos) == -1, QString("bounded by len at %1").arg(pos));
    }
}

static void testCountUtf8()
{
    using EncodingDetector::countUtf8;
    const auto counts = [](const QByteArray& b) {
        const auto c = countUtf8(b);
        return std::pair<qsizetype, qsizetype>(c.validUtf8, c.invalidBytes);
    };
    using P = std::pair<qsizetype, qsizetype>;
    CHECK(counts("ascii only") == P(0, 0), "ASCII: nothing");
    CHECK(counts(bytes({0xD0, 0x9F, 0xD1, 0x80})) == P(2, 0), "two 2-byte sequences");
    CHECK(counts(bytes({0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80})) == P(2, 0), "3- and 4-byte sequences");
    CHECK(counts(bytes({0x80, 0xBF})) == P(0, 2), "lone continuation bytes");
    CHECK(counts(bytes({0xC0, 0xAF, 0xC1, 0x81})) == P(0, 4), "overlong C0/C1");
    CHECK(counts(bytes({0xE0, 0x80, 0x80})) == P(0, 3), "overlong E0 80");
    CHECK(counts(bytes({0xED, 0xA0, 0x80})) == P(0, 3), "surrogate ED A0");
    CHECK(counts(bytes({0xF4, 0x90, 0x80, 0x80})) == P(0, 4), "above U+10FFFF");
    CHECK(counts(bytes({0xF5, 0xFF})) == P(0, 2), "F5, FF");
    CHECK(counts(bytes({0xCF, 0xF0, 0xE8, '\n'})) == P(0, 3), "cp1251 word");
    CHECK(counts(bytes({0xCF, 0xF0, 0xE8})) == P(0, 2), "cp1251 word cut by the window: last lead not counted");
    CHECK(counts(bytes({0xD0, 'a', 0xD0, 0x9F})) == P(1, 1), "broken lead does not swallow ASCII");
    // Оборванная краем окна: не считается вовсе — ни валидной, ни битой.
    CHECK(counts(bytes({0xD0, 0x9F, 0xD0})) == P(1, 0), "truncated 2-byte at the edge");
    CHECK(counts(bytes({0xD0, 0x9F, 0xE2, 0x82})) == P(1, 0), "truncated 3-byte at the edge");
    CHECK(counts(bytes({0xF0, 0x9F, 0x98})) == P(0, 0), "truncated 4-byte only");
    // Но битая на краю (второй байт уже невалиден) — битая.
    CHECK(counts(bytes({0xE0, 0x80})) == P(0, 2), "invalid second byte at the edge");
}

static void testDecideFixed()
{
    using EncodingDetector::decide;
    // «Привет, мир» в разных кодировках.
    const QString text = QString::fromUtf16(u"Привет, мир");
    const QByteArray utf8 = text.toUtf8();
    const QByteArray cp1251 = *encode(text, encodingOf(TextEncoding::Windows1251));
    const QByteArray cp866 = *encode(text, encodingOf(TextEncoding::Ibm866));
    const QByteArray koi8 = *encode(text, encodingOf(TextEncoding::Koi8R));
    const QByteArray prefix("2026-11-18 12:00:00 INFO ");

    CHECK(!decide("only ASCII bytes", TextEncoding::Windows1251).has_value(), "ASCII: undecided");
    CHECK(!decide(QByteArray(), TextEncoding::Windows1251).has_value(), "empty: undecided");
    CHECK(decide(prefix + utf8, TextEncoding::Windows1251) == TextEncoding::Utf8, "UTF-8, Cyrillic fallback");
    CHECK(decide(prefix + utf8, TextEncoding::Windows1252) == TextEncoding::Utf8, "UTF-8, Latin fallback");
    CHECK(decide(prefix + cp1251, TextEncoding::Windows1251) == TextEncoding::Windows1251, "cp1251");
    CHECK(decide(prefix + cp866, TextEncoding::Windows1251) == TextEncoding::Ibm866, "cp866");
    CHECK(decide(prefix + koi8, TextEncoding::Windows1251) == TextEncoding::Koi8R, "KOI8-R");
    // Запасная кириллическая — любая из трёх: выбор по байтам, а не по ней.
    CHECK(decide(cp1251, TextEncoding::Koi8R) == TextEncoding::Windows1251, "cp1251 with KOI8-R fallback");
    CHECK(decide(koi8, TextEncoding::Ibm866) == TextEncoding::Koi8R, "KOI8-R with cp866 fallback");
    // Запасная не кириллическая — она и есть ответ.
    CHECK(decide(cp1251, TextEncoding::Windows1252) == TextEncoding::Windows1252, "Latin fallback is kept");
    const QByteArray latin = *encode(QString::fromUtf16(u"café naïve über"), encodingOf(TextEncoding::Windows1252));
    CHECK(decide(latin, TextEncoding::Windows1252) == TextEncoding::Windows1252, "cp1252 text");
    CHECK(decide(latin, TextEncoding::Utf16LE) == TextEncoding::Windows1252, "non-single-byte fallback -> Windows-1252");

    // UTF-8 с одним битым байтом остаётся UTF-8.
    QByteArray broken = prefix + utf8;
    broken.insert(prefix.size() + 4, char(0xFF));
    CHECK(decide(broken, TextEncoding::Windows1251) == TextEncoding::Utf8, "UTF-8 with a broken byte");
    // Кусок cp1251 внутри UTF-8-строки (вставка из другого источника): UTF-8 больше.
    CHECK(decide(prefix + utf8 + " " + utf8 + " " + cp1251.left(3), TextEncoding::Windows1251) == TextEncoding::Utf8,
          "UTF-8 with a short cp1251 fragment");

    // Окно, оборванное посреди символа UTF-8.
    const QByteArray cut = prefix + utf8.left(utf8.size() - 1);   // «р» без второго байта
    CHECK(decide(cut, TextEncoding::Windows1251) == TextEncoding::Utf8, "window cut inside a UTF-8 sequence");
    CHECK(!decide(prefix + bytes({0xD0}), TextEncoding::Windows1251).has_value(), "only a truncated sequence: undecided");
    // Один кириллический символ UTF-8 — уже решение.
    CHECK(decide(prefix + bytes({0xD0, 0x90}), TextEncoding::Windows1251) == TextEncoding::Utf8, "single UTF-8 char");
}

// ---------------------------------------------------------------------------
// Фаззинг decide
// ---------------------------------------------------------------------------

static const char16_t* const kWords[] = {
    u"\u043E\u0448\u0438\u0431\u043A\u0430",
    u"\u0441\u043E\u0435\u0434\u0438\u043D\u0435\u043D\u0438\u0435",
    u"\u0441\u0435\u0440\u0432\u0435\u0440",
    u"\u043F\u043E\u043B\u044C\u0437\u043E\u0432\u0430\u0442\u0435\u043B\u044C",
    u"\u0437\u0430\u043F\u0440\u043E\u0441", u"\u043E\u0442\u0432\u0435\u0442",
    u"\u0444\u0430\u0439\u043B", u"\u043D\u0435", u"\u043D\u0430\u0439\u0434\u0435\u043D",
    u"\u0443\u0434\u0430\u043B\u043E\u0441\u044C", u"\u043E\u0442\u043A\u0440\u044B\u0442\u044C",
    u"\u043F\u043E\u0434\u043A\u043B\u044E\u0447\u0435\u043D\u0438\u0435",
    u"\u0431\u0430\u0437\u0430", u"\u0434\u0430\u043D\u043D\u044B\u0445",
    u"\u0432\u0440\u0435\u043C\u044F", u"\u043E\u0436\u0438\u0434\u0430\u043D\u0438\u044F",
    u"\u0438\u0441\u0442\u0435\u043A\u043B\u043E", u"\u0441\u043B\u0443\u0436\u0431\u0430",
    u"\u0437\u0430\u043F\u0443\u0449\u0435\u043D\u0430",
    u"\u043E\u0441\u0442\u0430\u043D\u043E\u0432\u043B\u0435\u043D\u0430",
    u"\u043F\u0440\u0435\u0434\u0443\u043F\u0440\u0435\u0436\u0434\u0435\u043D\u0438\u0435",
    u"\u0437\u0430\u0433\u0440\u0443\u0437\u043A\u0430",
    u"\u0437\u0430\u0432\u0435\u0440\u0448\u0435\u043D\u0430",
    u"\u0443\u0441\u043F\u0435\u0448\u043D\u043E",
    u"\u043E\u0442\u043A\u0430\u0437\u0430\u043D\u043E", u"\u0432",
    u"\u0434\u043E\u0441\u0442\u0443\u043F\u0435",
    u"\u043F\u043E\u0432\u0442\u043E\u0440\u043D\u0430\u044F",
    u"\u043F\u043E\u043F\u044B\u0442\u043A\u0430", u"\u0447\u0435\u0440\u0435\u0437",
    u"\u0441\u0435\u043A\u0443\u043D\u0434",
    u"\u043E\u0431\u043D\u043E\u0432\u043B\u0435\u043D\u0438\u0435",
    u"\u043A\u043E\u043D\u0444\u0438\u0433\u0443\u0440\u0430\u0446\u0438\u0438",
    u"\u043F\u0430\u043C\u044F\u0442\u0438",
    u"\u043D\u0435\u0434\u043E\u0441\u0442\u0430\u0442\u043E\u0447\u043D\u043E",
    u"\u0434\u0438\u0441\u043A", u"\u0437\u0430\u043F\u043E\u043B\u043D\u0435\u043D",
    u"\u0441\u0435\u0442\u044C", u"\u043D\u0435\u0434\u043E\u0441\u0442\u0443\u043F\u043D\u0430",
    u"\u043F\u0430\u0440\u0430\u043C\u0435\u0442\u0440",
    u"\u043D\u0435\u0432\u0435\u0440\u043D\u044B\u0439", u"\u0444\u043E\u0440\u043C\u0430\u0442",
    u"\u0441\u0442\u0440\u043E\u043A\u0438", u"\u043A\u043B\u044E\u0447",
    u"\u043E\u0442\u0441\u0443\u0442\u0441\u0442\u0432\u0443\u0435\u0442",
    u"\u043F\u0440\u043E\u0446\u0435\u0441\u0441",
    u"\u0437\u0430\u0432\u0435\u0440\u0448\u0438\u043B\u0441\u044F", u"\u0441",
    u"\u043A\u043E\u0434\u043E\u043C", u"\u0436\u0443\u0440\u043D\u0430\u043B",
    u"\u0441\u043E\u0431\u044B\u0442\u0438\u0439",
    u"\u0442\u0440\u0430\u043D\u0437\u0430\u043A\u0446\u0438\u044F",
    u"\u043E\u0442\u043C\u0435\u043D\u0435\u043D\u0430",
    u"\u0441\u0435\u0440\u0442\u0438\u0444\u0438\u043A\u0430\u0442",
    u"\u043F\u0440\u043E\u0441\u0440\u043E\u0447\u0435\u043D",
    u"\u043F\u0430\u0440\u043E\u043B\u044C", u"\u0438\u0437\u043C\u0435\u043D\u0451\u043D",
    u"\u0443\u0447\u0451\u0442\u043D\u0430\u044F", u"\u0437\u0430\u043F\u0438\u0441\u044C",
    u"\u0437\u0430\u0431\u043B\u043E\u043A\u0438\u0440\u043E\u0432\u0430\u043D\u0430",
    u"\u0437\u0430\u0434\u0430\u043D\u0438\u0435",
    u"\u0432\u044B\u043F\u043E\u043B\u043D\u0435\u043D\u043E",
    u"\u043E\u0447\u0435\u0440\u0435\u0434\u044C",
    u"\u0441\u043E\u043E\u0431\u0449\u0435\u043D\u0438\u0439",
    u"\u043F\u0435\u0440\u0435\u043F\u043E\u043B\u043D\u0435\u043D\u0430",
    u"\u0448\u0440\u0438\u0444\u0442", u"\u0449\u0438\u0442",
    u"\u043E\u0431\u044A\u0435\u043A\u0442", u"\u0441\u044A\u0451\u043C\u043D\u044B\u0439",
    u"\u044D\u0445\u043E", u"\u044E\u0433", u"\u044F\u0431\u043B\u043E\u043A\u043E",
    u"\u0446\u0438\u043A\u043B", u"\u0447\u0442\u0435\u043D\u0438\u0435",
    u"\u0436\u0451\u0441\u0442\u043A\u0438\u0439", u"\u0445\u044D\u0448",
    u"\u0444\u0438\u043B\u044C\u0442\u0440", u"\u0451\u043C\u043A\u043E\u0441\u0442\u044C",
    u"\u0449\u0435\u043B\u0447\u043E\u043A",
    u"\u0432\u044B\u0447\u0438\u0441\u043B\u0438\u0442\u044C",
};

static QString randomWord(std::mt19937& rng)
{
    const int count = int(sizeof(kWords) / sizeof(kWords[0]));
    QString w = QString::fromUtf16(kWords[std::uniform_int_distribution<int>(0, count - 1)(rng)]);
    const int style = std::uniform_int_distribution<int>(0, 9)(rng);
    if (style == 0)
        w = w.toUpper();                       // 10% — ВСЕ ЗАГЛАВНЫЕ
    else if (style <= 2)
        w[0] = w[0].toUpper();                 // 20% — С заглавной
    return w;
}

static QString randomPhrase(std::mt19937& rng, int words)
{
    QString s;
    for (int i = 0; i < words; ++i) {
        if (i)
            s += std::uniform_int_distribution<int>(0, 7)(rng) == 0 ? QStringLiteral(", ") : QStringLiteral(" ");
        s += randomWord(rng);
    }
    return s;
}

// Строка лога: ASCII-префикс, кириллица, иногда ASCII-вставки.
static QString randomLine(std::mt19937& rng)
{
    static const char* const kLevels[] = {"INFO", "WARN", "ERROR", "DEBUG"};
    QString line = QStringLiteral("2026-11-18 12:%1:%2.%3 %4 [worker-%5] ")
        .arg(std::uniform_int_distribution<int>(10, 59)(rng))
        .arg(std::uniform_int_distribution<int>(10, 59)(rng))
        .arg(std::uniform_int_distribution<int>(100, 999)(rng))
        .arg(QLatin1String(kLevels[std::uniform_int_distribution<int>(0, 3)(rng)]))
        .arg(std::uniform_int_distribution<int>(1, 16)(rng));
    line += randomPhrase(rng, std::uniform_int_distribution<int>(3, 12)(rng));
    if (std::uniform_int_distribution<int>(0, 2)(rng) == 0)
        line += QStringLiteral(": id=%1 path=/var/log/app.log").arg(std::uniform_int_distribution<int>(1, 99999)(rng));
    return line;
}

static void testDecideFuzz()
{
    std::mt19937 rng(20261118);
    const TextEncoding cyrillic[] = {TextEncoding::Windows1251, TextEncoding::Ibm866, TextEncoding::Koi8R};

    // Окно от первой не-ASCII строки: несколько строк лога. Ошибок быть не должно.
    const int kLongTrials = 2000;
    for (const TextEncoding id : cyrillic) {
        const Encoding& enc = encodingOf(id);
        int wrong = 0, utf8Wrong = 0;
        for (int t = 0; t < kLongTrials; ++t) {
            QString window;
            const int lines = std::uniform_int_distribution<int>(1, 6)(rng);
            for (int i = 0; i < lines; ++i)
                window += randomLine(rng) + QLatin1Char('\n');
            const QByteArray encoded = *encode(window, enc);
            for (const TextEncoding fallback : cyrillic)
                wrong += EncodingDetector::decide(encoded, fallback) != id;
            utf8Wrong += EncodingDetector::decide(window.toUtf8(), id) != TextEncoding::Utf8;
        }
        std::printf("fuzz %-12s lines: %d/%d wrong, UTF-8 copy: %d/%d wrong\n", enc.name, wrong,
                    kLongTrials * 3, utf8Wrong, kLongTrials);
        CHECK(wrong == 0, QString("%1: misdetected %2 log windows").arg(enc.name).arg(wrong));
        CHECK(utf8Wrong == 0, QString("%1: UTF-8 copy misdetected %2 times").arg(enc.name).arg(utf8Wrong));
    }

    // Короткие окна: строка из одного или двух слов без ASCII-контекста (окно
    // кончается переводом строки, как у строки в середине файла). Ошибки
    // возможны — их доля печатается и ограничена сверху: ошибка исправляется
    // одним щелчком, а окно из пары слов в логе — редкость (обычно решает
    // строка целиком и всё, что за ней в пределах 64 КБ).
    const int kShortTrials = 20000;
    for (const int words : {1, 2}) {
        for (const TextEncoding id : cyrillic) {
            const Encoding& enc = encodingOf(id);
            int wrong = 0, utf8Wrong = 0;
            std::map<QString, int> confusedWith;
            // TEXTENCODING_DUMP=1 — печатать каждое ошибочное окно.
            for (int t = 0; t < kShortTrials; ++t) {
                const QString phrase = randomPhrase(rng, words) + QLatin1Char('\n');
                const auto got = EncodingDetector::decide(*encode(phrase, enc), TextEncoding::Windows1251);
                if (got != id) {
                    if (qEnvironmentVariableIsSet("TEXTENCODING_DUMP"))
                        std::printf("  %s -> %s: %s", enc.name, got ? qPrintable(TextEncodings::displayName(*got)) : "-", qPrintable(phrase));
                    ++wrong;
                    ++confusedWith[got ? TextEncodings::displayName(*got) : QStringLiteral("undecided")];
                }
                utf8Wrong += EncodingDetector::decide(phrase.toUtf8(), id) != TextEncoding::Utf8;
            }
            QString confused;
            for (const auto& [name, n] : confusedWith)
                confused += QStringLiteral(" %1=%2").arg(name).arg(n);
            const double rate = 100.0 * wrong / kShortTrials;
            std::printf("fuzz %-12s %d word(s): %.2f%% wrong (%d/%d)%s; UTF-8 copy: %d wrong\n", enc.name,
                        words, rate, wrong, kShortTrials, qPrintable(confused), utf8Wrong);
            CHECK(utf8Wrong == 0, QString("%1: short UTF-8 copy misdetected").arg(enc.name));
            CHECK(rate <= (words == 1 ? 10.0 : 2.0),
                  QString("%1: %2-word windows misdetected %3%").arg(enc.name).arg(words).arg(rate));
        }
    }
}

// ---------------------------------------------------------------------------
// Вывод F-1 «классификатор смотрит только на ASCII»: одна и та же строка,
// прочитанная как UTF-8 и в своей однобайтной кодировке, даёт тот же момент,
// уровень и признак ведущей метки — после F-10 (новые формы меток) и F-12
// (ведущая метка). Кириллица стоит до, вокруг и после метки и уровня.
// ---------------------------------------------------------------------------

static void testClassifierIsAsciiOnly()
{
    const LineClassifier classifier;
    const LineClassifier::YearReference year = LineClassifier::yearReference(QDate(2026, 11, 18));
    const QString cyr = QString::fromUtf16(u"Ошибка");     // «Ошибка»
    const QString cyr2 = QString::fromUtf16(u"ёж ящик"); // «ёж ящик»
    const QString templates[] = {
        QStringLiteral("2026-11-18 13:55:36,123 ERROR %1 %2"),
        QStringLiteral("%1 2026-11-18T13:55:36.5Z WARN %2"),
        QStringLiteral("[2026-11-18 13:55:36] %1: INFO %2"),
        QStringLiteral("%1[2026-11-18 13:55:36] %2 DEBUG"),
        QStringLiteral("2026/11/18 13:55:36 [error] %1 %2"),
        QStringLiteral("10.0.0.1 - %1 [18/Nov/2026:13:55:36 +0300] \"GET /%2 HTTP/1.1\" 200"),
        QStringLiteral("%1 - - [18/Nov/2026:13:55:36 +0000] \"GET / HTTP/1.1\" 500 %2"),
        QStringLiteral("Nov 18 13:55:36 host app[42]: %1 %2"),
        QStringLiteral("Nov  8 13:55:36 host %1: %2 error"),
        QStringLiteral("18.11.2026 13:55:36 %1 FATAL %2"),
        QStringLiteral("18/11/2026 13:55:36.250 +03:00 %1 %2"),
        QStringLiteral("%1 18.11.2026 13:55:36 %2"),
        QStringLiteral("%1%2 2026-11-18 13:55:36 UTC"),
        QStringLiteral("\t%1 at com.example.Foo(%2.java:42)"),
        QStringLiteral("%1 WARNING%2 %1"),
        QStringLiteral("%1ERROR %2"),
        QStringLiteral("%1 %2"),
    };
    int compared = 0;
    for (const Encoding& enc : singleByteEncodings()) {
        if (!TextEncodings::isCyrillic(enc.id))
            continue;
        const TextDecoder native(enc.id);
        const TextDecoder asUtf8;
        for (const QString& tpl : templates) {
            const QString line = tpl.arg(cyr, cyr2);
            const QByteArray raw = *encode(line, enc);
            const QString a = native.decode(raw);
            const QString b = asUtf8.decode(raw);   // как сейчас читает индексатор
            CHECK(a == line, "native decode restores the line");
            qint64 msA = 0, msB = 0;
            bool leadA = false, leadB = false;
            const bool tsA = classifier.detectTimestampMs(a, msA, year, &leadA);
            const bool tsB = classifier.detectTimestampMs(b, msB, year, &leadB);
            LogLevel lvA = LogLevel::Unknown, lvB = LogLevel::Unknown;
            const bool hasA = classifier.detectLogLevel(a, lvA);
            const bool hasB = classifier.detectLogLevel(b, lvB);
            CHECK(tsA == tsB && (!tsA || msA == msB) && leadA == leadB,
                  QString("%1: timestamp differs between decodings: %2").arg(enc.name, tpl));
            CHECK(hasA == hasB && lvA == lvB,
                  QString("%1: level differs between decodings: %2").arg(enc.name, tpl));
            CHECK(LineClassifier::isPrimaryLine(false, tsA, leadA, hasA)
                  == LineClassifier::isPrimaryLine(false, tsB, leadB, hasB),
                  QString("%1: primary flag differs: %2").arg(enc.name, tpl));
            ++compared;
        }
    }
    // Шаблоны действительно что-то находят (иначе проверка пустая).
    qint64 ms = 0;
    bool lead = false;
    CHECK(classifier.detectTimestampMs(templates[7].arg(cyr, cyr2), ms, year, &lead) && lead, "syslog template is found");
    CHECK(classifier.detectTimestampMs(templates[5].arg(cyr, cyr2), ms, year, &lead) && lead, "access line: leading timestamp (F-12)");
    CHECK(classifier.detectTimestampMs(templates[1].arg(cyr, cyr2), ms, year, &lead) && !lead, "ISO after text: mid-line timestamp");
    std::printf("classifier: %d line(s) compared across decodings\n", compared);

    // Основание вывода: битая последовательность UTF-8 не поглощает следующий
    // за ней ASCII-байт (fromUtf8 даёт U+FFFD и продолжает с этого байта).
    const QByteArray broken = bytes({0xD0, '2', 0xE2, 0x82, '0', 0xF0, 0x9F, '-', 0xC3});
    const QString decoded = QString::fromUtf8(broken);
    QString asciiOnly;
    for (const QChar ch : decoded)
        if (ch.unicode() < 0x80)
            asciiOnly += ch;
    CHECK(asciiOnly == QStringLiteral("20-"), "fromUtf8 keeps ASCII after broken sequences: " + hex(decoded));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testTables();
    testDecoderBasics();
    testNamesAndBom();
    testFirstHighByte();
    testCountUtf8();
    testDecideFixed();
    testDecideFuzz();
    testClassifierIsAsciiOnly();
    if (failures) {
        std::fprintf(stderr, "textencoding: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("textencoding: OK\n");
    return 0;
}
