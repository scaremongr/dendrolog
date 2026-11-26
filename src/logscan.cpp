#include "logscan.h"
#include "textquery.h"

LogEntryMeta LogScanSnapshot::metaFor(const LogEntry& entry)
{
    LogEntryMeta m;
    m.timestampMs = entry.timestamp().isValid()
        ? entry.timestamp().toMSecsSinceEpoch() : kNoTimestampMs;
    m.level = entry.level();
    m.logicalEntryId = entry.logicalEntryId();
    m.sourceFile = entry.sourceFile().get();
    m.isPlainText = entry.isPlainText();
    return m;
}

bool LogScanSnapshot::timeRange(qint64* minMs, qint64* maxMs, qint64* timedRows,
                                const std::atomic_bool* cancel) const
{
    if (!m_impl) {
        if (timedRows)
            *timedRows = 0;
        return false;
    }
    return m_impl->timeRange(minMs, maxMs, timedRows, cancel);
}

bool LogScanSnapshot::Impl::timeRange(qint64* minMs, qint64* maxMs, qint64* timedRows,
                                      const std::atomic_bool* cancel) const
{
    qint64 end = rowCount();
    while (end > 0 && !hasTimestampMs(metaAt(end - 1).timestampMs)) {
        --end;
        if (cancel && (end & 0x3FFF) == 0 && cancel->load(std::memory_order_relaxed)) {
            end = 0;
            break;
        }
    }
    if (timedRows)
        *timedRows = end;
    if (end <= 0)
        return false;
    // Первая строка с меткой — в порядке времени это строка 0 (проход на
    // случай преамбулы — страховка, а не порядок файла: тот переопределён).
    qint64 begin = 0;
    while (!hasTimestampMs(metaAt(begin).timestampMs)) {
        ++begin;   // строка end - 1 с меткой — выход гарантирован
        if (cancel && (begin & 0x3FFF) == 0 && cancel->load(std::memory_order_relaxed))
            return false;
    }
    if (minMs)
        *minMs = metaAt(begin).timestampMs;
    if (maxMs)
        *maxMs = metaAt(end - 1).timestampMs;
    return true;
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

    LogLevel levelAt(qint64 row) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return LogLevel::Unknown;
        return m_entries.at(row)->level();
    }

    qint64 lineAt(qint64 row, const LogFile** file) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row)) {
            *file = nullptr;
            return 0;
        }
        const LogEntry& e = *m_entries.at(row);
        *file = e.sourceFile().get();
        return e.originalLineNumber();
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

    void forEachLineBackward(qint64 fromRow,
                             const std::function<bool(qint64, const LogEntryMeta&,
                                                      QStringView)>& visit) const override
    {
        for (qint64 i = qMin<qint64>(fromRow, m_entries.size() - 1); i >= 0; --i) {
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

    std::unique_ptr<LogScanSnapshot::LineCursor> lineCursor() const override
    {
        // Текст уже в памяти — курсор только смотрит в записи.
        class Cursor final : public LogScanSnapshot::LineCursor {
        public:
            explicit Cursor(const QVector<std::shared_ptr<LogEntry>>& entries)
                : m_entries(entries) {}
            QStringView lineAt(qint64 row) override
            {
                if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
                    return {};
                return QStringView(m_entries.at(row)->message());
            }
        private:
            const QVector<std::shared_ptr<LogEntry>>& m_entries;
        };
        return std::make_unique<Cursor>(m_entries);
    }

    LogFilePtr sourceFilePtrAt(qint64 row) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return LogFilePtr();
        return m_entries.at(row)->sourceFile();
    }

    LogEntryFields fieldsAt(qint64 row, const QString&) const override
    {
        if (row < 0 || row >= m_entries.size() || !m_entries.at(row))
            return LogEntryFields();
        return m_entries.at(row)->fields();
    }

private:
    QVector<std::shared_ptr<LogEntry>> m_entries;
};

} // namespace

