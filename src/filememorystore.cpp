#include "filememorystore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {

std::nullopt_t fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return std::nullopt;
}

const QLatin1String kJsonSuffix(".json");
constexpr QChar kDetachedMark = QLatin1Char('~');

QByteArray toBytes(const FileMemory& memory)
{
    return QJsonDocument(memory.toJson()).toJson(QJsonDocument::Indented);
}

std::optional<FileMemory> readMemoryFile(const QString& path, bool* newer)
{
    if (newer)
        *newer = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return std::nullopt;
    return FileMemory::fromJson(doc.object(), nullptr, newer);
}

bool containsRecord(const QVector<BookmarkState>& list, const RecordId& record)
{
    return std::any_of(list.cbegin(), list.cend(),
                       [&](const BookmarkState& b) { return b.record == record; });
}

// Фоновая запись одного файла памяти с объединением закладок (правило
// нескольких экземпляров — в заголовке).
void writeMerged(const QString& filePath, FileMemory ours, const QVector<RecordId>& base)
{
    bool newer = false;
    const auto theirs = readMemoryFile(filePath, &newer);
    if (newer)
        return; // писала новая программа — не трогаем
    const QString path = ours.source.source.path;
    if (theirs && theirs->source.source.checkAt(path) == SourceIdentity::State::Valid) {
        for (BookmarkState b : theirs->bookmarks) {
            if (base.contains(b.record) || containsRecord(ours.bookmarks, b.record))
                continue;
            b.source.path = path;
            ours.bookmarks.append(b);
        }
        if (!ours.view && theirs->view) {
            ours.view = theirs->view;
            if (ours.view->currentSource)
                ours.view->currentSource->path = path;
        }
    }
    FileMemoryStore::writeFileAtomically(filePath, toBytes(ours));
}

// Вытеснение: перепроверить на диске (другой экземпляр мог поставить закладки).
void removeIfEvictable(const QString& filePath)
{
    bool newer = false;
    const auto memory = readMemoryFile(filePath, &newer);
    if (newer || (memory && !memory->bookmarks.isEmpty()))
        return;
    QFile::remove(filePath);
}

} // namespace

// ---------------------------------------------------------------------------
// FileMemory
// ---------------------------------------------------------------------------

QJsonObject FileMemory::toJson() const
{
    QJsonObject json = source.toJson(); // "source", "encoding"
    json.insert(QStringLiteral("format"), QLatin1String(kFormatName));
    json.insert(QStringLiteral("version"), kFormatVersion);
    json.insert(QStringLiteral("hash"), QLatin1String(RecordFingerprint::kRegionAlgorithm));
    if (lastUsed.isValid())
        json.insert(QStringLiteral("lastUsed"), lastUsed.toUTC().toString(Qt::ISODateWithMs));
    QJsonArray marks;
    for (const BookmarkState& b : bookmarks)
        marks.append(b.toJson());
    json.insert(QStringLiteral("bookmarks"), marks);
    if (view)
        json.insert(QStringLiteral("view"), view->toJson());
    return json;
}

