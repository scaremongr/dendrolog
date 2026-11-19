#include "navhistory.h"
#include "logmodel.h"
#include "recordlocator.h"

NavHistory::NavHistory(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
    RecordLocator& loc = locator();
    connect(&loc, &RecordLocator::resolved, this, [this](int token, const RecordKey& key) {
        NavEntry* e = entryByToken(token);
        if (!e)
            return;
        if (e->record.hasText)
            e->token = 0; // недописанную строку локатор ведёт дальше
        e->key = key;
        e->verified = true;
        e->lost = false;
        emit changed();
    });
    connect(&loc, &RecordLocator::refused, this, [this](int token, RecordLocator::Refusal) {
        NavEntry* e = entryByToken(token);
        if (!e)
            return;
        e->token = 0;
        e->verified = true;
        e->lost = true;
        emit changed();
    });
    connect(&loc, &RecordLocator::completed, this,
            [this](int token, const RecordId& record, const RecordKey& key) {
        if (NavEntry* e = entryByToken(token)) {
            e->token = 0;
            e->record = record;
            // Дописанная строка могла начать запись: ключ другой.
            if (key.isValid() && key.file == e->key.file && key.line == e->line)
                e->key = key;
        }
    });
}

RecordLocator& NavHistory::locator() const
{
    return m_model.recordLocator();
}

NavEntry* NavHistory::entryByToken(int token)
{
    for (NavEntry& e : m_entries)
        if (e.token == token)
            return &e;
    return nullptr;
}

void NavHistory::release(NavEntry& e)
{
    if (e.token) {
        locator().cancel(e.token);
        e.token = 0;
    }
}

void NavHistory::releaseTokens(qsizetype from, qsizetype end)
{
    for (qsizetype i = from; i < end && i < m_entries.size(); ++i)
        release(m_entries[i]);
}

void NavHistory::reacquire(NavEntry& e)
{
    release(e);
    e.verified = false;
    e.lost = false; // файл могли вернуть к прежнему содержимому
    if (e.key.isValid())
        e.token = locator().reacquire(e.key.file, e.record);
}

NavEntry NavHistory::makeEntry(int row, NavKind kind, bool origin) const
{
    NavEntry e;
    e.key = m_model.keyAt(row);
    if (!e.key.isValid())
        return e;
    e.filePath = e.key.file->filePath;
    e.line = e.key.line;
    const QString text = m_model.messageAt(row);
    e.preview = text.left(kPreviewLength);
    e.record = locator().idFor(e.key, text);
    e.kind = kind;
    e.origin = origin;
    return e;
}

void NavHistory::recordJump(int fromRow, int toRow, NavKind kind)
{
    if (kind == NavKind::History)
        return;
    const int rows = m_model.rowCount();
    if (toRow < 0 || toRow >= rows)
        return;
    NavEntry to = makeEntry(toRow, kind, false);
    if (!to.key.isValid())
        return;
    NavEntry from;
    if (fromRow >= 0 && fromRow < rows)
        from = makeEntry(fromRow, kind, true);
    // Переход на ту же строку, где стоят, — не переход.
    if (from.key.isValid() && from.key == to.key)
        return;

    // Как в браузере: новый переход отбрасывает «вперёд».
    if (m_pos >= 0 && m_pos < m_entries.size() - 1) {
        releaseTokens(m_pos + 1, m_entries.size());
        m_entries.resize(m_pos + 1);
    }

    const bool atCurrent = m_pos >= 0 && from.key.isValid()
        && m_entries[m_pos].verified && !m_entries[m_pos].lost
        && m_entries[m_pos].key == from.key;
    // Склеивается только серия «шагов» (F3, F8, F2, выдача Search, новые
    // строки), продолжающая ПОСЛЕДНИЙ переход: место, куда вернулись по
    // истории, и цели разовых переходов (Go to, таймлайн, статистика)
    // не затираются.
    if (atCurrent && m_lastWasJump && isStepKind(kind) && !m_entries[m_pos].origin
        && m_entries[m_pos].kind == kind) {
        // Серия переходов одного вида без движения между ними: заменяем
        // прошлую цель, Alt+← вернёт к началу серии.
        release(m_entries[m_pos]);
        m_entries[m_pos] = to;
        if (m_pos > 0 && m_entries[m_pos - 1].key == to.key)
            m_entries.removeLast(); // серия вернулась туда, откуда началась
    } else {
        if (from.key.isValid() && !atCurrent)
            m_entries.append(from);
        m_entries.append(to);
    }
    if (m_entries.size() > kMaxEntries) {
        releaseTokens(0, m_entries.size() - kMaxEntries);
        m_entries.remove(0, m_entries.size() - kMaxEntries);
    }
    // Недописанная последняя строка: отпечаток — когда её допишут.
    for (qsizetype i = qMax<qsizetype>(0, m_entries.size() - 2); i < m_entries.size(); ++i) {
        NavEntry& e = m_entries[i];
        if (e.token == 0 && e.verified && !e.record.hasText && e.record.isValid()) {
            const int row = m_model.rowForKey(e.key);
            if (row >= 0)
                e.token = locator().watchCompletion(e.key, e.record, m_model.messageAt(row));
        }
    }
    m_pos = int(m_entries.size()) - 1;
    m_lastWasJump = true;
    emit changed();
}

