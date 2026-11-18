// logviewwidget.cpp
#include "logviewwidget.h"
#include "appsettings.h"
#include "indexedlogstore.h"
#include "quicksearch.h"
#include <QVBoxLayout>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
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
    layout->addWidget(m_view);

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
    connect(m_logParser, &LogParser::parsingFinished, this, &LogViewWidget::handleParsingFinished);
    connect(m_logParser, &LogParser::parsingFailed,   this, &LogViewWidget::handleParsingFailed);

    // Соединяем сигналы reload-парсера (только для инкрементальных обновлений)
    connect(m_reloadParser, &LogParser::entriesParsed, this, &LogViewWidget::handleIncrementalEntriesParsed);
    connect(m_reloadParser, &LogParser::tailState, this, &LogViewWidget::handleTailState);
    connect(m_reloadParser, &LogParser::encodingDecided, this, &LogViewWidget::handleEncodingDecided);
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
    const FileEncoding current = fileEncoding(logFile);
    if (current.source != FileEncoding::Source::User)
        return;
    if (!TextEncodings::isDecodable(fileStreamEncoding(logFile)))
        return;
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

void LogViewWidget::startReencode(const LogFilePtr& logFile)
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
        connect(m_reencodeParser, &LogParser::parsingFinished, this,
                [this](int, const LogFilePtr& f) { finishReencode(f); });
        connect(m_reencodeParser, &LogParser::parsingFailed, this,
                &LogViewWidget::handleReencodeFailed);
    }
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    st.loadInFlight = true;
    m_reencodeBuffers[logFile->filePath].clear();
    m_reencodeParser->setPattern(m_logParser->pattern().patternString());
    m_reencodeParser->setExtractionEnabled(m_extractionEnabled);
    m_reencodeParser->setFallbackEncoding(m_fallbackEncoding);
    m_reencodeParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath));
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
    std::sort(entries.begin(), entries.end(), logEntryPtrLess);
    st.nextLogicalEntryId = 0;
    st.lastEntry.reset();
    st.prevEntry.reset();
    for (const auto& e : std::as_const(entries))
        if (e)
            st.nextLogicalEntryId = qMax(st.nextLogicalEntryId, e->logicalEntryId() + 1);
    rememberLastLines(st, entries);
    m_model->replaceEntriesForFile(logFile->filePath, entries);
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
    if (st.reparsePending) {
        st.reparsePending = false;
        startReencode(logFile);
    }
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
    for (const auto& lf : m_loadedFiles) {
        if (!lf)
            continue;
        const FileEncoding encoding = store->fileEncoding(lf);
        if (encoding.decided())
            m_fileEncodings.insert(lf->filePath, encoding);
    }
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
    addLogFile(filePath, FileEncoding());
}

void LogViewWidget::addFilesFrom(const LogViewWidget& other)
{
    for (const LogFilePtr& lf : other.loadedFiles())
        if (lf)
            addLogFile(lf->filePath, other.fileEncoding(lf));
}

QString LogViewWidget::loadFailureMessage(const LogFilePtr& logFile) const
{
    if (!logFile)
        return tr("Failed to parse");
    const auto st = m_fileReloadStates.constFind(logFile->filePath);
    if (st == m_fileReloadStates.constEnd() || !st->unsupported)
        return tr("Failed to parse: %1").arg(logFile->shortName());
    return tr("%1 is %2: a UTF-16/UTF-32 file can only be shown on its own tab. "
              "Open it in a separate tab (merged tabs and large files read UTF-8 "
              "and single-byte encodings only).")
        .arg(logFile->shortName(), st->unsupportedReason);
}

