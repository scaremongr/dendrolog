#ifndef BYTELINEREADER_H
#define BYTELINEREADER_H

// Построчное чтение сырых байтов файла с точной позицией каждой строки —
// транспорт резидентного пути (LogParser) для всего, кроме UTF-16/32
// (Q-18, docs/design/F-1.md, «Q-18: что сделано»).
//
// Разрез строк и снятие '\r' — ровно как у QTextStream::readLine, которым
// парсер читал раньше (tests/textencoding сверяет на случайных файлах):
//   • строка кончается '\n'; '\r' прямо перед ним снимается ("\r\n" — EOL
//     в два байта), одиночный '\r' посреди строки — часть текста;
//   • последняя строка без '\n' — тоже строка; '\r' в самом её конце
//     снимается (EOL в один байт, но строка не кончилась переводом строки —
//     для дозаписи она оборвана);
//   • за завершающим '\n' пустой строки нет.
//
// Зачем не QTextStream: позицию строки (её ждёт EncodingDetector::advance)
// он отдаёт только через pos(), а pos() перечитывает свой буфер ПО БАЙТУ от
// места, где конец строки последний раз совпал с границей его 16-КБ блока
// чтения, — ~130 нс на байт, на сотнях МБ — секунды на вызов. Здесь позиция —
// счётчик: начало буфера в файле плюс смещение в буфере.
//
// Не потокобезопасен; устройство читается только через этот объект.

#include <QByteArray>
#include <QByteArrayView>
#include <QIODevice>

#include <cstring>

class ByteLineReader
{
public:
    // device уже стоит на startOffset (позиция в файле первой строки).
    ByteLineReader(QIODevice* device, qint64 startOffset, qsizetype chunkBytes = 1 << 20)
        : m_device(device), m_bufferStart(startOffset), m_chunk(qMax<qsizetype>(1, chunkBytes))
    {}

    // Следующая строка; false — строк больше нет (конец данных устройства).
    bool next()
    {
        m_lineStart = m_bufferStart + m_at;
        for (;;) {
            const qsizetype available = m_buffer.size() - m_at;
            if (available > 0) {
                const char* from = m_buffer.constData() + m_at;
                // memchr только по ещё не просмотренному: длинная строка
                // дочитывается кусками, без повторного поиска по началу.
                const qsizetype scanFrom = qMax(m_scanned, m_at);
                const void* nl = std::memchr(m_buffer.constData() + scanFrom, '\n',
                                             size_t(m_buffer.size() - scanFrom));
                if (nl) {
                    const qsizetype end = static_cast<const char*>(nl) - m_buffer.constData();
                    qsizetype textLen = end - m_at;
                    m_eol = 1;
                    if (textLen > 0 && from[textLen - 1] == '\r') {
                        --textLen;
                        m_eol = 2;
                    }
                    m_line = QByteArrayView(from, textLen);
                    m_newline = true;
                    m_at = end + 1;
                    m_scanned = m_at;
                    return true;
                }
                m_scanned = m_buffer.size();
            }
            if (!fill()) {
                if (available <= 0)
                    return false;
                // Последняя строка без '\n'; '\r' в её конце снимает и
                // QTextStream (конец данных устройства).
                const char* from = m_buffer.constData() + m_at;
                qsizetype textLen = available;
                m_eol = 0;
                if (from[textLen - 1] == '\r') {
                    --textLen;
                    m_eol = 1;
                }
                m_line = QByteArrayView(from, textLen);
                m_newline = false;
                m_at = m_buffer.size();
                m_scanned = m_at;
                return true;
            }
        }
    }

    // Байты текста строки без EOL; действительны до следующего next().
    QByteArrayView line() const { return m_line; }
    // Смещение начала строки в файле.
    qint64 lineStart() const { return m_lineStart; }
    // Байтов EOL, снятых со строки: 0, 1 ('\n' или '\r' в конце файла) или 2.
    int eolBytes() const { return m_eol; }
    // Строка кончилась '\n' (не оборвана).
    bool endsWithNewline() const { return m_newline; }
    // Смещение в файле сразу за прочитанным (за EOL последней строки).
    qint64 pos() const { return m_bufferStart + m_at; }

private:
    // Дочитать порцию в конец буфера; прочитанное до m_at выбрасывается.
    bool fill()
    {
        if (m_at > 0) {
            // Остаток — в начало того же хранилища: QByteArray::remove(0, n)
            // оставил бы свободное место в начале, и resize ниже каждый раз
            // выделял бы новый буфер.
            const qsizetype rest = m_buffer.size() - m_at;
            if (rest > 0)
                std::memmove(m_buffer.data(), m_buffer.constData() + m_at, size_t(rest));
            m_buffer.resize(rest);
            m_bufferStart += m_at;
            m_scanned -= m_at;
            m_at = 0;
        }
        const qsizetype had = m_buffer.size();
        m_buffer.resize(had + m_chunk);
        const qint64 got = m_device->read(m_buffer.data() + had, m_chunk);
        m_buffer.resize(had + qMax<qint64>(0, got));
        return got > 0;
    }

    QIODevice* m_device;
    QByteArray m_buffer;
    qint64 m_bufferStart;     // смещение m_buffer[0] в файле
    qsizetype m_at = 0;       // начало непрочитанного в буфере
    qsizetype m_scanned = 0;  // до сюда '\n' уже искали
    qsizetype m_chunk;
    QByteArrayView m_line;
    qint64 m_lineStart = 0;
    int m_eol = 0;
    bool m_newline = false;
};

#endif // BYTELINEREADER_H
