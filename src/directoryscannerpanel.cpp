#include "directoryscannerpanel.h"

#include "appsettings.h"
#include "archiveregistry.h"
#include "cardframe.h"
#include "apptheme.h"
#include "spooldir.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QActionGroup>
#include <QCheckBox>
#include <QDateTimeEdit>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

using RefreshMode = DirectoryScanner::RefreshMode;

namespace {
// Progress is reported in bytes, which overflows QProgressBar's int range on
// big trees — drive the bar in per-mille instead and keep the exact numbers
// for the status text.
constexpr int kProgressScale = 1000;
}

DirectoryScannerPanel::DirectoryScannerPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUi();

    m_scanner = new DirectoryScanner(m_tree, this);

    // Re-emit scanner activation signals straight through.
    connect(m_scanner, &DirectoryScanner::dateBoundsChanged,
            this, &DirectoryScannerPanel::onDateBoundsChanged);
    connect(m_scanner, &DirectoryScanner::contentFilterProgress,
            this, &DirectoryScannerPanel::onContentProgress);
    connect(m_scanner, &DirectoryScanner::contentFilterFinished,
            this, &DirectoryScannerPanel::onContentFinished);
    connect(m_scanner, &DirectoryScanner::rescanFinished,
            this, &DirectoryScannerPanel::onRescanFinished);
    connect(m_scanner, &DirectoryScanner::rescanStarted, this, [this]() {
        m_statusLabel->setText(tr("Refreshing…"));
        updateRefreshButton();
    });
    connect(m_scanner, &DirectoryScanner::pendingChangesChanged,
            this, [this](bool) { updateRefreshButton(); });

    // Restore the persisted refresh preference.
    const AppSettings& settings = AppSettings::instance();
    m_scanner->setAutoRefreshIntervalSecs(settings.scanRefreshIntervalSecs());
    m_scanner->setRefreshMode(static_cast<RefreshMode>(settings.scanRefreshMode()));
    // Запасная кодировка фильтра по содержимому — та же, что у вкладок (F-1).
    m_scanner->setFallbackEncoding(settings.fallbackEncoding());
    connect(&AppSettings::instance(), &AppSettings::fallbackEncodingChanged,
            m_scanner, &DirectoryScanner::setFallbackEncoding);
    updateRefreshButton();
}

