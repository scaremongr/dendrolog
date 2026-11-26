#include "bookmarkstore.h"
#include "logmodel.h"
#include "recordlocator.h"

#include <QJsonValue>

#include <algorithm>

BookmarkStore::BookmarkStore(LogModel& model)
    : LocatedPlaces(model)
{
    // Найденные и потерянные строки приходят пачками (порция загрузки) —
    // changed панели одним сигналом на пачку.
    m_changedTimer.setSingleShot(true);
    m_changedTimer.setInterval(0);
    connect(&m_changedTimer, &QTimer::timeout, this, &BookmarkStore::changed);
}

LocatedPlace* BookmarkStore::placeById(int id)
{
    const auto it = m_items.find(id);
    return it == m_items.end() ? nullptr : &it.value();
}

void BookmarkStore::forEachPlace(const std::function<void(int, LocatedPlace&)>& fn)
{
    for (auto it = m_items.begin(); it != m_items.end(); ++it)
        fn(it.key(), it.value());
}

void BookmarkStore::scheduleChanged()
{
    if (!m_changedTimer.isActive())
        m_changedTimer.start();
}

QVector<Bookmark> BookmarkStore::bookmarks() const
{
    QVector<Bookmark> list;
    list.reserve(m_items.size());
    for (const Bookmark& b : m_items)
        list.append(b);
    std::sort(list.begin(), list.end(), [](const Bookmark& a, const Bookmark& b) {
        if (a.filePath != b.filePath)
            return a.filePath < b.filePath;
        if (a.line != b.line)
            return a.line < b.line;
        return a.id < b.id;
    });
    return list;
}

const Bookmark* BookmarkStore::find(int id) const
{
    const auto it = m_items.constFind(id);
    return it == m_items.constEnd() ? nullptr : &it.value();
}

int BookmarkStore::idAtRow(int row) const
{
    if (m_items.isEmpty())
        return -1;
    const RecordKey key = model().keyAt(row);
    if (!key.isValid())
        return -1;
    const auto it = m_byLine.constFind(LineKey(key.file.get(), key.line));
    if (it == m_byLine.constEnd())
        return -1;
    const auto bit = m_items.constFind(it.value());
    if (bit == m_items.constEnd())
        return -1;
    const Bookmark& b = bit.value();
    // Строка с тем же номером, но из другой записи или с другой меткой — уже
    // не та строка (перезагрузка): отметки нет, пока её не перепроверят.
    if (!b.isLive() || b.key != key)
        return -1;
    return it.value();
}

void BookmarkStore::notifyRow(int row)
{
    model().notifyBookmarkRow(row);
}

void BookmarkStore::bindBookmark(int id, Bookmark& b, int row)
{
    if (b.key.isValid() && m_byLine.value(lineKey(b), -1) == id)
        m_byLine.remove(lineKey(b));
    bindToRow(id, b, row);
    if (const auto source = locator().sourceFor(b.key.file))
        b.source = *source;
    m_byLine.insert(lineKey(b), id);
}

int BookmarkStore::add(int row, const QString& note)
{
    const RecordKey key = model().keyAt(row);
    if (!key.isValid())
        return -1;
    const LineKey lk(key.file.get(), key.line);
    const auto existing = m_byLine.constFind(lk);
    if (existing != m_byLine.constEnd()) {
        const int id = existing.value();
        Bookmark& b = m_items[id];
        if (b.isLive() && b.key == key)
            return id;
        // Закладка этой строки потеряла её (перезагрузка, переписанный файл):
        // пользователь поставил её заново — привязываем, заметку сохраняем.
        bindBookmark(id, b, row);
        if (!note.isEmpty())
            b.note = note;
        notifyRow(row);
        emit changed();
        return id;
    }

    const int id = m_nextId++;
    Bookmark& b = m_items[id];
    b.id = id;
    b.note = note;
    b.created = QDateTime::currentDateTime();
    bindBookmark(id, b, row);
    notifyRow(row);
    emit changed();
    return id;
}

bool BookmarkStore::remove(int id)
{
    const auto it = m_items.find(id);
    if (it == m_items.end())
        return false;
    const int row = visibleRow(id);
    release(it.value());
    if (m_byLine.value(lineKey(it.value()), -1) == id)
        m_byLine.remove(lineKey(it.value()));
    m_items.erase(it);
    notifyRow(row);
    emit changed();
    return true;
}

bool BookmarkStore::toggle(int row)
{
    const int id = idAtRow(row);
    if (id >= 0) {
        remove(id);
        return false;
    }
    return add(row) >= 0;
}