std::optional<FileMemory> FileMemory::fromJson(const QJsonObject& json, QString* error, bool* newerVersion)
{
    if (newerVersion)
        *newerVersion = false;
    if (json.value(QLatin1String("format")).toString() != QLatin1String(kFormatName))
        return fail(error, QStringLiteral("not a file memory"));
    const int version = json.value(QLatin1String("version")).toInt(-1);
    if (version > kFormatVersion) {
        if (newerVersion)
            *newerVersion = true;
        return fail(error, QStringLiteral("file memory version %1 is newer than %2")
                               .arg(version).arg(kFormatVersion));
    }
    if (version != kFormatVersion)
        return fail(error, QStringLiteral("unsupported file memory version"));

    FileMemory m;
    QString why;
    const auto source = TabSource::fromJson(json, &why);
    if (!source)
        return fail(error, why);
    if (source->source.ephemeral)
        return fail(error, QStringLiteral("source: ephemeral"));
    m.source = *source;

    const QJsonValue used = json.value(QLatin1String("lastUsed"));
    if (!used.isUndefined()) {
        m.lastUsed = QDateTime::fromString(used.toString(), Qt::ISODateWithMs);
        if (!used.isString() || !m.lastUsed.isValid())
            return fail(error, QStringLiteral("bad lastUsed"));
    }

    const QJsonValue bookmarks = json.value(QLatin1String("bookmarks"));
    if (!bookmarks.isUndefined()) {
        if (!bookmarks.isArray())
            return fail(error, QStringLiteral("bad bookmarks"));
        for (const QJsonValue& v : bookmarks.toArray()) {
            const auto b = v.isObject() ? BookmarkState::fromJson(v.toObject(), &why) : std::nullopt;
            if (!b)
                return fail(error, QStringLiteral("bookmarks: ") + why);
            m.bookmarks.append(*b);
        }
    }

    const QJsonValue view = json.value(QLatin1String("view"));
    if (!view.isUndefined()) {
        const auto v = view.isObject() ? ViewState::fromJson(view.toObject(), &why) : std::nullopt;
        if (!v)
            return fail(error, QStringLiteral("view: ") + why);
        m.view = *v;
    }
    return m;
}

void FileMemory::relocate(const QString& newPath)
{
    // Сравнение — как у ключа памяти (на Windows без учёта регистра).
    const QString oldKey = FileMemoryStore::normalizedKeyPath(source.source.path);
    const auto sameFile = [&oldKey](const QString& p) {
        return FileMemoryStore::normalizedKeyPath(p) == oldKey;
    };
    source.source.path = newPath;
    for (BookmarkState& b : bookmarks)
        if (sameFile(b.source.path))
            b.source.path = newPath;
    if (view && view->currentSource && sameFile(view->currentSource->path))
        view->currentSource->path = newPath;
}

TabState FileMemory::toTabState() const
{
    TabState state;
    state.sources.append(source);
    state.bookmarks = bookmarks;
    if (view)
        state.view = *view;
    return state;
}

// ---------------------------------------------------------------------------
// FileMemoryStore
// ---------------------------------------------------------------------------

FileMemoryStore::FileMemoryStore(const QString& directory, int limit)
    : m_dir(QDir::cleanPath(directory))
    , m_limit(qMax(1, limit))
{
    m_pool.setMaxThreadCount(1); // записи — по порядку
}

FileMemoryStore::~FileMemoryStore()
{
    waitForWrites();
}

QString FileMemoryStore::normalizedKeyPath(const QString& path)
{
    if (path.isEmpty())
        return path;
    QString normalized = QDir::cleanPath(QFileInfo(QDir::fromNativeSeparators(path)).absoluteFilePath());
#ifdef Q_OS_WIN
    normalized = normalized.toLower(); // файловая система без учёта регистра
#endif
    return normalized;
}

QString FileMemoryStore::fileNameForPath(const QString& path)
{
    const QByteArray digest = QCryptographicHash::hash(normalizedKeyPath(path).toUtf8(),
                                                       QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16)) + kJsonSuffix;
}

QString FileMemoryStore::filePath(const QString& fileName) const
{
    return m_dir + QLatin1Char('/') + fileName;
}

std::optional<FileMemory> FileMemoryStore::readMemory(const QString& fileName, bool* newer) const
{
    return readMemoryFile(filePath(fileName), newer);
}

void FileMemoryStore::enqueue(std::function<void()> task)
{
    // Сделанные записи из списка — чтобы он не рос за долгую сессию.
    m_writes.erase(std::remove_if(m_writes.begin(), m_writes.end(),
                                  [](const QFuture<void>& f) { return f.isFinished(); }),
                   m_writes.end());
    m_writes.append(QtConcurrent::run(&m_pool, std::move(task)));
}

void FileMemoryStore::waitForWrites()
{
    for (QFuture<void>& f : m_writes)
        f.waitForFinished();
    m_writes.clear();
}

