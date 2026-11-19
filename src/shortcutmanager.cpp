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
        { QStringLiteral("reopenTab"),   tr("Reopen closed tab"),         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_T) },
        { QStringLiteral("reload"),      tr("Reload file"),               QKeySequence(Qt::Key_F5) },
        { QStringLiteral("autoReload"),  tr("Toggle auto-reload for this tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F5) },
        { QStringLiteral("followTail"),  tr("Toggle follow tail"),        QKeySequence(Qt::SHIFT | Qt::Key_F5) },
        // QKeySequence::Preferences is empty on Windows — spell Ctrl+, out.
        { QStringLiteral("settings"),    tr("Settings…"),                 QKeySequence(Qt::CTRL | Qt::Key_Comma) },
        { QStringLiteral("focusSearch"), tr("Focus quick search field"),  QKeySequence(QKeySequence::Find) },
        { QStringLiteral("searchNext"),  tr("Search next"),               QKeySequence(Qt::Key_F3) },
        { QStringLiteral("searchPrev"),  tr("Search previous"),           QKeySequence(Qt::SHIFT | Qt::Key_F3) },
        { QStringLiteral("findAll"),     tr("Find all (Search panel)"),   QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F) },
        { QStringLiteral("findMatchCase"), tr("Quick search: match case (toggle)"),   QKeySequence(Qt::ALT | Qt::Key_C) },
        { QStringLiteral("findWholeWord"), tr("Quick search: whole word (toggle)"),   QKeySequence(Qt::ALT | Qt::Key_W) },
        { QStringLiteral("findRegex"),     tr("Quick search: regular expression (toggle)"), QKeySequence(Qt::ALT | Qt::Key_R) },
        { QStringLiteral("goTo"),        tr("Go to line or time…"),       QKeySequence(Qt::CTRL | Qt::Key_G) },
        // История переходов (R-7): как «назад / вперёд» в браузере.
        { QStringLiteral("navigateBack"),    tr("Navigate back (navigation history)"),    QKeySequence(Qt::ALT | Qt::Key_Left) },
        { QStringLiteral("navigateForward"), tr("Navigate forward (navigation history)"), QKeySequence(Qt::ALT | Qt::Key_Right) },
        { QStringLiteral("nextError"),   tr("Next error"),                QKeySequence(Qt::Key_F8) },
        { QStringLiteral("prevError"),   tr("Previous error"),            QKeySequence(Qt::SHIFT | Qt::Key_F8) },
        { QStringLiteral("nextWarning"), tr("Next warning"),              QKeySequence(Qt::Key_F7) },
        { QStringLiteral("prevWarning"), tr("Previous warning"),          QKeySequence(Qt::SHIFT | Qt::Key_F7) },
        { QStringLiteral("toggleBookmark"), tr("Add / remove bookmark"),  QKeySequence(Qt::CTRL | Qt::Key_F2) },
        { QStringLiteral("nextBookmark"),   tr("Next bookmark"),          QKeySequence(Qt::Key_F2) },
        { QStringLiteral("prevBookmark"),   tr("Previous bookmark"),      QKeySequence(Qt::SHIFT | Qt::Key_F2) },
        { QStringLiteral("goToNewLines"), tr("Go to new lines (below the screen)"), QKeySequence(Qt::ALT | Qt::Key_End) },
        // Полный лог (R-3): фильтры вкладки приостановлены и обратно.
        { QStringLiteral("fullLog"),     tr("Show in full log / return to filtered view"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L) },
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
        // Ctrl+F2 — закладка (как в привычной раскладке F2 / Shift+F2 /
        // Ctrl+F2), поэтому Directory Scanner ушёл на Ctrl+F10.
        { QStringLiteral("panelDirScanner"),    tr("Show/Hide Directory Scanner panel"),  QKeySequence(Qt::CTRL | Qt::Key_F10) },
        { QStringLiteral("panelTimeFilter"),    tr("Show/Hide Time Filter panel"),        QKeySequence(Qt::CTRL | Qt::Key_F3) },
        { QStringLiteral("panelFields"),        tr("Show/Hide Log Fields panel"),         QKeySequence(Qt::CTRL | Qt::Key_F4) },
        { QStringLiteral("panelRowHighlighters"), tr("Show/Hide Row Highlighters panel"), QKeySequence(Qt::CTRL | Qt::Key_F5) },
        { QStringLiteral("panelTimeline"),        tr("Show/Hide Timeline panel"),         QKeySequence(Qt::CTRL | Qt::Key_F6) },
        { QStringLiteral("panelSearchResults"),   tr("Show/Hide Search panel"),   QKeySequence(Qt::CTRL | Qt::Key_F7) },
        { QStringLiteral("panelEntryDetails"),    tr("Show/Hide Entry Details panel"),    QKeySequence(Qt::CTRL | Qt::Key_F8) },
        { QStringLiteral("panelStatistics"),      tr("Show/Hide Statistics panel"),       QKeySequence(Qt::CTRL | Qt::Key_F9) },
        { QStringLiteral("panelBookmarks"),       tr("Show/Hide Bookmarks panel"),        QKeySequence(Qt::CTRL | Qt::Key_F11) },
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
    // Сочетание по умолчанию, которое пользователь уже отдал другой команде
    // (например, новое сочетание вышедшей позже команды), остаётся у той
    // команды: два действия на одной клавише не срабатывали бы оба.
    const QKeySequence def = defaultSequence(id);
    if (!def.isEmpty()) {
        for (auto o = m_overrides.constBegin(); o != m_overrides.constEnd(); ++o)
            if (o.key() != id && o.value() == def)
                return QKeySequence();
    }
    return def;
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
