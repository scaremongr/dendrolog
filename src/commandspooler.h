#ifndef COMMANDSPOOLER_H
#define COMMANDSPOOLER_H

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>

class QFile;
class QProcess;
class QTimer;
class SpoolItem;

// ============================================================================
// Вывод команды как растущий лог (F-7).
//
// CommandSpec — что запустить: строка команды, рабочий каталог и способ
// разбора строки. Два способа, выбор виден в диалоге:
//   • без shell (по умолчанию): строка делится на программу и аргументы по
//     пробелам, двойные кавычки объединяют (QProcess::splitCommand); ни
//     конвейеров, ни подстановок, ни перенаправлений — запускается ровно одна
//     программа;
//   • через shell (явная галочка): `/bin/sh -c <строка>`, на Windows
//     `cmd.exe /d /s /c "<строка>"` — для `a | grep b`, `2>&1`, переменных.
//
// CommandSpooler — процесс и его спул. Вывод процесса (stdout и stderr одним
// потоком, как в терминале: `kubectl`, `adb` и многие демоны пишут журнал в
// stderr) копируется в файл SpoolItem (`<n>-stream-<имя>.log` в каталоге
// экземпляра SpoolDir), вкладка читает его как растущий лог — тем же путём,
// что спул stdin. Через канал, а не перенаправлением в файл силами ОС:
// упади программа, у процесса закроется канал и он получит SIGPIPE /
// ERROR_BROKEN_PIPE на следующей записи, а не будет писать в файл вечно.
//
// Завершение «вежливо, затем принудительно»: stop() шлёт SIGTERM ГРУППЕ
// процессов (команда запускается лидером своей группы — setsid; так
// останавливаются и дети shell-конвейера), через gracePeriodMs — SIGKILL
// группе. На Windows вежливого сигнала консольному процессу нет: terminate()
// (WM_CLOSE — его слышат только оконные программы), затем TerminateJobObject
// — процесс с детьми живёт в job-объекте с KILL_ON_JOB_CLOSE, так что дерево
// не переживёт и падения программы.
//
// Время жизни: владелец (вкладка) при уходе зовёт release() — спулер
// останавливает процесс и удаляет себя сам, когда тот завершится. Деструктор
// завершает живой процесс синхронно (короткое ожидание) — так уходят
// спулеры при выходе программы. Только GUI-поток. Без виджетов (ядро).
// ============================================================================
struct CommandSpec {
    QString commandLine;
    QString workingDirectory; // пусто — текущий каталог программы
    bool    useShell = false;

    bool operator==(const CommandSpec& o) const
    {
        return commandLine == o.commandLine && workingDirectory == o.workingDirectory
            && useShell == o.useShell;
    }

    // Программа и аргументы для QProcess. false — строка пуста или без shell
    // не разбирается (незакрытая кавычка); error — почему (для диалога).
    struct Launch {
        QString program;
        QStringList arguments;
        QString nativeArguments; // Windows, cmd.exe: строка целиком
    };
    bool resolve(Launch* launch, QString* error = nullptr) const;

    // Короткое имя для вкладки: имя программы без каталога и `.exe`
    // (`kubectl`); через shell — первое слово строки.
    QString shortLabel() const;

    QVariantMap toVariant() const;
    static CommandSpec fromVariant(const QVariant& value);
};

// Недавние команды: свежая первой, без повторов (повтор поднимается наверх),
// не больше kMax; пустая строка не запоминается.
namespace CommandHistory {
constexpr int kMax = 20;
QList<CommandSpec> pushed(QList<CommandSpec> history, const CommandSpec& spec);
QStringList toStrings(const QList<CommandSpec>& history);     // JSON на запись в ini
QList<CommandSpec> fromStrings(const QStringList& stored);    // битые пропускаются
} // namespace CommandHistory

class CommandSpooler : public QObject {
    Q_OBJECT
public:
    enum class State {
        NotStarted,
        Running,
        Stopping,      // stop(): ждём выхода после вежливого сигнала
        Finished,      // процесс вышел (сам или после stop)
        FailedToStart, // не запустился: errorString()
    };
    Q_ENUM(State)

    // spool — файл, в который пойдёт вывод (SpoolDir::createStreamFile).
    CommandSpooler(CommandSpec spec, std::shared_ptr<SpoolItem> spool, QObject* parent = nullptr);
    ~CommandSpooler() override;

    const CommandSpec& spec() const { return m_spec; }
    std::shared_ptr<SpoolItem> spool() const { return m_spool; }
    QString spoolFilePath() const;

    // Запустить. Первый запуск создаёт файл заново, перезапуск (после выхода)
    // дописывает в тот же файл: строки прежнего запуска остаются. false —
    // процесс уже идёт или файл/строка не годятся (State::FailedToStart,
    // errorString). Сам запуск асинхронный: не нашлась программа —
    // FailedToStart придёт сигналом.
    bool start();
    // Вежливо остановить, через gracePeriodMs — принудительно. Асинхронно.
    void stop();
    // Владелец уходит: остановить и удалить себя после выхода процесса.
    // После вызова объект владельцу не принадлежит (родитель — приложение).
    void release();

    void setGracePeriodMs(int ms) { m_graceMs = ms; }
    int gracePeriodMs() const { return m_graceMs; }

    State state() const { return m_state; }
    bool isRunning() const { return m_state == State::Running || m_state == State::Stopping; }
    // Код возврата последнего запуска; crashed — завершён сигналом (в том
    // числе нашим stop) или аварийно.
    int exitCode() const { return m_exitCode; }
    bool crashed() const { return m_crashed; }
    // Остановлен по stop()/release(), а не вышел сам.
    bool stoppedByUser() const { return m_stopRequested; }
    QString errorString() const { return m_error; }
    qint64 processId() const { return m_pid; }
    qint64 bytesWritten() const { return m_bytes; }
    int runCount() const { return m_runs; }

    // Строка состояния для людей: «Running (PID 123)», «Exited with code 1»,
    // «Stopped», «Could not start: …».
    QString statusText() const;

signals:
    // В файл дописаны байты (не чаще ~4 раз в секунду; последний — на выходе).
    void bytesAppended(qint64 total);
    void stateChanged(CommandSpooler::State state);

private:
    void onReadyRead();
    void onStarted();
    void onFinished(int exitCode, int exitStatus);
    void onErrorOccurred(int error);
    void forceKill();
    void politeTerminate();
    // Жива ли группа процессов команды (лидер или его дети); Windows — job.
    bool groupAlive() const;
    void notifyAppended(bool now);
    void closeOutput();
    void setState(State state);

    CommandSpec m_spec;
    std::shared_ptr<SpoolItem> m_spool;
    QProcess* m_process = nullptr;
    std::unique_ptr<QFile> m_out;
    QTimer* m_graceTimer = nullptr;
    QTimer* m_notifyTimer = nullptr;
    State m_state = State::NotStarted;
    QString m_error;
    qint64 m_pid = 0;
    qint64 m_bytes = 0;
    qint64 m_notifiedBytes = -1;
    int m_exitCode = 0;
    bool m_crashed = false;
    bool m_stopRequested = false;
    bool m_released = false;
    int m_runs = 0;
    int m_graceMs = 3000;
#ifdef Q_OS_WIN
    void* m_job = nullptr; // HANDLE job-объекта
#endif
};

#endif // COMMANDSPOOLER_H