void DirectoryScannerPanel::buildUi()
{
    // Поля и промежутки — от CompactStyle, как у остальных панелей.
    auto* root = new QVBoxLayout(this);

    // ── Header card ─────────────────────────────────────────────────────────
    m_card = new CardFrame(this);
    m_card->setAccentColor(AppTheme::instance().logInfo);
    CardFrame* card = m_card;
    QVBoxLayout* rows = card->rowsLayout();
    rows->setSpacing(3);

    const QSize squareBtn(24, 24);

    // Row 1 (always visible): [Scan▢] [⟳▢] path…………… [⚙] [Exts▢]
    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);

    m_scanButton = card->makeToolButton(QString(),
        tr("Choose a folder and scan it for log files"));
    m_scanButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    m_scanButton->setFixedSize(squareBtn);
    headerRow->addWidget(m_scanButton);

    m_refreshButton = card->makeToolButton(QString(), QString());
    m_refreshButton->setObjectName(QStringLiteral("scannerRefreshButton"));
    m_refreshButton->setFixedSize(squareBtn);
    m_refreshButton->setEnabled(false);
    m_refreshButton->setContextMenuPolicy(Qt::CustomContextMenu);
    headerRow->addWidget(m_refreshButton);

    // «Назад» из архива к прежней папке (F-2, шаг 4): видна, пока показан архив.
    m_backButton = card->makeToolButton(QString(),
        tr("Leave the archive and return to the folder shown before it"));
    m_backButton->setObjectName(QStringLiteral("scannerBackButton"));
    m_backButton->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
    m_backButton->setFixedSize(squareBtn);
    m_backButton->setVisible(false);
    headerRow->addWidget(m_backButton);

    m_pathLabel = new QLabel(tr("No directory scanned yet"), card);
    m_pathLabel->setObjectName(QStringLiteral("scannerPathLabel"));
    m_pathLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-style: italic;"));
    // Don't let a long path force the dock wider — clip instead of pushing.
    m_pathLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headerRow->addWidget(m_pathLabel, 1);

    m_settingsToggle = card->makeToolButton(QStringLiteral("⚙"),
        tr("Show/hide content and date filters"));
    m_settingsToggle->setCheckable(true);
    m_settingsToggle->setFixedSize(squareBtn);
    headerRow->addWidget(m_settingsToggle);

    m_extsButton = card->makeToolButton(QString(),
        tr("Configure which file extensions are scanned (e.g. log, txt)"));
    m_extsButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    m_extsButton->setFixedSize(squareBtn);
    headerRow->addWidget(m_extsButton);
    rows->addLayout(headerRow);

    // ── Collapsible filter area (hidden until ⚙ is toggled) ──────────────────
    m_filterArea = new QWidget(card);
    auto* filterLayout = new QVBoxLayout(m_filterArea);
    filterLayout->setContentsMargins(0, 2, 0, 0);
    filterLayout->setSpacing(3);

    // Separator
    auto* sep = new QFrame(m_filterArea);
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setStyleSheet(QStringLiteral("color: palette(midlight);"));
    filterLayout->addWidget(sep);

    // Content filter
    auto* contentRow = new QHBoxLayout();
    contentRow->setSpacing(4);
    m_contentEdit = new QLineEdit(m_filterArea);
    m_contentEdit->setPlaceholderText(tr("Filter files by content…"));
    m_contentEdit->setClearButtonEnabled(true);
    m_contentEdit->setToolTip(tr("Keep only files whose contents match this text.\n"
                                 "Press Enter or Apply to run the search."));
    contentRow->addWidget(m_contentEdit, 1);

    m_regexButton = card->makeToolButton(QStringLiteral(".*"),
        tr("Treat the filter text as a regular expression"));
    m_regexButton->setCheckable(true);
    contentRow->addWidget(m_regexButton);

    m_caseButton = card->makeToolButton(QStringLiteral("Aa"),
        tr("Case-sensitive content match"));
    m_caseButton->setCheckable(true);
    contentRow->addWidget(m_caseButton);
    filterLayout->addLayout(contentRow);

    // Date filter
    auto* dateRow = new QHBoxLayout();
    dateRow->setSpacing(4);
    m_dateEnable = new QCheckBox(m_filterArea);
    m_dateEnable->setToolTip(tr("Enable date-range filtering"));
    dateRow->addWidget(m_dateEnable);

    const QString dtFormat = QStringLiteral("yyyy-MM-dd HH:mm:ss");
    m_dateFrom = new QDateTimeEdit(m_filterArea);
    m_dateFrom->setDisplayFormat(dtFormat);
    m_dateFrom->setCalendarPopup(true);
    m_dateFrom->setToolTip(tr("Keep files whose entries reach this start time or later"));
    dateRow->addWidget(m_dateFrom, 1);

    auto* arrow = new QLabel(QStringLiteral("→"), m_filterArea);
    dateRow->addWidget(arrow);

    m_dateTo = new QDateTimeEdit(m_filterArea);
    m_dateTo->setDisplayFormat(dtFormat);
    m_dateTo->setCalendarPopup(true);
    m_dateTo->setToolTip(tr("Keep files whose entries start at this end time or earlier"));
    dateRow->addWidget(m_dateTo, 1);

    m_dateResetButton = card->makeToolButton(QStringLiteral("⟳"),
        tr("Reset the range to the full span of the scanned files"));
    dateRow->addWidget(m_dateResetButton);
    filterLayout->addLayout(dateRow);

    // Actions + progress
    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(4);
    m_applyButton = new QPushButton(tr("Apply"), m_filterArea);
    m_applyButton->setToolTip(tr("Apply the content and date filters"));
    m_applyButton->setDefault(true);
    m_resetButton = new QPushButton(tr("Reset"), m_filterArea);
    m_resetButton->setToolTip(tr("Clear all filters and show every file"));
    actionRow->addWidget(m_applyButton);
    actionRow->addWidget(m_resetButton);

    m_progress = new QProgressBar(m_filterArea);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    m_progress->setVisible(false);
    actionRow->addWidget(m_progress, 1);

    m_statusLabel = new QLabel(m_filterArea);
    m_statusLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    actionRow->addWidget(m_statusLabel);
    actionRow->addStretch(0);
    filterLayout->addLayout(actionRow);

    m_filterArea->setVisible(false);
    rows->addWidget(m_filterArea);

    root->addWidget(card);

    // ── Results tree ─────────────────────────────────────────────────────────
    m_tree = new QTreeWidget(this);
    root->addWidget(m_tree, 1);

    setDateEditorsEnabled(false);

    // ── Wiring ───────────────────────────────────────────────────────────────
    connect(m_scanButton, &QToolButton::clicked,
            this, &DirectoryScannerPanel::scanRequested);
    connect(m_extsButton, &QToolButton::clicked,
            this, &DirectoryScannerPanel::configureExtensionsRequested);
    connect(m_backButton, &QToolButton::clicked,
            this, &DirectoryScannerPanel::returnFromArchive);
    connect(m_refreshButton, &QToolButton::clicked, this, [this]() {
        m_scanner->rescan();
    });
    connect(m_refreshButton, &QToolButton::customContextMenuRequested, this, [this](const QPoint& pos) {
        showRefreshMenu(m_refreshButton->mapToGlobal(pos));
    });
    connect(m_settingsToggle, &QToolButton::toggled, this, [this](bool on) {
        m_filterArea->setVisible(on);
    });
    connect(m_applyButton, &QPushButton::clicked,
            this, &DirectoryScannerPanel::onApplyClicked);
    connect(m_contentEdit, &QLineEdit::returnPressed,
            this, &DirectoryScannerPanel::onApplyClicked);
    connect(m_resetButton, &QPushButton::clicked,
            this, &DirectoryScannerPanel::onResetClicked);
    connect(m_dateEnable, &QCheckBox::toggled, this, [this](bool on) {
        setDateEditorsEnabled(on);
    });
    connect(m_dateResetButton, &QToolButton::clicked, this, [this]() {
        // The editors' range is kept at [earliest, latest] of the scan, so the
        // range bounds ARE the full span — snap both editors back to them.
        m_seeding = true;
        m_dateFrom->setDateTime(m_dateFrom->minimumDateTime());
        m_dateTo->setDateTime(m_dateTo->maximumDateTime());
        m_seeding = false;
        m_userTouchedDates = false;
    });

    auto markTouched = [this]() { m_userTouchedDates = true; };
    connect(m_dateFrom, &QDateTimeEdit::dateTimeChanged, this, [this, markTouched]() {
        if (!m_seeding) markTouched();
    });
    connect(m_dateTo, &QDateTimeEdit::dateTimeChanged, this, [this, markTouched]() {
        if (!m_seeding) markTouched();
    });
}

