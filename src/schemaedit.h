#ifndef SCHEMAEDIT_H
#define SCHEMAEDIT_H

#include "linetemplate.h"
#include "schemafit.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

// ============================================================================
// SchemaEdit — правка шаблона строки «на строке» (F-17, шаг 10;
// docs/design/F-17.md, разделы 2 и 8). Ядро без виджетов: шаблон + образцы
// (+ выделение) → новый шаблон. Зовёт редактор схем (SchemaEditorWindow) из
// воркера: каждая операция компилирует и оценивает кандидатов мерой
// SchemaFit на образцах (до ~2300 строк выборки файла), на GUI-потоке её не
// звать.
//
// Правило одно: РЕЗУЛЬТАТ ВСЕГДА КОМПИЛИРУЕТСЯ. Операция, которой нечего
// предложить, возвращает ошибку словами, а шаблон не меняется.
//
//   • Make Field (makeField) — выделение внутри значения поля образца
//     становится новым полем. Текст поля до выделения — литералом (если он
//     одинаков) или прежним полем; после выделения — прежним полем. Где новое
//     поле кончается, решают те же свидетельства, что у вывода структуры
//     (раздел 2): знак сразу за выделением (литерал), форма самого
//     выделения, форма следом, промежуток из пробелов, постоянная позиция
//     (ширина), число слов (граница по разнообразию продолжений стоит после
//     того же слова в ≥ 98 % строк), закрытый набор значений (выделенное
//     плюс найденные на этом месте правилом разнообразия), конец строки.
//     Каждая гипотеза — шаблон-кандидат; порядок — подтверждённые
//     свидетельством первыми, затем покрытие (шаг SchemaFit::kCoverageStep),
//     затем меньше разных значений нового поля, затем порядок свидетельств.
//     Нет ни одного свидетельства — список из одного выделенного значения
//     (noEvidence, message словами; остальные значения — Add Value…).
//   • «Ends at» (endOptions) — все виды конца поля с итогом на образцах
//     («after 2 words — words(2): 14 values · 100%»).
//   • rename, hide (поле разбирается, но не колонка — «{_}»), remove (текст
//     поля уходит соседнему: предыдущему, у первого — следующему),
//     mergeWithNext, addValue / suggestValues (значения для списка — тем же
//     правилом разнообразия по строкам, где разбор остановился на поле).
//
// Номера колонок — как у LineTemplate::columns() исходного шаблона.
// Потоки: чистые функции, без состояния.
// ============================================================================

