#include "logindexer.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
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

void LogIndexer::setPattern(const QString& schemaString)
{
    m_pattern.setPattern(schemaString);
}

void LogIndexer::startIndexing(const LogFilePtr& logFile,
                               const std::shared_ptr<LineIndex>& index)
{
    // Снапшот схемы на GUI-потоке — воркер не читает мутируемые члены.
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, index, patternSnapshot, extraction]() {
        this->doIndex(logFile, index, patternSnapshot, extraction,
                      /*initial=*/true, /*reindexProvisionalTail=*/false);
    });
}

void LogIndexer::startIndexingFrom(const LogFilePtr& logFile,
                                   const std::shared_ptr<LineIndex>& index,
                                   bool reindexProvisionalTail)
{
    const LogPattern patternSnapshot = m_pattern;
    const bool extraction = m_extractionEnabled;
    (void)QtConcurrent::run(&m_pool, [this, logFile, index, patternSnapshot,
                                      extraction, reindexProvisionalTail]() {
        this->doIndex(logFile, index, patternSnapshot, extraction,
                      /*initial=*/false, reindexProvisionalTail);
    });
}

void LogIndexer::doIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index,
                         const LogPattern& pattern, bool extraction,
                         bool initial, bool reindexProvisionalTail)
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
    qint64   currentTsMs  = -1;

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
        char bom[4] = {0, 0, 0, 0};
        const qint64 got = file.read(bom, qMin<qint64>(4, fileSize));
        const auto b = [&](int i) { return quint8(bom[i]); };
        if (got >= 2) {
            const bool utf16 = (b(0) == 0xFF && b(1) == 0xFE && !(got >= 4 && b(2) == 0 && b(3) == 0))
                            || (b(0) == 0xFE && b(1) == 0xFF);
            const bool utf32 = (got >= 4)
                            && ((b(0) == 0xFF && b(1) == 0xFE && b(2) == 0 && b(3) == 0)
                                || (b(0) == 0 && b(1) == 0 && b(2) == 0xFE && b(3) == 0xFF));
            if (utf16 || utf32) {
                emit needsResidentFallback(
                    logFile,
                    utf32 ? QStringLiteral("UTF-32 BOM")
                          : QStringLiteral("UTF-16 BOM"));
                return;
            }
            if (got >= 3 && b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF)
                startOffset = 3; // строки начинаются после UTF-8 BOM
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

    // Классификация и запись одной строки (bytes БЕЗ EOL-байт).
    const auto processLine = [&](const char* data, qint64 len, quint8 eol) {
        text = QString::fromUtf8(data, int(len));

        qint64 tsMs = -1;
        LogLevel lvl = LogLevel::Unknown;
        // detectTimestampMs: мс epoch без построения QDateTime на строку.
        const bool hasTs = m_classifier.detectTimestampMs(text, tsMs);
        const bool hasLvl = m_classifier.detectLogLevel(text, lvl);
        // extractFields здесь нужен ТОЛЬКО для решения primary/continuation
        // (спаны выбрасываются). Строка с таймстампом и уровнем первична и
        // без схемы — регекс схемы (самая дорогая часть, минуты на десятках
        // миллионов строк) гоняем только там, где он реально решает.
        const bool schemaMatched = (!(hasTs && hasLvl) && extraction && pattern.isValid())
            ? !pattern.extractFields(text).isEmpty()
            : false;

        LineIndex::LineRecord rec;
        rec.byteLength = quint32(len);
        rec.eolBytes = eol;
        rec.hasOwnTimestamp = hasTs;

        if (LineClassifier::isPrimaryLine(schemaMatched, hasTs, hasLvl)) {
            currentId = nextId++;
            currentLevel = lvl;
            currentTsMs = hasTs ? tsMs : -1;
            index->setLogicalTimestamp(currentId, currentTsMs);
            rec.isPrimary = true;
        } else if (!hasCurrent) {
            // Ещё нет ни одной записи — строка становится собственной записью
            // (та же семантика, что у LogParser).
            currentId = nextId++;
            currentLevel = LogLevel::Unknown;
            currentTsMs = -1;
            index->setLogicalTimestamp(currentId, -1);
        }
        hasCurrent = true;

        rec.level = currentLevel;
        // Паритет с LogEntry::isPlainText(): унаследованные ts/level пусты и
        // схема эту строку не распознала (у primary со схемой fields непусты).
        rec.isPlainText = (currentTsMs < 0) && currentLevel == LogLevel::Unknown
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
    if (!carry.isEmpty() && !truncatedByCap) {
        processLine(carry.constData(), carry.size(), /*eol=*/0);
        carry.clear();
    }

    publishPending(/*force=*/true);
    if (initial && lastReportedProgress < 100)
        emit indexingProgress(100, logFile);
    emit indexingFinished(pendingLines - baseLine, logFile);
}
