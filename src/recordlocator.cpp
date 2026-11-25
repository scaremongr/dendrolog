#include "recordlocator.h"

#include "indexedlogstore.h"
#include "residentlogstore.h"
#include "logentry.h"
#include "logmodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPair>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>

namespace {

RecordKey keyOfEntry(const LogEntry& e)
{
    RecordKey key;
    key.file = e.sourceFile();
    key.line = e.originalLineNumber();
    key.logicalEntryId = e.logicalEntryId();
    if (e.timestamp().isValid())
        key.timestampMs = e.timestamp().toMSecsSinceEpoch();
    return key;
}

// Ключ кэша итогов проверки источника: одна идентичность — одна проверка
// на поколение строк файла.
QString identityCacheKey(const SourceIdentity& s)
{
    return s.ephemeral ? QStringLiteral("ephemeral")
                       : QStringLiteral("%1:%2:%3").arg(s.size).arg(s.head).arg(s.tail);
}

using CheckResults = QVector<QPair<bool, quint64>>; // прочитано, отпечаток

} // namespace

// Фоновая сверка индексного пути: K позиционных чтений своим дескриптором.
// Воркер получает только свои копии (путь, смещения) и флаг отмены; локатор
// не трогает. Результат применяется на GUI-потоке (applyJob).
struct RecordLocator::CheckJob {
    QString path;
    QVector<CheckItem> items;
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
    QFutureWatcher<CheckResults> watcher;
};

const char* RecordLocator::refusalName(Refusal reason)
{
    switch (reason) {
    case Refusal::SourceChanged: return "SourceChanged";
    case Refusal::SourceMissing: return "SourceMissing";
    case Refusal::LineAbsent:    return "LineAbsent";
    case Refusal::TextMismatch:  return "TextMismatch";
    }
    return "?";
}

RecordLocator::RecordLocator(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
    qRegisterMetaType<RecordLocator::Refusal>();
}

RecordLocator::~RecordLocator()
{
    // Воркер держит только свои копии: ждать его не нужно, достаточно
    // попросить остановиться (закрытие вкладки посреди проверки).
    if (m_job)
        m_job->cancel->store(true);
}

QString RecordLocator::pathKey(const QString& path)
{
    QString key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toLower();
#endif
    return key;
}

QString RecordLocator::sourceKey(const LogFile& file)
{
    const LogSource& source = file.source();
    if (source.kind != LogSource::Kind::Archive)
        return pathKey(file.filePath);
    const QString key = pathKey(source.path);
    return source.member.isEmpty() ? key : key + QChar(0x01) + source.member;
}

QString RecordLocator::sourceKey(const SourceIdentity& source)
{
    const QString key = pathKey(source.path);
    return source.member.isEmpty() ? key : key + QChar(0x01) + source.member;
}

RecordLocator::FileState* RecordLocator::stateFor(const LogFilePtr& file, bool create)
{
    if (!file)
        return nullptr;
    const auto it = m_files.find(file.get());
    if (it != m_files.end())
        return &it.value();
    if (!create)
        return nullptr;
    FileState fs;
    fs.file = file;
    fs.path = sourceKey(*file); // сжатый источник — путь архива (F-2)
    return &m_files.insert(file.get(), fs).value();
}

