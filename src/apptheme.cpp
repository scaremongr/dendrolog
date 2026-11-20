#include "apptheme.h"
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
