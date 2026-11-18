#include "scrollbarmarksoverlay.h"

#include "LogListView.h"
#include "apptheme.h"

#include <QEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyle>
#include <QStyleOptionSlider>

namespace {

QStyleOptionSlider sliderOption(const QScrollBar* bar)
{
    // То же, что QScrollBar::initStyleOption (он protected).
    QStyleOptionSlider opt;
    opt.initFrom(bar);
    opt.subControls = QStyle::SC_All;
    opt.activeSubControls = QStyle::SC_None;
    opt.orientation = bar->orientation();
    opt.minimum = bar->minimum();
    opt.maximum = bar->maximum();
    opt.sliderPosition = bar->sliderPosition();
    opt.sliderValue = bar->value();
    opt.singleStep = bar->singleStep();
    opt.pageStep = bar->pageStep();
    opt.upsideDown = bar->invertedAppearance();
    if (bar->orientation() == Qt::Horizontal)
        opt.state |= QStyle::State_Horizontal;
    return opt;
}

} // namespace

ScrollbarMarksOverlay::ScrollbarMarksOverlay(LogListView* view, ScrollbarMarks* marks)
    : QWidget(view->verticalScrollBar())
    , m_view(view)
    , m_bar(view->verticalScrollBar())
    , m_marks(marks)
{
    setObjectName(QStringLiteral("scrollbarMarks"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(m_bar->rect());
    m_bar->installEventFilter(this);
    connect(m_marks, &ScrollbarMarks::marksChanged, this, qOverload<>(&QWidget::update));
    connect(m_bar, &QScrollBar::rangeChanged, this, qOverload<>(&QWidget::update));
    syncEnabled();
}

bool ScrollbarMarksOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_bar) {
        switch (event->type()) {
        case QEvent::Resize:
            setGeometry(m_bar->rect());
            break;
        case QEvent::Show:
        case QEvent::Hide:
            syncEnabled();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ScrollbarMarksOverlay::syncEnabled()
{
    // Скроллбар не виден (вкладка скрыта, лог помещается на экран) —
    // считать отметки незачем; появится — расчёт догонит.
    m_marks->setEnabled(m_bar->isVisible());
}

QRect ScrollbarMarksOverlay::grooveRect() const
{
    const QStyleOptionSlider opt = sliderOption(m_bar);
    return m_bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt,
                                          QStyle::SC_ScrollBarGroove, m_bar);
}

QRect ScrollbarMarksOverlay::sliderRect() const
{
    const QStyleOptionSlider opt = sliderOption(m_bar);
    return m_bar->style()->subControlRect(QStyle::CC_ScrollBar, &opt,
                                          QStyle::SC_ScrollBarSlider, m_bar);
}

ScrollbarTrack ScrollbarMarksOverlay::track() const
{
    ScrollbarTrack t;
    t.maximum = m_bar->maximum();
    t.pageStep = m_bar->pageStep();
    t.grooveLength = grooveRect().height();
    t.sliderLength = sliderRect().height();
    return t;
}

ScrollbarMarksOverlay::Key ScrollbarMarksOverlay::currentKey() const
{
    Key k;
    k.serial = m_marks->serial();
    k.maximum = m_bar->maximum();
    k.pageStep = m_bar->pageStep();
    k.groove = grooveRect();
    k.sliderLength = sliderRect().height();
    k.rows = m_view->model() ? m_view->model()->rowCount() : 0;
    k.geometry = m_view->markGeometryStamp();
    return k;
}

void ScrollbarMarksOverlay::ensurePixels() const
{
    const Key key = currentKey();
    if (key == m_key)
        return;
    m_key = key;
    ScrollbarTrack t;
    t.maximum = key.maximum;
    t.pageStep = key.pageStep;
    t.grooveLength = key.groove.height();
    t.sliderLength = key.sliderLength;
    const ScrollbarMarkSet& set = m_marks->marks();
    for (int c = 0; c < ScrollbarMarkSet::CategoryCount; ++c) {
        QVector<int>& out = m_pixels[c];
        out.clear();
        for (qint64 row : set.rows[c]) {
            if (row >= key.rows)
                break; // строк стало меньше, чем в снапшоте; пересчёт уже идёт
            const int px = ScrollbarMarks::trackPosition(m_view->markScrollValue(int(row)), t);
            if (px >= 0 && (out.isEmpty() || out.constLast() != px))
                out.append(px);
        }
    }
}

const QVector<int>& ScrollbarMarksOverlay::pixels(ScrollbarMarkSet::Category category) const
{
    ensurePixels();
    return m_pixels[category];
}

QPair<int, int> ScrollbarMarksOverlay::lane(ScrollbarMarkSet::Category category) const
{
    // Слева поиск, затем закладки, правая половина — уровни.
    const QRect g = grooveRect();
    const int left = g.left();
    const int w = g.width();
    const int quarter = qMax(1, w / 4);
    switch (category) {
    case ScrollbarMarkSet::Search:
        return { left, left + quarter };
    case ScrollbarMarkSet::Bookmark:
        return { left + quarter, left + 2 * quarter };
    default:
        return { left + w - qMax(1, w / 2), left + w };
    }
}

void ScrollbarMarksOverlay::paintEvent(QPaintEvent*)
{
    ensurePixels();
    const QRect groove = m_key.groove;
    if (groove.height() <= 0)
        return;
    const AppTheme& theme = AppTheme::instance();
    QPainter painter(this);
    const auto draw = [&](ScrollbarMarkSet::Category category, const QColor& color) {
        const QVector<int>& px = m_pixels[category];
        if (px.isEmpty())
            return;
        const QPair<int, int> x = lane(category);
        const int bottom = groove.top() + groove.height();
        for (int p : px) {
            const int y = qMin(groove.top() + p, bottom - kMarkHeight);
            painter.fillRect(QRect(x.first, y, x.second - x.first, kMarkHeight), color);
        }
    };
    // Ошибка поверх предупреждения — в одной полосе.
    draw(ScrollbarMarkSet::Warning, theme.forLevel(LogLevel::Warn));
    draw(ScrollbarMarkSet::Error, theme.forLevel(LogLevel::Error));
    draw(ScrollbarMarkSet::Bookmark, theme.gutterBookmark);
    QColor search = theme.searchMatch;
    search.setAlpha(255);
    draw(ScrollbarMarkSet::Search, search);
}
