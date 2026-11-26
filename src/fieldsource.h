#ifndef FIELDSOURCE_H
#define FIELDSOURCE_H

#include "fieldlayout.h"
#include "logfield.h"
#include "recordid.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringView>
#include <QVarLengthArray>
#include <QVector>

#include <atomic>
#include <memory>

struct LogFile;

// ============================================================================
// Источник полей (F-17, шаг 3; docs/design/F-17.md, раздел 3).
//
// Поля строки — функция (текст строки, схема её файла): в записях они не
// хранятся (LogEntry неизменяем после публикации), оба бэкенда получают их
// одним путём — FileSchema::extract. Здесь три входа:
//
//   • FieldSource — один на модель (владелец — LogModel, как RecordLocator),
//     только GUI-поток. fieldsFor — поля строки для показа (view, Entry
//     Details), с кэшем «файл + номер строки» на несколько тысяч строк
//     (вставки и фильтр его не портят: ключ — не номер ряда). setSchemas —
//     смена схем вкладки: поколение++, кэш пуст, O(1); строки не трогаются,
//     ни reset, ни прохода.
//   • FieldScanner — значение-снимок схем для воркеров (фильтр, Save View
//     As, поиск): extract зовётся прямо в воркере. Копируется дёшево
//     (указатели на неизменяемые FileSchema).
//   • FieldRecordCursor — правило фильтра по полю решается на ЗАПИСИ:
//     по полям её первой строки (раздел 3, «Правило по полю решается на
//     записи»). Курсор живёт один проход, помнит запись, её первую строку и
//     ответы правил по полю; поле разбирается один раз на запись и только
//     если правило до него дошло (FilterRuleSet::matchesLine).
//
// Класс цены извлекателя (FieldExtractor::Cost; шаг 6 F-17). Inline —
// fieldsFor считает сразу (и кладёт в кэш). Background (шаблон с /регексом/,
// будущий «строку разбирает модель») на GUI-потоке НЕ вызывается никогда:
// fieldsFor отдаёт Pending (показ — сырой текст строки) и ставит строку в
// очередь фонового расчёта. Очередь — «последние запрошенные первыми» с
// отсевом: запрошенные давно (прокрученные мимо) выбрасываются, когда
// очередь длиннее kQueueLines; текст строки задание получает значением.
// Одно задание на источник (вкладку) за раз, пачками до kBatchLines строк, в
// общем пуле с пониженным приоритетом потоков (backgroundPool); результат —
// QFutureWatcher на GUI-потоке, устаревшее поколение (смена схемы, сброс
// строк) отбрасывается. Готовые строки копятся и уходят одним сигналом
// fieldsReady не чаще раза в kReadyIntervalMs; модель превращает его в
// слитый dataChanged (роль FieldsReadyRole), view держит якорь.
//
// Счётчики (вызовы extract показа и сканеров, попадания в кэш) —
// для тестов бюджета: «смена схемы — ни прохода», «фильтр по полю — вызовов
// extract не больше строк, содержащих образец», «повторная отрисовка — 0».
// ============================================================================

struct FieldLookup {
    enum class State {
        None,     ///< у файла нет схемы или строка не принята — полей нет
        Ready,    ///< поля посчитаны
        Pending,  ///< Background-извлекатель считает в фоне (шаг 6)
    };
    State state = State::None;
    LogEntryFields fields;

    bool isReady() const { return state == State::Ready; }
};

// Счётчики источника; общие с его сканерами (воркеры дописывают атомарно).
struct FieldSourceCounters {
    std::atomic<qint64> guiExtracts{0};
    std::atomic<qint64> scanExtracts{0};
    std::atomic<qint64> cacheHits{0};
    // Фоновый путь (шаг 6): вызовы extract в задании и на каком потоке.
    std::atomic<qint64> backgroundExtracts{0};
};

class FieldScanner {
public:
    FieldScanner() = default;
    FieldScanner(FileSchemaSet schemas, std::shared_ptr<FieldSourceCounters> counters)
        : m_schemas(std::move(schemas)), m_counters(std::move(counters)) {}

    const FileSchemaSet& schemas() const { return m_schemas; }
    // Есть ли у вкладки хоть одна схема.
    bool hasSchemas() const;
    // Схема файла (своя или общая); nullptr — у файла нет схемы.
    const FileSchema* schemaFor(const LogFile* file) const
    {
        const FileSchema* schema = m_schemas.forFile(file);
        return (schema && schema->isValid()) ? schema : nullptr;
    }
    // Поля строки файла в колонках вкладки (FileSchema::extract); считает
    // вызов. Нет схемы — Rejected и пустые поля.
    FieldExtractor::Outcome extract(const FileSchema* schema, QStringView line,
                                    LogEntryFields* out, int upToColumn = -1) const;
    FieldExtractor::Outcome extract(const LogFile* file, QStringView line,
                                    LogEntryFields* out, int upToColumn = -1) const
    {
        return extract(schemaFor(file), line, out, upToColumn);
    }
    LogEntryFields fields(const LogFile* file, QStringView line) const
    {
        LogEntryFields out;
        extract(file, line, &out);
        return out;
    }

private:
    FileSchemaSet m_schemas;
    std::shared_ptr<FieldSourceCounters> m_counters;
};

