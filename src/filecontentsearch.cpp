#include "filecontentsearch.h"

#include "gzipdevice.h"
#include "userregex.h"

#include <QFile>
#include <QList>
#include <QStringDecoder>

#include <algorithm>
#include <optional>

// ─── ContentQuery ────────────────────────────────────────────────────────────

ContentQuery::ContentQuery(const QString& text, bool isRegex, Qt::CaseSensitivity cs)
    : m_text(text), m_isRegex(isRegex), m_cs(cs)
{
    if (m_isRegex)
        m_re = UserRegex::compile(text, cs); // уже с JIT: воркеры не компилируют
}

bool ContentQuery::isUsable() const
{
    return !m_text.isEmpty() && (!m_isRegex || m_re.isValid());
}

bool ContentQuery::matchesLines(QStringView text) const
{
    if (!m_isRegex)
        return text.contains(m_text, m_cs);

    qsizetype start = 0;
    while (start < text.size()) {
        const qsizetype nl = text.indexOf(u'\n', start);
        QStringView line = nl < 0 ? text.sliced(start) : text.sliced(start, nl - start);
        if (line.endsWith(u'\r'))
            line.chop(1);
        if (m_re.matchView(line).hasMatch())
            return true;
        if (nl < 0)
            break;
        start = nl + 1;
    }
    return false;
}

// ─── Поиск ───────────────────────────────────────────────────────────────────

namespace FileContentSearch {
namespace {

// Сдвиг назад к началу символа UTF-8 (не дальше трёх байтов продолжения).
// Для однобайтных кодировок лишний сдвиг безвреден: кусок лишь короче.
qsizetype alignToCharStart(QByteArrayView bytes, qsizetype pos)
{
    for (int steps = 0; steps < 3 && pos > 0 && pos < bytes.size()
                        && (uchar(bytes[pos]) & 0xC0) == 0x80;
         ++steps)
        --pos;
    return pos;
}

// Конец последнего ЦЕЛОГО символа UTF-8: последовательность, оборванная
// концом данных, остаётся за ним (дочитается следующим блоком).
qsizetype completeCharsEnd(QByteArrayView bytes)
{
    const qsizetype n = bytes.size();
    qsizetype i = n;
    for (int back = 0; back < 3 && i > 0 && (uchar(bytes[i - 1]) & 0xC0) == 0x80; ++back)
        --i;
    if (i == 0)
        return n;
    const uchar lead = uchar(bytes[i - 1]);
    const qsizetype length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return length > 1 && n - (i - 1) < length ? i - 1 : n;
}

// Чтение блоками: файл как есть или поток распакованных байтов `.gz` (F-2,
// PR 3). Смещения, которые видит поиск, — в распакованном потоке; прогресс —
// в байтах файла на диске (у `.gz` — сжатых, как и колонка Size сканера).
class Reader
{
public:
    Reader(QFile& file, GzipDevice* gzip, const Options& options)
        : m_file(file), m_gzip(gzip), m_options(options)
    {
    }

    bool cancelled() const
    {
        return m_options.cancelled && m_options.cancelled->load(std::memory_order_relaxed);
    }

    // Следующий блок; пустой — конец файла (или ошибка потока). Вид в буфер
    // читателя, живёт до следующего вызова (буфер один на файл: без выделения
    // на блок). Прогресс — только байты, прочитанные впервые (перечитывание
    // после пересмотра его не двигает).
    QByteArrayView next()
    {
        m_buffer.resize(0);
        readMore(m_options.blockBytes);
        if (m_options.afterBlock)
            m_options.afterBlock(m_pos);
        return m_buffer;
    }

    // Дочитать в текущий блок ещё до `more` байтов (окно определения
    // кодировки у потока, которому второй дескриптор не поможет). Вид — на
    // весь блок, с прежним началом.
    QByteArrayView extend(qint64 more)
    {
        readMore(more);
        return m_buffer;
    }

    qint64 counted() const { return m_counted; }

    // Перечитать с offset (смещение в потоке). У `.gz` — заново с начала и
    // пропустить offset байтов: поток не перематывается.
    bool seek(qint64 offset)
    {
        if (!m_gzip) {
            if (!m_file.seek(offset))
                return false;
            m_pos = offset;
            return true;
        }
        m_gzip->close();
        if (!m_file.seek(0) || !m_gzip->open(QIODevice::ReadOnly))
            return false;
        m_pos = 0;
        QByteArray skip;
        while (m_pos < offset) {
            if (cancelled())
                return false;
            skip.resize(qMin<qint64>(m_options.blockBytes, offset - m_pos));
            const qint64 got = m_gzip->read(skip.data(), skip.size());
            if (got <= 0)
                return false;
            m_pos += got;
            account();
        }
        return true;
    }

private:
    QIODevice& device() { return m_gzip ? static_cast<QIODevice&>(*m_gzip) : m_file; }

