#include "textencoding.h"

#include <QLocale>
#include <QSysInfo>

#include <cstring>

namespace {

// ----------------------------------------------------------------------------
// Таблицы байтов 0x80–0xFF → кодовые точки. Источник — таблицы стандартов
// (Windows-1251/1252 — кодовые страницы Microsoft, они же индексы WHATWG;
// IBM866 — кодовая страница DOS; KOI8-R — RFC 1489); байты, не определённые
// стандартом, — C1 с тем же номером. Тест tests/textencoding сверяет все 128 значений каждой
// таблицы с эталоном, записанным в тесте независимо.
// ----------------------------------------------------------------------------
constexpr char16_t kWindows1251[128] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,  // 0x80
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,  // 0x88
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 0x90
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,  // 0x98
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,  // 0xA0
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,  // 0xA8
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,  // 0xB0
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,  // 0xB8
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,  // 0xC0
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,  // 0xC8
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,  // 0xD0
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,  // 0xD8
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,  // 0xE0
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,  // 0xE8
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,  // 0xF0
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,  // 0xF8
};

constexpr char16_t kIbm866[128] = {
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,  // 0x80
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,  // 0x88
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,  // 0x90
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,  // 0x98
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,  // 0xA0
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,  // 0xA8
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,  // 0xB0
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,  // 0xB8
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,  // 0xC0
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,  // 0xC8
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,  // 0xD0
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,  // 0xD8
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,  // 0xE0
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,  // 0xE8
    0x0401, 0x0451, 0x0404, 0x0454, 0x0407, 0x0457, 0x040E, 0x045E,  // 0xF0
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x2116, 0x00A4, 0x25A0, 0x00A0,  // 0xF8
};

constexpr char16_t kKoi8R[128] = {
    0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524,  // 0x80
    0x252C, 0x2534, 0x253C, 0x2580, 0x2584, 0x2588, 0x258C, 0x2590,  // 0x88
    0x2591, 0x2592, 0x2593, 0x2320, 0x25A0, 0x2219, 0x221A, 0x2248,  // 0x90
    0x2264, 0x2265, 0x00A0, 0x2321, 0x00B0, 0x00B2, 0x00B7, 0x00F7,  // 0x98
    0x2550, 0x2551, 0x2552, 0x0451, 0x2553, 0x2554, 0x2555, 0x2556,  // 0xA0
    0x2557, 0x2558, 0x2559, 0x255A, 0x255B, 0x255C, 0x255D, 0x255E,  // 0xA8
    0x255F, 0x2560, 0x2561, 0x0401, 0x2562, 0x2563, 0x2564, 0x2565,  // 0xB0
    0x2566, 0x2567, 0x2568, 0x2569, 0x256A, 0x256B, 0x256C, 0x00A9,  // 0xB8
    0x044E, 0x0430, 0x0431, 0x0446, 0x0434, 0x0435, 0x0444, 0x0433,  // 0xC0
    0x0445, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E,  // 0xC8
    0x043F, 0x044F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0436, 0x0432,  // 0xD0
    0x044C, 0x044B, 0x0437, 0x0448, 0x044D, 0x0449, 0x0447, 0x044A,  // 0xD8
    0x042E, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,  // 0xE0
    0x0425, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E,  // 0xE8
    0x041F, 0x042F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,  // 0xF0
    0x042C, 0x042B, 0x0417, 0x0428, 0x042D, 0x0429, 0x0427, 0x042A,  // 0xF8
};

constexpr char16_t kWindows1252[128] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,  // 0x80
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,  // 0x88
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,  // 0x90
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,  // 0x98
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7,  // 0xA0
    0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,  // 0xA8
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7,  // 0xB0
    0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,  // 0xB8
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7,  // 0xC0
    0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,  // 0xC8
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7,  // 0xD0
    0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,  // 0xD8
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,  // 0xE0
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,  // 0xE8
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7,  // 0xF0
    0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,  // 0xF8
};

const char16_t* tableFor(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Windows1251: return kWindows1251;
    case TextEncoding::Ibm866:      return kIbm866;
    case TextEncoding::Koi8R:       return kKoi8R;
    case TextEncoding::Windows1252: return kWindows1252;
    default:                        return nullptr;
    }
}

} // namespace

// ============================================================================
// TextEncodings
// ============================================================================