void DirectoryScannerPanel::setDateEditorsEnabled(bool on)
{
    m_dateFrom->setEnabled(on);
    m_dateTo->setEnabled(on);
    m_dateResetButton->setEnabled(on);
}

void DirectoryScannerPanel::showPath(const QString& path)
{
    // Reset filter UI for the new scan.
    m_userTouchedDates = false;
    m_statusLabel->clear();
    m_progress->setVisible(false);

    m_scanner->scan(path);
    showCurrentPath();
    updateRefreshButton();
}

void DirectoryScannerPanel::showCurrentPath()
{
    m_backButton->setVisible(!m_archivePath.isEmpty());
    m_backButton->setToolTip(tr("Leave the archive and return to the folder shown before it"));
    if (!m_archivePath.isEmpty()) {
        // Подпись — архив, а не временный каталог (F-2): его путь ничего не
        // говорит и после выхода исчезнет.
        m_pathLabel->setStyleSheet(QString());
        m_pathLabel->setText(QFileInfo(m_archivePath).fileName());
        m_pathLabel->setToolTip(tr("Archive %1\nIts log files were extracted to a temporary folder; "
                                   "they are removed when you leave the archive.")
                                    .arg(QDir::toNativeSeparators(m_archivePath)));
    } else if (!m_scanner->rootPath().isEmpty()) {
        m_pathLabel->setStyleSheet(QString());
        m_pathLabel->setText(m_scanner->rootPath());
        m_pathLabel->setToolTip(m_scanner->rootPath());
    } else {
        m_pathLabel->setStyleSheet(QStringLiteral("color: palette(mid); font-style: italic;"));
        m_pathLabel->setText(tr("No directory scanned yet"));
        m_pathLabel->setToolTip(QString());
    }
}

