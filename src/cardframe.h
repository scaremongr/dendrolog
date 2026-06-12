#ifndef CARDFRAME_H
#define CARDFRAME_H

#include <QColor>
#include <QFrame>

class QToolButton;
class QVBoxLayout;

// ============================================================
// CardFrame — общий визуальный контейнер «блока» с рамкой.
//
// Вынесен из PatternBlockCard (Manage Field Schemas), чтобы все
// блочные элементы приложения выглядели одинаково:
//
//   ▌ <строки содержимого, добавляются в rowsLayout()>
//   ▌ ...
//
//   • цветная полоска-акцент слева;
//   • скруглённая рамка: тонкая нейтральная или 2px цвета акцента
//     (setAccentBorder) — например, для выделения особых блоков;
//   • makeToolButton() — единый стиль плоских кнопок карточки (⚙ ↑ ↓ ✕);
//   • применение тонировки кнопки ⚙ цветом акцента (tintToolButton),
//     когда «продвинутая» строка содержит значения.
//
// Используется PatternBlockCard, FilterRuleCard и MarkerCard.
// ============================================================

class CardFrame : public QFrame
{
    Q_OBJECT
public:
    explicit CardFrame(QWidget* parent = nullptr);

    void   setAccentColor(const QColor& color);
    QColor accentColor() const { return m_accent; }

    /// 2px рамка цвета акцента вместо тонкой нейтральной.
    void setAccentBorder(bool on);

    /// Вертикальный контейнер строк карточки (справа от полоски-акцента).
    QVBoxLayout* rowsLayout() const { return m_rows; }

    /// Плоская кнопка-инструмент карточки в едином стиле.
    QToolButton* makeToolButton(const QString& text, const QString& toolTip);

    /// Тонирует кнопку цветом акцента (hasContent = true) или сбрасывает
    /// стиль — подсказка «внутри ⚙ есть настройки».
    void tintToolButton(QToolButton* button, bool hasContent) const;

private:
    void applyFrameStyle();

    QFrame*      m_stripe = nullptr;
    QVBoxLayout* m_rows   = nullptr;
    QColor       m_accent;
    bool         m_accentBorder = false;
};

#endif // CARDFRAME_H
