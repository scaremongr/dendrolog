#include "commandspooler.h"

#include "spooldir.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// CommandSpec
// ---------------------------------------------------------------------------

namespace {

QString programLabel(QString program)
{
    // Оба разделителя на любой ОС: строка могла прийти из недавних другой ОС.
    const qsizetype slash = qMax(program.lastIndexOf(QLatin1Char('/')), program.lastIndexOf(QLatin1Char('\\')));
    QString name = program.mid(slash + 1);
    for (const QLatin1String ext : {QLatin1String(".exe"), QLatin1String(".cmd"), QLatin1String(".bat"),
                                    QLatin1String(".com")}) {
        if (name.size() > ext.size() && name.endsWith(ext, Qt::CaseInsensitive)) {
            name.chop(ext.size());
            break;
        }
    }
    return name;
}

// Те же правила, что у QProcess::splitCommand: `"` открывает и закрывает,
// три подряд — буквальная кавычка.
bool hasUnclosedQuote(const QString& line)
{
    bool inQuote = false;
    int quoteCount = 0;
    for (const QChar c : line) {
        if (c == QLatin1Char('"')) {
            if (++quoteCount == 3)
                quoteCount = 0;
            continue;
        }
        if (quoteCount == 1)
            inQuote = !inQuote;
        quoteCount = 0;
    }
    if (quoteCount == 1)
        inQuote = !inQuote;
    return inQuote;
}

const QString kKeyCommand = QStringLiteral("command");
const QString kKeyDir = QStringLiteral("dir");
const QString kKeyShell = QStringLiteral("shell");

} // namespace

bool CommandSpec::resolve(Launch* launch, QString* error) const
{
    const QString line = commandLine.trimmed();
    auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (line.isEmpty())
        return fail(QCoreApplication::translate("CommandSpec", "The command is empty."));
    Launch out;
    if (useShell) {
#ifdef Q_OS_WIN
        const QString comspec = qEnvironmentVariable("ComSpec");
        out.program = comspec.isEmpty() ? QStringLiteral("cmd.exe") : comspec;
        // /d — без AutoRun из реестра, /s /c "…" — строка как есть, внешние
        // кавычки снимаются целиком (cmd /?).
        out.nativeArguments = QStringLiteral("/d /s /c \"") + line + QLatin1Char('"');
#else
        out.program = QStringLiteral("/bin/sh");
        out.arguments = {QStringLiteral("-c"), line};
#endif
    } else {
        // QProcess::splitCommand незакрытую кавычку молча закрывает в конце —
        // команда вышла бы не той, что видна в поле.
        if (hasUnclosedQuote(line))
            return fail(QCoreApplication::translate("CommandSpec",
                "Unclosed double quote. Without the shell, arguments are split at spaces "
                "and double quotes group them."));
        QStringList parts = QProcess::splitCommand(line);
        if (parts.isEmpty())
            return fail(QCoreApplication::translate("CommandSpec", "The command is empty."));
        out.program = parts.takeFirst();
        out.arguments = parts;
    }
    if (launch)
        *launch = out;
    return true;
}

QString CommandSpec::shortLabel() const
{
    const QStringList parts = QProcess::splitCommand(commandLine.trimmed());
    const QString label = parts.isEmpty() ? QString() : programLabel(parts.first());
    return label.isEmpty() ? QStringLiteral("command") : label;
}

QVariantMap CommandSpec::toVariant() const
{
    QVariantMap map;
    map.insert(kKeyCommand, commandLine);
    map.insert(kKeyDir, workingDirectory);
    map.insert(kKeyShell, useShell);
    return map;
}

CommandSpec CommandSpec::fromVariant(const QVariant& value)
{
    const QVariantMap map = value.toMap();
    CommandSpec spec;
    spec.commandLine = map.value(kKeyCommand).toString();
    spec.workingDirectory = map.value(kKeyDir).toString();
    spec.useShell = map.value(kKeyShell).toBool();
    return spec;
}