void BookmarkStore::clear()
{
    if (m_items.isEmpty())
        return;
    const QVector<int> rows = visibleRows();
    for (Bookmark& b : m_items)
        release(b);
    m_items.clear();
    m_byLine.clear();
    for (const int row : rows)
        notifyRow(row);
    emit changed();
}

bool BookmarkStore::setNote(int id, const QString& note)
{
    const auto it = m_items.find(id);
    if (it == m_items.end() || it->note == note)
        return false;
    it->note = note;
    emit changed();
    return true;
}

BookmarkStore::Status BookmarkStore::status(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || it->lost)
        return Status::Missing;
    const RecordKey& key = it->key;
    if (model().rowForKey(key) >= 0)
        return it->verified ? Status::Visible : Status::Pending;
    if (model().nearestRowForKey(key) >= 0)
        return Status::Hidden;
    // nearestRowForKey не отличает «строки нет» от «не видно ни одной
    // строки»: при пустом видимом наборе проверенная закладка — скрытая.
    if (model().rowCount() == 0 && it->verified && model().totalLineCount() > 0)
        return Status::Hidden;
    return Status::Pending;
}

int BookmarkStore::visibleRow(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || !it->isLive())
        return -1;
    return model().rowForKey(it->key);
}

int BookmarkStore::targetRow(int id) const
{
    // Непроверенная строка после перезагрузки может оказаться чужой: к ней
    // не ведём, пока сверка не прошла (Pending).
    switch (status(id)) {
    case Status::Visible:
    case Status::Hidden:
        return model().nearestRowForKey(m_items.constFind(id)->key);
    case Status::Pending:
    case Status::Missing:
        break;
    }
    return -1;
}

QVector<int> BookmarkStore::visibleRows() const
{
    QVector<int> rows;
    rows.reserve(m_items.size());
    for (const Bookmark& b : m_items) {
        if (!b.isLive())
            continue;
        const int row = model().rowForKey(b.key);
        if (row >= 0)
            rows.append(row);
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

int BookmarkStore::nextRow(int startRow, bool forward, bool wrap) const
{
    const QVector<int> rows = visibleRows();
    if (rows.isEmpty())
        return -1;
    if (startRow < 0)
        return forward ? rows.constFirst() : rows.constLast();
    if (forward) {
        const auto it = std::upper_bound(rows.constBegin(), rows.constEnd(), startRow);
        if (it != rows.constEnd())
            return *it;
        return wrap ? rows.constFirst() : -1;
    }
    const auto it = std::lower_bound(rows.constBegin(), rows.constEnd(), startRow);
    if (it != rows.constBegin())
        return *(it - 1);
    return wrap ? rows.constLast() : -1;
}

// ---------------------------------------------------------------------------
// Сброс строк и ответы RecordLocator (общая обвязка — LocatedPlaces)
// ---------------------------------------------------------------------------

void BookmarkStore::invalidateFile(const QString& filePath)
{
    invalidatePlacesOfFile(filePath);
}

void BookmarkStore::recheckSource(const QString& sourceKey)
{
    recheckSavedSource(sourceKey);
}

void BookmarkStore::invalidateAll()
{
    invalidateAllPlaces();
}

void BookmarkStore::placeResolved(int id, const RecordKey& previousKey)
{
    Bookmark& b = m_items[id];
    const LineKey previous(previousKey.file.get(), previousKey.line);
    const bool hadLine = previousKey.isValid() && m_byLine.value(previous, -1) == id;
    const LineKey lk = lineKey(b);
    const int other = m_byLine.value(lk, -1);
    if (hadLine && previous != lk)
        m_byLine.remove(previous);
    if (other >= 0 && other != id) {
        // Сохранённая закладка нашла строку, на которой уже стоит другая:
        // одна закладка на строку, заметка не теряется.
        Bookmark& kept = m_items[other];
        if (kept.note.isEmpty())
            kept.note = b.note;
        release(b);
        m_items.remove(id);
        if (hadLine)
            notifyRow(model().rowForKey(previousKey));
        scheduleChanged();
        return;
    }
    m_byLine.insert(lk, id);
    notifyRow(model().rowForKey(b.key));
    scheduleChanged();
}

void BookmarkStore::placeRefused(int)
{
    scheduleChanged();
}

void BookmarkStore::placeCompleted(int id, bool keyChanged)
{
    if (!keyChanged)
        return;
    notifyRow(model().rowForKey(m_items[id].key));
    scheduleChanged();
}

void BookmarkStore::refreshPreviews(const QString& filePath)
{
    if (refreshVisiblePreviews(filePath))
        emit changed();
}

void BookmarkStore::fileLoaded(const QString& filePath, bool fullLoad)
{
    // Дочитан хвост: строка закладки могла быть предварительной (без '\n') и
    // дописаться — ключ тот же, текст длиннее; фрагмент показывает новый.
    if (!fullLoad && refreshVisiblePreviews(filePath))
        emit changed();
}

// ---------------------------------------------------------------------------
// Сохранение (R-4)
// ---------------------------------------------------------------------------

std::optional<BookmarkState> BookmarkStore::state(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || !it->record.isValid())
        return std::nullopt;
    BookmarkState s;
    s.source = it->source;
    // Строка найдена — идентичность по концу прочитанного сейчас.
    if (it->isLive() && it->key.file)
        if (const auto source = locator().sourceFor(it->key.file))
            s.source = *source;
    if (!s.source.isValid())
        return std::nullopt;
    s.record = it->record;
    s.note = it->note;
    s.preview = it->preview;
    s.created = it->created;
    return s;
}

QVector<BookmarkState> BookmarkStore::states() const
{
    QVector<BookmarkState> list;
    for (const Bookmark& b : bookmarks())
        if (const auto s = state(b.id))
            list.append(*s);
    return list;
}

int BookmarkStore::restore(const BookmarkState& state)
{
    if (!state.record.isValid() || !state.source.isValid())
        return -1;
    // Сравнение — по ключу источника: у сжатого файла (F-2) найденная
    // закладка стоит на распакованной копии, а сохранённая — на архиве.
    const QString pathKey = RecordLocator::sourceKey(state.source);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        Bookmark& b = it.value();
        const QString key = b.key.file ? RecordLocator::sourceKey(*b.key.file)
            : b.source.isValid() ? RecordLocator::sourceKey(b.source)
                                   : RecordLocator::pathKey(b.filePath);
        if (b.line == state.record.line && key == pathKey) {
            if (b.note.isEmpty() && !state.note.isEmpty()) {
                b.note = state.note;
                emit changed();
            }
            return it.key();
        }
    }
    const int id = m_nextId++;
    Bookmark& b = m_items[id];
    b.id = id;
    b.filePath = state.source.path;
    b.line = state.record.line;
    b.record = state.record;
    b.preview = state.preview.left(kPreviewLength);
    b.note = state.note;
    b.created = state.created;
    b.source = state.source;
    requestSaved(id, b);
    emit changed();
    return id;
}

