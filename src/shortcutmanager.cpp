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
        { QStringLiteral("closeTab"),    tr("Close tab"),                 QKeySequence(Qt::CTRL | Qt::Key_W) },
        { QStringLiteral("reload"),      tr("Reload file"),               QKeySequence(Qt::Key_F5) },
        { QStringLiteral("autoReload"),  tr("Toggle auto-reload for this tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F5) },
        { QStringLiteral("followTail"),  tr("Toggle follow tail"),        QKeySequence(Qt::SHIFT | Qt::Key_F5) },
        // QKeySequence::Preferences is empty on Windows — spell Ctrl+, out.
        { QStringLiteral("settings"),    tr("Settings…"),                 QKeySequence(Qt::CTRL | Qt::Key_Comma) },
        { QStringLiteral("focusSearch"), tr("Focus quick search field"),  QKeySequence(QKeySequence::Find) },
        { QStringLiteral("searchNext"),  tr("Search next"),               QKeySequence(Qt::Key_F3) },
        { QStringLiteral("searchPrev"),  tr("Search previous"),           QKeySequence(Qt::SHIFT | Qt::Key_F3) },
        { QStringLiteral("findAll"),     tr("Find all (Search panel)"),   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F) },
        { QStringLiteral("wordWrap"),    tr("Toggle word wrap"),          QKeySequence(Qt::ALT | Qt::Key_Z) },
        { QStringLiteral("zoomIn"),      tr("Larger font"),               QKeySequence(QKeySequence::ZoomIn) },
        { QStringLiteral("zoomOut"),     tr("Smaller font"),              QKeySequence(QKeySequence::ZoomOut) },
        { QStringLiteral("zoomReset"),   tr("Default font size"),         QKeySequence(Qt::CTRL | Qt::Key_0) },
        { QStringLiteral("levelFatal"),  tr("Show only Fatal (toggle)"),  QKeySequence(Qt::ALT | Qt::Key_1) },
        { QStringLiteral("levelError"),  tr("Show only Error (toggle)"),  QKeySequence(Qt::ALT | Qt::Key_2) },
        { QStringLiteral("levelWarn"),   tr("Show only Warn (toggle)"),   QKeySequence(Qt::ALT | Qt::Key_3) },
        { QStringLiteral("levelInfo"),   tr("Show only Info (toggle)"),   QKeySequence(Qt::ALT | Qt::Key_4) },
        { QStringLiteral("levelDebug"),  tr("Show only Debug (toggle)"),  QKeySequence(Qt::ALT | Qt::Key_5) },
        { QStringLiteral("levelTrace"),  tr("Show only Trace (toggle)"),  QKeySequence(Qt::ALT | Qt::Key_6) },
        { QStringLiteral("levelAll"),    tr("Show all levels"),           QKeySequence(Qt::ALT | Qt::Key_0) },
        { QStringLiteral("resetFilters"), tr("Reset filters of this tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R) },
        { QStringLiteral("panelTextFilters"),   tr("Show/Hide Text Filters panel"),       QKeySequence(Qt::CTRL | Qt::Key_F1) },
        { QStringLiteral("panelDirScanner"),    tr("Show/Hide Directory Scanner panel"),  QKeySequence(Qt::CTRL | Qt::Key_F2) },
        { QStringLiteral("panelTimeFilter"),    tr("Show/Hide Time Filter panel"),        QKeySequence(Qt::CTRL | Qt::Key_F3) },
        { QStringLiteral("panelFields"),        tr("Show/Hide Log Fields panel"),         QKeySequence(Qt::CTRL | Qt::Key_F4) },
        { QStringLiteral("panelRowHighlighters"), tr("Show/Hide Row Highlighters panel"), QKeySequence(Qt::CTRL | Qt::Key_F5) },
        { QStringLiteral("panelTimeline"),        tr("Show/Hide Timeline panel"),         QKeySequence(Qt::CTRL | Qt::Key_F6) },
        { QStringLiteral("panelSearchResults"),   tr("Show/Hide Search panel"),   QKeySequence(Qt::CTRL | Qt::Key_F7) },
        { QStringLiteral("panelEntryDetails"),    tr("Show/Hide Entry Details panel"),    QKeySequence(Qt::CTRL | Qt::Key_F8) },
        { QStringLiteral("panelStatistics"),      tr("Show/Hide Statistics panel"),       QKeySequence(Qt::CTRL | Qt::Key_F9) },
        { QStringLiteral("quit"),        tr("Exit"),                      QKeySequence(Qt::CTRL | Qt::Key_Q) },
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
