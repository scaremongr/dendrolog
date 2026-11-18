#include "navhistory.h"
#include "logmodel.h"

NavHistory::NavHistory(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
}

NavEntry NavHistory::makeEntry(int row, NavKind kind, bool origin) const
{
    NavEntry e;
    e.key = m_model.keyAt(row);
    if (!e.key.isValid())
        return e;
    e.filePath = e.key.file->filePath;
    e.line = e.key.line;
    e.preview = m_model.messageAt(row).left(kPreviewLength);
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
    if (m_pos >= 0 && m_pos < m_entries.size() - 1)
        m_entries.resize(m_pos + 1);

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
        m_entries[m_pos] = to;
        if (m_pos > 0 && m_entries[m_pos - 1].key == to.key)
            m_entries.removeLast(); // серия вернулась туда, откуда началась
    } else {
        if (from.key.isValid() && !atCurrent)
            m_entries.append(from);
        m_entries.append(to);
    }
    if (m_entries.size() > kMaxEntries)
        m_entries.remove(0, m_entries.size() - kMaxEntries);
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

bool NavHistory::textMatches(int row, const QString& preview) const
{
    return m_model.messageAt(row).left(kPreviewLength) == preview;
}

int NavHistory::resolve(NavEntry& e)
{
    if (e.lost || !e.key.isValid())
        return -1;
    if (e.verified) {
        // Строка видна — фрагмент освежаем: она могла быть предварительной
        // (хвост без '\n') и дописаться; иначе после следующей полной
        // перезагрузки сверка сочла бы её чужой.
        const int row = m_model.rowForKey(e.key);
        if (row >= 0) {
            e.preview = m_model.messageAt(row).left(kPreviewLength);
            return row;
        }
        return m_model.nearestRowForKey(e.key);
    }

    // Строки файла сбрасывались: ключ может указывать на чужую строку
    // переписанного файла — сверяем текст.
    int row = m_model.rowForKey(e.key);
    if (row < 0) {
        // Ключа нет среди видимых: другие границы записей после перезагрузки
        // (или строка скрыта) — ищем по номеру строки.
        const int byLine = m_model.nearestVisibleRowForLine(e.line, e.key.file.get());
        const RecordKey k = m_model.keyAt(byLine);
        if (k.isValid() && k.file == e.key.file && k.line == e.line)
            row = byLine;
    }
    if (row >= 0) {
        if (!textMatches(row, e.preview)) {
            e.lost = true;
            return -1;
        }
        e.key = m_model.keyAt(row);
        e.preview = m_model.messageAt(row).left(kPreviewLength);
        e.verified = true;
        return row;
    }
    // Строка скрыта фильтром: текст не проверить, ведёт ключ, если он есть
    // в данных. Иначе файл ещё грузится, убран или строки больше нет.
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
    m_entries.clear();
    m_pos = -1;
    m_lastWasJump = false;
    emit changed();
}

// Сброс строк снимает и «пропала»: файл могли вернуть к прежнему
// содержимому — пропавшее место сверится заново (как у закладок).
void NavHistory::invalidateFile(const QString& filePath)
{
    for (NavEntry& e : m_entries) {
        if (e.filePath == filePath) {
            e.verified = false;
            e.lost = false;
        }
    }
}

void NavHistory::invalidateAll()
{
    for (NavEntry& e : m_entries) {
        e.verified = false;
        e.lost = false;
    }
}
