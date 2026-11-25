#ifndef RECORDLOCATOR_H
#define RECORDLOCATOR_H

#include "recordid.h"

#include <QHash>
#include <QObject>
#include <QStringView>
#include <QVector>

#include <map>
#include <memory>
#include <optional>

class LogModel;
class LogEntry;

// ============================================================================
// RecordLocator — разрешение сохранённых идентификаторов строк в ключи
// (R-1, шаг 3, docs/design/R-1.md, «RecordLocator»). По одному на модель
// вкладки (владелец — LogModel, как у BookmarkStore и NavHistory: переживает
// конверсию бэкенда). Только GUI-поток; ядро, без виджетов.
//
// Два вида запросов, оба отвечают сигналом resolved(token, key) или
// refused(token, reason):
//   • request(SourceIdentity, RecordId) — идентификатор с диска (закладки и
//     память файла R-4, сессии R-5). Ждёт файл с тем же путём; источник
//     проверяется ОДИН раз на поколение строк файла — когда файл подключён
//     и пришла первая порция (или сразу, если файл уже читается/прочитан):
//     Changed → SourceChanged, Missing → SourceMissing, ни одна строка не
//     ищется. Valid / Ephemeral — строка ищется по номеру и сверяется по
//     отпечатку текста;
//   • reacquire(file, RecordId) — тот же LogFile в памяти, строки которого
//     сброшены и читаются заново (полная перезагрузка, конверсия бэкенда).
//     Источник НЕ проверяется: файл мог быть переписан, но строка с тем же
//     номером и текстом — та же строка (так вели себя закладки R-2 и
//     история R-7); решают номер и отпечаток.
//
// Питание — из тех же точек, что принимают порции (вкладка и тестовая
// обвязка tests/common/testdocuments.h):
//   • residentBatch — порция резидентного парсера В ПОРЯДКЕ ФАЙЛА (до
//     сортировки по времени); текст сверяется сразу;
//   • indexedRange — опубликованный индексатором диапазон строк (после
//     appendIndexedRows). Отпечаток сырых байтов строки считается в фоне
//     (QtConcurrent, свой дескриптор файла, K позиционных чтений, смещения
//     сняты из индекса на GUI-потоке), результат — на GUI-потоке, если
//     поколение файла не устарело;
//   • fileLoaded — проход закончился: ожидающие строки, которых в файле не
//     оказалось, — LineAbsent; идентичность источника снимается по концу
//     прочитанного (sourceFor); недописанная строка, ставшая полной, —
//     completed(token, RecordId) с отпечатком;
//   • fileReset / allReset (зовёт LogModel из invalidateFileRows и
//     конверсий) — строки сброшены: новое поколение, фоновые проверки файла
//     отменены, источник проверяется заново.
//
// Цена (инвариант 3): ожидающие идентификаторы файла лежат по номеру строки
// (std::map), порция — O(log K + найденные) плюс двоичный поиск строки в
// порции резидентного пути; саму порцию никто не просматривает
// (stats().examined — сколько ожидающих тронуто, тест на это закреплён).
// Памяти на строку документа нет.
//
// Поздние запросы — строка уже прошла (файл прочитан, а запрос пришёл
// после): одним отложенным проходом на файл. Индексный — O(K) чтений
// индекса; резидентный — линейный проход по записям стора (O(N), редкий
// путь: R-4 ставит запросы до загрузки).
//
// Ловушки:
//   • ключ (RecordKey) годен, пока строка не сброшена (см. recordid.h);
//     resolved приходит, когда строка уже показана модели;
//   • недописанная последняя строка (RecordId::hasText == false) находится
//     по одному номеру и дальше отслеживается до завершения (completed);
//   • поздний запрос и проверка индексного пути отвечают позже, чем пришла
//     порция: «занят» (isBusy / busyChanged) — вкладка считает это загрузкой.
// ============================================================================
class RecordLocator : public QObject {
    Q_OBJECT
public:
    enum class Refusal {
        SourceChanged,  // файл переписан (идентичность источника не совпала)
        SourceMissing,  // файла по пути нет или он не читается
        LineAbsent,     // строки с таким номером в файле нет
        TextMismatch,   // строка есть, но текст другой
    };
    static const char* refusalName(Refusal reason);