namespace SchemaEdit {

using Kind = LineTemplateElement::Kind;

constexpr double kAgree = 0.95;          ///< свидетельство у стольких строк
constexpr double kConstantWords = 0.98;  ///< постоянное число слов
constexpr double kCoverageLoss = 0.05;   ///< гипотеза подтверждена, если покрытие не ниже исходного − …
constexpr int kMaxAnchorWords = 4;
constexpr int kMaxValues = 30;
constexpr int kMinValueCount = 3;
constexpr int kMaxWidth = 64;

/// Элемент шаблона в виде, удобном для правки: участок {? … ?} — номер
/// группы у его элементов (opt), а не отдельный элемент.
struct Piece {
    using Type = LineTemplateElement::Type;
    Type type = Type::Literal;
    QString text;        ///< Literal
    QString name;        ///< Field: пусто — «{_}»
    Kind kind = Kind::Text;
    bool kindGiven = false;
    QStringList values;  ///< Of
    int count = 0;       ///< Width, Words
    QString regex;       ///< Regex
    int opt = -1;        ///< номер участка {? ?}, -1 — вне участка
    bool isField() const { return type == Type::Field; }
};

QVector<Piece> piecesOf(const LineTemplate& tpl);
/// Текст шаблона; вид поля пишется, если kindGiven.
QString render(const QVector<Piece>& pieces);
/// Как в шаблоне: «words(2)», «of(3 values)», «rest».
QString kindLabel(const Piece& field);

/// Свидетельство конца нового поля (порядок — порядок предпочтения при
/// равенстве покрытия и числа значений).
enum class Evidence {
    Literal,      ///< знак сразу за выделением
    Form,         ///< выделение само имеет форму (число, IP, метка…)
    FormFollows,  ///< следом токен с формой (уровень, метка, IP, UUID, кавычки)
    Gap,          ///< следом промежуток из двух и более пробелов
    Width,        ///< следующее поле начинается в одной и той же позиции
    Words,        ///< граница после того же по счёту слова
    Values,       ///< закрытый набор значений
    LineEnd,      ///< до конца строки
    SingleValue,  ///< нет свидетельства: список из одного выделенного значения
};
QString evidenceText(Evidence evidence);

struct Variant {
    Evidence evidence = Evidence::SingleValue;
    QString templateText;    ///< компилируется всегда
    QString field;           ///< имя нового поля
    int column = -1;         ///< колонка нового поля в новом шаблоне
    QString kind;            ///< kindLabel нового поля
    SchemaFit::Result fit;   ///< на образцах
    bool supported = false;  ///< свидетельство есть (и покрытие не упало)
    int distinct() const { return fit.distinct.value(column, 0); }
    /// «at “]” — 3 values · 100%».
    QString summary() const;
};

/// a лучше b: подтверждённая свидетельством раньше; покрытие (шаг
/// SchemaFit::kCoverageStep); меньше разных значений нового поля; порядок
/// свидетельств.
bool better(const Variant& a, const Variant& b);

struct MakeFieldResult {
    QVector<Variant> variants;   ///< лучший первым
    bool noEvidence = false;     ///< первый — SingleValue
    QString message;             ///< словами (что выбрано или почему свидетельства нет)
    QString error;               ///< нечего сделать — шаблон прежний
    bool ok() const { return !variants.isEmpty(); }
};

struct Options {
    SchemaFit::Budget budget;
    const std::atomic_bool* cancel = nullptr;
};

/// Выделение [start, start + length) в строке line (образец таблицы) — новым
/// полем. Строка может не входить в samples (по ним оценка).
MakeFieldResult makeField(const QString& templateText, const QString& line, qsizetype start,
                          qsizetype length, const QVector<SchemaFit::Sample>& samples,
                          const Options& options = {});

/// Пункт меню «Ends at» колонки.
struct EndOption {
    enum class Choice { NextSpace, Literal, NextForm, WideGap, Width, Words, Values, Form, LineEnd };
    Choice choice = Choice::NextSpace;
    QString label;           ///< «after 2 words»
    QString kind;            ///< «words(2)»
    QString templateText;    ///< шаблон с этим видом (пусто — недоступно)
    SchemaFit::Result fit;
    int column = -1;
    bool available = false;
    bool current = false;    ///< вид поля сейчас такой
    QString reason;          ///< недоступно — почему
    int distinct() const { return fit.distinct.value(column, 0); }
    /// «words(2): 14 values · 100%».
    QString summary() const;
};
QVector<EndOption> endOptions(const QString& templateText, int column,
                              const QVector<SchemaFit::Sample>& samples, const Options& options = {});

/// Итог простой операции: новый шаблон (компилируется) или ошибка — шаблон
/// прежний.
struct Edit {
    QString templateText;
    QString error;
    bool ok() const { return error.isEmpty() && !templateText.isEmpty(); }
};
Edit rename(const QString& templateText, int column, const QString& name);
Edit hide(const QString& templateText, int column);
Edit remove(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
            const Options& options = {});
Edit mergeWithNext(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
                   const Options& options = {});
Edit addValue(const QString& templateText, int column, const QString& value);
/// Значения для списка колонки column (вид of) по строкам, где разбор
/// остановился на этом поле, — частые первыми, без уже перечисленных.
QStringList suggestValues(const QString& templateText, int column,
                          const QVector<SchemaFit::Sample>& samples, const Options& options = {});

} // namespace SchemaEdit

#endif // SCHEMAEDIT_H