namespace TextEncodings {

bool isSingleByte(TextEncoding encoding)
{
    return tableFor(encoding) != nullptr;
}

bool isCyrillic(TextEncoding encoding)
{
    return encoding == TextEncoding::Windows1251 || encoding == TextEncoding::Ibm866
        || encoding == TextEncoding::Koi8R;
}

bool isDecodable(TextEncoding encoding)
{
    return encoding == TextEncoding::Utf8 || isSingleByte(encoding);
}

QString displayName(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:        return QStringLiteral("UTF-8");
    case TextEncoding::Windows1251: return QStringLiteral("Windows-1251");
    case TextEncoding::Ibm866:      return QStringLiteral("IBM866");
    case TextEncoding::Koi8R:       return QStringLiteral("KOI8-R");
    case TextEncoding::Windows1252: return QStringLiteral("Windows-1252");
    case TextEncoding::Utf16LE:     return QStringLiteral("UTF-16LE");
    case TextEncoding::Utf16BE:     return QStringLiteral("UTF-16BE");
    case TextEncoding::Utf32LE:     return QStringLiteral("UTF-32LE");
    case TextEncoding::Utf32BE:     return QStringLiteral("UTF-32BE");
    }
    return QString();
}

std::optional<TextEncoding> fromName(QStringView name)
{
    // Сравнение без учёта регистра, дефисов и подчёркиваний: «utf8», «UTF-8»,
    // «windows_1251», «CP1251» — одно и то же.
    QString key;
    key.reserve(name.size());
    for (const QChar ch : name) {
        if (ch == u'-' || ch == u'_' || ch == u' ')
            continue;
        key.append(ch.toLower());
    }
    static const struct { const char* key; TextEncoding encoding; } kNames[] = {
        {"utf8", TextEncoding::Utf8},
        {"windows1251", TextEncoding::Windows1251}, {"cp1251", TextEncoding::Windows1251},
        {"win1251", TextEncoding::Windows1251},
        {"ibm866", TextEncoding::Ibm866}, {"cp866", TextEncoding::Ibm866},
        {"dos866", TextEncoding::Ibm866},
        {"koi8r", TextEncoding::Koi8R}, {"koi8", TextEncoding::Koi8R},
        {"windows1252", TextEncoding::Windows1252}, {"cp1252", TextEncoding::Windows1252},
        {"win1252", TextEncoding::Windows1252},
    };
    for (const auto& entry : kNames) {
        if (key == QLatin1String(entry.key))
            return entry.encoding;
    }
    return std::nullopt;
}

std::optional<TextEncoding> fromQt(QStringConverter::Encoding encoding)
{
    const bool littleEndian = QSysInfo::ByteOrder == QSysInfo::LittleEndian;
    switch (encoding) {
    case QStringConverter::Utf8:    return TextEncoding::Utf8;
    case QStringConverter::Utf16:   return littleEndian ? TextEncoding::Utf16LE : TextEncoding::Utf16BE;
    case QStringConverter::Utf16LE: return TextEncoding::Utf16LE;
    case QStringConverter::Utf16BE: return TextEncoding::Utf16BE;
    case QStringConverter::Utf32:   return littleEndian ? TextEncoding::Utf32LE : TextEncoding::Utf32BE;
    case QStringConverter::Utf32LE: return TextEncoding::Utf32LE;
    case QStringConverter::Utf32BE: return TextEncoding::Utf32BE;
    default:                        return std::nullopt;
    }
}

std::optional<QStringConverter::Encoding> toQt(TextEncoding encoding)
{
    switch (encoding) {
    case TextEncoding::Utf8:    return QStringConverter::Utf8;
    case TextEncoding::Utf16LE: return QStringConverter::Utf16LE;
    case TextEncoding::Utf16BE: return QStringConverter::Utf16BE;
    case TextEncoding::Utf32LE: return QStringConverter::Utf32LE;
    case TextEncoding::Utf32BE: return QStringConverter::Utf32BE;
    default:                    return std::nullopt;
    }
}

std::optional<Bom> detectBom(QByteArrayView head)
{
    const auto at = [&](qsizetype i) { return quint8(head[i]); };
    const qsizetype n = head.size();
    if (n >= 3 && at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF)
        return Bom{TextEncoding::Utf8, 3};
    if (n >= 4 && at(0) == 0xFF && at(1) == 0xFE && at(2) == 0x00 && at(3) == 0x00)
        return Bom{TextEncoding::Utf32LE, 4};
    if (n >= 4 && at(0) == 0x00 && at(1) == 0x00 && at(2) == 0xFE && at(3) == 0xFF)
        return Bom{TextEncoding::Utf32BE, 4};
    if (n >= 2 && at(0) == 0xFF && at(1) == 0xFE)
        return Bom{TextEncoding::Utf16LE, 2};
    if (n >= 2 && at(0) == 0xFE && at(1) == 0xFF)
        return Bom{TextEncoding::Utf16BE, 2};
    return std::nullopt;
}

