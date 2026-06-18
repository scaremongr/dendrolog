// logviewwidget.cpp
#include "logviewwidget.h"
#include "appsettings.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>


// Comparator for merging/sorting LogEntry batches. Nulls sort first.
static bool compareLogEntries(const std::shared_ptr<LogEntry>& a,
                               const std::shared_ptr<LogEntry>& b)
{
    if (!a) return true;
    if (!b) return false;
    return *a < *b;
}

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
    if (m_model)
        m_model->setAvailableFields(m_logParser->pattern().fieldNames());
}

void LogViewWidget::setExtractionEnabled(bool enabled)
{
    m_logParser->setExtractionEnabled(enabled);
    m_reloadParser->setExtractionEnabled(enabled);
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
    
    // Запускаем асинхронный парсинг
    m_logParser->startParsing(logFile);
    // UI может показать сообщение "Загрузка файла..." или индикатор прогресса
    qDebug() << "LogViewWidget: Started parsing for" << filePath;
}

void LogViewWidget::handleEntriesParsed(
    const QVector<std::shared_ptr<LogEntry>>& entriesBatch,
    const LogFilePtr& parsedLogFile)
{
    if (entriesBatch.isEmpty()) return;

    // Sort the batch so it can be merged into the already-sorted model entries.
    QVector<std::shared_ptr<LogEntry>> sortedBatch(entriesBatch.begin(), entriesBatch.end());
    std::sort(sortedBatch.begin(), sortedBatch.end(), compareLogEntries);

    // Track the highest logical ID seen (bookkeeping for incremental reload).
    if (parsedLogFile) {
        int& nextId = m_fileReloadStates[parsedLogFile->filePath].nextLogicalEntryId;
        for (const auto& e : sortedBatch)
            if (e) nextId = qMax(nextId, e->logicalEntryId + 1);
    }

    // Merge into the model's current entries, preserving global sort order.
    const auto& current = m_model->allEntries();
    QVector<std::shared_ptr<LogEntry>> merged;
    merged.reserve(current.size() + sortedBatch.size());
    std::merge(current.begin(), current.end(),
               sortedBatch.constBegin(), sortedBatch.constEnd(),
               std::back_inserter(merged), compareLogEntries);

    m_model->setEntries(merged);
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
    bool anyChanged = false;
    for (const auto& logFile : m_loadedFiles) {
        if (!logFile) continue;

        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        if (!st.initialLoadDone) continue; // Still doing the initial parse

        switch (FileChangeDetector::classify(logFile->filePath, st.anchor)) {
        case FileChangeDetector::Change::Unchanged:
            break;

        case FileChangeDetector::Change::Appended: {
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
            st.nextLogicalEntryId = qMax(st.nextLogicalEntryId, e->logicalEntryId + 1);
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


