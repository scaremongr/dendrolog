// logviewwidget.cpp
#include "logviewwidget.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDebug>
#include <algorithm>


LogViewWidget::LogViewWidget(QWidget *parent)
    : QWidget(parent)
    , m_view(new LogListView(this))
    , m_model(new LogModel(this))
    , m_logParser(new LogParser(this))
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    m_view->setModel(m_model);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_view->setMinimumSize(0, 0);
    layout->addWidget(m_view);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);

    // Соединяем сигналы парсера со слотами
    connect(m_logParser, &LogParser::entriesParsed, this, &LogViewWidget::handleEntriesParsed);
    connect(m_logParser, &LogParser::parsingStarted, this, [this](const LogFilePtr& logFile){ emit fileParsingStarted(logFile); });
    connect(m_logParser, &LogParser::parsingProgress, this, [this](int progress, const LogFilePtr& logFile){ emit fileParsingProgress(logFile, progress); });
    connect(m_logParser, &LogParser::parsingFinished, this, [this](int totalEntries, const LogFilePtr& logFile){ emit fileParsingFinished(logFile, totalEntries); });
    connect(m_logParser, &LogParser::parsingFailed, this, [this](const LogFilePtr& logFile){ emit fileParsingFailed(logFile); });

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

void LogViewWidget::handleEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& parsedLogFile)
{
    qDebug() << "LogViewWidget: Received" << entriesBatch.size() << "entries for" << parsedLogFile->filePath;
    if (entriesBatch.isEmpty()) return;

    // Получаем текущие записи из модели
    QVector<std::shared_ptr<LogEntry>> currentEntries = m_model->allEntries();
    
    // Создаем компаратор (можно вынести в утилиты или сделать членом, если используется часто)
    auto compareLogEntries = [](const std::shared_ptr<LogEntry>& a, const std::shared_ptr<LogEntry>& b) {
        if (!a) return true;
        if (!b) return false;
        return *a < *b;
    };

    // Записи в entriesBatch уже отсортированы по originalLineNumber внутри своего файла.
    // currentEntries уже глобально отсортированы.
    // Нам нужно вставить batch в currentEntries, сохраняя общий порядок.

    QVector<std::shared_ptr<LogEntry>> newMergedEntries;
    newMergedEntries.reserve(currentEntries.size() + entriesBatch.size());

    // Простое добавление и полная пересортировка - менее эффективно для частых батчей, но проще в реализации.
    // currentEntries.append(entriesBatch);
    // std::sort(currentEntries.begin(), currentEntries.end(), compareLogEntries);
    // m_model->setEntries(currentEntries);

    // Более эффективное слияние, если currentEntries и entriesBatch отсортированы согласно compareLogEntries
    // entriesBatch приходит отсортированным по файлу, но для глобального merge его нужно отсортировать
    // Однако, т.к. operator< в LogEntry сравнивает сначала timestamp, потом sourceFile, потом line, 
    // то если batch от одного файла, он уже "частично" отсортирован для merge.
    // Для корректного std::merge оба диапазона должны быть отсортированы по одному и тому же критерию.
    // currentEntries УЖЕ отсортированы глобально.
    // entriesBatch (от одного файла) также внутренне отсортирован (по времени/строке). 
    // Если новый файл имеет времена, пересекающиеся с существующими, std::merge - правильный путь.
    
    // Копируем batch, чтобы его можно было отсортировать, если это необходимо для гарантии.
    // Однако, т.к. LogEntry::operator< есть, и он используется для сортировки, 
    // и batch приходит последовательно из файла, он уже будет совместим для std::merge
    // с глобально отсортированным currentEntries.

    std::merge(currentEntries.begin(), currentEntries.end(),
               entriesBatch.constBegin(), entriesBatch.constEnd(), // Используем constBegin/End для const& batch
               std::back_inserter(newMergedEntries),
               compareLogEntries);
    
    m_model->setEntries(newMergedEntries);
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex currentIndex = m_view->currentIndex();
    emit currentRowChanged(currentIndex.isValid() ? currentIndex.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleParsingFinished(int totalEntries, const LogFilePtr& parsedLogFile)
{
    qDebug() << "LogViewWidget: Finished parsing for" << parsedLogFile->filePath << ", total entries:" << totalEntries;
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex currentIndexAfterFinish = m_view->currentIndex();
    emit currentRowChanged(currentIndexAfterFinish.isValid() ? currentIndexAfterFinish.row() : -1, m_model->rowCount());
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

void LogViewWidget::handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile)
{
    // qDebug() << "LogViewWidget: Parsing progress for" << parsedLogFile->filePath << ":" << progressPercentage << "%";
    // Обновить UI для отображения прогресса (например, QProgressBar для конкретного файла или общий)
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
        m_view->scrollTo(foundIndex, QAbstractItemView::PositionAtCenter);
    } else {
        // Optional: Indicate not found (e.g., status bar message or beep)
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
        m_view->scrollTo(foundIndex, QAbstractItemView::PositionAtCenter);
    } else {
        // Optional: Indicate not found
    }
}