    void readMore(qint64 more)
    {
        const qsizetype had = m_buffer.size();
        m_buffer.resize(had + more);
        qsizetype filled = had;
        while (filled < m_buffer.size()) {
            const qint64 got = device().read(m_buffer.data() + filled, m_buffer.size() - filled);
            if (got <= 0)
                break;
            filled += got;
        }
        m_buffer.resize(filled);
        m_pos += filled - had;
        account();
    }

    void account()
    {
        const qint64 progress = m_gzip ? m_gzip->compressedBytesRead() : m_pos;
        if (progress > m_counted) {
            if (m_options.bytesDone)
                m_options.bytesDone->fetch_add(progress - m_counted, std::memory_order_relaxed);
            m_counted = progress;
        }
    }

    QFile& m_file;
    GzipDevice* m_gzip;
    const Options& m_options;
    qint64 m_pos = 0;       // смещение в (распакованном) потоке
    qint64 m_counted = 0;   // байтов файла, учтённых в прогрессе
    QByteArray m_buffer;
};

// UTF-16/32 по BOM: потоковый QStringDecoder (BOM он пропускает сам), куски
// режутся по '\n' уже в тексте.
bool searchWide(Reader& reader, QByteArrayView first, TextEncoding encoding,
                const ContentQuery& query, const Options& options, bool& cancelled)
{
    QStringDecoder decoder(*TextEncodings::toQt(encoding));
    const qsizetype overlap = query.isRegex() ? 0 : query.text().size() - 1;
    QString pending;
    QByteArrayView block = first;
    while (!block.isEmpty()) {
        pending += decoder.decode(block);
        const qsizetype nl = pending.lastIndexOf(u'\n');
        if (nl >= 0) {
            if (query.matchesLines(QStringView(pending).first(nl + 1)))
                return true;
            pending.remove(0, nl + 1);
        } else if (pending.size() > options.maxLineBytes) {
            if (query.matchesLines(pending))
                return true;
            pending = pending.right(overlap);
        }
        if (reader.cancelled()) {
            cancelled = true;
            return false;
        }
        block = reader.next();
    }
    return !pending.isEmpty() && query.matchesLines(pending);
}

// UTF-8 и однобайтные: сырые байты, решение — EncodingDetector::advance.
class ByteSearch
{
public:
    enum class Step { Continue, Found, Restart };

    ByteSearch(const QString& path, const ContentQuery& query, const Options& options,
               FileEncoding& state)
        : m_path(path), m_query(query), m_options(options), m_state(state)
    {
        m_testedWith = m_state.decoder;
    }

    // Состояние автоматики не сбрасывается: перечитанные байты advance
    // пропускает по evidence.end.
    void restart()
    {
        m_restartOffset = -1;
        m_pending.clear();
        m_retained.clear();
        m_retainedBytes = 0;
        m_overflow = false;
        m_tentative = false;
        m_testedWith = m_state.decoder;
    }

    // Откуда перечитывать при Restart: начало первой запомненной строки (всё
    // до неё — ASCII или проверено окончательно).
    qint64 restartOffset() const { return m_restartOffset; }

    Step feed(QByteArrayView block, qint64 offset)
    {
        EncodingDetector::advance(m_state, block, offset, m_path, m_options.fallback);
        if (const Step s = revisit(); s != Step::Continue)
            return s;

        // Блок проверяется на месте; копируется только начало строки, не
        // дочитанной до '\n' (обычно короткое), — без перевыделения на блок.
        QByteArrayView data = block;
        qint64 dataOffset = offset;
        if (!m_pending.isEmpty()) {
            const qsizetype nl = data.indexOf('\n');
            if (nl < 0) {
                m_pending.append(data);
                return flushOverlong();
            }
            m_pending.append(data.first(nl + 1));
            const Step s = process(m_pending, m_pendingOffset);
            m_pending.resize(0);
            if (s != Step::Continue)
                return s;
            data = data.sliced(nl + 1);
            dataOffset += nl + 1;
        }
        const qsizetype nl = data.lastIndexOf('\n');
        if (nl >= 0) {
            if (const Step s = process(data.first(nl + 1), dataOffset); s != Step::Continue)
                return s;
            data = data.sliced(nl + 1);
            dataOffset += nl + 1;
        }
        m_pendingOffset = dataOffset;
        m_pending.append(data);
        return flushOverlong();
    }

