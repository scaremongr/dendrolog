// logviewwidget.cpp
#include "logviewwidget.h"
#include "commandspooler.h"
#include "commandstatusbar.h"
#include "spooldir.h"
#include "fulllogcontroller.h"
#include "appsettings.h"
#include "filememorystore.h"
#include "indexedlogstore.h"
#include "archiveregistry.h"
#include "gzipdevice.h"
#include "tararchive.h"
#include "sourceunpacker.h"
#include "quicksearch.h"
#include "recordlocator.h"
#include <QVBoxLayout>
#include "filelegend.h"
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <algorithm>
#include <utility>



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

    // Плашки команд (F-7) — выше остальных: процесс — главное во вкладке.
    m_commandBars = new QWidget(this);
    m_commandBars->setObjectName(QStringLiteral("commandBars"));
    auto* commandBarsLayout = new QVBoxLayout(m_commandBars);
    commandBarsLayout->setContentsMargins(0, 0, 0, 0);
    commandBarsLayout->setSpacing(0);
    m_commandBars->hide();
    layout->addWidget(m_commandBars);

    // Плашка «вернуть фильтры прошлого раза» (память файла, R-4): над логом,
    // скрыта, пока фильтры не предложены.
    m_filtersOfferBar = new QFrame(this);
    m_filtersOfferBar->setObjectName(QStringLiteral("filtersOfferBar"));
    m_filtersOfferBar->setFrameShape(QFrame::StyledPanel);
    m_filtersOfferBar->setAutoFillBackground(true);
    m_filtersOfferBar->setBackgroundRole(QPalette::ToolTipBase);
    m_filtersOfferBar->setForegroundRole(QPalette::ToolTipText);
    auto* barLayout = new QHBoxLayout(m_filtersOfferBar);
    m_filtersOfferText = new QLabel(m_filtersOfferBar);
    m_filtersOfferText->setObjectName(QStringLiteral("filtersOfferText"));
    m_filtersOfferText->setWordWrap(true);
    barLayout->addWidget(m_filtersOfferText, 1);
    auto* restoreButton = new QPushButton(tr("Restore Filters"), m_filtersOfferBar);
    restoreButton->setObjectName(QStringLiteral("restoreFiltersButton"));
    restoreButton->setToolTip(tr("Apply the filters this file had when it was last closed"));
    barLayout->addWidget(restoreButton);
    auto* dismissButton = new QToolButton(m_filtersOfferBar);
    dismissButton->setObjectName(QStringLiteral("dismissFiltersOfferButton"));
    dismissButton->setText(QStringLiteral("✕"));
    dismissButton->setAutoRaise(true);
    dismissButton->setToolTip(tr("Hide this offer for this tab"));
    barLayout->addWidget(dismissButton);
    m_filtersOfferBar->hide();
    layout->addWidget(m_filtersOfferBar);
    connect(restoreButton, &QPushButton::clicked, this, &LogViewWidget::applyOfferedFilters);
    connect(dismissButton, &QToolButton::clicked, this, [this]() {
        // Отказ — до закрытия вкладки; сами фильтры не теряются: их снимет
        // captureState, и плашка появится при следующем открытии файла.
        m_filtersOfferDismissed = true;
        updateFiltersOfferBar();
    });
    connect(this, &LogViewWidget::offeredFiltersChanged, this, &LogViewWidget::updateFiltersOfferBar);

    // Плашка показа полного лога (R-3): фильтры вкладки приостановлены.
    // Тот же вид, что у предложения фильтров, — роли палитры, без своего стиля.
    m_fullLogBar = new QFrame(this);
    m_fullLogBar->setObjectName(QStringLiteral("fullLogBar"));
    m_fullLogBar->setFrameShape(QFrame::StyledPanel);
    m_fullLogBar->setAutoFillBackground(true);
    m_fullLogBar->setBackgroundRole(QPalette::ToolTipBase);
    m_fullLogBar->setForegroundRole(QPalette::ToolTipText);
    auto* fullLogLayout = new QHBoxLayout(m_fullLogBar);
    m_fullLogText = new QLabel(m_fullLogBar);
    m_fullLogText->setObjectName(QStringLiteral("fullLogText"));
    m_fullLogText->setWordWrap(true);
    fullLogLayout->addWidget(m_fullLogText, 1);
    auto* returnButton = new QPushButton(tr("Return to Filtered View"), m_fullLogBar);
    returnButton->setObjectName(QStringLiteral("returnToFilteredButton"));
    returnButton->setToolTip(tr("Apply the paused filters of this tab again"));
    fullLogLayout->addWidget(returnButton);
    m_fullLogBar->hide();
    layout->addWidget(m_fullLogBar);
    m_fullLog = new FullLogController(*m_model, *m_view, this);
    // Схема могла смениться, пока фильтры приостановлены: правила — к нынешней.
    m_fullLog->setRuleBinder([this](FilterRuleSet& rules) {
        rules.bindFields(parserFieldNames());
    });
    m_view->setFullLogActionEnabled(true);
    connect(returnButton, &QPushButton::clicked, this, [this]() { m_fullLog->returnToFiltered(); });
    connect(m_view, &LogListView::fullLogToggleRequested, this, [this](int row) { toggleFullLog(row); });
    connect(m_fullLog, &FullLogController::activeChanged, this, [this](bool shown) {
        m_view->setFullLogShown(shown);
        updateFullLogBar();
        emit fullLogViewChanged(shown);
    });

    // Легенда файлов слитой вкладки (U-23): над логом, видна при двух
    // файлах и больше в режимах меток Name и Color Only.
    m_fileLegend = new FileLegend(this);
    m_fileLegend->setView(m_view);
    m_fileLegend->hide();
    connect(m_fileLegend, &FileLegend::visibilityWanted, m_fileLegend, &QWidget::setVisible);
    connect(m_fileLegend, &FileLegend::statusMessage, this, &LogViewWidget::statusMessage);
    m_fileLegend->setModel(m_model);
    layout->addWidget(m_fileLegend);

    layout->addWidget(m_view);

    // Отложенная запись памяти файла после правки закладки или заметки.
    m_memorySaveTimer = new QTimer(this);
    m_memorySaveTimer->setSingleShot(true);
    m_memorySaveTimer->setInterval(2000);
    connect(m_memorySaveTimer, &QTimer::timeout, this, &LogViewWidget::saveFileMemory);
    connect(&bookmarks(), &BookmarkStore::changed, this, &LogViewWidget::onBookmarksChanged);

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(0, 0);

    m_quickSearch = new QuickSearch(this);
    connect(m_quickSearch, &QuickSearch::finished, this, &LogViewWidget::handleQuickSearchFinished);
    connect(m_quickSearch, &QuickSearch::progress, this,
            [this](int percent) { emit quickSearchProgress(m_quickSearchQuery.text, percent); });
    // Модель перестроилась посреди скана (фильтр, перезагрузка файла) —
    // ищем заново от текущей строки.
    connect(m_quickSearch, &QuickSearch::invalidated, this, &LogViewWidget::startQuickSearch);

    m_levelJump = new QuickSearch(this);
    connect(m_levelJump, &QuickSearch::finished, this,
            [this](int row) { finishLevelJump(row, m_levelJump->startRowNow()); });
    connect(m_levelJump, &QuickSearch::progress, this, [this](int percent) {
        emit levelJumpProgress(m_levelJumpLevels, m_levelJumpForward, percent);
    });
    // Модель перестроилась посреди скана: номер из снапшота ничего не значит,
    // а опорная строка могла исчезнуть — честно сообщаем, а не гадаем, откуда
    // искать заново (view восстанавливает выделение отложенно).
    connect(m_levelJump, &QuickSearch::invalidated, this, [this]() {
        emit levelJumpFinished(m_levelJumpLevels, LevelJumpOutcome::Interrupted);
    });

    // Соединяем сигналы парсера со слотами (начальная загрузка)
    connect(m_logParser, &LogParser::entriesParsed,   this, &LogViewWidget::handleEntriesParsed);
    connect(m_logParser, &LogParser::parsingStarted,  this, [this](const LogFilePtr& f){ emit fileParsingStarted(f); });
    connect(m_logParser, &LogParser::parsingProgress, this, [this](int p, const LogFilePtr& f){ emit fileParsingProgress(f, p); });
    connect(m_logParser, &LogParser::tailState, this, &LogViewWidget::handleTailState);
    connect(m_logParser, &LogParser::encodingDecided, this, &LogViewWidget::handleEncodingDecided);
    connect(m_logParser, &LogParser::lineFormatDecided, this, &LogViewWidget::handleLineFormatDecided);
    connect(m_logParser, &LogParser::parsingFinished, this, &LogViewWidget::handleParsingFinished);
    connect(m_logParser, &LogParser::parsingFailed,   this, &LogViewWidget::handleParsingFailed);

    // Соединяем сигналы reload-парсера (только для инкрементальных обновлений)
    connect(m_reloadParser, &LogParser::entriesParsed, this, &LogViewWidget::handleIncrementalEntriesParsed);
    connect(m_reloadParser, &LogParser::tailState, this, &LogViewWidget::handleTailState);
    connect(m_reloadParser, &LogParser::encodingDecided, this, &LogViewWidget::handleEncodingDecided);
    connect(m_reloadParser, &LogParser::lineFormatDecided, this, &LogViewWidget::handleLineFormatDecided);
    // Запасная кодировка — настройка приложения; её смену вкладка применяет
    // сама (новые проходы и предварительные решения), окну тут делать нечего.
    m_fallbackEncoding = AppSettings::instance().fallbackEncoding();
    connect(&AppSettings::instance(), &AppSettings::fallbackEncodingChanged,
            this, &LogViewWidget::setFallbackEncoding);
    m_logParser->setFallbackEncoding(m_fallbackEncoding);
    m_reloadParser->setFallbackEncoding(m_fallbackEncoding);
    connect(m_reloadParser, &LogParser::parsingFinished, this, &LogViewWidget::handleIncrementalParsingFinished);
    // Обязательно: без этого сорвавшаяся дозапись оставила бы loadInFlight
    // висеть, и файл больше никогда бы не обновлялся.
    connect(m_reloadParser, &LogParser::parsingFailed, this, &LogViewWidget::handleIncrementalParsingFailed);

    // Соединяем сигнал изменения текущей строки
    connect(m_view->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex &current, const QModelIndex &previous){
        Q_UNUSED(previous);
        int currentRow = current.isValid() ? current.row() : -1;
        int totalRows = m_model->rowCount();
        emit currentRowChanged(currentRow, totalRows);
    });

    // Контекстное меню строки: закладка и заметка к ней.
    m_view->setBookmarkActionsEnabled(true);
    m_view->setTimeZeroActionsEnabled(true);
    connect(m_view, &LogListView::bookmarkActionRequested, this,
            [this](int row, LogListView::BookmarkAction action) {
        if (action == LogListView::BookmarkAction::Toggle) {
            toggleBookmark(row);
            return;
        }
        const int before = bookmarks().count();
        const int id = bookmarks().add(row);
        if (id >= 0)
            emit bookmarkNoteEditRequested(id, bookmarks().count() > before);
    });

    // История переходов: боковые кнопки мыши и состояние пунктов меню.
    connect(m_view, &LogListView::historyNavigationRequested, this,
            [this](bool forward) { navigateHistory(forward); });
    connect(&m_model->navHistory(), &NavHistory::changed, this, &LogViewWidget::navHistoryChanged);

    // Поиск строк по сохранённым идентификаторам (R-1): проверка текста на
    // индексном пути идёт в фоне — пока она не кончилась, вкладка «грузится».
    connect(&m_model->recordLocator(), &RecordLocator::busyChanged,
            this, [this](bool) { updateLoadingState(); });
    // Позиция из сохранённого состояния вкладки (R-4): строку ищет тот же
    // локатор, что и закладки.
    connect(&m_model->recordLocator(), &RecordLocator::resolved, this,
            [this](int token, const RecordKey& key) {
        if (token == m_pendingPosition.token)
            finishPendingPosition(&key);
    });
    connect(&m_model->recordLocator(), &RecordLocator::refused, this,
            [this](int token, RecordLocator::Refusal) {
        if (token == m_pendingPosition.token)
            finishPendingPosition(nullptr);
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
    // Процессы команд (F-7): спулер уходит от вкладки и останавливает процесс
    // сам (вежливо, затем принудительно), спул удаляется после его выхода.
    for (CommandSpooler* spooler : std::as_const(m_commands)) {
        spooler->disconnect(this);
        spooler->release();
    }
    m_commands.clear();
    // Распаковки — до файлов (F-2): отмена срабатывает на ближайшем блоке
    // входа, частичный файл удаляет сам распаковщик; копию держит SpoolItem
    // файла, а он уходит вместе с m_loadedFiles — уже после ожидания.
    for (auto it = m_archives.begin(); it != m_archives.end(); ++it) {
        if (!it->unpacker)
            continue;
        it->unpacker->cancel();
        it->unpacker->waitForFinished();
        delete it->unpacker;
        it->unpacker = nullptr;
    }
    // Контроллер держит ссылки на модель и view — уходит раньше них.
    delete m_fullLog;
    m_fullLog = nullptr;
    // m_logParser будет удален автоматически, т.к. его parent - this (LogViewWidget)
    // Если бы он не был дочерним QObject, нужно было бы delete m_logParser;
}

void LogViewWidget::setParserPattern(const QString& pattern)
{
    const SchemaChange change = beginSchemaChange();
    if (pattern != parserPattern())
        m_schemaName.clear(); // имя каталога описывало прежнюю схему
    m_pattern.setPattern(pattern);
    m_fileSchemas = normalizedFileSchemas(m_fileSchemas);
    refreshSchemas();
    endSchemaChange(change);
}

QStringList LogViewWidget::layoutInputs() const
{
    // Схема вкладки, затем свои схемы файлов в порядке файлов вкладки —
    // порядок колонок раскладки (объединение по имени).
    QStringList inputs{ m_pattern.patternString() };
    for (const LogFilePtr& f : m_loadedFiles) {
        if (!f)
            continue;
        const QString format = lineFormat(f).fieldDefinition();
        if (!format.isEmpty()) {
            // JSON / logfmt (F-17, шаг 7): колонки — ключи формата строк.
            inputs << format;
            continue;
        }
        const auto it = m_fileSchemas.constFind(f->filePath);
        if (it != m_fileSchemas.constEnd() && it->hasSchema())
            inputs << it->schema;
    }
    return inputs;
}

FileSchemaPtr LogViewWidget::layoutSchema(const QString& definition)
{
    if (definition.isEmpty())
        return nullptr;
    const auto it = m_layoutSchemas.constFind(definition);
    if (it != m_layoutSchemas.constEnd())
        return it.value();
    FileSchemaPtr schema = m_fieldLayout.schemaFor(definition);
    m_layoutSchemas.insert(definition, schema);
    return schema;
}

void LogViewWidget::rebuildFieldLayout()
{
    // Смена схемы пользователем — раскладка заново (поля вкладки
    // переизвлекает контроллер). Те же входы — та же раскладка и те же
    // указатели (модель сравнивает наборы схем по указателям).
    // Порядок не сравнивается: файл со своей схемой, ставший ею позже
    // соседа (распаковка), растит раскладку в конец — пересборка в порядке
    // файлов переставила бы колонки под полями уже разобранных записей.
    const QStringList inputs = layoutInputs();
    if (m_tabSchema && !m_layoutInputs.isEmpty() && inputs.first() == m_layoutInputs.first()
        && QSet<QString>(inputs.cbegin(), inputs.cend())
               == QSet<QString>(m_layoutInputs.cbegin(), m_layoutInputs.cend()))
        return;
    m_fieldLayout.clear();
    m_layoutSchemas.clear();
    m_tabSchema = m_fieldLayout.schemaFor(m_pattern);
    m_layoutSchemas.insert(m_pattern.patternString(), m_tabSchema);
    m_layoutInputs = { m_pattern.patternString() };
    for (int i = 1; i < inputs.size(); ++i) {
        layoutSchema(inputs[i]);
        if (!m_layoutInputs.contains(inputs[i]))
            m_layoutInputs << inputs[i];
    }
}

void LogViewWidget::refreshSchemas()
{
    rebuildFieldLayout();
    // Показ колонок действует, только если у вкладки есть хоть одна колонка.
    m_fieldDisplay = m_showFieldsFlag && !m_fieldLayout.columns.isEmpty();
    applySchemaToModel();
    if (m_model)
        m_model->setAvailableFields(m_fieldLayout.columns);
}

void LogViewWidget::applySchemaToModel()
{
    if (!m_model)
        return;
    // Схема файла — своя или схема вкладки; nullptr — у файла нет схемы.
    // Галочка показа полей сюда не входит (F-17, шаг 3): поля, правила по
    // полю и чтение (до шага 7 — граница записи по схеме, ключи ключевой)
    // действуют при любом показе.
    FileSchemaSet schemas;
    if (m_tabSchema && m_tabSchema->isValid() && !m_tabSchema->extractor->fields().isEmpty())
        schemas.fallback = m_tabSchema;
    for (auto it = m_fileSchemas.cbegin(); it != m_fileSchemas.cend(); ++it) {
        const FileSchemaPtr schema = m_layoutSchemas.value(it->schema);
        schemas.byPath.insert(it.key(), schema && schema->isValid() ? schema : FileSchemaPtr());
    }
    // Файл JSON / logfmt (F-17, шаг 7): поля — ключи его формата строк,
    // главнее схемы вкладки и своей схемы (строчная схема по строке-объекту
    // ничего бы не разобрала).
    for (const LogFilePtr& f : std::as_const(m_loadedFiles)) {
        if (!f)
            continue;
        const QString format = lineFormat(f).fieldDefinition();
        if (format.isEmpty())
            continue;
        const FileSchemaPtr schema = m_layoutSchemas.value(format);
        if (schema && schema->isValid())
            schemas.byPath.insert(f->filePath, schema);
    }
    m_model->setFileSchemas(schemas);
}

QHash<QString, FieldSettings>
LogViewWidget::normalizedFileSchemas(const QHash<QString, FieldSettings>& files) const
{
    // Только файлы вкладки и только схемы, отличные от схемы вкладки; от
    // FieldSettings — имя и определение.
    QHash<QString, FieldSettings> out;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        if (it->schema == m_pattern.patternString())
            continue;
        const bool loaded = std::any_of(m_loadedFiles.cbegin(), m_loadedFiles.cend(),
            [&it](const LogFilePtr& f) { return f && f->filePath == it.key(); });
        if (!loaded)
            continue;
        FieldSettings schema;
        schema.schema = it->schema;
        schema.schemaName = it->hasSchema() ? it->schemaName : QString();
        out.insert(it.key(), schema);
    }
    return out;
}

