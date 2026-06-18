#include "shortcutmanager.h"
#include "appsettings.h"

#include <QSettings>

// ---------------------------------------------------------------------------
ShortcutManager& ShortcutManager::instance()
{
    static ShortcutManager s;
    return s;
}

// ---------------------------------------------------------------------------
ShortcutManager::ShortcutManager()
{
    // The order here is the order shown in the Settings dialog.
    m_commands = {
        { QStringLiteral("open"),        tr("Open log file(s)"),          QKeySequence(QKeySequence::Open) },
        { QStringLiteral("saveAs"),      tr("Save view as…"),             QKeySequence(QKeySequence::SaveAs) },
        { QStringLiteral("reload"),      tr("Reload file"),               QKeySequence(Qt::Key_F5) },
        { QStringLiteral("settings"),    tr("Settings…"),                 QKeySequence(QKeySequence::Preferences) },
        { QStringLiteral("focusSearch"), tr("Focus quick search field"),  QKeySequence(QKeySequence::Find) },
        { QStringLiteral("searchNext"),  tr("Search next"),               QKeySequence(Qt::Key_F3) },
        { QStringLiteral("searchPrev"),  tr("Search previous"),           QKeySequence(Qt::SHIFT | Qt::Key_F3) },
        { QStringLiteral("wordWrap"),    tr("Toggle word wrap"),          QKeySequence(Qt::ALT | Qt::Key_Z) },
        { QStringLiteral("panelTextFilters"),   tr("Show/Hide Text Filters panel"),       QKeySequence(Qt::CTRL | Qt::Key_F1) },
        { QStringLiteral("panelDirScanner"),    tr("Show/Hide Directory Scanner panel"),  QKeySequence(Qt::CTRL | Qt::Key_F2) },
        { QStringLiteral("panelTimeFilter"),    tr("Show/Hide Time Filter panel"),        QKeySequence(Qt::CTRL | Qt::Key_F3) },
        { QStringLiteral("panelFields"),        tr("Show/Hide Log Fields panel"),         QKeySequence(Qt::CTRL | Qt::Key_F4) },
        { QStringLiteral("panelRowHighlighters"), tr("Show/Hide Row Highlighters panel"), QKeySequence(Qt::CTRL | Qt::Key_F5) },
    };
}

// ---------------------------------------------------------------------------
QKeySequence ShortcutManager::defaultSequence(const QString& id) const
{
    for (const Command& c : m_commands)
        if (c.id == id)
            return c.defaultSeq;
    return QKeySequence();
}

// ---------------------------------------------------------------------------
QKeySequence ShortcutManager::sequence(const QString& id) const
{
    const auto it = m_overrides.constFind(id);
    if (it != m_overrides.constEnd())
        return it.value();
    return defaultSequence(id);
}

// ---------------------------------------------------------------------------
void ShortcutManager::setSequence(const QString& id, const QKeySequence& seq)
{
    const QKeySequence current = sequence(id);
    if (current == seq)
        return;

    if (seq == defaultSequence(id))
        m_overrides.remove(id);   // back to default → no stored override
    else
        m_overrides.insert(id, seq);

    emit shortcutsChanged();
}

// ---------------------------------------------------------------------------
void ShortcutManager::load()
{
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("Shortcuts"));
    m_overrides.clear();
    for (const Command& c : m_commands) {
        if (!s.contains(c.id))
            continue;
        const QString str = s.value(c.id).toString();
        const QKeySequence seq = QKeySequence::fromString(str, QKeySequence::PortableText);
        // Only treat it as an override if it actually differs from the default.
        if (seq != c.defaultSeq)
            m_overrides.insert(c.id, seq);
    }
    s.endGroup();
}

// ---------------------------------------------------------------------------
void ShortcutManager::save()
{
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("Shortcuts"));
    s.remove(QString());  // clear the whole group, then write current overrides
    for (auto it = m_overrides.constBegin(); it != m_overrides.constEnd(); ++it)
        s.setValue(it.key(), it.value().toString(QKeySequence::PortableText));
    s.endGroup();
}
