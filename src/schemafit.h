#ifndef SCHEMAFIT_H
#define SCHEMAFIT_H

#include "fieldextractor.h"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

struct LogFile;
class LogModel;
class LogScanSnapshot;

// ============================================================================
// SchemaFit — мера «насколько схема полей подходит строкам» (F-17, шаг 1;
// docs/design/F-17.md, раздел 1). ОДНА мера на всё приложение: подбор из
// каталога (rank), подпись покрытия в редакторе схем, проверка ответа
// нейро-подсказчика (шаг 2), вывод структуры (шаг 3), подбор при открытии
// (шаг 5). Второй меры нигде не заводить — тест schema_fit сверяет счётчики
// окна со счётчиками этой функции.
//
//   • Кандидат — строка, начинающая запись по правилу БЕЗ схемы
//     (LineClassifier::isPrimaryLine(false, …), время без даты — с
//     контекстом, F-11). Кандидатов меньше 20 % непустых строк (лог без
//     меток, JSON с эпохой числом) — кандидаты все непустые строки без
//     ведущего пробела и табуляции; если среди них строк со словом уровня
//     не меньше той же доли — только эти; если и их меньше, а кандидаты по
//     основному правилу есть (лог с длинными трассами) — остаются они.
//     Правило построчное (Corpus).
//   • Вердикт: «подходит» — покрытие ≥ 0,90 и specificity ≥ 2 (ключевая —
//     время или уровень в ≥ 90 % принятых); «сомнительно» — покрытие
//     ≥ 0,50, specificity ≥ 1 и полных не меньше частичных; иначе — «не
//     подходит».
//   • Покрытие — (full + 0,5·partial) / candidates: продолжения записи
//     (стек-трейс) схема принимать не должна, промахом они не считаются.
//   • loose — принятая строка, которая не кандидат (слишком мягкая схема),
//     specificity — среднее число непустых полей, кроме хвостового, на
//     принятого кандидата: схема из одного Message принимает всё, но её
//     specificity 0 — она не выбирается.
//   • Бюджет — проверка времени и флага отмены каждые 32 строки; превышен —
//     timedOut, проход остановлен, результат не используется (проход,
//     дошедший до конца, годен, сколько бы ни шёл) (защита от катастрофического
//     регекса пользователя или модели). Одна строка тяжёлого регекса
//     ограничена пределом PCRE2 (match limit), так что проверка не опаздывает
//     больше чем на 32 таких строки.
//
// Потоки: функции чистые, без состояния; извлекатель (FieldExtractor) можно
// разделять между потоками (как FileSchemaPtr у загрузки). Шаг 2 F-17: мера
// считается на интерфейсе FieldExtractor. SchemaFitJob — на GUI-потоке,
// считает в пуле (QtConcurrent::run + QFutureWatcher).
// ============================================================================

namespace SchemaFit {

// Пороги (раздел 1 F-17.md).
constexpr double kFitCoverage = 0.90;          ///< «подходит» — покрытие не ниже…
constexpr double kFitSpecificity = 2.0;        ///< …и специфичность не ниже
constexpr double kKeyedRoleShare = 0.90;       ///< ключевая: время или уровень в ≥ 90 % принятых
constexpr double kDoubtfulCoverage = 0.50;     ///< «сомнительно» — 0,50…0,90
constexpr double kMinSpecificity = 1.0;        ///< ниже — схема автоматически не выбирается
constexpr double kCandidateFallbackShare = 0.20;
constexpr double kCoverageStep = 0.02;         ///< better: покрытие округляется до шага
constexpr int kMaxMisses = 50;
constexpr int kDistinctCap = 1000;             ///< разных значений по колонке считается точно до…
constexpr int kBudgetCheckEvery = 32;
constexpr qint64 kDefaultBudgetMs = 150;

/// Бюджет одной оценки (одной схемы).
struct Budget {
    qint64 maxMs = kDefaultBudgetMs;  ///< <= 0 — без предела времени
    int checkEvery = kBudgetCheckEvery;
    /// Шов для теста: «сейчас» в мс (монотонно). Пусто — QElapsedTimer.
    std::function<qint64()> clock;
    /// Поднят — оценка прерывается (Result::cancelled).
    const std::atomic_bool* cancel = nullptr;
};

/// Строка-образец: номер физической строки файла (с 1; 0 — без файла).
struct Sample {
    qint64 line = 0;
    QString text;
};

/// Строки и их роль — считаются один раз на набор строк (rank оценивает
/// по ним все схемы).
struct Corpus {
    QVector<Sample> samples;
    QVector<bool> candidate;   ///< по строке
    QVector<bool> nonEmpty;    ///< по строке
    int nonEmptyCount = 0;
    int candidateCount = 0;
    bool fallbackRule = false; ///< кандидаты — по запасному правилу
};
Corpus corpus(const QVector<Sample>& samples);
Corpus corpus(const QStringList& lines);
/// Правило кандидата без запасного (для одной строки).
bool startsRecord(const QString& line);

/// Строка-кандидат, которую схема не приняла или приняла частично.
struct Miss {
    qint64 line = 0;        ///< номер строки файла, 0 — образец без файла
    QString text;
    int afterBlock = -1;    ///< последний сошедшийся блок (колонка у ключевой), -1 — ни одного
    int stopOffset = 0;     ///< где разбор остановился
    bool partial = false;   ///< принята деградацией
};

struct Result {
    int examined = 0;       ///< непустых строк
    int candidates = 0;
    int full = 0;
    int partial = 0;
    int missed = 0;
    int loose = 0;
    int keyedRoles = 0;     ///< ключевая схема: принятых строк с Timestamp или Level
    QVector<int> filled;    ///< по колонке: в скольких принятых строках поле не пусто
    /// По колонке: сколько РАЗНЫХ непустых значений (раскодированных) у
    /// принятых строк — точно до kDistinctCap, дальше kDistinctCap + 1
    /// («1000+», distinctText). Неверная граница поля видна сразу: вместо
    /// шести названий — сотни значений по одному разу (F-17, раздел 2).
    QVector<int> distinct;
    QVector<Miss> misses;   ///< не больше kMaxMisses, по порядку строк
    qint64 fieldsOnCandidates = 0; ///< сумма непустых полей (кроме хвостового) у принятых кандидатов
    bool keyed = false;
    bool timedOut = false;
    bool cancelled = false;
    bool valid = false;     ///< схема валидна и оценка дошла до конца

