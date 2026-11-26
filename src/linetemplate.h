#ifndef LINETEMPLATE_H
#define LINETEMPLATE_H

#include "fieldextractor.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>

#include <memory>

// ============================================================================
// Шаблон строки (F-17, docs/design/F-17.md, раздел 2) — правило разбиения
// строки лога на поля одной строкой, похожей на строку лога:
//
//     {Timestamp:time} [{Thread}] {Level:level} {Logger:word} - {Message}
//
// Грамматика:
//     шаблон   := элемент*
//     элемент  := литерал | пробел | поле | необязательное
//     поле     := '{' имя? (':' вид)? '}'
//     необяз.  := '{?' элемент* '?}'        — без вложенности
//     литерал  := любой текст; '{{' и '}}' — сами фигурные скобки
//     пробел   := пробелы и табуляции шаблона = один или несколько пробелов
//                 и табуляций строки (в конце строки — и ни одного)
//     имя      := имя колонки (пробелы допустимы); пусто или '_' — участок
//                 разбирается, но колонкой не становится
//
// Вид поля отвечает на один вопрос — где поле кончается:
//   • там, где кончается его ФОРМА: time (все формы LineClassifier, с дробью
//     и зоной), level (kLevelWords), int, num, hex, uuid, ip (IPv4, IPv6),
//     path, quoted (значение без кавычек; \" и \\ — бит FieldSpan Quoted),
//     of(a|b|…) — одно из значений (с пробелами внутри; самое длинное;
//     «\|», «\)», «\\» — сами знаки), /регекс/ (извлекатель — Background);
//   • word — на пробеле либо раньше, на литерале, который может идти следом;
//   • text — на том, что стоит следом: литерале (с пробелами вокруг — ищется
//     целиком), необязательном участке или поле с формой (на границе слова,
//     где это поле сходится), первое вхождение; greedy — последнее;
//   • padded — на промежутке из двух и более пробелов (или табуляции);
//   • width(N) — через N знаков; words(N) — через N слов;
//   • rest — в конце строки.
// Вид не указан: перед литералом, необязательным участком или полем с
// формой — text; перед пробелом, за которым поле без формы, — word; в конце
// шаблона — rest. Шаблон, где конец поля не определён (text, за которым
// только пробел и поле без формы, или поле вплотную к полю без формы), НЕ
// КОМПИЛИРУЕТСЯ — движок не угадывает.
//
// Правила разбора (их шесть; возвратов нет, кроме проверки одного элемента
// в правилах 3 и 4 — цена линейная по длине строки):
//   1. Строка читается слева направо один раз; литерал — на своём месте,
//      поле берёт текст по своему виду.
//   2. Значение поля — без окружающих пробелов (кроме quoted и of).
//   3. text перед полем с формой кончается в первом месте на границе слова,
//      где это поле сходится.
//   4. Необязательный участок берётся, только если сошёлся он сам И элемент
//      сразу за ним; иначе пропускается, его поля пусты.
//   5. Разбор остановился посреди шаблона — поля, сошедшиеся слева,
//      остаются; остаток строки — в последнее поле rest. Строка принята
//      (Partial), если сошёлся хотя бы один отличительный элемент: поле с
//      формой или литерал от трёх знаков. Шаблон кончился, а в строке остался
//      текст, — тоже Partial.
//   6. Не сошёлся первый элемент — строка не принята, полей нет.
//
// Скомпилированный шаблон неизменяем; разбор — const, без аллокаций на
// строку (кроме вида /регекс/), потокобезопасен.
// ============================================================================

