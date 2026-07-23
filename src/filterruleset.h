#ifndef FILTERRULESET_H
#define FILTERRULESET_H

#include "logentry.h"
#include "textmatchhighlighter.h"

#include <QColor>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

// ============================================================================
// FilterRule / FilterRuleSet — декларативное описание текстовой фильтрации.
//
// Чистые данные + алгоритм, без QObject и без UI:
//   • FilterPanelWidget редактирует FilterRuleSet;
//   • LogModel исполняет его в passesFilters();
//   • MainWindow сериализует его в настройки (JSON).
//
// Семантика правила:
//   • action   — Include («строка должна содержать») или
//                Exclude («строка не должна содержать»);
//   • connector — логическая связь с ПРЕДЫДУЩИМ правилом (And/Or);
//                для первого правила игнорируется;
//   • fieldName — привязка к колонке схемы Log Fields. Пустая строка =
//                поиск по всей строке. Привязка действует только когда
//                fieldScopeActive (галочка "Filter blocks" включена);
//                иначе правило прозрачно ищет по всей строке.
//   • caseSensitive / isRegex — пер-правильные опции поиска (⚙ в UI).
//                Невалидный регекс делает правило нейтральным.
//   • enabled  — выключенное правило полностью нейтрально: не участвует
//                ни в фильтрации, ни в подсветке, но сохраняется.
//
// Приоритет операторов: AND связывает сильнее OR. Список правил разбивается
// OR-коннекторами на AND-группы; строка проходит, если истинна хотя бы
// одна группа:  A AND B OR C  ≡  (A ∧ B) ∨ C.
//
// Семантика ОДНА и та же для панели в режимах Filter и Search — различается
// только назначение результата (скрыть строки в основном view vs. вывести
// список в панель результатов), но не способ комбинирования правил.
//
// Правило, привязанное к колонке, на строке без этой колонки (например,
// continuation-строка многострочной записи) даёт «не содержит».
//
// Производительность: substring-правила — contains по QStringView без
// аллокаций; регексы компилируются один раз в prepare() (вызывается из
// bindFields), а не на каждую запись.
// ============================================================================

struct FilterRule {
    enum class Action    { Include, Exclude };
    enum class Connector { And, Or };

    bool      enabled       = true;
    Action    action        = Action::Include;
    Connector connector     = Connector::And; // связь с предыдущим правилом
    QString   text;
    QString   fieldName;                      // пустая = вся строка
    bool      caseSensitive = false;
    bool      isRegex       = false;
    QColor    highlightColor;                 // цвет подсветки совпадений этого правила
    // Раскрашивать ли совпадения этого правила (основной view + результаты).
    // Отдельный тогл-«глаз» в карточке; фильтрацию/поиск не затрагивает.
    bool      highlightEnabled = true;

    // Участвует ли правило в вычислениях (включено и имеет текст).
    bool isActive() const { return enabled && !text.isEmpty(); }

    QJsonObject toJson() const;
    static FilterRule fromJson(const QJsonObject& json);
};

class FilterRuleSet {
public:
    QVector<FilterRule> rules;

    // Привязать правила к текущей схеме полей и скомпилировать регексы.
    // Заполняет внутренние индексы колонок по fieldName и включает/выключает
    // колоночную область поиска. Вызывать перед передачей набора в LogModel;
    // при выключенном Log Fields передавать fieldScopeActive = false —
    // правила ищут по всей строке.
    void bindFields(const QStringList& fieldNames, bool fieldScopeActive);

    // Есть ли хоть одно действующее правило (иначе фильтр пропускает всё).
    bool isActive() const;
    // Сколько правил реально участвует в вычислениях — активных и, для
    // регексов, успешно скомпилированных. Осмысленно после bindFields():
    // расходится с isActive() ровно тогда, когда правило испорчено регексом.
    int usableRuleCount() const;

    // Проходит ли запись фильтр. Реализация без аллокаций.
    bool matches(const LogEntry& entry) const;
    // То же по «сырой» строке и её полям — для индексного бэкенда, где
    // LogEntry не материализуется (view валиден только на время вызова).
    bool matchesLine(QStringView message, const LogEntryFields& fields) const;

    // Активные Include-правила как паттерны для TextMatchHighlighter —
    // визуализация того, что именно оставило строку в выдаче.
    QVector<HighlightPattern> highlightPatterns() const;

    QJsonObject toJson() const;
    static FilterRuleSet fromJson(const QJsonObject& json);

    bool operator==(const FilterRuleSet& other) const;

private:
    // «Нашлось ли» правило в строке — без учёта Include/Exclude.
    bool ruleContains(int ruleIndex, QStringView message,
                      const LogEntryFields& fields) const;
    // То же с применением действия правила (Exclude инвертирует).
    bool ruleMatches(int ruleIndex, QStringView message,
                     const LogEntryFields& fields) const;
    // Пригодно ли правило к вычислению (активно и, для регекса, скомпилировано).
    bool ruleUsable(int ruleIndex) const;

    // Производное состояние (заполняется bindFields, не сериализуется):
    QVector<int> m_boundFieldIndexes;            // индекс колонки (-1 = вся строка)
    QVector<QRegularExpression> m_compiledRegexes; // компилят для isRegex-правил
    bool m_fieldScopeActive = false;
};

#endif // FILTERRULESET_H
