#include "commandoutputdialog.h"

#include "apptheme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

CommandOutputDialog::CommandOutputDialog(const QList<CommandSpec>& recent,
                                         const QString& defaultDirectory, QWidget* parent)
    : QDialog(parent)
    , m_recent(recent)
{
    setObjectName(QStringLiteral("commandOutputDialog"));
    setWindowTitle(tr("Open Command Output"));

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("The command runs now; its output (stdout and stderr) opens as a "
                                "growing log. Closing the tab stops the process."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();
    m_command = new QComboBox(this);
    m_command->setObjectName(QStringLiteral("commandLineCombo"));
    m_command->setEditable(true);
    m_command->setInsertPolicy(QComboBox::NoInsert);
    m_command->setMinimumContentsLength(48);
    m_command->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const CommandSpec& spec : m_recent)
        m_command->addItem(spec.commandLine);
    m_command->setCurrentIndex(-1);
    m_command->setEditText(QString());
    m_command->lineEdit()->setPlaceholderText(tr("e.g. kubectl logs -f deploy/api"));
    m_command->setToolTip(tr("Recent commands are in the drop-down list"));
    form->addRow(tr("&Command:"), m_command);

    auto* dirRow = new QHBoxLayout();
    m_directory = new QLineEdit(QDir::toNativeSeparators(defaultDirectory), this);
    m_directory->setObjectName(QStringLiteral("commandDirectoryEdit"));
    dirRow->addWidget(m_directory, 1);
    auto* browse = new QPushButton(tr("Browse..."), this);
    browse->setObjectName(QStringLiteral("commandDirectoryBrowse"));
    dirRow->addWidget(browse);
    form->addRow(tr("&Working directory:"), dirRow);

#ifdef Q_OS_WIN
    const QString shellName = QStringLiteral("cmd.exe /c");
#else
    const QString shellName = QStringLiteral("/bin/sh -c");
#endif
    m_shell = new QCheckBox(tr("Run through the &shell (%1)").arg(shellName), this);
    m_shell->setObjectName(QStringLiteral("commandShellCheck"));
    form->addRow(QString(), m_shell);
    layout->addLayout(form);

    m_hint = new QLabel(this);
    m_hint->setObjectName(QStringLiteral("commandHint"));
    m_hint->setWordWrap(true);
    m_hint->setEnabled(false); // приглушённый текст
    layout->addWidget(m_hint);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("commandError"));
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Run"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &CommandOutputDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_command, &QComboBox::activated, this, &CommandOutputDialog::applyRecent);
    connect(m_command, &QComboBox::editTextChanged, this, [this]() { m_error->hide(); });
    connect(m_shell, &QCheckBox::toggled, this, [this]() {
        m_error->hide();
        updateHint();
    });
    connect(browse, &QPushButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Working Directory"),
                                                              QDir::fromNativeSeparators(m_directory->text()));
        if (!dir.isEmpty())
            m_directory->setText(QDir::toNativeSeparators(dir));
    });
    updateHint();
}

CommandSpec CommandOutputDialog::spec() const
{
    CommandSpec spec;
    spec.commandLine = m_command->currentText().trimmed();
    spec.workingDirectory = QDir::fromNativeSeparators(m_directory->text().trimmed());
    spec.useShell = m_shell->isChecked();
    return spec;
}

void CommandOutputDialog::setSpec(const CommandSpec& spec)
{
    m_command->setEditText(spec.commandLine);
    m_directory->setText(QDir::toNativeSeparators(spec.workingDirectory));
    m_shell->setChecked(spec.useShell);
}

void CommandOutputDialog::applyRecent(int index)
{
    if (index >= 0 && index < m_recent.size())
        setSpec(m_recent.at(index));
}

void CommandOutputDialog::updateHint()
{
    m_hint->setText(m_shell->isChecked()
        ? tr("The shell interprets the line: pipes, redirection, variables and quoting work "
             "as in a terminal.")
        : tr("No shell: one program is started; arguments are split at spaces, double quotes "
             "group them. Pipes and redirection need the shell."));
}

void CommandOutputDialog::accept()
{
    // Цвет ошибки — из темы (инвариант 6), на момент показа.
    QPalette pal = m_error->palette();
    pal.setColor(QPalette::WindowText, AppTheme::instance().logError);
    m_error->setPalette(pal);
    const CommandSpec s = spec();
    QString error;
    if (!s.resolve(nullptr, &error)) {
        m_error->setText(error);
        m_error->show();
        return;
    }
    if (!s.workingDirectory.isEmpty() && !QFileInfo(s.workingDirectory).isDir()) {
        m_error->setText(tr("The working directory does not exist."));
        m_error->show();
        return;
    }
    QDialog::accept();
}