// Сканеры видов поля — длина токена С ПОЗИЦИИ pos, -1 — нет. Общие с
// LineClassifier (time, level — его функции) и выводом структуры (шаг 4);
// регекс-эталоны — PatternHeuristics.
namespace TokenScan {
qsizetype timeAt(QStringView line, qsizetype pos);     ///< LineClassifier::timestampLengthAt
qsizetype levelAt(QStringView line, qsizetype pos);    ///< LineClassifier::levelLengthAt
qsizetype intAt(QStringView line, qsizetype pos);      ///< [+-]?\d+, за ним не \w
qsizetype numAt(QStringView line, qsizetype pos);      ///< [+-]?\d+(\.\d+)?([eE][+-]?\d+)?, за ним не \w
qsizetype hexAt(QStringView line, qsizetype pos);      ///< (0x)?[0-9A-Fa-f]+, за ним не \w
qsizetype uuidAt(QStringView line, qsizetype pos);     ///< 8-4-4-4-12 hex, за ним не \w
qsizetype ipAt(QStringView line, qsizetype pos);       ///< IPv4 или IPv6 (с «::» и IPv4 в конце)
qsizetype pathAt(QStringView line, qsizetype pos);     ///< PatternHeuristics::filePathRegex
/// Строка в кавычках «"…"» с «\» -экранированием; длина — с кавычками;
/// escaped — внутри была обратная косая. Незакрытая — -1.
qsizetype quotedAt(QStringView line, qsizetype pos, bool* escaped = nullptr);
} // namespace TokenScan

struct LineTemplateElement {
    enum class Type { Literal, Space, Field, Optional };
    enum class Kind {
        Time, Level, Int, Num, Hex, Uuid, Ip, Path, Quoted, Of, Regex,   // с формой
        Word, Text, Greedy, Padded, Width, Words, Rest                    // без формы
    };

    Type type = Type::Literal;
    int sourcePos = 0;          ///< где элемент начинается в тексте шаблона
    // Literal
    QString text;
    // Field
    QString name;               ///< пусто — без колонки («_» и «{}»)
    int column = -1;            ///< номер колонки, -1 — без колонки
    Kind kind = Kind::Text;
    bool kindGiven = false;     ///< вид указан в шаблоне (иначе выведен)
    QStringList values;         ///< Of — как в шаблоне
    QStringList valuesByLength; ///< Of — по убыванию длины (берётся самое длинное)
    int count = 0;              ///< Width, Words
    QString regexSource;        ///< Regex — текст между «/»
    QRegularExpression regex;   ///< Regex
    QStringList stops;          ///< Word, Words: литералы, на которых слово кончается раньше пробела
    bool directForm = false;    ///< Text, Greedy: следом вплотную поле с формой (граница слова)
    bool toEnd = false;         ///< Text, Greedy: следом ничего (кроме пробела) — до конца строки
    // Optional: его элементы — следующие за ним, до end (не включая).
    int end = -1;
    int top = -1;               ///< элемент верхнего уровня, содержащий этот (сам он или его участок)

    bool isField() const { return type == Type::Field; }
    bool hasForm() const { return type == Type::Field && kind <= Kind::Regex; }
    static bool isFormKind(Kind k) { return k <= Kind::Regex; }
};

struct LineTemplateError {
    enum class Code {
        None,
        Empty,              ///< в шаблоне нет элементов
        UnclosedField,      ///< «{» без «}»
        UnmatchedBrace,     ///< «}» без «{» (литерал — «}}»)
        UnclosedOptional,   ///< «{?» без «?}»
        NestedOptional,     ///< «{?» внутри «{? … ?}»
        EmptyOptional,      ///< «{??}»
        UnknownKind,        ///< неизвестный вид поля
        BadArgument,        ///< width(N) / words(N) / of(…) с неверным аргументом
        BadRegex,           ///< /регекс/ не компилируется
        UndefinedEnd,       ///< у поля не сказано, где оно кончается
        RestNotLast,        ///< rest — не в конце шаблона
        DuplicateName,      ///< колонка с таким именем уже есть
    };
    Code code = Code::None;
    int position = -1;      ///< знак шаблона, где ошибка
    QString field;          ///< имя поля, если ошибка в поле
    QString message;        ///< для человека (английский — язык интерфейса)
    bool isError() const { return code != Code::None; }
};

