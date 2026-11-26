// ansi_escape — снятие ANSI-последовательностей и стиль SGR (F-3,
// docs/design/F-3.md): таблица снятия (байты и UTF-16 — одна грамматика),
// TextDecoder снимает до декодирования (ESC внутри многобайтного UTF-8, cp1251),
// таблица спанов SGR против заданных, фаззинг инвариантов, раскраска строки
// (SyntaxHighlighter::withAnsi: ANSI главнее токенов, промежутки — токенам) и
// палитра AppTheme::ansiColor (читаемость на фоне обеих тем).

#include "ansiescape.h"
#include "apptheme.h"
#include "recordid.h"
#include "syntaxhighlighter.h"
#include "textencoding.h"

#include <QApplication>
#include <QPalette>

#include <cmath>
#include <cstdio>
#include <random>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static QString shown(const QByteArray& bytes)
{
    QString out;
    for (const char c : bytes) {
        const unsigned u = unsigned(static_cast<unsigned char>(c));
        if (u < 0x20 || u >= 0x7F)
            out += QStringLiteral("\\x%1").arg(u, 2, 16, QLatin1Char('0'));
        else
            out += QChar(u);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Снятие
// ---------------------------------------------------------------------------

struct StripCase {
    const char* input;
    const char* expected;
};

static void testStripTable()
{
    const StripCase cases[] = {
        {"plain text", "plain text"},
        {"", ""},
        {"\x1b[31mERROR\x1b[0m foo", "ERROR foo"},
        {"\x1b[1;38;5;208mX\x1b[m", "X"},
        {"\x1b[38;2;10;20;30mX\x1b[0m", "X"},
        {"\x1b[38:2::10:20:30mX", "X"},
        {"a\x1b[2Kb", "ab"},                         // CSI стирания
        {"a\x1b[?25lb", "ab"},                       // частный CSI
        {"a\x1b[1 qb", "ab"},                        // промежуточный байт
        {"a\x1b]0;title\x07" "b", "ab"},             // OSC + BEL
        {"a\x1b]8;;http://x/\x1b\\link\x1b]8;;\x1b\\b", "alinkb"}, // OSC 8 + ST
        {"a\x1bPq#0;1\x1b\\b", "ab"},                // DCS
        {"a\x1b_apc\x07" "b", "ab"},                 // APC
        {"a\x1b(Bb\x1b[m", "ab"},                    // nF (tput sgr0)
        {"a\x1b" "7b\x1b" "8", "ab"},                // Fp
        {"a\x1b" "cb", "ab"},                        // Fs
        {"a\x1b" "Db", "ab"},                        // Fe
        {"a\x1b[31", "a"},                           // оборвана концом строки
        {"a\x1b[", "a"},
        {"a\x1b[31\x01" "b", "a\x01" "b"},           // посторонний символ — текст
        {"a\x1b[31\xd0\x9f", "a\xd0\x9f"},           // байт ≥ 0x80 внутри CSI — текст
        {"a\x1b]0;title", "a"},                      // OSC без конца
        {"a\x1b]0;ti\x1b[1mtle", "atle"},            // ESC внутри OSC — обрыв, дальше CSI
        {"a\x1b", "a"},                              // одиночный ESC в конце
        {"\x1b", ""},
        {"a\x1b\x1b[1mb", "ab"},
        {"a\x1b\x01" "b", "a\x01" "b"},              // одиночный ESC перед управляющим
        {"a\x1b\xd0\x9f", "a\xd0\x9f"},              // одиночный ESC перед UTF-8
        {"a\x1b[31\nb", "a\nb"},                     // не переходит через '\n'
        {"a\x1b]0;t\nb", "a\nb"},
        {"a\x1b(\nb", "a\nb"},
        {"a\x1b\nb", "a\nb"},
        {"\x1b[32m2026-11-26 10:00:00\x1b[0m \x1b[31mERROR\x1b[0m db down",
         "2026-11-26 10:00:00 ERROR db down"},
    };
    for (const StripCase& c : cases) {
        const QByteArray input(c.input);
        const QByteArray expected(c.expected);
        const QByteArray got = AnsiEscape::strip(input);
        CHECK(got == expected, QStringLiteral("strip(%1) = %2, ждали %3")
                                   .arg(shown(input), shown(got), shown(expected)));
        // На месте (in == out).
        QByteArray inPlace = input;
        inPlace.resize(AnsiEscape::stripBytes(inPlace.data(), inPlace.size(), inPlace.data()));
        CHECK(inPlace == expected, QStringLiteral("stripBytes на месте: %1").arg(shown(input)));
        // UTF-16: та же грамматика (байты ≥ 0x80 — как Latin-1-символы).
        QString text = QString::fromLatin1(input);
        AnsiEscape::stripInPlace(text);
        CHECK(text == QString::fromLatin1(expected),
              QStringLiteral("stripInPlace(%1)").arg(shown(input)));
        // Блок из нескольких строк = построчно (сканер каталога).
        const QByteArray block = input + "\n" + input + "\n";
        CHECK(AnsiEscape::strip(block) == expected + "\n" + expected + "\n",
              QStringLiteral("блок: %1").arg(shown(input)));
    }
    // Без ESC — та же строка (тот же буфер у QByteArray-копии).
    const QByteArray plain("no escapes here");
    CHECK(!AnsiEscape::hasEscape(plain), "hasEscape на обычной строке");
    CHECK(AnsiEscape::hasEscape(QByteArrayView("x\x1b")), "hasEscape у ESC в конце");
}

// Где резать длинную строку на куски без разрыва последовательности.
static void testCompleteEnd()
{
    const struct { const char* data; int expected; } cases[] = {
        {"plain", 5},
        {"ab\x1b[31mcd", 9},
        {"ab\x1b[31", 2},
        {"ab\x1b[", 2},
        {"ab\x1b", 2},
        {"ab\x1b]0;title", 2},
        {"ab\x1b]0;title\x07", 12},
        {"ab\x1b]0;t\x1b", 7},        // ESC обрывает OSC и начинает своё
        {"ab\x1b]0;t\x1b\\", 9},
        {"ab\x1b(", 2},
        {"ab\x1b(B", 5},
        {"ab\x1b" "7", 4},
        {"ab\x1b[31\x01", 7},  // оборвана посторонним символом — закончена
    };
    for (const auto& c : cases) {
        const QByteArray data(c.data);
        CHECK(AnsiEscape::completeEnd(QByteArrayView(data)) == c.expected,
              QStringLiteral("completeEnd(%1) = %2, ждали %3")
                  .arg(shown(data)).arg(AnsiEscape::completeEnd(QByteArrayView(data))).arg(c.expected));
        CHECK(AnsiEscape::completeEnd(QStringView(QString::fromLatin1(data))) == c.expected,
              QStringLiteral("completeEnd UTF-16 %1").arg(shown(data)));
        // Кусок до completeEnd + остаток = снятие целого.
        const qsizetype cut = AnsiEscape::completeEnd(QByteArrayView(data));
        CHECK(AnsiEscape::strip(data.left(cut)) + AnsiEscape::strip(data.mid(cut)) == AnsiEscape::strip(data),
              QStringLiteral("разрез по completeEnd: %1").arg(shown(data)));
    }
}

static void testDecoder()
{
    const TextDecoder utf8;
    CHECK(utf8.decode(QByteArrayView("\x1b[31mERROR\x1b[0m foo")) == QStringLiteral("ERROR foo"),
          "UTF-8: коды сняты");
    // ESC внутри многобайтного символа: снимается из байтов до декодирования.
    CHECK(utf8.decode(QByteArrayView("\xd0\x1b[31m\x9f")) == QStringLiteral("П"),
          "UTF-8: ESC внутри символа не рвёт его");
    CHECK(utf8.decode(QByteArrayView("\x1b[1m\xd0\x9f\xd1\x80\xd0\xb8\x1b[0m")) == QStringLiteral("При"),
          "UTF-8: кириллица в цвете");
    const TextDecoder cp1251(TextEncoding::Windows1251);
    CHECK(cp1251.decode(QByteArrayView("\x1b[31m\xcf\xf0\xe8\x1b[0m")) == QStringLiteral("При"),
          "cp1251: коды сняты");
    CHECK(cp1251.decode(QByteArrayView("\xcf\x1b[0m\xf0")) == QStringLiteral("Пр"),
          "cp1251: ESC между буквами");
    // Строка без ESC — ровно прежний декодер.
    CHECK(utf8.decode(QByteArrayView("\xd0\x9f ok")) == QString::fromUtf8("\xd0\x9f ok"),
          "UTF-8 без кодов — fromUtf8");

    // Отпечаток строки (R-1): коды не входят ни в байты, ни в текст.
    const QByteArray raw("\x1b[32m2026-11-26\x1b[0m \x1b[31mERROR\x1b[0m \xd0\x9f");
    const QString text = utf8.decode(raw);
    CHECK(RecordFingerprint::ofLineBytes(raw) == RecordFingerprint::ofLineText(text),
          "отпечаток: байты с кодами = текст без кодов");
    CHECK(RecordFingerprint::ofLineBytes(raw) == RecordFingerprint::ofLineBytes(AnsiEscape::strip(raw)),
          "отпечаток: байты с кодами = байты без кодов");
    QString withCodes = QString::fromUtf8(raw);
    CHECK(RecordFingerprint::ofLineText(withCodes) == RecordFingerprint::ofLineText(text),
          "отпечаток: текст с кодами = текст без кодов");
}

// ---------------------------------------------------------------------------
// Стиль SGR
// ---------------------------------------------------------------------------

static AnsiSpan span(int start, int length, quint32 fg, quint32 bg = 0, quint8 attrs = 0)
{
    AnsiSpan s;
    s.start = start;
    s.length = length;
    s.fg = fg;
    s.bg = bg;
    s.attrs = attrs;
    return s;
}

static QString describe(const AnsiStyles& styles)
{
    QString out;
    for (const AnsiSpan& s : styles)
        out += QStringLiteral("{%1,%2,fg=%3,bg=%4,a=%5}")
                   .arg(s.start).arg(s.length).arg(s.fg, 0, 16).arg(s.bg, 0, 16).arg(s.attrs);
    return out;
}

struct StyleCase {
    const char* input;
    AnsiStyles expected;
};

static void testStyles()
{
    using namespace AnsiColor;
    const StyleCase cases[] = {
        {"plain", {}},
        {"\x1b[31mERROR\x1b[0m foo", {span(0, 5, indexed(1))}},
        {"a\x1b[1;32mbc\x1b[22mde\x1b[0mf",
         {span(1, 2, indexed(2), 0, AnsiSpan::Bold), span(3, 2, indexed(2))}},
        {"\x1b[38;5;208mX", {span(0, 1, indexed(208))}},
        {"\x1b[38;2;10;20;30mX\x1b[48;2;1;2;3mY",
         {span(0, 1, rgb(10, 20, 30)), span(1, 1, rgb(10, 20, 30), rgb(1, 2, 3))}},
        {"\x1b[38:2::10:20:30mX", {span(0, 1, rgb(10, 20, 30))}},
        {"\x1b[38:2:10:20:30mX", {span(0, 1, rgb(10, 20, 30))}},
        {"\x1b[38:5:9mX", {span(0, 1, indexed(9))}},
        {"\x1b[91;104mX", {span(0, 1, indexed(9), indexed(12))}},
        {"\x1b[2;3;4;7mX\x1b[23;24;27mY",
         {span(0, 1, 0, 0, AnsiSpan::Dim | AnsiSpan::Italic | AnsiSpan::Underline | AnsiSpan::Inverse),
          span(1, 1, 0, 0, AnsiSpan::Dim)}},
        {"\x1b[31mA\x1b[31mB", {span(0, 2, indexed(1))}},        // одинаковый стиль слит
        {"\x1b[31mA\x1b[2KB", {span(0, 2, indexed(1))}},         // не-SGR стиль не сбрасывает
        {"\x1b[0mplain", {}},
        {"\x1b[31m", {}},                                         // нет текста
        {"\x1b[?1mX", {}},                                        // частный — не SGR
        {"\x1b[31mA\x1b[39mB\x1b[44mC\x1b[49mD",
         {span(0, 1, indexed(1)), span(2, 1, 0, indexed(4))}},
        {"\x1b[31mA\x1b[mB", {span(0, 1, indexed(1))}},          // ESC[m — сброс
        {"\x1b[31;mA", {}},                                       // пустой параметр — 0
        {"\x1b[38;5mX", {}},                                      // неполная форма
        {"x\x1b]0;t\x07\x1b[33my", {span(1, 1, indexed(3))}},
        // Подпараметры через ':' — у своего кода (ITU T.416), не отдельные коды.
        {"\x1b[1;31mA\x1b[4:0mB", {span(0, 2, indexed(1), 0, AnsiSpan::Bold)}},
        {"\x1b[4:3mA\x1b[4:0mB", {span(0, 1, 0, 0, AnsiSpan::Underline)}},
        {"\x1b[58;2;1;3;4mA", {}},                               // цвет подчёркивания
        {"\x1b[58:5:9;31mA", {span(0, 1, indexed(1))}},
        {"\x1b[31;58;5;200;1mA", {span(0, 1, indexed(1), 0, AnsiSpan::Bold)}},
    };
    const TextDecoder utf8;
    for (const StyleCase& c : cases) {
        const AnsiStyles got = AnsiEscape::styles(QByteArrayView(c.input), utf8);
        CHECK(got == c.expected, QStringLiteral("styles(%1) = %2, ждали %3")
                                     .arg(shown(QByteArray(c.input)), describe(got),
                                          describe(c.expected)));
        const AnsiStyles wide = AnsiEscape::styles(QString::fromLatin1(c.input));
        CHECK(wide == c.expected, QStringLiteral("styles(UTF-16 %1)").arg(shown(QByteArray(c.input))));
    }
    // Координаты — в символах текста без кодов, не в байтах.
    const AnsiStyles cyr = AnsiEscape::styles(
        QByteArrayView("\xd0\x9f\x1b[31m\xd1\x80\xd0\xb8\x1b[0m!"), utf8);
    CHECK(cyr == AnsiStyles{span(1, 2, AnsiColor::indexed(1))}, "UTF-8: позиции в символах");
    const AnsiStyles cp = AnsiEscape::styles(QByteArrayView("\xcf\x1b[31m\xf0\xe8\x1b[0m!"),
                                             TextDecoder(TextEncoding::Windows1251));
    CHECK(cp == AnsiStyles{span(1, 2, AnsiColor::indexed(1))}, "cp1251: позиции");
    const AnsiStyles emoji = AnsiEscape::styles(
        QByteArrayView("\xf0\x9f\x98\x80\x1b[32mok"), utf8);
    CHECK(emoji == AnsiStyles{span(2, 2, AnsiColor::indexed(2))}, "суррогатная пара — две единицы");
    // ESC внутри символа: спан не выходит за текст.
    const QByteArray torn("\xd0\x1b[31m\x9f");
    for (const AnsiSpan& s : AnsiEscape::styles(torn, utf8))
        CHECK(s.start + s.length <= utf8.decode(torn).size(), "разорванный символ: спан в тексте");
}

// Случайные строки из кусков текста и последовательностей: инварианты.
static void testFuzz()
{
    std::mt19937 rng(20261126);
    const char* pieces[] = {
        "ERROR", " ", "foo", "\x1b[31m", "\x1b[0m", "\x1b[1;38;5;", "208m", "\x1b]0;t", "\x07",
        "\x1b\\", "\x1b", "[", "m", "\x1b(", "B", "\xd0\x9f", "\xd0", "\x9f", ";", "2", "\t",
        "\x1b[38:2::", "1:2:3m", "\x1b[?25l", "\x01",
    };
    const TextDecoder utf8;
    for (int iter = 0; iter < 20000; ++iter) {
        QByteArray line;
        const int n = int(rng() % 12);
        for (int i = 0; i < n; ++i)
            line += pieces[rng() % std::size(pieces)];
        const QByteArray stripped = AnsiEscape::strip(line);
        CHECK(!AnsiEscape::hasEscape(stripped), QStringLiteral("ESC остался: %1").arg(shown(line)));
        CHECK(AnsiEscape::strip(stripped) == stripped, "снятие идемпотентно");
        // UTF-16 и байты — одно и то же (символы ≥ 0x80 как Latin-1).
        QString wide = QString::fromLatin1(line);
        AnsiEscape::stripInPlace(wide);
        CHECK(wide == QString::fromLatin1(stripped), QStringLiteral("UTF-16 = байты: %1").arg(shown(line)));
        // Декодер = декодирование снятых байтов.
        const QString text = utf8.decode(line);
        CHECK(text == QString::fromUtf8(stripped), "decode = fromUtf8(strip)");
        // Спаны отсортированы, не пересекаются и лежат в тексте.
        const AnsiStyles styles = AnsiEscape::styles(line, utf8);
        int end = 0;
        for (const AnsiSpan& s : styles) {
            CHECK(s.length > 0 && s.start >= end && s.start + s.length <= text.size(),
                  QStringLiteral("спаны: %1 → %2").arg(shown(line), describe(styles)));
            end = s.start + s.length;
        }
        CHECK(RecordFingerprint::ofLineBytes(line) == RecordFingerprint::ofLineText(text),
              QStringLiteral("отпечаток: %1").arg(shown(line)));
    }
}

// ---------------------------------------------------------------------------
// Раскраска и палитра
// ---------------------------------------------------------------------------

static QPalette lightPalette()
{
    QPalette p;
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::Text, Qt::black);
    return p;
}

static QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Base, QColor(30, 30, 30));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    return p;
}