    double coverage() const;
    double specificity() const;
    int accepted() const { return full + partial + loose; }
    /// «412» или «1000+».
    QString distinctText(int column) const;
};

/// Мера на любом извлекателе (F-17, шаг 2) — основной путь.
Result evaluate(const FieldExtractor& schema, const Corpus& lines, const Budget& budget = {});
Result evaluate(const FieldExtractor& schema, const QStringList& lines, const Budget& budget = {});
Result evaluate(const FieldExtractor& schema, const QVector<Sample>& lines, const Budget& budget = {});
enum class Verdict { Fits, Doubtful, NoFit };
Verdict verdict(const Result& result);

/// a строго лучше b: досчитанная — раньше прерванной; схема со
/// specificity ≥ 1 и ненулевым покрытием — раньше остальных; покрытие,
/// округлённое до kCoverageStep; specificity; меньше partial. Равные —
/// false (порядок каталога решает вызывающий: rank устойчив).
bool better(const Result& a, const Result& b);

struct Ranked {
    int index = -1;         ///< номер схемы во входном списке
    Result result;
};
/// Все схемы по одним строкам, лучшая первой (устойчиво: равные — в
/// порядке списка); timedOut — в конце.
QVector<Ranked> rank(const QVector<FieldExtractorPtr>& schemas, const Corpus& lines,
                     const Budget& budget = {});

/// Причина промаха — данными; слова подставляет интерфейс (MatchReport,
/// fieldextractor.h: у шаблона — номер элемента шаблона, у блочной схемы —
/// блока).
using Reason = MatchReport;
/// Любой извлекатель (одна строка, не горячий путь).
Reason explain(const FieldExtractor& extractor, const QString& line);

/// Подпись для интерфейса: «Parsed 99.6% of 2300 record lines · 3 did not
/// match». Пустая — схема невалидна.
QString summary(const Result& result);
/// Доля в процентах для подписи: одна цифра после точки вниз (99,96 % —
/// «99.9», а не «100»), без «.0».
QString percent(double share);

} // namespace SchemaFit

// ============================================================================
// SchemaFitJob — проверка схем на файле вкладки в пуле (F-17, раздел 1).
// Снапшот всех строк модели (scanSnapshot(false)), строки ОДНОГО файла:
// первые kHeadLines и kSpreadLines равномерно по файлу; текст — LineCursor
// снапшота (индексный бэкенд читает диск в воркере, кэш вьюпорта не
// трогается). Поколение и флаг отмены; reset модели отбрасывает идущий
// расчёт (сигнал invalidated — вызывающий запускает заново). Ничего не
// считает на вставку строк (инвариант 3): запуск — по требованию.
// ============================================================================
class SchemaFitJob : public QObject {
    Q_OBJECT
public:
    static constexpr int kHeadLines = 300;
    static constexpr int kSpreadLines = 2000;

    struct Outcome {
        int generation = 0;
        QVector<SchemaFit::Result> results;   ///< по схеме, в порядке start()
        int sampled = 0;                      ///< строк в наборе
        bool cancelled = false;
    };

    explicit SchemaFitJob(QObject* parent = nullptr);
    ~SchemaFitJob() override;

    /// Источник строк. Reset модели — отмена и invalidated().
    void setModel(LogModel* model);
    LogModel* model() const { return m_model; }

    /// Запуск на снапшоте модели; идущий расчёт отменяется. file — файл,
    /// чьи строки берутся (nullptr — файл первой строки). Номер поколения.
    int start(const QVector<FieldExtractorPtr>& schemas, const LogFile* file = nullptr,
              const SchemaFit::Budget& budget = {});
    void cancel(bool wait = false);
    bool isRunning() const;
    int generation() const { return m_generation; }
    /// Итог последнего законченного расчёта текущего поколения.
    const Outcome& outcome() const { return m_outcome; }
    bool hasOutcome() const { return m_hasOutcome; }

    /// Набор строк (те же, что берёт задание) — для теста и синхронного
    /// пути. cancel может быть nullptr.
    static QVector<SchemaFit::Sample> sampleLines(const LogScanSnapshot& snapshot,
                                                  const LogFile* file,
                                                  const std::atomic_bool* cancel = nullptr,
                                                  int headLines = kHeadLines,
                                                  int spreadLines = kSpreadLines);

signals:
    void finished();
    void invalidated();

private:
    void onWatcherFinished();

    LogModel* m_model = nullptr;   ///< сбрасывается по destroyed модели
    QMetaObject::Connection m_resetConnection;
    QMetaObject::Connection m_destroyedConnection;
    QFutureWatcher<Outcome>* m_watcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
    Outcome m_outcome;
    bool m_hasOutcome = false;
};

#endif // SCHEMAFIT_H