void FileMemoryStore::ensureIndex()
{
    if (m_indexed)
        return;
    m_indexed = true;
    const QDir dir(m_dir);
    const QStringList names = dir.entryList({ QStringLiteral("*.json") }, QDir::Files);
    for (const QString& name : names) {
        const auto memory = readMemory(name);
        if (!memory)
            continue; // большая версия или битый — не наш кандидат
        m_index.insert(name, { memory->source.source, memory->lastUsed,
                               int(memory->bookmarks.size()) });
    }
}

FileMemoryStore::Lookup FileMemoryStore::open(const QString& requestedPath)
{
    Lookup result;
    if (requestedPath.isEmpty())
        return result;
    waitForWrites(); // только что закрытая вкладка того же файла — уже на диске
    const QString path = QDir::cleanPath(QFileInfo(requestedPath).absoluteFilePath());
    const QString name = fileNameForPath(path);
    const QString keyPath = normalizedKeyPath(path);

    // ---- 1–2: по пути ---------------------------------------------------------
    bool newer = false;
    std::optional<FileMemory> byPath = readMemory(name, &newer);
    if (newer) {
        result.found = Found::Refused;
        return result;
    }
    if (byPath && normalizedKeyPath(byPath->source.source.path) != keyPath)
        byPath.reset(); // чужой путь с тем же отпечатком имени — не наш
    if (byPath) {
        switch (byPath->source.source.checkAt(path)) {
        case SourceIdentity::State::Valid:
            byPath->relocate(path); // написание пути — как открыли сейчас
            result.found = Found::ByPath;
            result.memory = byPath;
            return result;
        case SourceIdentity::State::Changed: {
            // Память прежнего содержимого: с закладками — отсоединить.
            ensureIndex();
            m_index.remove(name);
            if (byPath->bookmarks.isEmpty()) {
                QFile::remove(filePath(name));
            } else {
                const QString stem = name.left(name.size() - kJsonSuffix.size());
                const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
                QString detached = stem + kDetachedMark + stamp + kJsonSuffix;
                for (int i = 1; QFileInfo::exists(filePath(detached)); ++i)
                    detached = stem + kDetachedMark + stamp + QLatin1Char('-') + QString::number(i) + kJsonSuffix;
                if (QFile::rename(filePath(name), filePath(detached)))
                    m_index.insert(detached, { byPath->source.source, byPath->lastUsed,
                                               int(byPath->bookmarks.size()) });
            }
            result.found = Found::Stale;
            result.memory = byPath;
            return result;
        }
        case SourceIdentity::State::Missing:   // не читается — не трогаем
        case SourceIdentity::State::Ephemeral:
            return result;
        }
    }

    // ---- 3: по содержимому ------------------------------------------------------
    const auto current = SourceIdentity::capture(path);
    if (!current || current->ephemeral)
        return result; // спул stdin, нечитаемый файл
    // Начало файла читается один раз: отпечаток начала кандидата сверяется в
    // памяти (min(4 КБ, его размер) байтов), checkAt — только у совпавших.
    QByteArray prefix;
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return result;
        prefix = file.read(SourceIdentity::kRegionBytes);
    }
    ensureIndex();
    QVector<std::pair<QDateTime, QString>> candidates; // свежие — первыми
    for (auto it = m_index.cbegin(); it != m_index.cend(); ++it) {
        const SourceIdentity& s = it->source;
        // Пустой префикс совпал бы с любым файлом; длиннее файла — не он.
        if (it.key() == name || s.size <= 0 || s.size > current->size)
            continue;
        const qint64 headLength = qMin(s.size, SourceIdentity::kRegionBytes);
        if (prefix.size() < headLength
            || RecordFingerprint::ofBytes(QByteArrayView(prefix).first(headLength)) != s.head)
            continue;
        candidates.append({ it->lastUsed, it.key() });
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [lastUsed, candidate] : std::as_const(candidates)) {
        Q_UNUSED(lastUsed);
        const auto entry = m_index.constFind(candidate);
        if (entry == m_index.cend() || entry->source.checkAt(path) != SourceIdentity::State::Valid)
            continue;
        std::optional<FileMemory> memory = readMemory(candidate); // с диска: перечень мог устареть
        if (!memory) {
            m_index.remove(candidate);
            continue;
        }
        if (memory->source.source.checkAt(path) != SourceIdentity::State::Valid)
            continue;
        const QString previousPath = memory->source.source.path;
        // Прежний файл на месте и цел — это копия: память остаётся и там.
        const bool detached = candidate.contains(kDetachedMark);
        const bool copy = !detached && memory->source.source.check() == SourceIdentity::State::Valid;
        memory->relocate(path);
        memory->lastUsed = QDateTime::currentDateTimeUtc();
        const QString from = filePath(candidate);
        const QString to = filePath(name);
        const QByteArray bytes = toBytes(*memory);
        enqueue([from, to, bytes, copy]() {
            if (FileMemoryStore::writeFileAtomically(to, bytes) && !copy)
                QFile::remove(from);
        });
        if (!copy)
            m_index.remove(candidate);
        m_index.insert(name, { memory->source.source, memory->lastUsed, int(memory->bookmarks.size()) });
        result.found = Found::ByContent;
        result.memory = memory;
        result.previousPath = previousPath;
        result.copied = copy;
        return result;
    }
    return result;
}

