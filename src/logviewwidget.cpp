// logviewwidget.cpp
#include "logviewwidget.h"
#include "appsettings.h"
#include "indexedlogstore.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>



LogViewWidget::LogViewWidget(QWidget *parent)
    : QWidget(parent)
    , m_view(new LogListView(this))
    , m_model(new LogModel(this))
    , m_logParser(new LogParser(this))
    , m_reloadParser(new LogParser(this))
    , m_autoReload(AppSettings::instance().autoReload())
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_view->setModel(m_model);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_view->setMinimumSize(0, 0);
    m_view->setWordWrap(true);
    layout->addWidget(m_view);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);

    // Соединяем сигналы парсера со слотами (начальная загрузка)
    connect(m_logParser, &LogParser::entriesParsed,   this, &LogViewWidget::handleEntriesParsed);
    connect(m_logParser, &LogParser::parsingStarted,  this, [this](const LogFilePtr& f){ emit fileParsingStarted(f); });
    connect(m_logParser, &LogParser::parsingProgress, this, [this](int p, const LogFilePtr& f){ emit fileParsingProgress(f, p); });
    connect(m_logParser, &LogParser::parsingFinished, this, &LogViewWidget::handleParsingFinished);
    connect(m_logParser, &LogParser::parsingFailed,   this, [this](const LogFilePtr& f){ emit fileParsingFailed(f); });

    // Соединяем сигналы reload-парсера (только для инкрементальных обновлений)
    connect(m_reloadParser, &LogParser::entriesParsed, this, &LogViewWidget::handleIncrementalEntriesParsed);
    connect(m_reloadParser, &LogParser::parsingFinished, this, &LogViewWidget::handleIncrementalParsingFinished);

    // Соединяем сигнал изменения текущей строки
    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex &current, const QModelIndex &previous){
        Q_UNUSED(previous);
        int currentRow = current.isValid() ? current.row() : -1;
        int totalRows = m_model->rowCount();
        emit currentRowChanged(currentRow, totalRows);
    });

    // Пробрасываем сигнал фильтрации модели
    connect(m_model, &LogModel::modelFiltered, this, &LogViewWidget::handleModelFilteredRelay);

    // Живое применение бюджета кэша текста индексного бэкенда из настроек.
    connect(&AppSettings::instance(), &AppSettings::settingsChanged, this, [this]() {
        if (auto* store = m_model->indexedOrNull())
            store->setTextCacheBudget(
                qint64(AppSettings::instance().textCacheBudgetMB()) * 1024 * 1024);
    });
}

LogViewWidget::~LogViewWidget()
{
    // m_logParser будет удален автоматически, т.к. его parent - this (LogViewWidget)
    // Если бы он не был дочерним QObject, нужно было бы delete m_logParser;
}

void LogViewWidget::setParserPattern(const QString& pattern)
{
    m_logParser->setPattern(pattern);
    m_reloadParser->setPattern(pattern);
    if (m_indexer)
        m_indexer->setPattern(pattern);
    if (auto* store = m_model ? m_model->indexedOrNull() : nullptr)
        store->setFieldPattern(pattern, m_extractionEnabled);
    if (m_model)
        m_model->setAvailableFields(m_logParser->pattern().fieldNames());
}

void LogViewWidget::setExtractionEnabled(bool enabled)
{
    m_extractionEnabled = enabled;
    m_logParser->setExtractionEnabled(enabled);
    m_reloadParser->setExtractionEnabled(enabled);
    if (m_indexer)
        m_indexer->setExtractionEnabled(enabled);
    if (auto* store = m_model ? m_model->indexedOrNull() : nullptr)
        store->setFieldPattern(m_logParser->pattern().patternString(), enabled);
}

QString LogViewWidget::parserPattern() const
{
    return m_logParser->pattern().patternString();
}

QStringList LogViewWidget::parserFieldNames() const
{
    return m_logParser->pattern().fieldNames();
}

