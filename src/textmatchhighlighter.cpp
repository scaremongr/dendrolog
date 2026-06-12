#include "textmatchhighlighter.h"

#include <QJsonObject>

QJsonArray highlightPatternsToJson(const QVector<HighlightPattern>& patterns)
{
    QJsonArray json;
    for (const auto& p : patterns) {
        QJsonObject obj;
        obj[QStringLiteral("text")]          = p.text;
        obj[QStringLiteral("color")]         = p.color.isValid() ? p.color.name(QColor::HexArgb) : QString();
        obj[QStringLiteral("enabled")]       = p.enabled;
        obj[QStringLiteral("caseSensitive")] = (p.caseSensitivity == Qt::CaseSensitive);
        obj[QStringLiteral("regex")]         = p.isRegex;
        json.append(obj);
    }
    return json;
}

QVector<HighlightPattern> highlightPatternsFromJson(const QJsonArray& json)
{
    QVector<HighlightPattern> patterns;
    patterns.reserve(json.size());
    for (const auto& value : json) {
        const QJsonObject obj = value.toObject();
        HighlightPattern p;
        p.text            = obj[QStringLiteral("text")].toString();
        p.enabled         = obj[QStringLiteral("enabled")].toBool(true);
        p.isRegex         = obj[QStringLiteral("regex")].toBool(false);
        p.caseSensitivity = obj[QStringLiteral("caseSensitive")].toBool(false)
                                ? Qt::CaseSensitive : Qt::CaseInsensitive;
        const QString colorName = obj[QStringLiteral("color")].toString();
        if (!colorName.isEmpty())
            p.color = QColor(colorName);
        patterns.append(p);
    }
    return patterns;
}

void TextMatchHighlighter::setPatterns(const QVector<HighlightPattern>& patterns)
{
    m_patterns = patterns;

    // Регексы компилируются один раз здесь, а не при каждом сканировании.
    m_regexes.clear();
    m_regexes.resize(m_patterns.size());
    for (int i = 0; i < m_patterns.size(); ++i) {
        const auto& p = m_patterns[i];
        if (!p.isRegex || p.text.isEmpty())
            continue;
        QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
        if (p.caseSensitivity == Qt::CaseInsensitive)
            options |= QRegularExpression::CaseInsensitiveOption;
        m_regexes[i] = QRegularExpression(p.text, options);
    }

    m_hasActive = false;
    for (int i = 0; i < m_patterns.size(); ++i) {
        if (patternUsable(i)) {
            m_hasActive = true;
            break;
        }
    }
    ++m_revision;
}

// Активен и пригоден к поиску: для регексов дополнительно требуется
// успешная компиляция (невалидный регекс молча выключается).
bool TextMatchHighlighter::patternUsable(int index) const
{
    const auto& p = m_patterns[index];
    if (!p.isActive())
        return false;
    if (p.isRegex && !m_regexes[index].isValid())
        return false;
    return true;
}

QVector<HighlightSpan> TextMatchHighlighter::computeSpans(QStringView text) const
{
    QVector<HighlightSpan> spans;
    if (!m_hasActive || text.isEmpty())
        return spans;

    for (int i = 0; i < m_patterns.size(); ++i) {
        if (!patternUsable(i))
            continue;
        const auto& pattern = m_patterns[i];

        if (pattern.isRegex) {
            auto it = m_regexes[i].globalMatchView(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch match = it.next();
                const int len = static_cast<int>(match.capturedLength());
                if (len <= 0)
                    continue; // пустые совпадения (например, ".*?") не подсвечиваем
                spans.append({static_cast<int>(match.capturedStart()), len, pattern.color});
            }
        } else {
            const int patternLen = pattern.text.length();
            qsizetype from = 0;
            while (true) {
                const qsizetype pos = text.indexOf(pattern.text, from, pattern.caseSensitivity);
                if (pos < 0)
                    break;
                spans.append({static_cast<int>(pos), patternLen, pattern.color});
                from = pos + patternLen;
            }
        }
    }
    return spans;
}

QColor TextMatchHighlighter::firstMatchColor(QStringView text) const
{
    if (!m_hasActive || text.isEmpty())
        return QColor();

    for (int i = 0; i < m_patterns.size(); ++i) {
        if (!patternUsable(i))
            continue;
        const auto& pattern = m_patterns[i];
        const bool found = pattern.isRegex
            ? m_regexes[i].matchView(text).hasMatch()
            : text.contains(pattern.text, pattern.caseSensitivity);
        if (found)
            return pattern.color;
    }
    return QColor();
}