RecordLocator::FileState* RecordLocator::stateForPath(const QString& key)
{
    for (auto it = m_files.begin(); it != m_files.end(); ++it)
        if (it->path == key)
            return &it.value();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Запросы
// ---------------------------------------------------------------------------

int RecordLocator::request(const SourceIdentity& source, const RecordId& id)
{
    const int token = m_nextToken++;
    Request r;
    r.kind = Kind::Saved;
    r.source = source;
    r.id = id;
    m_requests.insert(token, r);
    if (!id.isValid()) {
        finishRefused(token, Refusal::LineAbsent);
        scheduleFlush();
        return token;
    }
    if (!source.isValid()) {
        finishRefused(token, Refusal::SourceMissing);
        scheduleFlush();
        return token;
    }
    const QString key = sourceKey(source);
    m_waiting[key].append(token);
    // Файл с этим путём уже читается или прочитан — проверяем сразу. Строки
    // файла сброшены и ещё не пришли (перезагрузка, файл исчез) — ждём
    // первой порции нового поколения, как при подключении.
    if (FileState* fs = stateForPath(key); fs && (fs->maxLineFed > 0 || fs->loaded))
        bindWaiting(*fs);
    scheduleFlush();
    return token;
}

int RecordLocator::reacquire(const LogFilePtr& file, const RecordId& id)
{
    const int token = m_nextToken++;
    Request r;
    r.kind = Kind::Memory;
    r.id = id;
    Request& stored = m_requests.insert(token, r).value();
    FileState* fs = stateFor(file, true);
    if (!fs || !id.isValid()) {
        finishRefused(token, Refusal::LineAbsent);
        scheduleFlush();
        return token;
    }
    place(token, stored, *fs);
    scheduleFlush();
    return token;
}

int RecordLocator::watchCompletion(const RecordKey& key, const RecordId& id, QStringView text)
{
    if (!key.isValid() || id.hasText)
        return 0;
    FileState* fs = stateFor(key.file, true);
    if (!fs)
        return 0;
    const int token = m_nextToken++;
    Request r;
    r.kind = Kind::Watch;
    r.phase = Phase::Watching;
    r.id = id;
    r.key = key;
    r.file = key.file.get();
    r.generation = fs->generation;
    // Текст строки уже известен: если она окажется полной, конец прохода
    // сообщит completed и без её перечитывания.
    r.seen = true;
    r.seenText = RecordFingerprint::ofLineText(text);
    m_requests.insert(token, r);
    fs->watches[id.line].append(token);
    return token;
}

void RecordLocator::cancel(int token)
{
    const auto it = m_requests.find(token);
    if (it == m_requests.end())
        return;
    unplace(token, it.value());
    m_requests.erase(it);
    m_outbox.erase(std::remove_if(m_outbox.begin(), m_outbox.end(),
                                  [token](const Outcome& o) { return o.token == token; }),
                   m_outbox.end());
}

bool RecordLocator::isPending(int token) const
{
    return m_requests.contains(token);
}

bool RecordLocator::isBusy() const
{
    return m_job || !m_jobQueue.isEmpty() || !m_late.isEmpty();
}

void RecordLocator::updateBusy()
{
    const bool busy = isBusy();
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

// ---------------------------------------------------------------------------
// Обратное направление
// ---------------------------------------------------------------------------

qint64 RecordLocator::provisionalLine(const LogFile* file) const
{
    if (!file)
        return 0;
    if (const IndexedLogStore* store = m_model.indexedOrNull()) {
        const std::shared_ptr<LineIndex> index = store->indexForFile(file->filePath);
        return index && index->lastLineProvisional() ? index->lineCount() : 0;
    }
    const auto it = m_files.constFind(file);
    if (it == m_files.constEnd())
        return 0;
    // Проход идёт: оборвана ли последняя пришедшая строка, ещё неизвестно.
    return it->feeding && it->maxLineFed > it->provisional ? it->maxLineFed : it->provisional;
}

RecordId RecordLocator::idFor(const RecordKey& key, QStringView text) const
{
    if (!key.isValid())
        return {};
    return RecordId::forLine(key.line, text, key.line != provisionalLine(key.file.get()));
}

std::optional<SourceIdentity> RecordLocator::sourceFor(const LogFilePtr& file) const
{
    if (!file)
        return std::nullopt;
    const auto it = m_files.constFind(file.get());
    if (it != m_files.constEnd() && it->source)
        return it->source;
    return SourceIdentity::capture(*file);
}

// ---------------------------------------------------------------------------
// Привязка к файлу
// ---------------------------------------------------------------------------

SourceIdentity::State RecordLocator::checkSource(FileState& fs, const SourceIdentity& source)
{
    if (source.ephemeral)
        return SourceIdentity::State::Ephemeral;
    const QString key = identityCacheKey(source);
    const auto cached = fs.checks.constFind(key);
    if (cached != fs.checks.constEnd())
        return cached.value();
    ++m_stats.sourceChecks;
    // Путь — файла вкладки: идентичность сравнивает содержимое по нему (у
    // сжатого источника — по распакованной копии).
    const SourceIdentity::State state = source.checkContentAt(fs.file->filePath);
    fs.checks.insert(key, state);
    return state;
}

void RecordLocator::bindWaiting(FileState& fs)
{
    const auto it = m_waiting.find(fs.path);
    if (it == m_waiting.end())
        return;
    const QVector<int> tokens = std::move(it.value());
    m_waiting.erase(it);
    for (const int token : tokens) {
        const auto rit = m_requests.find(token);
        if (rit == m_requests.end())
            continue;
        Request& r = rit.value();
        switch (checkSource(fs, r.source)) {
        case SourceIdentity::State::Valid:
        case SourceIdentity::State::Ephemeral:
            place(token, r, fs);
            break;
        case SourceIdentity::State::Changed:
            finishRefused(token, Refusal::SourceChanged);
            break;
        case SourceIdentity::State::Missing:
            finishRefused(token, Refusal::SourceMissing);
            break;
        }
    }
}

void RecordLocator::place(int token, Request& r, FileState& fs)
{
    r.file = fs.file.get();
    r.generation = fs.generation;
    if (fs.loaded || r.id.line <= fs.maxLineFed) {
        // Строка этого поколения уже прошла (или файл прочитан целиком) —
        // отложенный поздний проход.
        r.phase = Phase::Late;
        m_late.append(token);
        scheduleLate();
        return;
    }
    r.phase = Phase::Pending;
    fs.pending[r.id.line].append(token);
}

void RecordLocator::unplace(int token, Request& r)
{
    const auto dropFrom = [token](std::map<qint64, QVector<int>>& map, qint64 line) {
        const auto node = map.find(line);
        if (node == map.end())
            return;
        node->second.removeAll(token);
        if (node->second.isEmpty())
            map.erase(node);
    };
    switch (r.phase) {
    case Phase::Waiting: {
        const auto it = m_waiting.find(sourceKey(r.source));
        if (it != m_waiting.end()) {
            it->removeAll(token);
            if (it->isEmpty())
                m_waiting.erase(it);
        }
        break;
    }
    case Phase::Pending:
        if (auto it = m_files.find(r.file); it != m_files.end())
            dropFrom(it->pending, r.id.line);
        break;
    case Phase::Watching:
        if (auto it = m_files.find(r.file); it != m_files.end())
            dropFrom(it->watches, r.id.line);
        break;
    case Phase::Late:
        m_late.removeAll(token);
        break;
    case Phase::Checking:
        break; // результат проверки отбросится: запроса больше нет
    }
}

void RecordLocator::finishResolved(int token, Request& r, const RecordKey& key)
{
    Outcome o;
    o.token = token;
    o.what = Outcome::Resolved;
    o.key = key;
    m_outbox.append(o);
    // Недописанная строка: найдена по номеру, дальше — до завершения.
    if (!r.id.hasText) {
        r.phase = Phase::Watching;
        r.key = key;
        r.seen = false;
        if (auto it = m_files.find(r.file); it != m_files.end())
            it->watches[r.id.line].append(token);
        return;
    }
    m_requests.remove(token);
}

void RecordLocator::finishRefused(int token, Refusal reason)
{
    Outcome o;
    o.token = token;
    o.what = Outcome::Refused;
    o.reason = reason;
    m_outbox.append(o);
    m_requests.remove(token);
}

void RecordLocator::scheduleFlush()
{
    if (m_flushScheduled || m_outbox.isEmpty())
        return;
    m_flushScheduled = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_flushScheduled = false;
        flush();
    }, Qt::QueuedConnection);
}

