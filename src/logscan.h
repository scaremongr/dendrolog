#ifndef LOGSCAN_H
#define LOGSCAN_H

#include "logentry.h"
#include <QString>
#include <QStringView>
#include <QVector>
#include <functional>
#include <memory>

// ============================================================================
// LogEntryMeta / LogScanSnapshot — шов для последовательных сканов данных вне
// модели (статистика, таймлайн, фильтры). Потребители не знают, КАК хранятся
// записи: резидентный снапшот — COW-копия вектора shared_ptr; индексный —
// вид в блочный индекс строк с чтением текста с диска. Контракт:
//   • снапшот дёшев в копировании и безопасен для переноса в воркер
//     (держит данные живыми независимо от модели);
//   • QStringView текста валиден ТОЛЬКО внутри колбэка forEachLine;
//   • LogEntryMeta::sourceFile жив, пока жив снапшот; долгоживущую ссылку
//     на файл (для UI-структур) берите через sourceFilePtrAt().
// ============================================================================

struct LogEntryMeta {
    qint64   timestampMs = -1;            // мс epoch; -1 — метка невалидна
    LogLevel level = LogLevel::Unknown;
    int      logicalEntryId = 0;
    const LogFile* sourceFile = nullptr;
    bool     isPlainText = false;
};

class LogScanSnapshot {
public:
    // Реализация снапшота конкретного хранилища. forEachLine может читать
    // диск — реализация обязана быть самодостаточной (владеть всеми ручками).
    class Impl {
    public:
        virtual ~Impl() = default;
        virtual qint64 rowCount() const = 0;
        virtual LogEntryMeta metaAt(qint64 row) const = 0;
        virtual void forEachMeta(
            qint64 fromRow,
            const std::function<bool(qint64, const LogEntryMeta&)>& visit) const = 0;
        virtual void forEachLine(
            qint64 fromRow,
            const std::function<bool(qint64, const LogEntryMeta&, QStringView)>&
                visit) const = 0;
        virtual QString textAt(qint64 row) const = 0;
        virtual LogFilePtr sourceFilePtrAt(qint64 row) const = 0;
    };

    LogScanSnapshot() = default;
    explicit LogScanSnapshot(std::shared_ptr<const Impl> impl)
        : m_impl(std::move(impl)) {}

    // Резидентный снапшот поверх COW-копии вектора записей.
    static LogScanSnapshot fromEntries(QVector<std::shared_ptr<LogEntry>> entries);

    qint64 rowCount() const { return m_impl ? m_impl->rowCount() : 0; }
    bool isEmpty() const { return rowCount() == 0; }

    // Метаданные строки; O(1). Для отсутствующей строки — дефолтная мета.
    LogEntryMeta metaAt(qint64 row) const
    {
        return m_impl ? m_impl->metaAt(row) : LogEntryMeta{};
    }

    // Последовательный обход от fromRow до конца; visit вернул false — стоп.
    void forEachMeta(qint64 fromRow,
                     const std::function<bool(qint64 row, const LogEntryMeta&)>& visit) const
    {
        if (m_impl)
            m_impl->forEachMeta(fromRow, visit);
    }
    void forEachLine(qint64 fromRow,
                     const std::function<bool(qint64 row, const LogEntryMeta&,
                                              QStringView text)>& visit) const
    {
        if (m_impl)
            m_impl->forEachLine(fromRow, visit);
    }

    // Материализовать текст одной строки (редкие точечные обращения).
    QString textAt(qint64 row) const
    {
        return m_impl ? m_impl->textAt(row) : QString();
    }
    // Долгоживущая ссылка на файл-источник строки.
    LogFilePtr sourceFilePtrAt(qint64 row) const
    {
        return m_impl ? m_impl->sourceFilePtrAt(row) : LogFilePtr();
    }

    static LogEntryMeta metaFor(const LogEntry& entry);

private:
    std::shared_ptr<const Impl> m_impl;
};

#endif // LOGSCAN_H