FieldSettings LogViewWidget::fileSchema(const LogFilePtr& logFile) const
{
    if (logFile) {
        const auto it = m_fileSchemas.constFind(logFile->filePath);
        if (it != m_fileSchemas.constEnd())
            return *it;
    }
    FieldSettings schema;
    schema.schema = parserPattern();
    schema.schemaName = schema.schema.isEmpty() ? QString() : m_schemaName;
    return schema;
}

void LogViewWidget::setFileSchema(const LogFilePtr& logFile, const FieldSettings& schema)
{
    if (!logFile)
        return;
    QHash<QString, FieldSettings> files = m_fileSchemas;
    files.insert(logFile->filePath, schema);
    setFieldSettings(fieldSettings(), files);
}

LogFilePtr LogViewWidget::fileForSourceKey(const QString& sourceKey) const
{
    for (const LogFilePtr& f : m_loadedFiles)
        if (f && RecordLocator::sourceKey(*f) == sourceKey)
            return f;
    return nullptr;
}

void LogViewWidget::setPendingFileSchema(const QString& sourceKey, const FieldSettings& schema)
{
    // Явная схема (addLogFile со схемой) главнее памяти файла.
    if (!m_pendingFileSchemas.contains(sourceKey))
        m_pendingFileSchemas.insert(sourceKey, schema);
    // Файл уже во вкладке, но строк у него нет (сжатый ждёт распаковки).
    const LogFilePtr file = fileForSourceKey(sourceKey);
    if (file && !fileHasRows(file))
        adoptPendingSchema(file);
}

bool LogViewWidget::fileHasRows(const LogFilePtr& logFile) const
{
    if (!logFile || isArchivePending(logFile))
        return false;
    const auto it = m_fileReloadStates.constFind(logFile->filePath);
    return it != m_fileReloadStates.constEnd() && (it->initialLoadDone || it->loadInFlight);
}

void LogViewWidget::adoptPendingSchema(const LogFilePtr& logFile)
{
    if (!logFile || m_pendingFileSchemas.isEmpty())
        return;
    const auto it = m_pendingFileSchemas.find(RecordLocator::sourceKey(*logFile));
    if (it == m_pendingFileSchemas.end())
        return;
    const FieldSettings schema = it.value();
    m_pendingFileSchemas.erase(it);
    if (schema.schema == parserPattern())
        return;
    // Строк у файла ещё нет: раскладка растёт (колонки дописываются в конец —
    // индексы полей загруженных записей не устаревают), переизвлекать нечего.
    const QStringList before = m_fieldLayout.columns;
    // Выбраны были все колонки — новые тоже показываются.
    const bool allVisible = std::all_of(before.cbegin(), before.cend(),
        [this](const QString& name) { return m_visibleFields.contains(name); });
    FieldSettings own;
    own.schema = schema.schema;
    own.schemaName = schema.hasSchema() ? schema.schemaName : QString();
    m_fileSchemas.insert(logFile->filePath, own);
    if (own.hasSchema()) {
        layoutSchema(own.schema);
        if (!m_layoutInputs.contains(own.schema))
            m_layoutInputs << own.schema;
    }
    // Новые колонки: по выбору снимка (сессия, Ctrl+Shift+T — колонки схем
    // файлов в нём есть), иначе — показаны, если были показаны все прежние.
    for (const QString& name : std::as_const(m_fieldLayout.columns)) {
        if (before.contains(name) || m_visibleFields.contains(name))
            continue;
        const bool show = m_stateVisibleFields
            ? (m_stateVisibleFields->isEmpty() || m_stateVisibleFields->contains(name))
            : allVisible;
        if (show)
            m_visibleFields << name;
    }
    if (m_pendingFileSchemas.isEmpty())
        m_stateVisibleFields.reset();
    refreshSchemas();
    if (m_fieldLayout.columns != before)
        emit fieldLayoutGrown();
}


void LogViewWidget::setShowFields(bool enabled)
{
    const SchemaChange change = beginSchemaChange();
    m_showFieldsFlag = enabled;
    refreshSchemas();
    endSchemaChange(change);
}

FieldSettings LogViewWidget::fieldSettings() const
{
    FieldSettings settings;
    settings.schema = parserPattern();
    settings.schemaName = settings.schema.isEmpty() ? QString() : m_schemaName;
    settings.showFields = m_showFieldsFlag;
    settings.visibleFields = m_visibleFields;
    return settings;
}

void LogViewWidget::setFieldSettings(const FieldSettings& settings)
{
    setFieldSettings(settings, m_fileSchemas);
}

void LogViewWidget::setFieldSettings(const FieldSettings& settings,
                                     const QHash<QString, FieldSettings>& files)
{
    // Позиция и подписи — до того, как новая схема дойдёт до модели и вида.
    const SchemaChange change = beginSchemaChange();
    m_showFieldsFlag = settings.showFields;
    if (settings.schema != parserPattern())
        m_pattern.setPattern(settings.schema);
    m_fileSchemas = normalizedFileSchemas(files);
    m_schemaName = settings.schema.isEmpty() ? QString() : settings.schemaName;
    m_visibleFields = settings.visibleFields;
    refreshSchemas();
    endSchemaChange(change);
}

// =============================================================================
// Перечитывание по смене схемы (F-4, PR 3)
// =============================================================================

QString LogViewWidget::readingSignature(const QString& filePath) const
{
    // Чтение не знает о схеме полей (F-17, шаг 7): момент, уровень и границу
    // записи решает только формат строк файла.
    for (const LogFilePtr& f : m_loadedFiles)
        if (f && f->filePath == filePath)
            return lineFormat(f).readingSignature();
    return QString();
}

// =============================================================================
// Формат строк файла (F-17, шаг 7)
// =============================================================================

LineFormat LogViewWidget::lineFormat(const LogFilePtr& logFile) const
{
    if (!logFile)
        return {};
    if (const auto* store = m_model ? m_model->indexedOrNull() : nullptr) {
        if (store->indexForFile(logFile->filePath))
            return store->lineFormat(logFile);
    } else {
        const auto it = m_fileFormats.constFind(logFile->filePath);
        if (it != m_fileFormats.constEnd())
            return *it;
    }
    return m_pendingFormats.value(logFile->filePath);
}

void LogViewWidget::storeLineFormat(const LogFilePtr& logFile, const LineFormat& format)
{
    if (!logFile)
        return;
    auto* store = m_model->indexedOrNull();
    if (store && store->indexForFile(logFile->filePath)) {
        store->setLineFormat(logFile, format);
        m_pendingFormats.remove(logFile->filePath);
    } else if (!store) {
        m_fileFormats.insert(logFile->filePath, format);
        m_pendingFormats.remove(logFile->filePath);
    } else {
        m_pendingFormats.insert(logFile->filePath, format);   // файл ещё не подключён
    }
}

LineFormat LogViewWidget::passFormat(const LogFilePtr& logFile)
{
    if (!logFile)
        return {};
    // Ожидающий формат (выбор до загрузки, объединение, переход порога)
    // становится форматом файла на старте прохода.
    const auto pending = m_pendingFormats.constFind(logFile->filePath);
    if (pending != m_pendingFormats.constEnd()) {
        const LineFormat format = *pending;
        storeLineFormat(logFile, format);
        if (format.isStructured())
            adoptFormatColumns(logFile);
        return format;
    }
    // Выбор из снимка или памяти файла ждёт файл по источнику (сжатый
    // читается копией с другим путём).
    if (!m_pendingSourceFormats.isEmpty()) {
        const auto bySource = m_pendingSourceFormats.find(RecordLocator::sourceKey(*logFile));
        if (bySource != m_pendingSourceFormats.end()) {
            const LineFormat format = *bySource;
            m_pendingSourceFormats.erase(bySource);
            storeLineFormat(logFile, format);
            if (format.isStructured())
                adoptFormatColumns(logFile);
            return format;
        }
    }
    return lineFormat(logFile);
}

void LogViewWidget::forgetAutoFormat(const LogFilePtr& logFile)
{
    const LineFormat current = lineFormat(logFile);
    if (current.isDecided() && !current.isUser())
        storeLineFormat(logFile, LineFormat());
}

void LogViewWidget::adoptFormatColumns(const LogFilePtr& logFile)
{
    const QString definition = lineFormat(logFile).fieldDefinition();
    if (definition.isEmpty() || m_layoutInputs.contains(definition)) {
        refreshSchemas();
        return;
    }
    // Раскладка растёт (как у схемы файла, пришедшей позже): колонки ключей
    // — в конец; показаны, если были показаны все прежние.
    const QStringList before = m_fieldLayout.columns;
    const bool allVisible = std::all_of(before.cbegin(), before.cend(),
        [this](const QString& name) { return m_visibleFields.contains(name); });
    layoutSchema(definition);
    m_layoutInputs << definition;
    if (allVisible)
        for (const QString& name : std::as_const(m_fieldLayout.columns))
            if (!before.contains(name) && !m_visibleFields.contains(name))
                m_visibleFields << name;
    refreshSchemas();
    if (m_fieldLayout.columns != before)
        emit fieldLayoutGrown();
}

void LogViewWidget::handleLineFormatDecided(const LogFilePtr& logFile, const LineFormat& format,
                                            bool afterLines)
{
    if (!logFile || !m_loadedFiles.contains(logFile))
        return;
    const LineFormat before = lineFormat(logFile);
    // Выбор пользователя, сделанный посреди прохода, главнее решения прохода
    // (перечитывание по нему уже назначено).
    if (before.isUser())
        return;
    const bool reread = afterLines && format.readingSignature() != before.readingSignature();
    const std::optional<RereadPosition> position =
        reread ? captureRereadPosition() : std::nullopt;
    storeLineFormat(logFile, format);
    adoptFormatColumns(logFile);
    emit lineFormatChanged(logFile);
    // Малый файл (меньше 20 непустых строк) решился в дозаписи, и формат
    // читает строки иначе, чем Plain, которым читались прежние проходы, —
    // файл перечитывается один раз (проход в полёте — по его концу).
    if (reread)
        rereadAfterSchemaChange(position, {logFile->filePath});
}

void LogViewWidget::setLineFormat(const LogFilePtr& logFile, const LineFormat& format)
{
    if (!logFile || !m_loadedFiles.contains(logFile))
        return;
    LineFormat chosen = format;
    chosen.origin = LineFormat::Origin::User;
    if (chosen.isStructured())
        chosen = LineFormat::structured(chosen.kind, chosen.keys, LineFormat::Origin::User);
    else
        chosen.keys = KeyedDefinition();
    const LineFormat before = lineFormat(logFile);
    if (!fileHasRows(logFile)) {
        m_pendingFormats.insert(logFile->filePath, chosen);
        emit lineFormatChanged(logFile);
        return;
    }
    const std::optional<RereadPosition> position = captureRereadPosition();
    // Проход в полёте читает форматом, с которым начался (или решит свой), —
    // перечитать по его концу в любом случае.
    const bool inFlight = m_fileReloadStates.value(logFile->filePath).loadInFlight;
    storeLineFormat(logFile, chosen);
    // Колонки — выбор пользователя: раскладка строится заново.
    m_layoutInputs.clear();
    refreshSchemas();
    emit fieldLayoutGrown();
    emit lineFormatChanged(logFile);
    if (inFlight || chosen.readingSignature() != before.readingSignature())
        rereadAfterSchemaChange(position, {logFile->filePath});
}

void LogViewWidget::restoreAutomaticLineFormat(const LogFilePtr& logFile)
{
    if (!logFile || !m_loadedFiles.contains(logFile) || !lineFormat(logFile).isUser())
        return;
    // Решение автоматики — заново по файлу (то же правило, что у прохода).
    const LineFormat automatic = LineFormatDetector::detectInFile(logFile->filePath);
    const LineFormat before = lineFormat(logFile);
    if (!fileHasRows(logFile)) {
        m_pendingFormats.remove(logFile->filePath);
        emit lineFormatChanged(logFile);
        return;
    }
    const std::optional<RereadPosition> position = captureRereadPosition();
    const bool inFlight = m_fileReloadStates.value(logFile->filePath).loadInFlight;
    storeLineFormat(logFile, automatic);
    m_layoutInputs.clear();
    refreshSchemas();
    emit fieldLayoutGrown();
    emit lineFormatChanged(logFile);
    if (inFlight || automatic.readingSignature() != before.readingSignature())
        rereadAfterSchemaChange(position, {logFile->filePath});
}

QHash<QString, QString> LogViewWidget::readingSignatures() const
{
    QHash<QString, QString> out;
    for (const LogFilePtr& f : m_loadedFiles)
        if (f)
            out.insert(f->filePath, readingSignature(f->filePath));
    return out;
}

LogViewWidget::SchemaChange LogViewWidget::beginSchemaChange() const
{
    return SchemaChange{ readingSignatures(), captureRereadPosition() };
}

void LogViewWidget::endSchemaChange(const SchemaChange& change)
{
    const QHash<QString, QString> after = readingSignatures();
    QSet<QString> changed;
    for (auto it = after.cbegin(); it != after.cend(); ++it)
        if (change.signatures.value(it.key()) != it.value())
            changed.insert(it.key());
    if (!changed.isEmpty())
        rereadAfterSchemaChange(change.position, changed);
}

std::optional<LogViewWidget::RereadPosition> LogViewWidget::captureRereadPosition() const
{
    // Строка и её место на экране; follow-tail — вид у хвоста, места нет.
    if (m_view->followTail() || m_loadedFiles.isEmpty())
        return std::nullopt;
    const LogListView::ViewAnchor anchor = m_view->viewAnchor();
    const RecordKey key = anchor.isValid() ? m_model->keyAt(anchor.row) : RecordKey();
    if (!key.isValid())
        return std::nullopt;
    RecordLocator& locator = m_model->recordLocator();
    const auto source = locator.sourceFor(key.file);
    if (!source)
        return std::nullopt;
    return RereadPosition{ RecordLocator::sourceKey(*key.file), *source,
                           locator.idFor(key, m_model->messageAt(anchor.row)),
                           anchor.offset, anchor.selected, m_view->userActionCount() };
}

