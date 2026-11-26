#include "appsettings.h"
#include "schemaproposal.h"
#include "spooldir.h"
#include "apptheme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QSettings>
#include <QStandardPaths>

namespace {
const QLatin1String kIniName("DendroLog.ini");
const QLatin1String kLegacyIniName("LogViewer.ini"); // имя до переименования проекта
}

// ---------------------------------------------------------------------------
AppSettings& AppSettings::instance()
{
    static AppSettings s;
    return s;
}

// ---------------------------------------------------------------------------
namespace {
QString& configDirOverride()
{
    static QString dir;
    return dir;
}
}

void AppSettings::overrideConfigDirForTesting(const QString& dir)
{
    QDir().mkpath(dir);
    configDirOverride() = QDir(dir).absolutePath();
}

QString AppSettings::spoolRoot()
{
    if (!configDirOverride().isEmpty())
        return configDirOverride() + QStringLiteral("/spool");
    return SpoolDir::defaultRoot();
}

// ---------------------------------------------------------------------------
QString AppSettings::configDir()
{
    if (!configDirOverride().isEmpty())
        return configDirOverride();
    static const QString dir = []() -> QString {
        const QDir exeDir(QApplication::applicationDirPath());
        // Portable mode: явный маркер или ini, уже живущий рядом с exe.
        if (exeDir.exists(QStringLiteral("portable")) ||
            exeDir.exists(kIniName) ||
            exeDir.exists(kLegacyIniName))
            return exeDir.absolutePath();

        const QString appData =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(appData);
        return appData;
    }();
    return dir;
}

// ---------------------------------------------------------------------------
QString AppSettings::iniFilePath()
{
    const QString path = configDir() + QLatin1Char('/') + kIniName;
    // Одноразовая миграция настроек со старого имени файла.
    if (!QFileInfo::exists(path)) {
        const QString legacy = configDir() + QLatin1Char('/') + kLegacyIniName;
        if (QFileInfo::exists(legacy))
            QFile::copy(legacy, path);
    }
    return path;
}

// ---------------------------------------------------------------------------
void AppSettings::load()
{
    QSettings s(iniFilePath(), QSettings::IniFormat);

    s.beginGroup(QStringLiteral("App"));
    const QStringList exts =
        s.value(QStringLiteral("scanExtensions"),
                QStringList{ QStringLiteral("log"), QStringLiteral("txt") })
        .toStringList();
    if (!exts.isEmpty())
        m_scanExtensions = exts;

    m_scanRefreshMode        = qBound(0, s.value(QStringLiteral("scanRefreshMode"), 0).toInt(), 2);
    m_scanRefreshIntervalSecs = qBound(5, s.value(QStringLiteral("scanRefreshIntervalSecs"), 30).toInt(), 3600);
    m_fontFamily             = s.value(QStringLiteral("fontFamily")).toString();
    m_fontSize               = s.value(QStringLiteral("fontSize"), kDefaultFontSize).toInt();
    m_autoReload             = s.value(QStringLiteral("autoReload"), false).toBool();
    m_autoReloadIntervalSecs = s.value(QStringLiteral("autoReloadIntervalSecs"), 2).toInt();
    m_indexedThresholdMB     = qBound(0, s.value(QStringLiteral("indexedThresholdMB"), 512).toInt(), 1 << 20);
    m_textCacheBudgetMB      = qBound(64, s.value(QStringLiteral("textCacheBudgetMB"), 256).toInt(), 2048);
    m_themeMode              = themeModeFromString(s.value(QStringLiteral("theme")).toString());
    m_fallbackEncoding       = fallbackEncodingFromString(
        s.value(QStringLiteral("fallbackEncoding")).toString());
    m_rememberFileState      = s.value(QStringLiteral("rememberFileState"), true).toBool();
    m_restoreSessionOnStartup = s.value(QStringLiteral("restoreSessionOnStartup"), false).toBool();
    m_autoPickSchema         = s.value(QStringLiteral("autoPickSchema"), true).toBool();

    // Migrate: if this is a first run with the new key, check the old location.
    const bool hasMigratedWordWrap =
        s.contains(QStringLiteral("wordWrap"));
    s.endGroup();

    if (hasMigratedWordWrap) {
        s.beginGroup(QStringLiteral("App"));
        m_wordWrap = s.value(QStringLiteral("wordWrap"), true).toBool();
        s.endGroup();
    } else {
        // One-time migration from the old [View]/wordWrap key.
        s.beginGroup(QStringLiteral("View"));
        m_wordWrap = s.value(QStringLiteral("wordWrap"), true).toBool();
        s.endGroup();
    }

    s.beginGroup(QStringLiteral("AI"));
    m_aiEnabled = s.value(QStringLiteral("enabled"), false).toBool();
    m_aiModel = s.value(QStringLiteral("model")).toString().trimmed();
    if (m_aiModel.isEmpty())
        m_aiModel = QString::fromLatin1(SchemaProposal::kDefaultModel);
    s.endGroup();

    s.beginGroup(QStringLiteral("View"));
    m_fileLabelMode = FileLabels::modeFromName(s.value(QStringLiteral("fileLabels")).toString());
    m_lineNumbers = s.value(QStringLiteral("lineNumbers"), false).toBool();
    m_timeDelta = s.value(QStringLiteral("timeDelta"), false).toBool();
    m_ansiColors = s.value(QStringLiteral("ansiColors"), true).toBool();
    m_fileColumnChars = qBound(0, s.value(QStringLiteral("fileColumnChars"), 0).toInt(), 400);
    s.endGroup();

    s.beginGroup(QStringLiteral("QuickSearch"));
    m_quickSearchMatchCase = s.value(QStringLiteral("matchCase"), false).toBool();
    m_quickSearchWholeWord = s.value(QStringLiteral("wholeWord"), false).toBool();
    m_quickSearchRegex     = s.value(QStringLiteral("regex"), false).toBool();
    m_quickSearchHistory   = s.value(QStringLiteral("history")).toStringList();
    s.endGroup();

    s.beginGroup(QStringLiteral("Commands"));
    m_recentCommands = s.value(QStringLiteral("recent")).toStringList();
    s.endGroup();

    // First-run font default: pick the best available monospaced font using the
    // same priority order as the old LogListView constructor used.
    if (m_fontFamily.isEmpty()) {
        static const QStringList candidates {
            QStringLiteral("Cascadia Mono"),
            QStringLiteral("Cascadia Code"),
            QStringLiteral("Consolas"),
        };
        for (const QString& name : candidates) {
            if (QFontDatabase::families().contains(name, Qt::CaseInsensitive)) {
                m_fontFamily = name;
                break;
            }
        }
        if (m_fontFamily.isEmpty())
            m_fontFamily = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    }

    // Load theme colors (group [Theme] is fully owned by AppTheme).
    AppTheme::instance().load(s);
}

