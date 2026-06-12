#ifndef TEXTMATCHHIGHLIGHTER_H
#define TEXTMATCHHIGHLIGHTER_H

#include <QColor>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <QVector>

// ============================================================================
// TextMatchHighlighter — универсальный движок подсветки подстрок.
//
// Самостоятельный модуль без привязки к фильтрам, модели или виджетам:
//   • LogListView использует его для inline-подсветки совпадений (фоновые
//     заливки под текстом, не конфликтующие с раскраской SyntaxHighlighter);
//   • LogModel использует его для недеструктивных row-маркеров
//     (цвет фона всей строки);
//   • любой будущий модуль (поиск, diff, ...) может использовать его так же.
//
// Дизайн:
//   • Value-semantics, без QObject — дёшево копировать, безопасно жить
//     в нескольких владельцах.
//   • revision() монотонно растёт при каждом изменении набора паттернов —
//     внешние кэши (например, кэш пиксмапов строк) сравнивают ревизию
//     вместо глубокого сравнения паттернов.
//   • Паттерн может быть простой подстрокой или регулярным выражением
//     (isRegex). Регексы компилируются один раз в setPatterns();
//     невалидный регекс просто не участвует в подсветке.
//   • computeSpans() вызывается только для видимых строк, результат оседает
//     в кэше отрисовки, поэтому скролл не страдает.
// ============================================================================

// Один диапазон подсветки в строке: [start, start+length).
struct HighlightSpan {
    int    start  = 0;
    int    length = 0;
    QColor color;
};

// Описание одного паттерна подсветки.
struct HighlightPattern {
    QString             text;                                  // подстрока или регекс
    QColor              color;                                 // цвет заливки
    Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
    bool                isRegex         = false;
    bool                enabled         = true;

    bool isActive() const { return enabled && !text.isEmpty() && color.isValid(); }
};

// Сериализация набора паттернов (персистентность row-маркеров и т.п.).
QJsonArray highlightPatternsToJson(const QVector<HighlightPattern>& patterns);
QVector<HighlightPattern> highlightPatternsFromJson(const QJsonArray& json);

class TextMatchHighlighter {
public:
    TextMatchHighlighter() = default;

    void setPatterns(const QVector<HighlightPattern>& patterns);
    const QVector<HighlightPattern>& patterns() const { return m_patterns; }

    // true, если нет ни одного активного паттерна — быстрый отсев
    // до какого-либо сканирования текста.
    bool isEmpty() const { return !m_hasActive; }

    // Монотонный счётчик изменений для инвалидации внешних кэшей.
    int revision() const { return m_revision; }

    // Все вхождения всех активных паттернов в text.
    // Спаны возвращаются в порядке паттернов; пересечения допустимы
    // (поздние заливки рисуются поверх ранних).
    QVector<HighlightSpan> computeSpans(QStringView text) const;

    // Цвет первого активного паттерна, найденного в text (для row-маркеров).
    // Невалидный QColor — совпадений нет.
    QColor firstMatchColor(QStringView text) const;

private:
    bool patternUsable(int index) const;

    QVector<HighlightPattern> m_patterns;
    // Параллелен m_patterns: скомпилированный регекс для isRegex-паттернов,
    // default-constructed для простых подстрок.
    QVector<QRegularExpression> m_regexes;
    bool m_hasActive = false;
    int  m_revision  = 0;
};

#endif // TEXTMATCHHIGHLIGHTER_H
