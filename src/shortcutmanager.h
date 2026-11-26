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
//     shared DendroLog.ini ([Shortcuts] group).
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

    // Override a command's sequence. An empty sequence clears the binding
    // (stored even while the default is yielded to another command — U-20);
    // passing the default removes the override. Emits shortcutsChanged()
    // when the stored overrides change.
    void setSequence(const QString& id, const QKeySequence& seq);

    // ---- Уступленные сочетания (U-20) ---------------------------------------
    // Кто какое сочетание держит: действующее (`assignment()`) или рабочая
    // копия диалога Settings. Функции ниже — чистые, по этой таблице, чтобы
    // диалог показывал состояние полей до OK.
    using Assignment = QHash<QString, QKeySequence>;

    // Действующие сочетания всех команд (id → sequence(id)).
    Assignment assignment() const;

    // Умолчание команды уступлено: своего переопределения нет, а
    // сочетание по умолчанию держит переопределение другой команды.
    bool isYielded(const QString& id) const;

    // Команда, у которой в `a` сочетание по умолчанию команды `id`, когда
    // у самой `id` поле пустое: «Alt+← уступлено команде Go to». Пусто —
    // ничего не уступлено (поле не пустое, умолчания нет или его никто не держит).
    QString defaultHolder(const QString& id, const Assignment& a) const;

    // Что даёт «вернуть сочетание по умолчанию» команде `id`.
    struct Restore {
        Assignment   result;      // таблица после возврата
        QString      holder;      // кто держал сочетание (пусто — никто)
        QKeySequence holderKeys;  // что у него после возврата: своё умолчание
                                  // или пусто, если и его кто-то держит
    };
    // `id` получает своё умолчание; команда, что его держала, теряет его и
    // возвращается к своему умолчанию, если оно свободно, иначе остаётся без
    // сочетания. Каскада нет: остальные команды не трогаются. Держателей
    // несколько (конфликт полей) — снимается со всех, `holder` — первый.
    Restore restoreDefault(const QString& id, const Assignment& a) const;

    void load();   // read overrides from DendroLog.ini
    void save();   // write overrides to DendroLog.ini

signals:
    void shortcutsChanged();

private:
    ShortcutManager();
    Q_DISABLE_COPY_MOVE(ShortcutManager)

    QVector<Command>              m_commands;
    QHash<QString, QKeySequence>  m_overrides;
};