// ---------------------------------------------------------------------------
// BookmarkState
// ---------------------------------------------------------------------------

QJsonObject BookmarkState::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("version"), kFormatVersion);
    json.insert(QStringLiteral("source"), source.toJson());
    json.insert(QStringLiteral("record"), record.toJson());
    if (!note.isEmpty())
        json.insert(QStringLiteral("note"), note);
    json.insert(QStringLiteral("preview"), preview);
    if (created.isValid())
        json.insert(QStringLiteral("created"), created.toUTC().toString(Qt::ISODateWithMs));
    return json;
}

std::optional<BookmarkState> BookmarkState::fromJson(const QJsonObject& json, QString* error)
{
    const auto fail = [error](const QString& text) -> std::optional<BookmarkState> {
        if (error)
            *error = text;
        return std::nullopt;
    };
    if (json.value(QLatin1String("version")).toInt(-1) != kFormatVersion)
        return fail(QStringLiteral("unsupported bookmark version"));
    BookmarkState s;
    QString why;
    const auto source = SourceIdentity::fromJson(json.value(QLatin1String("source")).toObject(), &why);
    if (!source)
        return fail(QStringLiteral("bad source: ") + why);
    const auto record = RecordId::fromJson(json.value(QLatin1String("record")).toObject(), &why);
    if (!record)
        return fail(QStringLiteral("bad record: ") + why);
    const QJsonValue note = json.value(QLatin1String("note"));
    const QJsonValue preview = json.value(QLatin1String("preview"));
    const QJsonValue created = json.value(QLatin1String("created"));
    if ((!note.isUndefined() && !note.isString()) || (!preview.isUndefined() && !preview.isString())
        || (!created.isUndefined() && !created.isString()))
        return fail(QStringLiteral("bad note, preview or created"));
    s.source = *source;
    s.record = *record;
    s.note = note.toString();
    s.preview = preview.toString().left(BookmarkStore::kPreviewLength);
    s.created = QDateTime::fromString(created.toString(), Qt::ISODateWithMs);
    if (!created.isUndefined() && !s.created.isValid())
        return fail(QStringLiteral("bad created time"));
    return s;
}
