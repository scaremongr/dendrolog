#ifndef FIELDEXTRACTOR_H
#define FIELDEXTRACTOR_H

#include "keyedline.h"
#include "logfield.h"

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <memory>


// ============================================================================
// FieldExtractor — «поля строки = функция (текст строки, схема)» (F-17,
// docs/design/F-17.md, раздел 2). Один интерфейс для всех, кто разбирает
// строку на поля: шаблон строки (TemplateExtractor, linetemplate.h),
// ключевая схема JSON / logfmt (KeyedExtractor). Прежней блочной схемы
// (LogPattern) с шага 9 нет: её файлы переведены в шаблоны. Будущий извлекатель «строку
// разбирает модель» — ещё одна реализация с Cost::Background и своим
// extractBatch; чтение и view для него менять не придётся.
//
// Контракт:
//   • Неизменяемый после построения: все методы const и потокобезопасны,
//     извлекатель делится между потоками через FieldExtractorPtr.
//   • Чистая функция: ответ зависит только от текста строки. Состояния
//     между строками нет (продолжения записи — дело чтения, а не полей).
//   • Спаны — в координатах переданного текста (FieldSpan, logfield.h),
//     значение с экранированием (JSON, «quoted») — с битом escaped; читать
//     через LogEntryFields::value.
//   • Cost::Inline — можно звать на GUI-потоке (доли микросекунды на
//     строку, без аллокаций); Cost::Background — только в воркере: показ
//     (FieldSource, шаг 6) считает его в фоне, view показывает строку
//     сырой и обновляется по готовности; фильтр по полю с ним — всегда
//     фоновый. Инвариант: Background на GUI-потоке не вызывается.
//
// Пользователи сейчас: SchemaFit (мера соответствия), тесты. Шаг 3
// (FieldSource) переводит на него загрузку, view, фильтр и экспорт.
// ============================================================================

// Почему разбор строки остановился — данными (слова подставляет интерфейс:
// окно подбора, редактор, SchemaFit::explain). Элемент — номер элемента
// схемы: у шаблона — элемента шаблона (LineTemplate::elements), у блочной —
// блока, у ключевой — колонки.
struct MatchReport {
    enum class Expect {
        Nothing,      ///< строка принята целиком
        Literal,      ///< ожидался текст literal (литерал шаблона, обёртка, разделитель)
        Timestamp, Level, Number, Hex, Ip, Path, Text, Regex,
        Record,       ///< ключевая схема: строка не JSON / logfmt с ключом роли
        Uuid,         ///< шаблон: вид uuid
        Quoted,       ///< шаблон: строка в кавычках
        Value,        ///< шаблон: одно из значений списка of(…)
        Spaces,       ///< шаблон: пробел(ы)
        Word,         ///< шаблон: слово (word, words(N), padded, width(N) — непустой текст)
    };
    int afterBlock = -1;    ///< последний сошедшийся элемент, -1 — ни одного
    int expectedBlock = -1; ///< элемент, на котором разбор остановился
    Expect expect = Expect::Nothing;
    QString literal;        ///< Literal — какой текст; Value — значения через «|»
    QString field;          ///< имя поля, на котором остановился (шаблон), если это поле
    int stopOffset = 0;     ///< где разбор остановился
    QString found;          ///< текст строки с места остановки (до 24 символов)
    bool partial = false;   ///< строка принята с остановкой (поля слева сошлись)
};

class FieldExtractor {
public:
    enum class Cost { Inline, Background };
    // Итог разбора строки. Rejected — строка не принята, полей нет;
    // Partial — принята с остановкой посреди схемы (поля слева сошлись,
    // остаток строки — в хвостовом поле); Full — сошлась вся схема.
    enum class Outcome { Rejected, Partial, Full };
    static bool accepted(Outcome o) { return o != Outcome::Rejected; }

    // Где разбор остановился (для SchemaFit без второго прохода): элемент,
    // сошедшийся последним (-1 — ни одного), и смещение остановки (-1 —
    // не останавливался).
    struct Stop {
        int afterElement = -1;
        int offset = -1;
    };

    virtual ~FieldExtractor() = default;

    /// Схема годна (шаблон скомпилировался); негодная ничего не принимает.
    virtual bool isValid() const { return true; }
    /// Колонки схемы по порядку (имена — контракт с правилами фильтров).
    virtual QStringList fields() const = 0;
    virtual Cost cost() const = 0;
    /// Колонка, куда уходит свободный текст строки (обычно Message); -1 — нет.
    virtual int tailField() const { return -1; }
    /// Ключевая схема (JSON / logfmt): SchemaFit судит её по ролям.
    virtual bool isKeyed() const { return false; }

    /// Спаны по колонкам fields() (out пересоздаётся под их число; без
    /// аллокаций, если out уже нужного размера и не разделён). upTo >= 0 —
    /// колонки дальше upTo можно не разбирать: тогда точен только ответ
    /// «принята / нет», Full и Partial не различаются, колонки за upTo могут
    /// остаться пустыми.
    virtual Outcome extract(QStringView line, LogEntryFields* out, int upTo = -1,
                            Stop* stop = nullptr) const = 0;

    /// Пачкой; по умолчанию — цикл по extract. Для извлекателей, которым
    /// выгодна пачка (сеть).
    virtual void extractBatch(const QVector<QStringView>& lines, QVector<LogEntryFields>* out) const
    {
        out->resize(lines.size());
        for (int i = 0; i < lines.size(); ++i)
            extract(lines[i], &(*out)[i]);
    }

    /// Где и почему разбор остановился (одна строка, не горячий путь).
    virtual MatchReport explain(QStringView line) const = 0;
};

using FieldExtractorPtr = std::shared_ptr<const FieldExtractor>;

// ----------------------------------------------------------------------------
// Ключевая схема (F-4): строка JSON / logfmt, колонки по именам ключей —
// обёртка KeyedLine::Lookup (без аллокаций, Inline). Колонки — Timestamp,
// Level, Message, затем extraColumns. Распознанная строка — Full, иначе
// Rejected (частичного разбора у ключей нет).
// ----------------------------------------------------------------------------
class KeyedExtractor : public FieldExtractor {
public:
    explicit KeyedExtractor(const KeyedDefinition& definition);

    const KeyedDefinition& definition() const { return m_definition; }

    QStringList fields() const override { return m_columns; }
    Cost cost() const override { return Cost::Inline; }
    int tailField() const override { return KeyedDefinition::kMessageColumn; }
    bool isKeyed() const override { return true; }
    Outcome extract(QStringView line, LogEntryFields* out, int upTo = -1,
                    Stop* stop = nullptr) const override;
    MatchReport explain(QStringView line) const override;

private:
    KeyedDefinition m_definition;
    KeyedLine::Lookup m_lookup;
    QStringList m_columns;
};

#endif // FIELDEXTRACTOR_H
