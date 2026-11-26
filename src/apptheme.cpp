#include "apptheme.h"

#include <cmath>
#include "cardframe.h"
#include <QSettings>
#include <iterator>

// ---------------------------------------------------------------------------
// Helper: read a color stored as "#aarrggbb" (hex-argb) with a fallback.
// ---------------------------------------------------------------------------
static QColor readColor(QSettings& s, const QString& key, const QColor& fallback)
{
    const QString raw = s.value(key, fallback.name(QColor::HexArgb)).toString();
    const QColor c(raw);
    return c.isValid() ? c : fallback;
}

// ---------------------------------------------------------------------------
void AppTheme::load(QSettings& s)
{
    s.beginGroup(QStringLiteral("Theme"));

    // Log level colors
    logFatal  = readColor(s, "logFatal",  logFatal);
    logError  = readColor(s, "logError",  logError);
    logWarn   = readColor(s, "logWarn",   logWarn);
    logInfo   = readColor(s, "logInfo",   logInfo);
    logDebug  = readColor(s, "logDebug",  logDebug);
    logTrace  = readColor(s, "logTrace",  logTrace);

    logDimFatal = readColor(s, "logDimFatal", logDimFatal);
    logDimError = readColor(s, "logDimError", logDimError);
    logDimWarn  = readColor(s, "logDimWarn",  logDimWarn);
    logDimInfo  = readColor(s, "logDimInfo",  logDimInfo);
    logDimDebug = readColor(s, "logDimDebug", logDimDebug);
    logDimTrace = readColor(s, "logDimTrace", logDimTrace);

    // Selection colors
    selectionFill  = readColor(s, "selectionFill",  selectionFill);
    selectionRowBg = readColor(s, "selectionRowBg", selectionRowBg);

    // Syntax highlight colors
    syntaxString = readColor(s, "syntaxString", syntaxString);
    syntaxNumber = readColor(s, "syntaxNumber", syntaxNumber);
    syntaxUrl    = readColor(s, "syntaxUrl",    syntaxUrl);
    syntaxHex    = readColor(s, "syntaxHex",    syntaxHex);
    syntaxPath   = readColor(s, "syntaxPath",   syntaxPath);
    syntaxGuid   = readColor(s, "syntaxGuid",   syntaxGuid);
    syntaxTime   = readColor(s, "syntaxTime",   syntaxTime);

    // UI element colors
    gutterMarker  = readColor(s, "gutterMarker",   gutterMarker);
    gutterNewEntry= readColor(s, "gutterNewEntry",  gutterNewEntry);
    gutterBookmark= readColor(s, "gutterBookmark",  gutterBookmark);
    badgeBg       = readColor(s, "badgeBg",         badgeBg);
    badgeFg       = readColor(s, "badgeFg",         badgeFg);
    bracketMatch  = readColor(s, "bracketMatch",    bracketMatch);
    searchMatch   = readColor(s, "searchMatch",     searchMatch);

    s.endGroup();
}

// ---------------------------------------------------------------------------
void AppTheme::save(QSettings& s) const
{
    s.beginGroup(QStringLiteral("Theme"));

    s.setValue("logFatal",  logFatal.name(QColor::HexArgb));
    s.setValue("logError",  logError.name(QColor::HexArgb));
    s.setValue("logWarn",   logWarn.name(QColor::HexArgb));
    s.setValue("logInfo",   logInfo.name(QColor::HexArgb));
    s.setValue("logDebug",  logDebug.name(QColor::HexArgb));
    s.setValue("logTrace",  logTrace.name(QColor::HexArgb));

    s.setValue("logDimFatal", logDimFatal.name(QColor::HexArgb));
    s.setValue("logDimError", logDimError.name(QColor::HexArgb));
    s.setValue("logDimWarn",  logDimWarn.name(QColor::HexArgb));
    s.setValue("logDimInfo",  logDimInfo.name(QColor::HexArgb));
    s.setValue("logDimDebug", logDimDebug.name(QColor::HexArgb));
    s.setValue("logDimTrace", logDimTrace.name(QColor::HexArgb));

    s.setValue("selectionFill",  selectionFill.name(QColor::HexArgb));
    s.setValue("selectionRowBg", selectionRowBg.name(QColor::HexArgb));

    s.setValue("syntaxString", syntaxString.name(QColor::HexArgb));
    s.setValue("syntaxNumber", syntaxNumber.name(QColor::HexArgb));
    s.setValue("syntaxUrl",    syntaxUrl.name(QColor::HexArgb));
    s.setValue("syntaxHex",    syntaxHex.name(QColor::HexArgb));
    s.setValue("syntaxPath",   syntaxPath.name(QColor::HexArgb));
    s.setValue("syntaxGuid",   syntaxGuid.name(QColor::HexArgb));
    s.setValue("syntaxTime",   syntaxTime.name(QColor::HexArgb));

    s.setValue("gutterMarker",   gutterMarker.name(QColor::HexArgb));
    s.setValue("gutterNewEntry",  gutterNewEntry.name(QColor::HexArgb));
    s.setValue("gutterBookmark",  gutterBookmark.name(QColor::HexArgb));
    s.setValue("badgeBg",         badgeBg.name(QColor::HexArgb));
    s.setValue("badgeFg",         badgeFg.name(QColor::HexArgb));
    s.setValue("bracketMatch",    bracketMatch.name(QColor::HexArgb));
    s.setValue("searchMatch",     searchMatch.name(QColor::HexArgb));

    s.endGroup();
}

QColor AppTheme::searchNotFoundBackground(const QPalette& palette)
{
    return CardFrame::mixedColor(instance().logError, palette.color(QPalette::Base), 0.72);
}