void DirectoryScannerPanel::scanDirectory(const QString& path)
{
    // Папка вместо архива: архив и его каталог отпускаются, идущее
    // извлечение — отменяется.
    cancelExtraction();
    const std::shared_ptr<SpoolItem> keep = m_archiveItem; // до смены корня
    releaseArchive();
    m_folderBeforeArchive.clear();
    showPath(path);
}

// ─── Archive (F-2, step 4) ───────────────────────────────────────────────────

DirectoryScannerPanel::~DirectoryScannerPanel()
{
    cancelExtraction();
    if (m_archiveItem)
        ArchiveRegistry::instance().remove(m_archiveItem->path());
}

void DirectoryScannerPanel::setSpoolRoot(const QString& root)
{
    if (root == m_spoolRoot)
        return;
    m_spoolRoot = root;
    m_spoolDir.reset(); // показанный архив держит свой каталог сам
}

void DirectoryScannerPanel::cancelExtraction()
{
    if (m_pendingArchive.isEmpty())
        return;
    // Не ждать: частичный каталог (бывает на гигабайты) воркер удаляет сам,
    // в своём потоке. Распаковщик отцепляется от панели и удаляет себя по
    // итогу; следующее извлечение берёт новый — его итог и прогресс с
    // прежними не путаются.
    SourceUnpacker* old = m_unpacker;
    m_unpacker = nullptr;
    old->disconnect(this);
    connect(old, &SourceUnpacker::finished, old, &QObject::deleteLater);
    old->cancel();
    m_pendingArchive.clear();
    m_pendingItem.reset();
    m_job = 0;
    showCurrentPath();
}

void DirectoryScannerPanel::releaseArchive()
{
    if (!m_archiveItem)
        return;
    ArchiveRegistry::instance().remove(m_archiveItem->path());
    m_archiveItem.reset(); // каталог удалится в фоне, когда отпустят все ссылки
    m_archivePath.clear();
}

