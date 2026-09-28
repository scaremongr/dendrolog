#include "searchpanelwidget.h"

#include "LogListView.h"
#include "compactstyle.h"
#include "filterpanelwidget.h"

#include <QLabel>
#include <QResizeEvent>
#include <QSplitter>
#include <QVBoxLayout>

namespace {

// Широкая раскладка — от этой ширины и при заметно вытянутой по горизонтали
// панели; обратно в узкую — с запасом (гистерезис).
constexpr int kWideMinWidth   = 640;
constexpr int kNarrowMaxWidth = 600;

} // namespace

SearchPanelWidget::SearchPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_splitter = new QSplitter(Qt::Vertical, this);
    m_splitter->setChildrenCollapsible(false);
    root->addWidget(m_splitter);

    m_query = new FilterPanelWidget(FilterPanelWidget::Mode::Search, m_splitter);
    m_splitter->addWidget(m_query);

    auto* results = new QWidget(m_splitter);
    auto* resultsLayout = new QVBoxLayout(results);
    resultsLayout->setContentsMargins(0, CompactStyle::kChildMargin, CompactStyle::kChildMargin,
                                      CompactStyle::kChildMargin);
    resultsLayout->setSpacing(2);

    m_statusLabel = new QLabel(results);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setContentsMargins(2, 0, 2, 0);
    resultsLayout->addWidget(m_statusLabel);

    m_resultsView = new LogListView(results);
    // Однострочный режим (klogg-style): word-wrap НЕ включаем.
    m_resultsView->setMinimumSize(200, 60);
    resultsLayout->addWidget(m_resultsView, /*stretch=*/1);
    m_splitter->addWidget(results);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
}

void SearchPanelWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateOrientation();
}

void SearchPanelWidget::updateOrientation()
{
    const int w = width();
    const int h = qMax(1, height());
    const bool isWide = m_splitter->orientation() == Qt::Horizontal;
    const bool wide = isWide ? (w >= kNarrowMaxWidth && w * 10 >= h * 13)
                             : (w >= kWideMinWidth && w * 10 >= h * 15);
    if (m_orientationChosen && wide == isWide)
        return;
    m_orientationChosen = true;

    m_splitter->setOrientation(wide ? Qt::Horizontal : Qt::Vertical);
    // Запросу — столько, сколько ему нужно (но не больше трети-половины),
    // результатам — остальное.
    const int total = wide ? w : h;
    const int querySize = wide
        ? qBound(m_query->minimumSizeHint().width(), total * 35 / 100, 520)
        : qBound(m_query->minimumSizeHint().height(),
                 qMin(total * 45 / 100, m_query->sizeHint().height()), 360);
    m_splitter->setSizes({ querySize, qMax(1, total - querySize) });
}
