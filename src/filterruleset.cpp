#include "filterruleset.h"
#include "userregex.h"

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

void FilterRuleSet::bindFields(const QStringList& fieldNames)
{
    m_boundFieldIndexes.resize(rules.size());
    m_compiledRegexes.clear();
    m_compiledRegexes.resize(rules.size());

    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& rule = rules[i];
        m_boundFieldIndexes[i] = rule.fieldName.isEmpty()
            ? -1
            : static_cast<int>(fieldNames.indexOf(rule.fieldName));

        // Скомпилирован с JIT здесь, на потоке, собирающем правила (S-11):
        // копия набора в фильтр-воркере делит готовый шаблон и не компилирует его.
        if (rule.isRegex && !rule.text.isEmpty()) {
            m_compiledRegexes[i] = UserRegex::compile(
                rule.text, rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
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

int FilterRuleSet::usableRuleCount() const
{
    int count = 0;
    for (int i = 0; i < rules.size(); ++i) {
        if (ruleUsable(i))
            ++count;
    }
    return count;
}

bool FilterRuleSet::ruleUsable(int ruleIndex) const
{
    const FilterRule& rule = rules[ruleIndex];
    if (!rule.isActive())
        return false;
    // Невалидный регекс — нейтральное правило: не валит весь лог.
    if (rule.isRegex && !m_compiledRegexes.value(ruleIndex).isValid())
        return false;
    // Колонки нет в текущей схеме — тоже нейтральное правило, а не молчаливый
    // поиск по всей строке («Thread содержит worker-1» не должно находить
    // worker-1 в тексте сообщения). Вернётся колонка — привязка оживёт.
    if (fieldMissing(ruleIndex))
        return false;
    return true;
}

bool FilterRuleSet::fieldMissing(int ruleIndex) const
{
    return !rules[ruleIndex].fieldName.isEmpty()
        && m_boundFieldIndexes.value(ruleIndex, -1) < 0;
}

bool FilterRuleSet::needsFields() const
{
    for (int i = 0; i < rules.size(); ++i)
        if (!rules[i].fieldName.isEmpty() && ruleUsable(i))
            return true;
    return false;
}

bool FilterRuleSet::textContains(int ruleIndex, QStringView scope) const
{
    const FilterRule& rule = rules[ruleIndex];
    if (scope.isEmpty())
        return false;
    if (rule.isRegex) {
        const QRegularExpression& re = m_compiledRegexes.value(ruleIndex);
        return re.isValid() && re.matchView(scope).hasMatch();
    }
    return scope.contains(rule.text, rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
}

bool FilterRuleSet::ruleContains(int ruleIndex, QStringView message,
                                 FieldRecordCursor* record) const
{
    const FilterRule& rule = rules[ruleIndex];
    const int fieldIndex = (ruleIndex < m_boundFieldIndexes.size())
        ? m_boundFieldIndexes[ruleIndex] : -1;
    if (rule.fieldName.isEmpty() || fieldIndex < 0)
        return textContains(ruleIndex, message);

    // Правило по полю — на записи: ответ по первой строке записи, один раз.
    if (!record)
        return false;
    const int known = record->ruleState(ruleIndex);
    if (known >= 0)
        return known == 1;

    bool contains = false;
    const FileSchema* schema = record->schema();
    if (schema && schema->has(fieldIndex)) {
        const QString& head = record->headText();
        // Предфильтр: образца нет во всей строке — нет и в поле. Только для
        // подстроки и только если значение — дословный кусок строки (нет
        // обратной косой черты: JSON \uXXXX, \" и quoted раскодируются).
        const bool prefilter = !rule.isRegex && !head.contains(QLatin1Char('\\'));
        if (!prefilter
            || head.contains(rule.text, rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive)) {
            const LogEntryFields* fields = record->fieldsUpTo(fieldIndex);
            if (fields && fields->has(fieldIndex)) {
                // Экранированное значение (JSON, quoted) — раскодированным;
                // аллокация только у таких значений.
                if (fields->isEscaped(fieldIndex)) {
                    const QString decoded = fields->value(fieldIndex, head);
                    contains = textContains(ruleIndex, decoded);
                } else {
                    contains = textContains(ruleIndex, fields->get(fieldIndex, head));
                }
            }
        }
    }
    record->setRuleState(ruleIndex, contains);
    return contains;
}

bool FilterRuleSet::ruleMatches(int ruleIndex, QStringView message,
                                FieldRecordCursor* record) const
{
    const bool contains = ruleContains(ruleIndex, message, record);
    return (rules[ruleIndex].action == FilterRule::Action::Exclude) ? !contains : contains;
}

bool FilterRuleSet::matches(const LogEntry& entry, FieldRecordCursor* record) const
{
    if (record) {
        const LogEntry* head = entry.recordHead() ? entry.recordHead().get() : &entry;
        record->setLine(quint64(quintptr(head)), entry.sourceFile().get(),
                        head->originalLineNumber(), &head->message());
    }
    return matchesLine(QStringView(entry.message()), record);
}

bool FilterRuleSet::matchesLine(QStringView message, FieldRecordCursor* record) const
{
    // AND связывает сильнее OR: правила разбиваются OR-коннекторами на
    // AND-группы; запись проходит, если истинна хотя бы одна группа.
    // Однопроходная схема: groupResult аккумулирует текущую AND-группу;
    // на OR-границе группа закрывается.
    bool anyActiveSeen = false;
    bool groupResult   = true;

    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& rule = rules[i];
        if (!ruleUsable(i))
            continue;

        if (anyActiveSeen && rule.connector == FilterRule::Connector::Or) {
            if (groupResult)
                return true;          // предыдущая AND-группа истинна
            groupResult = true;       // начинаем новую группу
        }

        if (groupResult)
            groupResult = ruleMatches(i, message, record);
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
    for (int i = 0; i < rules.size(); ++i) {
        const FilterRule& rule = rules[i];
        // Нейтральное правило (невалидный регекс, колонки нет в схеме) ничего
        // не находит — и подсвечивать ему нечего.
        if (!ruleUsable(i) || rule.action != FilterRule::Action::Include
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
