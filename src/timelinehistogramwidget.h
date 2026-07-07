#ifndef TIMELINEHISTOGRAMWIDGET_H
#define TIMELINEHISTOGRAMWIDGET_H

#include <QDateTime>
#include <QPixmap>
#include <QPoint>
#include <QVector>
#include <QWidget>

class LogModel;
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
// Данные берутся из filteredEntries() модели активной вкладки, поэтому
// гистограмма всегда соответствует видимому списку. Список отсортирован
// по времени (строки без метки — в конце): min/max берутся с краёв, а
// получатель клика может искать строку через lower_bound.
// Сборка: один проход по записям → kBins корзин + префикс-суммы
// (O(1) на пиксельную колонку при отрисовке, независимо от ширины).
// Перестройка дебаунсится таймером (rowsInserted при слиянии батчей
// приходит сериями); статичная картинка кэшируется в QPixmap, при
// ховере/смене маркера перерисовываются только оверлеи.
// ============================================================

class TimelineHistogramWidget : public QWidget
{
    Q_OBJECT
public:
    explicit TimelineHistogramWidget(QWidget* parent = nullptr);

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
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
    qint64 timeAtX(int x, const Layout& l) const; // центр колонки, мс epoch

    LogModel* m_model = nullptr;
    QTimer*   m_rebuildTimer = nullptr;

    static constexpr int kBins = 4096;

    // Префикс-суммы счётчиков по корзинам (размер kBins + 1): количество
    // в диапазоне корзин — разность двух элементов, последний элемент —
    // итог по всему диапазону (для подписей дорожек).
    QVector<quint32> m_totalPrefix, m_warnPrefix, m_errorPrefix, m_fatalPrefix;
    qint64 m_tMin = 0, m_tMax = 0;   // мс epoch; валидно при m_hasData
    bool   m_hasData = false;

    QDateTime m_currentTime;         // маркер текущей строки списка
    QPoint    m_hoverPos{-1, -1};    // позиция мыши; (-1,-1) — вне виджета

    QPixmap m_cache;                 // отрисованная статичная часть
    bool    m_cacheDirty = true;
};

#endif // TIMELINEHISTOGRAMWIDGET_H