// ---------------------------------------------------------------------------
void AppSettings::save()
{
    QSettings s(iniFilePath(), QSettings::IniFormat);

    s.beginGroup(QStringLiteral("App"));
    s.setValue(QStringLiteral("scanExtensions"),        m_scanExtensions);
    s.setValue(QStringLiteral("scanRefreshMode"),        m_scanRefreshMode);
    s.setValue(QStringLiteral("scanRefreshIntervalSecs"), m_scanRefreshIntervalSecs);
    s.setValue(QStringLiteral("wordWrap"),               m_wordWrap);
    s.setValue(QStringLiteral("autoReload"),             m_autoReload);
    s.setValue(QStringLiteral("autoReloadIntervalSecs"), m_autoReloadIntervalSecs);
    s.setValue(QStringLiteral("fontFamily"),             m_fontFamily);
    s.setValue(QStringLiteral("fontSize"),               m_fontSize);
    s.setValue(QStringLiteral("indexedThresholdMB"),     m_indexedThresholdMB);
    s.setValue(QStringLiteral("textCacheBudgetMB"),      m_textCacheBudgetMB);
    s.setValue(QStringLiteral("theme"),                  themeModeToString(m_themeMode));
    s.setValue(QStringLiteral("fallbackEncoding"),       fallbackEncodingToString(m_fallbackEncoding));
    s.setValue(QStringLiteral("rememberFileState"),      m_rememberFileState);
    s.setValue(QStringLiteral("restoreSessionOnStartup"), m_restoreSessionOnStartup);
    s.setValue(QStringLiteral("autoPickSchema"),         m_autoPickSchema);
    s.endGroup();

    s.beginGroup(QStringLiteral("AI"));
    s.setValue(QStringLiteral("enabled"), m_aiEnabled);
    s.setValue(QStringLiteral("model"), m_aiModel);
    s.endGroup();

    s.beginGroup(QStringLiteral("View"));
    s.setValue(QStringLiteral("fileLabels"), FileLabels::modeName(m_fileLabelMode));
    s.setValue(QStringLiteral("lineNumbers"), m_lineNumbers);
    s.setValue(QStringLiteral("timeDelta"), m_timeDelta);
    s.setValue(QStringLiteral("ansiColors"), m_ansiColors);
    s.setValue(QStringLiteral("fileColumnChars"), m_fileColumnChars);
    s.endGroup();

    writeQuickSearch(s);
    writeRecentCommands(s);

    // Save theme colors.
    AppTheme::instance().save(s);
}

