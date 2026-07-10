#include "filterruleset.h"

#include <QJsonArray>

// ---------------------------------------------------------------------------
// FilterRule — JSON
// ---------------------------------------------------------------------------

QJsonObject FilterRule::toJson() const
{
    QJsonObject json;
    json[QStringLiteral("enabled")]       = enabled;
    json[QStringLiteral("action")]        = (action == Action::Exclude) ? QStringLiteral("exclude")
                                                                        : QStringLiteral("include");
    json[QStringLiteral("connector")]     = (connector == Connector::Or) ? QStringLiteral("or")
                                                                         : QStringLiteral("and");
    json[QStringLiteral("text")]          = text;
    json[QStringLiteral("field")]         = fieldName;
    json[QStringLiteral("caseSensitive")] = caseSensitive;
    json[QStringLiteral("regex")]         = isRegex;
    json[QStringLiteral("color")]         = highlightColor.isValid() ? highlightColor.name(QColor::HexArgb)
                                                                     : QString();
    json[QStringLiteral("highlight")]     = highlightEnabled;
    return json;
}

FilterRule FilterRule::fromJson(const QJsonObject& json)
{
    FilterRule rule;
    rule.enabled       = json[QStringLiteral("enabled")].toBool(true);
    rule.action        = (json[QStringLiteral("action")].toString() == QLatin1String("exclude"))
                             ? Action::Exclude : Action::Include;
    rule.connector     = (json[QStringLiteral("connector")].toString() == QLatin1String("or"))
                             ? Connector::Or : Connector::And;
    rule.text          = json[QStringLiteral("text")].toString();
    rule.fieldName     = json[QStringLiteral("field")].toString();
    rule.caseSensitive = json[QStringLiteral("caseSensitive")].toBool(false);
    rule.isRegex       = json[QStringLiteral("regex")].toBool(false);
    const QString colorName = json[QStringLiteral("color")].toString();
    if (!colorName.isEmpty())
        rule.highlightColor = QColor(colorName);
    rule.highlightEnabled = json[QStringLiteral("highlight")].toBool(true);
    return rule;
}

// ---------------------------------------------------------------------------
// FilterRuleSet
// ---------------------------------------------------------------------------

void FilterRuleSet::bindFields(const QStringList& fieldNames, bool fieldScopeActive)
{
    m_fieldScopeActive = fieldScopeActive;

    m_boundFieldIndexes.resize(rules.size());
    m_compiledRegexes.clear();
    m_compiledRegexes.resize(rules.size());

    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& rule = rules[i];
        m_boundFieldIndexes[i] = rule.fieldName.isEmpty()
            ? -1
            : static_cast<int>(fieldNames.indexOf(rule.fieldName));

        if (rule.isRegex && !rule.text.isEmpty()) {
            QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption;
            if (!rule.caseSensitive)
                options |= QRegularExpression::CaseInsensitiveOption;
            m_compiledRegexes[i] = QRegularExpression(rule.text, options);
        }
    }
}

bool FilterRuleSet::isActive() const
{
    for (const auto& rule : rules) {
        if (rule.isActive())
            return true;
    }
    return false;
}

bool FilterRuleSet::ruleMatches(int ruleIndex, const LogEntry& entry) const
{
    const FilterRule& rule = rules[ruleIndex];
    const int fieldIndex = (ruleIndex < m_boundFieldIndexes.size())
        ? m_boundFieldIndexes[ruleIndex] : -1;

    // Область поиска: значение поля (zero-copy view) или вся строка.
    // Строка без запрошенного поля (continuation line) = «не содержит».
    QStringView scope;
    bool scopeAvailable = true;
    if (m_fieldScopeActive && fieldIndex >= 0) {
        scope = entry.fields.get(fieldIndex, entry.message);
        scopeAvailable = !scope.isEmpty();
    } else {
        scope = QStringView(entry.message);
    }

    bool contains = false;
    if (scopeAvailable) {
        if (rule.isRegex) {
            const QRegularExpression& re = m_compiledRegexes.value(ruleIndex);
            contains = re.isValid() && re.matchView(scope).hasMatch();
        } else {
            contains = scope.contains(rule.text,
                rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
        }
    }
    return (rule.action == FilterRule::Action::Exclude) ? !contains : contains;
}

bool FilterRuleSet::matches(const LogEntry& entry) const
{
    // AND связывает сильнее OR: правила разбиваются OR-коннекторами на
    // AND-группы; запись проходит, если истинна хотя бы одна группа.
    // Однопроходная схема: groupResult аккумулирует текущую AND-группу;
    // на OR-границе группа закрывается.
    bool anyActiveSeen = false;
    bool groupResult   = true;

    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& rule = rules[i];
        if (!rule.isActive())
            continue;
        // Невалидный регекс — нейтральное правило: не валит весь лог.
        if (rule.isRegex && !m_compiledRegexes.value(i).isValid())
            continue;

        if (anyActiveSeen && rule.connector == FilterRule::Connector::Or) {
            if (groupResult)
                return true;          // предыдущая AND-группа истинна
            groupResult = true;       // начинаем новую группу
        }

        if (groupResult)
            groupResult = ruleMatches(i, entry);
        // groupResult == false: группа уже провалена — правила до следующего
        // OR можно не вычислять (short-circuit), но пропускать их всё равно
        // нужно через цикл, чтобы найти OR-границу.

        anyActiveSeen = true;
    }

    return !anyActiveSeen || groupResult;
}

QVector<HighlightPattern> FilterRuleSet::highlightPatterns() const
{
    QVector<HighlightPattern> patterns;
    for (const auto& rule : rules) {
        if (!rule.isActive() || rule.action != FilterRule::Action::Include
            || !rule.highlightEnabled)
            continue;
        HighlightPattern p;
        p.text            = rule.text;
        p.color           = rule.highlightColor;
        p.isRegex         = rule.isRegex;
        p.caseSensitivity = rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
        patterns.append(p);
    }
    return patterns;
}

QJsonObject FilterRuleSet::toJson() const
{
    QJsonArray rulesArray;
    for (const auto& rule : rules)
        rulesArray.append(rule.toJson());

    QJsonObject json;
    json[QStringLiteral("rules")] = rulesArray;
    return json;
}

FilterRuleSet FilterRuleSet::fromJson(const QJsonObject& json)
{
    FilterRuleSet set;
    const QJsonArray rulesArray = json[QStringLiteral("rules")].toArray();
    set.rules.reserve(rulesArray.size());
    for (const auto& value : rulesArray)
        set.rules.append(FilterRule::fromJson(value.toObject()));

    // Миграция старого формата: глобальный флаг caseSensitive набора
    // переносится на правила, у которых нет собственного значения.
    if (json[QStringLiteral("caseSensitive")].toBool(false)) {
        for (auto& rule : set.rules) {
            // В старом формате у правил ключа caseSensitive не было —
            // fromJson выставил false; глобальный true имеет приоритет.
            rule.caseSensitive = true;
        }
    }
    return set;
}

bool FilterRuleSet::operator==(const FilterRuleSet& other) const
{
    if (rules.size() != other.rules.size())
        return false;
    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& a = rules[i];
        const FilterRule& b = other.rules[i];
        if (a.enabled != b.enabled || a.action != b.action || a.connector != b.connector
            || a.text != b.text || a.fieldName != b.fieldName
            || a.caseSensitive != b.caseSensitive || a.isRegex != b.isRegex
            || a.highlightColor != b.highlightColor
            || a.highlightEnabled != b.highlightEnabled)
            return false;
    }
    return true;
}
