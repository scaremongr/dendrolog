#ifndef LOGSCAN_H
#define LOGSCAN_H

#include "logentry.h"
#include <QString>
#include <QStringView>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

struct TextQuery;

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
    qint64   timestampMs = kNoTimestampMs; // мс epoch; kNoTimestampMs — метки нет
    LogLevel level = LogLevel::Unknown;
    int      logicalEntryId = 0;
    const LogFile* sourceFile = nullptr;
    bool     isPlainText = false;
};

// Обход строк «ближайшая подходящая от startRow» — один на переход по уровню
// (findRowWithLevel) и на поиск текста (findRowWithText: быстрый поиск в фоне
// и синхронный LogStore::findOccurrence):
//   • вперёд — со startRow + 1 до конца; назад — со startRow - 1 до начала
//     (startRow вне [0, n) — с последней строки);
//   • wrapAround — затем с другого края до startRow ВКЛЮЧИТЕЛЬНО: строка, с
//     которой начали, находится последней.
// visit(row) решает, что делать дальше; итог — найденная строка или -1.
enum class RowScanStep { Continue, Found, Stop };

template <typename Visit>
qint64 scanRowsFrom(qint64 n, qint64 startRow, bool forward, bool wrapAround, Visit&& visit)
{
    if (n <= 0)
        return -1;
    qint64 found = -1;
    // false — обход закончен (нашли или остановлены).
    const auto up = [&](qint64 from, qint64 end) {
        for (qint64 i = from; i < end; ++i) {
            const RowScanStep step = visit(i);
            if (step == RowScanStep::Found)
                found = i;
            if (step != RowScanStep::Continue)
                return false;
        }
        return true;
    };
    const auto down = [&](qint64 from, qint64 last) {
        for (qint64 i = from; i >= last; --i) {
            const RowScanStep step = visit(i);
            if (step == RowScanStep::Found)
                found = i;
            if (step != RowScanStep::Continue)
                return false;
        }
        return true;
    };
    if (forward) {
        const qint64 first = qBound<qint64>(0, startRow + 1, n);
        if (up(first, n) && wrapAround)
            up(0, first);
    } else {
        const bool inside = startRow >= 0 && startRow < n;
        if (down(inside ? startRow - 1 : n - 1, 0) && wrapAround && inside)
            down(n - 1, startRow);
    }
    return found;
}

class LogScanSnapshot {
public:
    // Реализация снапшота конкретного хранилища. forEachLine может читать
    // диск — реализация обязана быть самодостаточной (владеть всеми ручками).
    // Чтение текста строк по одной в порядке обхода (вперёд или назад) —
    // для сканов через scanRowsFrom. Держит свои ручки (индексный бэкенд —
    // SequentialLineReader), принадлежит одному потоку. QStringView валиден
    // до следующего вызова lineAt.
    class LineCursor {
    public:
        virtual ~LineCursor() = default;
        virtual QStringView lineAt(qint64 row) = 0;
    };

    class Impl {
    public:
        virtual ~Impl() = default;
        virtual qint64 rowCount() const = 0;
        virtual LogEntryMeta metaAt(qint64 row) const = 0;
        // Только уровень строки: дешевле metaAt (переход по уровню идёт по
        // сотням миллионов строк). Текст не читается.
        virtual LogLevel levelAt(qint64 row) const { return metaAt(row).level; }
        // Файл и номер физической строки в нём (с 1; 0 — строки нет): по ним
        // сопоставляются строки двух снапшотов (выдача поиска ↔ вкладка,
        // R-6). Текст и метка не читаются.
        virtual qint64 lineAt(qint64 row, const LogFile** file) const = 0;
        virtual void forEachMeta(
            qint64 fromRow,
            const std::function<bool(qint64, const LogEntryMeta&)>& visit) const = 0;
        virtual void forEachLine(
            qint64 fromRow,
            const std::function<bool(qint64, const LogEntryMeta&, QStringView)>&
                visit) const = 0;
        // От fromRow (или последней строки, если fromRow за концом) к 0.
        virtual void forEachLineBackward(
            qint64 fromRow,
            const std::function<bool(qint64, const LogEntryMeta&, QStringView)>&
                visit) const = 0;
        virtual QString textAt(qint64 row) const = 0;
        virtual std::unique_ptr<LineCursor> lineCursor() const = 0;
        virtual LogFilePtr sourceFilePtrAt(qint64 row) const = 0;
        // Поля строки row, чей текст — text (как отдал forEachLine): те же,
        // что покажет модель. Резидентный снапшот читает поля записи (их
        // меняет только GUI-поток при остановленных фоновых читателях —
        // инвариант 4), индексный извлекает схемой, как materializeEntry.
        virtual LogEntryFields fieldsAt(qint64 row, const QString& text) const = 0;
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

