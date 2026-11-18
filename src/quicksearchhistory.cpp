#include "quicksearchhistory.h"

QuickSearchHistory::QuickSearchHistory(const QStringList& entries)
{
    for (const QString& entry : entries) {
        if (m_entries.size() >= kMaxEntries)
            break;
        if (acceptable(entry) && !m_entries.contains(entry))
            m_entries.append(entry);
    }
}

bool QuickSearchHistory::acceptable(const QString& text)
{
    return !text.isEmpty() && text.size() <= kMaxEntryLength;
}

bool QuickSearchHistory::add(const QString& text)
{
    if (!acceptable(text))
        return false;
    if (!m_entries.isEmpty() && m_entries.constFirst() == text)
        return false;
    m_entries.removeAll(text);
    m_entries.prepend(text);
    while (m_entries.size() > kMaxEntries)
        m_entries.removeLast();
    return true;
}