void LogViewWidget::rereadAfterSchemaChange(const std::optional<RereadPosition>& position,
                                            const QSet<QString>& changed)
{
    // Позиция снята до смены схемы (captureRereadPosition). Восстановленная
    // позиция, которая ещё ждёт строку (applyState), ищется заново после
    // сброса строк: ключ, найденный в старых строках, пропадёт. Позиция в
    // файле, который не перечитывается, остаётся на месте сама (якорь вида);
    // позиция, которая ждёт перечитывания другого файла, остаётся ждать.
    const auto rereadsSource = [this, &changed](const QString& sourceKey) {
        for (const LogFilePtr& f : std::as_const(m_loadedFiles)) {
            if (!f || RecordLocator::sourceKey(*f) != sourceKey || !changed.contains(f->filePath))
                continue;
            const FileReloadState st = m_fileReloadStates.value(f->filePath);
            return !st.unsupported && (st.loadInFlight || st.initialLoadDone);
        }
        return false;
    };
    if (m_rereadPosition && rereadsSource(m_rereadPosition->sourceKey))
        m_rereadPosition.reset();
    if (isPositionPending()) {
        // Файл позиции ещё не читался (распаковка, загрузка сорвалась) —
        // запрос остаётся своим: его первая загрузка уже с новой схемой.
        if (m_pendingPosition.record.isValid()
            && rereadsSource(RecordLocator::sourceKey(m_pendingPosition.source))) {
            m_rereadPosition = RereadPosition{ RecordLocator::sourceKey(m_pendingPosition.source),
                                               m_pendingPosition.source,
                                               m_pendingPosition.record, m_pendingPosition.offset,
                                               m_pendingPosition.selected,
                                               m_pendingPosition.userActions };
            if (m_pendingPosition.token)
                m_model->recordLocator().cancel(m_pendingPosition.token);
            m_pendingPosition = {};
        }
    } else if (position && rereadsSource(position->sourceKey)) {
        m_rereadPosition = position;
    }
    // Резидентная вкладка держит позицию до конца всех перечитываний (первая
    // смена — её позиция: последующие снимают уже сдвинутый вид).
    if (!m_model->isIndexedBackend() && !m_heldPosition)
        m_heldPosition = m_rereadPosition ? m_rereadPosition : position;
    for (const LogFilePtr& logFile : std::as_const(m_loadedFiles)) {
        if (!logFile || !changed.contains(logFile->filePath))
            continue;
        const auto it = m_fileReloadStates.find(logFile->filePath);
        if (it == m_fileReloadStates.end() || it->unsupported)
            continue;
        if (it->loadInFlight) {
            // Проход идёт с прежним форматом строк (его дал passFormat на старте).
            it->schemaRereadPending = true;
            continue;
        }
        // Не прочитан (распаковка, файл пропал, загрузка сорвалась) — его
        // следующая загрузка возьмёт новую схему сама.
        if (it->initialLoadDone)
            rereadForSchema(logFile);
    }
    if (m_heldPosition
        && std::none_of(m_fileReloadStates.cbegin(), m_fileReloadStates.cend(),
                        [](const FileReloadState& s) { return s.reencodeRekey || s.schemaRereadPending; }))
        m_heldPosition.reset();
    updateLoadingState();
}

void LogViewWidget::rereadForSchema(const LogFilePtr& logFile)
{
    if (m_model->isIndexedBackend()) {
        // Индекс не хранит, какая схема решила границы, — файл индексируется
        // заново (как переписанный, Q-19): строки сброшены, закладки и
        // история найдут свои строки через RecordLocator, позиция — тоже.
        startFullReload(logFile, /*keepFormat=*/true);
        requestRereadPosition(logFile);
        return;
    }
    startReencode(logFile, /*rekey=*/true);
}

bool LogViewWidget::startPendingReread(const LogFilePtr& logFile)
{
    if (!logFile)
        return false;
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    const bool schema = std::exchange(st.schemaRereadPending, false);
    const bool encoding = std::exchange(st.reparsePending, false);
    if (!schema && !encoding)
        return false;
    if (m_model->isIndexedBackend()) {
        // Кодировку индексной вкладки перечитывает стор (без переиндексации).
        if (!schema)
            return false;
        rereadForSchema(logFile);
        return true;
    }
    // Резидентный: один разбор и для кодировки, и для схемы.
    startReencode(logFile, schema);
    return true;
}

void LogViewWidget::requestRereadPosition(const LogFilePtr& logFile)
{
    // Строки файла уже сброшены (локатор ждёт их нового поколения): запрос
    // не найдёт прежнюю строку в старых данных.
    if (!m_rereadPosition || !logFile
        || m_rereadPosition->sourceKey != RecordLocator::sourceKey(*logFile))
        return;
    // Схему сменили ещё раз посреди этого прохода: строку ищет следующий
    // проход — ключи этого через миг снова сменятся.
    if (m_fileReloadStates.value(logFile->filePath).schemaRereadPending)
        return;
    requestPosition(*std::exchange(m_rereadPosition, std::nullopt));
}

void LogViewWidget::requestPosition(const RereadPosition& pos)
{
    if (m_pendingPosition.token)
        m_model->recordLocator().cancel(m_pendingPosition.token);
    m_pendingPosition = {};
    m_pendingPosition.token = m_model->recordLocator().request(pos.source, pos.record);
    m_pendingPosition.source = pos.source;
    m_pendingPosition.record = pos.record;
    m_pendingPosition.requests = 1;
    m_pendingPosition.offset = pos.offset;
    m_pendingPosition.selected = pos.selected;
    m_pendingPosition.userActions = pos.userActions;
}

QVector<int> LogViewWidget::visibleFieldIndexes() const
{
    const QStringList names = parserFieldNames();
    QVector<int> indexes;
    indexes.reserve(names.size());
    for (int i = 0; i < names.size(); ++i)
        if (m_visibleFields.contains(names[i]))
            indexes.push_back(i);
    return indexes;
}

void LogViewWidget::setFallbackEncoding(TextEncoding encoding)
{
    m_fallbackEncoding = encoding;
    m_logParser->setFallbackEncoding(encoding);
    m_reloadParser->setFallbackEncoding(encoding);
    if (m_indexer)
        m_indexer->setFallbackEncoding(encoding);
    // Предварительные решения (шаг 3а) — по уже накопленному свидетельству с
    // новой запасной, путём пересмотра. «Не решено» решат следующие проходы,
    // окончательные и выбор пользователя не трогаются. Проход в полёте
    // решает со своей (прежней) запасной и вернул бы прежний ответ — тогда
    // пересматриваем, когда загрузка закончится (updateLoadingState).
    if (m_loading) {
        m_reconsiderPending = true;
        return;
    }
    reconsiderProvisional();
}

void LogViewWidget::reconsiderProvisional()
{
    m_reconsiderPending = false;
    const TextEncoding encoding = m_fallbackEncoding;
    for (const LogFilePtr& lf : std::as_const(m_loadedFiles)) {
        if (!lf)
            continue;
        FileEncoding state = fileEncoding(lf);
        if (!EncodingDetector::reconsider(state, encoding))
            continue;
        if (m_model->isIndexedBackend())
            adoptIndexedEncoding(lf, state, /*wholeState=*/false);
        else
            handleEncodingDecided(lf, state);
    }
}

FileEncoding LogViewWidget::fileEncoding(const LogFilePtr& logFile) const
{
    if (!logFile)
        return {};
    if (const auto* store = m_model->indexedOrNull())
        return store->fileEncoding(logFile);
    return m_fileEncodings.value(logFile->filePath);
}

TextEncoding LogViewWidget::fileStreamEncoding(const LogFilePtr& logFile) const
{
    if (!logFile)
        return TextEncoding::Utf8;
    // Резидентный путь держит кодировку потока (UTF-16/32); индексная вкладка
    // — только у файла, который она не смогла загрузить (unsupported).
    const auto st = m_fileReloadStates.constFind(logFile->filePath);
    if (st != m_fileReloadStates.constEnd() && (!m_model->isIndexedBackend() || st->unsupported)
        && !TextEncodings::isDecodable(st->encoding))
        return st->encoding;
    return fileEncoding(logFile).decoder.encoding();
}

void LogViewWidget::setFileEncoding(const LogFilePtr& logFile, TextEncoding encoding)
{
    if (!logFile || !TextEncodings::isDecodable(encoding)
        || !m_loadedFiles.contains(logFile))
        return;
    const TextDecoder decoder(encoding);
    // UTF-16/32 читает QTextStream по BOM — выбирать там нечего (в индексной
    // вкладке такой файл не загружен вовсе).
    if (!TextEncodings::isDecodable(fileStreamEncoding(logFile)))
        return;
    if (isArchivePending(logFile)) {
        setPendingArchiveEncoding(logFile, pendingArchiveEncoding(logFile).withUserChoice(decoder));
        return;
    }
    if (m_model->isIndexedBackend()) {
        adoptIndexedEncoding(logFile, fileEncoding(logFile).withUserChoice(decoder),
                             /*wholeState=*/true);
        return;
    }
    applyResidentEncoding(logFile, m_fileEncodings.value(logFile->filePath).withUserChoice(decoder));
}

void LogViewWidget::restoreAutomaticEncoding(const LogFilePtr& logFile)
{
    if (!logFile || !m_loadedFiles.contains(logFile))
        return;
    const FileEncoding current = pendingArchiveEncoding(logFile);
    if (current.source != FileEncoding::Source::User)
        return;
    if (!TextEncodings::isDecodable(fileStreamEncoding(logFile)))
        return;
    if (isArchivePending(logFile)) {
        setPendingArchiveEncoding(logFile, current.automatic());
        return;
    }
    if (m_model->isIndexedBackend()) {
        adoptIndexedEncoding(logFile, current.automatic(), /*wholeState=*/true);
        return;
    }
    // Автоматика под выбором решала рядом; «не решено» — файл до сих пор
    // ASCII либо перечитывание решит само (парсер стартует «не решённым»).
    applyResidentEncoding(logFile, current.automatic());
}

void LogViewWidget::applyResidentEncoding(const LogFilePtr& logFile, const FileEncoding& encoding)
{
    FileEncoding& current = m_fileEncodings[logFile->filePath];
    const bool changed = current.decoder != encoding.decoder;
    current = encoding;
    emit fileEncodingChanged(logFile);
    if (!changed)
        return;
    // Текст записей резидентного пути уже декодирован — файл перечитывается.
    // Пока он читается, второй проход запускать нельзя (дубли записей):
    // перечитаем по окончании.
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    if (st.loadInFlight) {
        st.reparsePending = true;
        return;
    }
    startReencode(logFile);
}

void LogViewWidget::adoptIndexedEncoding(const LogFilePtr& logFile, const FileEncoding& encoding,
                                         bool wholeState)
{
    auto* store = m_model->indexedOrNull();
    if (!store || !logFile)
        return;
    const TextDecoder before = store->fileEncoding(logFile).decoder;
    if (wholeState)
        store->setFileEncodingState(logFile, encoding);
    else
        store->adoptDetectedEncoding(logFile, encoding);
    if (store->fileEncoding(logFile).decoder != before)
        refreshTextFragments(logFile->filePath);
    emit fileEncodingChanged(logFile);
}

void LogViewWidget::refreshTextFragments(const QString& filePath)
{
    bookmarks().refreshPreviews(filePath);
    navHistory().refreshPreviews(filePath);
    // Перефильтрация (текстовые правила) ещё идёт: строки, которые она
    // откроет, получат фрагменты по её окончании (handleModelFilteredRelay).
    if (m_model->isFiltering())
        m_previewRefreshFiles.insert(filePath);
}

void LogViewWidget::startReencode(const LogFilePtr& logFile, bool rekey)
{
    if (!m_reencodeParser) {
        m_reencodeParser = new LogParser(this);
        connect(m_reencodeParser, &LogParser::entriesParsed, this,
                [this](const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr& f) {
                    if (f)
                        m_reencodeBuffers[f->filePath] += batch;
                });
        connect(m_reencodeParser, &LogParser::tailState, this, &LogViewWidget::handleTailState);
        connect(m_reencodeParser, &LogParser::encodingDecided, this,
                &LogViewWidget::handleEncodingDecided);
        connect(m_reencodeParser, &LogParser::lineFormatDecided, this,
                &LogViewWidget::handleLineFormatDecided);
        connect(m_reencodeParser, &LogParser::parsingFinished, this,
                [this](int, const LogFilePtr& f) { finishReencode(f); });
        connect(m_reencodeParser, &LogParser::parsingFailed, this,
                &LogViewWidget::handleReencodeFailed);
    }
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.loadInFlight = true;
    st.reencodeRekey = rekey;
    m_reencodeBuffers[logFile->filePath].clear();
    m_reencodeParser->setFallbackEncoding(m_fallbackEncoding);
    m_reencodeParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath),
                                   passFormat(logFile));
    updateLoadingState();
}

void LogViewWidget::finishReencode(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    QVector<std::shared_ptr<LogEntry>> entries = m_reencodeBuffers.take(logFile->filePath);
    // Вкладка за это время стала индексной (добавили большой файл) — её
    // файлы уже перечитывает индексатор со своим состоянием.
    if (m_model->isIndexedBackend() || !m_loadedFiles.contains(logFile))
        return;
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    const bool rekey = std::exchange(st.reencodeRekey, false);
    // Порция локатору — в порядке файла (до сортировки по времени).
    const QVector<std::shared_ptr<LogEntry>> fileOrder = rekey
        ? entries : QVector<std::shared_ptr<LogEntry>>();
    std::sort(entries.begin(), entries.end(), logEntryPtrLess);
    st.nextLogicalEntryId = 0;
    st.lastEntry.reset();
    st.prevEntry.reset();
    for (const auto& e : std::as_const(entries))
        if (e)
            st.nextLogicalEntryId = qMax(st.nextLogicalEntryId, e->logicalEntryId() + 1);
    rememberLastLines(st, entries);
    if (rekey) {
        // Смена схемы (F-4, PR 3): моменты и id записей другие — ключи
        // строк файла сброшены; закладки, история и позиция найдут строки по
        // номеру и отпечатку в порции ниже.
        m_model->invalidateFileRows(logFile->filePath);
        requestRereadPosition(logFile);
    }
    m_model->replaceEntriesForFile(logFile->filePath, entries);
    if (rekey)
        m_model->recordLocator().residentBatch(fileOrder);
    if (rekey && m_heldPosition) {
        // Reset этого файла вернул вид к записи строки другого файла, а не к
        // строке: строку ищет локатор (поздний проход). Файл строки ещё
        // перечитывается — найдёт его собственное окончание.
        const LogFilePtr positionFile = fileForSourceKey(m_heldPosition->sourceKey);
        const bool own = positionFile == logFile;
        const bool ownPending = positionFile && !own
            && (m_fileReloadStates.value(positionFile->filePath).reencodeRekey
                || m_fileReloadStates.value(positionFile->filePath).schemaRereadPending);
        if (!own && !ownPending && positionFile)
            requestPosition(*m_heldPosition);
        const bool rereading = std::any_of(m_fileReloadStates.cbegin(), m_fileReloadStates.cend(),
            [](const FileReloadState& s) { return s.reencodeRekey || s.schemaRereadPending; });
        if (!rereading)
            m_heldPosition.reset();
    }
    locatorFileLoaded(logFile);
    if (rekey) {
        bookmarksFileLoaded(logFile, true);
        // Моменты и диапазон времени другие — окно обновляет поля Time
        // Filter и статистику, как после индексного перечитывания.
        emit fileParsingFinished(logFile, int(entries.size()));
    }
    // Текст строк файла другой при тех же ключах — фрагменты закладок и
    // истории переходов, иначе сверка после следующей загрузки их потеряет.
    refreshTextFragments(logFile->filePath);
    emit fileEncodingChanged(logFile);
    const qint64 consumed = st.endOffset >= 0 ? st.endOffset
                                              : QFileInfo(logFile->filePath).size();
    st.anchor = FileChangeDetector::capture(logFile->filePath, consumed);
    st.initialLoadDone = true;
    st.loadInFlight = false;
    st.failedStamp = DiskStamp{};
    updateLoadingState();
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
    startPendingReread(logFile);
}

void LogViewWidget::handleReencodeFailed(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    m_reencodeBuffers.remove(logFile->filePath);
    // Файл не открылся: показанные записи остаются прежними (прежней
    // кодировкой), авто-обновление продолжит с прежнего якоря.
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.loadInFlight = false;
    // Перечитывание по смене схемы не состоялось — повторить по концу
    // следующего прохода по файлу (дозаписи), иначе строки остались бы
    // разобранными прежней схемой.
    if (std::exchange(st.reencodeRekey, false))
        st.schemaRereadPending = true;
    updateLoadingState();
}

void LogViewWidget::handleEncodingDecided(const LogFilePtr& logFile,
                                          const FileEncoding& encoding)
{
    // Решение резидентного парсера. Первое решение: строки до первой
    // не-ASCII — ASCII, их текст от решения не зависит; записи с ней парсер
    // отдал уже декодированными. Выбор пользователя автоматикой не
    // заменяется (парсер с ним и не решает, но проход мог начаться до
    // выбора) — решение автоматики хранится рядом.
    if (!logFile || m_model->isIndexedBackend())
        return;
    // Пересмотр предварительного решения (шаг 3а): записи, отданные прежде,
    // декодированы прежней кодировкой — перечитываем файл тем же путём, что
    // ручную смену (подмена записей под одним reset), источник — «авто».
    const bool revised = m_fileEncodings[logFile->filePath].adopt(encoding);
    emit fileEncodingChanged(logFile);
    if (!revised)
        return;
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    if (st.loadInFlight) {
        st.reparsePending = true; // по окончании прохода, как у ручной смены
        return;
    }
    startReencode(logFile);
}

void LogViewWidget::forgetAutoEncoding(const QString& filePath)
{
    const auto it = m_fileEncodings.find(filePath);
    if (it == m_fileEncodings.end())
        return;
    if (it->source != FileEncoding::Source::User)
        m_fileEncodings.erase(it);
    else
        *it = FileEncoding().withUserChoice(it->decoder);
}

QHash<QString, FileEncoding> LogViewWidget::takeEncodingsForIndexed()
{
    // Формат строк (F-17, шаг 7) переходит в стор на старте прохода
    // (passFormat берёт ожидающий).
    for (auto it = m_fileFormats.cbegin(); it != m_fileFormats.cend(); ++it)
        if (it->isDecided() && !m_pendingFormats.contains(it.key()))
            m_pendingFormats.insert(it.key(), *it);
    m_fileFormats.clear();
    QHash<QString, FileEncoding> encodings;
    encodings.swap(m_fileEncodings);
    return encodings;
}

