#include "lineindex.h"

// ---------------------------------------------------------------------------
// Общие помощники доступа к блокам. Блоки неизменны после публикации (кроме
// COW-подмены в truncateFrom), поэтому чтение по указателю безопасно, пока
// указатель удерживается (снапшотом или под мьютексом).
// ---------------------------------------------------------------------------
namespace {

inline const LineIndex::Block* blockFor(
    const QVector<std::shared_ptr<LineIndex::Block>>& blocks, qint64 line)
{
    return blocks.at(int(line / LineIndex::kBlockLines)).get();
}

inline int slotFor(qint64 line)
{
    return int(line % LineIndex::kBlockLines);
}

inline qint64 startOffsetOf(
    const QVector<std::shared_ptr<LineIndex::Block>>& blocks, qint64 line)
{
    const LineIndex::Block* b = blockFor(blocks, line);
    return b->baseOffset + b->relStart[slotFor(line)];
}

inline quint32 byteLengthOf(
    const QVector<std::shared_ptr<LineIndex::Block>>& blocks, qint64 line)
{
    const LineIndex::Block* b = blockFor(blocks, line);
    const int s = slotFor(line);
    return b->relStart[s + 1] - b->relStart[s] - LineIndex::eolSpan(b->eolBytes[s]);
}

inline qint64 tsOf(const QVector<std::shared_ptr<LineIndex::TsBlock>>& tsBlocks,
                   quint32 logicalId, quint32 logicalCount)
{
    if (logicalId >= logicalCount)
        return kNoTimestampMs;
    return tsBlocks.at(int(logicalId / LineIndex::kTsBlockEntries))
        ->ms[logicalId % LineIndex::kTsBlockEntries];
}

inline bool rangeOut(qint64 minMs, qint64 maxMs, qint64* outMin, qint64* outMax)
{
    if (!hasTimestampMs(maxMs))
        return false;
    if (outMin)
        *outMin = minMs;
    if (outMax)
        *outMax = maxMs;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Читатели LineIndex (GUI-поток)
// ---------------------------------------------------------------------------

qint64 LineIndex::lineCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_lineCount;
}

quint32 LineIndex::logicalEntryCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_logicalCount;
}

qint64 LineIndex::endOffset() const
{
    QMutexLocker lock(&m_mutex);
    return m_endOffset;
}

bool LineIndex::lastLineProvisional() const
{
    QMutexLocker lock(&m_mutex);
    if (m_lineCount <= 0)
        return false;
    const quint8 eol = blockFor(m_blocks, m_lineCount - 1)->eolBytes[slotFor(m_lineCount - 1)];
    return eol == 0 || eol == kEolProvisionalCr;
}

qint64 LineIndex::lineStartOffset(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return -1;
    return startOffsetOf(m_blocks, line);
}

quint32 LineIndex::lineByteLength(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return 0;
    return byteLengthOf(m_blocks, line);
}

quint8 LineIndex::lineEolBytes(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return 0;
    return eolSpan(blockFor(m_blocks, line)->eolBytes[slotFor(line)]);
}

LogLevel LineIndex::level(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return LogLevel::Unknown;
    return LogLevel(blockFor(m_blocks, line)->meta[slotFor(line)] & kLevelMask);
}

bool LineIndex::isPrimary(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return false;
    return blockFor(m_blocks, line)->meta[slotFor(line)] & kPrimaryBit;
}

bool LineIndex::isPlainTextLine(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return false;
    return blockFor(m_blocks, line)->meta[slotFor(line)] & kPlainTextBit;
}

quint32 LineIndex::logicalId(qint64 line) const
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return 0;
    return blockFor(m_blocks, line)->logicalId[slotFor(line)];
}

qint64 LineIndex::timestampMs(quint32 logicalId) const
{
    QMutexLocker lock(&m_mutex);
    return tsOf(m_tsBlocks, logicalId, m_logicalCount);
}

bool LineIndex::timeRangeMs(qint64* minMs, qint64* maxMs) const
{
    QMutexLocker lock(&m_mutex);
    return rangeOut(m_minMs, m_maxMs, minMs, maxMs);
}

LineIndexSnapshot LineIndex::snapshot() const
{
    QMutexLocker lock(&m_mutex);
    LineIndexSnapshot snap;
    snap.m_blocks = m_blocks;        // копия вектора shared_ptr — O(число блоков)
    snap.m_tsBlocks = m_tsBlocks;
    snap.m_lineCount = m_lineCount;
    snap.m_logicalCount = m_logicalCount;
    snap.m_minMs = m_minMs;
    snap.m_maxMs = m_maxMs;
    return snap;
}

