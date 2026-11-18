#ifndef QUICKSEARCH_H
#define QUICKSEARCH_H

#include "logscan.h"
#include "textquery.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class LogModel;
struct LogFile;
class QTimer;

// ============================================================================
// QuickSearch — быстрый поиск (F3 / Shift+F3) ближайшего вхождения текста в
// видимых строках модели, в фоне и с отменой. Тем же механизмом ищется
// ближайшая строка с заданным уровнем (F8 / Shift+F8, startLevel): по
// метаданным снапшота, без чтения текста.
//
// Запрос — TextQuery (подстрока, слово целиком, регекс; см. textquery.h).
// Синхронный LogModel::findOccurrence на индексном бэкенде —
// это скан диска на GUI-потоке: на гигантском файле без совпадений окно
// замирало на всё время чтения. Здесь скан идёт по снапшоту видимых строк
// (LogScanSnapshot) в пуле — тем же LogScanSnapshot::findRowWithText, что и
// синхронный поиск.
//
// Модель за время поиска может измениться:
//   • вставки строк (дозапись хвоста, слияние) — найденная строка сдвигается
//     на число вставленных перед ней, finished() отдаёт номер в ТЕКУЩЕЙ модели;
//   • reset, удаление или перестановка строк — номер из снапшота уже ничего не
//     значит: вместо finished() приходит invalidated(), и владелец начинает
//     поиск заново от строки, выбранной пользователем (startRowFor).
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
    // семантика startRow/wrapAround — как у LogModel::findOccurrence.
    // Невалидный запрос (TextQuery::isValid) находит -1: проверять до старта.
    void start(LogModel* model, const TextQuery& query, bool forward, int startRow,
               bool wrapAround = true);
    // То же по уровню: ближайшая строка, с которой начинается запись уровня из
    // levels (LogScanSnapshot::findRowWithLevel).
    void startLevel(LogModel* model, LogLevelMask levels, bool forward, int startRow,
                    bool wrapAround = true);
    // Строка, от которой стартует поиск (startRow для start/scan), по выбору
    // пользователя: selectedRow — выбранная строка view (LogListView::
    // userSelectedRow) или -1; anchorEntryId/anchorFile — запись, выбранная
    // последней (-1 — нет). Выбранная строка видна — от неё. Запись скрыта
    // фильтром — от её МЕСТА среди видимых строк: вперёд первой проверяется
    // следующая за ним видимая строка, назад — предыдущая. Выбора нет —
    // вперёд с начала (-1), назад с конца (rowCount). Место — LogModel::
    // placeAmongVisible.
    static int startRowFor(const LogModel& model, int selectedRow, int anchorEntryId,
                           const LogFile* anchorFile, bool forward);
    // Отменить поиск: сигналов по нему больше не будет.
    void cancel();
    bool isRunning() const { return m_running; }
    // Строка, от которой шёл последний завершённый поиск, в номерах ТЕКУЩЕЙ
    // модели (пересчитана через те же вставки, что и найденная): по ней
    // владелец отличает «дальше» от «завернули через край». Действительна в
    // обработчике finished().
    int startRowNow() const { return m_startRowNow; }

    // Сам скан — синхронно по снапшоту (воркер; тесты сверяют с моделью).
    // -1 — не найдено или отменено.
    static qint64 scan(const LogScanSnapshot& snapshot, const TextQuery& query,
                       bool forward, qint64 startRow,
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
    using Scan = std::function<qint64(const LogScanSnapshot&, const std::atomic_bool*,
                                      std::atomic<int>*)>;
    void launch(LogModel* model, int startRow, Scan scan);
    void detachModel();

    QPointer<LogModel> m_model;
    QVector<QMetaObject::Connection> m_connections;
    QVector<Insertion> m_insertions;
    bool m_valid = true;
    bool m_running = false;
    int m_startRow = -1;
    int m_startRowNow = -1;
    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress;
    QTimer* m_progressTimer = nullptr;
};

#endif // QUICKSEARCH_H