void RecordLocator::flush()
{
    // Слоты потребителей могут звать локатор (cancel, новые запросы):
    // исходы забираются целиком до первой доставки.
    while (!m_outbox.isEmpty()) {
        const QVector<Outcome> outcomes = std::exchange(m_outbox, {});
        for (const Outcome& o : outcomes) {
            switch (o.what) {
            case Outcome::Resolved:
                emit resolved(o.token, o.key);
                break;
            case Outcome::Refused:
                emit refused(o.token, o.reason);
                break;
            case Outcome::Completed:
                emit completed(o.token, o.id, o.key);
                break;
            }
        }
    }
    updateBusy();
}

// ---------------------------------------------------------------------------
// Питание
// ---------------------------------------------------------------------------

void RecordLocator::residentBatch(const QVector<std::shared_ptr<LogEntry>>& batch)
{
    qsizetype begin = 0;
    while (begin < batch.size()) {
        const std::shared_ptr<LogEntry>& head = batch[begin];
        if (!head) {
            ++begin;
            continue;
        }
        // Серия строк одного файла (порция парсера — один файл).
        qsizetype end = begin + 1;
        while (end < batch.size() && batch[end] && batch[end]->sourceFile() == head->sourceFile())
            ++end;
        const qsizetype runBegin = begin;
        begin = end;
        FileState* fs = stateFor(head->sourceFile(), true);
        if (!fs)
            continue;
        ++m_stats.feeds;
        bindWaiting(*fs);
        const qint64 lo = batch[runBegin]->originalLineNumber();
        const qint64 hi = batch[end - 1]->originalLineNumber();
        if (hi < lo)
            continue; // не в порядке файла — сюда такие не подают
        fs->maxLineFed = qMax(fs->maxLineFed, hi);
        fs->feeding = true;
        // Строка в серии — двоичным поиском по номеру (порядок файла).
        const auto entryAtLine = [&](qint64 line) -> const LogEntry* {
            const auto first = batch.cbegin() + runBegin;
            const auto last = batch.cbegin() + end;
            const auto it = std::lower_bound(first, last, line,
                [](const std::shared_ptr<LogEntry>& e, qint64 l) {
                    return e->originalLineNumber() < l;
                });
            return it != last && (*it)->originalLineNumber() == line ? it->get() : nullptr;
        };

        if (!fs->pending.empty()) {
            auto node = fs->pending.lower_bound(lo);
            while (node != fs->pending.end() && node->first <= hi) {
                const LogEntry* entry = entryAtLine(node->first);
                if (!entry) {
                    ++node; // пропуск в нумерации — решит fileLoaded
                    continue;
                }
                const RecordKey key = keyOfEntry(*entry);
                const QVector<int> tokens = std::move(node->second);
                node = fs->pending.erase(node);
                for (const int token : tokens) {
                    ++m_stats.examined;
                    const auto rit = m_requests.find(token);
                    if (rit == m_requests.end())
                        continue;
                    if (rit->id.matchesText(entry->message()))
                        finishResolved(token, rit.value(), key);
                    else
                        finishRefused(token, Refusal::TextMismatch);
                }
            }
        }
        // Недописанная строка перечитана: запомнить её текст до fileLoaded,
        // который скажет, завершена ли она.
        if (!fs->watches.empty()) {
            for (auto node = fs->watches.lower_bound(lo);
                 node != fs->watches.end() && node->first <= hi; ++node) {
                const LogEntry* entry = entryAtLine(node->first);
                if (!entry)
                    continue;
                const quint64 text = RecordFingerprint::ofLineText(entry->message());
                for (const int token : std::as_const(node->second)) {
                    ++m_stats.examined;
                    const auto rit = m_requests.find(token);
                    if (rit == m_requests.end())
                        continue;
                    rit->seen = true;
                    rit->seenText = text;
                    rit->key = keyOfEntry(*entry);
                }
            }
        }
    }
    flush();
}

