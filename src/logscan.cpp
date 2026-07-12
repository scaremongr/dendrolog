#include "logscan.h"

LogEntryMeta LogScanSnapshot::metaFor(const LogEntry& entry)
{
    LogEntryMeta m;
    m.timestampMs = entry.timestamp().isValid()
        ? entry.timestamp().toMSecsSinceEpoch() : -1;
    m.level = entry.level();
    m.logicalEntryId = entry.logicalEntryId();
    m.sourceFile = entry.sourceFile().get();
    m.isPlainText = entry.isPlainText();
    return m;
}

namespace {

// Резидентная реализация: COW-копия вектора shared_ptr держит записи живыми.
class EntriesSnapshotImpl final : public LogScanSnapshot::Impl {
public:
    explicit EntriesSnapshotImpl(QVector<std::shared_ptr<LogEntry>> entries)
        : m_entries(std::move(entries))
    {
    }

    qint64 rowCount() const override { return m_entries.size(); }

    LogEntryMeta metaAt(qint64 row) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return LogEntryMeta{};
        return LogScanSnapshot::metaFor(*m_entries.at(row));
    }

    void forEachMeta(qint64 fromRow,
                     const std::function<bool(qint64, const LogEntryMeta&)>& visit)
        const override
    {
        for (qint64 i = qMax<qint64>(0, fromRow); i < m_entries.size(); ++i) {
            const LogEntry* e = m_entries.at(i).get();
            if (!e)
                continue;
            if (!visit(i, LogScanSnapshot::metaFor(*e)))
                return;
        }
    }

    void forEachLine(qint64 fromRow,
                     const std::function<bool(qint64, const LogEntryMeta&,
                                              QStringView)>& visit) const override
    {
        for (qint64 i = qMax<qint64>(0, fromRow); i < m_entries.size(); ++i) {
            const LogEntry* e = m_entries.at(i).get();
            if (!e)
                continue;
            if (!visit(i, LogScanSnapshot::metaFor(*e), QStringView(e->message())))
                return;
        }
    }

    QString textAt(qint64 row) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return QString();
        return m_entries.at(row)->message();
    }

    LogFilePtr sourceFilePtrAt(qint64 row) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return LogFilePtr();
        return m_entries.at(row)->sourceFile();
    }

private:
    QVector<std::shared_ptr<LogEntry>> m_entries;
};

} // namespace

LogScanSnapshot LogScanSnapshot::fromEntries(QVector<std::shared_ptr<LogEntry>> entries)
{
    return LogScanSnapshot(std::make_shared<EntriesSnapshotImpl>(std::move(entries)));
}
