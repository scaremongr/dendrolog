#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <atomic>
#include <memory>

class LogModel;
class QTimer;
struct LogFile;

// ============================================================================
// FileShownCounter — сколько ПОКАЗАННЫХ строк у каждого файла вкладки (U-23,
// часть 4): чип легенды пишет «показано / всего», когда числа различаются
// (действует любой фильтр вкладки).
//
// Считается в фоне по снапшоту видимых строк (LogStore::scanSnapshot,
// только метаданные — текст не читается), по образцу ScrollbarMarks: события
// модели (вставка, удаление, reset, конец фильтра) ставят флаг и дебаунс,
// поток батчей даёт единицы запусков; reset посреди расчёта — результат
// отброшен по поколению; неактивный счётчик (легенды не видно) не считает,
// пока его не включат. Пока идёт расчёт, прежние числа остаются.
// Ядро: без виджетов.
// ============================================================================
class FileShownCounter : public QObject {
    Q_OBJECT
public:
    explicit FileShownCounter(QObject* parent = nullptr);
    ~FileShownCounter() override;

    void setModel(LogModel* model);
    // Считать ли (легенда видна). Включение с устаревшими числами — расчёт.
    void setActive(bool active);
    bool isActive() const { return m_active; }

    // Показанных строк файла; файла нет в ответе — 0.
    qint64 shown(const LogFile* file) const { return m_counts.value(file, 0); }
    const QHash<const LogFile*, qint64>& counts() const { return m_counts; }
    // Числа соответствуют модели: не ждут расчёта и он не идёт.
    bool isUpToDate() const;
    // Сколько раз запускался расчёт (тестам: поток батчей — единицы).
    int launchCount() const { return m_launches; }

signals:
    void countsChanged();

private:
    void markDirty(bool reset);
    void start();

    QPointer<LogModel> m_model;
    QList<QMetaObject::Connection> m_connections;
    QTimer* m_debounce = nullptr;
    bool m_active = false;
    bool m_dirty = false;
    bool m_running = false;
    int m_generation = 0;
    int m_launches = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QHash<const LogFile*, qint64> m_counts;
};
