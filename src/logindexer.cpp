#include "logindexer.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QByteArrayView>
#include <QtConcurrent>
#include <cstring>

namespace {
constexpr qint64 kReadBufBytes   = 8 * 1024 * 1024; // размер буфера чтения
constexpr qint64 kPublishLines   = 16384;           // публикация порций строк
// int-строки QAbstractItemModel: не индексируем больше, чем влезет в модель.
constexpr qint64 kMaxModelLines  = qint64(INT_MAX) - 16;
} // namespace

LogIndexer::LogIndexer(QObject* parent)
    : QObject(parent)
{
}

LogIndexer::~LogIndexer()
{
    m_abort.store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

void LogIndexer::startIndexing(const LogFilePtr& logFile,
                               const std::shared_ptr<LineIndex>& index,
                               FileEncoding encoding, FileSchemaPtr schema)
{
    // Схема прохода — неизменяемое значение, лямбда держит её живой.
    const TextEncoding fallback = m_fallbackEncoding;
    (void)QtConcurrent::run(&m_pool, [this, logFile, index, schema = std::move(schema),
                                      encoding, fallback]() {
        this->doIndex(logFile, index, schema.get(),
                      /*initial=*/true, /*reindexProvisionalTail=*/false,
                      encoding, fallback);
    });
}

void LogIndexer::startIndexingFrom(const LogFilePtr& logFile,
                                   const std::shared_ptr<LineIndex>& index,
                                   bool reindexProvisionalTail,
                                   FileEncoding encoding, FileSchemaPtr schema)
{
    const TextEncoding fallback = m_fallbackEncoding;
    (void)QtConcurrent::run(&m_pool, [this, logFile, index, schema = std::move(schema),
                                      reindexProvisionalTail, encoding, fallback]() {
        this->doIndex(logFile, index, schema.get(),
                      /*initial=*/false, reindexProvisionalTail, encoding, fallback);
    });
}

void LogIndexer::doIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index,
                         const FileSchema* schema,
                         bool initial, bool reindexProvisionalTail,
                         FileEncoding encoding, TextEncoding fallback)
{
    if (initial)
        emit indexingStarted(logFile);

    if (!logFile || logFile->filePath.isEmpty() || !index) {
        emit indexingFailed(logFile);
        return;
    }

    // Сырые байты: НИКАКОГО QIODevice::Text — смещения должны совпадать с
    // позициями в файле (и с якорями FileChangeDetector).
    QFile file(logFile->filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit indexingFailed(logFile);
        return;
    }
    const qint64 fileSize = file.size();

    // ---- Контекст логической записи для continuation-строк -----------------
    // При дозаписи наследуем из индекса; при переиндексации предварительного
    // хвоста контекст берётся от строки ПЕРЕД ним (сам хвост будет переписан).
    bool     hasCurrent   = false;
    quint32  currentId    = 0;
    LogLevel currentLevel = LogLevel::Unknown;
    qint64   currentTsMs  = kNoTimestampMs;

    if (!initial) {
        const qint64 oldCount = index->lineCount();
        qint64 contextLine = oldCount - 1;
        if (reindexProvisionalTail && index->lastLineProvisional()) {
            contextLine = oldCount - 2;
            index->truncateFrom(oldCount - 1);
        }
        if (contextLine >= 0) {
            hasCurrent = true;
            currentId = index->logicalId(contextLine);
            currentLevel = index->level(contextLine);
            currentTsMs = index->timestampMs(currentId);
        }
    }

    quint32 nextId = index->logicalEntryCount();
    qint64 startOffset = index->lineCount() > 0 ? index->endOffset() : 0;

    // ---- BOM ----------------------------------------------------------------
    if (startOffset == 0 && fileSize >= 2) {
        char head[4] = {0, 0, 0, 0};
        const qint64 got = file.read(head, qMin<qint64>(4, fileSize));
        if (const auto bom = TextEncodings::detectBom(QByteArrayView(head, qMax<qint64>(got, 0)))) {
            if (bom->encoding != TextEncoding::Utf8) {
                // UTF-16/32 — только резидентный путь (QTextStream по BOM).
                const bool utf32 = bom->encoding == TextEncoding::Utf32LE
                                || bom->encoding == TextEncoding::Utf32BE;
                emit needsResidentFallback(
                    logFile,
                    utf32 ? QStringLiteral("UTF-32 BOM")
                          : QStringLiteral("UTF-16 BOM"));
                return;
            }
            startOffset = bom->length; // строки начинаются после UTF-8 BOM
            // Под выбором пользователя BOM решает только автоматику рядом
            // (для «Определить автоматически»), декодер остаётся выбранным.
            const FileEncoding bomDecision{TextDecoder(TextEncoding::Utf8),
                                           FileEncoding::Source::Bom};
            encoding = encoding.source == FileEncoding::Source::User
                ? encoding.withAutomatic(bomDecision) : bomDecision;
            emit encodingDecided(logFile, encoding);
        }
    }

    if (!file.seek(startOffset)) {
        emit indexingFailed(logFile);
        return;
    }
    index->beginAppend(startOffset);

    const qint64 baseLine = index->lineCount(); // строк до этой сессии дозаписи
    qint64 publishedLines = baseLine;
    qint64 pendingLines = baseLine;
    int lastReportedProgress = -1;
    qint64 bytesConsumed = 0;
    bool truncatedByCap = false;

    QByteArray buf;
    buf.resize(int(kReadBufBytes));
    QByteArray carry; // незавершённая строка с прошлого буфера
    QString text;     // переиспользуемый декодированный текст строки

    // Троттлинг публикаций ПО ВРЕМЕНИ: батчи только по числу строк заваливают
    // GUI-очередь быстрее, чем она разгребается (event loop перестаёт дышать).
    // Публикуем не чаще ~5 раз в секунду; force — финал/границы.
    QElapsedTimer sincePublish;
    sincePublish.start();
    const auto publishPending = [&](bool force = false) {
        if (pendingLines == publishedLines)
            return;
        if (!force && sincePublish.elapsed() < 200)
            return;
        index->publish();
        emit indexBatchReady(logFile, publishedLines, pendingLines - publishedLines);
        publishedLines = pendingLines;
        sincePublish.restart();
    };

    // Год меток без года (syslog) — по mtime файла на момент этого прохода,
    // как у LogParser: дозапись после Нового года получает новый год.
    const LineClassifier::YearReference yearRef =
        LineClassifier::yearReferenceForFile(logFile->filePath);
    // Схема прохода (F-5) — как у LogParser: nullptr — без схемы; решает
    // только чтение (граница записи, у ключевой — метка и уровень).
    const LogPattern* const pattern =
        (schema && schema->pattern.isValid()) ? &schema->pattern : nullptr;
    const LogPattern* const keyedSchema = (pattern && pattern->isKeyed()) ? pattern : nullptr;
    // Дата времени без даты (F-11) — как у LogParser: дозапись продолжает
    // состояние прошлого прохода, предпросмотр — от начала этого прохода.
    ClockDates clock;
    if (!initial)
        clock.state = ClockDateState::fromPacked(logFile->clockState.load());
    clock.lookAhead = [&]() {
        return LineClassifier::clockRunInFile(logFile->filePath, startOffset, std::nullopt,
                                              false, &m_abort);
    };

    // Автоопределение кодировки (F-1): буфер, в котором впервые встретился
    // байт ≥ 0x80, решает до разбора своих строк. Строки, разобранные раньше,
    // — ASCII (иначе решение было бы принято на их буфере). Окно — от этого
    // байта: ASCII до него в decide ничего не добавил бы. Предварительное
    // решение (шаг 3а) advance продолжает пересматривать по каждому буферу;
    // пересмотр приходит сигналом до порции, как первое решение (стор
    // перечитывает уже показанные строки новым декодером — reset).
    qint64 reportedEvidenceEnd = encoding.evidence.end;
    const auto decideOn = [&](const char* data, qint64 len, qint64 dataFileOffset) {
        if (EncodingDetector::advance(encoding, QByteArrayView(data, len), dataFileOffset,
                                      logFile->filePath, fallback)) {
            emit encodingDecided(logFile, encoding);
            reportedEvidenceEnd = encoding.evidence.end;
        }
        // false — только ASCII, решение не изменилось либо единственная
        // последовательность оборвана концом файла («не решено» — решит
        // следующий буфер или дозапись).
    };

    // Классификация и запись одной строки (bytes БЕЗ EOL-байт).
    const auto processLine = [&](const char* data, qint64 len, quint8 eol) {
        text = encoding.decoder.decode(data, qsizetype(len));

        // Метка (мс epoch, без QDateTime), уровень и граница записи —
        // LineClassifier::classifyLine, то же правило, что у LogParser:
        // ключевая схема (F-4) берёт их из ключей (поля — по требованию в
        // сторе, здесь не нужны), без неё — эвристика по строке.
        const KeyedClassification cls = m_classifier.classifyLine(keyedSchema, text, yearRef, &clock);
        const bool hasTs = cls.hasTs;
        const qint64 tsMs = cls.tsMs;
        const LogLevel lvl = cls.hasLevel ? cls.level : LogLevel::Unknown;
        // Регексная схема нужна ТОЛЬКО для решения primary/continuation
        // (спаны выбрасываются). Строка, первичная и без схемы (метка с
        // уровнем или ведущая метка), схему не спрашивает — регекс схемы
        // (самая дорогая часть, минуты на десятках миллионов строк) гоняем
        // только там, где он реально решает.
        const bool primaryWithoutSchema =
            LineClassifier::isPrimaryLine(false, cls.hasTs, cls.leading, cls.hasLevel);
        const bool schemaMatched = keyedSchema
            ? cls.matched
            : (!primaryWithoutSchema && pattern)
                ? !pattern->extractFields(text).isEmpty()
                : false;

        LineIndex::LineRecord rec;
        rec.byteLength = quint32(len);
        rec.eolBytes = eol;
        rec.hasOwnTimestamp = hasTs;

        if (primaryWithoutSchema || schemaMatched) {
            currentId = nextId++;
            currentLevel = lvl;
            currentTsMs = hasTs ? tsMs : kNoTimestampMs;
            index->setLogicalTimestamp(currentId, currentTsMs);
            rec.isPrimary = true;
        } else if (!hasCurrent) {
            // Ещё нет ни одной записи — строка становится собственной записью
            // (та же семантика, что у LogParser).
            currentId = nextId++;
            currentLevel = LogLevel::Unknown;
            currentTsMs = kNoTimestampMs;
            index->setLogicalTimestamp(currentId, kNoTimestampMs);
        }
        hasCurrent = true;

        rec.level = currentLevel;
        // Паритет с LogEntry::isPlainText(): унаследованные ts/level пусты и
        // схема эту строку не распознала (у primary со схемой fields непусты).
        rec.isPlainText = !hasTimestampMs(currentTsMs) && currentLevel == LogLevel::Unknown
                       && !schemaMatched;
        rec.logicalId = currentId;
        index->appendLine(rec);
        ++pendingLines;
    };

    // ---- Основной цикл чтения ------------------------------------------------
    while (true) {
        if (m_abort.load(std::memory_order_relaxed))
            return; // приложение закрывается — бросаем всё
        if (pendingLines >= kMaxModelLines) {
            qWarning() << "LogIndexer: line cap reached, truncating load of"
                       << logFile->filePath;
            truncatedByCap = true;
            break;
        }

        const qint64 got = file.read(buf.data(), kReadBufBytes);
        if (got < 0) {
            publishPending(/*force=*/true);
            emit indexingFailed(logFile);
            return;
        }
        if (got == 0)
            break; // EOF

        const char* p = buf.constData();
        // Решённый файл advance не трогает (проверка — там же, а не здесь:
        // правило решения меняется в одном месте для обоих бэкендов).
        decideOn(p, got, file.pos() - got);
        qint64 pos = 0;
        while (pos < got) {
            if (m_abort.load(std::memory_order_relaxed))
                return;
            const char* nl = static_cast<const char*>(
                memchr(p + pos, '\n', size_t(got - pos)));
            if (!nl) {
                carry.append(p + pos, int(got - pos));
                break;
            }
            const qint64 nlPos = nl - p;
            if (carry.isEmpty()) {
                qint64 len = nlPos - pos;
                quint8 eol = 1;
                if (len > 0 && p[pos + len - 1] == '\r') { --len; eol = 2; }
                processLine(p + pos, len, eol);
            } else {
                carry.append(p + pos, int(nlPos - pos));
                qint64 len = carry.size();
                quint8 eol = 1;
                if (len > 0 && carry.at(int(len - 1)) == '\r') { --len; eol = 2; }
                processLine(carry.constData(), len, eol);
                carry.clear();
            }
            pos = nlPos + 1;

            if (pendingLines - publishedLines >= kPublishLines)
                publishPending();
            if (pendingLines >= kMaxModelLines)
                break;
        }

        bytesConsumed = file.pos() - startOffset - carry.size();
        if (initial && fileSize > startOffset) {
            const int progress = int((bytesConsumed * 100) / (fileSize - startOffset));
            if (progress != lastReportedProgress) {
                emit indexingProgress(progress, logFile);
                lastReportedProgress = progress;
            }
        }
    }

    // Хвост без '\n' — «предварительная» строка: переиндексируется при
    // следующей дозаписи (детерминированная замена resident-хака с пропуском
    // пустых строк).
    // Одиночный '\r' в конце хвоста — не текст, как у резидентного пути
    // (readLine снимает его) и как станет, когда допишут '\n'.
    // Состояние дат (F-11) — до предварительной строки: дозапись перечитает её
    // с начала, и огрызок времени не должен оставить в состоянии лишний день.
    ClockDateState clockToKeep = clock.state;
    if (!carry.isEmpty() && !truncatedByCap) {
        if (carry.endsWith('\r'))
            processLine(carry.constData(), carry.size() - 1, LineIndex::kEolProvisionalCr);
        else
            processLine(carry.constData(), carry.size(), /*eol=*/0);
        carry.clear();
    }

    // Свидетельство предварительного решения подросло без смены решения —
    // отдаём его стору: следующий проход (дозапись, переход порога)
    // продолжит копить с этого места, а не с прошлого.
    if (!encoding.detectionSettled() && encoding.evidence.end != reportedEvidenceEnd)
        emit encodingDecided(logFile, encoding);
    logFile->clockState.store(clockToKeep.packed());
    publishPending(/*force=*/true);
    if (initial && lastReportedProgress < 100)
        emit indexingProgress(100, logFile);
    emit indexingFinished(pendingLines - baseLine, logFile);
}