void DirectoryScannerPanel::openArchive(const QString& archivePath)
{
    cancelExtraction();
    const QString root = m_spoolRoot.isEmpty() ? AppSettings::spoolRoot() : m_spoolRoot;
    QString error;
    if (!m_spoolDir)
        m_spoolDir = SpoolDir::shared(root, &error);
    const QString name = QFileInfo(archivePath).fileName();
    if (!m_spoolDir) {
        emit statusMessage(tr("Cannot open %1: no place for temporary files (%2).").arg(name, error));
        return;
    }
    if (!m_unpacker) {
        m_unpacker = new SourceUnpacker(this);
        connect(m_unpacker, &SourceUnpacker::progress, this, [this](qint64 done, qint64 total) {
            if (m_pendingArchive.isEmpty())
                return;
            const int percent = total > 0 ? int(qMin<qint64>(100, done * 100 / total)) : 0;
            m_pathLabel->setText(tr("Extracting %1… %2%")
                                     .arg(QFileInfo(m_pendingArchive).fileName(), QString::number(percent)));
        });
        connect(m_unpacker, &SourceUnpacker::finished, this, &DirectoryScannerPanel::onArchiveExtracted);
    }
    // Члены отбираются тем же правилом имён, что и файлы дерева; воркер
    // получает копию списка.
    const QStringList extensions = m_scanner->effectiveExtensions();
    m_pendingItem = m_spoolDir->createDirectory(QFileInfo(archivePath).completeBaseName());
    if (!m_pendingItem) {
        emit statusMessage(tr("Cannot open %1: no place for temporary files in %2.")
                               .arg(name, QDir::toNativeSeparators(m_spoolDir->path())));
        return;
    }
    m_pendingArchive = archivePath;
    m_job = m_unpacker->unpackZip(archivePath, m_pendingItem->path(), [extensions](const QString& member) {
        return scannerNameMatches(QFileInfo(member).fileName(), extensions);
    });
    m_pathLabel->setStyleSheet(QString());
    m_pathLabel->setText(tr("Extracting %1…").arg(name));
    m_pathLabel->setToolTip(QDir::toNativeSeparators(archivePath));
    // Пока идёт извлечение, «назад» его отменяет (первый архив иначе нечем
    // остановить); показанное до него остаётся.
    m_backButton->setVisible(true);
    m_backButton->setToolTip(tr("Cancel extracting %1").arg(name));
}

void DirectoryScannerPanel::onArchiveExtracted(const SourceUnpacker::Result& r)
{
    if (r.job != m_job || m_pendingArchive.isEmpty())
        return; // отменённое задание
    const QString archive = m_pendingArchive;
    const std::shared_ptr<SpoolItem> item = std::move(m_pendingItem);
    m_pendingArchive.clear();
    m_job = 0;
    const QString name = QFileInfo(archive).fileName();
    if (!r.ok()) {
        showCurrentPath();
        emit statusMessage(r.error == SourceUnpacker::Error::Cancelled
                               ? tr("Extracting %1 was cancelled.").arg(name)
                               : tr("Cannot open %1: %2").arg(name, r.message));
        return;
    }
    QHash<QString, QString> members;
    for (const SourceUnpacker::Result::Extracted& e : r.extracted)
        members.insert(e.relativePath, e.member);
    // Папка до архива — та, что была показана до ПЕРВОГО архива: из архива в
    // архив «назад» ведёт к ней же.
    if (m_archivePath.isEmpty())
        m_folderBeforeArchive = m_scanner->rootPath();
    ArchiveRegistry::instance().add(item->path(), archive, members, item);
    const std::shared_ptr<SpoolItem> previous = m_archiveItem;
    if (previous)
        ArchiveRegistry::instance().remove(previous->path());
    m_archiveItem = item;
    m_archivePath = archive;
    showPath(item->path()); // прежний каталог отпускается после смены корня

    QStringList notes;
    if (r.extracted.isEmpty())
        notes << tr("%1 has no log files (names are matched by the scanner's extensions).").arg(name);
    if (r.skippedUnsafe > 0)
        notes << tr("%n entry(ies) of %1 skipped: unsafe names or links.", nullptr, r.skippedUnsafe).arg(name);
    if (r.skippedEncrypted > 0)
        notes << tr("%n entry(ies) of %1 skipped: password-protected or compressed with an unsupported method.",
                    nullptr, r.skippedEncrypted).arg(name);
    if (!notes.isEmpty())
        emit statusMessage(notes.join(QLatin1Char(' ')));
}

void DirectoryScannerPanel::returnFromArchive()
{
    // Идёт извлечение — «назад» отменяет только его.
    if (!m_pendingArchive.isEmpty()) {
        cancelExtraction();
        emit statusMessage(tr("Extracting the archive was cancelled."));
        return;
    }
    if (m_archivePath.isEmpty())
        return;
    const std::shared_ptr<SpoolItem> keep = m_archiveItem; // до смены корня
    releaseArchive();
    const QString folder = m_folderBeforeArchive;
    m_folderBeforeArchive.clear();
    showPath(folder);
}

void DirectoryScannerPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_scanner->refreshMode() == RefreshMode::Auto && m_scanner->hasPendingChanges())
        m_scanner->rescan();
}

// ─── Refresh mode ────────────────────────────────────────────────────────────

void DirectoryScannerPanel::showRefreshMenu(const QPoint& globalPos)
{
    QMenu menu(this);
    auto* group = new QActionGroup(&menu);
    group->setExclusive(true);

    const RefreshMode current = m_scanner->refreshMode();
    const auto addMode = [&](RefreshMode mode, const QString& text, const QString& tip) {
        QAction* act = menu.addAction(text);
        act->setCheckable(true);
        act->setChecked(current == mode);
        act->setToolTip(tip);
        group->addAction(act);
        connect(act, &QAction::triggered, this, [this, mode]() { setRefreshMode(mode); });
    };

    addMode(RefreshMode::Manual, tr("Refresh manually"),
            tr("The tree only changes when you refresh it; the button lights up "
               "when something changed on disk."));
    addMode(RefreshMode::Auto, tr("Refresh automatically (every %n s)", nullptr,
                                  m_scanner->autoRefreshIntervalSecs()),
            tr("Pick up changes on their own, re-reading only the files that moved."));
    addMode(RefreshMode::Off, tr("Refresh off"),
            tr("Nothing is watched. The button still refreshes on demand."));

    menu.addSeparator();
    QAction* interval = menu.addAction(tr("Auto-refresh interval…"));
    connect(interval, &QAction::triggered, this, &DirectoryScannerPanel::promptRefreshInterval);

    menu.exec(globalPos);
}

void DirectoryScannerPanel::setRefreshMode(RefreshMode mode)
{
    m_scanner->setRefreshMode(mode);
    AppSettings::instance().setScanRefreshMode(static_cast<int>(mode));
    updateRefreshButton();
}

void DirectoryScannerPanel::promptRefreshInterval()
{
    bool ok = false;
    const int secs = QInputDialog::getInt(this, tr("Auto-refresh interval"),
                                          tr("Rescan the directory every N seconds:"),
                                          m_scanner->autoRefreshIntervalSecs(),
                                          5, 3600, 5, &ok);
    if (!ok)
        return;
    m_scanner->setAutoRefreshIntervalSecs(secs);
    AppSettings::instance().setScanRefreshIntervalSecs(m_scanner->autoRefreshIntervalSecs());
    updateRefreshButton();
}

// Смена темы: глиф кнопки обновления перекрашен под палитру.
void DirectoryScannerPanel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && m_scanner)
        updateRefreshButton();
}

void DirectoryScannerPanel::updateRefreshButton()
{
    const bool haveRoot = !m_scanner->rootPath().isEmpty();
    m_refreshButton->setEnabled(haveRoot && !m_scanner->isRescanning());

    const RefreshMode mode = m_scanner->refreshMode();
    const bool pending = haveRoot && m_scanner->hasPendingChanges();
    // Accent = "this button wants your attention": auto-refresh is running, or
    // the watcher has changes waiting to be pulled in.
    const bool highlight = (mode == RefreshMode::Auto) || pending;

    QColor glyph = palette().buttonText().color();
    if (mode == RefreshMode::Off)
        glyph = CardFrame::mutedTextColor(palette());
    else if (highlight)
        glyph = AppTheme::instance().logInfo;
    m_refreshButton->setIcon(CardFrame::tintedIcon(QStringLiteral(":/icons/reload.svg"), glyph));
    m_card->tintToolButton(m_refreshButton, highlight);

    QString modeText;
    switch (mode) {
    case RefreshMode::Manual: modeText = tr("manual"); break;
    case RefreshMode::Auto:
        modeText = tr("automatic, every %n s", nullptr, m_scanner->autoRefreshIntervalSecs());
        break;
    case RefreshMode::Off:    modeText = tr("off"); break;
    }
    QString tip = tr("Refresh the scanned directory (mode: %1).\n"
                     "Only new, removed and modified files are re-read.\n"
                     "Right-click to choose the refresh mode.").arg(modeText);
    if (pending)
        tip = tr("The directory changed on disk — click to pull the changes in.\n\n") + tip;
    m_refreshButton->setToolTip(tip);
}

