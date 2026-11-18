#include "bookmarkstore.h"
#include "logmodel.h"
#include "recordlocator.h"

#include <QJsonValue>

#include <algorithm>

BookmarkStore::BookmarkStore(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
    // Найденные и потерянные строки приходят пачками (порция загрузки) —
    // changed панели одним сигналом на пачку.
    m_changedTimer.setSingleShot(true);
    m_changedTimer.setInterval(0);
    connect(&m_changedTimer, &QTimer::timeout, this, &BookmarkStore::changed);
    RecordLocator& loc = locator();
    connect(&loc, &RecordLocator::resolved, this, &BookmarkStore::onResolved);
    connect(&loc, &RecordLocator::refused, this,
            [this](int token, RecordLocator::Refusal) { onRefused(token); });
    connect(&loc, &RecordLocator::completed, this, &BookmarkStore::onCompleted);
}

RecordLocator& BookmarkStore::locator() const
{
    return m_model.recordLocator();
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
    for (const Item& item : m_items)
        list.append(item.bookmark);
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
    return it == m_items.constEnd() ? nullptr : &it->bookmark;
}

int BookmarkStore::idAtRow(int row) const
{
    if (m_items.isEmpty())
        return -1;
    const RecordKey key = m_model.keyAt(row);
    if (!key.isValid())
        return -1;
    const auto it = m_byLine.constFind(LineKey(key.file.get(), key.line));
    if (it == m_byLine.constEnd())
        return -1;
    const Item& item = m_items[it.value()];
    // Строка с тем же номером, но из другой записи или с другой меткой — уже
    // не та строка (перезагрузка): отметки нет, пока её не перепроверят.
    if (!item.verified || item.lost || item.bookmark.key != key)
        return -1;
    return it.value();
}

QString BookmarkStore::previewOf(int row) const
{
    return m_model.messageAt(row).left(kPreviewLength);
}

void BookmarkStore::notifyRow(int row)
{
    m_model.notifyBookmarkRow(row);
}

void BookmarkStore::releaseToken(Item& item)
{
    if (item.token == 0)
        return;
    locator().cancel(item.token);
    m_byToken.remove(item.token);
    item.token = 0;
}

void BookmarkStore::bindToRow(int id, Item& item, int row)
{
    releaseToken(item);
    const RecordKey key = m_model.keyAt(row);
    Bookmark& b = item.bookmark;
    if (b.key.isValid() && m_byLine.value(lineKey(b), -1) == id)
        m_byLine.remove(lineKey(b));
    b.key = key;
    b.filePath = key.file->filePath;
    b.line = key.line;
    b.preview = previewOf(row);
    b.record = locator().idFor(key, m_model.messageAt(row));
    item.verified = true;
    item.lost = false;
    if (const auto source = locator().sourceFor(key.file))
        item.source = *source;
    m_byLine.insert(lineKey(b), id);
    // Последняя строка без '\n': отпечаток — когда её допишут.
    if (!b.record.hasText) {
        item.token = locator().watchCompletion(key, b.record, m_model.messageAt(row));
        if (item.token)
            m_byToken.insert(item.token, id);
    }
}

int BookmarkStore::add(int row, const QString& note)
{
    const RecordKey key = m_model.keyAt(row);
    if (!key.isValid())
        return -1;
    const LineKey lk(key.file.get(), key.line);
    const auto existing = m_byLine.constFind(lk);
    if (existing != m_byLine.constEnd()) {
        const int id = existing.value();
        Item& item = m_items[id];
        if (item.verified && !item.lost && item.bookmark.key == key)
            return id;
        // Закладка этой строки потеряла её (перезагрузка, переписанный файл):
        // пользователь поставил её заново — привязываем, заметку сохраняем.
        bindToRow(id, item, row);
        if (!note.isEmpty())
            item.bookmark.note = note;
        notifyRow(row);
        emit changed();
        return id;
    }

    const int id = m_nextId++;
    Item& item = m_items[id];
    item.bookmark.id = id;
    item.bookmark.note = note;
    item.bookmark.created = QDateTime::currentDateTime();
    bindToRow(id, item, row);
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
    releaseToken(it.value());
    if (m_byLine.value(lineKey(it->bookmark), -1) == id)
        m_byLine.remove(lineKey(it->bookmark));
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
    for (Item& item : m_items)
        releaseToken(item);
    m_items.clear();
    m_byLine.clear();
    m_byToken.clear();
    for (const int row : rows)
        notifyRow(row);
    emit changed();
}