namespace CommandHistory {

QList<CommandSpec> pushed(QList<CommandSpec> history, const CommandSpec& spec)
{
    if (spec.commandLine.trimmed().isEmpty())
        return history;
    history.removeAll(spec);
    history.prepend(spec);
    while (history.size() > kMax)
        history.removeLast();
    return history;
}

QStringList toStrings(const QList<CommandSpec>& history)
{
    QStringList out;
    out.reserve(history.size());
    for (const CommandSpec& spec : history)
        out << QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(spec.toVariant()))
                                     .toJson(QJsonDocument::Compact));
    return out;
}

QList<CommandSpec> fromStrings(const QStringList& stored)
{
    QList<CommandSpec> out;
    for (const QString& s : stored) {
        const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8());
        if (!doc.isObject())
            continue;
        const CommandSpec spec = CommandSpec::fromVariant(doc.object().toVariantMap());
        if (spec.commandLine.trimmed().isEmpty() || out.contains(spec))
            continue;
        out << spec;
        if (out.size() >= kMax)
            break;
    }
    return out;
}

} // namespace CommandHistory

// ---------------------------------------------------------------------------
// CommandSpooler
// ---------------------------------------------------------------------------

CommandSpooler::CommandSpooler(CommandSpec spec, std::shared_ptr<SpoolItem> spool, QObject* parent)
    : QObject(parent)
    , m_spec(std::move(spec))
    , m_spool(std::move(spool))
{
    m_process = new QProcess(this);
    // stdout и stderr — одним потоком, как в терминале (см. заголовок).
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    // Ввода у команды нет: программа, ждущая stdin, получит EOF, а не повиснет.
    m_process->setStandardInputFile(QProcess::nullDevice());
#ifndef Q_OS_WIN
    // Лидер своей группы процессов: stop() останавливает и детей shell.
    // В дочернем процессе между fork и exec — только async-signal-safe вызовы.
    m_process->setChildProcessModifier([]() { ::setsid(); });
#endif
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CommandSpooler::onReadyRead);
    connect(m_process, &QProcess::started, this, &CommandSpooler::onStarted);
    connect(m_process, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus status) { onFinished(code, int(status)); });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) { onErrorOccurred(int(error)); });

    m_graceTimer = new QTimer(this);
    m_graceTimer->setSingleShot(true);
    connect(m_graceTimer, &QTimer::timeout, this, [this]() {
        forceKill();
        // Лидер вышел раньше, ждали остальных членов группы (release).
        if (m_released && m_process->state() == QProcess::NotRunning)
            deleteLater();
    });

    m_notifyTimer = new QTimer(this);
    m_notifyTimer->setSingleShot(true);
    m_notifyTimer->setInterval(250);
    connect(m_notifyTimer, &QTimer::timeout, this, [this]() {
        if (m_bytes != m_notifiedBytes) {
            m_notifiedBytes = m_bytes;
            emit bytesAppended(m_bytes);
            m_notifyTimer->start(); // следующий — не раньше интервала
        }
    });
}

CommandSpooler::~CommandSpooler()
{
    // Сигналы процесса больше не нужны: onFinished звал бы deleteLater и
    // слал stateChanged посреди деструктора.
    m_process->disconnect(this);
    if (m_process->state() != QProcess::NotRunning) {
        politeTerminate();
        if (!m_process->waitForFinished(qMin(m_graceMs, 500))) {
            forceKill();
            m_process->waitForFinished(2000);
        }
    }
    // Лидер вышел, а члены его группы (дети shell, фоновые) живы — владельца
    // больше нет, сиротами их не оставлять.
    forceKill();
    closeOutput();
#ifdef Q_OS_WIN
    // KILL_ON_JOB_CLOSE: дети, пережившие лидера, уходят вместе с job.
    if (m_job)
        CloseHandle(static_cast<HANDLE>(m_job));
#endif
}

QString CommandSpooler::spoolFilePath() const
{
    return m_spool ? m_spool->path() : QString();
}

