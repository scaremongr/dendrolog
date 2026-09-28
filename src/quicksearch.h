#ifndef QUICKSEARCH_H
#define QUICKSEARCH_H

#include "logscan.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

class LogModel;
class QTimer;

// ============================================================================
// QuickSearch — быстрый поиск (F3 / Shift+F3) ближайшего вхождения текста в
// видимых строках модели, в фоне и с отменой.
//
// Синхронный LogModel::findNext/PreviousOccurrence на индексном бэкенде —
// это скан диска на GUI-потоке: на гигантском файле без совпадений окно
// замирало на всё время чтения. Здесь скан идёт по снапшоту видимых строк
// (LogScanSnapshot) в пуле, с той же семантикой, что у синхронного поиска.
//
// Модель за время поиска может измениться:
//   • вставки строк (дозапись хвоста, слияние) — найденная строка сдвигается
//     на число вставленных перед ней, finished() отдаёт номер в ТЕКУЩЕЙ модели;
//   • reset, удаление или перестановка строк — номер из снапшота уже ничего не
//     значит: вместо finished() приходит invalidated(), и владелец начинает
//     поиск заново от текущей строки.
// ============================================================================
class QuickSearch : public QObject {
    Q_OBJECT

public:
    struct Insertion {
        qint64 first = 0;
        qint64 count = 0;
    };

    explicit QuickSearch(QObject* parent = nullptr);
    ~QuickSearch() override;

    // Новый поиск (прежний отменяется). startRow — текущая строка view или -1;
    // семантика startRow/wrapAround — как у LogModel::findNext/Previous.
    void start(LogModel* model, const QString& text, Qt::CaseSensitivity cs,
               bool forward, int startRow, bool wrapAround = true);
    // Отменить поиск: сигналов по нему больше не будет.
    void cancel();
    bool isRunning() const { return m_running; }

    // Сам скан — синхронно по снапшоту (воркер; тесты сверяют с моделью).
    // -1 — не найдено или отменено.
    static qint64 scan(const LogScanSnapshot& snapshot, const QString& text,
                       Qt::CaseSensitivity cs, bool forward, qint64 startRow,
                       bool wrapAround, const std::atomic_bool* cancel = nullptr,
                       std::atomic<int>* progress = nullptr);
    // Номер строки снапшота → номер в модели после вставок (по порядку).
    static qint64 mapRow(qint64 row, const QVector<Insertion>& insertions);

signals:
    void progress(int percent);
    // Найденная строка текущей модели или -1.
    void finished(int row);
    // Модель перестроилась, результат неприменим — искать заново.
    void invalidated();

private:
    void detachModel();

    QPointer<LogModel> m_model;
    QVector<QMetaObject::Connection> m_connections;
    QVector<Insertion> m_insertions;
    bool m_valid = true;
    bool m_running = false;
    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress;
    QTimer* m_progressTimer = nullptr;
};

#endif // QUICKSEARCH_H
