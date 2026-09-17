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
        // Промах НИЖЕ окна — скан идёт назад (поиск предыдущего), выше —
        // вперёд. Направление берётся по окну, а не по прошлому запросу:
        // «дрожание» внутри окна промахов не даёт вовсе.
        const bool below = offset < m_winStart;
        if (below || offset + byteLength > m_winStart + qint64(m_window.size())) {
            if (!slideTo(offset, byteLength, /*backward=*/below)) {
                m_scratch.clear();
                return m_scratch;
            }
        }
        const qint64 local = offset - m_winStart;
        m_scratch = QString::fromUtf8(m_window.constData() + local, int(byteLength));
        return m_scratch;
    }

    // Сколько раз окно перечитывалось с диска — наблюдаемая цена скана.
    int slideCount() const { return m_slides; }

private:
    // Окно ставится так, чтобы строка оказалась внутри, а основная часть окна
    // лежала ПО НАПРАВЛЕНИЮ скана: вперёд — окно начинается у строки, назад —
    // заканчивается у неё. Раньше окно всегда начиналось у строки, и скан
    // назад промахивался на КАЖДОЙ строке: seek + чтение целого окна (8 МБ)
    // на строку. Небольшой запас (1/8 окна) в обратную сторону гасит
    // «дрожание»: доступ по времени в мульти-файловой вкладке идёт по файлу
    // почти, но не строго монотонно.
    bool slideTo(qint64 offset, quint32 byteLength, bool backward)
    {
        const qint64 want = qMax<qint64>(m_windowBytes, byteLength);
        const qint64 slack = (want - qint64(byteLength)) / 8;
        const qint64 start = qMax<qint64>(0, backward
            ? offset + qint64(byteLength) + slack - want
            : offset - slack);
        if (!m_file.isOpen() || !m_file.seek(start))
            return false;
        m_window = m_file.read(want);
        m_winStart = start;
        ++m_slides;
        // start <= offset по построению; строка могла не поместиться только
        // если файл короче, чем обещает индекс (обрезан снаружи).
        return offset + qint64(byteLength) <= m_winStart + qint64(m_window.size());
    }

    QFile m_file;
    qint64 m_windowBytes;
    QByteArray m_window;
    qint64 m_winStart = 0;
    QString m_scratch;
    int m_slides = 0;
};

#endif // SEQUENTIALLINEREADER_H