    Step finish()
    {
        if (const Step s = process(m_pending, m_pendingOffset); s != Step::Continue)
            return s;
        m_pending.resize(0);
        return m_tentative ? Step::Found : Step::Continue;
    }

private:
    // Строка без конца длиннее предела: кусками, у подстроки — с перекрытием
    // на длину запроса (в байтах UTF-8 символ UTF-16 — не больше трёх).
    Step flushOverlong()
    {
        if (m_pending.size() <= m_options.maxLineBytes)
            return Step::Continue;
        const qsizetype flushEnd = completeCharsEnd(m_pending);
        if (const Step s = process(QByteArrayView(m_pending).first(flushEnd), m_pendingOffset);
            s != Step::Continue)
            return s;
        // search() держит предел строки больше перекрытия: кусок сдвигается.
        const qsizetype overlap = m_query.isRegex() ? 0 : 3 * (m_query.text().size() - 1);
        const qsizetype drop = alignToCharStart(m_pending, std::max<qsizetype>(0, flushEnd - overlap));
        m_pending.remove(0, drop);
        m_pendingOffset += drop;
        return Step::Continue;
    }

    // Решение могло смениться: предварительные находки — заново, новым
    // декодером; окончательное решение делает их окончательными.
    Step revisit()
    {
        if (m_state.decoder != m_testedWith) {
            if (m_overflow)
                return Step::Restart;
            m_tentative = false;
            for (const QByteArray& line : std::as_const(m_retained)) {
                if (m_query.matchesLines(m_state.decoder.decode(line))) {
                    m_tentative = true;
                    break;
                }
            }
            m_testedWith = m_state.decoder;
        }
        if (m_state.settled()) {
            if (m_tentative)
                return Step::Found;
            m_retained.clear();
            m_retainedBytes = 0;
            m_overflow = false;
        }
        return Step::Continue;
    }

    bool matches(QByteArrayView bytes) const
    {
        return !bytes.isEmpty() && m_query.matchesLines(m_state.decoder.decode(bytes));
    }

    // bytes начинается в файле со смещения fileOffset.
    Step process(QByteArrayView bytes, qint64 fileOffset)
    {
        if (bytes.isEmpty())
            return Step::Continue;
        // «Не решено» (всё прочитанное — ASCII; у последней строки файла —
        // разве что оборванная последовательность, её и вкладка читает как
        // UTF-8) и окончательное решение: находка окончательна.
        if (!m_state.decided() || m_state.settled())
            return matches(bytes) ? Step::Found : Step::Continue;

        // Предварительное: ASCII-строки — окончательно, остальные — запомнить.
        const qsizetype n = bytes.size();
        qsizetype pos = 0;
        while (pos < n) {
            const qsizetype high = EncodingDetector::firstHighByte(bytes.data() + pos, n - pos);
            if (high < 0)
                return matches(bytes.sliced(pos)) ? Step::Found : Step::Continue;
            const qsizetype at = pos + high;
            const qsizetype lineStart = std::max(pos, bytes.lastIndexOf('\n', at) + 1);
            if (matches(bytes.sliced(pos, lineStart - pos)))
                return Step::Found;
            const qsizetype nl = bytes.indexOf('\n', at);
            const qsizetype lineEnd = nl < 0 ? n : nl + 1;
            const QByteArrayView line = bytes.sliced(lineStart, lineEnd - lineStart);
            if (matches(line))
                m_tentative = true;
            if (m_restartOffset < 0)
                m_restartOffset = fileOffset + lineStart;
            if (!m_overflow) {
                if (m_retainedBytes + line.size() > m_options.retainedLimitBytes) {
                    m_overflow = true;
                    m_retained.clear();
                    m_retainedBytes = 0;
                } else {
                    m_retained.append(line.toByteArray());
                    m_retainedBytes += line.size();
                }
            }
            pos = lineEnd;
        }
        return Step::Continue;
    }

    const QString& m_path;
    const ContentQuery& m_query;
    const Options& m_options;
    FileEncoding& m_state;