void LogViewWidget::addLogFile(const QString &filePath)
{
    for (const auto &file : m_loadedFiles) {
        if (file->filePath == filePath)
            return; // Файл уже загружен или в процессе загрузки
    }

    auto logFile = std::make_shared<LogFile>(filePath);
    m_loadedFiles.append(logFile);

    // Выбор бэкенда: большие файлы идут через индекс (текст остаётся на
    // диске); один бэкенд на вкладку — большой файл в резидентной вкладке
    // конвертирует её целиком (все файлы перечитываются через индексатор).
    const qint64 size = QFileInfo(filePath).size();
    const bool wantIndexed = m_model->isIndexedBackend()
        || size >= AppSettings::instance().indexedThresholdBytes();

    if (wantIndexed) {
        if (!m_model->isIndexedBackend()) {
            auto* store = m_model->convertToIndexedBackend();
            store->setTextCacheBudget(
                qint64(AppSettings::instance().textCacheBudgetMB()) * 1024 * 1024);
            m_fileReloadStates.clear();
            // Вкладка могла уже держать резидентные файлы — перечитываем все.
            for (const auto& lf : m_loadedFiles)
                startIndexedLoad(lf);
            qDebug() << "LogViewWidget: Indexed backend enabled for tab,"
                     << m_loadedFiles.size() << "file(s)";
            return;
        }
        startIndexedLoad(logFile);
        qDebug() << "LogViewWidget: Started indexing for" << filePath;
        return;
    }

    // Запускаем асинхронный парсинг
    m_logParser->startParsing(logFile);
    // UI может показать сообщение "Загрузка файла..." или индикатор прогресса
    qDebug() << "LogViewWidget: Started parsing for" << filePath;
}

void LogViewWidget::ensureIndexer()
{
    if (m_indexer)
        return;
    m_indexer = new LogIndexer(this);
    m_indexer->setPattern(m_logParser->pattern().patternString());
    m_indexer->setExtractionEnabled(m_extractionEnabled);
    connect(m_indexer, &LogIndexer::indexingStarted, this,
            [this](const LogFilePtr& f) { emit fileParsingStarted(f); });
    connect(m_indexer, &LogIndexer::indexingProgress, this,
            [this](int p, const LogFilePtr& f) { emit fileParsingProgress(f, p); });
    connect(m_indexer, &LogIndexer::indexBatchReady,
            this, &LogViewWidget::handleIndexBatchReady);
    connect(m_indexer, &LogIndexer::indexingFinished,
            this, &LogViewWidget::handleIndexingFinished);
    connect(m_indexer, &LogIndexer::indexingFailed,
            this, &LogViewWidget::handleIndexingFailed);
    connect(m_indexer, &LogIndexer::needsResidentFallback,
            this, &LogViewWidget::handleResidentFallback);
}

void LogViewWidget::startIndexedLoad(const LogFilePtr& logFile)
{
    ensureIndexer();
    auto* store = m_model->indexedOrNull();
    if (!store || !logFile)
        return;
    store->setFieldPattern(m_logParser->pattern().patternString(), m_extractionEnabled);
    auto index = std::make_shared<LineIndex>();
    store->attachFile(logFile, index);
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.initialLoadDone = false;
    st.indexingInFlight = true;
    m_indexer->startIndexing(logFile, index);
}

