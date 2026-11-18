#ifndef QUICKSEARCHFIELD_H
#define QUICKSEARCHFIELD_H

#include "quicksearchhistory.h"
#include "textquery.h"

#include <QLineEdit>

class QAction;
class QMenu;

// ============================================================================
// QuickSearchField — поле быстрого поиска в тулбаре (Ctrl+F, F3 / Shift+F3).
//
// Собирает запрос (TextQuery) из текста и трёх переключателей — «Aa»
// (регистр), «слово целиком», «.*» (регулярное выражение) — и держит
// историю запросов. Сам ничего не ищет: поиск запускает MainWindow по
// query(), а в историю запрос кладёт remember() — только когда поиск реально
// запущен (Enter, F3, кнопки, Find All), а не на каждую букву.
//
// Переключатели — QAction поля: окно ставит их в тулбар рядом с полем и
// назначает им шорткаты (ShortcutManager). Их состояние и история хранятся в
// настройках (AppSettings, группа [QuickSearch]); поле о настройках не знает,
// а сообщает об изменениях сигналами.
//
// Состояния поля:
//   • невалидный регекс — красная рамка и сообщение с позицией в подсказке
//     поля (как у правила в панели Text Filters); queryError() его отдаёт,
//     поиск по такому запросу не запускается;
//   • «не найдено» — красноватый фон до правки запроса или флагов.
//
// История: стрелка вниз (или Alt+вниз) в поле — в любом его состоянии — и
// кнопка-стрелка в самом поле открывают список последних запросов. Выбор
// пункта ставит запрос в поле и сигналит historyActivated (окно ищет
// дальше); последний пункт — «Clear search history».
// ============================================================================
class QuickSearchField : public QLineEdit {
    Q_OBJECT

public:
    explicit QuickSearchField(QWidget* parent = nullptr);

    QAction* matchCaseAction() const { return m_matchCase; }
    QAction* wholeWordAction() const { return m_wholeWord; }
    QAction* regexAction() const { return m_regex; }
    // Стрелка в правом краю поля: открывает историю.
    QAction* historyAction() const { return m_historyButton; }

    TextQuery query() const;
    // Сообщение об ошибке текущего запроса (невалидный регекс) или пусто.
    QString queryError() const { return m_error; }

    // ---- История --------------------------------------------------------------
    void setHistory(const QStringList& entries);
    QStringList history() const { return m_history.entries(); }
    // Запрос реально запущен: запомнить текст. Сигналит historyChanged, если
    // список изменился.
    void remember(const QString& text);
    void clearHistory();
    // Открыть список истории под полем (неблокирующий popup).
    void showHistory();
    QMenu* historyMenu() const { return m_historyMenu; }

    // «Не найдено» — до правки запроса или флагов.
    void setNotFound(bool notFound);
    bool isNotFound() const { return m_notFound; }

    // Подсказка поля без ошибки (окно ставит её с текущими шорткатами).
    void setBaseToolTip(const QString& tip);
    // Перерисовать состояние в цветах текущей темы (окно зовёт при смене темы).
    void refreshStyle();

signals:
    void historyChanged(const QStringList& entries);
    // Пользователь выбрал запрос из истории — он уже стоит в поле.
    void historyActivated(const QString& text);
    // Переключён «Aa», «слово целиком» или «.*».
    void optionsChanged();

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    void updateValidity();

    QAction* m_matchCase = nullptr;
    QAction* m_wholeWord = nullptr;
    QAction* m_regex = nullptr;
    QAction* m_historyButton = nullptr;
    QMenu* m_historyMenu = nullptr;
    QuickSearchHistory m_history;
    QString m_error;
    QString m_baseToolTip;
    bool m_notFound = false;
};

#endif // QUICKSEARCHFIELD_H