void FileMemoryStore::save(const FileMemory& memory, const QVector<RecordId>& base)
{
    const SourceIdentity& source = memory.source.source;
    if (source.ephemeral || !source.isValid())
        return;
    FileMemory copy = memory;
    copy.lastUsed = QDateTime::currentDateTimeUtc();
    const QString name = fileNameForPath(source.path);
    ensureIndex();
    Entry& entry = m_index[name];
    entry.source = source;
    entry.lastUsed = copy.lastUsed;
    entry.bookmarks = qMax(entry.bookmarks, int(copy.bookmarks.size()));
    if (copy.bookmarks.isEmpty() && base.isEmpty())
        entry.bookmarks = 0; // своих нет и не было — на диске могут быть только чужие
    const QString target = filePath(name);
    QDir().mkpath(m_dir);
    enqueue([target, copy, base]() { writeMerged(target, copy, base); });
    evictIfNeeded(name);
}

void FileMemoryStore::evictIfNeeded(const QString& keep)
{
    if (m_index.size() <= m_limit)
        return;
    QVector<QString> victims;
    for (auto it = m_index.cbegin(); it != m_index.cend(); ++it)
        if (it.key() != keep && it->bookmarks == 0)
            victims.append(it.key());
    std::sort(victims.begin(), victims.end(), [this](const QString& a, const QString& b) {
        return m_index.constFind(a)->lastUsed < m_index.constFind(b)->lastUsed;
    });
    const qsizetype excess = m_index.size() - m_limit;
    for (qsizetype i = 0; i < qMin(excess, victims.size()); ++i) {
        const QString path = filePath(victims[i]);
        m_index.remove(victims[i]);
        enqueue([path]() { removeIfEvictable(path); });
    }
}

int FileMemoryStore::clear()
{
    waitForWrites();
    m_index.clear();
    m_indexed = true;
    return clearDirectory(m_dir);
}

int FileMemoryStore::clearDirectory(const QString& directory)
{
    QDir dir(directory);
    int removed = 0;
    for (const QString& name : dir.entryList({ QStringLiteral("*.json") }, QDir::Files))
        removed += dir.remove(name) ? 1 : 0;
    return removed;
}

int FileMemoryStore::fileCount() const
{
    return int(QDir(m_dir).entryList({ QStringLiteral("*.json") }, QDir::Files).size());
}

bool FileMemoryStore::writeFileAtomically(const QString& filePath, const QByteArray& bytes,
                                          qint64 abortAfterBytes)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (abortAfterBytes >= 0) {
        file.write(bytes.left(abortAfterBytes));
        file.cancelWriting(); // как сбой посреди записи: commit не переименует
        file.commit();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}
