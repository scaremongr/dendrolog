#ifndef TEXTCHUNKCACHE_H
#define TEXTCHUNKCACHE_H

#include "textencoding.h"

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
// декодирует декодером файла (строки длиннее двух чанков читаются напрямую с
// диска). В чанках — сырые байты: смена декодера кэш не инвалидирует.
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
    qint64 budgetBytes() const { return m_budgetBytes; }

    // Зарегистрировать файл и получить его id для lineText().
    int addFile(const QString& filePath, TextDecoder decoder = TextDecoder());
    // Сменить декодер файла (F-1): чанки остаются — в них сырые байты.
    void setFileDecoder(int fileId, TextDecoder decoder);
    void clear();

    // Текст строки [offset, offset+byteLength) файла fileId, декодированный
    // декодером файла. Пустая строка при ошибке чтения.
    QString lineText(int fileId, qint64 offset, quint32 byteLength) const;
    // Стиль ANSI строки (F-3): спаны SGR в координатах lineText(); пусто —
    // в строке нет ESC (проверка memchr по байтам чанка, без копии) или
    // строка длиннее чанка (её байты читались бы с диска второй раз).
    AnsiStyles lineAnsiStyles(int fileId, qint64 offset, quint32 byteLength) const;

    // Сбросить кэшированные чанки одного файла (файл заменён/переписан).
    void invalidateFile(int fileId);

private:
    struct Chunk {
        QByteArray bytes;
        quint64 lastUse = 0;
    };
    struct FileSlot {
        QString path;
        TextDecoder decoder;
        // Ручка файла НЕ хранится: см. chunkAt() — лог не должен быть залочен.
    };

    const QByteArray* chunkAt(int fileId, qint64 chunkIndex) const;
    // Как chunkAt, но короткий (последний на момент чтения) чанк, не
    // покрывающий neededBytes, перечитывается: файл мог вырасти.
    const QByteArray* chunkCovering(int fileId, qint64 chunkIndex, qint64 neededBytes) const;
    bool readRange(int fileId, qint64 offset, qint64 length, QByteArray& out) const;
    // Байты строки: f(data, len) прямо из чанка либо из собранной копии;
    // false — не прочитались.
    template <class F>
    bool withLineBytes(int fileId, qint64 offset, quint32 byteLength, F&& f) const;
    void evictIfNeeded() const;

    std::vector<FileSlot> m_files;
    mutable QHash<quint64, Chunk> m_chunks; // ключ: fileId << 40 | chunkIndex
    mutable qint64 m_cachedBytes = 0;
    mutable quint64 m_useCounter = 0;
    qint64 m_budgetBytes;
};

#endif // TEXTCHUNKCACHE_H
