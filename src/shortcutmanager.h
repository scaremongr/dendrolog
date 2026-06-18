#pragma once

#include <QObject>
#include <QKeySequence>
#include <QString>
#include <QVector>
#include <QHash>

// ---------------------------------------------------------------------------
// ShortcutManager — single source of truth for every user-configurable
// keyboard shortcut in the application.
//
//   • Owns an ordered list of named commands, each with a human-readable label
//     and a built-in default key sequence.
//   • Stores per-command overrides chosen by the user and persists them in the
//     shared LogViewer.ini ([Shortcuts] group).
//   • Emits shortcutsChanged() after the set is modified so the main window can
//     re-apply the sequences to its QActions without a restart.
//
// MainWindow registers a QAction for each command id; the Settings dialog edits
// the sequences. Neither needs to know the full command list — they iterate
// commands() and look actions up by id.
// ---------------------------------------------------------------------------
class ShortcutManager : public QObject
{
    Q_OBJECT

public:
    struct Command {
        QString      id;          // stable key, used in the INI and the action map
        QString      label;       // shown in the Settings dialog
        QKeySequence defaultSeq;  // built-in default
    };

    static ShortcutManager& instance();

    // Stable, ordered list of all configurable commands.
    const QVector<Command>& commands() const { return m_commands; }

    // Effective sequence for a command: the user override if set, else the
    // built-in default. Returns an empty sequence for an unknown id.
    QKeySequence sequence(const QString& id) const;

    // Default sequence for a command (ignores any override).
    QKeySequence defaultSequence(const QString& id) const;

    // Override a command's sequence. An empty sequence clears the binding;
    // passing the default removes the override. Emits shortcutsChanged().
    void setSequence(const QString& id, const QKeySequence& seq);

    void load();   // read overrides from LogViewer.ini
    void save();   // write overrides to LogViewer.ini

signals:
    void shortcutsChanged();

private:
    ShortcutManager();
    Q_DISABLE_COPY_MOVE(ShortcutManager)

    QVector<Command>              m_commands;
    QHash<QString, QKeySequence>  m_overrides;
};
