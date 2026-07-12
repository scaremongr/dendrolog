#include "appsettings.h"
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
QString AppSettings::configDir()
{
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

    m_fontFamily             = s.value(QStringLiteral("fontFamily")).toString();
    m_fontSize               = s.value(QStringLiteral("fontSize"), 10).toInt();
    m_autoReload             = s.value(QStringLiteral("autoReload"), false).toBool();
    m_autoReloadIntervalSecs = s.value(QStringLiteral("autoReloadIntervalSecs"), 2).toInt();
    m_indexedThresholdMB     = qBound(0, s.value(QStringLiteral("indexedThresholdMB"), 512).toInt(), 1 << 20);
    m_textCacheBudgetMB      = qBound(64, s.value(QStringLiteral("textCacheBudgetMB"), 256).toInt(), 2048);

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
    s.setValue(QStringLiteral("wordWrap"),               m_wordWrap);
    s.setValue(QStringLiteral("autoReload"),             m_autoReload);
    s.setValue(QStringLiteral("autoReloadIntervalSecs"), m_autoReloadIntervalSecs);
    s.setValue(QStringLiteral("fontFamily"),             m_fontFamily);
    s.setValue(QStringLiteral("fontSize"),               m_fontSize);
    s.setValue(QStringLiteral("indexedThresholdMB"),     m_indexedThresholdMB);
    s.setValue(QStringLiteral("textCacheBudgetMB"),      m_textCacheBudgetMB);
    s.endGroup();

    // Save theme colors.
    AppTheme::instance().save(s);
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
bool AppSettings::wordWrap() const
{
    return m_wordWrap;
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
