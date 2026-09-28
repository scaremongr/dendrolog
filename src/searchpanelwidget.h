#ifndef SEARCHPANELWIDGET_H
#define SEARCHPANELWIDGET_H

#include <QWidget>

class FilterPanelWidget;
class LogListView;
class QLabel;
class QSplitter;

// ============================================================================
// SearchPanelWidget — содержимое дока «Search»: запрос и результаты в одной
// панели.
//
//   запрос    — FilterPanelWidget в роли Search (правила, профили, Search/Clear);
//   результаты — подпись статуса и LogListView со строками-совпадениями
//               (модель — SearchResultsController, её ставит MainWindow).
//
// Раскладка следует форме панели: широкая (док снизу, плавающее окно) —
// запрос слева, результаты справа; узкая (док сбоку) — друг под другом,
// иначе строкам лога в результатах не хватило бы ширины. Переключение с
// гистерезисом, чтобы не дёргаться на границе.
//
// Логики поиска здесь нет — только вёрстка; жизнью выдачи управляют
// MainWindow и SearchResultsController.
// ============================================================================
class SearchPanelWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SearchPanelWidget(QWidget* parent = nullptr);

    FilterPanelWidget* query() const { return m_query; }
    LogListView* resultsView() const { return m_resultsView; }
    QLabel* statusLabel() const { return m_statusLabel; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateOrientation();

    QSplitter*         m_splitter = nullptr;
    FilterPanelWidget* m_query = nullptr;
    QLabel*            m_statusLabel = nullptr;
    LogListView*       m_resultsView = nullptr;
    bool               m_orientationChosen = false;
};

#endif // SEARCHPANELWIDGET_H
