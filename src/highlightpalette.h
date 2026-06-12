#ifndef HIGHLIGHTPALETTE_H
#define HIGHLIGHTPALETTE_H

#include <QColor>

// ============================================================================
// HighlightPalette — фиксированный набор цветов для подсветки.
//
// Цвета подобраны как фоновые: достаточно насыщенные, чтобы различаться
// между собой, и достаточно светлые, чтобы тёмный текст и раскраска
// SyntaxHighlighter оставались читаемыми поверх заливки.
//
// Используется конструктором фильтров и панелью row-маркеров для
// автоназначения цвета новому правилу (по кругу).
// ============================================================================

namespace HighlightPalette {

inline QColor colorAt(int index)
{
    static const QColor kColors[] = {
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
    constexpr int kCount = int(sizeof(kColors) / sizeof(kColors[0]));
    return kColors[((index % kCount) + kCount) % kCount];
}

} // namespace HighlightPalette

#endif // HIGHLIGHTPALETTE_H