    explicit RecordLocator(LogModel& model);
    ~RecordLocator() override;

    // ---- Запросы ------------------------------------------------------------
    // Токен > 0; ответ — сигналом (никогда не внутри самого вызова).
    int request(const SourceIdentity& source, const RecordId& id);
    int reacquire(const LogFilePtr& file, const RecordId& id);
    // Строка key уже найдена, но недописана (id.hasText == false): сообщить
    // completed, когда её допишут. text — её текст сейчас (строка могла
    // оказаться полной: тогда completed придёт с концом текущего прохода).
    // Для полной строки — 0, ждать нечего.
    int watchCompletion(const RecordKey& key, const RecordId& id, QStringView text);
    void cancel(int token);
    bool isPending(int token) const;
    int pendingCount() const { return int(m_requests.size()); }
    // Идёт фоновая проверка или ждёт поздний проход.
    bool isBusy() const;

    // ---- Обратное направление ----------------------------------------------
    // Идентификатор строки на экране: номер и отпечаток текста; последняя
    // строка без '\n' — без отпечатка (RecordId::forLine(…, false)). Пока
    // резидентный проход по файлу идёт, последняя пришедшая строка считается
    // недописанной (оборвана ли она, парсер скажет в конце прохода).
    RecordId idFor(const RecordKey& key, QStringView text) const;
    // Идентичность файла по концу прочитанного (снята на последнем
    // fileLoaded); файл ещё не дочитывался — по текущему размеру.
    std::optional<SourceIdentity> sourceFor(const LogFilePtr& file) const;
    // Последняя строка файла без '\n' (0 — нет).
    qint64 provisionalLine(const LogFile* file) const;

    // ---- Питание (вкладка, тестовая обвязка) --------------------------------
    void residentBatch(const QVector<std::shared_ptr<LogEntry>>& batch);
    void indexedRange(const LogFilePtr& file, qint64 firstLine, qint64 count);
    // consumedBytes < 0 — текущий размер; provisionalLine — номер последней
    // строки без '\n' (0 — нет; индексному пути его считает сам локатор).
    void fileLoaded(const LogFilePtr& file, qint64 consumedBytes = -1, qint64 provisionalLine = 0);
    void fileReset(const QString& filePath);
    void allReset();

