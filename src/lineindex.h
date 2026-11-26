#ifndef LINEINDEX_H
#define LINEINDEX_H

#include "logentry.h"
#include <QMutex>
#include <QVector>
#include <limits>
#include <memory>

// ============================================================================
// LineIndex — пофайловый блочный индекс строк для индексного бэкенда.
// Хранит на строку: байтовое смещение/длину (через кумулятивные relStart
// внутри блока), EOL (0/1/2 байта), упакованные метаданные (уровень, primary,
// plain-text) и id логической записи; на логическую запись — таймстамп (мс).
// Текст строк НЕ хранится — его читает с диска TextChunkCache по смещениям.
// ≈10 байт/строку + 8 байт/логическую запись. «Метки нет» у записи —
// kNoTimestampMs (logentry.h): отрицательный момент — метка до 1970 года.
//
// Потоковая модель:
//   • пишет ровно один поток (воркер LogIndexer): appendLine()/
//     setLogicalTimestamp(), затем publish() делает дозаписанное видимым;
//   • читатели (GUI-поток) берут значения через const-методы (короткий
//     мьютекс на вызов), воркеры сканов — через snapshot() (копия векторов
//     указателей на блоки; блоки за пределами своего count не читает);
//   • минимум и максимум меток файла (S-13) аппендер ведёт сам, O(1) на
//     запись: у каждого TsBlock — свои (их читает только аппендер, в
//     truncateFrom), у индекса — опубликованная пара (под мьютексом, вместе
//     со счётчиками; снапшот берёт её копией). Одно-файловая вкладка в
//     порядке файла берёт диапазон таймлайна и статистики отсюда;
//   • опубликованные строки неизменны. Единственное исключение —
//     «предварительный» хвост без \n: перед его переиндексацией
//     truncateFrom() СНАЧАЛА подменяет затрагиваемые блоки копиями (COW),
//     поэтому живые снапшоты продолжают видеть старую версию целиком.
// ============================================================================

class LineIndexSnapshot;

class LineIndex {
public:
    static constexpr int kBlockLines = 1024;      // строк в блоке индекса
    static constexpr int kTsBlockEntries = 4096;  // таймстампов в блоке

    // Биты meta[i]
    static constexpr quint8 kLevelMask       = 0x07; // LogLevel (0..6)
    static constexpr quint8 kPrimaryBit      = 0x08; // первичная строка записи
    static constexpr quint8 kOwnTimestampBit = 0x10; // ts распознан в САМОЙ строке
    static constexpr quint8 kPlainTextBit    = 0x20; // аналог LogEntry::isPlainText

    struct Block {
        qint64  baseOffset = 0;              // абс. смещение начала первой строки
        quint32 relStart[kBlockLines + 1];   // строка i: [relStart[i], relStart[i+1]) − eolSpan(eol)
        quint8  eolBytes[kBlockLines];       // код конца строки, см. kEolProvisionalCr
        quint8  meta[kBlockLines];
        quint32 logicalId[kBlockLines];
    };
    struct TsBlock {
        qint64 ms[kTsBlockEntries];          // kNoTimestampMs — метки нет
        // Минимум и максимум меток блока (S-13); без меток — maxMs ==
        // kNoTimestampMs. Пишет и читает только аппендер.
        qint64 minMs = (std::numeric_limits<qint64>::max)();
        qint64 maxMs = kNoTimestampMs;
    };

    // Данные одной строки для аппендера.
    // Код конца строки в eolBytes: число байтов перевода строки (0 — хвост
    // без \n, 1 — \n, 2 — \r\n) либо kEolProvisionalCr — хвост без \n,
    // кончающийся одиночным \r: этот \r не текст (как у резидентного пути
    // и как будет, когда допишут \n), но байт файла — смещения точны.
    static constexpr quint8 kEolProvisionalCr = 3;
    static constexpr int eolSpan(quint8 eol) { return eol == kEolProvisionalCr ? 1 : eol; }

    struct LineRecord {
        quint32  byteLength = 0;   // без EOL
        quint8   eolBytes = 1;     // 0 — хвост без \n, 1 — \n, 2 — \r\n, kEolProvisionalCr
        LogLevel level = LogLevel::Unknown; // унаследованный уровень записи
        bool     isPrimary = false;
        bool     hasOwnTimestamp = false;
        bool     isPlainText = false;
        quint32  logicalId = 0;
    };