void LogViewWidget::captureEncodingsFromIndexed()
{
    const auto* store = m_model->indexedOrNull();
    if (!store)
        return;
    m_fileEncodings.clear();
    m_fileFormats.clear();
    for (const auto& lf : m_loadedFiles) {
        if (!lf)
            continue;
        const LineFormat format = store->lineFormat(lf);
        if (format.isDecided())
            m_fileFormats.insert(lf->filePath, format);
        const FileEncoding encoding = store->fileEncoding(lf);
        if (encoding.decided())
            m_fileEncodings.insert(lf->filePath, encoding);
    }
}

QString LogViewWidget::parserPattern() const
{
    return m_pattern.patternString();
}

QStringList LogViewWidget::parserFieldNames() const
{
    // Колонки вкладки (F-5): объединение колонок схем её файлов.
    return m_fieldLayout.columns;
}

void LogViewWidget::addLogFile(const QString &filePath)
{
    addLogFile(filePath, FileEncoding());
}

void LogViewWidget::addLogFile(const QString& filePath, const FileEncoding& initial,
                               const FieldSettings& schema)
{
    // Схема ждёт файл по ключу источника: обычный файл, .gz (путь архива) —
    // их путь; член архива (каталог сканера) — ArchiveRegistry ниже.
    QString key = RecordLocator::pathKey(normalizedPath(filePath));
    if (const auto member = ArchiveRegistry::instance().locate(normalizedPath(filePath)))
        key = RecordLocator::pathKey(member->archivePath) + QChar(0x01) + member->member;
    m_pendingFileSchemas.insert(key, schema);
    addLogFile(filePath, initial);
    // Файл уже был во вкладке (или не открылся) — схема не ждёт дальше.
    m_pendingFileSchemas.remove(key);
}

void LogViewWidget::addFilesFrom(const LogViewWidget& other)
{
    // Файлы переезжают со своим состоянием — память файла здесь не читается
    // (её закладки уже в other, а свежие правки ещё не записаны).
    const bool wasApplying = std::exchange(m_memoryApplying, true);
    // И «база» объединения памяти (что other прочитал из памяти или записал):
    // иначе запись этой вкладки вернула бы закладки, снятые после переноса.
    for (auto it = other.m_memoryBase.cbegin(); it != other.m_memoryBase.cend(); ++it)
        if (!m_memoryBase.contains(it.key()))
            m_memoryBase.insert(it.key(), it.value());
    // Закладки — до файлов: строки найдутся по ходу загрузки. Источник —
    // идентичность по концу прочитанного в other: тот же файл в том же
    // процессе (и спул stdin — Ephemeral) проходит проверку.
    for (const BookmarkState& b : other.bookmarks().states())
        bookmarks().restore(b);
    // Схемы файлов (F-5, PR 3): схема файла в other — своя здесь, если
    // отличается от схемы этой вкладки. Файл получает её, когда появится
    // (adoptPendingSchema); уже открытый файл схемы не меняет.
    QStringList schemaKeys;
    for (const LogFilePtr& lf : other.loadedFiles()) {
        if (!lf)
            continue;
        const QString key = RecordLocator::sourceKey(*lf);
        m_pendingFileSchemas.insert(key, other.fileSchema(lf));
        schemaKeys << key;
    }
    for (const LogFilePtr& lf : other.loadedFiles()) {
        if (!lf)
            continue;
        // Формат строк (F-17, шаг 7) — к загрузке файла здесь.
        const LineFormat format = other.lineFormat(lf);
        if (format.isDecided()
            && std::none_of(m_loadedFiles.cbegin(), m_loadedFiles.cend(),
                            [&lf](const LogFilePtr& f) { return f && f->filePath == lf->filePath; }))
            m_pendingFormats.insert(lf->filePath, format);
        if (lf->source().kind == LogSource::Kind::Stream) {
            // Спул потока (F-7) делится: тот же файл, источник и SpoolItem.
            const bool present = std::any_of(m_loadedFiles.cbegin(), m_loadedFiles.cend(),
                [&lf](const LogFilePtr& f) { return f && samePath(f->filePath, lf->filePath); });
            if (!present)
                addStream(lf->spool, lf->displayName, lf->source().label, other.fileEncoding(lf));
            continue;
        }
        if (!isArchive(lf)) {
            addLogFile(lf->filePath, other.fileEncoding(lf));
            continue;
        }
        // Сжатый файл (F-2): распакованная копия делится (тот же SpoolItem) —
        // без повторной распаковки и без удаления копии из-под читателя.
        // Ещё не распакован — распаковывается здесь заново.
        const bool present = std::any_of(m_loadedFiles.cbegin(), m_loadedFiles.cend(),
            [&lf](const LogFilePtr& f) { return sameSource(f, lf->source().path, lf->source().member); });
        if (present)
            continue;
        if (!other.isUnpacked(lf)) {
            // Выбор пользователя, сделанный, пока копия пишется, — у
            // ArchiveState источника, а не в его хранилище.
            openSource(lf->source().path, lf->source().member, other.pendingArchiveEncoding(lf));
            continue;
        }
        auto copy = std::make_shared<LogFile>(lf->filePath);
        copy->displayName = lf->displayName;
        copy->lastModified = lf->lastModified;
        copy->setSource(lf->source());
        copy->spool = lf->spool;
        m_loadedFiles.append(copy);
        adoptPendingSchema(copy);
        ArchiveState& as = m_archives[copy->filePath];
        as.unpacked = true;
        as.archiveStamp = other.m_archives.value(lf->filePath).archiveStamp;
        startLoad(copy, other.fileEncoding(lf));
    }
    for (const QString& key : std::as_const(schemaKeys))
        m_pendingFileSchemas.remove(key);
    m_memoryApplying = wasApplying;
}

QString LogViewWidget::loadFailureMessage(const LogFilePtr& logFile) const
{
    if (!logFile)
        return tr("Failed to parse");
    if (isArchive(logFile)) {
        const auto as = m_archives.constFind(logFile->filePath);
        if (as != m_archives.constEnd() && !as->error.isEmpty())
            return tr("Cannot open %1: %2").arg(logFile->shortName(), as->error);
    }
    const auto st = m_fileReloadStates.constFind(logFile->filePath);
    if (st == m_fileReloadStates.constEnd() || !st->unsupported)
        return tr("Failed to parse: %1").arg(logFile->shortName());
    return tr("%1 is %2: a UTF-16/UTF-32 file can only be shown on its own tab. "
              "Open it in a separate tab (merged tabs and large files read UTF-8 "
              "and single-byte encodings only).")
        .arg(logFile->shortName(), st->unsupportedReason);
}

void LogViewWidget::addLogFile(const QString &requestedPath, const FileEncoding& initial)
{
    // Один файл — один LogFile во вкладке: ключ строки (RecordKey) и
    // LogEntry::operator< сравнивают файлы по указателю, а порядок — по
    // пути; два экземпляра одного файла (разное написание пути) дали бы
    // дубли строк и промахи rowForKey. Путь приводится к абсолютному виду.
    // Сжатый файл сравнивается по источнику (архиву), а не по копии (F-2).
    const QString filePath = normalizedPath(requestedPath);
    // Файл извлечённого в сканере архива (F-2, шаг 4) — член архива: в
    // недавние, память и сессии идёт архив и имя, а не временный путь.
    if (const auto member = ArchiveRegistry::instance().locate(filePath)) {
        addArchiveMember(member->archivePath, member->member, initial);
        return;
    }
    for (const auto &file : m_loadedFiles) {
        if (sameSource(file, filePath)) {
            reopenLoaded(file, initial);
            return;
        }
    }

    // Архив с набором файлов (zip, tar, `.tar.gz`) вкладкой не читается:
    // двоичные байты вместо строк. Его открывает Directory Scanner (окно
    // направляет туда). Проверка — до gzip: `.tar.gz` тоже gzip.
    if (!SpoolDir::isStreamFile(filePath) && ArchiveFormats::isMultiFileArchive(filePath)) {
        emit statusMessage(tr("%1 is an archive: open it in the Directory Scanner to see its files.")
                               .arg(QFileInfo(filePath).fileName()));
        return;
    }
    // Сжатый источник (F-2): по сигнатуре, а не по имени. Память файла —
    // после распаковки (continueArchive).
    if (!SpoolDir::isStreamFile(filePath) && GzipDevice::fileHasSignature(filePath)) {
        addArchive(filePath, initial);
        return;
    }

    // Память файла (R-4): до старта загрузки — закладки и позиция ищутся по
    // ходу загрузки, фильтры стоят в модели до первой порции.
    if (!m_memoryApplying && fileMemoryActive() && applyFileMemory(filePath, initial))
        return;

    auto logFile = std::make_shared<LogFile>(filePath);
    m_loadedFiles.append(logFile);
    adoptPendingSchema(logFile);
    startLoad(logFile, initial);
}

// ---------------------------------------------------------------------------
// Потоки: stdin и вывод команды (F-7)
// ---------------------------------------------------------------------------

void LogViewWidget::addStream(const std::shared_ptr<SpoolItem>& spool, const QString& name,
                              const QString& label, const FileEncoding& initial)
{
    if (!spool)
        return;
    const QString filePath = normalizedPath(spool->path());
    for (const auto& file : m_loadedFiles) {
        if (file && samePath(file->filePath, filePath)) {
            reopenLoaded(file, initial);
            return;
        }
    }
    auto logFile = std::make_shared<LogFile>(filePath);
    logFile->displayName = name;
    LogSource source;
    source.kind = LogSource::Kind::Stream;
    source.label = label;
    logFile->setSource(source);
    logFile->spool = spool;
    m_loadedFiles.append(logFile);
    adoptPendingSchema(logFile);
    startLoad(logFile, initial);
}

void LogViewWidget::addCommandOutput(CommandSpooler* spooler, const FileEncoding& initial)
{
    if (!spooler || m_commands.contains(spooler))
        return;
    spooler->setParent(this);
    if (spooler->state() == CommandSpooler::State::NotStarted)
        spooler->start();
    // Не запустилась (каталог, строка) — файла может не быть: вкладка всё
    // равно открывается пустой, причина — на плашке.
    if (!QFileInfo::exists(spooler->spoolFilePath())) {
        QFile empty(spooler->spoolFilePath());
        if (empty.open(QIODevice::WriteOnly))
            empty.close();
    }
    attachCommand(spooler);
    addStream(spooler->spool(), spooler->spec().shortLabel(), spooler->spec().commandLine, initial);
    // Живой поток: авто-обновление + автопрокрутка к новым строкам.
    setAutoReload(true);
    if (m_view)
        m_view->setFollowTail(true);
}

void LogViewWidget::attachCommand(CommandSpooler* spooler)
{
    m_commands.append(spooler);
    // Пинки спулера — сверх таймера авто-обновления: хвост подтягивается
    // живо (спулер сигналит не чаще ~4 раз в секунду).
    connect(spooler, &CommandSpooler::bytesAppended, this, &LogViewWidget::onStreamGrew);
    connect(spooler, &CommandSpooler::stateChanged, this, [this]() { emit commandStateChanged(); });
    connect(spooler, &QObject::destroyed, this, [this, spooler]() { m_commands.removeAll(spooler); });
    auto* bar = new CommandStatusBar(spooler, m_commandBars);
    connect(bar, &CommandStatusBar::restartRequested, this,
            [this](CommandSpooler* s) { restartCommand(s); });
    m_commandBars->layout()->addWidget(bar);
    m_commandBars->show();
    emit commandStateChanged();
}

QList<CommandSpooler*> LogViewWidget::commands() const
{
    return m_commands;
}

bool LogViewWidget::restartCommand(CommandSpooler* spooler)
{
    if (!spooler || !m_commands.contains(spooler) || spooler->isRunning())
        return false;
    if (!spooler->start())
        return false;
    if (m_view)
        m_view->setFollowTail(true);
    return true;
}

void LogViewWidget::adoptCommandsFrom(LogViewWidget& other)
{
    const QList<CommandSpooler*> moved = other.m_commands;
    other.m_commands.clear();
    for (CommandSpooler* spooler : moved) {
        spooler->disconnect(&other);
        // Плашка уходит с вкладкой-источником; здесь — своя.
        const auto bars = other.m_commandBars->findChildren<CommandStatusBar*>();
        for (CommandStatusBar* bar : bars)
            if (bar->spooler() == spooler)
                bar->deleteLater();
        spooler->setParent(this);
        attachCommand(spooler);
    }
    if (!moved.isEmpty())
        setAutoReload(true);
}

void LogViewWidget::reopenLoaded(const LogFilePtr& file, const FileEncoding& initial)
{
    // Сжатый файл распакован и ждёт решения памяти файла: её
    // applyState / addLogFile приходит сюда — теперь грузим.
    if (isArchive(file)) {
        ArchiveState& as = m_archives[file->filePath];
        if (as.awaitingLoad) {
            as.awaitingLoad = false;
            startLoad(file, initial);
            return;
        }
        // Копия ещё пишется: выбор кодировки — к её загрузке, а не
        // перечитыванием недописанного файла.
        if (isArchivePending(file)) {
            if (initial.source == FileEncoding::Source::User)
                setPendingArchiveEncoding(file, initial);
            return;
        }
    }
    // Файл уже загружен или в процессе загрузки: --encoding для него —
    // обычный выбор пользователя.
    if (initial.source == FileEncoding::Source::User)
        setFileEncoding(file, initial.decoder.encoding());
}

void LogViewWidget::addArchiveMember(const QString& archivePath, const QString& member,
                                     const FileEncoding& initial)
{
    if (member.isEmpty()) {
        addLogFile(archivePath, initial);
        return;
    }
    const QString path = normalizedPath(archivePath);
    for (const auto& file : m_loadedFiles) {
        if (sameSource(file, path, member)) {
            reopenLoaded(file, initial);
            return;
        }
    }
    addArchive(path, initial, member);
}

void LogViewWidget::openSource(const QString& path, const QString& member, const FileEncoding& initial)
{
    if (member.isEmpty())
        addLogFile(path, initial);
    else
        addArchiveMember(path, member, initial);
}

void LogViewWidget::startLoad(const LogFilePtr& logFile, const FileEncoding& initial)
{
    const QString filePath = logFile->filePath;
    if (isArchive(logFile)) {
        ArchiveState& as = m_archives[filePath];
        as.loadStarted = true;
        as.awaitingLoad = false;
        // Скрытый файл снимка ждал распаковки (U-23).
        if (m_hiddenArchiveSources.remove(RecordLocator::sourceKey(*logFile))) {
            QSet<QString> hidden = m_model->hiddenFiles();
            hidden.insert(filePath);
            m_model->setHiddenFiles(hidden);
        }
    }

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
            // Вкладка могла уже держать резидентные файлы — перечитываем все,
            // с уже решёнными кодировками (F-1): решение, а тем более выбор
            // пользователя, на пороге не теряется.
            QHash<QString, FileEncoding> encodings = takeEncodingsForIndexed();
            if (initial.decided())
                encodings.insert(filePath, initial);
            for (const auto& lf : m_loadedFiles) {
                // Сжатый файл, который ещё распаковывается (или не
                // распаковался), не читается: копия не готова (F-2). Его
                // загрузит конец распаковки — уже в индексную вкладку.
                if (isArchivePending(lf)) {
                    // Выбор пользователя — к загрузке копии (решение
                    // автоматики относилось к прежнему содержимому).
                    // Ещё не начатый файл хранит свой initial как есть.
                    ArchiveState& pending = m_archives[lf->filePath];
                    if (pending.loadStarted) {
                        const FileEncoding chosen = encodings.value(lf->filePath);
                        pending.initial = chosen.source == FileEncoding::Source::User ? chosen
                                                                                        : FileEncoding();
                    }
                    continue;
                }
                startIndexedLoad(lf, encodings.value(lf->filePath));
            }
            qDebug() << "LogViewWidget: Indexed backend enabled for tab,"
                     << m_loadedFiles.size() << "file(s)";
            return;
        }
        startIndexedLoad(logFile, initial);
        qDebug() << "LogViewWidget: Started indexing for" << filePath;
        return;
    }

    // Запускаем асинхронный парсинг. Состояние заводим здесь: пока полная
    // загрузка в полёте, тик авто-обновления не должен запустить вторую.
    FileReloadState& st = m_fileReloadStates[filePath];
    st = FileReloadState{};
    st.loadInFlight = true;
    if (initial.decided())
        m_fileEncodings.insert(filePath, initial);
    m_logParser->startParsing(logFile, m_fileEncodings.value(filePath), passFormat(logFile));
    updateLoadingState();
    // UI может показать сообщение "Загрузка файла..." или индикатор прогресса
    qDebug() << "LogViewWidget: Started parsing for" << filePath;
}

// ---------------------------------------------------------------------------
// Сжатые источники (F-2)
// ---------------------------------------------------------------------------

