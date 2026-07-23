#include "textchunkcache.h"

#include <QVector>

namespace {
inline quint64 chunkKey(int fileId, qint64 chunkIndex)
{
    return (quint64(quint32(fileId)) << 40) | quint64(chunkIndex);
}
} // namespace

void TextChunkCache::setBudgetBytes(qint64 budget)
{
    m_budgetBytes = qMax<qint64>(budget, kChunkBytes);
    evictIfNeeded();
}

int TextChunkCache::addFile(const QString& filePath)
{
    FileSlot slot;
    slot.path = filePath;
    m_files.push_back(std::move(slot));
    return int(m_files.size()) - 1;
}

void TextChunkCache::clear()
{
    m_files.clear();
    m_chunks.clear();
    m_cachedBytes = 0;
}

void TextChunkCache::invalidateFile(int fileId)
{
    for (auto it = m_chunks.begin(); it != m_chunks.end();) {
        if (int(it.key() >> 40) == fileId) {
            m_cachedBytes -= it.value().bytes.size();
            it = m_chunks.erase(it);
        } else {
            ++it;
        }
    }
}

const QByteArray* TextChunkCache::chunkAt(int fileId, qint64 chunkIndex) const
{
    const quint64 key = chunkKey(fileId, chunkIndex);
    auto it = m_chunks.find(key);
    if (it != m_chunks.end()) {
        it.value().lastUse = ++m_useCounter;
        return &it.value().bytes;
    }

    if (fileId < 0 || fileId >= int(m_files.size()))
        return nullptr;

    // Ручку файла между чтениями НЕ держим: открытый QFile на Windows (без
    // FILE_SHARE_DELETE) не даёт ни удалить, ни переименовать лог — ломались бы
    // и ротация со стороны писателя, и пересоздание файла пользователем.
    // Открытие на промах кэша — раз в 4 МБ, на фоне самого чтения незаметно.
    QFile file(m_files[size_t(fileId)].path);
    if (!file.open(QIODevice::ReadOnly))
        return nullptr;
    if (!file.seek(chunkIndex * kChunkBytes))
        return nullptr;
    QByteArray bytes = file.read(kChunkBytes);
    if (bytes.isEmpty())
        return nullptr;

    // ВАЖНО: вытеснение — ДО вставки. Удаление из Qt6 QHash сдвигает соседние
    // элементы (backward-shift), т.е. инвалидирует чужие итераторы/указатели:
    // «вставить, потом вытеснить, потом вернуть указатель по старому
    // итератору» — это висячий указатель и падение в release.
    m_cachedBytes += bytes.size();
    evictIfNeeded();

    Chunk chunk;
    chunk.bytes = std::move(bytes);
    chunk.lastUse = ++m_useCounter;
    it = m_chunks.insert(key, std::move(chunk));
    // После вставки хэш больше не мутирует — указатель стабилен до возврата.
    return &it.value().bytes;
}

void TextChunkCache::evictIfNeeded() const
{
    while (m_cachedBytes > m_budgetBytes && !m_chunks.isEmpty()) {
        auto victim = m_chunks.end();
        quint64 oldest = ~0ull;
        for (auto it = m_chunks.begin(); it != m_chunks.end(); ++it) {
            if (it.value().lastUse < oldest) {
                oldest = it.value().lastUse;
                victim = it;
            }
        }
        if (victim == m_chunks.end())
            return;
        m_cachedBytes -= victim.value().bytes.size();
        m_chunks.erase(victim);
    }
}

bool TextChunkCache::readRange(int fileId, qint64 offset, qint64 length,
                               QByteArray& out) const
{
    // Прямое чтение с диска для строк, не влезающих в пару чанков.
    if (fileId < 0 || fileId >= int(m_files.size()))
        return false;
    QFile f(m_files[size_t(fileId)].path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
        return false;
    out = f.read(length);
    return out.size() == length;
}

QString TextChunkCache::lineText(int fileId, qint64 offset, quint32 byteLength) const
{
    if (byteLength == 0)
        return QString();

    const qint64 firstChunk = offset / kChunkBytes;
    const qint64 lastChunk = (offset + byteLength - 1) / kChunkBytes;

    if (lastChunk == firstChunk) {
        const QByteArray* bytes = chunkAt(fileId, firstChunk);
        const qint64 local = offset - firstChunk * kChunkBytes;
        if (!bytes || local + byteLength > bytes->size())
            return QString();
        return QString::fromUtf8(bytes->constData() + local, int(byteLength));
    }

    if (lastChunk == firstChunk + 1) {
        // Строка на стыке двух чанков: собираем без прямого чтения диска.
        // ВАЖНО: второй chunkAt может вытеснить первый чанк — копируем головку
        // до второго обращения.
        const QByteArray* first = chunkAt(fileId, firstChunk);
        if (!first)
            return QString();
        const qint64 local = offset - firstChunk * kChunkBytes;
        QByteArray assembled;
        assembled.reserve(int(byteLength));
        assembled.append(first->constData() + local, int(first->size() - local));
        const QByteArray* second = chunkAt(fileId, lastChunk);
        if (!second)
            return QString();
        const qint64 remain = byteLength - assembled.size();
        if (remain > second->size())
            return QString();
        assembled.append(second->constData(), int(remain));
        return QString::fromUtf8(assembled);
    }

    // Гигантская строка (> 2 чанков) — мимо кэша, напрямую с диска.
    QByteArray raw;
    if (!readRange(fileId, offset, byteLength, raw))
        return QString();
    return QString::fromUtf8(raw);
}
