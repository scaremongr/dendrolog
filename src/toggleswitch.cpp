#include "toggleswitch.h"

#include <QPainter>
#include <QPropertyAnimation>

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    m_anim = new QPropertyAnimation(this, "knobPosition", this);
    m_anim->setDuration(140);
    m_anim->setEasingCurve(QEasingCurve::InOutQuad);

    // Плавно двигаем кружок при пользовательском переключении (клик/Space).
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_anim->stop();
        m_anim->setStartValue(m_knobPosition);
        m_anim->setEndValue(on ? 1.0 : 0.0);
        m_anim->start();
    });
}

QSize ToggleSwitch::sizeHint() const
{
    // Компактный — заметно ниже tool-кнопок (цветной образец 22×22).
    return QSize(30, 16);
}

void ToggleSwitch::setKnobPosition(qreal p)
{
    m_knobPosition = qBound(0.0, p, 1.0);
    update();
}

void ToggleSwitch::enterEvent(QEnterEvent*)
{
    m_hovered = true;
    update();
}

void ToggleSwitch::leaveEvent(QEvent*)
{
    m_hovered = false;
    update();
}

void ToggleSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal h = r.height();
    const qreal radius = h / 2.0;

    // Дорожка: серая (off) → highlight (on), линейная интерполяция по позиции.
    const QColor off = palette().color(QPalette::Mid);
    const QColor on  = palette().color(QPalette::Highlight);
    const auto lerp = [this](int a, int b) { return int(a + (b - a) * m_knobPosition); };
    QColor track(lerp(off.red(), on.red()), lerp(off.green(), on.green()), lerp(off.blue(), on.blue()));
    if (!isEnabled())
        track.setAlpha(100);
    else if (m_hovered)
        track = track.lighter(112);

    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(r, radius, radius);

    // Кружок — белый с лёгкой тенью-контуром, для обеих тем.
    const qreal margin = 2.0;
    const qreal knobD = h - 2.0 * margin;
    const qreal x = r.left() + margin + m_knobPosition * (r.width() - knobD - 2.0 * margin);
    const QRectF knob(x, r.top() + margin, knobD, knobD);
    p.setBrush(QColor(255, 255, 255));
    p.setPen(QPen(QColor(0, 0, 0, 40), 1));
    p.drawEllipse(knob);
}
