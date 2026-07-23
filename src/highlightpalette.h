#ifndef HIGHLIGHTPALETTE_H
#define HIGHLIGHTPALETTE_H

#include <QColor>

// ============================================================================
// HighlightPalette — фиксированный набор цветов для подсветки.
//
// Два ряда одних и тех же оттенков:
//   • светлый (пастель) — для светлой темы: тёмный текст и раскраска
//     SyntaxHighlighter остаются читаемыми поверх заливки;
//   • тёмный (приглушённая насыщенность) — для тёмной темы: светлый текст
//     не сливается с заливкой, а сама заливка заметна на фоне #1e1e1e.
//
// Ряд выбирается по ФАКТИЧЕСКОМУ фону виджета (QPalette::Base), а не по
// какой-либо настройке темы: приложение красится системной цветовой схемой.
//
// Используется конструктором фильтров и панелью row-маркеров для
// автоназначения цвета новому правилу (по кругу).
// ============================================================================

namespace HighlightPalette {

// Тёмный ли фон: относительная яркость по sRGB-коэффициентам.
inline bool isDarkBackground(const QColor& background)
{
    if (!background.isValid())
        return false;
    const double luma = 0.2126 * background.redF()
                      + 0.7152 * background.greenF()
                      + 0.0722 * background.blueF();
    return luma < 0.45;
}

// Цвет №index палитры под заданный фон. Невалидный фон = светлая тема
// (историческое поведение).
inline QColor colorAt(int index, const QColor& background = QColor())
{
    static const QColor kLight[] = {
        QColor(0xFF, 0xE0, 0x8A), // янтарный
        QColor(0xA8, 0xD8, 0xFF), // голубой
        QColor(0xB6, 0xF0, 0xB6), // зелёный
        QColor(0xFF, 0xB3, 0xC8), // розовый
        QColor(0xD8, 0xC2, 0xFF), // сиреневый
        QColor(0xFF, 0xD1, 0xA1), // оранжевый
        QColor(0x9F, 0xE8, 0xE0), // бирюзовый
        QColor(0xE6, 0xE6, 0x8A), // оливковый
        QColor(0xC4, 0xE0, 0x8C), // салатовый
        QColor(0xF2, 0xC4, 0xF2), // лиловый
    };
    // Те же оттенки, но как фон под светлый текст: L ≈ 25–30 %.
    static const QColor kDark[] = {
        QColor(0x6E, 0x52, 0x14), // янтарный
        QColor(0x14, 0x45, 0x70), // голубой
        QColor(0x1E, 0x5A, 0x2E), // зелёный
        QColor(0x6E, 0x1F, 0x3E), // розовый
        QColor(0x3F, 0x2C, 0x70), // сиреневый
        QColor(0x70, 0x3E, 0x14), // оранжевый
        QColor(0x14, 0x54, 0x4F), // бирюзовый
        QColor(0x55, 0x56, 0x1B), // оливковый
        QColor(0x3D, 0x5A, 0x18), // салатовый
        QColor(0x5C, 0x2A, 0x5C), // лиловый
    };
    constexpr int kCount = int(sizeof(kLight) / sizeof(kLight[0]));
    const int i = ((index % kCount) + kCount) % kCount;
    return isDarkBackground(background) ? kDark[i] : kLight[i];
}

// Читаемый цвет текста поверх заливки color. Используется отрисовкой строк:
// какой бы цвет пользователь ни выбрал, буквы в совпадении остаются видны.
inline QColor textOn(const QColor& fill)
{
    return isDarkBackground(fill) ? QColor(0xF2, 0xF2, 0xF2) : QColor(0x14, 0x14, 0x14);
}

} // namespace HighlightPalette

#endif // HIGHLIGHTPALETTE_H