RecordKey RecordLocator::indexedKey(const LogFile* file, qint64 line, qint64* offset,
                                    quint32* length) const
{
    const IndexedLogStore* store = m_model.indexedOrNull();
    return store ? store->keyForLine(file, line, offset, length) : RecordKey();
}

void RecordLocator::indexedRange(const LogFilePtr& file, qint64 firstLine, qint64 count)
{
    FileState* fs = stateFor(file, true);
    if (!fs || count <= 0)
        return;
    ++m_stats.feeds;
    bindWaiting(*fs);
    const qint64 lo = firstLine + 1; // индекс считает строки с 0
    const qint64 hi = firstLine + count;
    fs->maxLineFed = qMax(fs->maxLineFed, hi);
    fs->feeding = true;
    QVector<CheckItem> items;
    if (!fs->pending.empty()) {
        auto node = fs->pending.lower_bound(lo);
        while (node != fs->pending.end() && node->first <= hi) {
            qint64 offset = 0;
            quint32 length = 0;
            const RecordKey key = indexedKey(file.get(), node->first, &offset, &length);
            if (!key.isValid()) {
                ++node; // строка не показана модели — решит fileLoaded
                continue;
            }
            const QVector<int> tokens = std::move(node->second);
            node = fs->pending.erase(node);
            for (const int token : tokens) {
                ++m_stats.examined;
                const auto rit = m_requests.find(token);
                if (rit == m_requests.end())
                    continue;
                if (!rit->id.hasText) {
                    finishResolved(token, rit.value(), key);
                    continue;
                }
                rit->phase = Phase::Checking;
                rit->key = key;
                items.append({ token, fs->generation, offset, length, false });
            }
        }
    }
    if (!fs->watches.empty()) {
        for (auto node = fs->watches.lower_bound(lo);
             node != fs->watches.end() && node->first <= hi; ++node) {
            for (const int token : std::as_const(node->second)) {
                ++m_stats.examined;
                if (const auto rit = m_requests.find(token); rit != m_requests.end())
                    rit->seen = true;
            }
        }
    }
    if (!items.isEmpty())
        enqueueChecks(fs->file->filePath, std::move(items));
    flush();
}

