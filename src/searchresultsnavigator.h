#ifndef SEARCHRESULTSNAVIGATOR_H
#define SEARCHRESULTSNAVIGATOR_H

#include <QObject>
#include <QPointer>

#include <functional>

class LogListView;
class LogModel;
class LogViewWidget;
class QModelIndex;

// ============================================================================
// SearchResultsNavigator — выбор строки в выдаче панели Search (мышь,
// клавиатура) → переход к той же записи во вкладке (LogViewWidget::goToEntry,
// NavKind::Search: выбор пользователя, запись в историю переходов).
//
// Выдача перестраивается сама (полный поиск после фильтра, дозаписи,
// переписанного файла): view выдачи на reset восстанавливает своё выделение,
// и Qt присылает currentRowChanged. Это НЕ выбор пользователя: переход во
// вкладке он делать не должен — иначе вкладка уехала бы к записи выдачи, а
// в истории появился бы переход, которого пользователь не делал. Поэтому
// навигатор глушит переходы с modelAboutToBeReset до конца modelReset — а
// конец ловит ПОСЛЕ обработчика view (подписка после setModel), — и на время
// явных перестроек (Suppress вокруг setSource / search / clear).
//
// Без MainWindow: проверяется в tests/nav_history.
// ============================================================================
class SearchResultsNavigator : public QObject {
    Q_OBJECT
public:
    // Ставит results в resultsView сам: порядок подписок относительно
    // обработчиков reset самой view здесь существенен.
    SearchResultsNavigator(LogModel* results, LogListView* resultsView,
                           QObject* parent = nullptr);

    // Вкладка, в которой искали (активная); nullptr — переходить некуда.
    void setTarget(std::function<LogViewWidget*()> target) { m_target = std::move(target); }

    // Перестройка выдачи по действию окна (не пользователя в выдаче).
    class Suppress {
    public:
        explicit Suppress(SearchResultsNavigator& n) : m_n(n) { ++m_n.m_suppressed; }
        ~Suppress() { --m_n.m_suppressed; }
        Suppress(const Suppress&) = delete;
        Suppress& operator=(const Suppress&) = delete;
    private:
        SearchResultsNavigator& m_n;
    };
    bool isSuppressed() const { return m_suppressed > 0; }

    // Переходов во вкладку (для тестов).
    int navigations() const { return m_navigations; }

private:
    void activate(const QModelIndex& current);

    QPointer<LogModel> m_results;
    std::function<LogViewWidget*()> m_target;
    int m_suppressed = 0;
    int m_navigations = 0;
};

#endif // SEARCHRESULTSNAVIGATOR_H