LogScanSnapshot LogScanSnapshot::fromEntries(QVector<std::shared_ptr<LogEntry>> entries)
{
    return LogScanSnapshot(std::make_shared<EntriesSnapshotImpl>(std::move(entries)));
}

qint64 LogScanSnapshot::findRowWithLevel(LogLevelMask levels, qint64 startRow, bool forward,
                                         bool wrapAround, const std::atomic_bool* cancel,
                                         std::atomic<int>* progress) const
{
    if (!m_impl)
        return -1;
    const Impl& impl = *m_impl;
    const qint64 n = impl.rowCount();
    qint64 scanned = 0;
    bool cancelled = false;
    const qint64 found = scanRowsFrom(n, startRow, forward, wrapAround, [&](qint64 row) {
        // Строка стоит наносекунды — флаг отмены и прогресс раз в 64k строк.
        if ((++scanned & 0xFFFF) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                return RowScanStep::Stop;
            }
            if (progress)
                progress->store(int(qMin<qint64>(99, scanned * 100 / n)),
                                std::memory_order_relaxed);
        }
        if (!(levelBit(impl.levelAt(row)) & levels))
            return RowScanStep::Continue;
        // Уровень подошёл (редкий случай) — продолжение ли это записи, чья
        // строка показана прямо перед этой? Строки записи идут подряд в
        // любом порядке вкладки.
        if (row > 0) {
            const LogEntryMeta current = impl.metaAt(row);
            const LogEntryMeta previous = impl.metaAt(row - 1);
            if (previous.logicalEntryId == current.logicalEntryId
                && previous.sourceFile == current.sourceFile)
                return RowScanStep::Continue;
        }
        return RowScanStep::Found;
    });
    if (progress)
        progress->store(100, std::memory_order_relaxed);
    return cancelled ? -1 : found;
}

qint64 LogScanSnapshot::findRowOfFile(const LogFile* file, qint64 startRow, bool forward,
                                      bool wrapAround, const std::atomic_bool* cancel,
                                      std::atomic<int>* progress) const
{
    if (!m_impl || !file)
        return -1;
    const Impl& impl = *m_impl;
    const qint64 n = impl.rowCount();
    qint64 scanned = 0;
    bool cancelled = false;
    const qint64 found = scanRowsFrom(n, startRow, forward, wrapAround, [&](qint64 row) {
        if ((++scanned & 0xFFFF) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                return RowScanStep::Stop;
            }
            if (progress)
                progress->store(int(qMin<qint64>(99, scanned * 100 / n)),
                                std::memory_order_relaxed);
        }
        const LogFile* rowFile = nullptr;
        impl.lineAt(row, &rowFile);
        return rowFile == file ? RowScanStep::Found : RowScanStep::Continue;
    });
    if (progress)
        progress->store(100, std::memory_order_relaxed);
    return cancelled ? -1 : found;
}

qint64 LogScanSnapshot::findRowWithText(const TextQuery& query, qint64 startRow,
                                        bool forward, bool wrapAround,
                                        const std::atomic_bool* cancel,
                                        std::atomic<int>* progress) const
{
    const qint64 n = rowCount();
    const TextMatcher matcher(query);
    if (!m_impl || n == 0 || !matcher.isUsable())
        return -1;
    const std::unique_ptr<LineCursor> cursor = m_impl->lineCursor();
    qint64 scanned = 0;
    bool cancelled = false;
    // Прогресс — доля просмотренных строк от всех: второй проход при
    // заворачивании продолжает первый.
    const qint64 found = scanRowsFrom(n, startRow, forward, wrapAround, [&](qint64 row) {
        if ((++scanned & 0xFFF) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                cancelled = true;
                return RowScanStep::Stop;
            }
            if (progress)
                progress->store(int(qMin<qint64>(99, scanned * 100 / n)),
                                std::memory_order_relaxed);
        }
        return matcher.matches(cursor->lineAt(row)) ? RowScanStep::Found
                                                    : RowScanStep::Continue;
    });
    if (progress)
        progress->store(100, std::memory_order_relaxed);
    return cancelled ? -1 : found;
}
