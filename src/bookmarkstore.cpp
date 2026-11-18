#include "bookmarkstore.h"
#include "logmodel.h"

#include <algorithm>

BookmarkStore::BookmarkStore(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
    // Сверка непроверенных закладок — после того как модель показала новые
    // строки. Отложенно и одним проходом на пачку событий: dataChanged из
    // обработчика rowsInserted и лишние проходы на каждый батч не нужны.
    m_verifyTimer.setSingleShot(true);
    m_verifyTimer.setInterval(0);
    connect(&m_verifyTimer, &QTimer::timeout, this, [this]() {
        if (verifyPending())
            emit changed();
    });
    connect(&m_model, &QAbstractItemModel::rowsInserted, this, &BookmarkStore::scheduleVerify);
    connect(&m_model, &QAbstractItemModel::modelReset, this, &BookmarkStore::scheduleVerify);
    connect(&m_model, &QAbstractItemModel::layoutChanged, this, &BookmarkStore::scheduleVerify);
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

bool BookmarkStore::inData(const RecordKey& key) const
{
    return m_model.rowForKey(key) >= 0 || m_model.nearestRowForKey(key) >= 0;
}

void BookmarkStore::notifyRow(int row)
{
    m_model.notifyBookmarkRow(row);
}

int BookmarkStore::add(int row, const QString& note)
{
    const RecordKey key = m_model.keyAt(row);
    if (!key.isValid())
        return -1;
    const LineKey lk(key.file.get(), key.line);
    const auto existing = m_byLine.constFind(lk);
    if (existing != m_byLine.constEnd()) {
        Item& item = m_items[existing.value()];
        if (item.verified && !item.lost && item.bookmark.key == key)
            return existing.value();
        // Закладка этой строки потеряла её (перезагрузка, переписанный файл):
        // пользователь поставил её заново — привязываем, заметку сохраняем.
        if (!item.verified)
            --m_unverified;
        item.verified = true;
        item.lost = false;
        item.bookmark.key = key;
        item.bookmark.preview = previewOf(row);
        if (!note.isEmpty())
            item.bookmark.note = note;
        notifyRow(row);
        emit changed();
        return existing.value();
    }

    Item item;
    item.bookmark.id = m_nextId++;
    item.bookmark.key = key;
    item.bookmark.filePath = key.file->filePath;
    item.bookmark.line = key.line;
    item.bookmark.preview = previewOf(row);
    item.bookmark.note = note;
    item.bookmark.created = QDateTime::currentDateTime();
    const int id = item.bookmark.id;
    m_items.insert(id, item);
    m_byLine.insert(lk, id);
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
    if (!it->verified)
        --m_unverified;
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
    m_items.clear();
    m_byLine.clear();
    m_unverified = 0;
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

void BookmarkStore::invalidateFile(const QString& filePath)
{
    for (Item& item : m_items) {
        if (item.bookmark.filePath != filePath || !item.verified || item.lost)
            continue;
        item.verified = false;
        ++m_unverified;
    }
}

void BookmarkStore::invalidateAll()
{
    for (Item& item : m_items) {
        if (!item.verified || item.lost)
            continue;
        item.verified = false;
        ++m_unverified;
    }
}

void BookmarkStore::scheduleVerify()
{
    if (m_unverified > 0 && !m_verifyTimer.isActive())
        m_verifyTimer.start();
}

bool BookmarkStore::verifyPending()
{
    if (m_unverified <= 0)
        return false;
    bool anyChange = false;
    for (Item& item : m_items) {
        if (item.verified || item.lost)
            continue;
        const int row = m_model.rowForKey(item.bookmark.key);
        if (row < 0)
            continue; // строки ещё нет или она скрыта — сверим, когда покажется
        --m_unverified;
        item.verified = true;
        anyChange = true;
        if (previewOf(row) == item.bookmark.preview)
            notifyRow(row);
        else
            item.lost = true; // ключ совпал с чужой строкой переписанного файла
    }
    return anyChange;
}

void BookmarkStore::fileLoaded(const QString& filePath, bool fullLoad)
{
    bool anyChange = verifyPending();
    // Дочитан хвост: строка закладки могла быть предварительной (без '\n') и
    // дописаться — ключ тот же, текст длиннее. Фрагмент обновляется, иначе
    // следующая полная перезагрузка сочла бы строку чужой.
    if (!fullLoad) {
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
    }
    // Без видимых строк nearestRowForKey не скажет, есть ли строка в данных.
    if (m_model.rowCount() > 0) {
        for (Item& item : m_items) {
            Bookmark& b = item.bookmark;
            if (b.filePath != filePath)
                continue;
            if (item.lost ? !fullLoad : inData(b.key))
                continue;
            // Ключа нет в данных (или строка потеряна, а файл прочитан
            // заново): ищем строку по номеру и сверяем текст.
            const int row = m_model.nearestVisibleRowForLine(b.line, b.key.file.get());
            const RecordKey key = m_model.keyAt(row);
            const bool same = key.isValid() && key.file == b.key.file && key.line == b.line
                && previewOf(row) == b.preview;
            if (!item.verified)
                --m_unverified;
            item.verified = true;
            if (same) {
                b.key = key;
                item.lost = false;
                notifyRow(row);
            } else {
                item.lost = true;
            }
            anyChange = true;
        }
    }
    if (anyChange)
        emit changed();
}