TextEncoding systemFallback()
{
    return QLocale::system().script() == QLocale::CyrillicScript ? TextEncoding::Windows1251
                                                                  : TextEncoding::Windows1252;
}

} // namespace TextEncodings

// ============================================================================
// TextDecoder
// ============================================================================

TextDecoder::TextDecoder(TextEncoding encoding)
    : m_table(tableFor(encoding))
    , m_encoding(m_table ? encoding : TextEncoding::Utf8)
{
    Q_ASSERT(TextEncodings::isDecodable(encoding));
}

QString TextDecoder::decodeSingleByte(const char* data, qsizetype len) const
{
    // fromLatin1 — векторизованный перенос байтов в UTF-16; по таблице
    // переписываются только символы с первого байта ≥ 0x80. Строка из одних
    // ASCII-байтов второго прохода не получает.
    QString text = QString::fromLatin1(data, len);
    const qsizetype first = EncodingDetector::firstHighByte(data, len);
    if (first >= 0) {
        char16_t* d = reinterpret_cast<char16_t*>(text.data());
        for (qsizetype i = first; i < len; ++i) {
            const char16_t c = d[i];
            if (c >= 0x80)
                d[i] = m_table[c - 0x80];
        }
    }
    return text;
}

void TextDecoder::remapLatin1(QString& text) const
{
    if (!m_table)
        return;
    const qsizetype n = text.size();
    const char16_t* src = reinterpret_cast<const char16_t*>(text.constData());
    qsizetype i = 0;
    while (i < n && src[i] < 0x80)
        ++i;
    if (i == n)
        return;   // одни ASCII — без detach
    char16_t* d = reinterpret_cast<char16_t*>(text.data());
    for (; i < n; ++i) {
        const char16_t c = d[i];
        if (c >= 0x80 && c <= 0xFF)
            d[i] = m_table[c - 0x80];
    }
}

// ============================================================================
// EncodingDetector
// ============================================================================

namespace EncodingDetector {

qsizetype firstHighByte(const char* data, qsizetype len)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    qsizetype i = 0;
    // По 8 байт: старший бит любого из них — в маске.
    constexpr quint64 kHigh = 0x8080808080808080ULL;
    for (; i + 8 <= len; i += 8) {
        quint64 word;
        std::memcpy(&word, p + i, sizeof(word));
        if (word & kHigh)
            break;
    }
    for (; i < len; ++i) {
        if (p[i] >= 0x80)
            return i;
    }
    return -1;
}

Utf8Counts countUtf8(QByteArrayView window)
{
    Utf8Counts counts;
    const auto* p = reinterpret_cast<const unsigned char*>(window.data());
    const qsizetype n = window.size();
    const auto isCont = [](unsigned char b) { return (b & 0xC0) == 0x80; };
    qsizetype i = 0;
    while (i < n) {
        const unsigned char b = p[i];
        if (b < 0x80) {
            ++i;
            continue;
        }
        // Длина и допустимый диапазон второго байта (RFC 3629, таблица 3-7
        // стандарта Unicode): без overlong (C0, C1, E0 80–9F, F0 80–8F),
        // суррогатов (ED A0–BF) и кодов выше U+10FFFF (F4 90+, F5–FF).
        int length = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            length = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
            length = 3;
            if (b == 0xE0) lo = 0xA0;
            if (b == 0xED) hi = 0x9F;
        } else if (b >= 0xF0 && b <= 0xF4) {
            length = 4;
            if (b == 0xF0) lo = 0x90;
            if (b == 0xF4) hi = 0x8F;
        }
        if (length == 0) {
            ++counts.invalidBytes;   // продолжение без начала, C0/C1, F5–FF
            ++i;
            continue;
        }
        // Сколько байтов последовательности корректны подряд.
        int good = 1;
        if (i + 1 < n && p[i + 1] >= lo && p[i + 1] <= hi) {
            good = 2;
            while (good < length && i + good < n && isCont(p[i + good]))
                ++good;
        }
        if (good == length) {
            ++counts.validUtf8;
            i += length;
        } else if (i + good == n) {
            break;   // оборвана краем окна: не считается ни так, ни так
        } else {
            // Битая: невалиден начальный байт; хвост пересматривается сам
            // (продолжения дальше посчитаются невалидными по одному).
            ++counts.invalidBytes;
            ++i;
        }
    }
    return counts;
}