void LogViewWidget::handleIndexBatchReady(const LogFilePtr& logFile,
                                          qint64 firstLine, qint64 count)
{
    auto* store = m_model->indexedOrNull();
    if (!store || !logFile)
        return;
    // Порции первичной индексации — обычные строки; порции дозаписи (после
    // initialLoadDone) помечаются «новыми» (зелёный маркер гаттера).
    const bool tailAppend =
        m_fileReloadStates.value(logFile->filePath).initialLoadDone;
    store->appendIndexedRows(logFile, firstLine, count, tailAppend);
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleIndexingFinished(qint64 newLines, const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    auto* store = m_model->indexedOrNull();
    const auto index = store ? store->indexForFile(logFile->filePath) : nullptr;

    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    const bool wasInitial = !st.initialLoadDone;
    // Якорь — по концу проиндексированных байт (включая предварительный
    // хвост): следующая дозапись начнётся с index->endOffset().
    const qint64 anchorOffset = index ? index->endOffset()
                                      : QFileInfo(logFile->filePath).size();
    st.anchor = FileChangeDetector::capture(logFile->filePath, anchorOffset);
    st.initialLoadDone = true;
    st.indexingInFlight = false;

    if (wasInitial)
        emit fileParsingFinished(logFile,
                                 int(qMin<qint64>(newLines, INT_MAX)));
    else
        emit reloadFinished(int(qMin<qint64>(newLines, INT_MAX)));
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleIndexingFailed(const LogFilePtr& logFile)
{
    if (logFile)
        m_fileReloadStates[logFile->filePath].indexingInFlight = false;
    emit fileParsingFailed(logFile);
}

void LogViewWidget::handleResidentFallback(const LogFilePtr& logFile,
                                           const QString& reason)
{
    if (!logFile)
        return;
    qWarning() << "LogViewWidget: indexed backend fallback for"
               << logFile->filePath << "-" << reason;
    m_fileReloadStates[logFile->filePath].indexingInFlight = false;

    // Индексный путь понимает только UTF-8: файл ведём через резидентный
    // парсер. Смешивать бэкенды в одной вкладке нельзя — откат возможен,
    // только пока этот файл в ней единственный.
    if (m_loadedFiles.size() == 1) {
        m_model->convertToResidentBackend();
        m_fileReloadStates.remove(logFile->filePath);
        m_logParser->startParsing(logFile);
        return;
    }
    emit fileParsingFailed(logFile);
}

void LogViewWidget::handleEntriesParsed(
    const QVector<std::shared_ptr<LogEntry>>& entriesBatch,
    const LogFilePtr& parsedLogFile)
{
    if (entriesBatch.isEmpty()) return;

    // Sort the batch so it can be merged into the already-sorted model entries.
    QVector<std::shared_ptr<LogEntry>> sortedBatch(entriesBatch.begin(), entriesBatch.end());
    std::sort(sortedBatch.begin(), sortedBatch.end(), logEntryPtrLess);

    // Track the highest logical ID seen (bookkeeping for incremental reload).
    if (parsedLogFile) {
        int& nextId = m_fileReloadStates[parsedLogFile->filePath].nextLogicalEntryId;
        for (const auto& e : sortedBatch)
            if (e) nextId = qMax(nextId, e->logicalEntryId() + 1);
    }

    // Слияние в модель без reset: выделение и позиция скролла сохраняются,
    // стоимость батча O(B) при загрузке одного файла (append в конец) вместо
    // прежней полной пересборки O(N) на каждый батч.
    m_model->mergeEntries(sortedBatch);
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleParsingFinished(int totalEntries, const LogFilePtr& parsedLogFile)
{
    // Anchor the consumed prefix at the end of the initial load. nextLogicalEntryId
    // was already updated incrementally in handleEntriesParsed — no O(N) scan needed.
    if (parsedLogFile) {
        FileReloadState& st = m_fileReloadStates[parsedLogFile->filePath];
        const qint64 size  = QFileInfo(parsedLogFile->filePath).size();
        st.anchor          = FileChangeDetector::capture(parsedLogFile->filePath, size);
        st.initialLoadDone = true;
    }

    emit fileParsingFinished(parsedLogFile, totalEntries);
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleParsingFailed(const LogFilePtr& parsedLogFile)
{
    qWarning() << "LogViewWidget: Failed parsing for" << parsedLogFile->filePath;
    // Удалить parsedLogFile из m_loadedFiles, если он там есть и парсинг критичен
    // m_loadedFiles.removeAll(parsedLogFile); // Если нужно
    // Показать сообщение пользователю
}

void LogViewWidget::handleModelFilteredRelay(int totalRowsAfterFilter)
{
    emit modelFiltered(totalRowsAfterFilter);
}

bool LogViewWidget::reloadChangedFiles()
{
    // Растущий файл (типично — спул stdin) пересёк порог индексного бэкенда:
    // конвертируем вкладку, дальше хвост дочитывает индексатор. Возможные
    // хвостовые батчи резидентного парсера в полёте модель молча отбросит.
    if (!m_model->isIndexedBackend() && m_loadedFiles.size() == 1
        && m_loadedFiles[0]) {
        const qint64 size = QFileInfo(m_loadedFiles[0]->filePath).size();
        if (size >= AppSettings::instance().indexedThresholdBytes()) {
            auto* store = m_model->convertToIndexedBackend();
            store->setTextCacheBudget(
                qint64(AppSettings::instance().textCacheBudgetMB()) * 1024 * 1024);
            m_fileReloadStates.clear();
            startIndexedLoad(m_loadedFiles[0]);
            return true;
        }
    }

    bool anyChanged = false;
    IndexedLogStore* indexedStore = m_model->indexedOrNull();

    for (const auto& logFile : m_loadedFiles) {
        if (!logFile) continue;

        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        if (!st.initialLoadDone) continue; // Still doing the initial parse
        // Индекс — single-writer: пока предыдущая (до)индексация не
        // завершилась, вторую по тому же файлу не запускаем.
        if (indexedStore && st.indexingInFlight) continue;

        switch (FileChangeDetector::classify(logFile->filePath, st.anchor)) {
        case FileChangeDetector::Change::Unchanged:
            break;

        case FileChangeDetector::Change::Appended: {
            if (indexedStore) {
                // Дозапись через индексатор: смещение он берёт сам из
                // index->endOffset(); предварительный хвост без '\n'
                // переиндексируется от своего начала.
                if (auto index = indexedStore->indexForFile(logFile->filePath)) {
                    ensureIndexer();
                    st.indexingInFlight = true;
                    m_indexer->startIndexingFrom(logFile, index,
                                                 index->lastLineProvisional());
                    // Re-anchor: следующий тик поллинга не должен снова
                    // классифицировать этот же диапазон как дозапись.
                    const qint64 newSize = QFileInfo(logFile->filePath).size();
                    st.anchor = FileChangeDetector::capture(logFile->filePath, newSize);
                    anyChanged = true;
                }
                break;
            }
            // Prefix is intact and the file grew — read only the new tail and
            // append it, preserving selection and scroll position.
            m_reloadParser->setPattern(m_logParser->pattern().patternString());
            m_reloadParser->startParsingFrom(logFile, st.anchor.consumedBytes, st.nextLogicalEntryId);
            // Re-anchor immediately so a concurrent poll won't re-read this range.
            const qint64 newSize = QFileInfo(logFile->filePath).size();
            st.anchor = FileChangeDetector::capture(logFile->filePath, newSize);
            anyChanged = true;
            break;
        }

        case FileChangeDetector::Change::Replaced: {
            if (indexedStore) {
                // Файл заменён: сброс его строк и индексация заново в свежий
                // LineIndex (store сам чистит кэш текста и refs).
                ensureIndexer();
                auto fresh = std::make_shared<LineIndex>();
                indexedStore->resetFileIndex(logFile, fresh);
                st = FileReloadState{};
                st.indexingInFlight = true;
                m_indexer->startIndexing(logFile, fresh);
                anyChanged = true;
                break;
            }
            // File was truncated or overwritten with different content. Discard the
            // now-stale entries for this file and re-parse it from scratch; state is
            // rebuilt by handleParsingFinished. Other files' entries are untouched.
            m_model->removeEntriesForFile(logFile->filePath);
            m_fileReloadStates.remove(logFile->filePath); // invalidates `st`; do not reuse
            m_logParser->startParsing(logFile);
            anyChanged = true;
            break;
        }
        }
    }
    return anyChanged;
}

void LogViewWidget::handleIncrementalEntriesParsed(
    const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr& logFile)
{
    if (batch.isEmpty() || !logFile) return;

    // Update the next logical entry ID for this file.
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    for (const auto& e : batch) {
        if (e)
            st.nextLogicalEntryId = qMax(st.nextLogicalEntryId, e->logicalEntryId() + 1);
    }

    // Append entries directly — no model reset, selection preserved.
    m_model->appendEntries(batch);
}

void LogViewWidget::handleIncrementalParsingFinished(int newEntries, const LogFilePtr& /*logFile*/)
{
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
    emit reloadFinished(newEntries);
}

void LogViewWidget::handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile)
{
    // qDebug() << "LogViewWidget: Parsing progress for" << parsedLogFile->filePath << ":" << progressPercentage << "%";
}


void LogViewWidget::searchTextNext(const QString& term, bool caseSensitive)
{
    if (!m_model || term.isEmpty()) {
        return;
    }

    int currentRow = -1;
    if (m_view->selectionModel() && m_view->selectionModel()->currentIndex().isValid()) {
        currentRow = m_view->selectionModel()->currentIndex().row();
    }

    Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QModelIndex foundIndex = m_model->findNextOccurrence(term, currentRow, cs);

    if (foundIndex.isValid()) {
        m_view->setCurrentIndex(foundIndex);
        // Раскрыть найденную строку и подсветить в ней вхождения term
        // ДО scrollTo: раскрытие меняет высоту строки, и прокрутка должна
        // целиться уже в финальную геометрию.
        m_view->showSearchMatch(foundIndex.row(), term, caseSensitive);
        m_view->scrollTo(foundIndex, QAbstractItemView::PositionAtCenter);
    } else {
        m_view->clearSearchMatch();
    }
}

void LogViewWidget::searchTextPrevious(const QString& term, bool caseSensitive)
{
    if (!m_model || term.isEmpty()) {
        return;
    }

    int currentRow = m_model->rowCount(); // Start from end if no selection for previous
    if (m_view->selectionModel() && m_view->selectionModel()->currentIndex().isValid()) {
        currentRow = m_view->selectionModel()->currentIndex().row();
    }
    if (currentRow == -1) { // If view is empty or no selection, effectively start from end
        currentRow = m_model->rowCount();
    }

    Qt::CaseSensitivity cs = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QModelIndex foundIndex = m_model->findPreviousOccurrence(term, currentRow, cs);

    if (foundIndex.isValid()) {
        m_view->setCurrentIndex(foundIndex);
        m_view->showSearchMatch(foundIndex.row(), term, caseSensitive);
        m_view->scrollTo(foundIndex, QAbstractItemView::PositionAtCenter);
    } else {
        m_view->clearSearchMatch();
    }
}


