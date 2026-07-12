#ifndef TIMELINEHISTOGRAMWIDGET_H
#define TIMELINEHISTOGRAMWIDGET_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPoint>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>

class LogModel;
class LogScanSnapshot;
class QTimer;
enum class LogLevel;

// ============================================================
// TimelineHistogramWidget — содержимое дока «Timeline».
//
// Плотная гистограмма распределения записей по времени в две дорожки:
//   • верхняя — общая плотность логических записей (нейтральный серый);
//   • нижняя — только Warn/Error/Fatal, стопкой цветами уровней.
//     Отдельная дорожка, а не слой поверх общей: у ошибок пики обычно
//     на порядки ниже общего фона и на общей шкале были бы невидимы.
// Общая ось времени снизу. Ховер — перекрестие и плашка с точными
// счётчиками колонки; клик — сигнал timeClicked (клик по дорожке
// ошибок просит перейти к ближайшей ошибке). setCurrentTime() рисует
// маркер позиции текущей строки списка.
//
// Данные берутся из видимых строк модели активной вкладки (обход
// метаданных scanSnapshot(true), без текста), поэтому гистограмма всегда
// соответствует видимому списку. Список отсортирован по времени (строки
// без метки — в конце): min/max берутся с краёв, а получатель клика
// ищет строку через LogModel::firstVisibleRowAtOrAfter.
// Сборка: один проход по записям → kBins корзин + префикс-суммы
// (O(1) на пиксельную колонку при отрисовке, независимо от ширины).
// Перестройка дебаунсится таймером (rowsInserted при слиянии батчей
// приходит сериями); статичная картинка кэшируется в QPixmap, при
// ховере/смене маркера перерисовываются только оверлеи.
//
// Большие логи: проход по метаданным миллионов строк стоит сотни мс,
// поэтому выше порога биннинг уезжает в QtConcurrent-воркер поверх
// LogScanSnapshot (поколение отсекает устаревшие результаты, cancel
// останавливает воркер); малые модели считаются синхронно, как раньше.
// ============================================================

class TimelineHistogramWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineHistogramWidget(QWidget* parent = nullptr);
    ~TimelineHistogramWidget() override;

    // Модель активной вкладки; nullptr — вкладок нет. Виджет подписывается
    // на изменения данных/фильтра и перестраивает гистограмму сам.
    void setModel(LogModel* model);

    // Позиция текущей строки списка на шкале (вертикальный маркер).
    // Невалидное время скрывает маркер.
    void setCurrentTime(const QDateTime& time);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // Клик по гистограмме. preferErrors — клик пришёлся в дорожку ошибок:
    // получатель должен перейти к ближайшей записи Warn/Error/Fatal,
    // а не просто к ближайшей по времени строке.
    void timeClicked(const QDateTime& time, bool preferErrors);

    // Протяжка левой кнопкой — выделен временной интервал (from < to,
    // границы — края крайних колонок). Получатель применяет его к фильтру
    // по времени. Короткое движение (< kDragThresholdPx) остаётся кликом.
    // Ctrl+колесо шлёт тот же сигнал: новые границы вокруг курсора-якоря
    // (тики накапливаются и коммитятся одним сигналом после паузы).
    void timeRangeSelected(const QDateTime& from, const QDateTime& to);

    // Контекстное меню «Reset time filter»: снять фильтр по времени
    // (отменить зум), не открывая док Time Filter.
    void resetRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // Геометрия дорожек для текущего размера виджета.
    struct Layout {
        QRect label1, plot1;   // подпись и график «все записи»
        QRect label2, plot2;   // подпись-легенда и график Warn/Error/Fatal
        QRect axis;            // полоса тиков и подписей оси времени
        int   plotX = 0, plotW = 1;
    };
    Layout layoutForSize(const QSize& size) const;

    void scheduleRebuild();
    void rebuildHistogram();

    // Результат биннинга: префикс-суммы корзин + границы шкалы.
    struct HistogramData {
        QVector<quint32> totalPrefix, warnPrefix, errorPrefix, fatalPrefix;
        qint64 tMin = 0, tMax = 0;
        bool   hasData = false;
    };
    // Чистый расчёт по снапшоту — безопасен в воркере. filter*Ms — активный
    // фильтр по времени (расширяет шкалу); cancel — кооперативная отмена
    // (обрезанный результат отбрасывается проверкой поколения).
    static HistogramData buildHistogram(const LogScanSnapshot& snap,
                                        qint64 filterMinMs, qint64 filterMaxMs,
                                        bool filterValid,
                                        const std::atomic_bool* cancel);
    void applyHistogram(HistogramData&& h);
    void startRebuildJob(qint64 filterMinMs, qint64 filterMaxMs, bool filterValid);
    // Обесценить полётный джоб (поколение++, cancel-флаг воркеру).
    void cancelRebuildJob();

    // Диапазон корзин [b0, b1) пиксельной колонки x (0..plotW-1).
    void columnBins(int x, int plotW, int& b0, int& b1) const;
    static quint32 rangeCount(const QVector<quint32>& prefix, int b0, int b1)
    {
        return prefix[b1] - prefix[b0];
    }

    // Цвет столбиков Warn/Error/Fatal с учётом светлой/тёмной палитры:
    // насыщенные logWarn/logError на светлом фоне не читаются в 1px-колонках,
    // а тёмные badge-варианты пропадают на тёмном.
    QColor levelBarColor(LogLevel level) const;

    void renderCache();     // статичная часть: фон, дорожки, подписи, ось
    void paintAxis(QPainter& p, const Layout& l) const;
    void paintHoverOverlay(QPainter& p, const Layout& l) const;
    void paintDragOverlay(QPainter& p, const Layout& l) const;
    void paintZoomOverlay(QPainter& p, const Layout& l) const;
    // Плашка-подсказка «от – до (длительность)» по центру centerX.
    void drawInfoBox(QPainter& p, const Layout& l, const QString& text, int centerX) const;
    qint64 timeAtX(int x, const Layout& l) const; // центр колонки, мс epoch

    LogModel* m_model = nullptr;
    QTimer*   m_rebuildTimer = nullptr;
    // Самотроттлинг асинхронных пересборок: пока данные растут (загрузка),
    // джобы стартуют не чаще раза в полторы секунды — иначе воркер сканирует
    // метаданные непрерывно.
    QElapsedTimer m_lastRebuild;

    // Асинхронный биннинг: поколение отсекает устаревшие результаты (смена
    // вкладки/новый джоб), again — запрос пришёл во время полёта
    // (пересборка перезапустится по завершении текущей).
    int  m_rebuildGeneration = 0;
    bool m_rebuildJobActive = false;
    bool m_rebuildAgain = false;
    std::shared_ptr<std::atomic_bool> m_rebuildCancel;

    static constexpr int kBins = 4096;

    // Префикс-суммы счётчиков по корзинам (размер kBins + 1): количество
    // в диапазоне корзин — разность двух элементов, последний элемент —
    // итог по всему диапазону (для подписей дорожек).
    QVector<quint32> m_totalPrefix, m_warnPrefix, m_errorPrefix, m_fatalPrefix;
    qint64 m_tMin = 0, m_tMax = 0;   // мс epoch; валидно при m_hasData
    bool   m_hasData = false;

    QDateTime m_currentTime;         // маркер текущей строки списка
    QPoint    m_hoverPos{-1, -1};    // позиция мыши; (-1,-1) — вне виджета

    // Протяжка выделения интервала. m_pressPos.x() < 0 — кнопка не зажата;
    // выделением протяжка становится после сдвига на kDragThresholdPx.
    static constexpr int kDragThresholdPx = 4;
    QPoint m_pressPos{-1, -1};
    int    m_dragCurX = -1;
    bool   m_dragging = false;
    bool   m_suppressContextMenu = false; // ПКМ отменила протяжку — меню не показывать

    // Ctrl+колесо: накопленный, ещё не применённый диапазон зума. Тики
    // мультипликативно сужают/расширяют pending-границы вокруг курсора,
    // m_zoomCommitTimer коммитит одним timeRangeSelected после паузы —
    // без перефильтровки модели на каждый щелчок колеса.
    QTimer* m_zoomCommitTimer = nullptr;
    bool    m_zoomPending = false;
    qint64  m_pendingMin = 0, m_pendingMax = 0;

    QPixmap m_cache;                 // отрисованная статичная часть
    bool    m_cacheDirty = true;
};

#endif // TIMELINEHISTOGRAMWIDGET_H