    LogLevel levelAt(qint64 row) const
    {
        return m_impl ? m_impl->levelAt(row) : LogLevel::Unknown;
    }

    // Файл и номер строки в файле (с 1); 0 и nullptr — строки нет. O(1).
    qint64 lineAt(qint64 row, const LogFile** file) const
    {
        if (m_impl)
            return m_impl->lineAt(row, file);
        *file = nullptr;
        return 0;
    }

    // Ближайшая от startRow строка, с которой начинается запись уровня из
    // levels; -1 — нет или отменено. Строки-продолжения многострочной записи
    // наследуют её уровень, поэтому целью служит только ПЕРВАЯ показанная
    // строка записи: иначе «следующая ошибка» шагала бы по каждой строке
    // стек-трейса. Порядок обхода — scanRowsFrom. Читает только метаданные;
    // годится и для GUI-потока (небольшой документ), и для воркера.
    qint64 findRowWithLevel(LogLevelMask levels, qint64 startRow, bool forward,
                            bool wrapAround, const std::atomic_bool* cancel = nullptr,
                            std::atomic<int>* progress = nullptr) const;

    // Ближайшая от startRow строка, подходящая под запрос (TextMatcher:
    // подстрока, слово целиком, регекс); -1 — нет, запрос пуст или невалиден,
    // либо отменено. Порядок обхода — scanRowsFrom, как у findRowWithLevel.
    // Текст читается (индексный бэкенд — с диска): на большом документе —
    // из воркера (QuickSearch).
    qint64 findRowWithText(const TextQuery& query, qint64 startRow, bool forward,
                           bool wrapAround, const std::atomic_bool* cancel = nullptr,
                           std::atomic<int>* progress = nullptr) const;

    // Диапазон меток в порядке вкладки (время; строки без метки — в конце,
    // как у таймлайна и статистики): метка первой и последней строки С
    // меткой (в порядке файла одно-файловой индексной вкладки — приближение); *timedRows — число строк до хвоста без меток. false — меток
    // нет или отменено. «Метки нет» — только kNoTimestampMs: отрицательный
    // момент (до 1970 года) — метка. Хвост без меток проходится с конца.
    bool timeRange(qint64* minMs, qint64* maxMs, qint64* timedRows = nullptr,
                   const std::atomic_bool* cancel = nullptr) const;

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
    // Обход назад: от fromRow (не дальше последней строки) до 0.
    void forEachLineBackward(qint64 fromRow,
                             const std::function<bool(qint64 row, const LogEntryMeta&,
                                                      QStringView text)>& visit) const
    {
        if (m_impl)
            m_impl->forEachLineBackward(fromRow, visit);
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
    // Поля строки для показа (см. Impl::fieldsAt).
    LogEntryFields fieldsAt(qint64 row, const QString& text) const
    {
        return m_impl ? m_impl->fieldsAt(row, text) : LogEntryFields();
    }

    static LogEntryMeta metaFor(const LogEntry& entry);

private:
    std::shared_ptr<const Impl> m_impl;
};

#endif // LOGSCAN_H
