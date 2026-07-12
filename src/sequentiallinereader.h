#ifndef SEQUENTIALLINEREADER_H
#define SEQUENTIALLINEREADER_H

#include <QByteArray>
#include <QFile>
#include <QString>

// ============================================================================
// SequentialLineReader — последовательное чтение текста строк по байтовым
// смещениям LineIndex для сканов в воркерах (фильтр, статистика, поиск).
// Собственное скользящее окно (8 МБ), НЕ через TextChunkCache — скан не
// вымывает кэш вьюпорта и не требует синхронизации с GUI-потоком.
// Использовать строго из одного потока.
// ============================================================================
class SequentialLineReader {
public:
    explicit SequentialLineReader(const QString& filePath,
                                  qint64 windowBytes = 8 * 1024 * 1024)
        : m_file(filePath)
        , m_windowBytes(qMax<qint64>(windowBytes, 64 * 1024))
    {
    }

    bool open() { return m_file.open(QIODevice::ReadOnly); }

    // Декодированный текст строки [offset, offset+byteLength). Возвращаемая
    // ссылка валидна до следующего вызова lineAt().
    const QString& lineAt(qint64 offset, quint32 byteLength)
    {
        if (byteLength == 0) {
            m_scratch.clear();
            return m_scratch;
        }
        if (offset < m_winStart
            || offset + byteLength > m_winStart + qint64(m_window.size())) {
            if (!slideTo(offset, byteLength)) {
                m_scratch.clear();
                return m_scratch;
            }
        }
        const qint64 local = offset - m_winStart;
        m_scratch = QString::fromUtf8(m_window.constData() + local, int(byteLength));
        return m_scratch;
    }

private:
    bool slideTo(qint64 offset, quint32 byteLength)
    {
        const qint64 want = qMax<qint64>(m_windowBytes, byteLength);
        if (!m_file.isOpen() || !m_file.seek(offset))
            return false;
        m_window = m_file.read(want);
        m_winStart = offset;
        return qint64(m_window.size()) >= qint64(byteLength);
    }

    QFile m_file;
    qint64 m_windowBytes;
    QByteArray m_window;
    qint64 m_winStart = 0;
    QString m_scratch;
};

#endif // SEQUENTIALLINEREADER_H