bool BookmarkStore::setNote(int id, const QString& note)
{
    const auto it = m_items.find(id);
    if (it == m_items.end() || it->bookmark.note == note)
        return false;
    it->bookmark.note = note;
    emit changed();
    return true;
}

BookmarkStore::Status BookmarkStore::status(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || it->lost)
        return Status::Missing;
    const RecordKey& key = it->bookmark.key;
    if (m_model.rowForKey(key) >= 0)
        return it->verified ? Status::Visible : Status::Pending;
    if (m_model.nearestRowForKey(key) >= 0)
        return Status::Hidden;
    // nearestRowForKey не отличает «строки нет» от «не видно ни одной
    // строки»: при пустом видимом наборе проверенная закладка — скрытая.
    if (m_model.rowCount() == 0 && it->verified && m_model.totalLineCount() > 0)
        return Status::Hidden;
    return Status::Pending;
}

int BookmarkStore::visibleRow(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || !it->verified || it->lost)
        return -1;
    return m_model.rowForKey(it->bookmark.key);
}

int BookmarkStore::targetRow(int id) const
{
    // Непроверенная строка после перезагрузки может оказаться чужой: к ней
    // не ведём, пока сверка не прошла (Pending).
    switch (status(id)) {
    case Status::Visible:
    case Status::Hidden:
        return m_model.nearestRowForKey(m_items[id].bookmark.key);
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
    for (const Item& item : m_items) {
        if (!item.verified || item.lost)
            continue;
        const int row = m_model.rowForKey(item.bookmark.key);
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
// Сброс строк и ответы RecordLocator
// ---------------------------------------------------------------------------

void BookmarkStore::reacquire(int id, Item& item)
{
    releaseToken(item);
    item.verified = false;
    item.lost = false; // файл могли вернуть к прежнему содержимому
    item.token = locator().reacquire(item.bookmark.key.file, item.bookmark.record);
    m_byToken.insert(item.token, id);
}

void BookmarkStore::requestSaved(int id, Item& item)
{
    releaseToken(item);
    item.verified = false;
    item.lost = false;
    item.token = locator().request(item.source, item.bookmark.record);
    m_byToken.insert(item.token, id);
}

void BookmarkStore::invalidateFile(const QString& filePath)
{
    const QString key = RecordLocator::pathKey(filePath);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        Item& item = it.value();
        if (item.bookmark.key.file) {
            if (item.bookmark.key.file->filePath == filePath)
                reacquire(it.key(), item);
        } else if (item.lost && RecordLocator::pathKey(item.bookmark.filePath) == key) {
            // Сохранённая закладка получила отказ (файл был другим) — файл
            // читается заново, проверяем ещё раз. Ждущая ждёт сама.
            requestSaved(it.key(), item);
        }
    }
}

void BookmarkStore::invalidateAll()
{
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        if (it->bookmark.key.file)
            reacquire(it.key(), it.value());
        else if (it->lost)
            requestSaved(it.key(), it.value());
    }
}

void BookmarkStore::onResolved(int token, const RecordKey& key)
{
    const auto tit = m_byToken.constFind(token);
    if (tit == m_byToken.constEnd())
        return;
    const int id = tit.value();
    Item& item = m_items[id];
    Bookmark& b = item.bookmark;
    // Недописанная строка: локатор следит за ней тем же токеном до completed.
    if (b.record.hasText) {
        m_byToken.remove(token);
        item.token = 0;
    }
    const LineKey lk(key.file.get(), key.line);
    const int other = m_byLine.value(lk, -1);
    if (other >= 0 && other != id) {
        // Сохранённая закладка нашла строку, на которой уже стоит другая:
        // одна закладка на строку, заметка не теряется.
        Item& kept = m_items[other];
        if (kept.bookmark.note.isEmpty())
            kept.bookmark.note = b.note;
        releaseToken(item);
        m_items.remove(id);
        scheduleChanged();
        return;
    }
    if (b.key.isValid() && m_byLine.value(lineKey(b), -1) == id)
        m_byLine.remove(lineKey(b));
    b.key = key;
    b.filePath = key.file->filePath;
    b.line = key.line;
    item.verified = true;
    item.lost = false;
    m_byLine.insert(lk, id);
    notifyRow(m_model.rowForKey(key));
    scheduleChanged();
}

void BookmarkStore::onRefused(int token)
{
    const auto tit = m_byToken.constFind(token);
    if (tit == m_byToken.constEnd())
        return;
    Item& item = m_items[tit.value()];
    m_byToken.erase(tit);
    item.token = 0;
    item.verified = true;
    item.lost = true;
    scheduleChanged();
}

void BookmarkStore::onCompleted(int token, const RecordId& record, const RecordKey& key)
{
    const auto tit = m_byToken.constFind(token);
    if (tit == m_byToken.constEnd())
        return;
    const int id = tit.value();
    Item& item = m_items[id];
    m_byToken.erase(tit);
    item.token = 0;
    Bookmark& b = item.bookmark;
    b.record = record;
    // Дописанная строка могла начать запись: ключ другой, строка та же.
    if (key.isValid() && key != b.key && key.file == b.key.file && key.line == b.line) {
        b.key = key;
        notifyRow(m_model.rowForKey(key));
        scheduleChanged();
    }
}

bool BookmarkStore::updateVisiblePreviews(const QString& filePath)
{
    bool anyChange = false;
    for (Item& item : m_items) {
        if (item.bookmark.filePath != filePath || !item.verified || item.lost)
            continue;
        const int row = m_model.rowForKey(item.bookmark.key);
        if (row < 0)
            continue;
        const QString preview = previewOf(row);
        if (preview != item.bookmark.preview) {
            item.bookmark.preview = preview;
            anyChange = true;
        }
    }
    return anyChange;
}

void BookmarkStore::refreshPreviews(const QString& filePath)
{
    if (updateVisiblePreviews(filePath))
        emit changed();
}

void BookmarkStore::fileLoaded(const QString& filePath, bool fullLoad)
{
    // Дочитан хвост: строка закладки могла быть предварительной (без '\n') и
    // дописаться — ключ тот же, текст длиннее; фрагмент показывает новый.
    if (!fullLoad && updateVisiblePreviews(filePath))
        emit changed();
}

// ---------------------------------------------------------------------------
// Сохранение (R-4)
// ---------------------------------------------------------------------------

std::optional<BookmarkState> BookmarkStore::state(int id) const
{
    const auto it = m_items.constFind(id);
    if (it == m_items.constEnd() || !it->bookmark.record.isValid())
        return std::nullopt;
    BookmarkState s;
    s.source = it->source;
    // Строка найдена — идентичность по концу прочитанного сейчас.
    if (it->verified && !it->lost && it->bookmark.key.file)
        if (const auto source = locator().sourceFor(it->bookmark.key.file))
            s.source = *source;
    if (!s.source.isValid())
        return std::nullopt;
    s.record = it->bookmark.record;
    s.note = it->bookmark.note;
    s.preview = it->bookmark.preview;
    s.created = it->bookmark.created;
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
    const QString pathKey = RecordLocator::pathKey(state.source.path);
    for (auto it = m_items.begin(); it != m_items.end(); ++it) {
        Bookmark& b = it->bookmark;
        if (b.line == state.record.line && RecordLocator::pathKey(b.filePath) == pathKey) {
            if (b.note.isEmpty() && !state.note.isEmpty()) {
                b.note = state.note;
                emit changed();
            }
            return it.key();
        }
    }
    const int id = m_nextId++;
    Item& item = m_items[id];
    item.bookmark.id = id;
    item.bookmark.filePath = state.source.path;
    item.bookmark.line = state.record.line;
    item.bookmark.record = state.record;
    item.bookmark.preview = state.preview.left(kPreviewLength);
    item.bookmark.note = state.note;
    item.bookmark.created = state.created;
    item.source = state.source;
    requestSaved(id, item);
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
