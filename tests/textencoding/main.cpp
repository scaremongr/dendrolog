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

#include "bytelinereader.h"
#include "lineclassifier.h"
#include "logfile.h"
#include "logparser.h"
#include "textencoding.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QString>
#include <QStringDecoder>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>

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

        // Все байты одной строкой: ASCII — как есть, старшие — по таблице.
        // Кроме ESC (0x1B): декодер снимает ANSI-последовательности (F-3,
        // tests/ansi_escape), одиночный ESC — тоже.
        QByteArray all;
        QVector<int> order;
        for (int b = 0; b < 256; ++b) {
            if (b == 0x1B)
                continue;
            all.append(char(b));
            order.append(b);
        }
        const QString text = decoder.decode(all);
        bool same = text.size() == order.size();
        for (int i = 0; same && i < order.size(); ++i) {
            const int b = order[i];
            same = text[i].unicode() == (b < 0x80 ? char16_t(b) : enc.table[b - 0x80]);
        }
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
// Шаг 3а: предварительное решение и свидетельство
// ---------------------------------------------------------------------------

// Сжатое свидетельство решает так же, как исходные байты: ASCII в decide
// значит только «разделяет байты ≥ 0x80».
static void testEvidenceCompression()
{
    std::mt19937 rng(3);
    const TextEncoding all[] = {TextEncoding::Windows1251, TextEncoding::Ibm866,
                                TextEncoding::Koi8R, TextEncoding::Windows1252};
    int mismatches = 0, trials = 0;
    for (int t = 0; t < 4000; ++t) {
        QString text;
        const int lines = std::uniform_int_distribution<int>(1, 5)(rng);
        for (int i = 0; i < lines; ++i)
            text += randomLine(rng) + QLatin1Char('\n');
        const int kind = t % 5;
        QByteArray raw = kind == 4 ? text.toUtf8()
                                   : *encode(text, encodingOf(TextEncoding(int(TextEncoding::Windows1251) + kind % 3)));
        // Мусорные байты и обрыв окна посреди последовательности.
        if (std::uniform_int_distribution<int>(0, 3)(rng) == 0)
            raw.insert(std::uniform_int_distribution<int>(0, int(raw.size()))(rng), char(0xFF));
        if (std::uniform_int_distribution<int>(0, 3)(rng) == 0)
            raw.chop(std::uniform_int_distribution<int>(0, 3)(rng));
        EncodingEvidence ev;
        const qsizetype used = EncodingDetector::appendEvidence(ev, raw.constData(), raw.size(),
                                                                qsizetype(1) << 30);
        CHECK(used == raw.size(), "evidence: unlimited append consumes everything");
        for (const TextEncoding fallback : all) {
            ++trials;
            mismatches += EncodingDetector::decide(QByteArrayView(ev.bytes), fallback)
                       != EncodingDetector::decide(raw, fallback);
        }
        CHECK(qsizetype(ev.bytes.size()) <= 2 * ev.highBytes + 1, "evidence: ASCII runs collapse");
    }
    CHECK(mismatches == 0, QString("evidence: compressed decides differently in %1/%2").arg(mismatches).arg(trials));
    // Лимит: ровно столько байтов ≥ 0x80, остаток не поглощается.
    EncodingEvidence ev;
    const QByteArray raw = QByteArray("ab\xD0\x96 cd\xD0\x96\xD0\x96 ef");
    const qsizetype used = EncodingDetector::appendEvidence(ev, raw.constData(), raw.size(), 3);
    CHECK(ev.highBytes == 3 && ev.bytes == std::string("\xD0\x96\n\xD0") && used == 8,
          "evidence: stops at the limit");
}

// Файл для advance: окно у первого решения дочитывается из файла.
struct FeedFile {
    QString path;
    QByteArray bytes;
};

static FeedFile writeFeedFile(const QString& dir, const QString& name, const QByteArray& bytes)
{
    FeedFile f{dir + QLatin1Char('/') + name, bytes};
    QFile out(f.path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size())
        std::abort();
    return f;
}

struct FeedResult {
    FileEncoding state;
    QVector<FileEncoding> changes; // что advance сообщил (true)
};

// Как индексатор: подряд идущие порции (буферы) случайного размера с from.
static FeedResult feedChunks(const FeedFile& f, FileEncoding state, std::mt19937& rng,
                             qint64 from = 0, qint64 to = -1)
{
    FeedResult r;
    const qint64 end = to < 0 ? f.bytes.size() : to;
    for (qint64 pos = from; pos < end;) {
        const qint64 n = qMin<qint64>(end - pos, std::uniform_int_distribution<int>(1, 300000)(rng));
        if (EncodingDetector::advance(state, QByteArrayView(f.bytes).sliced(pos, n), pos, f.path,
                                      TextEncoding::Windows1251))
            r.changes.append(state);
        pos += n;
    }
    r.state = state;
    return r;
}

// Как парсер: только не-ASCII строки, без EOL, с '\n', если строка им кончилась.
static FeedResult feedLines(const FeedFile& f, FileEncoding state, qint64 from = 0,
                            qint64 to = -1)
{
    FeedResult r;
    const qint64 end = to < 0 ? f.bytes.size() : to;
    for (qint64 pos = from; pos < end;) {
        qint64 nl = f.bytes.indexOf('\n', pos);
        const bool complete = nl >= 0 && nl < end;
        const qint64 lineEnd = complete ? nl : end;
        qint64 len = lineEnd - pos;
        if (complete && len > 0 && f.bytes.at(lineEnd - 1) == '\r')
            --len;
        QByteArray portion = f.bytes.mid(pos, len);
        if (!state.detectionSettled() && EncodingDetector::firstHighByte(portion) >= 0) {
            if (complete)
                portion.append('\n');
            if (EncodingDetector::advance(state, portion, pos, f.path, TextEncoding::Windows1251))
                r.changes.append(state);
        }
        pos = complete ? nl + 1 : end;
    }
    r.state = state;
    return r;
}

static bool sameDecision(const FileEncoding& a, const FileEncoding& b)
{
    return a.decoder == b.decoder && a.source == b.source && a.provisional == b.provisional
        && (a.provisional ? a.evidence.bytes == b.evidence.bytes : true);
}

