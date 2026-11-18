#ifndef NEWLINESINDICATOR_H
#define NEWLINESINDICATOR_H

#include <QAbstractButton>

// ============================================================================
// NewLinesIndicator — плашка «↓ N new lines» поверх нижнего края LogListView.
// Только вид: счёт и переход ведёт LogListView (см. newLinesBelow). Рисуется
// сама, без style sheet: фон и текст — роли Highlight/HighlightedText текущей
// палитры на каждый кадр, поэтому смена темы перекрашивает её без перезапуска
// и без сброса кэшей. Шрифт — шрифт приложения, а не моноширинный шрифт лога:
// Ctrl+колесо меняет размер текста лога, но не плашки.
// ============================================================================
class NewLinesIndicator : public QAbstractButton {
    Q_OBJECT
public:
    explicit NewLinesIndicator(QWidget* parent = nullptr);

    // Число новых строк → текст плашки: точно до 99 999 (с разделителем
    // разрядов), дальше сокращённо — «120K», «3.4M», «25M». Точное число —
    // в подсказке.
    static QString label(qint64 count);

    void setCount(qint64 count);
    qint64 count() const { return m_count; }

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    qint64 m_count = 0;
};

#endif // NEWLINESINDICATOR_H