void LogViewWidget::addLogFile(const QString &filePath, const FileEncoding& initial)
{
    for (const auto &file : m_loadedFiles) {
        if (file->filePath == filePath) {
            // Файл уже загружен или в процессе загрузки: --encoding для него —
            // обычный выбор пользователя.
            if (initial.source == FileEncoding::Source::User)
                setFileEncoding(file, initial.decoder.encoding());
            return;
        }
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
            // Вкладка могла уже держать резидентные файлы — перечитываем все,
            // с уже решёнными кодировками (F-1): решение, а тем более выбор
            // пользователя, на пороге не теряется.
            QHash<QString, FileEncoding> encodings = takeEncodingsForIndexed();
            if (initial.decided())
                encodings.insert(filePath, initial);
            for (const auto& lf : m_loadedFiles)
                startIndexedLoad(lf, encodings.value(lf->filePath));
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
    m_logParser->startParsing(logFile, m_fileEncodings.value(filePath));
    updateLoadingState();
    // UI может показать сообщение "Загрузка файла..." или индикатор прогресса
    qDebug() << "LogViewWidget: Started parsing for" << filePath;
}

void LogViewWidget::updateLoadingState()
{
    bool loading = false;
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
    if (!m_loading && m_reconsiderPending)
        reconsiderProvisional(); // запасную сменили посреди загрузки
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
    m_model->invalidateFileRows(logFile->filePath);
    if (auto* store = m_model->indexedOrNull())
        store->resetFileIndex(logFile, std::make_shared<LineIndex>());
    else
        m_model->removeEntriesForFile(logFile->filePath);
    emit totalRowCountChanged(m_model->rowCount());
    const QModelIndex cur = m_view->currentIndex();
    emit currentRowChanged(cur.isValid() ? cur.row() : -1, m_model->rowCount());
}

void LogViewWidget::startFullReload(const LogFilePtr& logFile)
{
    if (!logFile)
        return;
    // Ключ уже есть в хэше у всех вызывающих — вставки (и рехэша, который
    // инвалидировал бы их ссылку на состояние) здесь не происходит.
    FileReloadState& st = m_fileReloadStates[logFile->filePath];
    // Строки файла сбрасываются: закладки и история переходов сверят их,
    // когда строки покажутся.
    m_model->invalidateFileRows(logFile->filePath);

    if (auto* store = m_model->indexedOrNull()) {
        // Строки файла сбрасываются, индексация идёт в свежий LineIndex
        // (store сам чистит кэш текста и refs).
        ensureIndexer();
        auto fresh = std::make_shared<LineIndex>();
        store->resetFileIndex(logFile, fresh);
        st = FileReloadState{};
        st.loadInFlight = true;
        // resetFileIndex вернул автоматике «не решено», выбор пользователя
        // оставил — с ним и перечитываем.
        m_indexer->startIndexing(logFile, fresh, store->fileEncoding(logFile));
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
    m_logParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath));
    updateLoadingState();
    emit fileEncodingChanged(logFile);
}

void LogViewWidget::ensureIndexer()
{
    if (m_indexer)
        return;
    m_indexer = new LogIndexer(this);
    m_indexer->setPattern(m_logParser->pattern().patternString());
    m_indexer->setExtractionEnabled(m_extractionEnabled);
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
    store->setFieldPattern(m_logParser->pattern().patternString(), m_extractionEnabled);
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
    m_indexer->startIndexing(logFile, index, encoding);
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
    updateLoadingState();

    bookmarksFileLoaded(logFile, wasInitial);
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
    if (logFile) {
        // Индекс может остаться частичным — файл считается незагруженным и
        // будет прочитан заново, как только изменится на диске.
        FileReloadState& st = m_fileReloadStates[logFile->filePath];
        st.loadInFlight = false;
        st.initialLoadDone = false;
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
        m_logParser->startParsing(logFile, m_fileEncodings.value(logFile->filePath));
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
        updateLoadingState();
        bookmarksFileLoaded(parsedLogFile, true);
        if (st.reparsePending) {
            // Кодировку сменили посреди загрузки — перечитываем выбранной.
            st.reparsePending = false;
            startReencode(parsedLogFile);
        }
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
}

bool LogViewWidget::reloadChangedFiles(bool force)
{
    // Растущий файл (типично — спул stdin) пересёк порог индексного бэкенда:
    // конвертируем вкладку, дальше хвост дочитывает индексатор. Конвертируем
    // только когда по файлу нет джоба в полёте — иначе резидентный парсер
    // дописал бы в состояние, которым уже владеет индексатор. Файл, который
    // индексатор уже вернул в резидентный путь (UTF-16), не конвертируем.
    if (!m_model->isIndexedBackend() && m_loadedFiles.size() == 1
        && m_loadedFiles[0]
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
                    st.loadInFlight = true;
                    // Кодировка файла (F-1): «не решено» переживает дозапись —
                    // индексатор продолжит проверять новые байты.
                    m_indexer->startIndexingFrom(logFile, index,
                                                 index->lastLineProvisional(),
                                                 indexedStore->fileEncoding(logFile));
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
            m_reloadParser->setPattern(m_logParser->pattern().patternString());
            st.loadInFlight = true;
            // Якорь сдвинется по концу прочитанного (tailState); повторный
            // опрос этот диапазон не возьмёт — по файлу загрузка в полёте.
            // Кодировка хвоста (F-1): поток — как читалось начало (UTF-16/32),
            // байты — состоянием файла; «не решено» решит не-ASCII строка хвоста.
            m_reloadParser->startParsingFrom(logFile, from, context, st.encoding,
                                             m_fileEncodings.value(logFile->filePath));
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
        updateLoadingState();
        bookmarksFileLoaded(logFile, false);
        if (st.reparsePending) {
            st.reparsePending = false;
            startReencode(logFile);
        }
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
    return true;
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