// Файл схемы версии 4 (<configDir>/patterns/*.json, F-17):
//   {"name": …, "description": …, "version": 4, "type": "line-template",
//    "template": "…"}
// Чтение — здесь; каталог читает их с шага 3 (SchemaStore).
struct LineTemplateSchema {
    static constexpr int kVersion = 4;
    QString name;
    QString description;
    QString templateText;
    /// false — не файл шаблона (другая версия или тип, битый JSON, нет имени
    /// или шаблона); шаблон при этом НЕ проверяется — это LineTemplate.
    static bool read(const QByteArray& json, LineTemplateSchema* out, QString* error = nullptr);
    /// Определение схемы-шаблона (F-17, шаг 3: каталог SchemaStore, состояния
    /// вкладок): JSON версии 4 с типом line-template и непустым шаблоном,
    /// имя не обязательно. false — не определение шаблона.
    static bool templateOf(const QString& definition, QString* templateText);
    /// Каноническое определение: {"type":"line-template","version":4,
    /// "template":…} компактно — как SchemaEntry::schema блочной схемы.
    static QString definition(const QString& templateText);
};

class LineTemplate {
public:
    using Element = LineTemplateElement;
    using Outcome = FieldExtractor::Outcome;

    LineTemplate() = default;
    explicit LineTemplate(const QString& text);

    bool isValid() const { return m_valid; }
    const LineTemplateError& error() const { return m_error; }
    const QString& text() const { return m_text; }

    /// Колонки по порядку элементов.
    const QStringList& columns() const { return m_columns; }
    /// Колонка последнего поля rest (свободный текст строки), -1 — нет.
    int tailColumn() const { return m_tailColumn; }
    bool hasRegex() const { return m_hasRegex; }
    const QVector<Element>& elements() const { return m_elements; }

    /// Разбор строки (правила 1–6). out — спаны по columns(). upTo, stop —
    /// как у FieldExtractor::extract. report — причина остановки (не
    /// горячий путь). steps — счётчик шагов разбора (тест линейности):
    /// проверки позиций и прочитанные сканерами знаки.
    Outcome match(QStringView line, LogEntryFields* out, int upTo = -1,
                  FieldExtractor::Stop* stop = nullptr, MatchReport* report = nullptr,
                  qint64* steps = nullptr) const;

    /// Литерал для текста шаблона: «{» и «}» удвоены.
    static QString escapeLiteral(const QString& text);
    /// Имя вида в шаблоне («time», «of», …).
    static QString kindName(Element::Kind kind);

private:
    struct Ctx;
    bool parse();
    bool resolve();
    bool fail(LineTemplateError::Code code, int position, const QString& field, const QString& message);

    bool matchElement(int i, qsizetype& pos, Ctx& c) const;
    bool probe(int idx, qsizetype pos, Ctx& c, bool forText = false) const;
    qsizetype formLength(const Element& e, qsizetype pos, Ctx& c, bool* escaped) const;
    qsizetype textEnd(int i, qsizetype start, Ctx& c) const;
    bool stopAt(const Element& e, qsizetype pos, Ctx& c) const;

    QString m_text;
    QVector<Element> m_elements;
    QStringList m_columns;
    QVector<int> m_columnTop;   ///< по колонке — элемент верхнего уровня, где она
    int m_tailColumn = -1;
    int m_tailElement = -1;
    bool m_hasRegex = false;
    bool m_valid = false;
    LineTemplateError m_error;
};

// Извлекатель на шаблоне: Inline, с /регексом/ — Background.
class TemplateExtractor : public FieldExtractor {
public:
    explicit TemplateExtractor(std::shared_ptr<const LineTemplate> tpl);
    explicit TemplateExtractor(const QString& text);

    const LineTemplate& lineTemplate() const { return *m_template; }

    bool isValid() const override { return m_template->isValid(); }
    QStringList fields() const override { return m_template->columns(); }
    Cost cost() const override { return m_template->hasRegex() ? Cost::Background : Cost::Inline; }
    int tailField() const override { return m_template->tailColumn(); }
    Outcome extract(QStringView line, LogEntryFields* out, int upTo = -1,
                    Stop* stop = nullptr) const override
    {
        return m_template->match(line, out, upTo, stop);
    }
    MatchReport explain(QStringView line) const override;

private:
    std::shared_ptr<const LineTemplate> m_template;
};

#endif // LINETEMPLATE_H
