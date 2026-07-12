#ifndef TEXTCHUNKCACHE_H
#define TEXTCHUNKCACHE_H

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>

// ============================================================================
// TextChunkCache — чтение текста строк большого файла по требованию.
// Файл разбивается на выровненные байтовые чанки по 4 МБ; чанки держатся в
// LRU-кэше с бюджетом по байтам. lineText() собирает строку из 1–2 чанков и
// декодирует UTF-8 (строки длиннее двух чанков читаются напрямую с диска).
//
// ТОЛЬКО GUI-поток (путь отображения). Последовательные сканы (фильтр,
// статистика, поиск) НЕ ходят сюда — у них свой SequentialLineReader с
// собственным буфером, чтобы скан не вымывал кэш вьюпорта.
// ============================================================================
class TextChunkCache {
public:
    static constexpr qint64 kChunkBytes = 4 * 1024 * 1024;

    explicit TextChunkCache(qint64 budgetBytes = 256ll * 1024 * 1024)
        : m_budgetBytes(budgetBytes)
    {
    }

    void setBudgetBytes(qint64 budget);

    // Зарегистрировать файл и получить его id для lineText().
    int addFile(const QString& filePath);
    void clear();

    // Текст строки [offset, offset+byteLength) файла fileId, декодированный
    // из UTF-8. Пустая строка при ошибке чтения.
    QString lineText(int fileId, qint64 offset, quint32 byteLength) const;

    // Сбросить кэшированные чанки одного файла (файл заменён/переписан).
    void invalidateFile(int fileId);

private:
    struct Chunk {
        QByteArray bytes;
        quint64 lastUse = 0;
    };
    struct FileSlot {
        QString path;
        // Ленивая ручка файла: открывается при первом чтении.
        mutable std::unique_ptr<QFile> file;
    };

    const QByteArray* chunkAt(int fileId, qint64 chunkIndex) const;
    bool readRange(int fileId, qint64 offset, qint64 length, QByteArray& out) const;
    void evictIfNeeded() const;

    std::vector<FileSlot> m_files;
    mutable QHash<quint64, Chunk> m_chunks; // ключ: fileId << 40 | chunkIndex
    mutable qint64 m_cachedBytes = 0;
    mutable quint64 m_useCounter = 0;
    qint64 m_budgetBytes;
};

#endif // TEXTCHUNKCACHE_H
