#pragma once

#include <QString>
#include <utility>

// Определяет границы токена под позицией pos при двойном клике.
// Возвращает {start, end} — полуоткрытый интервал [start, end).
// Если позиция приходится на пустое место, возвращает {start, start}.
namespace TextToken {
    std::pair<int,int> findDoubleClickToken(const QString& text, int pos);
}
