#include "commandstatusbar.h"

#include "commandspooler.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

CommandStatusBar::CommandStatusBar(CommandSpooler* spooler, QWidget* parent)
    : QFrame(parent)
    , m_spooler(spooler)
{
    setObjectName(QStringLiteral("commandStatusBar"));
    // Тот же вид, что у плашек вкладки (предложение фильтров, полный лог).
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::ToolTipBase);
    setForegroundRole(QPalette::ToolTipText);
    auto* layout = new QHBoxLayout(this);
    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("commandStatusText"));
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_text, 1);
    m_stop = new QPushButton(tr("Stop"), this);
    m_stop->setObjectName(QStringLiteral("commandStopButton"));
    m_stop->setToolTip(tr("Stop the process (politely, then forcibly); the lines stay"));
    layout->addWidget(m_stop);
    m_restart = new QPushButton(tr("Restart"), this);
    m_restart->setObjectName(QStringLiteral("commandRestartButton"));
    m_restart->setToolTip(tr("Run the command again; its output is appended below"));
    layout->addWidget(m_restart);

    connect(m_stop, &QPushButton::clicked, this, [this]() {
        if (m_spooler)
            m_spooler->stop();
    });
    connect(m_restart, &QPushButton::clicked, this, [this]() {
        if (m_spooler)
            emit restartRequested(m_spooler);
    });
    if (spooler)
        connect(spooler, &CommandSpooler::stateChanged, this, &CommandStatusBar::refresh);
    refresh();
}

QString CommandStatusBar::text() const
{
    return m_text->text();
}

void CommandStatusBar::refresh()
{
    if (!m_spooler) {
        m_text->clear();
        m_stop->setEnabled(false);
        m_restart->setEnabled(false);
        return;
    }
    const CommandSpec& spec = m_spooler->spec();
    // Команда — как введена: способ разбора виден рядом.
    QString command = spec.commandLine.toHtmlEscaped();
    QString text = QStringLiteral("<b>%1</b> — <code>%2</code>")
                       .arg(m_spooler->statusText().toHtmlEscaped(), command);
    m_text->setText(text);
    QString tip = spec.useShell ? tr("Run through the shell") : tr("Run without a shell");
    if (!spec.workingDirectory.isEmpty())
        tip += QLatin1Char('\n') + tr("Working directory: %1").arg(QDir::toNativeSeparators(spec.workingDirectory));
    m_text->setToolTip(tip);
    const bool running = m_spooler->isRunning();
    m_stop->setVisible(running);
    m_stop->setEnabled(m_spooler->state() == CommandSpooler::State::Running);
    m_restart->setVisible(!running);
    m_restart->setEnabled(!running);
}