void RecordLocator::fileLoaded(const LogFilePtr& file, qint64 consumedBytes, qint64 provisional)
{
    FileState* fs = stateFor(file, true);
    if (!fs)
        return;
    fs->source = SourceIdentity::capture(*file, consumedBytes);
    fs->loaded = true;
    fs->feeding = false;
    if (!m_model.isIndexedBackend())
        fs->provisional = provisional;
    // Пустой файл: порций не было, сохранённые запросы проверяются здесь.
    bindWaiting(*fs);
    // Проход закончился: ожидающих строк в файле нет.
    for (auto& node : fs->pending) {
        for (const int token : std::as_const(node.second)) {
            ++m_stats.examined;
            if (m_requests.contains(token))
                finishRefused(token, Refusal::LineAbsent);
        }
    }
    fs->pending.clear();

    // Недописанные строки, ставшие полными.
    const qint64 prov = provisionalLine(file.get());
    const bool indexed = m_model.isIndexedBackend();
    QVector<CheckItem> items;
    for (auto node = fs->watches.begin(); node != fs->watches.end();) {
        const qint64 line = node->first;
        if (line == prov) {
            ++node;
            continue;
        }
        QVector<int> keep;
        for (const int token : std::as_const(node->second)) {
            const auto rit = m_requests.find(token);
            if (rit == m_requests.end())
                continue;
            Request& r = rit.value();
            if (!r.seen) {
                keep.append(token); // строку ещё не перечитывали
                continue;
            }
            if (indexed) {
                qint64 offset = 0;
                quint32 length = 0;
                if (!indexedKey(file.get(), line, &offset, &length).isValid()) {
                    keep.append(token);
                    continue;
                }
                r.phase = Phase::Checking;
                r.key = indexedKey(file.get(), line, nullptr, nullptr);
                items.append({ token, fs->generation, offset, length, true });
                continue;
            }
            Outcome o;
            o.token = token;
            o.what = Outcome::Completed;
            o.id.line = line;
            o.id.text = r.seenText;
            o.id.hasText = true;
            o.key = r.key;
            m_outbox.append(o);
            m_requests.erase(rit);
        }
        if (keep.isEmpty()) {
            node = fs->watches.erase(node);
        } else {
            node->second = keep;
            ++node;
        }
    }
    if (!items.isEmpty())
        enqueueChecks(file->filePath, std::move(items));
    flush();
}