static double contrast(const QColor& a, const QColor& b)
{
    const auto lum = [](const QColor& c) {
        const auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
        return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
    };
    const double x = lum(a), y = lum(b);
    return (qMax(x, y) + 0.05) / (qMin(x, y) + 0.05);
}

static void testWithAnsi()
{
    const QPalette light = lightPalette();
    // Токены syntax: число в [10, 12) и строка в [0, 3) — ANSI занимает [0, 5).
    QList<HighlightToken> syntax;
    syntax.append(HighlightToken{0, 3, QColor(Qt::green), QColor()});
    syntax.append(HighlightToken{4, 8, QColor(Qt::blue), QColor()});
    syntax.append(HighlightToken{10, 12, QColor(Qt::darkGreen), QColor()});
    const AnsiStyles ansi{span(0, 5, AnsiColor::indexed(1), 0, AnsiSpan::Bold)};
    const QList<HighlightToken> merged = SyntaxHighlighter::withAnsi(ansi, syntax, light);
    CHECK(merged.size() == 3, QStringLiteral("withAnsi: %1 токенов").arg(merged.size()));
    if (merged.size() == 3) {
        CHECK(merged[0].start == 0 && merged[0].end == 5
                  && merged[0].color == AppTheme::ansiColor(1, light)
                  && merged[0].fontAttrs == AnsiSpan::Bold,
              "withAnsi: участок ANSI — его цвет и жирный");
        CHECK(merged[1].start == 5 && merged[1].end == 8 && merged[1].color == QColor(Qt::blue),
              "withAnsi: токен обрезан по краю ANSI");
        CHECK(merged[2].start == 10 && merged[2].end == 12, "withAnsi: токен в промежутке");
    }
    // Только фон ANSI: текст палитры на нём не всегда читается — чёрный фон
    // в светлой теме даёт белый текст, светлый фон в тёмной — чёрный.
    {
        const QList<HighlightToken> onBlack =
            SyntaxHighlighter::withAnsi({span(0, 3, 0, AnsiColor::indexed(0))}, {}, light);
        CHECK(onBlack.size() == 1 && contrast(onBlack[0].color, onBlack[0].bgColor) >= 3.0,
              "light theme: text on ESC[40m is readable");
        const QPalette dark = darkPalette();
        for (const int bg : {7, 15}) {
            const QList<HighlightToken> t =
                SyntaxHighlighter::withAnsi({span(0, 3, 0, AnsiColor::indexed(bg))}, {}, dark);
            CHECK(t.size() == 1 && contrast(t[0].color, t[0].bgColor) >= 3.0,
                  QStringLiteral("dark theme: text on background %1 is readable").arg(bg));
        }
    }
    CHECK(SyntaxHighlighter::withAnsi({}, syntax, light).size() == syntax.size(),
          "withAnsi без ANSI — токены как есть");
    // inverse: цвет текста — фон, фон — цвет текста.
    const QList<HighlightToken> inv = SyntaxHighlighter::withAnsi(
        {span(0, 1, 0, 0, AnsiSpan::Inverse)}, {}, light);
    CHECK(inv.size() == 1 && inv[0].bgColor == light.color(QPalette::Text)
              && inv[0].color == light.color(QPalette::Base),
          "inverse без цветов — текст и фон палитры наоборот");

    // Палитра: 16 цветов читаемы как текст на фоне своей темы.
    for (const QPalette& palette : {light, darkPalette()}) {
        const QColor base = palette.color(QPalette::Base);
        for (int i = 0; i < 16; ++i) {
            const QColor c = AppTheme::ansiColor(i, palette);
            CHECK(contrast(c, base) >= 1.6,
                  QStringLiteral("ansiColor(%1) читаем на %2").arg(i).arg(base.name()));
            CHECK(AppTheme::ansiReadableText(c, base, palette) == c,
                  QStringLiteral("ansiColor(%1) не подменяется").arg(i));
        }
    }
    CHECK(AppTheme::ansiColor(16, light) == QColor(0, 0, 0), "кубик: 16 — чёрный");
    CHECK(AppTheme::ansiColor(231, light) == QColor(255, 255, 255), "кубик: 231 — белый");
    CHECK(AppTheme::ansiColor(196, light) == QColor(255, 0, 0), "кубик: 196 — красный");
    CHECK(AppTheme::ansiColor(232, light) == QColor(8, 8, 8), "серые: 232");
    CHECK(AppTheme::ansiColor(255, light) == QColor(238, 238, 238), "серые: 255");
    // Белый truecolor на белом фоне — цвет текста палитры.
    CHECK(AppTheme::ansiReadableText(QColor(250, 250, 250), Qt::white, light) == QColor(Qt::black),
          "нечитаемый цвет заменён");
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testStripTable();
    testDecoder();
    testCompleteEnd();
    testStyles();
    testFuzz();
    testWithAnsi();

    if (failures) {
        std::fprintf(stderr, "ansi_escape: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("ansi_escape: OK\n");
    return 0;
}