void LogViewWidget::setSpoolRoot(const QString& root)
{
    if (root == m_spoolRoot)
        return;
    m_spoolRoot = root;
    m_spoolDir.reset(); // уже распакованные держат свой каталог сами
}

QString LogViewWidget::spoolRoot() const
{
    return m_spoolRoot.isEmpty() ? AppSettings::spoolRoot() : m_spoolRoot;
}

bool LogViewWidget::isUnpacking() const
{
    for (const ArchiveState& as : m_archives)
        if (as.running)
            return true;
    return false;
}

void LogViewWidget::cancelUnpacking()
{
    for (const ArchiveState& as : std::as_const(m_archives))
        if (as.running && as.unpacker)
            as.unpacker->cancel();
}

bool LogViewWidget::isUnpacked(const LogFilePtr& logFile) const
{
    if (!isArchive(logFile))
        return false;
    const auto it = m_archives.constFind(logFile->filePath);
    return it != m_archives.constEnd() && it->unpacked;
}

bool LogViewWidget::isArchivePending(const LogFilePtr& logFile) const
{
    if (!isArchive(logFile))
        return false;
    const auto it = m_archives.constFind(logFile->filePath);
    // Перераспаковка по F5 (loadStarted, но копия пишется заново) — тоже:
    // недописанную копию не читает ни конвертация, ни снимок.
    return it == m_archives.constEnd() || !it->loadStarted || it->running || !it->unpacked;
}

FileEncoding LogViewWidget::pendingArchiveEncoding(const LogFilePtr& logFile) const
{
    // Загрузка копии ещё не начиналась — выбор лежит только в ArchiveState;
    // перераспаковка (F5) — в хранилище вкладки, как у загруженного файла.
    // Вкладка стала индексной посреди перераспаковки — хранилище файла ещё
    // не знает, выбор отложен туда же.
    if (isArchivePending(logFile)) {
        const auto it = m_archives.constFind(logFile->filePath);
        if (it == m_archives.constEnd())
            return {};
        const auto* store = m_model->indexedOrNull();
        if (!it->loadStarted || (store && !store->indexForFile(logFile->filePath)))
            return it->initial;
    }
    return fileEncoding(logFile);
}

void LogViewWidget::setPendingArchiveEncoding(const LogFilePtr& logFile, const FileEncoding& encoding)
{
    // Копия ещё пишется: выбор — к её загрузке, без перечитывания
    // недописанного файла (F-2). После F5 строки файла уже сброшены, поэтому
    // выбор кладётся и туда, откуда его возьмёт полная перезагрузка: в
    // индексное хранилище (пустой индекс — без перечитывания) или в
    // m_fileEncodings резидентного пути.
    ArchiveState& as = m_archives[logFile->filePath];
    as.initial = encoding;
    if (as.loadStarted) {
        auto* store = m_model->indexedOrNull();
        if (store && store->indexForFile(logFile->filePath))
            store->setFileEncodingState(logFile, encoding);
        else if (!store)
            m_fileEncodings[logFile->filePath] = encoding;
    }
    emit fileEncodingChanged(logFile);
}

bool LogViewWidget::sameSource(const LogFilePtr& logFile, const QString& path, const QString& member)
{
    if (!logFile)
        return false;
    if (!isArchive(logFile))
        return member.isEmpty() && samePath(logFile->filePath, path);
    return logFile->source().member == member && samePath(logFile->source().path, path);
}

void LogViewWidget::addArchive(const QString& archivePath, const FileEncoding& initial,
                               const QString& member)
{
    QString error;
    if (!m_spoolDir)
        m_spoolDir = SpoolDir::shared(spoolRoot(), &error);
    // Имя вкладки — архива (.gz) или члена (zip): `app.log.gz`, `app.log`.
    const QString archiveName = member.isEmpty() ? QFileInfo(archivePath).fileName()
                                                 : QFileInfo(member).fileName();
    if (!m_spoolDir) {
        emit statusMessage(tr("Cannot unpack %1: no place for temporary files (%2).")
                               .arg(archiveName, error));
        return;
    }
    // Имя копии — узнаваемое: app.log.gz → app.log.
    QString name = archiveName;
    if (name.endsWith(QLatin1String(".gz"), Qt::CaseInsensitive) && name.size() > 3)
        name.chop(3);
    const std::shared_ptr<SpoolItem> item = m_spoolDir->createFile(name);
    auto logFile = std::make_shared<LogFile>(item->path());
    logFile->displayName = archiveName;
    LogSource source;
    source.kind = LogSource::Kind::Archive;
    source.path = archivePath;
    source.member = member;
    logFile->setSource(source);
    logFile->spool = item;
    m_loadedFiles.append(logFile);
    adoptPendingSchema(logFile);

    ArchiveState& as = m_archives[logFile->filePath];
    as.initial = initial;
    if (const auto it = m_stateIdentities.constFind(RecordLocator::sourceKey(*logFile));
        it != m_stateIdentities.constEnd())
        as.identity = *it;
    as.applyingState = m_applyingState;
    as.memoryApplying = m_memoryApplying;
    startUnpack(logFile);
}

void LogViewWidget::startUnpack(const LogFilePtr& logFile)
{
    ArchiveState& as = m_archives[logFile->filePath];
    if (!as.unpacker) {
        as.unpacker = new SourceUnpacker(this);
        const QString key = logFile->filePath;
        const std::weak_ptr<LogFile> weak = logFile;
        connect(as.unpacker, &SourceUnpacker::progress, this,
                [this, weak](qint64 done, qint64 total) {
            if (const LogFilePtr lf = weak.lock())
                emit unpackingProgress(lf, total > 0 ? int(qMin<qint64>(100, done * 100 / total)) : 0);
        });
        connect(as.unpacker, &SourceUnpacker::finished, this,
                [this, weak](const SourceUnpacker::Result& r) {
            if (const LogFilePtr lf = weak.lock())
                handleUnpackFinished(lf, r.job, r.ok(), r.error == SourceUnpacker::Error::Cancelled
                                     ? tr("unpacking was cancelled") : r.message);
        });
    }
    as.archiveStamp = DiskStamp::of(logFile->source().path);
    as.unpacked = false;
    as.error.clear();
    // .gz — распаковать; член zip или tar (F-2, шаги 4–5) — извлечь
    // (член-gzip — распакованным).
    const auto run = [&as, &logFile]() {
        const LogSource& src = logFile->source();
        return src.member.isEmpty() ? as.unpacker->unpackGzip(src.path, logFile->filePath)
                                    : as.unpacker->unpackArchiveMember(src.path, src.member, logFile->filePath);
    };
    as.job = run();
    if (as.job == 0) {
        // Прежнее задание ещё не отпустило воркер (отмена посреди F5).
        as.unpacker->cancel();
        as.unpacker->waitForFinished();
        as.job = run();
    }
    as.running = as.job != 0;
    updateLoadingState();
    emit unpackingStarted(logFile);
}

void LogViewWidget::handleUnpackFinished(const LogFilePtr& logFile, quint64 job, bool ok,
                                         const QString& message)
{
    const auto it = m_archives.find(logFile->filePath);
    if (it == m_archives.end() || it->job != job || !m_loadedFiles.contains(logFile))
        return; // прежнее задание (перезапуск) или файл уже не во вкладке
    ArchiveState& as = it.value();
    as.running = false;
    if (!ok) {
        as.error = message;
        updateLoadingState();
        emit unpackingFinished(logFile, false);
        emit fileParsingFailed(logFile);
        return;
    }
    as.unpacked = true;
    // Время копии — архива: опорная дата года syslog (LineClassifier::
    // yearReferenceForFile) — когда лог писали, а не когда распаковали.
    const QFileInfo archive(logFile->source().path);
    if (archive.lastModified().isValid()) {
        QFile copy(logFile->filePath);
        if (copy.open(QIODevice::ReadWrite))
            copy.setFileTime(archive.lastModified(), QFileDevice::FileModificationTime);
    }
    const QFileInfo info(logFile->filePath);
    logFile->fileSize = info.size();
    logFile->lastModified = archive.lastModified();
    as.identity.reset();
    emit unpackingFinished(logFile, true);
    if (as.loadStarted) {
        // F5 по изменившемуся архиву: строки уже сброшены — читаем заново.
        // Серые закладки (источник был другим) проверяются снова.
        bookmarks().recheckSource(RecordLocator::sourceKey(*logFile));
        const auto* store = m_model->indexedOrNull();
        if (store && !store->indexForFile(logFile->filePath)) {
            // Пока копия писалась, вкладка стала индексной и файл пропустила;
            // выбор пользователя она отложила в as.initial.
            startLoad(logFile, as.initial.source == FileEncoding::Source::User ? as.initial
                                                                             : FileEncoding());
            return;
        }
        if (!m_fileReloadStates.contains(logFile->filePath))
            m_fileReloadStates.insert(logFile->filePath, FileReloadState{});
        startFullReload(logFile);
        return;
    }
    continueArchive(logFile);
}

void LogViewWidget::continueArchive(const LogFilePtr& logFile)
{
    ArchiveState& as = m_archives[logFile->filePath];
    const FileEncoding initial = as.initial;
    as.awaitingLoad = true;
    // Флаги addLogFile — как в момент вызова (снимок, перенос вкладки).
    const bool wasApplying = std::exchange(m_applyingState, as.applyingState);
    const bool wasMemory = std::exchange(m_memoryApplying, as.memoryApplying);
    if (!m_memoryApplying && fileMemoryActive())
        applyFileMemory(logFile->source().path, initial, logFile);
    m_applyingState = wasApplying;
    m_memoryApplying = wasMemory;
    // Память не нашлась (или не открыла файл сама) — обычная загрузка.
    if (m_archives.value(logFile->filePath).awaitingLoad)
        startLoad(logFile, initial);
}

bool LogViewWidget::reloadArchive(const LogFilePtr& logFile)
{
    ArchiveState& as = m_archives[logFile->filePath];
    if (as.running || m_fileReloadStates.value(logFile->filePath).loadInFlight)
        return false;
    const QString archivePath = logFile->source().path;
    const DiskStamp stamp = DiskStamp::of(archivePath);
    const bool signature = logFile->source().member.isEmpty() ? GzipDevice::fileHasSignature(archivePath)
                                                              : ArchiveFormats::detectKnown(archivePath) != ArchiveFormats::Format::None;
    if (!stamp.exists() || !signature) {
        // Копия самодостаточна: строки остаются. Копии нет (распаковка не
        // удалась или отменена) — показывать нечего.
        emit statusMessage(as.unpacked
            ? tr("%1: the archive was not found; the unpacked copy stays open.")
                  .arg(QDir::toNativeSeparators(archivePath))
            : tr("%1: the archive was not found; the file is not loaded.")
                  .arg(QDir::toNativeSeparators(archivePath)));
        return false;
    }
    if (as.unpacked && stamp == as.archiveStamp)
        return false; // архив не менялся
    // Перед распаковкой в тот же путь строки сброшены: индексный бэкенд
    // читает текст копии с диска по требованию.
    if (as.loadStarted && as.unpacked) {
        // Идентичность прежнего содержимого — для снимка, пока копия пишется.
        as.identity = m_model->recordLocator().sourceFor(logFile);
        dropRowsForFile(logFile);
    }
    startUnpack(logFile);
    return true;
}

QString LogViewWidget::normalizedPath(const QString& path)
{
    if (path.isEmpty())
        return path;
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

bool LogViewWidget::samePath(const QString& a, const QString& b)
{
    // Как у сохранённых путей (R-8): путь Windows из сессии с другой ОС — без
    // учёта регистра и на Linux; локальные пути — как раньше.
    return SavedPath::same(a, b);
}

void LogViewWidget::locatorFileLoaded(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    RecordLocator& locator = m_model->recordLocator();
    if (const auto* store = m_model->indexedOrNull()) {
        const auto index = store->indexForFile(logFile->filePath);
        locator.fileLoaded(logFile, index ? index->endOffset() : -1);
        return;
    }
    const FileReloadState st = m_fileReloadStates.value(logFile->filePath);
    const qint64 provisional = st.lastLinePartial && st.lastEntry
        ? st.lastEntry->originalLineNumber() : 0;
    locator.fileLoaded(logFile, st.endOffset, provisional);
}

void LogViewWidget::updateLoadingState()
{
    retryStreamKick();
    bool loading = m_model->recordLocator().isBusy() || isUnpacking();
    for (auto it = m_fileReloadStates.constBegin();
         it != m_fileReloadStates.constEnd(); ++it) {
        if (it->loadInFlight) {
            loading = true;
            break;
        }
    }
    if (loading == m_loading)
        return;
    m_loading = loading;
    emit loadingChanged(m_loading);
    if (!m_loading) {
        retryPendingPosition();
        if (!m_model->isFiltering())
            m_view->settleAwaitedAnchor();
    }
    if (!m_loading && m_reconsiderPending)
        reconsiderProvisional(); // запасную сменили посреди загрузки
}

bool LogViewWidget::anyLoadInFlight() const
{
    for (const FileReloadState& st : m_fileReloadStates)
        if (st.loadInFlight)
            return true;
    return false;
}

void LogViewWidget::onStreamGrew()
{
    // Пинок потока (F-7), пока файл дочитывается: тик по файлу в полёте
    // пропускается, а следующего пинка может не быть (процесс вышел) — без
    // таймера окна хвост остался бы непрочитанным. Повтор — по концу загрузки.
    if (anyLoadInFlight()) {
        m_streamKickPending = true;
        return;
    }
    m_streamKickPending = false;
    reloadChangedFiles();
}

void LogViewWidget::retryStreamKick()
{
    if (m_streamKickPending && !anyLoadInFlight())
        QTimer::singleShot(0, this, &LogViewWidget::onStreamGrew);
}

LogViewWidget::DiskStamp LogViewWidget::DiskStamp::of(const QString& filePath)
{
    DiskStamp stamp;
    const QFileInfo info(filePath);
    if (info.exists()) {
        stamp.size = info.size();
        stamp.mtimeMs = info.lastModified().toMSecsSinceEpoch();
    }
    return stamp;
}

void LogViewWidget::dropRowsForFile(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    // Закладки, история и локатор узнают о сбросе строк файла один раз:
    // резидентный removeEntriesForFile сообщает им сам.
    if (auto* store = m_model->indexedOrNull()) {
        m_model->invalidateFileRows(logFile->filePath);
        store->resetFileIndex(logFile, std::make_shared<LineIndex>());
    } else {
        m_model->removeEntriesForFile(logFile->filePath);
    }
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::startFullReload(const LogFilePtr& logFile, bool keepFormat)
{
    if (!logFile)
        return;
    // Ключ уже есть в хэше у всех вызывающих — вставки (и рехэша, который
    // инвалидировал бы их ссылку на состояние) здесь не происходит.
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    // Содержимое новое — решение автоматики о формате строк к нему не
    // относится (F-17, шаг 7); выбор пользователя остаётся. Перечитывание
    // ради формата (rereadForSchema) формат сохраняет.
    if (!keepFormat)
        forgetAutoFormat(logFile);

    if (auto* store = m_model->indexedOrNull()) {
        // Строки файла сбрасываются: закладки и история переходов найдут их
        // заново (RecordLocator), когда строки покажутся. Резидентный
        // removeEntriesForFile ниже сообщает об этом сам.
        m_model->invalidateFileRows(logFile->filePath);
        // Строки файла сбрасываются, индексация идёт в свежий LineIndex
        // (store сам чистит кэш текста и refs).
        ensureIndexer();
        auto fresh = std::make_shared<LineIndex>();
        store->resetFileIndex(logFile, fresh);
        st = FileReloadState{};
        st.loadInFlight = true;
        // resetFileIndex вернул автоматике «не решено», выбор пользователя
        // оставил — с ним и перечитываем.
        m_indexer->startIndexing(logFile, fresh, store->fileEncoding(logFile),
                                 passFormat(logFile));
        updateLoadingState();
        emit fileEncodingChanged(logFile);
        return;
    }

    // Резидентный бэкенд: выбрасываем устаревшие записи этого файла и читаем
    // его с нуля. Записи остальных файлов вкладки не трогаем.
    m_model->removeEntriesForFile(logFile->filePath);
    st = FileReloadState{};
    st.loadInFlight = true;
    // Содержимое новое — решение автоматики к нему не относится; выбор
    // пользователя остаётся (с ним и перечитываем после setFileEncoding).
    forgetAutoEncoding(logFile->filePath);
    m_logParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath),
                              passFormat(logFile));
    updateLoadingState();
    emit fileEncodingChanged(logFile);
}

void LogViewWidget::ensureIndexer()
{
    if (m_indexer)
        return;
    m_indexer = new LogIndexer(this);
    m_indexer->setFallbackEncoding(m_fallbackEncoding);
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
    // Решение о кодировке приходит раньше порции с первой не-ASCII строкой
    // (очередь сигналов воркера сохраняет порядок): стор читает её уже новым
    // декодером. Первое решение — без reset (прежние строки — ASCII);
    // пересмотр предварительного (шаг 3а) — reset и при текстовых правилах
    // перефильтрация, как ручная смена (IndexedLogStore::adoptDetectedEncoding).
    connect(m_indexer, &LogIndexer::lineFormatDecided, this, &LogViewWidget::handleLineFormatDecided);
    connect(m_indexer, &LogIndexer::encodingDecided, this,
            [this](const LogFilePtr& logFile, const FileEncoding& encoding) {
                adoptIndexedEncoding(logFile, encoding, /*wholeState=*/false);
            });
}

