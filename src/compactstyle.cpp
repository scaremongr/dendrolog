#include "compactstyle.h"

#include <QStyleOptionButton>
#include <QWidget>

namespace {

constexpr int kButtonMargin       = 4;  // Fusion: 6 (добавляется к высоте и ширине)
constexpr int kTextButtonPadding  = 8;  // поле текста кнопки слева и справа
constexpr int kMinTextButtonWidth = 60; // Fusion: 80

} // namespace

CompactStyle::CompactStyle(QStyle* base)
    : QProxyStyle(base)
{
}

int CompactStyle::pixelMetric(PixelMetric metric, const QStyleOption* option,
                              const QWidget* widget) const
{
    switch (metric) {
    case PM_LayoutLeftMargin:
    case PM_LayoutTopMargin:
    case PM_LayoutRightMargin:
    case PM_LayoutBottomMargin: {
        // Как в QCommonStyle: окно (диалог) — одни поля, вложенный виджет —
        // другие; значения — плотные.
        bool isWindow = false;
        if (option)
            isWindow = option->state & State_Window;
        else if (widget)
            isWindow = widget->isWindow();
        return isWindow ? kWindowMargin : kChildMargin;
    }
    case PM_LayoutHorizontalSpacing:
    case PM_LayoutVerticalSpacing:
        return kSpacing;

    case PM_ButtonMargin:
        return kButtonMargin;

    case PM_ToolBarIconSize:
        return kToolIconSize;
    case PM_ToolBarItemMargin:
    case PM_ToolBarFrameWidth:
        return 1;
    case PM_ToolBarHandleExtent:
        return 7;
    case PM_ToolBarSeparatorExtent:
        return 5;

    case PM_DockWidgetSeparatorExtent:
        return 4;

    case PM_TabBarTabHSpace:
        return 14;
    case PM_TabBarTabVSpace:
        return 6;
    case PM_TabCloseIndicatorWidth:
    case PM_TabCloseIndicatorHeight:
        return 14;

    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QSize CompactStyle::sizeFromContents(ContentsType type, const QStyleOption* option,
                                     const QSize& contentsSize, const QWidget* widget) const
{
    QSize size = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);
    if (type == CT_PushButton) {
        // Fusion растягивает любую текстовую кнопку до 80 px — «Apply»
        // выходил шире поля ввода рядом. Ширина — по тексту с полями
        // kTextButtonPadding, но не уже kMinTextButtonWidth.
        if (const auto* button = qstyleoption_cast<const QStyleOptionButton*>(option);
            button && !button->text.isEmpty()) {
            int width = contentsSize.width() + 2 * kTextButtonPadding
                + 2 * pixelMetric(PM_DefaultFrameWidth, option, widget);
            if (button->features & QStyleOptionButton::AutoDefaultButton)
                width += 2 * pixelMetric(PM_ButtonDefaultIndicator, option, widget);
            size.setWidth(qMax(width, kMinTextButtonWidth));
        }
    }
    return size;
}
