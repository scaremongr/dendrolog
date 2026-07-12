#ifndef LINEINDEX_H
#define LINEINDEX_H

#include "logentry.h"
#include <QMutex>
#include <QVector>
#include <memory>

// ============================================================================
// LineIndex — пофайловый блочный индекс строк для индексного бэкенда.
// Хранит на строку: байтовое смещение/длину (через кумулятивные relStart
// внутри блока), EOL (0/1/2 байта), упакованные метаданные (уровень, primary,
// plain-text) и id логической записи; на логическую запись — таймстамп (мс).
// Текст строк НЕ хранится — его читает с диска TextChunkCache по смещениям.
// ≈10 байт/строку + 8 байт/логическую запись.
//
// Потоковая модель:
//   • пишет ровно один поток (воркер LogIndexer): appendLine()/
//     setLogicalTimestamp(), затем publish() делает дозаписанное видимым;
//   • читатели (GUI-поток) берут значения через const-методы (короткий
//     мьютекс на вызов), воркеры сканов — через snapshot() (копия векторов
//     указателей на блоки; блоки за пределами своего count не читает);
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
        quint32 relStart[kBlockLines + 1];   // строка i: [relStart[i], relStart[i+1]) − eol
        quint8  eolBytes[kBlockLines];
        quint8  meta[kBlockLines];
        quint32 logicalId[kBlockLines];
    };
    struct TsBlock {
        qint64 ms[kTsBlockEntries];          // -1 — метка невалидна
    };

    // Данные одной строки для аппендера.
    struct LineRecord {
        quint32  byteLength = 0;   // без EOL
        quint8   eolBytes = 1;     // 0 — хвост без \n, 1 — \n, 2 — \r\n
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
    quint8  lineEolBytes(qint64 line) const;
    LogLevel level(qint64 line) const;
    bool    isPrimary(qint64 line) const;
    bool    isPlainTextLine(qint64 line) const;
    quint32 logicalId(qint64 line) const;
    qint64  timestampMs(quint32 logicalId) const;

    LineIndexSnapshot snapshot() const;

    // ---- Аппендер (только поток индексатора) ---------------------------------
    // Начать (про)дозапись с абсолютного смещения (0/после BOM/якорь хвоста).
    void beginAppend(qint64 startOffset);
    void appendLine(const LineRecord& rec);
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

    // Состояние аппендера (только поток индексатора; читатели не трогают).
    qint64  m_pendingLines = 0;   // всего строк с учётом неопубликованных
    quint32 m_pendingLogical = 0;
    qint64  m_nextOffset = 0;     // смещение начала следующей строки
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

private:
    friend class LineIndex;
    QVector<std::shared_ptr<LineIndex::Block>> m_blocks;
    QVector<std::shared_ptr<LineIndex::TsBlock>> m_tsBlocks;
    qint64  m_lineCount = 0;
    quint32 m_logicalCount = 0;
};

#endif // LINEINDEX_H