// ---------------------------------------------------------------------------
// Аппендер (поток индексатора). Заполнение текущего блока идёт БЕЗ мьютекса —
// читатели не видят строк за m_lineCount; мьютекс берётся только на рост
// вектора блоков и на publish().
// ---------------------------------------------------------------------------

void LineIndex::beginAppend(qint64 startOffset)
{
    m_nextOffset = startOffset;
}

void LineIndex::appendLine(const LineRecord& rec)
{
    const int slot = slotFor(m_pendingLines);
    Block* tail = nullptr;

    const qint64 lineSpan = qint64(rec.byteLength) + eolSpan(rec.eolBytes);

    if (slot == 0) {
        // Новый блок: baseOffset — начало этой строки.
        auto block = std::make_shared<Block>();
        block->baseOffset = m_nextOffset;
        block->relStart[0] = 0;
        QMutexLocker lock(&m_mutex);
        m_blocks.append(std::move(block));
        // constLast: неконстантный last() может сделать COW-detach вектора,
        // разделённого со снапшотом, — это мутация m_blocks вне контракта.
        tail = m_blocks.constLast().get();
    } else {
        tail = m_blocks.constLast().get();
        // Кумулятивные relStart — quint32: блок не может покрыть > 4 ГБ.
        // Патологические сверхдлинные строки закрываем блоком досрочно…
        // …но досрочное закрытие ломает адресацию line→block/slot (деление на
        // kBlockLines). Поэтому вместо этого просто требуем, чтобы одна строка
        // не переполняла relStart; строки > ~4 ГБ вне поддержки.
        Q_ASSERT(qint64(tail->relStart[slot]) + lineSpan <= qint64(UINT32_MAX));
    }

    tail->relStart[slot + 1] = tail->relStart[slot] + quint32(lineSpan);
    tail->eolBytes[slot] = rec.eolBytes;
    quint8 meta = quint8(int(rec.level) & kLevelMask);
    if (rec.isPrimary)        meta |= kPrimaryBit;
    if (rec.hasOwnTimestamp)  meta |= kOwnTimestampBit;
    if (rec.isPlainText)      meta |= kPlainTextBit;
    tail->meta[slot] = meta;
    tail->logicalId[slot] = rec.logicalId;

    if (rec.logicalId >= m_pendingLogical)
        m_pendingLogical = rec.logicalId + 1;

    ++m_pendingLines;
    m_nextOffset += lineSpan;
}

void LineIndex::setLogicalTimestamp(quint32 logicalId, qint64 tsMs)
{
    const int blockIdx = int(logicalId / kTsBlockEntries);
    if (blockIdx >= m_tsBlocks.size()) {
        auto block = std::make_shared<TsBlock>();
        std::fill(std::begin(block->ms), std::end(block->ms), kNoTimestampMs);
        QMutexLocker lock(&m_mutex);
        m_tsBlocks.append(std::move(block));
    }
    TsBlock* block = m_tsBlocks.at(blockIdx).get();
    block->ms[logicalId % kTsBlockEntries] = tsMs;
    if (hasTimestampMs(tsMs)) {
        // Поля блока читает только аппендер (truncateFrom) — без мьютекса.
        block->minMs = qMin(block->minMs, tsMs);
        block->maxMs = qMax(block->maxMs, tsMs);
        m_pendingMinMs = qMin(m_pendingMinMs, tsMs);
        m_pendingMaxMs = qMax(m_pendingMaxMs, tsMs);
    }
}

void LineIndex::publish()
{
    QMutexLocker lock(&m_mutex);
    m_lineCount = m_pendingLines;
    m_logicalCount = m_pendingLogical;
    m_endOffset = m_nextOffset;
    m_minMs = m_pendingMinMs;
    m_maxMs = m_pendingMaxMs;
}

