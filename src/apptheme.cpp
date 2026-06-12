#include "apptheme.h"
#include <QSettings>

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

    // Selection colors
    selectionFill  = readColor(s, "selectionFill",  selectionFill);
    selectionRowBg = readColor(s, "selectionRowBg", selectionRowBg);

    // Syntax highlight colors
    syntaxString = readColor(s, "syntaxString", syntaxString);
    syntaxNumber = readColor(s, "syntaxNumber", syntaxNumber);
    syntaxUrl    = readColor(s, "syntaxUrl",    syntaxUrl);
    syntaxHex    = readColor(s, "syntaxHex",    syntaxHex);
    syntaxPath   = readColor(s, "syntaxPath",   syntaxPath);

    // UI element colors
    gutterMarker  = readColor(s, "gutterMarker",   gutterMarker);
    gutterNewEntry= readColor(s, "gutterNewEntry",  gutterNewEntry);
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

    s.setValue("selectionFill",  selectionFill.name(QColor::HexArgb));
    s.setValue("selectionRowBg", selectionRowBg.name(QColor::HexArgb));

    s.setValue("syntaxString", syntaxString.name(QColor::HexArgb));
    s.setValue("syntaxNumber", syntaxNumber.name(QColor::HexArgb));
    s.setValue("syntaxUrl",    syntaxUrl.name(QColor::HexArgb));
    s.setValue("syntaxHex",    syntaxHex.name(QColor::HexArgb));
    s.setValue("syntaxPath",   syntaxPath.name(QColor::HexArgb));

    s.setValue("gutterMarker",   gutterMarker.name(QColor::HexArgb));
    s.setValue("gutterNewEntry",  gutterNewEntry.name(QColor::HexArgb));
    s.setValue("badgeBg",         badgeBg.name(QColor::HexArgb));
    s.setValue("badgeFg",         badgeFg.name(QColor::HexArgb));
    s.setValue("bracketMatch",    bracketMatch.name(QColor::HexArgb));
    s.setValue("searchMatch",     searchMatch.name(QColor::HexArgb));

    s.endGroup();
}
