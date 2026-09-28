#ifndef COMPACTSTYLE_H
#define COMPACTSTYLE_H

#include <QProxyStyle>

// ============================================================================
// CompactStyle — плотные метрики поверх Fusion, единые для всего приложения.
//
// Fusion рассчитан на «воздушные» формы: поля раскладок 9–11 px, тулбар с
// иконками 24 px высотой ~35 px, вкладки ~32 px, текстовая кнопка не уже
// 80 px. В просмотрщике логов это съедает место у данных и делает окно
// разнородным: плотный лог рядом с пышными кнопками. Здесь — одна сетка
// отступов на всё окно:
//
//   • поля раскладки: 4 px у виджетов, 8 px у окон (диалоги);
//   • промежуток между элементами — 4 px;
//   • тулбары: иконки 16 px, рамка и поля по 1 px;
//   • вкладки: ниже и уже (поля 6 × 14 px), крестик 14 px;
//   • разделитель доков — 4 px (за него ещё удобно тянуть);
//   • кнопки: поля 4 px, минимальная ширина текстовой кнопки 60 px.
//
// Панели НЕ задают поля и промежутки своих корневых раскладок числами —
// они берут их у стиля, поэтому выглядят одинаково. Явные значения — только
// там, где нужно отличие (0 у обёрток, плотные строки внутри карточек).
// Цвета — не здесь, а в AppTheme и палитре.
// ============================================================================
class CompactStyle : public QProxyStyle
{
    Q_OBJECT
public:
    // Базовый стиль переходит во владение прокси (как у QProxyStyle).
    explicit CompactStyle(QStyle* base = nullptr);

    static constexpr int kChildMargin    = 4;  // поля раскладки виджета/панели
    static constexpr int kWindowMargin   = 8;  // поля раскладки окна/диалога
    static constexpr int kSpacing        = 4;  // промежуток между элементами
    static constexpr int kToolIconSize   = 16; // иконки тулбаров и кнопок-инструментов

    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                    const QWidget* widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option,
                           const QSize& contentsSize,
                           const QWidget* widget = nullptr) const override;
};

#endif // COMPACTSTYLE_H