QColor AppTheme::wrapToggleFg(const QPalette& palette)
{
    return CardFrame::mixedColor(palette.color(QPalette::Text), palette.color(QPalette::Base), 0.45);
}

QColor AppTheme::wrapToggleBg(const QPalette& palette)
{
    return CardFrame::mixedColor(palette.color(QPalette::Text), palette.color(QPalette::Base), 0.90);
}

QColor AppTheme::lineNumberFg(const QPalette& palette)
{
    return CardFrame::mixedColor(palette.color(QPalette::Text), palette.color(QPalette::Base), 0.55);
}

QColor AppTheme::wrapRail(const QPalette& palette)
{
    return CardFrame::mixedColor(palette.color(QPalette::Text), palette.color(QPalette::Base), 0.70);
}

namespace {
// Средние тона: белый текст метки читается, полоса видна на белом и на
// тёмном фоне лога; соседние по кругу различаются оттенком.
const QColor kFileColors[] = {
    QColor(0x1f, 0x6f, 0xb2), // синий
    QColor(0xb0, 0x45, 0x2a), // кирпичный
    QColor(0x2e, 0x8b, 0x57), // зелёный
    QColor(0x8e, 0x44, 0xad), // фиолетовый
    QColor(0xa6, 0x7c, 0x00), // охра
    QColor(0x00, 0x83, 0x8f), // бирюзовый
    QColor(0xc2, 0x18, 0x5b), // малиновый
    QColor(0x5d, 0x6d, 0x7e), // сланцевый
    QColor(0x6a, 0x8f, 0x1a), // оливковый
    QColor(0x3f, 0x51, 0xb5), // индиго
    QColor(0x8d, 0x5a, 0x3b), // коричневый
};
} // namespace

int AppTheme::fileColorCount()
{
    return int(std::size(kFileColors));
}

QColor AppTheme::fileColor(int ordinal, const QPalette& palette)
{
    const int n = fileColorCount();
    const QColor base = kFileColors[((ordinal % n) + n) % n];
    const bool dark = palette.color(QPalette::Base).lightness() < 128;
    return dark ? base.lighter(125) : base;
}

namespace {
// Светлая тема (фон белый): цвета темнее терминальных, «белые» — серые.
const QColor kAnsiLight[16] = {
    QColor(0x00, 0x00, 0x00), QColor(0xc5, 0x0f, 0x1f), QColor(0x10, 0x7c, 0x10),
    QColor(0x94, 0x7a, 0x00), QColor(0x00, 0x37, 0xda), QColor(0x88, 0x17, 0x98),
    QColor(0x0e, 0x7c, 0x8a), QColor(0x76, 0x76, 0x76),
    QColor(0x55, 0x55, 0x55), QColor(0xe7, 0x48, 0x56), QColor(0x13, 0xa1, 0x0e),
    QColor(0xb5, 0x8f, 0x00), QColor(0x3b, 0x78, 0xff), QColor(0xb4, 0x00, 0x9e),
    QColor(0x0f, 0x9a, 0xaa), QColor(0x9a, 0x9a, 0x9a),
};
// Тёмная тема (фон тёмный): «чёрные» — серые.
const QColor kAnsiDark[16] = {
    QColor(0x6e, 0x6e, 0x6e), QColor(0xf1, 0x4c, 0x4c), QColor(0x23, 0xd1, 0x8b),
    QColor(0xe5, 0xe5, 0x10), QColor(0x3b, 0x8e, 0xea), QColor(0xd6, 0x70, 0xd6),
    QColor(0x29, 0xb8, 0xdb), QColor(0xcc, 0xcc, 0xcc),
    QColor(0x8a, 0x8a, 0x8a), QColor(0xff, 0x6b, 0x6b), QColor(0x5a, 0xf7, 0x8e),
    QColor(0xf5, 0xf5, 0x43), QColor(0x6c, 0xb6, 0xff), QColor(0xf0, 0x8a, 0xf0),
    QColor(0x5f, 0xe0, 0xf0), QColor(0xf2, 0xf2, 0xf2),
};

double relativeLuminance(const QColor& c)
{
    const auto lin = [](double v) {
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}
} // namespace

QColor AppTheme::ansiColor(int index, const QPalette& palette)
{
    index = qBound(0, index, 255);
    if (index < 16) {
        const bool dark = palette.color(QPalette::Base).lightness() < 128;
        return dark ? kAnsiDark[index] : kAnsiLight[index];
    }
    if (index < 232) {
        const int n = index - 16;
        const auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(level(n / 36), level((n / 6) % 6), level(n % 6));
    }
    const int gray = 8 + (index - 232) * 10;
    return QColor(gray, gray, gray);
}

QColor AppTheme::ansiReadableText(const QColor& fg, const QColor& bg, const QPalette& palette)
{
    const auto contrast = [](const QColor& x, const QColor& y) {
        const double a = relativeLuminance(x);
        const double b = relativeLuminance(y);
        return (qMax(a, b) + 0.05) / (qMin(a, b) + 0.05);
    };
    if (fg.isValid() && contrast(fg, bg) >= 1.6)
        return fg;
    // Цвет текста палитры, если читается на этом фоне; иначе чёрный или
    // белый — что контрастнее (фон ANSI бывает любым: ESC[40m в светлой теме).
    const QColor text = palette.color(QPalette::Text);
    if (contrast(text, bg) >= 3.0)
        return text;
    return relativeLuminance(bg) > 0.18 ? QColor(Qt::black) : QColor(Qt::white);
}