namespace {

// ln(доля буквы в русском тексте) × 16, порядок а…я (без ё); ё — отдельно.
// Частоты — общеизвестная таблица по Национальному корпусу русского языка.
constexpr int kLetterScore[32] = {
    -40, -66, -49, -65, -56, -40, -75, -66, -42, -71, -54, -50, -55, -43, -35, -57,
    -49, -46, -44, -58, -95, -74, -85, -68, -79, -90, -125, -63, -65, -92, -81, -63,
};
constexpr int kYoScore = -125;
// Символ, который в русском тексте почти не встречается (псевдографика,
// буквы других кириллических алфавитов, редкие знаки): как буква с долей
// ~0,1%.
constexpr int kForeignScore = -110;
// Типографские знаки, обычные в русском тексте однобайтной кодировки
// (кавычки-ёлочки, тире, №, многоточие, кавычки-лапки).
constexpr int kPunctScore = -30;
// Регистр — по слову целиком (ln доли × 16 относительно строчного): строчное
// — 0, с заглавной — ln(0,2/0,7), ВСЕ ЗАГЛАВНЫЕ — ln(0,1/0,7), смешанное
// («пРИМЕР», «ПримеР») почти не встречается. Соседние кодировки
// (Windows-1251 и KOI8-R) переворачивают регистр, поэтому строчный текст,
// прочитанный не в своей, проигрывает и при похожих частотах букв.
constexpr int kTitleScore = -20;
constexpr int kUpperScore = -31;
constexpr int kMixedCaseScore = -110;

bool isRussianPunct(char16_t c)
{
    switch (c) {
    case 0x00AB: case 0x00BB: case 0x2013: case 0x2014:
    case 0x2116: case 0x2026: case 0x201C: case 0x201D: case 0x201E:
        return true;
    default:
        return false;
    }
}

// Счёт «насколько байты окна, прочитанные по таблице, похожи на русский
// текст»: сумма ln частот букв (без учёта регистра), штраф за не-буквы и за
// регистр слова. Больше — правдоподобнее.
long long russianScore(QByteArrayView window, const char16_t* table)
{
    long long score = 0;
    // Текущее слово из кириллических букв: регистр первой и остальных.
    int letters = 0;
    bool firstUpper = false, restLower = false, restUpper = false;
    const auto endWord = [&] {
        if (letters == 0)
            return;
        if (restLower && restUpper)
            score += kMixedCaseScore;
        else if (firstUpper && restUpper)
            score += kUpperScore;
        else if (firstUpper)
            score += kTitleScore;                    // в том числе одна заглавная
        else if (restUpper)
            score += kMixedCaseScore;                // «пРИМЕР»
        letters = 0;
        firstUpper = restLower = restUpper = false;
    };
    for (const char ch : window) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x80) {
            endWord();
            continue;
        }
        const char16_t c = table[b - 0x80];
        int letterScore = 0;
        bool upper = false;
        if (c >= 0x0410 && c <= 0x042F) {          // А–Я
            letterScore = kLetterScore[c - 0x0410];
            upper = true;
        } else if (c >= 0x0430 && c <= 0x044F) {   // а–я
            letterScore = kLetterScore[c - 0x0430];
        } else if (c == 0x0401 || c == 0x0451) {   // Ё ё
            letterScore = kYoScore;
            upper = c == 0x0401;
        } else {
            endWord();
            score += isRussianPunct(c) ? kPunctScore : kForeignScore;
            continue;
        }
        score += letterScore;
        if (letters == 0)
            firstUpper = upper;
        else if (upper)
            restUpper = true;
        else
            restLower = true;
        ++letters;
    }
    endWord();
    return score;
}

} // namespace

TextEncoding guessCyrillic(QByteArrayView window)
{
    // Порядок — приоритет при равенстве: Windows-1251 — самая частая.
    static constexpr TextEncoding kCandidates[] = {
        TextEncoding::Windows1251, TextEncoding::Ibm866, TextEncoding::Koi8R,
    };
    TextEncoding best = kCandidates[0];
    long long bestScore = 0;
    bool first = true;
    for (const TextEncoding candidate : kCandidates) {
        const long long score = russianScore(window, tableFor(candidate));
        if (first || score > bestScore) {
            best = candidate;
            bestScore = score;
            first = false;
        }
    }
    return best;
}

std::optional<TextEncoding> decide(QByteArrayView window, TextEncoding fallback)
{
    const Utf8Counts counts = countUtf8(window);
    if (counts.validUtf8 == 0 && counts.invalidBytes == 0)
        return std::nullopt;
    if (counts.validUtf8 > counts.invalidBytes)
        return TextEncoding::Utf8;
    if (TextEncodings::isCyrillic(fallback))
        return guessCyrillic(window);
    if (!TextEncodings::isSingleByte(fallback))
        return TextEncoding::Windows1252;
    return fallback;
}

} // namespace EncodingDetector
