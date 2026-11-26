#include "logparser.h"
#include "patternheuristics.h"
#include "bytelinereader.h"
#include "gzipdevice.h"
#include "ansiescape.h"
#include <QFile>
#include <QStringDecoder>
#include <QTextStream>
#include <QtConcurrent> // Для QtConcurrent::run
#include <QFileInfo>    // Для получения размера файла (для progress)
#include <QStringView>
#include <QDebug>       // Для qWarning()

#include <optional>

LogParser::LogParser(QObject* parent)
    : QObject(parent)
    // Классификация строк (таймстамп/уровень/primary) делегируется
    // LineClassifier — общему с LogIndexer источнику истины.
{
}

LogParser::~LogParser()
{
    // Tell any running parse to stop, drop not-yet-started tasks, then block
    // until the workers have actually returned. This guarantees no worker is
    // touching m_pattern / the regex members while they are being destroyed —
    // the use-after-free that crashed on close.
    m_abort.store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

void LogParser::startParsing(const LogFilePtr& logFile, FileEncoding encoding,
                             FileSchemaPtr schema)
{
    // The schema is the pass's own immutable value (the lambda keeps it
    // alive). The task runs on our own pool, which the destructor joins.
    const TextEncoding fallback = m_fallbackEncoding;
    (void)QtConcurrent::run(&m_pool, [this, logFile, schema = std::move(schema),
                                      encoding, fallback]() {
        this->parseFrom(logFile, 0, ResumeContext(), schema.get(),
                        /*initial=*/true, TextEncoding::Utf8, encoding, fallback);
    });
}

void LogParser::startParsingFrom(const LogFilePtr& logFile, qint64 startOffset,
                                 const ResumeContext& context,
                                 TextEncoding streamEncoding, FileEncoding encoding,
                                 FileSchemaPtr schema)
{
    const TextEncoding fallback = m_fallbackEncoding;
    (void)QtConcurrent::run(&m_pool, [this, logFile, startOffset, context,
                                      schema = std::move(schema), streamEncoding,
                                      encoding, fallback]() {
        this->parseFrom(logFile, startOffset, context, schema.get(),
                        /*initial=*/false, streamEncoding, encoding, fallback);
    });
}

namespace {

// Кодировки с переводом строки шире байта: ширина кодовой единицы и порядок
// байтов. Обобщённые Utf16/Utf32 (без BOM) — порядок байтов машины.
struct CodeUnit {
    int width = 1;
    bool bigEndian = false;
};

CodeUnit codeUnitOf(QStringConverter::Encoding encoding)
{
    const bool hostBig = QSysInfo::ByteOrder == QSysInfo::BigEndian;
    switch (encoding) {
    case QStringConverter::Utf16:   return {2, hostBig};
    case QStringConverter::Utf16LE: return {2, false};
    case QStringConverter::Utf16BE: return {2, true};
    case QStringConverter::Utf32:   return {4, hostBig};
    case QStringConverter::Utf32LE: return {4, false};
    case QStringConverter::Utf32BE: return {4, true};
    default:                        return {};
    }
}

// Смещение в chunk (он начинается на границе единицы) сразу ЗА последним
// переводом строки, либо -1.
qsizetype afterLastNewline(const QByteArray& chunk, CodeUnit unit)
{
    if (unit.width == 1) {
        const qsizetype newline = chunk.lastIndexOf('\n');
        return newline < 0 ? -1 : newline + 1;
    }
    const qsizetype low = unit.bigEndian ? unit.width - 1 : 0; // байт с '\n'
    for (qsizetype at = (chunk.size() / unit.width - 1) * unit.width; at >= 0;
         at -= unit.width) {
        bool newline = chunk[at + low] == '\n';
        for (qsizetype i = 0; newline && i < unit.width; ++i)
            newline = i == low || chunk[at + i] == 0;
        if (newline)
            return at + unit.width;
    }
    return -1;
}

// Файл — ровно метка порядка байтов, без единой строки.
bool isOnlyByteOrderMark(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(5);
    static const QByteArray kMarks[] = {
        QByteArray("\xEF\xBB\xBF", 3),        // UTF-8
        QByteArray("\xFF\xFE", 2),            // UTF-16 LE
        QByteArray("\xFE\xFF", 2),            // UTF-16 BE
        QByteArray("\xFF\xFE\x00\x00", 4),    // UTF-32 LE
        QByteArray("\x00\x00\xFE\xFF", 4),    // UTF-32 BE
    };
    for (const QByteArray& mark : kMarks)
        if (head == mark)
            return true;
    return false;
}

} // namespace

void LogParser::parseFrom(const LogFilePtr& logFile, qint64 startOffset,
                          const ResumeContext& context, const FileSchema* schema,
                          bool initial,
                          TextEncoding streamEncoding, FileEncoding fileEncoding,
                          TextEncoding fallback)
{
    if (initial)
        emit parsingStarted(logFile);

    if (!logFile || logFile->filePath.isEmpty()) {
        if (initial)
            emit parsingFailed(logFile);
        else
            emit parsingFinished(0, logFile);
        return;
    }

    // Без QIODevice::Text: он выбрасывает из текста КАЖДЫЙ '\r', а не только
    // перед '\n', — текст строки разошёлся бы с индексным путём. "\r\n"
    // снимает сам readLine.
    QFile file(logFile->filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit parsingFailed(logFile);
        return;
    }
    // Транспорт (F-1, docs/design/F-1.md, «Резидентный путь»; Q-18):
    //   • UTF-16/32 (BOM в начале файла; у дозаписи — streamEncoding) —
    //     QTextStream в этой кодировке, как до F-1;
    //   • всё остальное — ByteLineReader: строка приходит сырыми байтами, разрез
    //     строк и снятие '\r' — как у QTextStream::readLine, позиция каждой
    //     строки в файле — счётчик. Строку с байтами ≥ 0x80 декодирует
    //     декодер файла (UTF-8 — fromUtf8, как индексатор строку), ASCII-строка
    //     берётся как есть (fromLatin1).
    // BOM разбирает TextEncodings::detectBom — тот же разбор, что у
    // индексатора: UTF-8 BOM пропускается и решает кодировку (Source::Bom).
    bool byteMode = true;
    bool detectUnicodeBom = false;
    qint64 linesBegin = startOffset; // где начинается первая строка (за BOM)
    if (startOffset == 0) {
        const QByteArray head = file.peek(4);
        if (const auto bom = TextEncodings::detectBom(head)) {
            linesBegin = bom->length; // у UTF-16/32 — ровно одна кодовая единица
            if (bom->encoding == TextEncoding::Utf8) {
                // Под выбором пользователя BOM решает только автоматику
                // рядом, декодер остаётся выбранным (как у индексатора).
                const FileEncoding bomDecision{TextDecoder(TextEncoding::Utf8),
                                               FileEncoding::Source::Bom};
                fileEncoding = fileEncoding.source == FileEncoding::Source::User
                    ? fileEncoding.withAutomatic(bomDecision) : bomDecision;
                emit encodingDecided(logFile, fileEncoding);
            } else {
                // UTF-16/32: QTextStream сам узнаёт BOM и снимает его.
                byteMode = false;
                detectUnicodeBom = true;
            }
        }
    } else if (!TextEncodings::isDecodable(streamEncoding)) {
        // Дозапись UTF-16/32: BOM есть только в начале файла — кодировка из
        // tailState, иначе хвост декодировался бы как UTF-8.
        byteMode = false;
    }
    std::optional<ByteLineReader> bytes;
    std::optional<QTextStream> stream;
    if (byteMode) {
        if (linesBegin > 0 && !file.seek(linesBegin)) {
            emit parsingFinished(0, logFile);
            return;
        }
        bytes.emplace(&file, linesBegin);
    } else {
        stream.emplace(&file);
        QTextStream& in = *stream;
        // Начало чтения — seek САМОГО потока, а не устройства до его создания:
        // иначе QTextStream::pos() посреди потока считает от нуля (его опорная
        // позиция буфера — 0, пока буфер не прочитан целиком; tests/textencoding).
        // У UTF-16/32 с BOM поток стоит в нуле и снимает BOM сам.
        const qint64 readFrom = detectUnicodeBom ? 0 : linesBegin;
        if (readFrom > 0 && !in.seek(readFrom)) {
            emit parsingFinished(0, logFile);
            return;
        }
        in.setAutoDetectUnicode(detectUnicodeBom);
        if (!detectUnicodeBom)
            in.setEncoding(TextEncodings::toQt(streamEncoding).value_or(QStringConverter::Utf8));
    }
    qint64 reportedEvidenceEnd = fileEncoding.evidence.end;
    QVector<std::shared_ptr<LogEntry>> batchEntries;
    const int BATCH_SIZE = 5000;
    batchEntries.reserve(BATCH_SIZE);

    // Состояние продолжается с того места, где кончилась прошлая порция: строка
    // продолжения в начале дозаписи принадлежит последней записи файла, а
    // номера строк идут дальше, а не с 1.
    int logicalEntryIdCounter = context.nextLogicalEntryId;
    int currentLogicalEntryId = context.currentLogicalEntryId;
    std::shared_ptr<const LogEntry> currentHead = context.currentHead;
    QDateTime currentLogicalEntryTimestamp = context.currentTimestamp;
    LogLevel currentLogicalEntryLevel = context.currentLevel;
    int fileLineNumber = context.nextLineNumber - 1;
    int totalParsedEntries = 0;

    const qint64 fileSize = initial ? QFileInfo(logFile->filePath).size() : 0;
    qint64 bytesRead = 0;
    int lastReportedProgress = -1;

    // Год меток без года (syslog) — по mtime файла на момент этого прохода:
    // дозапись после Нового года получает новый год (LineClassifier).
    const LineClassifier::YearReference yearRef =
        LineClassifier::yearReferenceForFile(logFile->filePath);
    // Схема прохода (F-5): nullptr — без схемы. Ключевая (F-4) решает метку,
    // уровень и границу записи, регексная — только границу. Полей записи не
    // хранят (F-17, шаг 3): поля — функция текста и схемы (FieldSource).
    const LogPattern* const passPattern =
        (schema && schema->pattern.isValid()) ? &schema->pattern : nullptr;
    const LogPattern* const keyedSchema =
        (passPattern && passPattern->isKeyed()) ? passPattern : nullptr;
    // Дата времени без даты (F-11): дозапись продолжает состояние прошлого
    // прохода (LogFile::clockState), проход с начала файла — с нуля; первая
    // такая строка при пустом состоянии зовёт предпросмотр от начала прохода.
    ClockDates clock;
    if (!initial)
        clock.state = ClockDateState::fromPacked(logFile->clockState.load());
    // Предпросмотр прерывается вместе с проходом (m_abort): на огромном файле
    // закрытие вкладки не ждёт его конца.
    clock.lookAhead = [&]() {
        if (byteMode)
            return LineClassifier::clockRunInFile(logFile->filePath, linesBegin, std::nullopt,
                                                  false, &m_abort);
        if (detectUnicodeBom)
            return LineClassifier::clockRunInFile(logFile->filePath, 0, QStringConverter::Utf8,
                                                  /*autoDetectBom=*/true, &m_abort);
        return LineClassifier::clockRunInFile(
            logFile->filePath, linesBegin,
            TextEncodings::toQt(streamEncoding).value_or(QStringConverter::Utf8), false, &m_abort);
    };
    // Состояние ДО последней прочитанной строки: оборванную (без '\n')
    // следующий проход перечитает с начала, и её огрызок («13:55:36» от
    // «13:55:36:123») не должен оставить в состоянии лишний день.
    ClockDateState clockBeforeLine = clock.state;

    // Последняя строка байтового транспорта: начало и кончилась ли '\n' —
    // для tailState без второго прохода по файлу.
    qint64 bytesLastStart = -1;
    bool bytesLastNewline = true;
    QString line;
    AnsiStyles lineAnsi;
    for (;;) {
        if (m_abort.load(std::memory_order_relaxed))
            return; // Parser is shutting down — drop the rest.
        // Строка — то, что кончается '\n', плюс хвост без него; пустая
        // последняя строка ("…\n\n") — тоже строка. За завершающим '\n'
        // строки нет (оба транспорта), поэтому пропускать здесь нечего:
        // прежний пропуск пустой строки у конца терял настоящую пустую строку
        // и сдвигал номера всех дописанных за ней — номера разошлись бы с
        // индексным путём, а на номере строки стоит её идентификатор (R-1).
        if (bytes) {
            if (!bytes->next())
                break;
            const QByteArrayView raw = bytes->line();
            bytesLastStart = bytes->lineStart();
            bytesLastNewline = bytes->endsWithNewline();
            // Стиль ANSI (F-3) — только у строки с ESC; текст декодер (и
            // ASCII-ветка) отдаёт уже без кодов.
            const bool escaped = AnsiEscape::hasEscape(raw);
            if (EncodingDetector::firstHighByte(raw.data(), raw.size()) < 0) {
                if (Q_UNLIKELY(escaped)) {
                    const QByteArray stripped = AnsiEscape::strip(raw);
                    line = QString::fromLatin1(stripped.data(), stripped.size());
                } else {
                    line = QString::fromLatin1(raw.data(), raw.size());
                }
            } else {
                // Не-ASCII строка, пока автоматике есть что решать, — порция
                // для EncodingDetector::advance (правило индексатора; первая
                // такая строка «не решённого» файла открывает окно от своего
                // первого байта ≥ 0x80). Строки до неё — ASCII: их текст от
                // решения не зависит. Предварительное решение (шаг 3а) копит
                // свидетельство по каждой следующей не-ASCII строке; строка
                // подаётся с '\n', если кончилась переводом строки — как в
                // буфере индексатора (оборванная последовательность в конце
                // строки — битая, а не «оборвана краем»). Начало строки в
                // файле — счётчик читателя (Q-18: не QTextStream::pos()).
                if (!fileEncoding.detectionSettled()) {
                    QByteArray portion = raw.toByteArray();
                    if (bytes->endsWithNewline())
                        portion.append('\n');
                    if (EncodingDetector::advance(fileEncoding, portion, bytes->lineStart(),
                                                  logFile->filePath, fallback)) {
                        emit encodingDecided(logFile, fileEncoding);
                        reportedEvidenceEnd = fileEncoding.evidence.end;
                    }
                }
                line = fileEncoding.decoder.decode(raw);
            }
            if (Q_UNLIKELY(escaped))
                lineAnsi = AnsiEscape::styles(raw, fileEncoding.decoder);
        } else {
            QTextStream& in = *stream;
            if (in.atEnd())
                break;
            line = in.readLine();
            lineAnsi = AnsiEscape::styles(QStringView(line));
            AnsiEscape::stripInPlace(line);
            // Исключение одно: файл из одного BOM. readLine съедает BOM и
            // отдаёт пустую строку, которой в файле нет.
            if (line.isEmpty() && in.atEnd() && startOffset == 0 && totalParsedEntries == 0
                && isOnlyByteOrderMark(logFile->filePath))
                break;
        }
        // Прогресс: у байтового транспорта — точная позиция, у потока —
        // приблизительно (символы + 1 на перевод строки).
        bytesRead = bytes ? bytes->pos() : bytesRead + line.length() + 1;
        fileLineNumber++;

        // Метка, уровень и граница записи — LineClassifier::classifyLine, то же
        // правило, что у индексатора: ключевая схема (F-4) берёт их из ключей
        // и сразу отдаёт поля, без неё — эвристика по строке. Регексная схема
        // решает только «первичная ли» (и даёт поля).
        clockBeforeLine = clock.state;
        const KeyedClassification cls =
            m_classifier.classifyLine(keyedSchema, line, yearRef, &clock);
        const QDateTime lineTs = cls.hasTs ? QDateTime::fromMSecsSinceEpoch(cls.tsMs) : QDateTime();
        const LogLevel lineLevel = cls.hasLevel ? cls.level : LogLevel::Unknown;
        // Регексная схема решает только «первичная ли» — и только там, где
        // без неё строка первичной не была бы (как у индексатора): строке с
        // ведущей меткой разбор схемой не нужен.
        bool schemaMatched = cls.matched;
        if (!keyedSchema && passPattern
            && !LineClassifier::isPrimaryLine(false, cls.hasTs, cls.leading, cls.hasLevel))
            schemaMatched = !passPattern->extractFields(line).isEmpty();

        std::shared_ptr<LogEntry> currentEntry;
        if (LineClassifier::isPrimaryLine(schemaMatched, cls.hasTs, cls.leading, cls.hasLevel)) {
            currentLogicalEntryId = logicalEntryIdCounter++;
            currentLogicalEntryTimestamp = lineTs;
            currentLogicalEntryLevel = lineLevel;
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentEntry->setSchemaMatched(schemaMatched);
            currentHead = currentEntry;
        } else if (currentLogicalEntryId < 0 || !currentHead) {
            // Ни одной записи ещё нет — строка становится собственной записью
            // (и её первой строкой). Запись без первой строки (дозапись без
            // контекста) — так же.
            if (currentLogicalEntryId < 0) {
                currentLogicalEntryId = logicalEntryIdCounter++;
                currentLogicalEntryTimestamp = QDateTime();
                currentLogicalEntryLevel = LogLevel::Unknown;
            }
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentHead = currentEntry;
        } else {
            currentEntry = std::make_shared<LogEntry>(currentLogicalEntryId, fileLineNumber,
                currentLogicalEntryTimestamp, currentLogicalEntryLevel, line, logFile);
            currentEntry->setRecordHead(currentHead);
        }
        if (!lineAnsi.isEmpty())
            currentEntry->setAnsiStyles(std::move(lineAnsi));
        lineAnsi = AnsiStyles();
        batchEntries.push_back(currentEntry);
        totalParsedEntries++;

        if (batchEntries.size() >= BATCH_SIZE) {
            emit entriesParsed(batchEntries, logFile);
            batchEntries.clear();
            batchEntries.reserve(BATCH_SIZE);
        }

        if (fileSize > 0) {
            const int progress = static_cast<int>(qMin<qint64>(100, (bytesRead * 100) / fileSize));
            if (progress != lastReportedProgress) {
                emit parsingProgress(progress, logFile);
                lastReportedProgress = progress;
            }
        }
    }

    if (!batchEntries.isEmpty())
        emit entriesParsed(batchEntries, logFile);
    if (initial && lastReportedProgress < 100 && fileSize > 0)
        emit parsingProgress(100, logFile);

    // Точный конец прочитанного (дальше дочитывание продолжит отсюда) и не
    // оборвана ли последняя строка: писатель мог не успеть дописать '\n'.
    // Такую строку вкладка показывает предварительно и при дозаписи читает
    // заново с её начала. Всё считается по сырым байтам, в кодовых единицах
    // кодировки: в UTF-16 перевод строки — два байта, а порция может
    // оборваться посреди единицы — такой огрызок не считается прочитанным
    // (единицы выровнены от начала файла: BOM — ровно одна единица).
    bool lastLinePartial = false;
    qint64 lastLineStart = -1;
    qint64 endOffset = -1;
    TextEncoding reported = fileEncoding.decoder.encoding();
    if (bytes) {
        // Байтовый транспорт знает и конец, и последнюю строку сам.
        endOffset = bytes->pos();
        if (totalParsedEntries > 0 && !bytesLastNewline) {
            lastLinePartial = true;
            lastLineStart = bytesLastStart;
        }
    } else {
        const QStringConverter::Encoding encoding = stream->encoding();
        const CodeUnit unit = codeUnitOf(encoding);
        reported = TextEncodings::fromQt(encoding).value_or(TextEncoding::Utf8);
        // Один вызов pos() на проход: буфер потока здесь прочитан целиком, и
        // QTextStream отдаёт позицию устройства без перечитывания.
        endOffset = stream->pos();
        if (endOffset < 0)
            endOffset = file.size();
        endOffset -= endOffset % unit.width;
        // Строки начинаются после BOM: перечитывание с нуля отдало бы его
        // декодеру без автоопределения.
        const qint64 firstLineStart = linesBegin;
        if (totalParsedEntries > 0 && endOffset > firstLineStart) {
            QFile raw(logFile->filePath);
            if (raw.open(QIODevice::ReadOnly) && raw.seek(endOffset - unit.width)
                && afterLastNewline(raw.read(unit.width), unit) < 0) {
                lastLinePartial = true;
                lastLineStart = firstLineStart;
                constexpr qint64 kChunk = 64 * 1024;
                for (qint64 pos = endOffset; pos > firstLineStart;) {
                    const qint64 from = qMax(firstLineStart, pos - kChunk);
                    if (!raw.seek(from))
                        break;
                    const qsizetype after = afterLastNewline(raw.read(pos - from), unit);
                    if (after >= 0) {
                        lastLineStart = from + after;
                        break;
                    }
                    pos = from;
                }
            }
        }
    }
    // Свидетельство предварительного решения подросло без смены решения —
    // отдаём его вкладке: следующий проход продолжит копить с этого места.
    if (bytes && !fileEncoding.detectionSettled() && fileEncoding.evidence.end != reportedEvidenceEnd)
        emit encodingDecided(logFile, fileEncoding);
    logFile->clockState.store((lastLinePartial ? clockBeforeLine : clock.state).packed());
    emit tailState(logFile, endOffset, lastLinePartial, lastLineStart, reported);
    emit parsingFinished(totalParsedEntries, logFile);
}

LogParser::StatsScan LogParser::analyzeFileForStats(const QString& filePath,
                                                    const StatsResume& from,
                                                    const std::atomic<bool>* cancelled,
                                                    const LogPattern* keyedSchema) const
{
    StatsScan scan;
    FileStats& stats = scan.stats;
    stats.fileSize = QFileInfo(filePath).size();

    // Без QIODevice::Text: смещения — сырые байты (инвариант 5), '\r' перед
    // '\n' снимается вручную.
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        stats.parseSuccess = false;
        scan.complete = stats;
        qWarning() << "AnalyzeStats: Failed to open file:" << filePath;
        return scan;
    }

    // Сжатый лог (F-2, PR 3) — по сигнатуре, потоком распакованных байтов,
    // без записи на диск. Продолжения с середины у него нет: поток не
    // перематывается, а дозаписи у архива не бывает — изменившийся файл
    // разбирается заново (next.offset остаётся 0).
    std::optional<GzipDevice> gzip;
    if (GzipDevice::hasSignature(file.peek(2))) {
        gzip.emplace(&file);
        if (from.offset != 0 || !gzip->open(QIODevice::ReadOnly)) {
            stats.parseSuccess = false;
            scan.complete = stats;
            return scan;
        }
    }
    QIODevice& input = gzip ? static_cast<QIODevice&>(*gzip) : file;

    // Кодировка: с начала файла — по BOM, как автоопределение QTextStream у
    // разбора вкладки; хвост — в кодировке, известной по прошлому разбору.
    QStringConverter::Encoding encoding = from.encoding;
    qint64 offset = from.offset;
    QByteArray pending;           // байты начатой, ещё не законченной строки
    if (offset == 0) {
        // Голова читается и остаётся в разборе (не перечитывается seek'ом:
        // поток `.gz` назад не ходит).
        pending = input.read(4);
        const auto detected = QStringConverter::encodingForData(pending);
        if (detected) {
            // Строки начинаются за BOM: в UTF-8 он три байта, в UTF-16/32 —
            // одна кодовая единица.
            encoding = *detected;
            offset = encoding == QStringConverter::Utf8 ? 3 : codeUnitOf(encoding).width;
        } else {
            encoding = QStringConverter::Utf8;
        }
        pending.remove(0, offset);
    }
    const CodeUnit unit = codeUnitOf(encoding);
    if (offset % unit.width != 0 || (from.offset != 0 && !file.seek(offset))) {
        stats.parseSuccess = false;
        scan.complete = stats;
        return scan;
    }

    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    const LineClassifier::YearReference yearRef = LineClassifier::yearReferenceForFile(filePath);
    // Схему задаёт вызывающий (сканер — нет); она живёт до конца разбора.
    if (keyedSchema && !keyedSchema->isKeyed())
        keyedSchema = nullptr;
    bool inRecord = from.inRecord;
    LogLevel recordLevel = LogLevel::Unknown;
    // Время без даты (F-11): предпросмотра нет (`.gz` пришлось бы
    // распаковывать дважды) — при пустом состоянии дни идут от
    // предварительной опоры, а в конце From/To и состояние сдвигаются.
    ClockDates clock;
    clock.state = from.clock;
    const bool provisionalClock = !clock.state.isSet();
    bool firstIsClock = false, lastIsClock = false;

    const auto countLine = [&](const QByteArray& bytes) {
        QString line = decoder.decode(bytes);
        if (line.endsWith(u'\r'))
            line.chop(1);
        // Коды ANSI (F-3) — как у вкладки: классификатор видит текст без них.
        AnsiEscape::stripInPlace(line);
        // То же правило, что у загрузки вкладки (LineClassifier::classifyLine):
        // ключевая схема с извлечением полей — метка, уровень и граница из
        // ключей. Регексная схема здесь не участвует (её цена — регекс на
        // каждую строку); сканер каталога схему не задаёт вовсе — счётчики
        // по эвристике, как у вкладки без схемы.
        const KeyedClassification cls = m_classifier.classifyLine(keyedSchema, line, yearRef, &clock);
        const QDateTime ts = cls.hasTs ? QDateTime::fromMSecsSinceEpoch(cls.tsMs) : QDateTime();
        const LogLevel level = cls.hasLevel ? cls.level : LogLevel::Unknown;
        if (LineClassifier::isPrimaryLine(cls.matched, cls.hasTs, cls.leading, cls.hasLevel)) {
            recordLevel = level;
            if (!stats.firstEntryTimestamp.isValid()) {
                stats.firstEntryTimestamp = ts;
                firstIsClock = clock.matched;
            }
            stats.lastEntryTimestamp = ts;
            lastIsClock = clock.matched;
        } else if (!inRecord) {
            recordLevel = LogLevel::Unknown;   // строка до первой записи — своя запись
        } else {
            return;                            // продолжение текущей записи
        }
        inRecord = true;
        stats.totalEntries++;
        switch (recordLevel) {
        case LogLevel::Warn:  stats.warnCount++;  break;
        case LogLevel::Error: stats.errorCount++; break;
        case LogLevel::Fatal: stats.fatalCount++; break;
        default: break;
        }
    };

    // Поиск перевода строки — по кодовым единицам кодировки.
    const qsizetype low = unit.bigEndian ? unit.width - 1 : 0;
    const auto findNewline = [&](const QByteArray& buf, qsizetype fromPos) -> qsizetype {
        if (unit.width == 1)
            return buf.indexOf('\n', fromPos);
        for (qsizetype at = fromPos; at + unit.width <= buf.size(); at += unit.width) {
            if (buf[at + low] != '\n')
                continue;
            bool newline = true;
            for (qsizetype k = 0; newline && k < unit.width; ++k)
                newline = k == low || buf[at + k] == 0;
            if (newline)
                return at;
        }
        return -1;
    };

    const auto isCancelled = [cancelled] {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    };
    if (gzip)
        gzip->setAbortFlag(cancelled);

    constexpr qint64 kChunk = 1 << 20;
    qint64 lineStart = offset;    // сырое смещение начала pending
    qsizetype searchFrom = 0;     // pending до этого места '\n' уже не содержит
    qint64 dropped = 0;           // > 0: строка длиннее kMaxStatsLineBytes, в pending — её голова
    while (!isCancelled()) {
        // Последний проход — с пустым чтением: голова, прочитанная до цикла,
        // тоже режется на строки.
        const QByteArray chunk = input.read(kChunk);
        if (dropped > 0) {
            // Хвост слишком длинной строки: только найти её конец.
            const qsizetype newline = findNewline(chunk, 0);
            if (newline < 0) {
                dropped += chunk.size();
                if (chunk.isEmpty())
                    break;
                continue;
            }
            countLine(pending);
            lineStart += pending.size() + dropped + newline + unit.width;
            pending = chunk.mid(newline + unit.width);
            dropped = 0;
            searchFrom = 0;
        } else {
            pending += chunk;
        }
        qsizetype pos = 0;
        while (true) {
            const qsizetype newline = findNewline(pending, std::max(pos, searchFrom));
            if (newline < 0)
                break;
            countLine(pending.mid(pos, newline - pos));
            lineStart += newline + unit.width - pos;
            pos = newline + unit.width;
        }
        pending.remove(0, pos);
        // Поиск продолжится с конца прочитанного (по кодовым единицам), а не
        // с начала строки: длинная строка не перечитывается на каждый кусок.
        searchFrom = pending.size() - pending.size() % unit.width;
        // Строка без конца (бинарный файл, `.gz` из одних нулей) не растит
        // буфер без предела: для счёта нужна голова — метка и уровень.
        if (pending.size() > kMaxStatsLineBytes) {
            dropped = pending.size() - kMaxStatsLineBytes;
            pending.truncate(kMaxStatsLineBytes);
        }
        if (chunk.isEmpty())
            break;
    }

    if (isCancelled()) {
        stats.parseSuccess = false;
        scan.complete = stats;
        return scan;
    }

    // Поток `.gz` оборвался или не сошёлся с CRC: цифры не описывают файл
    // («Parse Failed», как у вкладки, которая такой архив не откроет).
    if (gzip && gzip->error() != GzipDevice::Error::None) {
        stats.parseSuccess = false;
        scan.complete = stats;
        return scan;
    }

    // Целые строки кончились: это точка продолжения. Оборванная последняя
    // строка (писатель не дописал '\n') считается в показанных цифрах, но не
    // в базе — при дозаписи она перечитается целиком.
    scan.complete = stats;
    scan.next.offset = gzip ? 0 : lineStart;
    scan.next.encoding = encoding;
    scan.next.inRecord = inRecord;
    scan.next.clock = clock.state;
    const bool completeFirstIsClock = firstIsClock, completeLastIsClock = lastIsClock;
    if (pending.size() >= unit.width)
        countLine(pending.left(pending.size() - pending.size() % unit.width));

    // Предварительные дни времени без даты (F-11) — к настоящим: последняя
    // такая строка (с оборванной: её показывает и вкладка) получает дату по
    // mtime, остальные сдвигаются на ту же разницу.
    if (provisionalClock && clock.state.isSet()) {
        const qint32 shift = LineClassifier::clockLastDay(clock.state.timeMs, yearRef) - clock.state.day;
        const auto moveDays = [shift](QDateTime& ts, bool isClock) {
            if (isClock && ts.isValid())
                ts = ts.addDays(shift);
        };
        moveDays(stats.firstEntryTimestamp, firstIsClock);
        moveDays(stats.lastEntryTimestamp, lastIsClock);
        moveDays(scan.complete.firstEntryTimestamp, completeFirstIsClock);
        moveDays(scan.complete.lastEntryTimestamp, completeLastIsClock);
        if (scan.next.clock.isSet())
            scan.next.clock.day += shift;
    }
    return scan;
}

LogParser::FileStats LogParser::mergeStats(const FileStats& head, const FileStats& tail)
{
    FileStats merged = head;
    merged.fileSize      = tail.fileSize;
    merged.totalEntries += tail.totalEntries;
    merged.warnCount    += tail.warnCount;
    merged.errorCount   += tail.errorCount;
    merged.fatalCount   += tail.fatalCount;
    merged.parseSuccess  = head.parseSuccess && tail.parseSuccess;
    if (!merged.firstEntryTimestamp.isValid())
        merged.firstEntryTimestamp = tail.firstEntryTimestamp;
    if (tail.lastEntryTimestamp.isValid())
        merged.lastEntryTimestamp = tail.lastEntryTimestamp;
    return merged;
}
