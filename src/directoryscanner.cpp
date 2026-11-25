#include "directoryscanner.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFileSystemWatcher>
#include <QHeaderView>
#include <QLocale>
#include <QMenu>
#include <QThread>
#include <QRegularExpression>
#include <QtConcurrent>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <algorithm>
#include "apptheme.h"
#include "filechangedetector.h"

class ScannerDelegate : public QStyledItemDelegate {
public:
    explicit ScannerDelegate(QObject* parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        if (index.column() != ScanCol::Alerts) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // Draw row background (selection, hover, etc.) — no text
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        painter->save();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        opt.text = "";
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        // Clip drawing to this cell so badges never bleed into adjacent columns
        painter->setClipRect(opt.rect);

        const QString text = index.data(Qt::DisplayRole).toString();

        // No-alert placeholder "—": draw it centered in a dim color
        if (!text.contains('/')) {
            painter->setPen(opt.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(opt.rect, Qt::AlignCenter, text);
            painter->restore();
            return;
        }

        // Format is "W/E/F" — each part is the count or empty string when 0
        const QStringList parts = text.split('/');
        if (parts.size() != 3) {
            painter->restore();
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // Colored badge rectangles — always the same colors regardless of selection,
        // so text is always readable on any background.
        const AppTheme& theme     = AppTheme::instance();
        const QColor& warnBadge   = theme.treeBadgeWarn;
        const QColor& errBadge    = theme.treeBadgeError;
        const QColor& fatBadge    = theme.treeBadgeFatal;
        const QColor& sepCol      = theme.treeBadgeSep;

        const QFontMetrics fm = painter->fontMetrics();
        const int hPad   = 2;   // horizontal padding inside badge
        const int vPad   = 0;   // vertical padding inside badge
        const int radius = 2;
        const int badgeH = std::min(fm.height() + vPad * 2, opt.rect.height() - 2);
        const int badgeY = opt.rect.top() + (opt.rect.height() - badgeH) / 2;
        const int textY  = opt.rect.top() + (opt.rect.height() - fm.height()) / 2 + fm.ascent();

        // Center the whole "W/E/F" group horizontally in the cell
        // First compute total width
        auto segWidth = [&](const QString& s) -> int {
            return s.isEmpty() ? 0 : fm.horizontalAdvance(s) + hPad * 2;
        };
        const int sepW  = fm.horizontalAdvance('/');
        const int total = segWidth(parts[0]) + sepW + segWidth(parts[1]) + sepW + segWidth(parts[2]);
        int x = opt.rect.left() + (opt.rect.width() - total) / 2;
        if (x < opt.rect.left() + 2) x = opt.rect.left() + 2;

        painter->setRenderHint(QPainter::Antialiasing, true);

        auto drawBadge = [&](const QString& s, const QColor& badgeColor) {
            if (s.isEmpty()) return;
            const int tw = fm.horizontalAdvance(s);
            const int bw = tw + hPad * 2;
            const QRect r(x, badgeY, bw, badgeH);
            painter->setBrush(badgeColor);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(r, radius, radius);
            painter->setPen(Qt::white);
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->drawText(r, Qt::AlignCenter, s);
            painter->setRenderHint(QPainter::Antialiasing, true);
            x += bw;
        };

        auto drawSep = [&]() {
            painter->setRenderHint(QPainter::Antialiasing, false);
            painter->setPen(sepCol);
            painter->drawText(x, textY, QStringLiteral("/"));
            x += sepW;
            painter->setRenderHint(QPainter::Antialiasing, true);
        };

        drawBadge(parts[0], warnBadge);
        drawSep();
        drawBadge(parts[1], errBadge);
        drawSep();
        drawBadge(parts[2], fatBadge);

        painter->restore();
    }
};

// ─── Concurrency ─────────────────────────────────────────────────────────────

// Number of files scanned in parallel – use all available logical cores,
// capped at 8 so we don't starve the UI thread pool on many-core machines.
static int maxConcurrentScans()
{
    const int cores = std::max(1, QThread::idealThreadCount());
    // Keep at least one core for UI/event loop responsiveness.
    return std::clamp(cores - 1, 1, 4);
}

// Content search is I/O-bound: a few more workers than cores keeps the drive
// busy while others wait on reads, and the dedicated pool means the search
// never has to queue behind the (CPU-bound) stat analysis.
static int maxContentSearchThreads()
{
    return std::clamp(QThread::idealThreadCount(), 2, 8);
}

// ─── Name ordering ───────────────────────────────────────────────────────────

int scannerCompareNames(QStringView a, QStringView b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const QChar ca = a[i];
        const QChar cb = b[j];

        // Digit runs compare by value, so "log2" lands before "log10".
        if (ca.isDigit() && cb.isDigit()) {
            const qsizetype si = i, sj = j;
            while (i < a.size() && a[i].isDigit()) ++i;
            while (j < b.size() && b[j].isDigit()) ++j;
            QStringView na = a.sliced(si, i - si);
            QStringView nb = b.sliced(sj, j - sj);
            // Leading zeros carry no value ("007" == "7"), keep one digit.
            while (na.size() > 1 && na.front() == u'0') na = na.sliced(1);
            while (nb.size() > 1 && nb.front() == u'0') nb = nb.sliced(1);
            if (na.size() != nb.size())
                return na.size() < nb.size() ? -1 : 1;
            if (const int c = na.compare(nb); c != 0)
                return c < 0 ? -1 : 1;
            continue;
        }

        const QChar fa = ca.toCaseFolded();
        const QChar fb = cb.toCaseFolded();
        if (fa != fb)
            return fa < fb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    // Identical ignoring case — fall back to a case-sensitive tie-break so the
    // order of "README" vs "readme" stays stable instead of depending on chance.
    const int c = a.compare(b, Qt::CaseSensitive);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

// ─── Which files are logs ────────────────────────────────────────────────────

namespace {

bool isRotationSeparator(QChar c)
{
    return c == u'.' || c == u'-' || c == u'_';
}

// Empty, or separator + digits, then any more (separator + digits) groups.
bool isRotationTail(QStringView tail)
{
    qsizetype i = 0;
    while (i < tail.size()) {
        if (!isRotationSeparator(tail[i]))
            return false;
        ++i;
        const qsizetype digitsStart = i;
        while (i < tail.size() && tail[i] >= u'0' && tail[i] <= u'9')
            ++i;
        if (i == digitsStart)
            return false;
    }
    return true;
}

} // namespace

bool scannerNameMatches(QStringView fileName, const QStringList& extensions)
{
    if (extensions.isEmpty())
        return true;
    const QString name = fileName.toString().toLower();
    const auto matches = [&extensions](QStringView candidate) {
        for (const QString& ext : extensions) {
            const QString component = u'.' + ext;
            for (qsizetype at = candidate.indexOf(component); at >= 0;
                 at = candidate.indexOf(component, at + 1)) {
                if (isRotationTail(candidate.sliced(at + component.size())))
                    return true;
            }
        }
        return false;
    };
    if (matches(name))
        return true;   // в том числе расширение из настроек вида «gz» или «log.gz»
    // Сжатый лог — по имени без «.gz» (содержимое читается через GzipDevice).
    static const QString kGzip = QStringLiteral(".gz");
    return name.endsWith(kGzip) && matches(QStringView(name).chopped(kGzip.size()));
}

// ─── Constructor / destructor ────────────────────────────────────────────────

DirectoryScanner::DirectoryScanner(QTreeWidget* tree, QObject* parent)
    : QObject(parent), m_tree(tree)
{
    setupTreeWidget();

    m_contentPool.setMaxThreadCount(maxContentSearchThreads());

    m_contentProgressTimer = new QTimer(this);
    m_contentProgressTimer->setInterval(100);
    connect(m_contentProgressTimer, &QTimer::timeout, this, [this]() {
        if (!m_contentProgress)
            return;
        emit contentFilterProgress(m_contentProgress->bytesDone.load(std::memory_order_relaxed),
                                   m_contentTotalBytes,
                                   m_contentProgress->filesDone.load(std::memory_order_relaxed),
                                   m_contentTodo ? int(m_contentTodo->size()) : 0);
    });

    // File-system events arrive in bursts (a writer flushing, an archiver
    // rotating a dozen files) — collapse them into a single refresh.
    m_watchDebounce = new QTimer(this);
    m_watchDebounce->setSingleShot(true);
    m_watchDebounce->setInterval(1500);
    connect(m_watchDebounce, &QTimer::timeout, this, &DirectoryScanner::rescan);

    // Heartbeat for Auto mode: catches what the watcher can miss (network
    // shares, files appended to without a directory notification).
    m_periodicTimer = new QTimer(this);
    connect(m_periodicTimer, &QTimer::timeout, this, [this]() {
        // Refreshing a dock nobody is looking at only burns I/O; remember that
        // something is due and let the panel ask for it when it reappears.
        if (!m_tree->isVisible()) {
            setPendingChanges(true);
            return;
        }
        rescan();
    });
}

DirectoryScanner::~DirectoryScanner()
{
    m_statsCancel->store(true);
    cancelContentSearch();
    m_contentPool.clear();
    m_contentPool.waitForDone();
}

// ─── Public API ──────────────────────────────────────────────────────────────

void DirectoryScanner::setFileExtensions(const QStringList& extensions)
{
    m_extensions.clear();
    for (const QString& ext : extensions) {
        QString e = ext.trimmed().toLower();
        while (e.startsWith(u'.'))
            e.remove(0, 1);
        if (!e.isEmpty())
            m_extensions.append(e);
    }
}

void DirectoryScanner::scan(const QString& rootPath)
{
    cancelContentSearch();

    // Invalidate everything still in flight from the previous root.
    ++m_generation;
    m_rescanInFlight = false;
    m_watchDebounce->stop();

    m_tree->clear();
    m_itemMap.clear();
    m_dirMap.clear();
    m_scanState.clear();
    m_pending.clear();
    // Already-running workers stop at their next chunk and call applyStats(),
    // which silently discards results because m_itemMap no longer contains
    // their paths.
    m_statsCancel->store(true);
    m_statsCancel = std::make_shared<std::atomic<bool>>(false);

    // Reset accumulated date bounds and the content-filter result cache; the
    // date-filter window itself is left to the panel to re-seed from the new
    // bounds once results start arriving.
    m_globalMin = QDateTime();
    m_globalMax = QDateTime();
    m_contentFilterActive = false;
    m_contentMatches.clear();
    m_contentCache.clear();
    m_contentCacheKey.clear();
    emit dateBoundsChanged(m_globalMin, m_globalMax);

    const QString cleaned = QDir::cleanPath(rootPath);
    m_rootPath = QDir(cleaned).exists() ? cleaned : QString();
    setPendingChanges(false);
    applyRefreshMode();   // (re)arms the watcher and the Auto-mode heartbeat

    if (m_rootPath.isEmpty())
        return;

    populateLevelAsync(m_rootPath);
}

// ─── Tree setup ──────────────────────────────────────────────────────────────

void DirectoryScanner::setupTreeWidget()
{
    const QStringList headers = {
        tr("Name"), tr("Entries"), tr("From"), tr("To"),
        tr("W / E / F"), tr("Size")
    };
    m_tree->setColumnCount(ScanCol::Count);
    m_tree->setHeaderLabels(headers);

    // Slightly smaller font so more entries fit on screen
    QFont f = m_tree->font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    m_tree->setFont(f);
    // Keep header font at the default size for readability
    QFont hdrFont = m_tree->header()->font();
    m_tree->header()->setFont(hdrFont);

    QHeaderView* hdr = m_tree->header();
    hdr->setSectionResizeMode(ScanCol::Name, QHeaderView::Interactive);
    hdr->resizeSection(ScanCol::Name, 200);
    hdr->setStretchLastSection(false);
    for (int c = 1; c < ScanCol::Count; ++c)
        hdr->setSectionResizeMode(c, QHeaderView::Interactive);

    // Apply custom delegate for the W / E / F column
    m_tree->setItemDelegateForColumn(ScanCol::Alerts, new ScannerDelegate(m_tree));

    // Disable auto-sort on data change (prevents layout storms during scan).
    // NOTE: setSortingEnabled(false) internally calls setSectionsClickable(false),
    // so we MUST call setSectionsClickable(true) AFTER to keep header clicks working.
    m_tree->setSortingEnabled(false);
    hdr->setSectionsClickable(true);
    hdr->setSortIndicatorShown(true);

    m_tree->setIndentation(15);
    m_tree->setItemsExpandable(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(hdr,    &QHeaderView::sectionClicked,
            this,   &DirectoryScanner::onHeaderClicked);
    connect(m_tree, &QTreeWidget::itemExpanded,
            this,   &DirectoryScanner::onItemExpanded);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this,   &DirectoryScanner::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this,   &DirectoryScanner::onContextMenuRequested);
}

// ─── Tree population (one level) ─────────────────────────────────────────────

DirectoryScanner::DirListing DirectoryScanner::listDirectory(const QString& dirPath)
{
    DirListing listing;
    listing.dirPath = dirPath;

    QDir dir(dirPath);
    listing.readable = dir.exists();
    if (!listing.readable)
        return listing;

    dir.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    // Sorting here would be case-sensitive (QDir::Name); order the names
    // ourselves below so the tree matches what ScannerItem::operator< does.
    dir.setSorting(QDir::NoSort);

    const QFileInfoList infos = dir.entryInfoList();
    listing.entries.reserve(infos.size());
    for (const QFileInfo& info : infos) {
        DirEntryInfo e;
        e.name    = info.fileName();
        e.path    = info.filePath();
        e.isFile  = info.isFile();
        e.size    = info.size();
        e.mtimeMs = info.lastModified().toMSecsSinceEpoch();
        listing.entries.append(e);
    }

    std::sort(listing.entries.begin(), listing.entries.end(),
              [](const DirEntryInfo& a, const DirEntryInfo& b) {
        if (a.isFile != b.isFile)
            return !a.isFile;                     // directories first
        return scannerCompareNames(a.name, b.name) < 0;
    });
    return listing;
}

void DirectoryScanner::populateLevelAsync(const QString& dirPath)
{
    const int generation = m_generation;

    auto* watcher = new QFutureWatcher<DirListing>(this);
    connect(watcher, &QFutureWatcher<DirListing>::finished, this, [this, watcher, generation]() {
        const DirListing listing = watcher->result();
        watcher->deleteLater();
        // The tree may have been re-rooted (or cleared) while we were listing;
        // resolving the parent by path — never by a stashed pointer — keeps
        // stale results from writing into freed items.
        if (generation != m_generation)
            return;
        if (QTreeWidgetItem* parent = itemForDir(listing.dirPath))
            handlePopulateResult(listing, parent);
    });
    watcher->setFuture(QtConcurrent::run(&DirectoryScanner::listDirectory, dirPath));
}

QTreeWidgetItem* DirectoryScanner::itemForDir(const QString& dirPath) const
{
    if (dirPath == m_rootPath)
        return m_tree->invisibleRootItem();
    return m_dirMap.value(dirPath, nullptr);
}

QTreeWidgetItem* DirectoryScanner::createEntryItem(const DirEntryInfo& info, QTreeWidgetItem* parent)
{
    auto* item = new ScannerItem(parent);
    item->setText(ScanCol::Name, info.name);
    item->setToolTip(ScanCol::Name, info.path);
    item->setData(ScanCol::Name, ScanRole::FilePath, info.path);
    item->setData(ScanCol::Name, ScanRole::IsFile,   info.isFile);

    if (!info.isFile) {
        item->setData(ScanCol::Name, ScanRole::Populated, false);
        // A placeholder child makes the expand-arrow visible without
        // recursing into subdirectories prematurely.
        auto* ph = new QTreeWidgetItem(item);
        ph->setText(ScanCol::Name, tr("…"));   // "…"
        ph->setFlags(Qt::NoItemFlags);              // not interactive
        m_dirMap.insert(info.path, item);
        return item;
    }

    m_itemMap.insert(info.path, item);
    queueFileScan(item, info, /*allowResume=*/false);
    return item;
}

void DirectoryScanner::handlePopulateResult(const DirListing& listing, QTreeWidgetItem* parent)
{
    // Reconciling (rather than blindly appending) keeps this idempotent, so a
    // refresh that lands while a lazy expand is still in flight can't duplicate
    // the level.
    int added = 0, removed = 0, updated = 0;

    // Batch UI updates to avoid repaint/layout storms on huge directories.
    m_tree->setUpdatesEnabled(false);
    reconcileLevel(parent, listing, added, removed, updated);
    m_tree->setUpdatesEnabled(true);
    m_tree->viewport()->update();

    scheduleScans();
    scheduleResort();
    updateWatchedPaths();
    scheduleFilterRefresh();
}

void DirectoryScanner::forgetItem(QTreeWidgetItem* item)
{
    const QString path = item->data(ScanCol::Name, ScanRole::FilePath).toString();
    if (!path.isEmpty()) {
        m_itemMap.remove(path);
        m_dirMap.remove(path);
        m_scanState.remove(path);
        m_contentMatches.remove(path);
        m_contentCache.remove(path);
        m_pending.removeIf([&path](const ScanRequest& r) { return r.path == path; });
    }
    for (int i = 0; i < item->childCount(); ++i)
        forgetItem(item->child(i));
}

// ─── Parallel scanning pool ───────────────────────────────────────────────────

// Queue `item`'s file for (re-)analysis and show it as pending in the tree.
// With allowResume the previously collected stats are kept as a base and only
// the bytes appended since the last scan are read.
void DirectoryScanner::queueFileScan(QTreeWidgetItem* item, const DirEntryInfo& info, bool allowResume)
{
    // A file queued twice (refresh landing on an already-pending file) must be
    // analysed once, with the newest request winning.
    m_pending.removeIf([&info](const ScanRequest& r) { return r.path == info.path; });

    ScanRequest request;
    request.path = info.path;

    // Growing log: keep the numbers we already have and read only the tail.
    // The worker still verifies the prefix is byte-identical before trusting it.
    if (allowResume) {
        const auto known = m_scanState.constFind(info.path);
        if (known != m_scanState.cend() && known->stats.parseSuccess
            && known->resume.offset > 0 && known->anchor.valid
            && info.size > known->resume.offset) {
            request.resume      = known->resume;
            request.anchor      = known->anchor;
            request.base        = known->complete;
        }
    }

    item->setData(ScanCol::Name, ScanRole::FileSize,  info.size);
    item->setData(ScanCol::Name, ScanRole::FileMTime, info.mtimeMs);
    item->setText(ScanCol::Size, QLocale().toString(info.size));
    item->setData(ScanCol::Size, ScanRole::SortKey, info.size);
    item->setText(ScanCol::Entries, tr("Queued…"));
    if (request.resume.offset == 0) {
        // Full re-read: the old numbers no longer describe the file.
        m_scanState.remove(info.path);
        item->setData(ScanCol::Entries, ScanRole::SortKey, QVariant());
        for (int c = ScanCol::From; c <= ScanCol::Alerts; ++c) {
            item->setText(c, QStringLiteral("—"));
            item->setData(c, ScanRole::SortKey, QVariant());
        }
    }

    m_pending.append(request);
}

void DirectoryScanner::scheduleScans()
{
    while (m_activeScans < maxConcurrentScans() && !m_pending.isEmpty())
        dispatchOneScan(m_pending.takeFirst());
}

void DirectoryScanner::dispatchOneScan(const ScanRequest& request)
{
    if (QTreeWidgetItem* item = m_itemMap.value(request.path))
        item->setText(ScanCol::Entries, tr("Scanning…"));

    ++m_activeScans;

    // Each worker creates its own LogParser to avoid any shared-state concerns.
    auto* watcher = new QFutureWatcher<ScanResult>(this);
    connect(watcher, &QFutureWatcher<ScanResult>::finished, this, [this, watcher]() {
        const ScanResult res = watcher->result();
        watcher->deleteLater();
        --m_activeScans;
        applyStats(res);
        scheduleScans();  // fill the freed slot from the queue
    });
    watcher->setFuture(QtConcurrent::run([request, cancel = m_statsCancel]() -> ScanResult {
        // Records are counted the way a tab without field extraction splits
        // them (timestamp + level), so the scanner needs no schema.
        LogParser parser;

        ScanResult result;
        result.path = request.path;

        // Append-only growth: verify the already-analysed prefix is still
        // byte-identical, then read just the tail and fold it into the old
        // numbers. Anything else (rewrite, rotation) needs a full pass.
        LogParser::StatsScan scan;
        bool resumed = false;
        if (request.resume.offset > 0) {
            if (FileChangeDetector::classify(request.path, request.anchor)
                == FileChangeDetector::Change::Appended) {
                scan = parser.analyzeFileForStats(request.path, request.resume, cancel.get());
                if (scan.stats.parseSuccess) {
                    scan.stats    = LogParser::mergeStats(request.base, scan.stats);
                    scan.complete = LogParser::mergeStats(request.base, scan.complete);
                    resumed = true;
                }
            }
        }
        if (!resumed)
            scan = parser.analyzeFileForStats(request.path, LogParser::StatsResume(), cancel.get());

        result.state.stats    = scan.stats;
        result.state.complete = scan.complete;

        // Anchor for the *next* refresh, captured here so the GUI thread never
        // reads from disk: right after the last whole line, so a line the
        // writer has not finished yet is read again once it is complete.
        if (scan.stats.parseSuccess && scan.next.offset > 0) {
            const FileChangeDetector::Anchor anchor =
                FileChangeDetector::capture(request.path, scan.next.offset);
            if (anchor.valid) {
                result.state.resume = scan.next;
                result.state.anchor = anchor;
            }
        }
        return result;
    }));
}

void DirectoryScanner::applyStats(const ScanResult& result)
{
    const QString& filePath = result.path;
    const FileStats& stats  = result.state.stats;

    QTreeWidgetItem* item = m_itemMap.value(filePath);
    if (!item) return;  // Tree was cleared while scan was in flight

    if (stats.parseSuccess) {
        m_scanState.insert(filePath, result.state);

        item->setText(ScanCol::Entries, QString::number(stats.totalEntries));
        item->setData(ScanCol::Entries, ScanRole::SortKey, (qint64)stats.totalEntries);

        auto applyTime = [&](int col, const QDateTime& dt) {
            if (dt.isValid()) {
                item->setText(col, dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                item->setData(col, ScanRole::SortKey, dt.toMSecsSinceEpoch());
            } else {
                item->setText(col, QStringLiteral("N/A"));
                item->setData(col, ScanRole::SortKey, QVariant());
            }
        };
        applyTime(ScanCol::From, stats.firstEntryTimestamp);
        applyTime(ScanCol::To,   stats.lastEntryTimestamp);

        // Widen the global date bounds used to seed the date filter window.
        if (stats.firstEntryTimestamp.isValid()
            && (!m_globalMin.isValid() || stats.firstEntryTimestamp < m_globalMin))
            m_globalMin = stats.firstEntryTimestamp;
        if (stats.lastEntryTimestamp.isValid()
            && (!m_globalMax.isValid() || stats.lastEntryTimestamp > m_globalMax))
            m_globalMax = stats.lastEntryTimestamp;
        scheduleBoundsUpdate();

        // Composite key: true lexicographic sort — fatals > errors > warns.
        // Multipliers are large enough that even 999 999 errors can't reach
        // the weight of a single fatal (and same for warns vs errors).
        const qint64 alertKey = (qint64)stats.fatalCount * 1'000'000'000'000LL
                              + (qint64)stats.errorCount * 1'000'000LL
                              + (qint64)stats.warnCount;
        item->setText(ScanCol::Alerts,
                      formatAlerts(stats.fatalCount, stats.errorCount, stats.warnCount));
        item->setData(ScanCol::Alerts, ScanRole::SortKey, alertKey);
    } else {
        m_scanState.remove(filePath);
        for (int c = ScanCol::Entries; c < ScanCol::Size; ++c)
            item->setText(c, tr("Parse Failed"));
    }
    item->setToolTip(ScanCol::Name, buildTooltip(stats, filePath));

    scheduleResort();
    scheduleResize();
}

void DirectoryScanner::scheduleResort()
{
    if (m_sortColumn < 0 || m_resortScheduled)
        return;

    m_resortScheduled = true;
    QTimer::singleShot(120, this, [this]() {
        m_resortScheduled = false;
        // QTreeWidget::sortItems() calls model()->sort() directly and works
        // regardless of setSortingEnabled state.
        m_tree->sortItems(m_sortColumn, m_sortOrder);
    });
}

void DirectoryScanner::scheduleResize()
{
    if (m_resizeScheduled)
        return;

    m_resizeScheduled = true;
    QTimer::singleShot(500, this, [this]() {
        m_resizeScheduled = false;

        QHeaderView* hdr = m_tree->header();
        Q_UNUSED(hdr);
        // Resize all columns except Name to contents
        for (int c = 1; c < ScanCol::Count; ++c) {
            m_tree->resizeColumnToContents(c);
        }

        // Keep the date filter consistent as fresh stats stream in. The content
        // filter is left untouched here (it only re-evaluates on Apply).
        if (m_dateFilterEnabled)
            refreshAllVisibility();
    });
}

// Debounced emission of the global date bounds so the panel can seed/extend the
// date-filter window without a signal storm during a busy scan.
void DirectoryScanner::scheduleBoundsUpdate()
{
    if (m_boundsScheduled)
        return;
    m_boundsScheduled = true;
    QTimer::singleShot(200, this, [this]() {
        m_boundsScheduled = false;
        emit dateBoundsChanged(m_globalMin, m_globalMax);
    });
}

// After files disappear the accumulated min/max may be too wide — rebuild them
// from what is actually left in the tree.
void DirectoryScanner::recomputeDateBounds()
{
    QDateTime lo, hi;
    for (auto it = m_scanState.cbegin(); it != m_scanState.cend(); ++it) {
        const FileStats& s = it.value().stats;
        if (s.firstEntryTimestamp.isValid() && (!lo.isValid() || s.firstEntryTimestamp < lo))
            lo = s.firstEntryTimestamp;
        if (s.lastEntryTimestamp.isValid() && (!hi.isValid() || s.lastEntryTimestamp > hi))
            hi = s.lastEntryTimestamp;
    }
    if (lo == m_globalMin && hi == m_globalMax)
        return;
    m_globalMin = lo;
    m_globalMax = hi;
    scheduleBoundsUpdate();
}

// ─── Incremental refresh ──────────────────────────────────────────────────────

void DirectoryScanner::rescan()
{
    m_watchDebounce->stop();
    if (m_rootPath.isEmpty() || m_rescanInFlight)
        return;

    // Only levels the user has actually opened are re-listed; collapsed
    // directories are re-read lazily when they are expanded again.
    QStringList dirs;
    dirs.reserve(m_dirMap.size() + 1);
    dirs.append(m_rootPath);
    for (auto it = m_dirMap.cbegin(); it != m_dirMap.cend(); ++it) {
        if (it.value()->data(ScanCol::Name, ScanRole::Populated).toBool())
            dirs.append(it.key());
    }

    m_rescanInFlight = true;
    setPendingChanges(false);
    emit rescanStarted();

    const int generation = m_generation;
    auto* watcher = new QFutureWatcher<QList<DirListing>>(this);
    connect(watcher, &QFutureWatcher<QList<DirListing>>::finished, this, [this, watcher, generation]() {
        const QList<DirListing> listings = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation)
            return;   // a fresh scan() superseded this refresh
        m_rescanInFlight = false;
        applyRescanResult(listings);
    });
    watcher->setFuture(QtConcurrent::run([dirs]() {
        QList<DirListing> listings;
        listings.reserve(dirs.size());
        for (const QString& dir : dirs)
            listings.append(listDirectory(dir));
        return listings;
    }));
}

void DirectoryScanner::applyRescanResult(const QList<DirListing>& listings)
{
    int added = 0, removed = 0, updated = 0;

    m_tree->setUpdatesEnabled(false);
    for (const DirListing& listing : listings) {
        QTreeWidgetItem* parent = itemForDir(listing.dirPath);
        if (!parent)
            continue;   // collapsed or deleted while we were listing
        if (!listing.readable) {
            // The directory itself is gone; drop it (the root just empties out).
            if (parent != m_tree->invisibleRootItem()) {
                forgetItem(parent);
                delete parent;
                ++removed;
            }
            continue;
        }
        reconcileLevel(parent, listing, added, removed, updated);
    }
    m_tree->setUpdatesEnabled(true);
    m_tree->viewport()->update();

    if (removed > 0)
        recomputeDateBounds();
    if (added > 0 || updated > 0)
        scheduleScans();
    scheduleResort();
    scheduleResize();
    updateWatchedPaths();

    // Newly added / changed files have no verdict from the current content
    // filter yet, so re-run it (cached files are skipped, so this is cheap).
    if (added > 0 || removed > 0 || updated > 0)
        scheduleFilterRefresh();

    emit rescanFinished(added, removed, updated);
}

void DirectoryScanner::reconcileLevel(QTreeWidgetItem* parent, const DirListing& listing,
                                      int& added, int& removed, int& updated)
{
    // Index the level as it currently stands. The "…" placeholder under an
    // unexpanded directory carries no path and is skipped, so it survives.
    QHash<QString, QTreeWidgetItem*> existing;
    existing.reserve(parent->childCount());
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem* child = parent->child(i);
        const QString path = child->data(ScanCol::Name, ScanRole::FilePath).toString();
        if (!path.isEmpty())
            existing.insert(path, child);
    }

    QSet<QString> onDisk;
    onDisk.reserve(listing.entries.size());
    for (const DirEntryInfo& info : listing.entries) {
        if (info.isFile && !matchesExtension(info.name))
            continue;
        onDisk.insert(info.path);

        QTreeWidgetItem* item = existing.value(info.path, nullptr);
        if (!item) {
            createEntryItem(info, parent);
            ++added;
            continue;
        }
        if (!info.isFile)
            continue;   // directories carry no stats of their own

        // Size *and* mtime unchanged ⇒ the analysis still holds.
        if (item->data(ScanCol::Name, ScanRole::FileSize).toLongLong()  == info.size &&
            item->data(ScanCol::Name, ScanRole::FileMTime).toLongLong() == info.mtimeMs)
            continue;

        queueFileScan(item, info, /*allowResume=*/true);
        ++updated;
    }

    for (auto it = existing.cbegin(); it != existing.cend(); ++it) {
        if (onDisk.contains(it.key()))
            continue;
        forgetItem(it.value());
        delete it.value();
        ++removed;
    }
}

// Watch the root plus every expanded level, so both new files and edits inside
// an open directory raise an event.
void DirectoryScanner::updateWatchedPaths()
{
    const bool wanted = !m_rootPath.isEmpty() && m_refreshMode != RefreshMode::Off;
    if (!wanted) {
        if (m_watcher) {
            m_watcher->deleteLater();
            m_watcher = nullptr;
        }
        return;
    }

    if (!m_watcher) {
        m_watcher = new QFileSystemWatcher(this);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged,
                this, &DirectoryScanner::onWatchedPathChanged);
    }

    // Each watched directory costs an OS handle; a deep tree that the user has
    // opened everywhere is not worth thousands of them.
    constexpr int kMaxWatchedDirs = 256;
    QStringList wantedPaths;
    wantedPaths.reserve(m_dirMap.size() + 1);
    wantedPaths.append(m_rootPath);
    for (auto it = m_dirMap.cbegin(); it != m_dirMap.cend() && wantedPaths.size() < kMaxWatchedDirs; ++it) {
        if (it.value()->data(ScanCol::Name, ScanRole::Populated).toBool())
            wantedPaths.append(it.key());
    }

    const QStringList current = m_watcher->directories();
    QStringList toAdd;
    for (const QString& path : wantedPaths)
        if (!current.contains(path))
            toAdd.append(path);
    QStringList toRemove;
    for (const QString& path : current)
        if (!wantedPaths.contains(path))
            toRemove.append(path);

    if (!toRemove.isEmpty())
        m_watcher->removePaths(toRemove);
    if (!toAdd.isEmpty())
        m_watcher->addPaths(toAdd);
}

void DirectoryScanner::setRefreshMode(RefreshMode mode)
{
    if (m_refreshMode == mode)
        return;
    m_refreshMode = mode;
    applyRefreshMode();
}

void DirectoryScanner::setAutoRefreshIntervalSecs(int secs)
{
    const int clamped = qBound(5, secs, 3600);
    if (m_autoRefreshSecs == clamped)
        return;
    m_autoRefreshSecs = clamped;
    applyRefreshMode();
}

void DirectoryScanner::applyRefreshMode()
{
    updateWatchedPaths();

    if (m_refreshMode == RefreshMode::Auto && !m_rootPath.isEmpty()) {
        m_periodicTimer->start(m_autoRefreshSecs * 1000);
    } else {
        m_periodicTimer->stop();
        m_watchDebounce->stop();
    }
    if (m_refreshMode == RefreshMode::Off)
        setPendingChanges(false);
}

void DirectoryScanner::setPendingChanges(bool pending)
{
    if (m_pendingChanges == pending)
        return;
    m_pendingChanges = pending;
    emit pendingChangesChanged(pending);
}

void DirectoryScanner::onWatchedPathChanged()
{
    switch (m_refreshMode) {
    case RefreshMode::Off:
        break;
    case RefreshMode::Auto:
        m_watchDebounce->start();
        break;
    case RefreshMode::Manual:
        // Don't touch the tree under the user's cursor — just light up the
        // refresh button so they can pull the change in when they want it.
        setPendingChanges(true);
        break;
    }
}

// ─── Filtering ────────────────────────────────────────────────────────────────

DirectoryScanner::ContentVerdict
DirectoryScanner::ContentMatcher::operator()(const SearchItem& item) const
{
    ContentVerdict verdict;
    qint64 counted = 0;   // байты этого файла в общем счётчике (воркеров несколько)

    if (!progress->cancelled.load(std::memory_order_relaxed)) {
        FileContentSearch::Options options;
        options.fallback  = fallback;
        options.cancelled = &progress->cancelled;
        options.bytesDone = &progress->bytesDone;
        const FileContentSearch::Result result = FileContentSearch::search(item.path, query, options);
        verdict.matched = result.matched;
        verdict.dependsOnFallback = result.dependsOnFallback;
        counted = result.bytesRead;
    }

    // A file we bailed out of early (match found, unreadable, cancelled) still
    // has to account for its full declared size, otherwise the progress bar
    // would never reach the end.
    if (counted < item.size)
        progress->bytesDone.fetch_add(item.size - counted, std::memory_order_relaxed);
    progress->filesDone.fetch_add(1, std::memory_order_relaxed);
    return verdict;
}

void DirectoryScanner::setDateFilter(bool enabled, const QDateTime& from, const QDateTime& to)
{
    m_dateFilterEnabled = enabled;
    m_dateFrom = from;
    m_dateTo   = to;
}

void DirectoryScanner::setContentFilter(const QString& text, bool isRegex, bool caseSensitive)
{
    m_contentText          = text;
    m_contentRegex         = isRegex;
    m_contentCaseSensitive = caseSensitive;
}

void DirectoryScanner::applyFilters()
{
    cancelContentSearch();

    if (m_contentText.isEmpty()) {
        // No content filter — just apply the (cheap) date filter immediately.
        m_contentFilterActive = false;
        m_contentMatches.clear();
        refreshAllVisibility();
        emit contentFilterFinished(0, 0);
        return;
    }
    startContentSearch();
}

void DirectoryScanner::clearFilters()
{
    cancelContentSearch();
    m_dateFilterEnabled   = false;
    m_contentText.clear();
    m_contentFilterActive = false;
    m_contentMatches.clear();
    refreshAllVisibility();
}

void DirectoryScanner::setFallbackEncoding(TextEncoding encoding)
{
    if (encoding == m_fallbackEncoding)
        return;
    m_fallbackEncoding = encoding;
    // Вердикты файлов, решённых автоматикой в однобайтную кодировку, могли
    // измениться; ASCII, UTF-8 и файлы с BOM от запасной не зависят.
    int dropped = 0;
    for (auto it = m_contentCache.begin(); it != m_contentCache.end();) {
        if (it->dependsOnFallback) {
            it = m_contentCache.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    // Идущий поиск читает с прежней запасной — заново; применённый фильтр —
    // только если какой-то вердикт от неё зависел (остальные — из кэша).
    if (!m_contentText.isEmpty()
        && (m_contentWatcher || (m_contentFilterActive && dropped > 0))) {
        cancelContentSearch();
        startContentSearch();
    }
}

void DirectoryScanner::cancelContentSearch()
{
    m_contentProgressTimer->stop();
    if (!m_contentWatcher)
        return;
    // Workers poll this between chunks, so they stop within a chunk rather than
    // after finishing the file they happen to be halfway through.
    if (m_contentProgress)
        m_contentProgress->cancelled.store(true, std::memory_order_relaxed);
    m_contentProgress.reset();

    m_contentWatcher->disconnect(this);
    m_contentWatcher->cancel();
    // Detach: let the cancelled future wind down and free itself without
    // blocking the UI thread.
    QFutureWatcher<ContentVerdict>* w = m_contentWatcher;
    m_contentWatcher = nullptr;
    connect(w, &QFutureWatcher<ContentVerdict>::finished, w, &QObject::deleteLater);
}

QString DirectoryScanner::contentQueryKey() const
{
    return QStringLiteral("%1|%2|%3")
        .arg(m_contentRegex ? 1 : 0)
        .arg(m_contentCaseSensitive ? 1 : 0)
        .arg(m_contentText);
}

void DirectoryScanner::startContentSearch()
{
    // A different query invalidates every cached verdict.
    const QString key = contentQueryKey();
    if (key != m_contentCacheKey) {
        m_contentCache.clear();
        m_contentCacheKey = key;
    }

    // Build the candidate set. Two cheap wins before any file is opened:
    //   • files the date filter already rejects are never read;
    //   • files whose size and mtime match a cached verdict reuse it, so
    //     re-applying a filter (or applying it again after a refresh) only
    //     touches what actually changed.
    //
    // The list is built afresh and never edited once the workers have it. A
    // cancelled search winds down on its own (cancelContentSearch does not
    // wait), so its workers may still be reading the previous list, or may
    // have just let go of it: reusing that buffer would rely on nothing but
    // the container's reference count to keep the two apart.
    m_contentTodo.reset();
    auto todo = std::make_shared<QList<SearchItem>>();
    m_contentMatches.clear();
    m_contentCandidates = 0;
    m_contentTotalBytes = 0;

    for (auto it = m_itemMap.cbegin(); it != m_itemMap.cend(); ++it) {
        QTreeWidgetItem* item = it.value();
        if (!passesDateFilter(item))
            continue;
        ++m_contentCandidates;

        SearchItem candidate;
        candidate.path    = it.key();
        candidate.size    = item->data(ScanCol::Name, ScanRole::FileSize).toLongLong();
        candidate.mtimeMs = item->data(ScanCol::Name, ScanRole::FileMTime).toLongLong();

        const auto cached = m_contentCache.constFind(candidate.path);
        if (cached != m_contentCache.cend()
            && cached->size == candidate.size && cached->mtimeMs == candidate.mtimeMs) {
            if (cached->matched)
                m_contentMatches.insert(candidate.path);
            continue;
        }
        if (candidate.size <= 0) {
            m_contentCache.insert(candidate.path, { candidate.size, candidate.mtimeMs, false, false });
            continue;
        }

        m_contentTotalBytes += candidate.size;
        todo->append(candidate);
    }

    if (todo->isEmpty()) {
        m_contentFilterActive = true;
        refreshAllVisibility();
        emit contentFilterFinished(m_contentMatches.size(), m_contentCandidates);
        return;
    }

    ContentMatcher matcher;
    matcher.query = ContentQuery(m_contentText, m_contentRegex,
                                 m_contentCaseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive);
    matcher.fallback = m_fallbackEncoding;
    m_contentProgress = std::make_shared<SearchProgress>();
    matcher.progress  = m_contentProgress;
    m_contentTodo = std::move(todo);
    matcher.items = m_contentTodo;

    m_contentWatcher = new QFutureWatcher<ContentVerdict>(this);
    connect(m_contentWatcher, &QFutureWatcher<ContentVerdict>::finished, this, [this]() {
        QFutureWatcher<ContentVerdict>* w = m_contentWatcher;
        m_contentWatcher = nullptr;
        m_contentProgressTimer->stop();
        m_contentProgress.reset();

        const int n = m_contentTodo ? int(m_contentTodo->size()) : 0;
        for (int i = 0; i < n && i < w->future().resultCount(); ++i) {
            const SearchItem& searched = m_contentTodo->at(i);
            const ContentVerdict verdict = w->resultAt(i);
            if (verdict.matched)
                m_contentMatches.insert(searched.path);
            m_contentCache.insert(searched.path, { searched.size, searched.mtimeMs,
                                                   verdict.matched, verdict.dependsOnFallback });
        }
        m_contentFilterActive = true;
        w->deleteLater();

        refreshAllVisibility();
        emit contentFilterFinished(m_contentMatches.size(), m_contentCandidates);
    });

    emit contentFilterProgress(0, m_contentTotalBytes, 0, int(m_contentTodo->size()));
    m_contentProgressTimer->start();
    // mapped() iterates over the shared list; the functor it copies keeps the
    // list alive. A cancelled search keeps running until its workers notice,
    // and the next Apply replaces m_contentTodo immediately — a sequence owned
    // only by this object would be pulled out from under those workers.
    m_contentWatcher->setFuture(
        QtConcurrent::mapped(&m_contentPool, m_contentTodo->cbegin(), m_contentTodo->cend(),
                             matcher));
}

bool DirectoryScanner::passesDateFilter(QTreeWidgetItem* item) const
{
    if (!m_dateFilterEnabled)
        return true;

    const QVariant fromKey = item->data(ScanCol::From, ScanRole::SortKey);
    const QVariant toKey   = item->data(ScanCol::To,   ScanRole::SortKey);
    // A file with no detected timestamps can't be judged — keep it visible.
    if (!fromKey.isValid() && !toKey.isValid())
        return true;

    const QDateTime fileFrom = fromKey.isValid()
        ? QDateTime::fromMSecsSinceEpoch(fromKey.toLongLong()) : QDateTime();
    const QDateTime fileTo = toKey.isValid()
        ? QDateTime::fromMSecsSinceEpoch(toKey.toLongLong())
        : fileFrom;
    const QDateTime fileStart = fileFrom.isValid() ? fileFrom : fileTo;

    // Overlap test against [m_dateFrom, m_dateTo] (either bound may be invalid).
    if (m_dateFrom.isValid() && fileTo.isValid() && fileTo < m_dateFrom)
        return false;
    if (m_dateTo.isValid() && fileStart.isValid() && fileStart > m_dateTo)
        return false;
    return true;
}

bool DirectoryScanner::refreshVisibility(QTreeWidgetItem* item)
{
    const bool isFile = item->data(ScanCol::Name, ScanRole::IsFile).toBool();

    if (isFile) {
        bool visible = passesDateFilter(item);
        if (visible && m_contentFilterActive) {
            const QString path = item->data(ScanCol::Name, ScanRole::FilePath).toString();
            visible = m_contentMatches.contains(path);
        }
        item->setHidden(!visible);
        return visible;
    }

    // Directory: visible if it has any visible descendant. Unpopulated dirs
    // (lazy children) are kept visible so the user can still expand them.
    bool anyChildVisible = false;
    const int count = item->childCount();
    for (int i = 0; i < count; ++i)
        if (refreshVisibility(item->child(i)))
            anyChildVisible = true;

    const bool populated = item->data(ScanCol::Name, ScanRole::Populated).toBool();
    const bool filtering = m_dateFilterEnabled || m_contentFilterActive;
    const bool visible = anyChildVisible || !populated || !filtering;
    item->setHidden(!visible);
    return visible;
}

void DirectoryScanner::refreshAllVisibility()
{
    m_tree->setUpdatesEnabled(false);
    QTreeWidgetItem* root = m_tree->invisibleRootItem();
    for (int i = 0; i < root->childCount(); ++i)
        refreshVisibility(root->child(i));
    m_tree->setUpdatesEnabled(true);
    m_tree->viewport()->update();
}

// The tree gained or lost items (lazy expand, refresh) — bring the filters back
// in line. Debounced so expanding several directories in a row costs one pass.
void DirectoryScanner::scheduleFilterRefresh()
{
    if (m_filterRefreshScheduled)
        return;
    if (!m_dateFilterEnabled && !m_contentFilterActive)
        return;

    m_filterRefreshScheduled = true;
    QTimer::singleShot(200, this, [this]() {
        m_filterRefreshScheduled = false;
        if (m_contentFilterActive && !m_contentText.isEmpty())
            startContentSearch();   // cached verdicts make this nearly free
        else
            refreshAllVisibility();
    });
}

// ─── Slots ───────────────────────────────────────────────────────────────────

void DirectoryScanner::onItemExpanded(QTreeWidgetItem* item)
{
    // Ignore files and directories that have already been populated.
    if (!item) return;
    if (item->data(ScanCol::Name, ScanRole::IsFile).toBool())    return;
    if (item->data(ScanCol::Name, ScanRole::Populated).toBool()) return;

    item->setData(ScanCol::Name, ScanRole::Populated, true);

    // Remove the placeholder child(ren).
    while (item->childCount() > 0)
        delete item->takeChild(0);

    const QString dirPath = item->data(ScanCol::Name, ScanRole::FilePath).toString();
    if (!dirPath.isEmpty())
        populateLevelAsync(dirPath);
}

void DirectoryScanner::onItemDoubleClicked(QTreeWidgetItem* item, int /*col*/)
{
    if (!item || !item->data(ScanCol::Name, ScanRole::IsFile).toBool()) return;
    const QString path = item->data(ScanCol::Name, ScanRole::FilePath).toString();
    if (!path.isEmpty() && QFile::exists(path))
        emit fileActivated(path);
}

void DirectoryScanner::onContextMenuRequested(const QPoint& pos)
{
    QStringList files;
    for (QTreeWidgetItem* sel : m_tree->selectedItems()) {
        if (sel && sel->data(ScanCol::Name, ScanRole::IsFile).toBool()) {
            const QString path = sel->data(ScanCol::Name, ScanRole::FilePath).toString();
            if (!path.isEmpty() && QFile::exists(path))
                files.append(path);
        }
    }

    QMenu menu(m_tree);
    if (!files.isEmpty()) {
        QAction* act = menu.addAction(tr("Open Selected Files (%1)").arg(files.size()));
        connect(act, &QAction::triggered, this, [this, files]() {
            emit filesActivated(files);
        });
        menu.addSeparator();
    }
    QAction* refresh = menu.addAction(tr("Refresh"));
    refresh->setEnabled(!m_rootPath.isEmpty() && !m_rescanInFlight);
    connect(refresh, &QAction::triggered, this, &DirectoryScanner::rescan);

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void DirectoryScanner::onHeaderClicked(int column)
{
    if (column == m_sortColumn) {
        // Toggle: second click on the same column reverses direction.
        m_sortOrder = (m_sortOrder == Qt::AscendingOrder)
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
    } else {
        m_sortColumn = column;
        // Names read best A→Z; every other column is numeric, where the
        // interesting end (most entries, most errors, newest) is the top.
        m_sortOrder  = (column == ScanCol::Name) ? Qt::AscendingOrder
                                                 : Qt::DescendingOrder;
    }
    m_tree->header()->setSortIndicator(m_sortColumn, m_sortOrder);
    m_tree->sortItems(m_sortColumn, m_sortOrder);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

bool DirectoryScanner::matchesExtension(const QString& fileName) const
{
    static const QStringList kDefault{ QStringLiteral("log"), QStringLiteral("txt") };
    return scannerNameMatches(fileName, m_extensions.isEmpty() ? kDefault : m_extensions);
}

QString DirectoryScanner::formatAlerts(int fatals, int errors, int warns)
{
    if (fatals == 0 && errors == 0 && warns == 0)
        return QStringLiteral("—");  // "—"
    // Format: W/E/F — empty string for zero values so delegate can color them
    // Example: "1/2/3", "/3/", "1//"
    return (warns  > 0 ? QString::number(warns)  : QString()) + '/' +
           (errors > 0 ? QString::number(errors) : QString()) + '/' +
           (fatals > 0 ? QString::number(fatals) : QString());
}

QString DirectoryScanner::buildTooltip(const FileStats& stats, const QString& filePath)
{
    const QFileInfo fi(filePath);
    QString tt = fi.fileName() + QStringLiteral("\n") + filePath;
    if (stats.parseSuccess) {
        tt += QStringLiteral("\nEntries: ") + QString::number(stats.totalEntries);
        if (stats.firstEntryTimestamp.isValid())
            tt += QStringLiteral("\nFirst:   ")
                + stats.firstEntryTimestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (stats.lastEntryTimestamp.isValid())
            tt += QStringLiteral("\nLast:    ")
                + stats.lastEntryTimestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (stats.fatalCount > 0)
            tt += QStringLiteral("\nFatals:  ") + QString::number(stats.fatalCount);
        if (stats.errorCount > 0)
            tt += QStringLiteral("\nErrors:  ") + QString::number(stats.errorCount);
        if (stats.warnCount > 0)
            tt += QStringLiteral("\nWarns:   ") + QString::number(stats.warnCount);
        tt += QStringLiteral("\nSize:    ")
            + QLocale().toString(fi.size())
            + QStringLiteral(" bytes");
    } else {
        tt += QStringLiteral("\n(Parse failed)");
    }
    return tt;
}