// Поля ЗАПИСИ для правил фильтра по полю — на один проход (один поток).
// Вызывающий перед проверкой строки сообщает её запись (setLine); курсор
// сбрасывает память, когда запись сменилась. Текст первой строки записи —
// либо известен сразу (у самой первой строки — её текст; резидентная запись
// знает свою первую строку — LogEntry::recordHead), либо читается по
// требованию функцией fetch (индексный: строка lower_bound по id записи) —
// только если правилу по полю он понадобился.
class FieldRecordCursor {
public:
    using HeadFetch = QString (*)(void* context, const LogFile* file, qint64 line);

    explicit FieldRecordCursor(const FieldScanner* scanner, HeadFetch fetch = nullptr,
                               void* context = nullptr)
        : m_scanner(scanner), m_fetch(fetch), m_context(context) {}

    // Строка line (номер строки файла, для fetch) принадлежит записи token
    // файла file (token уникален среди записей прохода). headText — текст
    // первой строки записи, если известен (живёт, пока курсор на этой записи;
    // QString копируется без копии данных); nullptr — fetch по требованию.
    void setLine(quint64 token, const LogFile* file, qint64 line, const QString* headText)
    {
        if (m_hasRecord && token == m_token && file == m_file)
            return;
        m_hasRecord = true;
        m_token = token;
        m_file = file;
        m_line = line;
        m_schemaKnown = false;
        m_extractedUpTo = kNotExtracted;
        m_ruleState.clear();
        if (headText) {
            m_head = *headText;
            m_headKnown = true;
        } else {
            m_head.clear();
            m_headKnown = false;
        }
    }

    // Память ответов правил по полю на текущей записи: -1 — не считали.
    int ruleState(int rule) const
    {
        return rule < m_ruleState.size() ? m_ruleState[rule] : -1;
    }
    void setRuleState(int rule, bool contains)
    {
        while (m_ruleState.size() <= rule)
            m_ruleState.append(qint8(-1));
        m_ruleState[rule] = contains ? 1 : 0;
    }

    // Схема файла записи; nullptr — полей у записи нет.
    const FileSchema* schema()
    {
        if (!m_schemaKnown) {
            m_schema = m_scanner ? m_scanner->schemaFor(m_file) : nullptr;
            m_schemaKnown = true;
        }
        return m_schema;
    }
    // Текст первой строки записи (читается при первом обращении).
    const QString& headText();
    // Поля первой строки записи, разобранные хотя бы до колонки column
    // вкладки; nullptr — у схемы файла нет такой колонки или схемы нет.
    // Пустые спаны — строка не принята.
    const LogEntryFields* fieldsUpTo(int column);

private:
    static constexpr int kNotExtracted = -2;
    const FieldScanner* m_scanner = nullptr;
    HeadFetch m_fetch = nullptr;
    void* m_context = nullptr;

    bool m_hasRecord = false;
    quint64 m_token = 0;
    const LogFile* m_file = nullptr;
    qint64 m_line = 0;
    bool m_headKnown = false;
    QString m_head;
    bool m_schemaKnown = false;
    const FileSchema* m_schema = nullptr;
    int m_extractedUpTo = kNotExtracted;   // поле схемы, до которого разобрано
    LogEntryFields m_fields;
    QVarLengthArray<qint8, 8> m_ruleState;
};

class QThreadPool;
template <typename T> class QFutureWatcher;
class QTimer;

class FieldSource : public QObject {
    Q_OBJECT
public:
    explicit FieldSource(QObject* parent = nullptr);
    ~FieldSource() override;

    // Смена схем файлов вкладки: поколение++, кэш и очередь пусты, расчёт в
    // полёте отброшен. O(1), строки не трогаются. Тот же набор — ничего.
    void setSchemas(const FileSchemaSet& schemas);
    const FileSchemaSet& schemas() const { return m_schemas; }
    quint64 generation() const { return m_generation; }

    // Есть ли у вкладки схема с Background-извлекателем: фильтр по полю
    // с ней — всегда фоновый (LogModel::fieldRulesInBackground).
    bool hasBackgroundSchema() const { return m_hasBackground; }

    // GUI-поток: поля строки key (файл и номер строки) с текстом text для
    // показа. Inline — считает сразу (Ready / None); Background — из кэша
    // либо Pending и строка в очереди фонового расчёта, по готовности —
    // fieldsReady.
    FieldLookup fieldsFor(const RecordKey& key, QStringView text);

    // Снимок схем для воркера.
    FieldScanner scanner() const { return FieldScanner(m_schemas, m_counters); }

    // Строки файла сброшены (перечитывание, удаление) — кэш и очередь пусты,
    // расчёт в полёте отброшен. Ключ кэша — указатель файла и номер строки,
    // текст сверяется при попадании; сброс всего (событие редкое) проще,
    // чем выборочный.
    void fileReset(const QString& filePath);
    void allReset();

