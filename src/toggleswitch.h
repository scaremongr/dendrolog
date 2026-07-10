#ifndef TOGGLESWITCH_H
#define TOGGLESWITCH_H

#include <QAbstractButton>

class QPropertyAnimation;

// ============================================================================
// ToggleSwitch — компактный переключатель-«тумблер» (iOS-style): скруглённая
// дорожка + скользящий кружок. По сути checkable QAbstractButton (isChecked/
// toggled как у чекбокса), но выглядит как switch. Подпись — снаружи, рядом,
// и НЕ меняется при переключении. Цвета берутся из palette (тема-осознан).
// ============================================================================
class ToggleSwitch : public QAbstractButton {
    Q_OBJECT
    Q_PROPERTY(qreal knobPosition READ knobPosition WRITE setKnobPosition)
public:
    explicit ToggleSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    qreal knobPosition() const { return m_knobPosition; }
    void  setKnobPosition(qreal p);   // 0 = off (слева) … 1 = on (справа)

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    qreal m_knobPosition = 0.0;
    bool  m_hovered = false;
    QPropertyAnimation* m_anim = nullptr;
};

#endif // TOGGLESWITCH_H