void LogViewWidget::startIndexedLoad(const LogFilePtr& logFile, FileEncoding encoding)
{
    ensureIndexer();
    auto* store = m_model->indexedOrNull();
    if (!store || !logFile)
        return;
    auto index = std::make_shared<LineIndex>();
    store->attachFile(logFile, index);
    // Кодировка, решённая резидентным путём до перехода порога: стор читает
    // текст ею с первой строки, индексатор не решает заново — окончательное
    // решение EncodingDetector::advance не трогает (BOM UTF-8 — то же
    // решение), предварительное (шаг 3а) продолжает копить свидетельство с
    // того места, где остановился резидентный путь, и может быть пересмотрено.
    // Выбор пользователя (--encoding, объединение вкладок, порог) — тоже
    // состояние целиком: файл только подключён, reset не нужен.
    if (encoding.decided())
        store->setFileEncodingState(logFile, encoding);
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st = FileReloadState{};
    st.loadInFlight = true;
    m_indexer->startIndexing(logFile, index, encoding, passFormat(logFile));
    updateLoadingState();
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
    // Строки показаны модели — локатор сверяет ожидающие идентификаторы
    // этого диапазона (O(log K + найденные), диапазон не просматривается).
    m_model->recordLocator().indexedRange(logFile, firstLine, count);
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
    st.loadInFlight = false;
    st.failedStamp = DiskStamp{};
    locatorFileLoaded(logFile);
    updateLoadingState();

    bookmarksFileLoaded(logFile, wasInitial);
    if (wasInitial)
        emit fileParsingFinished(logFile,
                                 int(qMin<qint64>(newLines, INT_MAX)));
    else
        emit reloadFinished(int(qMin<qint64>(newLines, INT_MAX)));
    // Схему сменили посреди прохода — переиндексировать новой.
    startPendingReread(logFile);
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleIndexingFailed(const LogFilePtr& logFile)
{
    if (logFile) {
        // Индекс может остаться частичным — файл считается незагруженным и
        // будет прочитан заново, как только изменится на диске.
        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        st.loadInFlight = false;
        st.initialLoadDone = false;
        // Следующая загрузка — с нуля и уже текущей схемой.
        st.schemaRereadPending = false;
        st.failedStamp = DiskStamp::of(logFile->filePath);
        updateLoadingState();
    }
    emit fileParsingFailed(logFile);
}

void LogViewWidget::handleResidentFallback(const LogFilePtr& logFile,
                                           const QString& reason)
{
    if (!logFile)
        return;
    qWarning() << "LogViewWidget: indexed backend fallback for"
               << logFile->filePath << "-" << reason;
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.loadInFlight = false;

    // Индексный путь понимает только UTF-8: файл ведём через резидентный
    // парсер. Смешивать бэкенды в одной вкладке нельзя — откат возможен,
    // только пока этот файл в ней единственный.
    if (m_loadedFiles.size() == 1) {
        // Решения стора (в том числе выбор пользователя) переходят к
        // резидентному пути вместе с файлом.
        captureEncodingsFromIndexed();
        m_model->convertToResidentBackend();
        m_residentOnlyFiles.insert(logFile->filePath);
        st = FileReloadState{};
        st.loadInFlight = true;
        m_logParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath),
                                  passFormat(logFile));
        return;
    }
    // Отката нет: помечаем файл неподдерживаемым, чтобы авто-обновление не
    // переиндексировало его впустую на каждое изменение.
    st.initialLoadDone = false;
    st.unsupported = true;
    st.unsupportedReason = reason.section(QLatin1Char(' '), 0, 0); // "UTF-16 BOM" → "UTF-16"
    // Кодировка по BOM — для индикатора (подпись без меню) и чтобы выбор
    // кодировки для такого файла игнорировался.
    QFile head(logFile->filePath);
    if (head.open(QIODevice::ReadOnly))
        if (const auto bom = TextEncodings::detectBom(head.read(4)))
            st.encoding = bom->encoding;
    updateLoadingState();
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
        FileReloadState& st = m_fileReloadStates[parsedLogFile->filePath];
        for (const auto& e : sortedBatch)
            if (e) st.nextLogicalEntryId = qMax(st.nextLogicalEntryId, e->logicalEntryId() + 1);
        rememberLastLines(st, sortedBatch);
    }

    // Слияние в модель без reset: выделение и позиция скролла сохраняются,
    // стоимость батча O(B) при загрузке одного файла (append в конец) вместо
    // прежней полной пересборки O(N) на каждый батч.
    m_model->mergeEntries(sortedBatch);
    // Порция в порядке файла — локатору (строки уже показаны модели). После
    // конверсии во индексный бэкенд модель такие порции отбрасывает.
    if (!m_model->isIndexedBackend())
        m_model->recordLocator().residentBatch(entriesBatch);
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
        // Якорь — там, где парсер действительно остановился, а не текущий
        // размер: дописанное после его конца иначе было бы пропущено.
        const qint64 consumed = st.endOffset >= 0
            ? st.endOffset : QFileInfo(parsedLogFile->filePath).size();
        st.anchor          = FileChangeDetector::capture(parsedLogFile->filePath, consumed);
        st.initialLoadDone = true;
        st.loadInFlight    = false;
        st.failedStamp     = DiskStamp{};
        locatorFileLoaded(parsedLogFile);
        updateLoadingState();
        bookmarksFileLoaded(parsedLogFile, true);
        // Кодировку или схему сменили посреди загрузки — перечитываем.
        startPendingReread(parsedLogFile);
    }

    emit fileParsingFinished(parsedLogFile, totalEntries);
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::handleParsingFailed(const LogFilePtr& parsedLogFile)
{
    if (!parsedLogFile)
        return;
    qWarning() << "LogViewWidget: Failed parsing for" << parsedLogFile->filePath;

    // Файл не открылся (удалён, заблокирован писателем, нет прав). Снимаем
    // «загрузку в полёте» и запоминаем подпись файла: без этого он остался бы
    // навсегда незагруженным и F5 по нему больше ничего бы не делал. Повтор
    // произойдёт, когда файл на диске изменится (в частности — появится снова).
    FileReloadState& st = m_fileReloadStates[parsedLogFile->filePath];
    st.loadInFlight = false;
    st.initialLoadDone = false;
    st.failedStamp = DiskStamp::of(parsedLogFile->filePath);
    updateLoadingState();
    emit fileParsingFailed(parsedLogFile);
}

void LogViewWidget::rememberLastLines(FileReloadState& st,
                                      const QVector<std::shared_ptr<LogEntry>>& batch)
{
    // Две строки файла с наибольшими номерами — в любом порядке батча
    // (начальные батчи отсортированы по времени, а не по строкам).
    for (const auto& entry : batch) {
        if (!entry)
            continue;
        const int line = entry->originalLineNumber();
        if (!st.lastEntry || line > st.lastEntry->originalLineNumber()) {
            st.prevEntry = st.lastEntry;
            st.lastEntry = entry;
        } else if (line != st.lastEntry->originalLineNumber()
                   && (!st.prevEntry || line > st.prevEntry->originalLineNumber())) {
            st.prevEntry = entry;
        }
    }
}

void LogViewWidget::handleTailState(const LogFilePtr& logFile, qint64 endOffset,
                                    bool lastLinePartial, qint64 lastLineStart, TextEncoding encoding)
{
    if (!logFile)
        return;
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.endOffset = endOffset;
    st.lastLinePartial = lastLinePartial;
    st.lastLineStart = lastLineStart;
    st.encoding = encoding;
}

void LogViewWidget::handleModelFilteredRelay(int totalRowsAfterFilter)
{
    // Перефильтрация после смены кодировки закончилась: строки, которые она
    // открыла, — с новым текстом; их фрагменты закладок и истории тоже.
    if (!m_previewRefreshFiles.isEmpty() && !m_model->isFiltering()) {
        const QSet<QString> files = std::exchange(m_previewRefreshFiles, {});
        for (const QString& path : files)
            refreshTextFragments(path);
    }
    emit modelFiltered(totalRowsAfterFilter);
    retryPendingPosition();
    if (!m_loading && !m_model->isFiltering())
        m_view->settleAwaitedAnchor();
}

bool LogViewWidget::reloadChangedFiles(bool force)
{
    // Растущий файл (типично — спул stdin) пересёк порог индексного бэкенда:
    // конвертируем вкладку, дальше хвост дочитывает индексатор. Конвертируем
    // только когда по файлу нет джоба в полёте — иначе резидентный парсер
    // дописал бы в состояние, которым уже владеет индексатор. Файл, который
    // индексатор уже вернул в резидентный путь (UTF-16), не конвертируем.
    if (!m_model->isIndexedBackend() && m_loadedFiles.size() == 1
        && m_loadedFiles[0] && !isArchive(m_loadedFiles[0]) // сжатый не растёт (F-2)
        && !m_residentOnlyFiles.contains(m_loadedFiles[0]->filePath)
        && !m_fileReloadStates.value(m_loadedFiles[0]->filePath).loadInFlight) {
        const qint64 size = QFileInfo(m_loadedFiles[0]->filePath).size();
        if (size >= AppSettings::instance().indexedThresholdBytes()) {
            auto* store = m_model->convertToIndexedBackend();
            store->setTextCacheBudget(
                qint64(AppSettings::instance().textCacheBudgetMB()) * 1024 * 1024);
            m_fileReloadStates.clear();
            const QHash<QString, FileEncoding> encodings = takeEncodingsForIndexed();
            startIndexedLoad(m_loadedFiles[0], encodings.value(m_loadedFiles[0]->filePath));
            return true;
        }
    }

    bool anyChanged = false;
    IndexedLogStore* indexedStore = m_model->indexedOrNull();

    for (const auto& logFile : m_loadedFiles) {
        if (!logFile) continue;

        // Сжатый источник (F-2): дозаписи у него не бывает, а молча
        // перераспаковывать гигабайты из-за ротации нельзя — тик его
        // пропускает, F5 распаковывает изменившийся архив заново.
        if (isArchive(logFile)) {
            if (force && reloadArchive(logFile))
                anyChanged = true;
            continue;
        }

        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        // Фоновая загрузка ещё идёт — второй джоб по тому же файлу запускать
        // нельзя (дубли записей / гонка за single-writer индексом).
        if (st.loadInFlight || st.unsupported) continue;

        const DiskStamp stamp = DiskStamp::of(logFile->filePath);

        if (!stamp.exists()) {
            // Файла нет на диске. Сбрасываем его строки (view пустеет) ровно
            // один раз и ждём, пока он появится снова: обнулённое состояние
            // хранит failedStamp «файла нет», и следующие тики сюда не зайдут.
            if (st.initialLoadDone || st.failedStamp.exists()) {
                dropRowsForFile(logFile);
                st = FileReloadState{};
                anyChanged = true;
            }
            continue;
        }

        if (!st.initialLoadDone) {
            // Файл есть, но не загружен: либо создан заново после удаления,
            // либо прошлая загрузка сорвалась. Сами по себе повторяем, только
            // когда на диске что-то изменилось (иначе перечитывали бы
            // недоступный файл на каждый тик); ручной F5 пробует всегда.
            if (!force && stamp == st.failedStamp) continue;
            if (m_residentOnlyFiles.remove(logFile->filePath))
                return reloadChangedFiles(force) || anyChanged; // см. Replaced
            startFullReload(logFile);
            anyChanged = true;
            continue;
        }

        switch (FileChangeDetector::check(logFile->filePath, st.anchor)) {
        case FileChangeDetector::Change::Unchanged:
            break;

        case FileChangeDetector::Change::Appended: {
            if (indexedStore) {
                // Дозапись через индексатор: смещение он берёт сам из
                // index->endOffset(); предварительный хвост без '\n'
                // переиндексируется от своего начала.
                if (auto index = indexedStore->indexForFile(logFile->filePath)) {
                    ensureIndexer();
                    st.loadInFlight = true;
                    // Кодировка файла (F-1): «не решено» переживает дозапись —
                    // индексатор продолжит проверять новые байты.
                    m_indexer->startIndexingFrom(logFile, index,
                                                 index->lastLineProvisional(),
                                                 indexedStore->fileEncoding(logFile),
                                                 passFormat(logFile));
                    // Re-anchor: следующий тик поллинга не должен снова
                    // классифицировать этот же диапазон как дозапись.
                    const qint64 newSize = QFileInfo(logFile->filePath).size();
                    st.anchor = FileChangeDetector::capture(logFile->filePath, newSize);
                    anyChanged = true;
                }
                break;
            }
            // Prefix is intact and the file grew — read only the new tail and
            // append it, preserving selection and scroll position. The tail
            // continues the file's last record and its line numbering.
            const auto contextAfter = [&st](const std::shared_ptr<LogEntry>& entry) {
                LogParser::ResumeContext context;
                context.nextLogicalEntryId = st.nextLogicalEntryId;
                if (entry) {
                    context.currentLogicalEntryId = entry->logicalEntryId();
                    context.currentTimestamp = entry->timestamp();
                    context.currentLevel = entry->level();
                    context.nextLineNumber = entry->originalLineNumber() + 1;
                    // Первая строка записи (F-17, шаг 3): продолжение в
                    // дозаписи решает правило по полю по ней.
                    context.currentHead = entry->recordHead()
                        ? entry->recordHead() : std::shared_ptr<const LogEntry>(entry);
                }
                return context;
            };
            LogParser::ResumeContext context = contextAfter(st.lastEntry);
            qint64 from = st.anchor.consumedBytes;
            if (st.lastLinePartial && st.lastEntry && st.lastLineStart >= 0) {
                // Последняя строка была без перевода строки и показана
                // предварительно — писатель её дописывает. Убираем её и читаем
                // заново с её начала: иначе дописанная часть стала бы
                // отдельной строкой.
                const std::shared_ptr<LogEntry> partial = st.lastEntry;
                const bool partialStartedRecord = !st.prevEntry
                    || st.prevEntry->logicalEntryId() != partial->logicalEntryId();
                context = contextAfter(partialStartedRecord ? st.prevEntry : partial);
                if (partialStartedRecord)
                    context.nextLogicalEntryId = partial->logicalEntryId();
                context.nextLineNumber = partial->originalLineNumber();
                m_model->removeEntry(partial);
                st.lastEntry = st.prevEntry;
                st.prevEntry.reset();
                from = st.lastLineStart;
            }
            st.lastLinePartial = false;
            st.loadInFlight = true;
            // Якорь сдвинется по концу прочитанного (tailState); повторный
            // опрос этот диапазон не возьмёт — по файлу загрузка в полёте.
            // Кодировка хвоста (F-1): поток — как читалось начало (UTF-16/32),
            // байты — состоянием файла; «не решено» решит не-ASCII строка хвоста.
            m_reloadParser->startParsingFrom(logFile, from, context, st.encoding,
                                             m_fileEncodings.value(logFile->filePath),
                                             passFormat(logFile));
            anyChanged = true;
            break;
        }

        case FileChangeDetector::Change::Replaced:
            // Файл обрезан или переписан другим содержимым (в том числе удалён
            // и создан заново): выбрасываем устаревшие строки и читаем заново.
            // Состояние восстановится в handle*Finished.
            // Содержимое новое — отметка «только резидентный» к нему не
            // относится: проверяем порог заново (сначала этой функции), а
            // UTF-16 индексатор вернёт обратно, прочитав только BOM.
            if (m_residentOnlyFiles.remove(logFile->filePath))
                return reloadChangedFiles(force) || anyChanged;
            startFullReload(logFile); // `st` переинициализирован — не переиспользовать
            anyChanged = true;
            break;
        }
    }
    updateLoadingState(); // ветки дозаписи выше могли взвести loadInFlight
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
    rememberLastLines(st, batch);

    // Append entries directly — no model reset, selection preserved.
    m_model->appendEntries(batch);
    if (!m_model->isIndexedBackend())
        m_model->recordLocator().residentBatch(batch);
}

void LogViewWidget::handleIncrementalParsingFinished(int newEntries, const LogFilePtr& logFile)
{
    if (logFile) {
        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        st.loadInFlight = false;
        // Якорь — ровно по концу прочитанного: дописанное во время чтения
        // не прочитается повторно и не потеряется.
        if (st.endOffset >= 0)
            st.anchor = FileChangeDetector::capture(logFile->filePath, st.endOffset);
        locatorFileLoaded(logFile);
        updateLoadingState();
        bookmarksFileLoaded(logFile, false);
        startPendingReread(logFile);
    }
    emit totalRowCountChanged(m_model->rowCount());
    QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
    emit reloadFinished(newEntries);
}

void LogViewWidget::handleIncrementalParsingFailed(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    // Хвост дочитать не удалось (файл исчез или заблокирован), а якорь уже
    // сдвинут на непрочитанные байты — доверять ему больше нельзя. Помечаем
    // файл незагруженным: следующая проверка перечитает его целиком (или
    // очистит, если файла нет).
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.loadInFlight = false;
    st.initialLoadDone = false;
    st.failedStamp = DiskStamp::of(logFile->filePath);
    updateLoadingState();
}

