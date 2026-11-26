#ifndef FILTERRULESET_H
#define FILTERRULESET_H

#include "fieldsource.h"
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
//   • fieldName — привязка к колонке вкладки (Log Fields) по ИМЕНИ. Пустая
//                строка = поиск по всей строке. Колонка есть в схеме вкладки
//                — правило ищет в поле (при любом показе полей, F-17, шаг 3);
//                нет — правило нейтрально (fieldMissing, карточка это
//                показывает).
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
// Правило по полю решается на ЗАПИСИ (F-17, шаг 3): по полям первой строки
// записи, и решение действует на все её строки — «Thread содержит main»
// оставляет стек-трейс своей записи, Exclude убирает его вместе с записью.
// Правила «по всей строке» — построчные. Поля даёт FieldRecordCursor
// (fieldsource.h) лениво: поле разбирается один раз на запись и только если
// правило до него дошло; ответ правила на записи запоминается. Первая
// строка записи, не принятая схемой, или схема файла без этой колонки —
// «не содержит».
//
// Производительность: substring-правила — contains по QStringView без
// аллокаций; регексы компилируются один раз в bindFields, а не на каждую
// запись. Правило по полю без регекса сначала ищет образец во всей первой
// строке записи (предфильтр): нет — «не содержит» без разбора. Предфильтр
// верен, только пока значение поля — дословный кусок строки, поэтому на
// строке с обратной косой чертой (экранирование JSON и quoted) он
// пропускается. Разбор — до нужной колонки (FieldExtractor::extract, upTo).
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

    // Привязать правила к колонкам вкладки и скомпилировать регексы.
    // Заполняет внутренние индексы колонок по fieldName. Вызывать перед
    // передачей набора в LogModel.
    void bindFields(const QStringList& fieldNames);

    // Есть ли хоть одно действующее правило (иначе фильтр пропускает всё).
    bool isActive() const;
    // Сколько правил реально участвует в вычислениях — активных и, для
    // регексов, успешно скомпилированных. Осмысленно после bindFields():
    // расходится с isActive(), когда правило испорчено регексом или его
    // колонки нет в текущей схеме.
    int usableRuleCount() const;
    // Правило привязано к колонке, которой нет в колонках из bindFields.
    // Такое правило нейтрально — не ищет по всей строке; UI показывает это
    // явно.
    bool fieldMissing(int ruleIndex) const;
    // Есть ли действующее правило по полю (проходу нужен FieldRecordCursor).
    bool needsFields() const;

    // Проходит ли строка фильтр. record — поля её записи (курсор прохода,
    // запись строки уже сообщена ему — FieldRecordCursor::setLine);
    // nullptr — полей нет, правила по полю «не содержат». Без аллокаций,
    // кроме разбора поля и раскодирования экранированного значения.
    bool matchesLine(QStringView message, FieldRecordCursor* record) const;
    // Резидентная запись: сообщает курсору её запись (LogEntry::recordHead) и
    // проверяет строку.
    bool matches(const LogEntry& entry, FieldRecordCursor* record) const;

    // Активные Include-правила как паттерны для TextMatchHighlighter —
    // визуализация того, что именно оставило строку в выдаче.
    QVector<HighlightPattern> highlightPatterns() const;

    QJsonObject toJson() const;
    static FilterRuleSet fromJson(const QJsonObject& json);

    bool operator==(const FilterRuleSet& other) const;

private:
    // «Нашлось ли» правило в строке — без учёта Include/Exclude.
    bool ruleContains(int ruleIndex, QStringView message,
                      FieldRecordCursor* record) const;
    bool textContains(int ruleIndex, QStringView scope) const;
    // То же с применением действия правила (Exclude инвертирует).
    bool ruleMatches(int ruleIndex, QStringView message,
                     FieldRecordCursor* record) const;
    // Пригодно ли правило к вычислению: активно, регекс скомпилирован,
    // колонка (если задана) есть в схеме.
    bool ruleUsable(int ruleIndex) const;

    // Производное состояние (заполняется bindFields, не сериализуется):
    QVector<int> m_boundFieldIndexes;            // индекс колонки (-1 = вся строка или нет в схеме)
    QVector<QRegularExpression> m_compiledRegexes; // компилят для isRegex-правил
};

#endif // FILTERRULESET_H
