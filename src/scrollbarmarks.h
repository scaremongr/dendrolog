#ifndef SCROLLBARMARKS_H
#define SCROLLBARMARKS_H

#include "logscan.h"

#include <QObject>
#include <QPointer>
#include <QVector>
#include <array>
#include <atomic>
#include <memory>

class LogModel;
class QTimer;
class SearchResultsController;

// ============================================================================
// Отметки на полосе прокрутки (R-6, docs/design/R-6.md) — расчёт без виджетов.
// Рисует их ScrollbarMarksOverlay поверх скроллбара LogListView.
//
// Что отмечается — ВИДИМЫЕ строки вкладки четырёх категорий:
//   • Error — первая показанная строка записи уровня Error/Fatal, Warning —
//     уровня Warn (как цель F8: продолжения записи наследуют её уровень);
//   • Search — строки выдачи панели Search, пока её поиск запущен и эта
//     вкладка — её источник;
//   • Bookmark — видимые живые закладки (BookmarkStore::visibleRows).
//
// Набор — номера строк, прорежённые до первой и последней отмеченной строки в
// каждой из kFineBuckets равных корзин (см. ScrollbarMarkSet). Проекция строк
// в пиксели — дело view (геометрия строк у него): trackPosition.
//
// Жизнь расчёта (ScrollbarMarks):
//   • rowsInserted / modelFiltered / dataChanged — только флаг «устарело» и
//     дебаунс, O(1) на батч (инвариант 3);
//   • расчёт — в пуле по снапшоту видимых строк, с поколением и cancel-флагом;
//     пока он идёт, следующий не стартует;
//   • modelReset / rowsRemoved / layoutChanged / rowsMoved отменяют идущий
//     расчёт: его результат отбрасывается по поколению. Вставки — нет: на
//     растущем логе расчёт иначе не закончился бы ни разу; результат
//     публикуется, следующий расчёт взводится сразу;
//   • закладки — на GUI-потоке, при публикации и на BookmarkStore::changed;
//   • setEnabled(false) (view скрыт) — расчёты не стартуют, изменения копятся;
//   • выдача поиска, которая ещё считается, в расчёт не берётся. Если она
//     перестраивается — отметок поиска нет до её готовности; если только
//     дописывается (дозапись источника) — переносятся прежние отметки.
// Только GUI-поток.
// ============================================================================

struct ScrollbarMarkSet {
    enum Category { Error, Warning, Search, Bookmark, CategoryCount };

    // Строк в снапшоте, по которому считан набор.
    qint64 rowCount = 0;
    // Номера видимых строк по возрастанию; для Error/Warning/Search прорежены
    // (первая и последняя в каждой корзине), Bookmark — все.
    std::array<QVector<qint64>, CategoryCount> rows;

    bool isEmpty() const
    {
        for (const QVector<qint64>& r : rows)
            if (!r.isEmpty())
                return false;
        return true;
    }
};

// Геометрия дорожки скроллбара для проекции строки в пиксель.
struct ScrollbarTrack {
    int maximum = 0;      // максимум скроллбара (минимум — 0)
    int pageStep = 1;     // шаг страницы скроллбара (= высота viewport в его шкале)
    int grooveLength = 0; // длина дорожки, px
    int sliderLength = 0; // длина ползунка, px
};

class ScrollbarMarks : public QObject {
    Q_OBJECT

public:
    // Число корзин прореживания: до него набор точный построчно, а при
    // uniform-высотах — попиксельно при любой длине дорожки короче него.
    static constexpr qint64 kFineBuckets = 65536;
    // Пауза между изменением модели и расчётом: батчи идут пачками.
    static constexpr int kDebounceMs = 150;

    explicit ScrollbarMarks(QObject* parent = nullptr);
    ~ScrollbarMarks() override;

    // Модель вкладки (nullptr — нет). Смена модели отменяет расчёт.
    void setModel(LogModel* model);
    LogModel* model() const;
    // Источник отметок поиска; учитывается, только когда его source() — эта
    // модель и поиск активен.
    void setSearch(SearchResultsController* search);
    // false — расчёты не запускаются (view скрыт); true — запустить, если
    // что-то устарело.
    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    // Текущий опубликованный набор (Bookmark — актуальные закладки).
    const ScrollbarMarkSet& marks() const { return m_marks; }
    // Растёт при каждой публикации набора (в т.ч. смене закладок).
    quint64 serial() const { return m_serial; }

    // Для тестов: всё посчитано (нет ни идущего расчёта, ни ждущего дебаунса,
    // ни устаревших данных) — при включённом расчёте.
    bool isUpToDate() const;
    bool isComputing() const { return m_running; }
    // Сколько расчётов запущено / отброшено как устаревшие (по поколению).
    int launchCount() const { return m_launches; }
    int discardedCount() const { return m_discarded; }
    // Запустить ждущий расчёт сразу, без дебаунса (тесты, показ вкладки).
    void flush();

    // Сам расчёт по снапшоту видимых строк (воркер; тесты сверяют с перебором).
    // search — снапшот выдачи поиска (подпоследовательность строк snapshot в
    // том же порядке) или пустой. Bookmark не заполняется. Отменён — пустой
    // набор и *cancelled = true.
    static ScrollbarMarkSet compute(const LogScanSnapshot& snapshot,
                                    const LogScanSnapshot& search,
                                    const std::atomic_bool* cancel = nullptr,
                                    bool* cancelled = nullptr);

    // Пиксель дорожки (от её начала) для значения скроллбара value (не
    // обрезанного по максимуму; дробное округляется, как округляет view,
    // ставя строку наверх). При value в [0, max] это верх ползунка — той же
    // формулой, что у QStyle::sliderPositionFromValue, — поэтому строка на
    // экране попадает в ползунок; за max (последняя страница) — продолжение
    // той же пропорции. Без прокрутки (max = 0) — доля от страницы. Результат
    // обрезан в [0, G − 1]; -1 — дорожки нет.
    static int trackPosition(double value, const ScrollbarTrack& track);

signals:
    // Набор опубликован (marks(), serial()).
    void marksChanged();

private:
    void connectModel();
    void disconnectModel();
    void markStale();
    void invalidate();
    void launch();
    void refreshBookmarks();
    bool searchApplies() const;
    LogModel* searchModel() const;

    QPointer<LogModel> m_model;
    QPointer<SearchResultsController> m_search;
    QVector<QMetaObject::Connection> m_modelConnections;
    // changed хранилища, чьи закладки показывает модель (своё или источника
    // выдачи Search, U-19).
    QMetaObject::Connection m_bookmarkConnection;
    void connectBookmarks();
    QVector<QMetaObject::Connection> m_searchConnections;
    ScrollbarMarkSet m_marks;
    quint64 m_serial = 0;
    bool m_enabled = true;
    bool m_stale = false;
    bool m_running = false;
    bool m_runningSearch = false; // идущий расчёт сопоставляет выдачу поиска
    // Идущий расчёт не взял выдачу, потому что она ещё считалась.
    bool m_runningSearchSkipped = false;
    // Опубликованные отметки поиска сопоставлены с выдачей, и с тех пор ни
    // вкладка, ни выдача не перестраивались (только дописывались): расчёт,
    // пропустивший выдачу на время её дозаписи, переносит их, а не гасит.
    bool m_searchCarry = false;
    int m_generation = 0;
    int m_launches = 0;
    int m_discarded = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QTimer* m_debounce = nullptr;
};

#endif // SCROLLBARMARKS_H
