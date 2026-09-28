#ifndef CARDFRAME_H
#define CARDFRAME_H

#include <QColor>
#include <QFrame>
#include <QIcon>
#include <QScrollArea>

class QLabel;
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

    // ---- Палитро-зависимые «приглушённые» цвета -------------------------
    // QSS palette(mid) на тёмных палитрах почти сливается с фоном, поэтому
    // весь неяркий «хром» (рамки, глифы-связки, подписи) смешивается из
    // WindowText и Window в коде. t = доля второго цвета (0 → a, 1 → b).

    /// Монохромная SVG-иконка из ресурсов, перекрашенная в заданный цвет:
    /// один и тот же глиф остаётся видимым и на светлой, и на тёмной палитре.
    static QIcon tintedIcon(const QString& resourcePath, const QColor& color);

    static QColor mixedColor(const QColor& a, const QColor& b, qreal t);
    /// Рамки и разделители: 35% текста / 65% фона.
    static QColor mutedBorderColor(const QPalette& palette);
    /// Вторичные глифы и подписи: 65% текста / 35% фона.
    static QColor mutedTextColor(const QPalette& palette);

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyFrameStyle();

    QFrame*      m_stripe = nullptr;
    QVBoxLayout* m_rows   = nullptr;
    QColor       m_accent;
    bool         m_accentBorder = false;
};

// ============================================================
// CardListArea — прокручиваемый вертикальный список карточек
// (правила Text Filters, маркеры Row Highlighters).
//
//   • десяток карточек не растягивает док за край окна — список
//     прокручивается, а шапка панели над ним остаётся на месте;
//   • ширина не ужимается уже самих карточек (minimumSizeHint
//     учитывает содержимое), поэтому горизонтальной прокрутки нет;
//   • пока карточек нет, вместо пустоты показана подсказка
//     (setEmptyText) — что это за список и как его наполнить.
//
// Карточки добавляются в cardsLayout(); после добавления/удаления
// нужно звать cardsChanged() — он обновляет подсказку и геометрию.
// ============================================================
class CardListArea : public QScrollArea
{
    Q_OBJECT
public:
    explicit CardListArea(QWidget* parent = nullptr);

    QVBoxLayout* cardsLayout() const { return m_cards; }

    /// Текст-заглушка пустого списка (приглушённым цветом, с переносом).
    void setEmptyText(const QString& text);

    /// Пересчитать заглушку и минимальную ширину после смены карточек.
    void cardsChanged();

    /// Прокрутить к карточке, когда раскладка её уже расставит.
    void revealLater(QWidget* card);

    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;

private:
    QVBoxLayout* m_cards = nullptr;
    QLabel*      m_emptyLabel = nullptr;
};

#endif // CARDFRAME_H