bool CommandSpooler::start()
{
    if (isRunning() || m_released)
        return false;
    auto fail = [this](const QString& why) {
        m_error = why;
        setState(State::FailedToStart);
        return false;
    };

    CommandSpec::Launch launch;
    QString error;
    if (!m_spec.resolve(&launch, &error))
        return fail(error);
    if (!m_spool)
        return fail(tr("No place for the output file."));
    if (!m_spec.workingDirectory.isEmpty() && !QFileInfo(m_spec.workingDirectory).isDir())
        return fail(tr("The working directory %1 does not exist.")
                        .arg(QDir::toNativeSeparators(m_spec.workingDirectory)));

    // Первый запуск — файл заново, перезапуск — дописывает (строки остаются).
    m_out = std::make_unique<QFile>(m_spool->path());
    const QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Unbuffered
        | (m_runs == 0 ? QIODevice::Truncate : QIODevice::Append);
    if (!m_out->open(mode)) {
        const QString why = m_out->errorString();
        m_out.reset();
        return fail(tr("Cannot write the output file: %1").arg(why));
    }

#ifdef Q_OS_WIN
    if (!m_job) {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            m_job = job;
        }
    }
#endif

    // Пауза прошлой остановки (ждали группу) к новому процессу не относится.
    m_graceTimer->stop();
    ++m_runs;
    m_error.clear();
    m_exitCode = 0;
    m_crashed = false;
    m_stopRequested = false;
    m_pid = 0;
    m_process->setWorkingDirectory(m_spec.workingDirectory);
    m_process->setProgram(launch.program);
    m_process->setArguments(launch.arguments);
#ifdef Q_OS_WIN
    m_process->setNativeArguments(launch.nativeArguments);
#endif
    setState(State::Running);
    m_process->start(QIODevice::ReadOnly);
    return true;
}

void CommandSpooler::onStarted()
{
    m_pid = m_process->processId();
#ifdef Q_OS_WIN
    // Окно гонки (ребёнок успел породить внука до назначения) мало, а
    // приостановленный старт QProcess не даёт: дескриптор потока скрыт.
    if (m_job && m_pid > 0) {
        if (HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(m_pid))) {
            AssignProcessToJobObject(static_cast<HANDLE>(m_job), h);
            CloseHandle(h);
        }
    }
#endif
    emit stateChanged(m_state); // PID стал известен
}

void CommandSpooler::onReadyRead()
{
    const QByteArray data = m_process->readAllStandardOutput();
    if (data.isEmpty() || !m_out)
        return;
    const qint64 written = m_out->write(data);
    if (written > 0)
        m_bytes += written;
    if (written != data.size() && m_error.isEmpty()) {
        // Диск полон или файл недоступен: вывод теряется — видно на плашке.
        m_error = tr("Cannot write the output file: %1").arg(m_out->errorString());
        emit stateChanged(m_state);
    }
    notifyAppended(false);
}

void CommandSpooler::notifyAppended(bool now)
{
    // Первая порция — сразу, следующие — не чаще интервала таймера; хвост
    // серии придёт по таймеру.
    if (now || !m_notifyTimer->isActive()) {
        m_notifyTimer->stop();
        if (m_bytes != m_notifiedBytes) {
            m_notifiedBytes = m_bytes;
            emit bytesAppended(m_bytes);
        }
        if (!now)
            m_notifyTimer->start();
    }
}

void CommandSpooler::onFinished(int exitCode, int exitStatus)
{
    onReadyRead(); // остаток канала
    m_exitCode = exitCode;
    m_crashed = exitStatus == int(QProcess::CrashExit);
    closeOutput();
    notifyAppended(true);
    // Остановка: лидер вышел, но члены группы могут жить (SIGTERM
    // игнорируют, `cmd &`) — таймер вежливой паузы добьёт их SIGKILL.
    const bool waitGroup = m_stopRequested && m_graceTimer->isActive() && groupAlive();
    if (!waitGroup)
        m_graceTimer->stop();
    setState(State::Finished);
    if (m_released && !waitGroup)
        deleteLater();
}