    QByteArray m_pending;              // начало строки, не дочитанной до '\n'
    qint64 m_pendingOffset = 0;        // его смещение в файле
    QList<QByteArray> m_retained;      // не-ASCII строки под предварительным решением
    qsizetype m_retainedBytes = 0;
    qint64 m_restartOffset = -1;       // первая из них в файле
    bool m_overflow = false;           // m_retained не влез: пересмотр перечитывает файл
    bool m_tentative = false;          // находка среди m_retained декодером m_testedWith
    TextDecoder m_testedWith;
};

} // namespace

Result search(const QString& path, const ContentQuery& query, const Options& options)
{
    Result result;
    if (!query.isUsable())
        return result;   // файл не открываем: искать нечем
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.readable = false;
        return result;
    }
    // Сжатый лог (F-2) узнаётся по сигнатуре, а не по имени — как вкладка
    // узнаёт его при открытии. Читается потоком, без записи на диск.
    std::optional<GzipDevice> gzip;
    if (GzipDevice::hasSignature(file.peek(2))) {
        gzip.emplace(&file);
        gzip->setAbortFlag(options.cancelled);
        if (!gzip->open(QIODevice::ReadOnly)) {
            result.readable = false;
            return result;
        }
    }

    Options opts = options;
    opts.blockBytes = std::max<qint64>(opts.blockBytes, 16);  // BOM целиком в первом блоке
    // Кусок длинной строки обязан быть длиннее перекрытия подстроки, иначе
    // flushOverlong ничего не отбросит и буфер будет расти.
    opts.maxLineBytes = std::max<qsizetype>(opts.maxLineBytes, 8 * query.text().size() + 16);
    Reader reader(file, gzip ? &*gzip : nullptr, opts);
    const auto finishResult = [&] {
        result.bytesRead = reader.counted();
        if (gzip && gzip->error() == GzipDevice::Error::Aborted)
            result.cancelled = true;
        else if (gzip && gzip->error() != GzipDevice::Error::None)
            result.readable = false;   // обрыв, CRC, мусор: ответ по прочитанному до ошибки
        if (result.cancelled)
            result.matched = false;
        return result;
    };

    QByteArrayView block = reader.next();
    if (block.isEmpty())
        return finishResult();

    qint64 start = 0;
    if (const std::optional<TextEncodings::Bom> bom = TextEncodings::detectBom(block)) {
        result.usedEncoding = bom->encoding;
        if (bom->encoding != TextEncoding::Utf8) {
            bool cancelled = false;
            result.matched = searchWide(reader, block, bom->encoding, query, opts,
                                        cancelled);
            result.cancelled = cancelled;
            return finishResult();
        }
        result.encoding.source = FileEncoding::Source::Bom;
        start = bom->length;
        block = block.sliced(start);
    }

    FileEncoding& state = result.encoding;
    // Окно решения о кодировке, не уместившееся в блок, advance дочитывает
    // вторым дескриптором по пути файла. У потока пути нет (в файле — сжатые
    // байты): окно дочитывается в сам блок, и advance его не открывает.
    const QString windowPath = gzip ? QString() : path;
    ByteSearch scan(windowPath, query, opts, state);
    qint64 offset = start;
    qsizetype skip = start;   // начало блока в буфере читателя (BOM первого блока)
    while (true) {
        if (reader.cancelled()) {
            result.cancelled = true;
            break;
        }
        if (block.isEmpty()) {
            // Конец файла: решение больше не меняется (finish без advance).
            result.matched = scan.finish() == ByteSearch::Step::Found;
            break;
        }
        if (gzip && !state.decided()) {
            const qsizetype high = EncodingDetector::firstHighByte(block);
            if (high >= 0 && block.size() - high < EncodingDetector::kWindowBytes)
                block = reader.extend(EncodingDetector::kWindowBytes - (block.size() - high))
                            .sliced(skip);
        }
        const ByteSearch::Step step = scan.feed(block, offset);
        offset += block.size();
        skip = 0;
        if (step == ByteSearch::Step::Found) {
            result.matched = true;
            break;
        }
        if (step == ByteSearch::Step::Restart) {
            // Запомненные строки не влезли: перечитать с первой из них.
            const qint64 from = std::max(start, scan.restartOffset());
            scan.restart();
            if (!reader.seek(from)) {
                result.cancelled = reader.cancelled();
                break;
            }
            offset = from;
        }
        block = reader.next();
    }

    result.usedEncoding = state.decoder.encoding();
    result.dependsOnFallback = state.source == FileEncoding::Source::Auto
        && TextEncodings::isSingleByte(state.decoder.encoding());
    return finishResult();
}

} // namespace FileContentSearch
