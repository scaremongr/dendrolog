#ifndef COMMANDOUTPUTDIALOG_H
#define COMMANDOUTPUTDIALOG_H

#include "commandspooler.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

// ============================================================================
// File → Open Command Output… (F-7): строка команды (с выпадающими недавними
// командами), рабочий каталог, «через shell». Сам ничего не запускает: окно
// берёт spec() после accept. Невалидная строка (пустая, незакрытая кавычка
// без shell, нет каталога) не закрывает диалог — причина под полем.
// ============================================================================
class CommandOutputDialog : public QDialog {
    Q_OBJECT
public:
    CommandOutputDialog(const QList<CommandSpec>& recent, const QString& defaultDirectory,
                        QWidget* parent = nullptr);

    CommandSpec spec() const;
    void setSpec(const CommandSpec& spec);

    void accept() override;

private:
    void applyRecent(int index);
    void updateHint();

    QList<CommandSpec> m_recent;
    QComboBox* m_command = nullptr;
    QLineEdit* m_directory = nullptr;
    QCheckBox* m_shell = nullptr;
    QLabel* m_hint = nullptr;
    QLabel* m_error = nullptr;
};

#endif // COMMANDOUTPUTDIALOG_H