void CommandSpooler::onErrorOccurred(int error)
{
    // Остальные ошибки (Crashed, ReadError) закончатся finished.
    if (error != int(QProcess::FailedToStart))
        return;
    m_graceTimer->stop();
    m_pid = 0; // не запустился — группы нет
    m_error = m_process->errorString();
    closeOutput();
    setState(State::FailedToStart);
    if (m_released)
        deleteLater();
}

void CommandSpooler::stop()
{
    if (!isRunning())
        return;
    m_stopRequested = true;
    if (m_state != State::Stopping) {
        setState(State::Stopping);
        politeTerminate();
        m_graceTimer->start(m_graceMs);
    }
}

void CommandSpooler::release()
{
    if (m_released)
        return;
    m_released = true;
    // Родитель-вкладка уходит; приложение удалит спулер, если программа
    // закончится раньше процесса (деструктор добьёт его синхронно).
    setParent(QCoreApplication::instance());
    if (isRunning() && m_process->state() != QProcess::NotRunning) {
        stop();
    } else if (groupAlive()) {
        // Лидер вышел сам, а его группа жива: вежливо, затем принудительно.
        m_stopRequested = true;
        politeTerminate();
        m_graceTimer->start(m_graceMs);
    } else {
        deleteLater();
    }
}

bool CommandSpooler::groupAlive() const
{
#ifdef Q_OS_WIN
    if (!m_job)
        return false;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
    return QueryInformationJobObject(static_cast<HANDLE>(m_job), JobObjectBasicAccountingInformation,
                                     &info, sizeof(info), nullptr)
        && info.ActiveProcesses > 0;
#else
    // Пока в группе есть хоть один процесс, её номер (PID лидера) занят и
    // чужому процессу не достанется — сигнал группе безопасен.
    return m_pid > 0 && ::kill(-pid_t(m_pid), 0) == 0;
#endif
}

void CommandSpooler::politeTerminate()
{
#ifdef Q_OS_WIN
    if (m_process->state() != QProcess::NotRunning)
        m_process->terminate();
#else
    // Группе: лидер (setsid) и его дети. Лидер ещё не подобран
    // (state != NotRunning) — его PID никому не отдан; подобран — группа
    // жива, только пока в ней кто-то есть (groupAlive).
    const bool leaderRunning = m_process->state() != QProcess::NotRunning;
    if ((leaderRunning || groupAlive()) && m_pid > 0 && ::kill(-pid_t(m_pid), SIGTERM) == 0)
        return;
    if (leaderRunning)
        m_process->terminate();
#endif
}

void CommandSpooler::forceKill()
{
    const bool leaderRunning = m_process->state() != QProcess::NotRunning;
#ifdef Q_OS_WIN
    if (m_job && (leaderRunning || groupAlive()))
        TerminateJobObject(static_cast<HANDLE>(m_job), 1);
    if (leaderRunning)
        m_process->kill();
#else
    if ((leaderRunning || groupAlive()) && m_pid > 0 && ::kill(-pid_t(m_pid), SIGKILL) == 0)
        return;
    if (leaderRunning)
        m_process->kill();
#endif
}

void CommandSpooler::closeOutput()
{
    // Закрыть до того, как SpoolItem удалит файл (Windows: открытый файл не
    // удаляется).
    if (m_out) {
        m_out->close();
        m_out.reset();
    }
}

void CommandSpooler::setState(State state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged(state);
}

QString CommandSpooler::statusText() const
{
    switch (m_state) {
    case State::NotStarted:
        return tr("Not started");
    case State::Running:
        if (!m_error.isEmpty())
            return tr("Running (PID %1), output lost: %2").arg(m_pid).arg(m_error);
        return m_pid > 0 ? tr("Running (PID %1)").arg(m_pid) : tr("Starting");
    case State::Stopping:
        return tr("Stopping (PID %1)").arg(m_pid);
    case State::Finished:
        if (m_stopRequested)
            return tr("Stopped");
        if (m_crashed)
            return tr("Process crashed");
        return tr("Process exited with code %1").arg(m_exitCode);
    case State::FailedToStart:
        return tr("Could not start: %1").arg(m_error);
    }
    return {};
}