static QByteArray asciiFiller(qint64 bytes)
{
    QByteArray out;
    int i = 0;
    while (out.size() < bytes)
        out += "2026-11-18 12:00:" + QByteArray::number(10 + i++ % 50) + ".000 INFO [app] plain ascii line\n";
    return out;
}

static QByteArray cyrillicLines(std::mt19937& rng, int lines, TextEncoding enc)
{
    QString text;
    for (int i = 0; i < lines; ++i)
        text += randomLine(rng) + QLatin1Char('\n');
    return enc == TextEncoding::Utf8 ? text.toUtf8() : *encode(text, encodingOf(enc));
}

static void testProvisionalScenarios()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "provisional: temp dir");
    std::mt19937 rng(77);
    const qint64 window = EncodingDetector::kWindowBytes;

    // 1. Мусорный байт, больше окна ASCII, затем кириллица UTF-8: первое
    //    решение по одному байту — предварительное однобайтное, итог — UTF-8,
    //    окончательно.
    {
        const QByteArray bytes = "head \xFF tail\n" + asciiFiller(window + 1000)
                               + cyrillicLines(rng, 40, TextEncoding::Utf8);
        const FeedFile f = writeFeedFile(tmp.path(), "garbage_utf8.log", bytes);
        for (int mode = 0; mode < 2; ++mode) {
            const FeedResult r = mode ? feedLines(f, {}) : feedChunks(f, {}, rng);
            const QString tag = mode ? "lines" : "chunks";
            // Строками (и буферами, если первый кончился до кириллицы) первое
            // решение — по одному байту.
            if (mode)
                CHECK(r.changes.size() >= 2 && r.changes.first().provisional
                          && r.changes.first().decoder.encoding() == TextEncoding::Windows1251,
                      "provisional garbage: first decision is provisional single-byte, " + tag);
            CHECK(r.state.decoder.isUtf8() && r.state.source == FileEncoding::Source::Auto
                      && !r.state.provisional,
                  "provisional garbage: revised to final UTF-8, " + tag);
        }
    }
    // 3. Одно короткое слово cp1251 — cp1251, предварительно.
    {
        const QString line = QStringLiteral("2026-11-18 12:00:00.000 INFO [app] ") + QString::fromUtf16(u"ошибка") + "\n";
        const FeedFile f = writeFeedFile(tmp.path(), "one_word.log",
                                         asciiFiller(5000) + *encode(line, encodingOf(TextEncoding::Windows1251))
                                             + asciiFiller(5000));
        for (int mode = 0; mode < 2; ++mode) {
            const FeedResult r = mode ? feedLines(f, {}) : feedChunks(f, {}, rng);
            CHECK(r.changes.size() == 1 && r.state.decoder.encoding() == TextEncoding::Windows1251
                      && r.state.provisional,
                  "provisional one word: cp1251, provisional");
        }
    }
    // 4. cp1251: короткое слово, затем за окном — русский текст. Кодировка не
    //    меняется ни разу, решение становится окончательным.
    {
        const QString word = QStringLiteral("2026-11-18 12:00:00.000 INFO [app] ") + QString::fromUtf16(u"старт") + "\n";
        const QByteArray bytes = *encode(word, encodingOf(TextEncoding::Windows1251))
                               + asciiFiller(window + 5000)
                               + cyrillicLines(rng, 200, TextEncoding::Windows1251);
        const FeedFile f = writeFeedFile(tmp.path(), "late_1251.log", bytes);
        for (int mode = 0; mode < 2; ++mode) {
            const FeedResult r = mode ? feedLines(f, {}) : feedChunks(f, {}, rng);
            bool same = true;
            for (const FileEncoding& c : r.changes)
                same = same && c.decoder.encoding() == TextEncoding::Windows1251;
            CHECK(same && r.changes.size() == 2 && r.changes.first().provisional
                      && !r.changes.last().provisional,
                  "provisional late cp1251: one encoding, provisional then final");
        }
    }
    // 5. Окончательное UTF-8 не пересматривается блоком мусора.
    {
        QByteArray garbage;
        for (int i = 0; i < 4000; ++i)
            garbage += char(0x80 + i % 0x7F);
        const QByteArray bytes = asciiFiller(1000) + cyrillicLines(rng, 30, TextEncoding::Utf8)
                               + asciiFiller(window * 2) + garbage + "\n"
                               + cyrillicLines(rng, 30, TextEncoding::Windows1251);
        const FeedFile f = writeFeedFile(tmp.path(), "final_utf8.log", bytes);
        for (int mode = 0; mode < 2; ++mode) {
            const FeedResult r = mode ? feedLines(f, {}) : feedChunks(f, {}, rng);
            CHECK(r.changes.size() == 1 && r.state.decoder.isUtf8() && !r.state.provisional,
                  "provisional final utf8: one final decision survives garbage");
        }
    }
    // 6. BOM и выбор пользователя не пересматриваются.
    {
        const FeedFile f = writeFeedFile(tmp.path(), "user.log",
                                         cyrillicLines(rng, 30, TextEncoding::Windows1251));
        const FileEncoding user = FileEncoding().withUserChoice(TextDecoder(TextEncoding::Utf8));
        FileEncoding bom{TextDecoder(TextEncoding::Utf8), FileEncoding::Source::Bom};
        for (const FileEncoding& start : {user, bom}) {
            const FeedResult r = feedChunks(f, start, rng);
            CHECK((start.source == FileEncoding::Source::User || r.changes.isEmpty())
                      && r.state.decoder.isUtf8() && r.state.source == start.source,
                  "provisional: user choice and BOM are never revised");
        }
        // Под выбором пользователя автоматика решает рядом (шаг 4) — так же,
        // как без выбора, строками и буферами.
        {
            const FileEncoding plain = feedLines(f, {}).state;
            const FeedResult byLines = feedLines(f, user);
            const FeedResult byChunks = feedChunks(f, user, rng);
            CHECK(byLines.state.decoder.isUtf8() && byChunks.state.decoder.isUtf8()
                      && sameDecision(byLines.state.automatic(), plain)
                      && sameDecision(byChunks.state.automatic(), plain),
                  "provisional: detection under a user choice decides alongside, the choice stays");
        }
        // Выбор пользователя хранит решение автоматики рядом.
        FileEncoding autoState = feedLines(f, {}).state;
        const FileEncoding chosen = autoState.withUserChoice(TextDecoder(TextEncoding::Koi8R));
        CHECK(chosen.source == FileEncoding::Source::User
                  && chosen.decoder.encoding() == TextEncoding::Koi8R
                  && sameDecision(chosen.automatic(), autoState),
              "provisional: user choice keeps the automatic decision beside it");
        CHECK(sameDecision(chosen.withAutomatic(autoState).automatic(), autoState)
                  && chosen.withAutomatic(autoState).decoder.encoding() == TextEncoding::Koi8R,
              "provisional: a late automatic decision updates the kept one, not the choice");
    }
    // Буфер индексатора кончается ведущим байтом UTF-8, следующий — только
    // ASCII: строка парсера с этим байтом в конце подаётся с '\n' (байт
    // битый), и буферы обязаны прийти к тому же — разделитель ставит
    // ASCII-порция, а не только следующий байт ≥ 0x80.
    {
        // Мусор (предварительное), затем больше окна ASCII: окно кончается
        // раньше края буфера.
        // Счёт подобран так, что битый байт решает: 1 мусорный + 1 битый
        // против двух «Ж» — запасная; без него — UTF-8.
        const QByteArray head = "x \xFF y\n" + asciiFiller(window + 1000);
        const QByteArray cut = "ab \xD0\x96 \xD0\x96 \xD0"; // две «Ж» и оборванный ведущий байт
        const QByteArray bytes = head + cut + "\n" + asciiFiller(1000);
        const FeedFile f = writeFeedFile(tmp.path(), "edge_lead.log", bytes);
        const qint64 split = head.size() + cut.size();
        FileEncoding byBuffers;
        EncodingDetector::advance(byBuffers, QByteArrayView(bytes).first(split), 0, f.path,
                                  TextEncoding::Windows1251);
        EncodingDetector::advance(byBuffers, QByteArrayView(bytes).sliced(split, 500), split,
                                  f.path, TextEncoding::Windows1251);
        EncodingDetector::advance(byBuffers, QByteArrayView(bytes).sliced(split + 500), split + 500,
                                  f.path, TextEncoding::Windows1251);
        const FileEncoding byLines = feedLines(f, {}).state;
        CHECK(byBuffers.provisional && sameDecision(byBuffers, byLines),
              "provisional: a lead byte at a buffer edge followed by ASCII counts the same as in lines");
    }
    // Независимость от порций: случайные файлы, случайное чтение —
    // буферами, строками, двумя проходами (дозапись посреди строки с
    // перечитыванием предварительной строки, переход между «бэкендами»).
    {
        int mismatches = 0;
        const TextEncoding kinds[] = {TextEncoding::Utf8, TextEncoding::Windows1251,
                                      TextEncoding::Ibm866, TextEncoding::Koi8R};
        for (int t = 0; t < 120; ++t) {
            QByteArray bytes;
            const int parts = std::uniform_int_distribution<int>(1, 8)(rng);
            for (int p = 0; p < parts; ++p) {
                const int what = std::uniform_int_distribution<int>(0, 9)(rng);
                if (what < 4)
                    bytes += asciiFiller(std::uniform_int_distribution<int>(10, 90000)(rng));
                else if (what == 4)
                    bytes += QByteArray("x\xFF\xFE y\r\n");
                else
                    bytes += cyrillicLines(rng, std::uniform_int_distribution<int>(1, 4)(rng),
                                           kinds[t % 4]);
            }
            if (t % 3 == 0)
                bytes.replace("\n", "\r\n");
            if (t % 4 == 1)
                bytes.chop(std::uniform_int_distribution<int>(1, 5)(rng)); // хвост без '\n'
            const FeedFile f = writeFeedFile(tmp.path(), QStringLiteral("rand%1.log").arg(t), bytes);
            const FileEncoding byChunks = feedChunks(f, {}, rng).state;
            const FileEncoding byLines = feedLines(f, {}).state;
            // Два прохода: первый кончается посреди строки (предварительная),
            // второй перечитывает её с начала, вторым «бэкендом».
            const qint64 cut = std::uniform_int_distribution<qint64>(0, bytes.size())(rng);
            qint64 lineStart = bytes.lastIndexOf('\n', cut - 1) + 1;
            if (cut == 0)
                lineStart = 0;
            const FileEncoding first = feedLines(f, {}, 0, cut).state;
            const FileEncoding twoPass = feedChunks(f, first, rng, lineStart).state;
            const FileEncoding firstChunks = feedChunks(f, {}, rng, 0, cut).state;
            const FileEncoding twoPassRev = feedLines(f, firstChunks, lineStart).state;
            const bool ok = sameDecision(byChunks, byLines) && sameDecision(byChunks, twoPass)
                         && sameDecision(byChunks, twoPassRev);
            if (!ok && qEnvironmentVariableIsSet("TEXTENCODING_DUMP"))
                std::printf("  chunking mismatch #%d: %s/%s/%s/%s\n", t,
                            qPrintable(TextEncodings::displayName(byChunks.decoder.encoding())),
                            qPrintable(TextEncodings::displayName(byLines.decoder.encoding())),
                            qPrintable(TextEncodings::displayName(twoPass.decoder.encoding())),
                            qPrintable(TextEncodings::displayName(twoPassRev.decoder.encoding())));
            mismatches += !ok;
        }
        CHECK(mismatches == 0, QString("provisional: decision depends on chunking in %1/120 files").arg(mismatches));
    }
}

