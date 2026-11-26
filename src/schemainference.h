#ifndef SCHEMAINFERENCE_H
#define SCHEMAINFERENCE_H

#include "schemafit.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

// ============================================================================
// SchemaInference — вывод шаблона строки по образцам (F-17, шаг 4;
// docs/design/F-17.md, раздел 6). Ядро без интерфейса: строки на экране и
// выборка файла SchemaFitJob → до трёх шаблонов-вариантов, каждый —
// скомпилированный LineTemplate с оценкой SchemaFit. Детерминированно,
// офлайн; время линейно по числу строк (счётчик steps); смотрится голова
// строки (kLineChars), окно поиска якоря — 240 знаков.
//
// Как выводится (результат — буквально последовательность литералов и
// полей; промежуточного представления нет):
//   1. Только строки-кандидаты (SchemaFit::corpus). Большинство строк —
//      JSON-объект или logfmt — не схема (Failure::KeyedLines).
//   2. Строки читаются слева направо в лад: у каждой — своя позиция. На
//      каждом шаге — что стоит на позиции у строк: конец, пробелы, токен с
//      ФОРМОЙ (сканеры видов шаблона, TokenScan: time, level, uuid, ip, int,
//      num, hex, quoted, path), знак-разделитель, слово.
//   3. Согласны ≥ 95 % строк — элемент шаблона (пробел, поле с формой,
//      литерал); согласны 15–95 % и элемент следом сходится у всех —
//      необязательный участок {? … ?} (повтор групп в скобках — Spring Boot
//      «[app] [thread]», «*N » nginx error, «[pid]» syslog); строки, не
//      согласные с обязательным элементом, выбывают (их судит мера).
//   4. Слова без согласия — поле до ЯКОРЯ: литерала или поля с формой, что
//      стоят следом в ≥ 95 % строк (ближайший; не дальше четырёх слов, кроме
//      закрывающей скобки). Постоянное число слов до якоря — столько полей
//      word; переменное — text (у формы следом — «трудная граница»).
//   5. Якоря нет — «трудная граница» раздела 2: по порядку промежуток из
//      двух пробелов (padded), выравнивание (width(N)), постоянное число
//      слов по разнообразию продолжений (words(N)), закрытый набор значений
//      (of(…)). Ничего — остаток строки — Message и заметка NoBoundary.
//   6. Имена по статистике колонки: Timestamp, Level, Thread, Logger, PID,
//      Id, Ip, Host, Program, Component, Message; остальные — Field1….
//   7. Варианты: полный; осторожный (до первой трудной границы, дальше
//      Message). Каждый компилируется (неудачный элемент — усечение до
//      Message) и оценивается; покрытие < 0,60 — отброшен. Не разобранные
//      строки сами дают шаблон с покрытием ≥ 0,90 — заметка TwoKinds.
//
// Потоки: чистая функция, без состояния; звать можно из воркера.
// ============================================================================

namespace SchemaInference {

constexpr double kAgree = 0.95;          ///< согласие строк в элементе
constexpr double kAgreeLoose = 0.85;     ///< обязательный элемент, если участок не вышел
constexpr double kOptionalMin = 0.15;    ///< доля строк необязательного участка
constexpr double kConstantWords = 0.98;  ///< постоянное число слов
constexpr double kMinCoverage = 0.60;    ///< вариант ниже — отброшен
constexpr int kMaxAnchorWords = 4;       ///< якорь не дальше стольких слов
constexpr int kMaxValues = 30;           ///< закрытый набор: значений не больше…
constexpr int kMinValueCount = 3;        ///< …каждое не реже
constexpr int kMaxFields = 32;
constexpr int kMaxLines = 4000;          ///< строк в выводе (дальше — равномерно)
constexpr int kMaxVariants = 3;

/// Заметка к варианту или выводу — данными; слова подставляет интерфейс
/// (text — английская подпись по умолчанию).
struct Note {
    enum class Kind {
        NoBoundary,   ///< после поля after — текст без устойчивой границы (не разбит)
        TwoKinds,     ///< в файле два вида строк; шаблон второго — otherTemplate
    };
    Kind kind = Kind::NoBoundary;
    QString after;          ///< NoBoundary: имя поля, за которым текст
    QString otherTemplate;  ///< TwoKinds: шаблон строк, которые вариант не разобрал
    double otherShare = 0;  ///< TwoKinds: доля таких строк среди кандидатов
    QString text;
};

enum class Failure {
    None,
    NoLines,       ///< нет строк-кандидатов
    KeyedLines,    ///< строки — JSON или logfmt: это формат строк, а не схема
    NoStructure,   ///< ничего, кроме Message (или покрытие ниже kMinCoverage)
    TimedOut,      ///< оценка вариантов не уложилась в бюджет SchemaFit
    Cancelled,
};

struct Variant {
    enum class Kind {
        Full,       ///< весь найденный заголовок
        Cautious,   ///< до первой трудной границы, дальше — Message
    };
    Kind kind = Kind::Full;
    QString templateText;    ///< компилируется всегда (LineTemplate::isValid)
    SchemaFit::Result fit;   ///< на всех строках входа
    QVector<Note> notes;
    int hardFields = 0;      ///< полей, найденных по свидетельству «трудной границы»
};

struct Options {
    /// Бюджет оценки SchemaFit одного варианта (без своего флага отмены
    /// берёт cancel).
    SchemaFit::Budget budget;
    /// Поднят — вывод прерывается (Failure::Cancelled, вариантов нет):
    /// проверяется на каждом шаге чтения строк и в оценке.
    const std::atomic_bool* cancel = nullptr;
};

struct Result {
    QVector<Variant> variants;   ///< лучший первым (SchemaFit::better), не больше kMaxVariants
    Failure failure = Failure::None;
    QString failureText;
    int candidates = 0;          ///< строк-кандидатов, по которым шёл вывод
    qint64 steps = 0;            ///< счётчик работы (тест линейности)
    bool ok() const { return !variants.isEmpty(); }
};

Result infer(const QVector<SchemaFit::Sample>& lines, const Options& options = {});
Result infer(const QStringList& lines, const Options& options = {});

QString failureText(Failure failure);

} // namespace SchemaInference

#endif // SCHEMAINFERENCE_H