void LineIndex::truncateFrom(qint64 line)
{
    QMutexLocker lock(&m_mutex);
    if (line < 0 || line >= m_lineCount)
        return;

    // Целые блоки после усечённого — долой; частично занятый — COW-копия,
    // чтобы живые снапшоты продолжали видеть старую версию.
    const int keepBlocks = int(line / kBlockLines) + (slotFor(line) > 0 ? 1 : 0);
    m_blocks.resize(keepBlocks);
    if (slotFor(line) > 0) {
        auto copy = std::make_shared<Block>(*m_blocks.last());
        m_blocks.last() = std::move(copy);
    }

    // Таймстампы: записи с id >= logicalId усечённой строки будут перезаписаны
    // переиндексацией — COW-подмена их блока снимает гонку со снапшотами.
    const quint32 cutLogical = (slotFor(line) > 0 || keepBlocks > 0)
        ? (line > 0 ? blockFor(m_blocks, line - 1)->logicalId[slotFor(line - 1)] + 1
                    : 0)
        : 0;
    const int keepTsBlocks = int(cutLogical / kTsBlockEntries)
        + (cutLogical % kTsBlockEntries ? 1 : 0);
    if (keepTsBlocks <= m_tsBlocks.size()) {
        m_tsBlocks.resize(keepTsBlocks);
        if (cutLogical % kTsBlockEntries && !m_tsBlocks.isEmpty()) {
            auto copy = std::make_shared<TsBlock>(*m_tsBlocks.last());
            m_tsBlocks.last() = std::move(copy);
        }
    }

    // Минимум и максимум уцелевших записей: целые блоки — по своим полям,
    // частичный (уже COW-копия) — пересчётом по его записям до cutLogical,
    // хвост копии — «метки нет» (будет переписан). O(число блоков + 4096).
    qint64 minMs = std::numeric_limits<qint64>::max();
    qint64 maxMs = kNoTimestampMs;
    const int fullTsBlocks = int(qMin<qint64>(cutLogical / kTsBlockEntries, m_tsBlocks.size()));
    for (int i = 0; i < fullTsBlocks; ++i) {
        const TsBlock* b = m_tsBlocks.at(i).get();
        minMs = qMin(minMs, b->minMs);
        maxMs = qMax(maxMs, b->maxMs);
    }
    const int partial = int(cutLogical % kTsBlockEntries);
    if (partial > 0 && fullTsBlocks < m_tsBlocks.size()) {
        TsBlock* b = m_tsBlocks[fullTsBlocks].get(); // копия, снапшотам не видна
        std::fill(b->ms + partial, b->ms + kTsBlockEntries, kNoTimestampMs);
        b->minMs = std::numeric_limits<qint64>::max();
        b->maxMs = kNoTimestampMs;
        for (int k = 0; k < partial; ++k) {
            if (hasTimestampMs(b->ms[k])) {
                b->minMs = qMin(b->minMs, b->ms[k]);
                b->maxMs = qMax(b->maxMs, b->ms[k]);
            }
        }
        minMs = qMin(minMs, b->minMs);
        maxMs = qMax(maxMs, b->maxMs);
    }
    m_pendingMinMs = m_minMs = minMs;
    m_pendingMaxMs = m_maxMs = maxMs;

    m_pendingLines = line;
    m_pendingLogical = cutLogical;
    m_lineCount = line;
    m_logicalCount = cutLogical;
    m_endOffset = line > 0 ? startOffsetOf(m_blocks, line - 1)
                                 + byteLengthOf(m_blocks, line - 1)
                                 + eolSpan(blockFor(m_blocks, line - 1)->eolBytes[slotFor(line - 1)])
                           : (m_blocks.isEmpty() ? 0 : m_blocks.first()->baseOffset);
}

// ---------------------------------------------------------------------------
// LineIndexSnapshot
// ---------------------------------------------------------------------------

qint64 LineIndexSnapshot::lineStartOffset(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return -1;
    return startOffsetOf(m_blocks, line);
}

quint32 LineIndexSnapshot::lineByteLength(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return 0;
    return byteLengthOf(m_blocks, line);
}

quint8 LineIndexSnapshot::lineEolBytes(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return 0;
    return LineIndex::eolSpan(blockFor(m_blocks, line)->eolBytes[slotFor(line)]);
}

LogLevel LineIndexSnapshot::level(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return LogLevel::Unknown;
    return LogLevel(blockFor(m_blocks, line)->meta[slotFor(line)]
                    & LineIndex::kLevelMask);
}

bool LineIndexSnapshot::isPrimary(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return false;
    return blockFor(m_blocks, line)->meta[slotFor(line)] & LineIndex::kPrimaryBit;
}

bool LineIndexSnapshot::isPlainTextLine(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return false;
    return blockFor(m_blocks, line)->meta[slotFor(line)] & LineIndex::kPlainTextBit;
}

quint32 LineIndexSnapshot::logicalId(qint64 line) const
{
    if (line < 0 || line >= m_lineCount)
        return 0;
    return blockFor(m_blocks, line)->logicalId[slotFor(line)];
}

qint64 LineIndexSnapshot::timestampMs(quint32 logicalId) const
{
    return tsOf(m_tsBlocks, logicalId, m_logicalCount);
}

bool LineIndexSnapshot::timeRangeMs(qint64* minMs, qint64* maxMs) const
{
    return rangeOut(m_minMs, m_maxMs, minMs, maxMs);
}