    // Фоновая работа ещё идёт: очередь, задание или готовые строки, ещё не
    // объявленные fieldsReady. Тестам и LogListView::hasPendingLayout.
    bool hasPendingWork() const;

    struct Stats {
        qint64 guiExtracts = 0;     ///< вызовов extract для показа (fieldsFor, GUI-поток)
        qint64 scanExtracts = 0;    ///< вызовов extract сканером (FieldScanner): фильтр,
                                    ///< экспорт, поиск — в воркере или синхронным
                                    ///< фильтром на GUI-потоке
        qint64 cacheHits = 0;       ///< fieldsFor ответил из кэша
        // Фоновый путь (шаг 6):
        qint64 backgroundExtracts = 0; ///< вызовов extract в фоновом задании
        qint64 queued = 0;          ///< строк поставлено в очередь
        qint64 dropped = 0;         ///< выброшено из очереди отсевом (прокручены мимо)
        qint64 batches = 0;         ///< запущено фоновых заданий
        qint64 discarded = 0;       ///< заданий, чей итог отброшен поколением
        qint64 readySignals = 0;    ///< испущено fieldsReady
    };
    Stats stats() const;
    void resetStats();
    int cachedLines() const { return int(m_recent.size() + m_old.size()); }
    int queuedLines() const { return int(m_queue.size()); }

    // Предел кэша: строк в каждом из двух поколений (LRU приближённо: на
    // переполнении «свежие» становятся «старыми», старые выбрасываются).
    static constexpr int kCacheLines = 4096;
    // Очередь фонового расчёта: экран и два вокруг (до ~256 строк на экран);
    // длиннее — выбрасываются запрошенные раньше всех.
    static constexpr int kQueueLines = 768;
    // Строк в одном фоновом задании (сеть выгодно звать пачкой).
    static constexpr int kBatchLines = 64;
    // Готовые строки объявляются не чаще раза в столько мс.
    static constexpr int kReadyIntervalMs = 30;

    // Общий пул фоновых заданий полей: потоки пониженного приоритета.
    static QThreadPool* backgroundPool();

signals:
    // Поля строк, посчитанные в фоне, готовы (модель превращает их в
    // слитый dataChanged). Не чаще раза в kReadyIntervalMs.
    void fieldsReady(const QVector<RecordKey>& keys);
    // Схемы сменились (setSchemas): видимое — перерисовать.
    void schemasChanged();
    // Фоновая работа отброшена сбросом строк (allReset / fileReset): ждущие
    // строк (Entry Details) запрашивают их заново — fieldsReady за
    // отброшенное не придёт.
    void backgroundDropped();

private:
    struct CacheKey {
        const LogFile* file = nullptr;
        qint64 line = 0;
        bool operator==(const CacheKey& o) const { return file == o.file && line == o.line; }
    };
    friend size_t qHash(const CacheKey& key, size_t seed) noexcept
    {
        return qHashMulti(seed, quintptr(key.file), key.line);
    }
    struct CacheItem {
        QString text;          // сверка: тот же указатель файла мог достаться другому файлу
        FieldLookup lookup;
    };
    // Строка в очереди фонового расчёта.
    struct QueueItem {
        RecordKey key;
        QString text;
        FileSchemaPtr schema;
        quint64 stamp = 0;     // когда запрошена последний раз (больше — свежее)
    };
    // Пачка задания: вход и итог (итог короче входа — задание отменили).
    struct BatchLine {
        QString text;
        FileSchemaPtr schema;
    };
    struct BatchResult {
        QVector<FieldLookup> lookups;
    };

    void insertCache(const CacheKey& key, const QString& text, const FieldLookup& lookup);
    void enqueue(const CacheKey& key, const RecordKey& recordKey, QStringView text,
                 const FileSchemaPtr& schema);
    void pruneQueue();
    void startBatch();
    void finishBatch();
    void flushReady();
    void dropBackground();   // поколение сменилось: очередь, задание, готовые — прочь

    FileSchemaSet m_schemas;
    bool m_hasBackground = false;
    quint64 m_generation = 0;
    std::shared_ptr<FieldSourceCounters> m_counters;
    QHash<CacheKey, CacheItem> m_recent;
    QHash<CacheKey, CacheItem> m_old;

    // Фоновый путь.
    QHash<CacheKey, QueueItem> m_queue;
    quint64 m_requestClock = 0;
    bool m_startPosted = false;
    // Задание в полёте: его строки (ключ, текст) и поколение.
    QVector<QueueItem> m_inFlight;
    QHash<CacheKey, int> m_inFlightIndex;
    quint64 m_inFlightGeneration = 0;
    QFutureWatcher<BatchResult>* m_watcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QVector<RecordKey> m_readyKeys;
    QTimer* m_readyTimer = nullptr;
    qint64 m_queued = 0, m_dropped = 0, m_batches = 0, m_discarded = 0, m_readySignals = 0;
};

#endif // FIELDSOURCE_H
