#include "directoryscanner.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHeaderView>
#include <QLocale>
#include <QMenu>
#include <QThread>
#include <QtConcurrent>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QApplication>
#include <algorithm>
#include "logcolors.h"

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
        static const QColor warnBadge(175, 125,   0);  // dark amber
        static const QColor errBadge (175,  45,  45);  // medium red
        static const QColor fatBadge (110,   0,   0);  // dark crimson
        const QColor sepCol = QColor(130, 130, 130);

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

// ─── Constructor ─────────────────────────────────────────────────────────────

DirectoryScanner::DirectoryScanner(QTreeWidget* tree, QObject* parent)
    : QObject(parent), m_tree(tree)
{
    setupTreeWidget();
}

// ─── Public API ──────────────────────────────────────────────────────────────

void DirectoryScanner::setFileExtensions(const QStringList& extensions)
{
    m_extensions = extensions;
}

void DirectoryScanner::setConversionPattern(const QString& pattern)
{
    m_pattern = pattern;
}

void DirectoryScanner::scan(const QString& rootPath)
{
    m_tree->clear();
    m_itemMap.clear();
    m_pending.clear();
    // Already-running workers will finish and call applyStats(), which silently
    // discards results because m_itemMap no longer contains their paths.

    if (!QDir(rootPath).exists()) return;

    populateLevelAsync(rootPath, m_tree->invisibleRootItem());
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


void DirectoryScanner::populateLevelAsync(const QString& dirPath, QTreeWidgetItem* parent)
{
    // Run directory listing in a background thread
    auto future = QtConcurrent::run([=]() -> QList<DirectoryScanner::DirEntryInfo> {
        QList<DirectoryScanner::DirEntryInfo> entries;
        QDir dir(dirPath);
        dir.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        dir.setSorting(QDir::Name | QDir::DirsFirst);
        for (const QFileInfo& info : dir.entryInfoList()) {
            DirectoryScanner::DirEntryInfo e;
            e.name = info.fileName();
            e.path = info.filePath();
            e.isFile = info.isFile();
            e.size = info.size();
            e.suffix = info.suffix().toLower();
            entries.append(e);
        }
        return entries;
    });
    // When ready, update the tree in the GUI thread
    auto* watcher = new QFutureWatcher<QList<DirectoryScanner::DirEntryInfo>>(this);
    connect(watcher, &QFutureWatcher<QList<DirectoryScanner::DirEntryInfo>>::finished, this, [this, watcher, parent]() {
        QList<DirectoryScanner::DirEntryInfo> entries = watcher->result();
        watcher->deleteLater();
        handlePopulateResult(entries, parent);
    });
    watcher->setFuture(future);
}

void DirectoryScanner::handlePopulateResult(const QList<DirectoryScanner::DirEntryInfo>& entries, QTreeWidgetItem* parent)
{
    // Batch UI updates to avoid repaint/layout storms on huge directories.
    m_tree->setUpdatesEnabled(false);

    for (const DirectoryScanner::DirEntryInfo& info : entries) {
        if (!info.isFile) {
            auto* item = new ScannerItem(parent);
            item->setText(ScanCol::Name, info.name);
            item->setToolTip(ScanCol::Name, info.path);
            item->setData(ScanCol::Name, ScanRole::FilePath,  info.path);
            item->setData(ScanCol::Name, ScanRole::IsFile,    false);
            item->setData(ScanCol::Name, ScanRole::Populated, false);
            // A placeholder child makes the expand-arrow visible without
            // recursing into subdirectories prematurely.
            auto* ph = new QTreeWidgetItem(item);
            ph->setText(ScanCol::Name, tr("\u2026"));   // "…"
            ph->setFlags(Qt::NoItemFlags);              // not interactive
        } else if (matchesExtension(info.suffix)) {
            auto* item = new ScannerItem(parent);
            item->setText(ScanCol::Name, info.name);
            item->setToolTip(ScanCol::Name, info.path);
            item->setData(ScanCol::Name, ScanRole::FilePath, info.path);
            item->setData(ScanCol::Name, ScanRole::IsFile,   true);
            item->setText(ScanCol::Entries, tr("Queued\u2026"));
            for (int c = ScanCol::From; c <= ScanCol::Alerts; ++c)
                item->setText(c, QStringLiteral("\u2014"));   // "—"
            item->setText(ScanCol::Size, QLocale().toString(info.size));
            item->setData(ScanCol::Size, ScanRole::SortKey, info.size);

            m_itemMap.insert(info.path, item);
            m_pending.append(info.path);
        }
    }

    m_tree->setUpdatesEnabled(true);
    m_tree->viewport()->update();

    scheduleScans();
    scheduleResort();
}

// ─── Parallel scanning pool ───────────────────────────────────────────────────

void DirectoryScanner::scheduleScans()
{
    while (m_activeScans < maxConcurrentScans() && !m_pending.isEmpty())
        dispatchOneScan(m_pending.takeFirst());
}

void DirectoryScanner::dispatchOneScan(const QString& filePath)
{
    if (QTreeWidgetItem* item = m_itemMap.value(filePath))
        item->setText(ScanCol::Entries, tr("Scanning\u2026"));

    ++m_activeScans;

    const QString pattern = m_pattern;  // snapshot for worker thread

    // Each worker creates its own LogParser to avoid any shared-state concerns.
    auto* watcher = new QFutureWatcher<StatResult>(this);
    connect(watcher, &QFutureWatcher<StatResult>::finished, this, [this, watcher]() {
        const StatResult res = watcher->result();
        watcher->deleteLater();
        --m_activeScans;
        applyStats(res.first, res.second);
        scheduleScans();  // fill the freed slot from the queue
    });
    watcher->setFuture(QtConcurrent::run([filePath, pattern]() -> StatResult {
        LogParser parser;
        parser.setPattern(pattern);
        return { parser.analyzeFileForStats(filePath), filePath };
    }));
}

void DirectoryScanner::applyStats(const FileStats& stats, const QString& filePath)
{
    QTreeWidgetItem* item = m_itemMap.value(filePath);
    if (!item) return;  // Tree was cleared while scan was in flight

    if (stats.parseSuccess) {
        item->setText(ScanCol::Entries, QString::number(stats.totalEntries));
        item->setData(ScanCol::Entries, ScanRole::SortKey, (qint64)stats.totalEntries);

        auto applyTime = [&](int col, const QDateTime& dt) {
            if (dt.isValid()) {
                item->setText(col, dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
                item->setData(col, ScanRole::SortKey, dt.toMSecsSinceEpoch());
            } else {
                item->setText(col, QStringLiteral("N/A"));
            }
        };
        applyTime(ScanCol::From, stats.firstEntryTimestamp);
        applyTime(ScanCol::To,   stats.lastEntryTimestamp);

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
        // Resize all columns except Name to contents
        for (int c = 1; c < ScanCol::Count; ++c) {
            m_tree->resizeColumnToContents(c);
        }
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
        populateLevelAsync(dirPath, item);
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
    if (files.isEmpty()) return;

    QMenu menu(m_tree);
    QAction* act = menu.addAction(tr("Open Selected Files (%1)").arg(files.size()));
    connect(act, &QAction::triggered, this, [this, files]() {
        emit filesActivated(files);
    });
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
        m_sortOrder  = Qt::DescendingOrder;  // First click: larger values first
    }
    m_tree->header()->setSortIndicator(m_sortColumn, m_sortOrder);
    m_tree->sortItems(m_sortColumn, m_sortOrder);
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

bool DirectoryScanner::matchesExtension(const QString& lowerSuffix) const
{
    if (m_extensions.isEmpty())
        return lowerSuffix == QLatin1String("log")
            || lowerSuffix == QLatin1String("txt");
    return m_extensions.contains(lowerSuffix);
}

QString DirectoryScanner::formatAlerts(int fatals, int errors, int warns)
{
    if (fatals == 0 && errors == 0 && warns == 0)
        return QStringLiteral("\u2014");  // "—"
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