    // ---- Для тестов -----------------------------------------------------------
    struct Stats {
        qint64 feeds = 0;        // порций и диапазонов
        qint64 examined = 0;     // ожидающих идентификаторов тронуто порциями
        qint64 sourceChecks = 0; // проверок идентичности источника
        qint64 byteChecks = 0;   // строк, сверенных в фоне по сырым байтам
        qint64 latePasses = 0;   // поздних проходов
    };
    const Stats& stats() const { return m_stats; }

signals:
    void resolved(int token, const RecordKey& key);
    void refused(int token, RecordLocator::Refusal reason);
    // Недописанная строка дописана: её полный идентификатор и ключ
    // (перечитанная строка могла стать началом записи — id и метка другие).
    void completed(int token, const RecordId& id, const RecordKey& key);
    void busyChanged(bool busy);

private:
    enum class Kind : quint8 { Saved, Memory, Watch };
    enum class Phase : quint8 {
        Waiting,   // Saved: файл с таким путём ещё не подключён
        Pending,   // в карте ожидающих файла (ждёт порцию)
        Checking,  // индексный путь: сверка байтов в фоне
        Late,      // строка уже прошла: ждёт позднего прохода
        Watching,  // найдена, недописана — ждёт завершения
    };
    struct Request {
        Kind kind = Kind::Saved;
        Phase phase = Phase::Waiting;
        SourceIdentity source;
        RecordId id;
        const LogFile* file = nullptr; // привязка (Pending и дальше)
        quint64 generation = 0;
        RecordKey key;                 // Watching: найденный ключ
        bool seen = false;             // Watching: строку видели в порции
        quint64 seenText = 0;          //   … и её отпечаток (резидентный путь)
    };
    struct FileState {
        LogFilePtr file;
        QString path;                  // ключ источника (sourceKey)
        quint64 generation = 1;
        qint64 maxLineFed = 0;         // строки поколения до этой — прошли
        bool loaded = false;           // поколение прочитано до конца
        bool feeding = false;          // идёт проход: порции после fileLoaded
        bool sourceChecked = false;    // Saved-запросы поколения проверены
        QHash<QString, SourceIdentity::State> checks; // идентичность → итог
        std::optional<SourceIdentity> source; // по концу прочитанного
        qint64 provisional = 0;        // резидентный путь: строка без '\n'
        std::map<qint64, QVector<int>> pending;  // строка → токены
        std::map<qint64, QVector<int>> watches;  // строка → токены
    };
    struct CheckItem {
        int token = 0;
        quint64 generation = 0;
        qint64 offset = 0;
        quint32 length = 0;
        bool completion = false;       // считать отпечаток для completed
    };
    struct CheckJob;

public:
    // Путь для сравнения: абсолютный, без «.» и «..», на Windows — без учёта
    // регистра. Так сравнивает пути локатор (и закладки — с ним).
    static QString pathKey(const QString& path);
    // Ключ источника (F-2): у обычного файла — pathKey(filePath), у сжатого —
    // путь архива [+ имя внутри]. По нему сохранённые запросы ждут свой файл.
    static QString sourceKey(const LogFile& file);
    static QString sourceKey(const SourceIdentity& source);

private:
    FileState* stateFor(const LogFilePtr& file, bool create);
    FileState* stateForPath(const QString& key);
    // Новые порции файла: проверить источник ожидающих по пути.
    void bindWaiting(FileState& fs);
    SourceIdentity::State checkSource(FileState& fs, const SourceIdentity& source);
    // Поставить запрос в файл: в карту ожидающих или в поздний проход.
    void place(int token, Request& r, FileState& fs);
    void unplace(int token, Request& r);
    void finishResolved(int token, Request& r, const RecordKey& key);
    void finishRefused(int token, Refusal reason);
    void scheduleLate();
    void runLate();
    void enqueueChecks(const QString& path, QVector<CheckItem> items);
    void startNextJob();
    void applyJob();
    void updateBusy();
    RecordKey indexedKey(const LogFile* file, qint64 line, qint64* offset, quint32* length) const;
    // Исходы копятся и доставляются после изменения состояния: из порций —
    // в конце вызова, из request()/reacquire() — отложенно (никогда внутри).
    struct Outcome {
        enum What : quint8 { Resolved, Refused, Completed } what = Resolved;
        int token = 0;
        RecordKey key;
        Refusal reason = Refusal::LineAbsent;
        RecordId id;
    };
    void scheduleFlush();
    void flush();

    LogModel& m_model;
    QHash<int, Request> m_requests;
    QHash<const LogFile*, FileState> m_files;
    QHash<QString, QVector<int>> m_waiting; // путь → Saved-токены (Waiting)
    QVector<int> m_late;
    bool m_lateScheduled = false;
    QVector<Outcome> m_outbox;
    bool m_flushScheduled = false;
    std::unique_ptr<CheckJob> m_job;        // идущая проверка
    QVector<std::pair<QString, QVector<CheckItem>>> m_jobQueue;
    bool m_busy = false;
    int m_nextToken = 1;
    Stats m_stats;
};

Q_DECLARE_METATYPE(RecordLocator::Refusal)

#endif // RECORDLOCATOR_H
