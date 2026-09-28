#ifndef FIELDREEXTRACTION_H
#define FIELDREEXTRACTION_H

#include "logentry.h"

#include <QFutureWatcher>
#include <QObject>
#include <QVector>
#include <memory>

class LogPattern;

// ============================================================================
// FieldReextraction — фоновое переизвлечение полей резидентных записей при
// смене схемы, БЕЗ записи в сами записи.
//
// Воркеры кладут новые поля в буфер задания; apply() ставит их в записи
// одним проходом на GUI-потоке. Пока задание идёт, записи не меняются вовсе,
// поэтому фоновые фильтры, статистика и таймлайн, запущенные в это время,
// читают поля без гонки, а отмена оставляет записи в старой схеме целиком
// (а не наполовину в новой).
//
// Порядок у владельца: finished() → остановить (с ожиданием) фоновые фильтры,
// которые могут читать поля этих записей → apply() → перефильтровать.
// Старые спаны (по буферу на запись — миллионы мелких освобождений)
// освобождаются вне GUI-потока.
// ============================================================================
class FieldReextraction : public QObject {
    Q_OBJECT

public:
    FieldReextraction(QVector<std::shared_ptr<LogEntry>> entries,
                      std::shared_ptr<const LogPattern> pattern,
                      QObject* parent = nullptr);
    // Отменяет и дожидается воркеров.
    ~FieldReextraction() override;

    int entryCount() const { return int(m_jobs ? m_jobs->size() : 0); }
    void start();
    // Остановить воркеров и дождаться их; записи не тронуты, finished() не
    // придёт.
    void cancel();
    // Задание досчитано и ещё не применено.
    bool isReady() const;
    // Поставить новые поля в записи. Только GUI-поток, после finished() и
    // остановки фоновых фильтров, читающих поля этих записей.
    void apply();

signals:
    void progressRangeChanged(int minimum, int maximum);
    void progressValueChanged(int value);
    void finished();

private:
    struct Job {
        std::shared_ptr<LogEntry> entry;
        LogEntryFields fields;
    };
    std::shared_ptr<QVector<Job>> m_jobs;
    std::shared_ptr<const LogPattern> m_pattern;
    QFutureWatcher<void> m_watcher;
    bool m_started = false;
};

#endif // FIELDREEXTRACTION_H
