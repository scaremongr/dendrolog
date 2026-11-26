#ifndef COMMANDSTATUSBAR_H
#define COMMANDSTATUSBAR_H

#include <QFrame>
#include <QPointer>

class CommandSpooler;
class QLabel;
class QPushButton;

// ============================================================================
// CommandStatusBar — плашка над логом вкладки с выводом команды (F-7):
// команда, её состояние («Running (PID …)», «Process exited with code N»,
// «Could not start: …») и кнопки Stop / Restart. Только вид: процесс ведёт
// CommandSpooler, вкладка ставит плашку на каждый свой спулер.
// ============================================================================
class CommandStatusBar : public QFrame {
    Q_OBJECT
public:
    explicit CommandStatusBar(CommandSpooler* spooler, QWidget* parent = nullptr);

    CommandSpooler* spooler() const { return m_spooler; }
    QString text() const;

signals:
    // Перезапуск — через вкладку: она возвращает живой хвост (follow-tail).
    void restartRequested(CommandSpooler* spooler);

private:
    void refresh();

    QPointer<CommandSpooler> m_spooler;
    QLabel* m_text = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_restart = nullptr;
};

#endif // COMMANDSTATUSBAR_H