bool NavHistory::isStepKind(NavKind kind)
{
    switch (kind) {
    case NavKind::QuickSearch:
    case NavKind::LevelJump:
    case NavKind::Bookmark:
    case NavKind::Search:
    case NavKind::NewLines:
        return true;
    case NavKind::History:
    case NavKind::Other:
    case NavKind::GoTo:
    case NavKind::Timeline:
    case NavKind::Statistics:
    case NavKind::FullLog:
        break;
    }
    return false;
}

bool NavHistory::canGoBack() const
{
    for (int i = m_pos - 1; i >= 0; --i)
        if (!m_entries[i].lost)
            return true;
    return false;
}

bool NavHistory::canGoForward() const
{
    for (int i = m_pos + 1; m_pos >= 0 && i < m_entries.size(); ++i)
        if (!m_entries[i].lost)
            return true;
    return false;
}

int NavHistory::resolve(NavEntry& e)
{
    if (e.lost || !e.key.isValid())
        return -1;
    // Строки файла сброшены, локатор ещё не нашёл строку: место пока
    // недоступно (файл грузится, удалён) — пропускаем.
    if (!e.verified)
        return -1;
    // Строка видна — фрагмент освежаем: она могла быть предварительной
    // (хвост без '\n') и дописаться.
    const int row = m_model.rowForKey(e.key);
    if (row >= 0) {
        e.preview = m_model.messageAt(row).left(kPreviewLength);
        return row;
    }
    return m_model.nearestRowForKey(e.key);
}

int NavHistory::step(int direction, int currentRow)
{
    if (m_pos < 0)
        return -1;
    for (int i = m_pos + direction; i >= 0 && i < m_entries.size(); i += direction) {
        const int row = resolve(m_entries[i]);
        if (row < 0 || row == currentRow)
            continue;
        m_pos = i;
        m_lastWasJump = false;
        emit changed();
        return row;
    }
    return -1;
}

int NavHistory::back(int currentRow)
{
    return step(-1, currentRow);
}

int NavHistory::forward(int currentRow)
{
    return step(+1, currentRow);
}

void NavHistory::clear()
{
    if (m_entries.isEmpty())
        return;
    releaseTokens(0, m_entries.size());
    m_entries.clear();
    m_pos = -1;
    m_lastWasJump = false;
    emit changed();
}

// Сброс строк снимает и «пропала»: файл могли вернуть к прежнему
// содержимому — пропавшее место ищется заново (как у закладок).
void NavHistory::invalidateFile(const QString& filePath)
{
    for (NavEntry& e : m_entries)
        if (e.filePath == filePath)
            reacquire(e);
}

void NavHistory::refreshPreviews(const QString& filePath)
{
    for (NavEntry& e : m_entries) {
        if (e.filePath != filePath || !e.verified || e.lost)
            continue;
        const int row = m_model.rowForKey(e.key);
        if (row >= 0)
            e.preview = m_model.messageAt(row).left(kPreviewLength);
    }
}

void NavHistory::invalidateAll()
{
    for (NavEntry& e : m_entries)
        reacquire(e);
}