// ---------------------------------------------------------------------------
ThemeMode AppSettings::themeMode() const
{
    return m_themeMode;
}

void AppSettings::setThemeMode(ThemeMode mode)
{
    if (m_themeMode == mode)
        return;
    m_themeMode = mode;
    emit settingsChanged();
}

QString AppSettings::themeModeToString(ThemeMode mode)
{
    switch (mode) {
        case ThemeMode::Light: return QStringLiteral("light");
        case ThemeMode::Dark:  return QStringLiteral("dark");
        case ThemeMode::System: break;
    }
    return QStringLiteral("system");
}

ThemeMode AppSettings::themeModeFromString(const QString& text)
{
    const QString word = text.trimmed().toLower();
    if (word == QLatin1String("light"))
        return ThemeMode::Light;
    if (word == QLatin1String("dark"))
        return ThemeMode::Dark;
    return ThemeMode::System;
}

// ---------------------------------------------------------------------------
QStringList AppSettings::scanExtensions() const
{
    return m_scanExtensions;
}

void AppSettings::setScanExtensions(const QStringList& extensions)
{
    if (m_scanExtensions == extensions)
        return;
    m_scanExtensions = extensions;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
int AppSettings::scanRefreshMode() const
{
    return m_scanRefreshMode;
}

void AppSettings::setScanRefreshMode(int mode)
{
    const int clamped = qBound(0, mode, 2);
    if (m_scanRefreshMode == clamped)
        return;
    m_scanRefreshMode = clamped;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
int AppSettings::scanRefreshIntervalSecs() const
{
    return m_scanRefreshIntervalSecs;
}

void AppSettings::setScanRefreshIntervalSecs(int secs)
{
    const int clamped = qBound(5, secs, 3600);
    if (m_scanRefreshIntervalSecs == clamped)
        return;
    m_scanRefreshIntervalSecs = clamped;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
void AppSettings::writeQuickSearch(QSettings& s) const
{
    s.beginGroup(QStringLiteral("QuickSearch"));
    s.setValue(QStringLiteral("matchCase"), m_quickSearchMatchCase);
    s.setValue(QStringLiteral("wholeWord"), m_quickSearchWholeWord);
    s.setValue(QStringLiteral("regex"),     m_quickSearchRegex);
    s.setValue(QStringLiteral("history"),   m_quickSearchHistory);
    s.endGroup();
}

void AppSettings::writeRecentCommands(QSettings& s) const
{
    s.beginGroup(QStringLiteral("Commands"));
    s.setValue(QStringLiteral("recent"), m_recentCommands);
    s.endGroup();
}

void AppSettings::saveRecentCommands()
{
    QSettings s(iniFilePath(), QSettings::IniFormat);
    writeRecentCommands(s);
}

void AppSettings::saveQuickSearch()
{
    QSettings s(iniFilePath(), QSettings::IniFormat);
    writeQuickSearch(s);
}

void AppSettings::setQuickSearchMatchCase(bool on)
{
    m_quickSearchMatchCase = on;
}

void AppSettings::setQuickSearchWholeWord(bool on)
{
    m_quickSearchWholeWord = on;
}

void AppSettings::setQuickSearchRegex(bool on)
{
    m_quickSearchRegex = on;
}

void AppSettings::setQuickSearchHistory(const QStringList& entries)
{
    m_quickSearchHistory = entries;
}

// ---------------------------------------------------------------------------
bool AppSettings::wordWrap() const
{
    return m_wordWrap;
}

void AppSettings::setLineNumbers(bool visible)
{
    if (m_lineNumbers == visible)
        return;
    m_lineNumbers = visible;
    emit settingsChanged();
}

void AppSettings::setTimeDelta(bool visible)
{
    if (m_timeDelta == visible)
        return;
    m_timeDelta = visible;
    emit settingsChanged();
}

void AppSettings::setAnsiColors(bool enabled)
{
    if (m_ansiColors == enabled)
        return;
    m_ansiColors = enabled;
    emit settingsChanged();
}

void AppSettings::setFileColumnChars(int chars)
{
    chars = qBound(0, chars, 400);
    if (m_fileColumnChars == chars)
        return;
    m_fileColumnChars = chars;
    emit settingsChanged();
}

void AppSettings::setFileLabelMode(FileLabelMode mode)
{
    if (m_fileLabelMode == mode)
        return;
    m_fileLabelMode = mode;
    emit settingsChanged();
}

void AppSettings::setWordWrap(bool enabled)
{
    if (m_wordWrap == enabled)
        return;
    m_wordWrap = enabled;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
QString AppSettings::fontFamily() const
{
    return m_fontFamily;
}

void AppSettings::setFontFamily(const QString& family)
{
    if (m_fontFamily == family)
        return;
    m_fontFamily = family;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
int AppSettings::fontSize() const
{
    return m_fontSize;
}

void AppSettings::setFontSize(int size)
{
    if (m_fontSize == size)
        return;
    m_fontSize = size;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
int AppSettings::indexedThresholdMB() const
{
    return m_indexedThresholdMB;
}

void AppSettings::setIndexedThresholdMB(int mb)
{
    const int clamped = qBound(0, mb, 1 << 20);
    if (m_indexedThresholdMB == clamped)
        return;
    m_indexedThresholdMB = clamped;
    emit settingsChanged();
}

qint64 AppSettings::indexedThresholdBytes() const
{
    return qint64(m_indexedThresholdMB) * 1024 * 1024;
}

// ---------------------------------------------------------------------------
void AppSettings::setRememberFileState(bool enabled)
{
    if (m_rememberFileState == enabled)
        return;
    m_rememberFileState = enabled;
    emit settingsChanged();
}

QString AppSettings::fileMemoryDir()
{
    return configDir() + QStringLiteral("/memory");
}

void AppSettings::setAiEnabled(bool enabled)
{
    if (m_aiEnabled == enabled)
        return;
    m_aiEnabled = enabled;
    emit settingsChanged();
}

void AppSettings::setAiModel(const QString& model)
{
    const QString value = model.trimmed().isEmpty() ? QString::fromLatin1(SchemaProposal::kDefaultModel)
                                                    : model.trimmed();
    if (m_aiModel == value)
        return;
    m_aiModel = value;
    emit settingsChanged();
}

QString AppSettings::aiModel() const
{
    return m_aiModel.isEmpty() ? QString::fromLatin1(SchemaProposal::kDefaultModel) : m_aiModel;
}

bool AppSettings::isConfigDirOverridden()
{
    return !configDirOverride().isEmpty();
}

void AppSettings::setRestoreSessionOnStartup(bool enabled)
{
    if (m_restoreSessionOnStartup == enabled)
        return;
    m_restoreSessionOnStartup = enabled;
    emit settingsChanged();
}

void AppSettings::setAutoPickSchema(bool enabled)
{
    if (m_autoPickSchema == enabled)
        return;
    m_autoPickSchema = enabled;
    emit settingsChanged();
}

QString AppSettings::lastSessionFile()
{
    return configDir() + QStringLiteral("/last-session.json");
}

int AppSettings::textCacheBudgetMB() const
{
    return m_textCacheBudgetMB;
}

void AppSettings::setTextCacheBudgetMB(int mb)
{
    const int clamped = qBound(64, mb, 2048);
    if (m_textCacheBudgetMB == clamped)
        return;
    m_textCacheBudgetMB = clamped;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
bool AppSettings::autoReload() const
{
    return m_autoReload;
}

void AppSettings::setAutoReload(bool enabled)
{
    if (m_autoReload == enabled)
        return;
    m_autoReload = enabled;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
int AppSettings::autoReloadIntervalSecs() const
{
    return m_autoReloadIntervalSecs;
}

void AppSettings::setAutoReloadIntervalSecs(int secs)
{
    const int clamped = qBound(1, secs, 60);
    if (m_autoReloadIntervalSecs == clamped)
        return;
    m_autoReloadIntervalSecs = clamped;
    emit settingsChanged();
}

// ---------------------------------------------------------------------------
void AppSettings::setFallbackEncodingChoice(std::optional<TextEncoding> encoding)
{
    if (encoding && !TextEncodings::isSingleByte(*encoding))
        encoding.reset();
    if (m_fallbackEncoding == encoding)
        return;
    const TextEncoding before = fallbackEncoding();
    m_fallbackEncoding = encoding;
    if (fallbackEncoding() != before)
        emit fallbackEncodingChanged(fallbackEncoding());
    emit settingsChanged();
}

TextEncoding AppSettings::fallbackEncoding() const
{
    return m_fallbackEncoding.value_or(TextEncodings::systemFallback());
}

QString AppSettings::fallbackEncodingToString(std::optional<TextEncoding> encoding)
{
    return encoding ? TextEncodings::displayName(*encoding) : QStringLiteral("system");
}

std::optional<TextEncoding> AppSettings::fallbackEncodingFromString(const QString& text)
{
    const std::optional<TextEncoding> encoding = TextEncodings::fromName(text.trimmed());
    if (!encoding || !TextEncodings::isSingleByte(*encoding))
        return std::nullopt;
    return encoding;
}