void LogViewWidget::handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile)
{
    // qDebug() << "LogViewWidget: Parsing progress for" << parsedLogFile->filePath << ":" << progressPercentage << "%";
}


void LogViewWidget::searchTextNext(const TextQuery& query)
{
    if (!m_model || query.isEmpty())
        return;
    m_quickSearchQuery = query;
    m_quickSearchForward = true;
    startQuickSearch();
}

void LogViewWidget::searchTextPrevious(const TextQuery& query)
{
    if (!m_model || query.isEmpty())
        return;
    m_quickSearchQuery = query;
    m_quickSearchForward = false;
    startQuickSearch();
}

void LogViewWidget::startQuickSearch()
{
    // Старт — от строки, выбранной пользователем (инвариант 10: текущую без
    // выделения мог назначить Qt по фокусу). Её скрыл фильтр — от её места;
    // выбора нет — вперёд с начала, назад с конца. Сюда же приходит перезапуск
    // после invalidated: view к этому моменту уже восстановил выделение (в
    // обработчике modelReset), а скрытая запись находится по якорю.
    const int currentRow = QuickSearch::startRowFor(*m_model, m_view->userSelectedRow(),
                                                    m_view->selectedRecordId(),
                                                    m_view->selectedRecordFile(),
                                                    m_quickSearchForward);
    m_quickSearch->start(m_model, m_quickSearchQuery, m_quickSearchForward, currentRow);
}

bool LogViewWidget::goToEntry(int logicalEntryId, const LogFile* sourceFile, NavKind kind)
{
    if (!m_model || !m_view)
        return false;
    const int row = m_model->nearestVisibleRow(logicalEntryId, sourceFile);
    if (row < 0)
        return false;
    m_view->goToRow(row, kind);
    return true;
}

bool LogViewWidget::navigateHistory(bool forward)
{
    // Фоновый F3 / F8, закончившись после возврата, увёл бы вид от места,
    // куда вернулись, и записал бы переход оттуда.
    cancelQuickSearch();
    cancelLevelJump();
    NavHistory& history = navHistory();
    const int current = m_view->userSelectedRow();
    const int row = forward ? history.forward(current) : history.back(current);
    if (row < 0)
        return false;
    m_view->goToRow(row, NavKind::History);
    return true;
}

void LogViewWidget::cancelQuickSearch()
{
    m_quickSearch->cancel();
}

bool LogViewWidget::isQuickSearchRunning() const
{
    return m_quickSearch->isRunning();
}

void LogViewWidget::goToLevel(LogLevelMask levels, bool forward)
{
    if (!m_model || !levels)
        return;
    m_levelJumpLevels = levels;
    m_levelJumpForward = forward;
    // Опорная строка — выбранная пользователем: текущую без выделения мог
    // назначить Qt при возврате фокуса (инвариант 10). Без неё вперёд ищем с
    // начала, назад — с конца.
    const int startRow = m_view->userSelectedRow();
    m_levelJump->cancel();
    if (m_model->rowCount() <= m_levelJumpSyncRows) {
        finishLevelJump(m_model->findRowWithLevel(levels, startRow, forward), startRow);
        return;
    }
    m_levelJump->startLevel(m_model, levels, forward, startRow);
}


void LogViewWidget::cancelLevelJump()
{
    m_levelJump->cancel();
}

void LogViewWidget::finishLevelJump(int row, int startRow)
{
    LevelJumpOutcome outcome = LevelJumpOutcome::NotFound;
    if (row >= 0 && row < m_model->rowCount()) {
        if (row == startRow)
            outcome = LevelJumpOutcome::OnlyCurrent;
        else if (startRow >= 0 && (m_levelJumpForward ? row < startRow : row > startRow))
            outcome = LevelJumpOutcome::Wrapped;
        else
            outcome = LevelJumpOutcome::Moved;
        m_view->goToRow(row, NavKind::LevelJump);
    }
    emit levelJumpFinished(m_levelJumpLevels, outcome);
}

void LogViewWidget::bookmarksFileLoaded(const LogFilePtr& logFile, bool fullLoad)
{
    if (logFile)
        bookmarks().fileLoaded(logFile->filePath, fullLoad);
}

bool LogViewWidget::toggleBookmark(int row)
{
    return bookmarks().toggle(row);
}

bool LogViewWidget::toggleBookmarkAtSelection()
{
    const int row = m_view->userSelectedRow();
    if (row < 0)
        return false;
    toggleBookmark(row);
    return true;
}

void LogViewWidget::goToBookmark(bool forward)
{
    // Опорная строка — выбранная пользователем (инвариант 10), как у F8.
    const int startRow = m_view->userSelectedRow();
    const int row = bookmarks().nextRow(startRow, forward);
    LevelJumpOutcome outcome = LevelJumpOutcome::NotFound;
    if (row >= 0) {
        if (row == startRow)
            outcome = LevelJumpOutcome::OnlyCurrent;
        else if (startRow >= 0 && (forward ? row < startRow : row > startRow))
            outcome = LevelJumpOutcome::Wrapped;
        else
            outcome = LevelJumpOutcome::Moved;
        m_view->goToRow(row, NavKind::Bookmark);
    }
    emit bookmarkJumpFinished(forward, outcome);
}