// Порог окончательности: на свидетельстве из kFinalHighBytes байтов ≥ 0x80
// decide не ошибается. Печатает долю ошибок для меньших порогов — таблица в
// docs/design/F-1.md, «Шаг 3а». Свидетельство — строки лога с 1–3 словами
// кириллицы (разреженный лог: каждое слово — отдельная порция), иногда
// мусорный байт в начале.
static void testProvisionalFuzz()
{
    std::mt19937 rng(20261118 + 3);
    const TextEncoding kinds[] = {TextEncoding::Windows1251, TextEncoding::Ibm866,
                                  TextEncoding::Koi8R, TextEncoding::Utf8};
    const qsizetype thresholds[] = {8, 16, 32, 64, 128, 256};
    const int kTrials = 3000;
    for (const bool garbage : {false, true}) {
        for (const TextEncoding id : kinds) {
            QString row;
            for (const qsizetype k : thresholds) {
                int wrong = 0;
                for (int t = 0; t < kTrials; ++t) {
                    EncodingEvidence ev;
                    if (garbage)
                        EncodingDetector::appendEvidence(ev, "\xFF\n", 2, k);
                    while (ev.highBytes < k) {
                        const QString phrase = randomPhrase(rng, std::uniform_int_distribution<int>(1, 3)(rng))
                                             + QLatin1Char('\n');
                        const QByteArray raw = id == TextEncoding::Utf8 ? phrase.toUtf8()
                                                                         : *encode(phrase, encodingOf(id));
                        EncodingDetector::appendEvidence(ev, raw.constData(), raw.size(), k);
                    }
                    wrong += EncodingDetector::decide(QByteArrayView(ev.bytes), TextEncoding::Windows1251) != id;
                }
                row += QStringLiteral(" K=%1: %2%").arg(k).arg(100.0 * wrong / kTrials, 0, 'f', 2);
                if (k == EncodingDetector::kFinalHighBytes)
                    CHECK(wrong == 0, QString("provisional fuzz %1%2: final threshold misdetects %3/%4")
                                          .arg(TextEncodings::displayName(id), garbage ? " +garbage" : "")
                                          .arg(wrong).arg(kTrials));
            }
            std::printf("fuzz threshold %-12s%s%s\n", qPrintable(TextEncodings::displayName(id)),
                        garbage ? " +garbage" : "         ", qPrintable(row));
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

// ---------------------------------------------------------------------------
// Риск шага 3 (docs/design/F-1.md, «Риски»): резидентный путь читал файл
// QTextStream с кодировкой Latin1 и автоопределением BOM — как транспорт
// байтов (с Q-18 — ByteLineReader, а QTextStream остался у UTF-16/32; его
// разрез строк — эталон ByteLineReader, testByteLineReader). Закрепляем
// поведение потока: без BOM строка — байты один к
// одному (все 254 значения, кроме '\n' и '\r'), с BOM поток переходит на
// UTF-8/16/32 и снимает BOM, разрез строк и снятие '\r' — как в режиме UTF-8,
// pos() — точное байтовое смещение, а при чтении с середины без
// автоопределения байты EF BB BF — обычные символы, не BOM.
// ---------------------------------------------------------------------------

static QStringList readAllLines(const QByteArray& data, bool latin1, qint64 seekTo = 0,
                                bool autoDetect = true,
                                QStringConverter::Encoding* finalEncoding = nullptr,
                                qint64* finalPos = nullptr)
{
    QTemporaryFile file;
    file.open();
    file.write(data);
    file.flush();
    file.seek(seekTo);
    QTextStream in(&file);
    if (latin1)
        in.setEncoding(QStringConverter::Latin1);
    in.setAutoDetectUnicode(autoDetect);
    QStringList lines;
    while (!in.atEnd())
        lines << in.readLine();
    if (finalEncoding)
        *finalEncoding = in.encoding();
    if (finalPos)
        *finalPos = in.pos();
    return lines;
}

static void testTextStreamLatin1Transport()
{
    // 1. Без BOM: каждый байт — символ с тем же номером; кодировка остаётся Latin1.
    QByteArray allBytes;
    for (int b = 1; b < 256; ++b)
        if (b != '\n' && b != '\r')
            allBytes.append(char(b));
    QByteArray data;
    // Больше буфера QTextStream (16 КБ): строки режутся на границах буфера.
    for (int i = 0; i < 200; ++i)
        data += allBytes + "\n";
    data += allBytes; // последняя строка без '\n'
    QStringConverter::Encoding enc = QStringConverter::Utf8;
    qint64 pos = -1;
    const QStringList lines = readAllLines(data, true, 0, true, &enc, &pos);
    CHECK(lines.size() == 201, QStringLiteral("latin1: line count %1").arg(lines.size()));
    bool oneToOne = true;
    for (const QString& line : lines) {
        if (line.size() != allBytes.size()) { oneToOne = false; break; }
        for (qsizetype i = 0; i < line.size(); ++i)
            if (line[i].unicode() != uchar(allBytes[i])) { oneToOne = false; break; }
    }
    CHECK(oneToOne, "latin1: every byte is the character with the same number");
    CHECK(enc == QStringConverter::Latin1, "latin1: no BOM keeps Latin1");
    CHECK(pos == data.size(), QStringLiteral("latin1: pos() is the byte offset (%1 vs %2)").arg(pos).arg(data.size()));

    // pos() посреди файла: после каждой строки — точное смещение начала следующей.
    {
        QTemporaryFile file;
        file.open();
        const QByteArray mixed = QByteArray("ab\xE0\xE1\r\n") + "\xFF\n" + "\r\n" + "x\ry\n" + "end";
        file.write(mixed);
        file.flush();
        file.seek(0);
        QTextStream in(&file);
        in.setEncoding(QStringConverter::Latin1);
        in.setAutoDetectUnicode(true);
        const qint64 expected[] = {6, 8, 10, 14, 17};
        int i = 0;
        while (!in.atEnd() && i < 5) {
            in.readLine();
            CHECK(in.pos() == expected[i], QStringLiteral("latin1: pos after line %1 = %2").arg(i).arg(in.pos()));
            ++i;
        }
        CHECK(i == 5, "latin1: five lines");
    }

    // 2. Разрез строк и снятие '\r' — как у потока UTF-8 (семантика резидентного
    // пути не меняется): "\r\n" снимается, одиночный '\r' внутри и в конце —
    // по тем же правилам; пустые строки и строка без '\n' в конце.
    const QByteArray ascii("a\r\nb\rc\n\n\r\nd\r\r\ne\r\n\nlast\r");
    CHECK(readAllLines(ascii, true) == readAllLines(ascii, false),
          "latin1: same line split and '\\r' handling as UTF-8: "
              + readAllLines(ascii, true).join(u'|') + " vs " + readAllLines(ascii, false).join(u'|'));
    const QByteArray asciiNoTail("one\ntwo\n");
    CHECK(readAllLines(asciiNoTail, true) == readAllLines(asciiNoTail, false),
          "latin1: no phantom line after the final '\\n', same as UTF-8");

    // 3. BOM: поток переходит на кодировку BOM и снимает его.
    const QString text = QStringLiteral("Привет, мир\nвторая строка");
    struct BomCase {
        QStringConverter::Encoding encoding;
        const char* name;
    };
    const BomCase cases[] = {
        {QStringConverter::Utf8, "UTF-8"},
        {QStringConverter::Utf16LE, "UTF-16LE"},
        {QStringConverter::Utf16BE, "UTF-16BE"},
        {QStringConverter::Utf32LE, "UTF-32LE"},
        {QStringConverter::Utf32BE, "UTF-32BE"},
    };
    for (const BomCase& c : cases) {
        QStringEncoder encoder(c.encoding, QStringConverter::Flag::WriteBom);
        const QByteArray encoded = encoder.encode(text);
        QStringConverter::Encoding detected = QStringConverter::Latin1;
        qint64 endPos = -1;
        const QStringList got = readAllLines(encoded, true, 0, true, &detected, &endPos);
        const bool sameFamily = detected == c.encoding
            || (c.encoding == QStringConverter::Utf16LE && detected == QStringConverter::Utf16 && QSysInfo::ByteOrder == QSysInfo::LittleEndian)
            || (c.encoding == QStringConverter::Utf32LE && detected == QStringConverter::Utf32 && QSysInfo::ByteOrder == QSysInfo::LittleEndian);
        const auto detectedName = QStringConverter::nameForEncoding(detected);
        CHECK(sameFamily || TextEncodings::fromQt(detected) == TextEncodings::fromQt(c.encoding),
              QString::fromLatin1(c.name) + ": BOM switches the stream, got "
                  + QString::fromLatin1(detectedName ? detectedName : "?"));
        CHECK(got == text.split(u'\n'),
              QString::fromLatin1(c.name) + ": BOM stripped, text decoded: " + got.join(u'|'));
        CHECK(endPos == encoded.size(),
              QString::fromLatin1(c.name) + QStringLiteral(": pos() at the end = %1 of %2").arg(endPos).arg(encoded.size()));
    }

    // pos() посреди потока, начатого с середины файла: поток сдвигается
    // своим seek — тогда pos() точен. (Устройство, сдвинутое до создания
    // потока, даёт pos() от нуля, пока буфер не прочитан целиком: опорная
    // позиция буфера QTextStream — 0. Поэтому парсер двигает поток.)
    {
        QTemporaryFile file;
        file.open();
        const QByteArray prefix("0123456789\n");
        const QByteArray body = QByteArray("ab\xE0\r\n") + "\xFF\xFE\n" + "tail";
        file.write(prefix + body);
        file.flush();
        QTextStream in(&file);
        in.setEncoding(QStringConverter::Latin1);
        in.setAutoDetectUnicode(false);
        CHECK(in.seek(prefix.size()), "latin1: stream seek");
        const qint64 expected[] = {prefix.size() + 5, prefix.size() + 8, prefix.size() + 12};
        int i = 0;
        while (!in.atEnd() && i < 3) {
            in.readLine();
            CHECK(in.pos() == expected[i],
                  QStringLiteral("latin1 from the middle: pos after line %1 = %2, want %3")
                      .arg(i).arg(in.pos()).arg(expected[i]));
            ++i;
        }
        CHECK(i == 3, "latin1 from the middle: three lines");
    }

    // 4. Дозапись: чтение с середины без автоопределения — байты EF BB BF и
    // FF FE там обычные символы, не BOM.
    const QByteArray head("head\n");
    const QByteArray tail = QByteArray("\xEF\xBB\xBFx\n\xFF\xFEy\n");
    QStringConverter::Encoding tailEnc = QStringConverter::Utf8;
    const QStringList tailLines = readAllLines(head + tail, true, head.size(), false, &tailEnc);
    CHECK(tailLines.size() == 2 && tailLines.value(0) == QString::fromLatin1("\xEF\xBB\xBFx")
              && tailLines.value(1) == QString::fromLatin1("\xFF\xFEy"),
          "latin1 tail: BOM-like bytes in the middle are plain characters: " + tailLines.join(u'|'));
    CHECK(tailEnc == QStringConverter::Latin1, "latin1 tail: encoding stays Latin1");
}


// ---------------------------------------------------------------------------
// Q-18: транспорт резидентного пути — ByteLineReader вместо QTextStream
// ---------------------------------------------------------------------------

// Эталон разбиения байтов файла на строки — независимо от ByteLineReader:
// строка кончается '\n' ('\r' перед ним снимается), хвост без '\n' — строка
// ('\r' в его конце снимается, как у QTextStream в конце данных).
struct RefLine {
    qint64 start = 0;
    QByteArray text;
    int eol = 0;
    bool newline = false;
};

static QVector<RefLine> referenceLines(const QByteArray& data, qint64 from = 0)
{
    QVector<RefLine> out;
    qint64 pos = from;
    while (pos < data.size()) {
        RefLine r;
        r.start = pos;
        const qint64 nl = data.indexOf('\n', pos);
        const qint64 end = nl >= 0 ? nl : data.size();
        r.text = data.mid(pos, end - pos);
        r.newline = nl >= 0;
        r.eol = r.newline ? 1 : 0;
        if (r.text.endsWith('\r')) {
            r.text.chop(1);
            ++r.eol;
        }
        out.append(r);
        pos = r.newline ? nl + 1 : data.size();
    }
    return out;
}

static QByteArray randomLineBytes(std::mt19937& rng, int maxLen)
{
    // Алфавит с перекосом в сторону того, что ломает разрез: '\r', '\n',
    // байты ≥ 0x80 и NUL.
    static const char kAlphabet[] = {'a', 'b', ' ', '\r', '\n', '\n', '\r', char(0xE0),
                                     char(0xD0), char(0xFF), '\0', 'z'};
    const int len = std::uniform_int_distribution<int>(0, maxLen)(rng);
    QByteArray out;
    for (int i = 0; i < len; ++i)
        out.append(kAlphabet[std::uniform_int_distribution<int>(0, int(sizeof kAlphabet) - 1)(rng)]);
    return out;
}

static void testByteLineReader()
{
    QTemporaryDir dir;
    std::mt19937 rng(18);
    const qsizetype chunks[] = {1, 2, 3, 7, 64, 1 << 20};
    int cases = 0;
    for (int iter = 0; iter < 600; ++iter) {
        QByteArray data = randomLineBytes(rng, iter < 400 ? 60 : 40000);
        if (iter % 50 == 7)
            data.prepend("\xEF\xBB\xBF");
        const QString path = dir.path() + QStringLiteral("/r%1.log").arg(iter);
        {
            QFile out(path);
            if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
                std::abort();
        }
        // Старт: с начала, за BOM и с начала каждой строки (как дозапись).
        QVector<qint64> starts{0};
        if (data.startsWith("\xEF\xBB\xBF"))
            starts << 3;
        for (const RefLine& r : referenceLines(data))
            if (r.start > 0 && starts.size() < 6)
                starts << r.start;
        for (const qint64 from : starts) {
            const QVector<RefLine> ref = referenceLines(data, from);

            // QTextStream с Latin1 — прежний транспорт: те же строки, тот же текст.
            QStringList stream;
            {
                QFile f(path);
                f.open(QIODevice::ReadOnly);
                QTextStream in(&f);
                in.setAutoDetectUnicode(false);
                in.setEncoding(QStringConverter::Latin1);
                if (from > 0)
                    in.seek(from);
                while (!in.atEnd())
                    stream << in.readLine();
            }

            for (const qsizetype chunk : chunks) {
                QFile f(path);
                f.open(QIODevice::ReadOnly);
                f.seek(from);
                ByteLineReader reader(&f, from, chunk);
                QVector<RefLine> got;
                while (reader.next()) {
                    RefLine r;
                    r.start = reader.lineStart();
                    r.text = reader.line().toByteArray();
                    r.eol = reader.eolBytes();
                    r.newline = reader.endsWithNewline();
                    got.append(r);
                }
                ++cases;
                bool same = got.size() == ref.size();
                for (qsizetype i = 0; same && i < got.size(); ++i)
                    same = got[i].start == ref[i].start && got[i].text == ref[i].text
                        && got[i].eol == ref[i].eol && got[i].newline == ref[i].newline;
                CHECK(same, QStringLiteral("ByteLineReader vs reference split: file %1 from %2 chunk %3 (%4 vs %5 lines)")
                                .arg(iter).arg(from).arg(chunk).arg(got.size()).arg(ref.size()));
                CHECK(reader.pos() == data.size(),
                      QStringLiteral("ByteLineReader: pos() at the end %1 of %2").arg(reader.pos()).arg(data.size()));
                QStringList asText;
                for (const RefLine& r : got)
                    asText << QString::fromLatin1(r.text);
                CHECK(asText == stream,
                      QStringLiteral("ByteLineReader vs QTextStream Latin1: file %1 from %2 chunk %3 (%4 vs %5 lines)")
                          .arg(iter).arg(from).arg(chunk).arg(asText.size()).arg(stream.size()));
                if (!same || asText != stream)
                    return; // одной подробной ошибки достаточно
            }
        }
    }

    // Растущий файл: читатель дочитывает дописанное после «конца» —
    // оборванная строка без '\n' и "\r", разорванный посреди "\r\n".
    {
        const QString path = dir.path() + QStringLiteral("/grow.log");
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write("one\r\ntwo\r");
        out.flush();
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        ByteLineReader reader(&f, 0, 4);
        CHECK(reader.next() && reader.line() == "one" && reader.eolBytes() == 2 && reader.lineStart() == 0,
              "grow: first line");
        CHECK(reader.next() && reader.line() == "two" && reader.eolBytes() == 1 && !reader.endsWithNewline()
                  && reader.lineStart() == 5,
              "grow: '\\r' at the current end is stripped, line is not terminated");
        CHECK(!reader.next() && reader.pos() == 9, "grow: end");
        // Дописано: строка дочитывается со своего начала, как парсер дозаписи.
        out.write("\nthree");
        out.flush();
        QFile g(path);
        g.open(QIODevice::ReadOnly);
        g.seek(5);
        ByteLineReader again(&g, 5, 4);
        CHECK(again.next() && again.line() == "two" && again.eolBytes() == 2 && again.endsWithNewline(),
              "grow: the line torn between '\\r' and '\\n' reads whole");
        CHECK(again.next() && again.line() == "three" && again.lineStart() == 10 && !again.next()
                  && again.pos() == 15,
              "grow: appended line");
    }
    std::printf("ByteLineReader: %d reader runs match the reference split and QTextStream\n", cases);
}

// Прогон LogParser (полный разбор или дозапись) до конца прохода.
struct ParserRun {
    QVector<FileEncoding> decided;     // каждое encodingDecided по порядку
    FileEncoding finalState;
    qint64 endOffset = -1;
    bool lastLinePartial = false;
    qint64 lastLineStart = -1;
    QStringList texts;
    bool finished = false;
};

static ParserRun runParser(const QString& path, FileEncoding state, qint64 from = 0,
                           TextEncoding fallback = TextEncoding::Windows1251)
{
    ParserRun run;
    run.finalState = state;
    auto lf = std::make_shared<LogFile>(path);
    LogParser parser;
    parser.setFallbackEncoding(fallback);
    QObject relay;
    QObject::connect(&parser, &LogParser::encodingDecided, &relay,
                     [&](const LogFilePtr&, const FileEncoding& e) {
                         run.decided.append(e);
                         run.finalState = e;
                     });
    QObject::connect(&parser, &LogParser::entriesParsed, &relay,
                     [&](const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr&) {
                         for (const auto& e : batch)
                             run.texts << e->message();
                     });
    QObject::connect(&parser, &LogParser::tailState, &relay,
                     [&](const LogFilePtr&, qint64 end, bool partial, qint64 lastStart, TextEncoding) {
                         run.endOffset = end;
                         run.lastLinePartial = partial;
                         run.lastLineStart = lastStart;
                     });
    QObject::connect(&parser, &LogParser::parsingFinished, &relay,
                     [&](int, const LogFilePtr&) { run.finished = true; });
    QObject::connect(&parser, &LogParser::parsingFailed, &relay,
                     [&](const LogFilePtr&) { run.finished = true; });
    if (from == 0)
        parser.startParsing(lf, state);
    else
        parser.startParsingFrom(lf, from, LogParser::ResumeContext{}, TextEncoding::Utf8, state);
    QElapsedTimer t;
    t.start();
    while (!run.finished && t.elapsed() < 600000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return run;
}

// Эталон того, что парсер подаёт автоматике: не-ASCII строки по эталонному
// разбиению, с '\n', если строка им кончилась, НАЧАЛО СТРОКИ — по байтам файла.
static FileEncoding referenceAdvance(const QString& path, const QByteArray& data, FileEncoding state,
                                     qint64 from, int* changes,
                                     TextEncoding fallback = TextEncoding::Windows1251)
{
    for (const RefLine& r : referenceLines(data, from)) {
        if (state.detectionSettled() || EncodingDetector::firstHighByte(r.text) < 0)
            continue;
        QByteArray portion = r.text;
        if (r.newline)
            portion.append('\n');
        if (EncodingDetector::advance(state, portion, r.start, path, fallback))
            ++*changes;
    }
    return state;
}

static bool sameEncodingState(const FileEncoding& a, const FileEncoding& b)
{
    return a.decoder == b.decoder && a.source == b.source && a.provisional == b.provisional
        && a.evidence.bytes == b.evidence.bytes && a.evidence.end == b.evidence.end
        && a.evidence.highBytes == b.evidence.highBytes && a.autoDecoder == b.autoDecoder
        && a.autoSource == b.autoSource;
}

// Позиция не-ASCII строки, переданная парсером в EncodingDetector::advance,
// против эталона — через состояние автоматики: evidence.end = начало порции
// + поглощённое, а пропуск уже учтённого идёт по нему же, поэтому сдвиг
// начала хотя бы одной строки меняет свидетельство. Решение держится
// предварительным (меньше kFinalHighBytes байтов ≥ 0x80) — advance зовётся на
// каждой не-ASCII строке. Плюс tailState против эталона.
static void testParserLinePositions()
{
    QTemporaryDir dir;
    std::mt19937 rng(1818);
    // Не-ASCII вставки: cp1251-слово, UTF-8-слово, одиночный байт.
    const QByteArray words[] = {QByteArray("\xEF\xF0\xE8\xE2\xE5\xF2"), QByteArray("\xD0\xBC\xD0\xB8\xD1\x80"),
                                QByteArray("\xFF")};
    struct Variant {
        const char* name;
        int eolKind;      // 0 — "\n", 1 — "\r\n", 2 — смешанные, 3 — со одиночным '\r' в строках
        bool bom;
        bool partialTail; // последняя строка без '\n'
        bool crTail;      // последняя строка оканчивается одиночным '\r'
    };
    const Variant variants[] = {
        {"LF", 0, false, false, false},
        {"CRLF", 1, false, false, false},
        {"mixed", 2, false, false, false},
        {"lone CR", 3, false, false, false},
        {"partial tail", 2, false, true, false},
        {"CR tail", 1, false, true, true},
        {"BOM", 2, true, true, false},
    };
    int checked = 0;
    for (const Variant& v : variants) {
        for (int rep = 0; rep < 4; ++rep) {
            QByteArray data;
            if (v.bom)
                data += "\xEF\xBB\xBF";
            const int lines = 120;
            int high = 0;
            for (int i = 0; i < lines; ++i) {
                QByteArray line = "2026-11-18 12:00:00.000 INFO line " + QByteArray::number(i);
                if (v.eolKind == 3 && i % 7 == 3)
                    line += "\rinner";
                const bool last = i == lines - 1;
                // Последняя строка — не-ASCII: начало оборванной строки (в том
                // числе с '\r' в конце файла) тоже идёт в advance.
                if ((i % 3 == 1 || last) && high < 200) {
                    const QByteArray& w = words[std::uniform_int_distribution<int>(0, 2)(rng)];
                    line += ' ' + w + " tail";
                    high += int(w.size());
                }
                if (last && v.partialTail) {
                    data += line;
                    if (v.crTail)
                        data += '\r';
                    break;
                }
                const int kind = v.eolKind == 2 ? std::uniform_int_distribution<int>(0, 1)(rng)
                                                : (v.eolKind == 1 ? 1 : 0);
                data += line + (kind ? "\r\n" : "\n");
            }
            const QString path = dir.path() + QStringLiteral("/p_%1_%2.log").arg(checked).arg(rep);

            // Полный разбор.
            {
                QFile out(path);
                out.open(QIODevice::WriteOnly);
                out.write(data);
            }
            const ParserRun run = runParser(path, FileEncoding{});
            int refChanges = 0;
            FileEncoding refState;
            if (v.bom)
                refState = FileEncoding{TextDecoder(TextEncoding::Utf8), FileEncoding::Source::Bom};
            refState = referenceAdvance(path, data, refState, v.bom ? 3 : 0, &refChanges);
            CHECK(run.finished, QString::fromLatin1(v.name) + ": parser finished");
            CHECK(sameEncodingState(run.finalState, refState),
                  QString::fromLatin1(v.name) + QStringLiteral(": evidence of the whole parse = reference (end %1 vs %2, bytes %3 vs %4)")
                      .arg(run.finalState.evidence.end).arg(refState.evidence.end)
                      .arg(run.finalState.evidence.bytes.size()).arg(refState.evidence.bytes.size()));
            if (!v.bom)
                CHECK(refState.provisional, QString::fromLatin1(v.name) + ": decision stays provisional (test premise)");
            const QVector<RefLine> ref = referenceLines(data, v.bom ? 3 : 0);
            CHECK(run.texts.size() == ref.size(),
                  QString::fromLatin1(v.name) + QStringLiteral(": line count %1 vs %2").arg(run.texts.size()).arg(ref.size()));
            CHECK(run.endOffset == data.size(),
                  QString::fromLatin1(v.name) + QStringLiteral(": tailState end %1 vs %2").arg(run.endOffset).arg(data.size()));
            const bool refPartial = !ref.isEmpty() && !ref.constLast().newline;
            CHECK(run.lastLinePartial == refPartial
                      && (!refPartial || run.lastLineStart == ref.constLast().start),
                  QString::fromLatin1(v.name) + QStringLiteral(": tailState partial %1 start %2")
                      .arg(run.lastLinePartial).arg(run.lastLineStart));

            // Дозапись частями: разрезы в случайных местах (посреди строки,
            // между '\r' и '\n'), каждая порция — с tailState прошлой, как у
            // вкладки; свидетельство обязано совпасть с эталоном на каждом шаге.
            if (v.bom)
                continue; // BOM решает окончательно — автоматике нечего копить
            QVector<qint64> cuts;
            for (int k = 0; k < 5; ++k)
                cuts << std::uniform_int_distribution<qint64>(1, data.size() - 1)(rng);
            std::sort(cuts.begin(), cuts.end());
            cuts << data.size();
            const QString growPath = path + QStringLiteral(".grow");
            QFile grow(growPath);
            grow.open(QIODevice::WriteOnly);
            FileEncoding state;
            FileEncoding refGrow;
            qint64 written = 0;
            qint64 resume = 0;
            for (const qint64 cut : cuts) {
                grow.write(data.mid(written, cut - written));
                grow.flush();
                written = cut;
                const QByteArray now = data.left(written);
                const ParserRun part = runParser(growPath, state, resume);
                int changes = 0;
                refGrow = referenceAdvance(growPath, now, refGrow, resume, &changes);
                CHECK(sameEncodingState(part.finalState, refGrow),
                      QString::fromLatin1(v.name) + QStringLiteral(": append up to %1 from %2 — evidence = reference (end %3 vs %4)")
                          .arg(written).arg(resume).arg(part.finalState.evidence.end).arg(refGrow.evidence.end));
                // Следующая порция — с начала оборванной строки или с конца.
                const QVector<RefLine> nowRef = referenceLines(now, resume);
                const bool partial = !nowRef.isEmpty() && !nowRef.constLast().newline;
                const qint64 refResume = partial ? nowRef.constLast().start : written;
                const qint64 gotResume = part.lastLinePartial ? part.lastLineStart : part.endOffset;
                CHECK(gotResume == refResume,
                      QString::fromLatin1(v.name) + QStringLiteral(": resume offset %1 vs %2").arg(gotResume).arg(refResume));
                state = part.finalState;
                resume = refResume;
            }
            ++checked;
        }
    }
    std::printf("parser line positions: %d files, whole and appended in parts\n", checked);
}

// Цена разбора под предварительным решением (Q-18): файл, где до конца
// набирается меньше kFinalHighBytes байтов ≥ 0x80 на ~200 строках, против
// того же файла без них. Строки по 998 байтов: конец строки не совпадает с
// границей 16-КБ блока QTextStream ближе чем через 8 МБ — прежний транспорт
// перечитывал бы буфер по байту на каждом вызове pos() (~0,2 с на вызов,
// десятки секунд на файл). Сравнение — с базовым прогоном (не дольше
// 4 × базового + 2 с), а не в абсолютных секундах.
static void testParserProvisionalCost()
{
    QTemporaryDir dir;
    QByteArray ascii;
    const int lines = 3000;
    ascii.reserve(lines * 998);
    for (int i = 0; i < lines; ++i) {
        QByteArray line = "2026-11-18 12:00:00.000 INFO line " + QByteArray::number(i) + ' ';
        line += QByteArray(997 - line.size(), 'x');
        ascii += line + '\n';
    }
    QByteArray sparse = ascii;
    const int highLines = 200;
    for (int i = 0; i < highLines; ++i)
        sparse[qsizetype(i * (lines / highLines) + 7) * 998 + 500] = char(0xE0);
    const QString asciiPath = dir.path() + QStringLiteral("/ascii.log");
    const QString sparsePath = dir.path() + QStringLiteral("/sparse.log");
    for (const auto& [path, bytes] : {std::pair{asciiPath, ascii}, std::pair{sparsePath, sparse}}) {
        QFile out(path);
        out.open(QIODevice::WriteOnly);
        out.write(bytes);
    }
    const auto timed = [](const QString& path, ParserRun* last) {
        qint64 best = -1;
        for (int i = 0; i < 2; ++i) {
            QElapsedTimer t;
            t.start();
            *last = runParser(path, FileEncoding{});
            const qint64 ms = t.elapsed();
            best = best < 0 ? ms : qMin(best, ms);
        }
        return best;
    };
    ParserRun asciiRun, sparseRun;
    const qint64 asciiMs = timed(asciiPath, &asciiRun);
    const qint64 sparseMs = timed(sparsePath, &sparseRun);
    CHECK(sparseRun.finished && sparseRun.texts.size() == lines, "cost: sparse file parsed");
    CHECK(sparseRun.finalState.source == FileEncoding::Source::Auto && sparseRun.finalState.provisional
              && sparseRun.finalState.evidence.highBytes == highLines,
          QStringLiteral("cost: decision provisional to the end with %1 high bytes (test premise)")
              .arg(sparseRun.finalState.evidence.highBytes));
    std::printf("parser cost: ascii %lld ms, provisional sparse %lld ms\n",
                (long long)asciiMs, (long long)sparseMs);
    // Запас — на шум CI и санитайзеры; прежний транспорт был дольше в тысячи раз.
    CHECK(sparseMs <= 4 * asciiMs + 2000,
          QStringLiteral("cost: provisional sparse parse %1 ms vs plain %2 ms").arg(sparseMs).arg(asciiMs));
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
    testEvidenceCompression();
    testProvisionalScenarios();
    testProvisionalFuzz();
    testClassifierIsAsciiOnly();
    testTextStreamLatin1Transport();
    testByteLineReader();
    testParserLinePositions();
    testParserProvisionalCost();
    if (failures) {
        std::fprintf(stderr, "textencoding: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("textencoding: OK\n");
    return 0;
}