void RecordLocator::fileReset(const QString& filePath)
{
    // Путь чтения, а не ключ источника: сбрасываются строки этого LogFile.
    const QString key = pathKey(filePath);
    bool any = false;
    for (auto it = m_files.begin(); it != m_files.end(); ++it) {
        if (pathKey(it->file->filePath) != key)
            continue;
        any = true;
        FileState& fs = it.value();
        ++fs.generation;
        fs.maxLineFed = 0;
        fs.loaded = false;
        fs.feeding = false;
        // Идентичность прежнего содержимого: до конца нового прохода
        // sourceFor снимает её по текущему файлу.
        fs.source.reset();
        fs.checks.clear();
        fs.provisional = 0;
        // Запросы файла — к новому поколению: сохранённые снова ждут проверки
        // источника, найденные в памяти — новых порций; слежение за
        // недописанной строкой теряет смысл (её строка сброшена).
        QVector<int> tokens;
        for (const auto& node : fs.pending)
            tokens += node.second;
        for (const auto& node : fs.watches)
            tokens += node.second;
        for (auto rit = m_requests.begin(); rit != m_requests.end(); ++rit)
            if (rit->file == fs.file.get()
                && (rit->phase == Phase::Checking || rit->phase == Phase::Late))
                tokens.append(rit.key());
        fs.pending.clear();
        fs.watches.clear();
        for (const int token : std::as_const(tokens)) {
            const auto rit = m_requests.find(token);
            if (rit == m_requests.end())
                continue;
            Request& r = rit.value();
            if (r.phase == Phase::Late)
                m_late.removeAll(token);
            if (r.kind == Kind::Watch) {
                m_requests.erase(rit);
                continue;
            }
            if (r.kind == Kind::Saved) {
                r.phase = Phase::Waiting;
                r.file = nullptr;
                m_waiting[key].append(token);
                continue;
            }
            place(token, r, fs);
        }
        if (m_job && m_job->path == fs.file->filePath)
            m_job->cancel->store(true);
        // Ждущие задания файла сверяли бы смещения прежнего индекса.
        m_jobQueue.erase(std::remove_if(m_jobQueue.begin(), m_jobQueue.end(),
                                        [&fs](const auto& job) { return job.first == fs.file->filePath; }),
                         m_jobQueue.end());
    }
    if (any)
        updateBusy();
}

void RecordLocator::allReset()
{
    QStringList paths;
    for (const FileState& fs : std::as_const(m_files))
        paths.append(fs.file->filePath);
    for (const QString& path : std::as_const(paths))
        fileReset(path);
}

// ---------------------------------------------------------------------------
// Поздний проход
// ---------------------------------------------------------------------------

void RecordLocator::scheduleLate()
{
    if (m_lateScheduled)
        return;
    m_lateScheduled = true;
    updateBusy();
    QMetaObject::invokeMethod(this, [this]() {
        m_lateScheduled = false;
        runLate();
    }, Qt::QueuedConnection);
}

void RecordLocator::runLate()
{
    const QVector<int> tokens = std::exchange(m_late, {});
    if (tokens.isEmpty()) {
        updateBusy();
        return;
    }
    ++m_stats.latePasses;
    // Запросы по файлам.
    QHash<const LogFile*, QVector<int>> byFile;
    for (const int token : tokens) {
        const auto rit = m_requests.find(token);
        if (rit == m_requests.end() || rit->phase != Phase::Late)
            continue;
        byFile[rit->file].append(token);
    }
    const IndexedLogStore* store = m_model.indexedOrNull();
    if (store) {
        for (auto it = byFile.cbegin(); it != byFile.cend(); ++it) {
            const auto fit = m_files.find(it.key());
            if (fit == m_files.end())
                continue;
            QVector<CheckItem> items;
            for (const int token : it.value()) {
                Request& r = m_requests[token];
                qint64 offset = 0;
                quint32 length = 0;
                const RecordKey key = indexedKey(it.key(), r.id.line, &offset, &length);
                if (!key.isValid()) {
                    finishRefused(token, Refusal::LineAbsent);
                    continue;
                }
                if (!r.id.hasText) {
                    finishResolved(token, r, key);
                    continue;
                }
                r.phase = Phase::Checking;
                r.key = key;
                items.append({ token, fit->generation, offset, length, false });
            }
            if (!items.isEmpty())
                enqueueChecks(fit->file->filePath, std::move(items));
        }
    } else {
        // Резидентный: один проход по всем записям стора (редкий путь).
        QHash<const LogFile*, QVector<qint64>> lines;
        for (auto it = byFile.cbegin(); it != byFile.cend(); ++it) {
            QVector<qint64>& v = lines[it.key()];
            for (const int token : it.value())
                v.append(m_requests[token].id.line);
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }
        QHash<QPair<const LogFile*, qint64>, std::shared_ptr<LogEntry>> found;
        if (const ResidentLogStore* store = m_model.residentOrNull()) {
            for (auto it = lines.cbegin(); it != lines.cend(); ++it)
                for (const auto& e : store->entriesAtLines(it.key(), it.value()))
                    found.insert({ it.key(), e->originalLineNumber() }, e);
        }
        for (auto it = byFile.cbegin(); it != byFile.cend(); ++it) {
            for (const int token : it.value()) {
                Request& r = m_requests[token];
                const auto fit = found.constFind({ it.key(), r.id.line });
                if (fit == found.constEnd()) {
                    finishRefused(token, Refusal::LineAbsent);
                    continue;
                }
                if (r.id.matchesText(fit.value()->message()))
                    finishResolved(token, r, keyOfEntry(*fit.value()));
                else
                    finishRefused(token, Refusal::TextMismatch);
            }
        }
    }
    flush();
}