    // ---- Читатели (GUI-поток; короткий мьютекс на вызов) --------------------
    qint64  lineCount() const;
    quint32 logicalEntryCount() const;
    qint64  endOffset() const;               // конец опубликованных байт
    bool    lastLineProvisional() const;     // последняя строка без EOL

    qint64  lineStartOffset(qint64 line) const;
    quint32 lineByteLength(qint64 line) const;   // без EOL
    quint8  lineEolBytes(qint64 line) const;     // байтов перевода строки
    LogLevel level(qint64 line) const;
    bool    isPrimary(qint64 line) const;
    bool    isPlainTextLine(qint64 line) const;
    quint32 logicalId(qint64 line) const;
    qint64  timestampMs(quint32 logicalId) const;   // kNoTimestampMs — метки нет
    // Настоящие минимум и максимум меток опубликованных записей (S-13; не
    // первая и последняя — записи файла бывают не по порядку). false — меток
    // нет. O(1).
    bool    timeRangeMs(qint64* minMs, qint64* maxMs) const;

    LineIndexSnapshot snapshot() const;

    // ---- Аппендер (только поток индексатора) ---------------------------------
    // Начать (про)дозапись с абсолютного смещения (0/после BOM/якорь хвоста).
    void beginAppend(qint64 startOffset);
    void appendLine(const LineRecord& rec);
    // Один раз на запись (id по порядку): минимум и максимум только растут.
    void setLogicalTimestamp(quint32 logicalId, qint64 tsMs);
    // Сделать всё дозаписанное видимым читателям.
    void publish();
    // Отбросить строки [line, конец) перед переиндексацией предварительного
    // хвоста; затрагиваемые блоки подменяются копиями (живые снапшоты
    // продолжают видеть старую версию). Вызывать до beginAppend().
    void truncateFrom(qint64 line);

private:
    friend class LineIndexSnapshot;

    // Незащищённые читатели для внутреннего пользования под уже взятым мьютексом.
    Block* mutableTailBlock();

    mutable QMutex m_mutex;
    QVector<std::shared_ptr<Block>> m_blocks;      // публикация — через m_lineCount
    QVector<std::shared_ptr<TsBlock>> m_tsBlocks;
    qint64  m_lineCount = 0;      // опубликовано строк
    quint32 m_logicalCount = 0;   // опубликовано логических записей
    qint64  m_endOffset = 0;      // конец опубликованных байт
    qint64  m_minMs = (std::numeric_limits<qint64>::max)(); // опубликованные мин./макс. меток
    qint64  m_maxMs = kNoTimestampMs;

    // Состояние аппендера (только поток индексатора; читатели не трогают).
    qint64  m_pendingLines = 0;   // всего строк с учётом неопубликованных
    quint32 m_pendingLogical = 0;
    qint64  m_nextOffset = 0;     // смещение начала следующей строки
    qint64  m_pendingMinMs = (std::numeric_limits<qint64>::max)();
    qint64  m_pendingMaxMs = kNoTimestampMs;
};

// Снимок индекса для воркеров: копия векторов указателей на блоки + счётчики.
// Не блокирует читаемое; строки за пределами lineCount() не читать.
class LineIndexSnapshot {
public:
    LineIndexSnapshot() = default;

    qint64  lineCount() const { return m_lineCount; }
    quint32 logicalEntryCount() const { return m_logicalCount; }

    qint64  lineStartOffset(qint64 line) const;
    quint32 lineByteLength(qint64 line) const;
    quint8  lineEolBytes(qint64 line) const;
    LogLevel level(qint64 line) const;
    bool    isPrimary(qint64 line) const;
    bool    isPlainTextLine(qint64 line) const;
    quint32 logicalId(qint64 line) const;
    qint64  timestampMs(quint32 logicalId) const;
    // Минимум и максимум меток записей снимка (LineIndex::timeRangeMs).
    bool    timeRangeMs(qint64* minMs, qint64* maxMs) const;

private:
    friend class LineIndex;
    QVector<std::shared_ptr<LineIndex::Block>> m_blocks;
    QVector<std::shared_ptr<LineIndex::TsBlock>> m_tsBlocks;
    qint64  m_lineCount = 0;
    quint32 m_logicalCount = 0;
    qint64  m_minMs = (std::numeric_limits<qint64>::max)();
    qint64  m_maxMs = kNoTimestampMs;
};

#endif // LINEINDEX_H