void DirectoryScannerPanel::onRescanFinished(int added, int removed, int updated)
{
    updateRefreshButton();
    if (added == 0 && removed == 0 && updated == 0) {
        m_statusLabel->setText(tr("Up to date"));
        return;
    }
    QStringList parts;
    if (added > 0)   parts << tr("+%1 new").arg(added);
    if (removed > 0) parts << tr("−%1 gone").arg(removed);
    if (updated > 0) parts << tr("%1 changed").arg(updated);
    m_statusLabel->setText(parts.join(QStringLiteral(", ")));
}

// ─── Filters ─────────────────────────────────────────────────────────────────

void DirectoryScannerPanel::onApplyClicked()
{
    const QString text = m_contentEdit->text();
    m_scanner->setContentFilter(text, m_regexButton->isChecked(), m_caseButton->isChecked());

    if (m_dateEnable->isChecked())
        m_scanner->setDateFilter(true, m_dateFrom->dateTime(), m_dateTo->dateTime());
    else
        m_scanner->setDateFilter(false, QDateTime(), QDateTime());

    if (!text.isEmpty()) {
        m_progress->setRange(0, kProgressScale);
        m_progress->setValue(0);
        m_progress->setVisible(true);
        m_statusLabel->setText(tr("Searching…"));
    }
    // Tint the ⚙ button while any filter is active, so it stays visible when
    // the filter area is collapsed.
    m_card->tintToolButton(m_settingsToggle, !text.isEmpty() || m_dateEnable->isChecked());
    m_scanner->applyFilters();
}

void DirectoryScannerPanel::onResetClicked()
{
    m_contentEdit->clear();
    m_regexButton->setChecked(false);
    m_caseButton->setChecked(false);
    m_dateEnable->setChecked(false);
    m_userTouchedDates = false;
    m_progress->setVisible(false);
    m_statusLabel->clear();
    m_card->tintToolButton(m_settingsToggle, false);
    m_scanner->clearFilters();
}

void DirectoryScannerPanel::onDateBoundsChanged(const QDateTime& earliest, const QDateTime& latest)
{
    if (!earliest.isValid() || !latest.isValid())
        return;

    m_seeding = true;
    m_dateFrom->setDateTimeRange(earliest, latest);
    m_dateTo->setDateTimeRange(earliest, latest);
    if (!m_userTouchedDates) {
        m_dateFrom->setDateTime(earliest);
        m_dateTo->setDateTime(latest);
    }
    m_seeding = false;
}

void DirectoryScannerPanel::onContentProgress(qint64 bytesDone, qint64 bytesTotal,
                                              int filesDone, int filesTotal)
{
    if (bytesTotal <= 0)
        return;
    const qint64 done = qBound<qint64>(0, bytesDone, bytesTotal);

    m_progress->setVisible(true);
    m_progress->setRange(0, kProgressScale);
    m_progress->setValue(int(done * kProgressScale / bytesTotal));
    m_statusLabel->setText(tr("Searching… %1% (%2/%3 files)")
                               .arg(done * 100 / bytesTotal)
                               .arg(filesDone)
                               .arg(filesTotal));
}

void DirectoryScannerPanel::onContentFinished(int matched, int total)
{
    m_progress->setVisible(false);
    if (total > 0)
        m_statusLabel->setText(tr("%1 of %2 files matched").arg(matched).arg(total));
    else
        m_statusLabel->clear();
}