// ---------------------------------------------------------------------------
// Фоновая сверка индексного пути
// ---------------------------------------------------------------------------

void RecordLocator::enqueueChecks(const QString& path, QVector<CheckItem> items)
{
    m_jobQueue.append({ path, std::move(items) });
    startNextJob();
    updateBusy();
}

void RecordLocator::startNextJob()
{
    if (m_job || m_jobQueue.isEmpty())
        return;
    auto job = std::make_unique<CheckJob>();
    auto next = m_jobQueue.takeFirst();
    job->path = next.first;
    job->items = std::move(next.second);
    connect(&job->watcher, &QFutureWatcherBase::finished, this, &RecordLocator::applyJob);
    const QString path = job->path;
    const QVector<CheckItem> items = job->items;
    const std::shared_ptr<std::atomic_bool> cancel = job->cancel;
    job->watcher.setFuture(QtConcurrent::run([path, items, cancel]() {
        CheckResults results;
        results.reserve(items.size());
        // Свой дескриптор, без QIODevice::Text: отпечаток — по сырым байтам
        // (инвариант 5), декодер не нужен (RecordFingerprint::ofLineBytes).
        QFile file(path);
        const bool open = file.open(QIODevice::ReadOnly);
        for (const CheckItem& item : items) {
            if (cancel->load(std::memory_order_relaxed) || !open || !file.seek(item.offset)) {
                results.append({ false, 0 });
                continue;
            }
            const QByteArray bytes = file.read(item.length);
            if (bytes.size() != qsizetype(item.length)) {
                results.append({ false, 0 });
                continue;
            }
            results.append({ true, RecordFingerprint::ofLineBytes(bytes) });
        }
        return results;
    }));
    m_job = std::move(job);
}

void RecordLocator::applyJob()
{
    if (!m_job)
        return;
    const std::unique_ptr<CheckJob> job = std::move(m_job);
    const CheckResults results = job->watcher.result();
    const bool cancelled = job->cancel->load();
    for (qsizetype i = 0; i < job->items.size() && i < results.size(); ++i) {
        const CheckItem& item = job->items[i];
        const auto rit = m_requests.find(item.token);
        if (rit == m_requests.end() || rit->phase != Phase::Checking)
            continue;
        Request& r = rit.value();
        const auto fit = m_files.find(r.file);
        if (cancelled || fit == m_files.end() || fit->generation != item.generation
            || r.generation != item.generation)
            continue; // поколение сменилось: запрос уже переставлен fileReset
        ++m_stats.byteChecks;
        const QPair<bool, quint64>& res = results[i];
        if (item.completion) {
            if (!res.first) {
                r.phase = Phase::Watching; // перечитаем при следующей дозаписи
                r.seen = false;
                fit->watches[r.id.line].append(item.token);
                continue;
            }
            Outcome o;
            o.token = item.token;
            o.what = Outcome::Completed;
            o.id.line = r.id.line;
            o.id.text = res.second;
            o.id.hasText = true;
            o.key = r.key;
            m_outbox.append(o);
            m_requests.erase(rit);
            continue;
        }
        if (!res.first)
            finishRefused(item.token, Refusal::LineAbsent);
        else if (res.second == r.id.text)
            finishResolved(item.token, r, r.key);
        else
            finishRefused(item.token, Refusal::TextMismatch);
    }
    startNextJob();
    flush();
}