bool LogViewWidget::goToBookmarkId(int id)
{
    const int row = bookmarks().targetRow(id);
    if (row < 0)
        return false;
    m_view->goToRow(row, NavKind::Bookmark);
    // Строка закладки скрыта фильтром — пришли к ближайшей видимой. Следующий
    // «Show in Full Log» покажет саму закладку (R-3), если вид не трогали.
    m_hiddenTarget = {};
    const Bookmark* b = bookmarks().find(id);
    if (b && b->key.isValid() && m_model->rowForKey(b->key) < 0 && m_fullLog->canShow()) {
        m_hiddenTarget.key = b->key;
        m_hiddenTarget.userActions = m_view->userActionCount();
        emit statusMessage(tr("This bookmark is hidden by the filters of this tab; the nearest "
                              "visible line is selected. Show in Full Log shows the bookmark itself."));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Полный лог (R-3)
// ---------------------------------------------------------------------------

bool LogViewWidget::isFullLogShown() const
{
    return m_fullLog->isActive();
}

bool LogViewWidget::toggleFullLog(int row)
{
    if (m_fullLog->isActive())
        return m_fullLog->returnToFiltered();
    const HiddenTarget target = std::exchange(m_hiddenTarget, {});
    if (row < 0 && target.key.isValid() && m_view->userActionCount() == target.userActions
        && m_fullLog->showRecord(target.key))
        return true;
    return m_fullLog->show(row);
}

void LogViewWidget::discardFullLogView()
{
    m_fullLog->discard();
}

void LogViewWidget::updateFullLogBar()
{
    const auto& filters = m_fullLog->suspendedFilters();
    if (filters) {
        QStringList parts;
        if (!filters->levels.isEmpty())
            parts << tr("levels");
        if (filters->from.isValid() || filters->to.isValid())
            parts << tr("time range");
        if (filters->rules.usableRuleCount() > 0)
            parts << tr("text rules");
        if (!filters->hiddenFiles.isEmpty())
            parts << tr("files");
        m_fullLogText->setText(
            tr("Showing the full log: the filters of this tab (%1) are paused.")
                .arg(parts.join(QStringLiteral(", "))));
    }
    m_fullLogBar->setVisible(filters.has_value());
}

void LogViewWidget::handleQuickSearchFinished(int row)
{
    const QString term = m_quickSearchQuery.text;
    if (row >= 0 && row < m_model->rowCount()) {
        // Раскрыть найденную строку и подсветить в ней вхождения term ДО
        // перехода: раскрытие меняет высоту строки, и прокрутка должна
        // целиться уже в финальную геометрию. Сам переход — общий (goToRow):
        // строка становится выбором пользователя, follow-tail снимается.
        m_view->showSearchMatch(row, m_quickSearchQuery);
        m_view->goToRow(row, NavKind::QuickSearch);
        emit quickSearchFinished(term, true);
    } else {
        m_view->clearSearchMatch();
        emit quickSearchFinished(term, false);
    }
}



// ---------------------------------------------------------------------------
// Состояние вкладки вне вкладки (R-4, documentstate.h)
// ---------------------------------------------------------------------------

TabState LogViewWidget::captureState() const
{
    TabState state;
    const RecordLocator& locator = m_model->recordLocator();
    QHash<QString, int> sourceIndex; // путь файла → индекс в state.sources
    for (const LogFilePtr& lf : m_loadedFiles) {
        // Копия сжатого файла не готова (F-2): идентичность по недописанной
        // копии была бы ложной — такой файл не снимается.
        std::optional<SourceIdentity> source;
        if (isArchivePending(lf)) {
            // Копия не готова: идентичность прежнего содержимого или снимка,
            // по которому файл открыт; иначе — архив без прочитанного
            // содержимого (снимок откроет его заново).
            source = m_archives.value(lf->filePath).identity;
            if (!source)
                source = SourceIdentity::captureUnread(*lf);
        } else {
            source = locator.sourceFor(lf);
        }
        if (!source || source->ephemeral)
            continue; // спул stdin не сохраняется; файла нет — снимать нечего
        TabSource ts;
        ts.source = *source;
        const FileEncoding encoding = fileEncoding(lf);
        if (encoding.source == FileEncoding::Source::User)
            ts.userEncoding = encoding.decoder.encoding();
        // Формат строк (F-17, шаг 7) — только выбор пользователя.
        const LineFormat format = lineFormat(lf);
        if (format.isUser())
            ts.lineFormat = format;
        // Своя схема файла (F-5, PR 3) — только если отличается от схемы вкладки.
        const auto own = m_fileSchemas.constFind(lf->filePath);
        if (own != m_fileSchemas.constEnd())
            ts.schema = *own;
        sourceIndex.insert(lf->filePath, int(state.sources.size()));
        state.sources.append(ts);
    }
    for (const BookmarkState& b : bookmarks().states())
        if (!b.source.ephemeral)
            state.bookmarks.append(b);

    ViewState& view = state.view;
    if (const auto& paused = m_fullLog->suspendedFilters()) {
        // Полный лог показан (R-3): настоящие фильтры вкладки — приостановленные.
        // Закрытие вкладки или выход в этом режиме их не теряет; вкладка из
        // снимка открывается отфильтрованной.
        view.levels = paused->levels;
        view.timeFrom = paused->from;
        view.timeTo = paused->to;
        view.textRules = paused->rules;
        for (const QString& path : paused->hiddenFiles)
            if (sourceIndex.contains(path))
                view.hiddenSources.append(sourceIndex.value(path));
    } else if (m_offeredFilters && m_model->filterRules().rules.isEmpty()
        && m_model->hiddenFiles().isEmpty()
        && m_model->logLevelFilter().isEmpty() && !m_model->startTimeFilter().isValid()
        && !m_model->endTimeFilter().isValid()) {
        // Предложенные фильтры не приняты, своих нет — не теряем их.
        view.levels = m_offeredFilters->levels;
        view.timeFrom = m_offeredFilters->timeFrom;
        view.timeTo = m_offeredFilters->timeTo;
        view.textRules = m_offeredFilters->textRules;
    } else {
        view.levels = m_model->logLevelFilter();
        view.timeFrom = m_model->startTimeFilter();
        view.timeTo = m_model->endTimeFilter();
        view.textRules = m_model->filterRules();
        for (const QString& path : m_model->hiddenFiles())
            if (sourceIndex.contains(path))
                view.hiddenSources.append(sourceIndex.value(path));
    }
    std::sort(view.hiddenSources.begin(), view.hiddenSources.end());
    view.markers = m_model->rowMarkers();
    view.followTail = m_view->followTail();
    view.autoReload = m_autoReload;
    view.fields = fieldSettings();
    if (!view.followTail && isPositionPending() && m_pendingPosition.record.isValid()
        && m_view->userActionCount() == m_pendingPosition.userActions) {
        // Восстановленная позиция ещё ждёт строку (файл грузится), а
        // пользователь вид не трогал: снимок — она, а не верх
        // недогруженного экрана (запись памяти посреди загрузки её бы потеряла).
        view.currentSource = m_pendingPosition.source;
        view.current = m_pendingPosition.record;
        view.viewportOffset = m_pendingPosition.offset;
        view.currentSelected = m_pendingPosition.selected;
    } else if (!view.followTail) {
        const LogListView::ViewAnchor anchor = m_view->viewAnchor();
        const RecordKey key = anchor.isValid() ? m_model->keyAt(anchor.row) : RecordKey();
        if (key.isValid()) {
            const auto source = locator.sourceFor(key.file);
            if (source && !source->ephemeral) {
                view.currentSource = *source;
                view.current = locator.idFor(key, m_model->messageAt(anchor.row));
                view.viewportOffset = anchor.offset;
                view.currentSelected = anchor.selected;
            }
        }
    }
    return state;
}

void LogViewWidget::setStateFilters(const ViewState& view, const QSet<QString>* hiddenFiles)
{
    FilterRuleSet rules = view.textRules;
    rules.bindFields(parserFieldNames());
    const QSet<QString> hidden = hiddenFiles ? *hiddenFiles : m_model->hiddenFiles();
    // Те же фильтры — без перефильтрации (и без reset вида).
    if (view.levels == m_model->logLevelFilter() && view.timeFrom == m_model->startTimeFilter()
        && view.timeTo == m_model->endTimeFilter() && rules == m_model->filterRules()
        && hidden == m_model->hiddenFiles())
        return;
    m_model->setFilters(view.levels, view.timeFrom, view.timeTo, rules, hidden);
}

LogViewWidget::StateApplyReport LogViewWidget::applyState(const TabState& state, StateFilters filters)
{
    StateApplyReport report;
    const ViewState& view = state.view;
    if (!m_memoryApplying) // память файла — не снимок: вкладка с её файлом и так не пуста
        m_fromState = true;

    // Схема полей (F-5) — до фильтров: правила привязываются к схеме вкладки.
    // Ищет её в каталоге и ставит FieldSchemaController (синхронно).
    // Своя схема единственного файла снимка — схема вкладки (F-5, PR 3).
    std::optional<FieldSettings> fields = view.fields;
    if (state.sources.size() == 1 && state.sources[0].schema) {
        FieldSettings f = fields.value_or(fieldSettings());
        f.schemaName = state.sources[0].schema->schemaName;
        f.schema = state.sources[0].schema->schema;
        fields = f;
    }
    if (fields)
        emit savedFieldSettingsFound(*fields, m_memoryApplying);
    if (fields && state.sources.size() > 1)
        m_stateVisibleFields = fields->visibleFields;
    // Свои схемы файлов (F-5, PR 3) — после схемы вкладки: схема, равная ей,
    // своей не станет. Файл получит схему, когда откроется ниже (или сразу,
    // если он уже во вкладке).
    if (state.sources.size() > 1) {
        for (const TabSource& source : state.sources)
            if (source.schema)
                emit savedFileSchemaFound(RecordLocator::sourceKey(source.source), *source.schema,
                                          m_memoryApplying);
    }

    // Закладки — до файлов: строки ищутся по ходу загрузки (RecordLocator).
    // Переписанный источник — отказ (Missing: серая, с фрагментом и
    // заметкой); пропавший файл не открывается — закладка ждёт его.
    for (const BookmarkState& b : state.bookmarks)
        bookmarks().restore(b);

    // Проверка источников — по разу на файл (два чтения по 4 КБ).
    QHash<QString, SourceIdentity::State> checks;
    for (const TabSource& source : state.sources)
        checks.insert(source.source.path, source.source.check());
    const auto checkOf = [&checks](const SourceIdentity& source) {
        const auto it = checks.constFind(source.path);
        return it != checks.constEnd() ? *it : source.check();
    };

    m_model->setRowMarkers(view.markers);
    if (filters == StateFilters::Apply) {
        // Фильтры снимка — правда: показ полного лога (R-3) кончается, даже
        // если они совпали с нынешними и перефильтрации не будет.
        m_fullLog->discard();
        // Скрытые файлы снимка — по индексам источников (U-23). Пропавший
        // файл не откроется: его в наборе не держим, а если скрыты все
        // открывающиеся — не скрываем ничего (вкладка не пустеет без легенды).
        QSet<QString> hidden;
        QSet<QString> hiddenArchives; // сжатые: скрыть копию после распаковки (F-2)
        int opening = 0;
        for (int i = 0; i < state.sources.size(); ++i) {
            const SourceIdentity& source = state.sources[i].source;
            if (checkOf(source) == SourceIdentity::State::Missing)
                continue;
            ++opening;
            if (!view.hiddenSources.contains(i))
                continue;
            if (source.compressed)
                hiddenArchives.insert(RecordLocator::sourceKey(source));
            else
                hidden.insert(source.path);
        }
        if (hidden.size() + hiddenArchives.size() >= opening) {
            hidden.clear();
            hiddenArchives.clear();
        }
        m_hiddenArchiveSources = hiddenArchives;
        setStateFilters(view, &hidden);
        dismissOfferedFilters();
    } else {
        // Предложение — для памяти файла (вкладка одного файла): скрывать в
        // ней нечего, фильтр по файлу не предлагается.
        ViewState offered = view;
        offered.hiddenSources.clear();
        if (offered.hasFilters()) {
            m_offeredFilters = offered;
            emit offeredFiltersChanged(true);
        }
    }
    setAutoReload(view.autoReload);


    // Позиция: follow-tail главнее — вид у хвоста, строка не ставится.
    if (m_pendingPosition.token)
        m_model->recordLocator().cancel(m_pendingPosition.token);
    m_pendingPosition = {};
    if (view.followTail) {
        m_view->setFollowTail(true);
    } else if (view.hasCurrent()
               && checkOf(*view.currentSource) != SourceIdentity::State::Missing) {
        // Файла нет — он не откроется, строке неоткуда взяться.
        m_pendingPosition.token = m_model->recordLocator().request(*view.currentSource, view.current);
        m_pendingPosition.source = *view.currentSource;
        m_pendingPosition.record = view.current;
        m_pendingPosition.requests = 1;
        m_pendingPosition.offset = view.viewportOffset;
        m_pendingPosition.selected = view.currentSelected;
        m_pendingPosition.userActions = m_view->userActionCount();
    }

    for (const TabSource& source : state.sources)
        if (source.source.compressed)
            m_stateIdentities.insert(RecordLocator::sourceKey(source.source), source.source);
    for (const TabSource& source : state.sources) {
        switch (checkOf(source.source)) {
        case SourceIdentity::State::Missing:
            report.missingSources.append(source.source.path);
            continue;
        case SourceIdentity::State::Changed:
            report.changedSources.append(source.source.path);
            break;
        case SourceIdentity::State::Valid:
        case SourceIdentity::State::Ephemeral:
            break;
        }
        FileEncoding initial;
        if (source.userEncoding && TextEncodings::isDecodable(*source.userEncoding))
            initial = FileEncoding().withUserChoice(TextDecoder(*source.userEncoding));
        if (source.lineFormat)
            m_pendingSourceFormats.insert(RecordLocator::sourceKey(source.source), *source.lineFormat);
        // Память файла здесь дополняет снимок только закладками: вид, фильтры
        // и позиция — снимка (сессия главнее памяти, docs/design/R-5.md).
        const bool wasApplying = std::exchange(m_applyingState, true);
        openSource(source.source.path, source.source.member, initial);
        m_applyingState = wasApplying;
    }
    // Восстановленные закладки — не правка: отложенной записи памяти не надо.
    m_bookmarkSignature = bookmarkSignature();
    return report;
}

void LogViewWidget::addRelocatedSource(const QString& savedPath, const TabSource& source)
{
    QVector<BookmarkState> moved;
    for (const Bookmark& b : bookmarks().bookmarks()) {
        if (!samePath(b.filePath, savedPath))
            continue;
        // Закладку, которую не снять (нет идентификатора), оставляем как есть:
        // лучше «line not loaded», чем потерянная заметка.
        if (auto state = bookmarks().state(b.id)) {
            // Члены одного архива делят путь — переносятся закладки только этого члена.
            if (state->source.member != source.source.member)
                continue;
            state->source.path = source.source.path;
            moved.append(*state);
            bookmarks().remove(b.id);
        }
    }
    for (const BookmarkState& b : moved)
        bookmarks().restore(b);
    // Своя схема файла (F-5, PR 3) ждала его по прежнему пути — переезжает.
    {
        const QString oldKey = source.source.member.isEmpty()
            ? RecordLocator::pathKey(savedPath)
            : RecordLocator::pathKey(savedPath) + QChar(0x01) + source.source.member;
        const auto pending = m_pendingFileSchemas.find(oldKey);
        if (pending != m_pendingFileSchemas.end()) {
            const FieldSettings schema = pending.value();
            m_pendingFileSchemas.erase(pending);
            m_pendingFileSchemas.insert(RecordLocator::sourceKey(source.source), schema);
        }
    }
    FileEncoding initial;
    if (source.userEncoding && TextEncodings::isDecodable(*source.userEncoding))
        initial = FileEncoding().withUserChoice(TextDecoder(*source.userEncoding));
    if (source.lineFormat)
        m_pendingSourceFormats.insert(RecordLocator::sourceKey(source.source), *source.lineFormat);
    const bool wasApplying = std::exchange(m_applyingState, true);
    openSource(source.source.path, source.source.member, initial);
    m_applyingState = wasApplying;
}

void LogViewWidget::applyOfferedFilters()
{
    if (!m_offeredFilters)
        return;
    const ViewState view = *m_offeredFilters;
    m_offeredFilters.reset();
    setStateFilters(view);
    emit offeredFiltersChanged(false);
}

void LogViewWidget::dismissOfferedFilters()
{
    if (!m_offeredFilters)
        return;
    m_offeredFilters.reset();
    emit offeredFiltersChanged(false);
}

void LogViewWidget::finishPendingPosition(const RecordKey* found)
{
    m_pendingPosition.token = 0;
    m_pendingPosition.waitingRow = false;
    const auto drop = [this]() {
        m_pendingPosition = {};
        emit positionRestoreFinished(false);
    };
    // Инвариант 10: пользователь успел выбрать строку, прокрутить вид или
    // включить слежение за хвостом — его выбор главнее восстановленного.
    if (!found || m_view->userActionCount() != m_pendingPosition.userActions
        || m_view->followTail()) {
        drop();
        return;
    }
    const RecordKey key = *found; // found может указывать на m_pendingPosition.key
    int row = m_model->rowForKey(key);
    if (row < 0 && (m_loading || m_model->isFiltering())) {
        // Строка уже в данных, но видимый набор ещё строится (фильтр в фоне,
        // порции загрузки): решать «скрыта ли она» рано.
        m_pendingPosition.waitingRow = true;
        m_pendingPosition.key = key;
        return;
    }
    const bool visible = row >= 0;
    if (!visible)
        row = m_model->nearestRowForKey(key); // скрыта фильтром — рядом с её местом
    if (row < 0) {
        // Ключ больше ничего не значит (строки файла сброшены, файл закрыт) —
        // спросить локатор заново, пока есть попытки.
        if (m_pendingPosition.requests < kPositionRequests && m_pendingPosition.record.isValid()) {
            ++m_pendingPosition.requests;
            m_pendingPosition.token = m_model->recordLocator().request(m_pendingPosition.source,
                                                                       m_pendingPosition.record);
            return;
        }
        drop();
        return;
    }
    const PendingPosition done = m_pendingPosition;
    m_pendingPosition = {};
    m_view->setViewAnchor({ row, done.offset, done.selected && visible });
    emit positionRestoreFinished(true);
}

void LogViewWidget::retryPendingPosition()
{
    if (m_pendingPosition.waitingRow && !m_loading && !m_model->isFiltering())
        finishPendingPosition(&m_pendingPosition.key);
}

void LogViewWidget::setAutoReload(bool enabled)
{
    if (m_autoReload == enabled)
        return;
    m_autoReload = enabled;
    emit autoReloadChanged(enabled);
}

// ---------------------------------------------------------------------------
// Память файла на диске (R-4, шаг 2)
// ---------------------------------------------------------------------------

bool LogViewWidget::fileMemoryActive() const
{
    return m_fileMemory && AppSettings::instance().rememberFileState();
}

void LogViewWidget::setFileMemorySaveDelay(int ms)
{
    m_memorySaveTimer->setInterval(qMax(0, ms));
}

bool LogViewWidget::isFileMemorySavePending() const
{
    return m_memorySaveTimer->isActive();
}

static QVector<RecordId> recordsOf(const QVector<BookmarkState>& list)
{
    QVector<RecordId> records;
    records.reserve(list.size());
    for (const BookmarkState& b : list)
        records.append(b.record);
    return records;
}

bool LogViewWidget::applyFileMemory(const QString& filePath, const FileEncoding& initial,
                                    const LogFilePtr& self)
{
    if (SpoolDir::isStreamFile(filePath))
        return false;
    // Член zip (F-2, шаг 4): ключ памяти — архив и имя члена.
    const QString member = self ? self->source().member : QString();
    const FileMemoryStore::Lookup found =
        m_fileMemory->open(filePath, self ? self->filePath : QString(), member);
    const QString name = self ? self->displayName : QFileInfo(filePath).fileName();
    switch (found.found) {
    case FileMemoryStore::Found::None:
        return false;
    case FileMemoryStore::Found::Refused:
        emit statusMessage(tr("The saved state of %1 was written by a newer version of "
                              "DendroLog and was left untouched.").arg(name));
        return false;
    case FileMemoryStore::Found::Stale: {
        // Память прежнего содержимого: файл открывается чистым, закладки —
        // серыми (с текстом и заметкой, в логе не показаны). Сама память
        // отсоединена и найдётся, когда прежнее содержимое появится снова.
        const auto count = found.memory ? found.memory->bookmarks.size() : 0;
        if (found.memory)
            for (const BookmarkState& b : found.memory->bookmarks)
                bookmarks().restore(b);
        m_bookmarkSignature = bookmarkSignature();
        emit statusMessage(count > 0
            ? tr("%1 has changed since it was last opened: %n saved bookmark(s) could not be "
                 "found and are shown greyed out in the Bookmarks panel.", nullptr, int(count)).arg(name)
            : tr("%1 has changed since it was last opened: its saved position and markers "
                 "no longer apply.").arg(name));
        return false;
    }
    case FileMemoryStore::Found::ByPath:
    case FileMemoryStore::Found::ByContent:
        break;
    }

    FileMemory memory = *found.memory;
    if (initial.source == FileEncoding::Source::User)
        memory.source.userEncoding = initial.decoder.encoding(); // --encoding главнее памяти
    m_memoryBase.insert(FileMemoryStore::keyFor(filePath, member), recordsOf(memory.bookmarks));

    const bool wasApplying = std::exchange(m_memoryApplying, true);
    // «Первый файл» — по порядку открытия: сжатый (self) уже лежит в
    // m_loadedFiles с момента открытия, и считаются только открытые до него;
    // файл, открытый после ещё распаковывающегося архива, — второй.
    const qsizetype others = self && m_loadedFiles.contains(self) ? m_loadedFiles.indexOf(self)
                                                                   : m_loadedFiles.size();
    if (others == 0 && memory.view && !m_applyingState) {
        // Первый файл вкладки — состояние целиком, фильтры предложением.
        applyState(memory.toTabState(), StateFilters::Offer);
    } else {
        // Второй файл или память без ViewState — закладки, выбор кодировки и
        // схема файла (F-5, PR 3: своя у файла слитой вкладки, иначе схема
        // одно-файловой вкладки; «без схемы» не навязывается — схема этой
        // вкладки); файл снимка (applyState) — только закладки: кодировку и
        // схему даёт снимок.
        for (const BookmarkState& b : memory.bookmarks)
            bookmarks().restore(b);
        if (!m_applyingState) {
            std::optional<FieldSettings> schema = memory.source.schema;
            if (!schema && memory.view && memory.view->fields)
                schema = *memory.view->fields;
            if (schema && schema->hasSchema()) {
                if (others == 0) {
                    // Первый файл вкладки — его схема и есть схема вкладки.
                    FieldSettings tab = fieldSettings();
                    tab.schemaName = schema->schemaName;
                    tab.schema = schema->schema;
                    emit savedFieldSettingsFound(tab, true);
                } else {
                    FieldSettings ref;
                    ref.schemaName = schema->schemaName;
                    ref.schema = schema->schema;
                    const QString key = member.isEmpty()
                        ? RecordLocator::pathKey(filePath)
                        : RecordLocator::pathKey(filePath) + QChar(0x01) + member;
                    emit savedFileSchemaFound(key, ref, true);
                }
            }
        }
        // Выбор формата строк (F-17, шаг 7) — как выбор кодировки.
        if (!m_applyingState && memory.source.lineFormat)
            m_pendingSourceFormats.insert(RecordLocator::sourceKey(memory.source.source),
                                          *memory.source.lineFormat);
        FileEncoding encoding = initial;
        if (!m_applyingState && initial.source != FileEncoding::Source::User && memory.source.userEncoding
            && TextEncodings::isDecodable(*memory.source.userEncoding))
            encoding = FileEncoding().withUserChoice(TextDecoder(*memory.source.userEncoding));
        openSource(filePath, member, encoding);
    }
    m_memoryApplying = wasApplying;
    m_bookmarkSignature = bookmarkSignature(); // восстановление — не правка

    if (found.found == FileMemoryStore::Found::ByContent) {
        emit statusMessage(tr("%1: restored the saved state of %2 — the same content under another name.")
                               .arg(name, QDir::toNativeSeparators(found.previousPath)));
    }
    return true;
}

QString LogViewWidget::bookmarkSignature() const
{
    // Что пишется в память: строка и заметка. Статус (сверка, видимость) —
    // нет: на каждую порцию загрузки запись не нужна.
    QString signature;
    for (const BookmarkState& b : bookmarks().states()) {
        signature += b.source.path;
        signature += QLatin1Char('\x1f');
        signature += b.source.member;
        signature += QLatin1Char('\x1f');
        signature += QString::number(b.record.line);
        signature += QLatin1Char('\x1f');
        signature += QString::number(b.record.text, 16);
        signature += QLatin1Char('\x1f');
        signature += b.note;
        signature += QLatin1Char('\x1e');
    }
    return signature;
}

void LogViewWidget::onBookmarksChanged()
{
    if (!fileMemoryActive() || m_memoryApplying)
        return;
    const QString signature = bookmarkSignature();
    if (signature == m_bookmarkSignature)
        return;
    m_bookmarkSignature = signature;
    m_memorySaveTimer->start(); // дебаунс: серия правок — одна запись
}

void LogViewWidget::saveFileMemory()
{
    m_memorySaveTimer->stop();
    if (!fileMemoryActive())
        return;
    const TabState state = captureState();
    // ViewState — только у файла, который во вкладке один (спул stdin рядом —
    // тоже второй файл: позиция могла быть на нём).
    const bool single = state.sources.size() == 1 && m_loadedFiles.size() == 1;
    for (const TabSource& source : state.sources) {
        // Архив, из которого ещё ничего не прочитано (закрыт посреди первой
        // распаковки): помнить нечего, а запись затёрла бы его память.
        if (source.source.isUnreadArchive())
            continue;
        FileMemory memory;
        memory.source = source;
        QHash<QString, bool> sameContent; // идентичность закладки → то же содержимое
        for (const BookmarkState& b : state.bookmarks) {
            if (!samePath(b.source.path, source.source.path) || b.source.member != source.source.member)
                continue;
            // Серая закладка прежнего содержимого (файл переписан) — в
            // отсоединённой памяти, а не в памяти нового содержимого.
            const QString id = QStringLiteral("%1:%2:%3").arg(b.source.size).arg(b.source.head).arg(b.source.tail);
            auto it = sameContent.constFind(id);
            if (it == sameContent.constEnd())
                it = sameContent.insert(id, b.source.sameContent(source.source)
                    || (!b.source.compressed // сжатый (F-2): архив по пути не сверить
                        && b.source.checkAt(source.source.path) == SourceIdentity::State::Valid));
            if (*it)
                memory.bookmarks.append(b);
        }
        if (single) {
            memory.view = state.view;
            // Своя схема единственного файла — схема его вкладки в памяти.
            if (memory.source.schema && memory.view->fields) {
                memory.view->fields->schemaName = memory.source.schema->schemaName;
                memory.view->fields->schema = memory.source.schema->schema;
            }
            memory.source.schema.reset();
        } else {
            // Файл слитой вкладки помнит свою схему (F-5, PR 3) — какой бы она
            // ни была: своей или схемой вкладки.
            const QString sourceKey = RecordLocator::sourceKey(source.source);
            for (const LogFilePtr& lf : std::as_const(m_loadedFiles)) {
                if (lf && RecordLocator::sourceKey(*lf) == sourceKey) {
                    memory.source.schema = fileSchema(lf);
                    break;
                }
            }
        }
        const QString key = FileMemoryStore::keyFor(source.source.path, source.source.member);
        m_fileMemory->save(memory, m_memoryBase.value(key));
        m_memoryBase.insert(key, recordsOf(memory.bookmarks));
    }
    m_bookmarkSignature = bookmarkSignature();
}

void LogViewWidget::updateFiltersOfferBar()
{
    const bool show = m_offeredFilters.has_value() && !m_filtersOfferDismissed;
    if (show) {
        const ViewState& v = *m_offeredFilters;
        QStringList parts;
        if (!v.levels.isEmpty())
            parts << tr("levels");
        if (v.timeFrom.isValid() || v.timeTo.isValid())
            parts << tr("time range");
        if (v.textRules.isActive())
            parts << tr("text rules");
        m_filtersOfferText->setText(parts.isEmpty()
            ? tr("This file was filtered when you last closed it.")
            : tr("This file was filtered when you last closed it (%1).").arg(parts.join(QStringLiteral(", "))));
    }
    m_filtersOfferBar->setVisible(show);
}
