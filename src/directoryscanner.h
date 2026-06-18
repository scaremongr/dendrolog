#ifndef DIRECTORYSCANNER_H
#define DIRECTORYSCANNER_H

#include "logparser.h"

#include <QObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTreeWidgetItem>
#include <QFutureWatcher>
#include <QPair>
#include <QTimer>
#include <QDateTime>
#include <Qt>

// ─── Column indices ─────────────────────────────────────────────────────────
// Single source of truth: change column order here only.
namespace ScanCol {
    constexpr int Name    = 0;
    constexpr int Entries = 1;
    constexpr int From    = 2;
    constexpr int To      = 3;
    constexpr int Alerts  = 4;  // Combined Warns / Errors / Fatals
    constexpr int Size    = 5;
    constexpr int Count   = 6;
}

// ─── Custom item data roles ──────────────────────────────────────────────────
namespace ScanRole {
    constexpr int FilePath  = Qt::UserRole;       // QString  – absolute path (files & dirs)
    constexpr int IsFile    = Qt::UserRole + 1;   // bool     – true = log file, false = dir
    constexpr int Populated = Qt::UserRole + 2;   // bool     – dir children already populated
    constexpr int SortKey   = Qt::UserRole + 3;   // qint64   – numeric sort key (per column)
}

// ─── Tree item with numeric / composite sort support ────────────────────────
//
//  Stores a qint64 SortKey in each column so that numeric columns
//  (Entries, From, To, Alerts, Size) sort correctly.
//  The Alerts key encodes severity: F * 10^9 + E * 10^6 + W so that
//  files with fatals always rank above files with only errors/warns.
//
class ScannerItem : public QTreeWidgetItem
{
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    // Override: compare by SortKey when available, else case-insensitive text.
    bool operator<(const QTreeWidgetItem& other) const override
    {
        // Use sortIndicatorSection() directly: it is set by sortItems() BEFORE
        // model()->sort() is called, so it works even when setSortingEnabled(false).
        const QHeaderView* hv = treeWidget() ? treeWidget()->header() : nullptr;
        int col = hv ? hv->sortIndicatorSection() : 0;
        if (col < 0) col = 0;

        bool myIsFile    = data(0, ScanRole::IsFile).toBool();
        bool otherIsFile = other.data(0, ScanRole::IsFile).toBool();
        if (myIsFile != otherIsFile)
            return !myIsFile; // Dirs sort before files

        const QVariant myKey    = data(col, ScanRole::SortKey);
        const QVariant otherKey = other.data(col, ScanRole::SortKey);
        if (myKey.isValid() && otherKey.isValid())
            return myKey.toLongLong() < otherKey.toLongLong();

        return text(col).compare(other.text(col), Qt::CaseInsensitive) < 0;
    }
};

// ─── DirectoryScanner ────────────────────────────────────────────────────────
//
//  Self-contained controller that owns all directory-scanning behaviour:
//    • Tree setup (column headers, sort, resize policy)
//    • Lazy one-level population on item expand
//    • Parallel file-stat analysis (up to idealThreadCount() workers)
//    • Column-click sorting (first click descending; toggle on repeat)
//    • Context menu + double-click → signals to MainWindow
//
//  MainWindow only needs to:
//    1. Create the object and pass the tree widget pointer.
//    2. Call setFileExtensions() / setConversionPattern() as needed.
//    3. Call scan(rootPath) to start a scan.
//    4. Connect fileActivated / filesActivated to open tabs.
//
class DirectoryScanner : public QObject
{
    Q_OBJECT

public:
    explicit DirectoryScanner(QTreeWidget* tree, QObject* parent = nullptr);

    // Call before scan() to pick up the latest settings.
    void setFileExtensions(const QStringList& extensions);
    void setConversionPattern(const QString& pattern);

    // Clears the tree and starts a fresh scan of rootPath.
    // Running workers finish gracefully; stale results are silently discarded.
    void scan(const QString& rootPath);

    // ---- Filtering ----------------------------------------------------------
    // Date filter: keep only files whose [first,last] timestamp range overlaps
    // [from,to]. An invalid QDateTime means "unbounded" on that side; passing
    // both invalid (or enabled=false) disables date filtering. Cheap & sync.
    void setDateFilter(bool enabled, const QDateTime& from, const QDateTime& to);

    // Content filter: keep only files whose contents match `text`. Empty text
    // disables the content filter. Reading file contents is expensive, so the
    // search runs in parallel in the background (see contentFilter* signals).
    void setContentFilter(const QString& text, bool isRegex, bool caseSensitive);

    // Re-evaluate visibility for the current filters. Launches the async content
    // search when a content filter is active; date filtering is applied at once.
    void applyFilters();

    // Drop all filters and show every item.
    void clearFilters();

    // True while a background content search is running.
    bool isContentFilterRunning() const { return m_contentWatcher != nullptr; }

signals:
    void fileActivated(const QString& filePath);
    void filesActivated(const QStringList& filePaths);

    // Emitted as scan results arrive: the earliest "from" and latest "to"
    // timestamps across all scanned files. Invalid dates mean "not yet known".
    void dateBoundsChanged(const QDateTime& earliest, const QDateTime& latest);

    // Background content-search progress / completion.
    void contentFilterProgress(int done, int total);
    void contentFilterFinished(int matched, int total);

private slots:
    void onItemExpanded(QTreeWidgetItem* item);
    void onItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onContextMenuRequested(const QPoint& pos);
    void onHeaderClicked(int column);

private:
    // Data returned by background directory listing.
    struct DirEntryInfo {
        QString name;
        QString path;
        bool isFile;
        qint64 size;
        QString suffix;
    };

    using FileStats  = LogParser::FileStats;
    using StatResult = QPair<FileStats, QString>;  // (stats, filePath)

    // ---- Tree ----
    void setupTreeWidget();
    void populateLevelAsync(const QString& dirPath, QTreeWidgetItem* parent);
    void handlePopulateResult(const QList<DirEntryInfo>& entries, QTreeWidgetItem* parent);

    // ---- Parallel scanning pool ----
    void scheduleScans();
    void dispatchOneScan(const QString& filePath);
    void applyStats(const FileStats& stats, const QString& filePath);
    void scheduleResort();
    void scheduleResize();
    void scheduleBoundsUpdate();

    // ---- Filtering ----
    void cancelContentSearch();
    void startContentSearch();
    bool passesDateFilter(QTreeWidgetItem* item) const;
    // Recursively recompute item visibility; returns true if `item` (or any
    // descendant) stays visible, so empty directories collapse out of view.
    bool refreshVisibility(QTreeWidgetItem* item);
    void refreshAllVisibility();

    // ---- Utilities ----
    bool    matchesExtension(const QString& lowerSuffix) const;
    static QString formatAlerts(int fatals, int errors, int warns);
    static QString buildTooltip(const FileStats& stats, const QString& filePath);

    // ---- State ----
    QTreeWidget*                    m_tree;
    QStringList                     m_extensions;
    QString                         m_pattern;
    QList<QString>                  m_pending;          // Paths waiting to be scanned
    QMap<QString, QTreeWidgetItem*> m_itemMap;           // filePath → tree item (files only)
    int                             m_activeScans = 0;
    int                             m_sortColumn  = -1;
    Qt::SortOrder                   m_sortOrder   = Qt::DescendingOrder;
    bool                            m_resortScheduled = false;
    bool                            m_resizeScheduled = false;
    bool                            m_boundsScheduled = false;

    // ---- Global date bounds across scanned files ----
    QDateTime                       m_globalMin;
    QDateTime                       m_globalMax;

    // ---- Date filter state ----
    bool                            m_dateFilterEnabled = false;
    QDateTime                       m_dateFrom;
    QDateTime                       m_dateTo;

    // ---- Content filter state ----
    QString                         m_contentText;
    bool                            m_contentRegex = false;
    bool                            m_contentCaseSensitive = false;
    bool                            m_contentFilterActive = false;   // results valid
    QSet<QString>                   m_contentMatches;                // matching paths
    QStringList                     m_contentPaths;                  // sequence for mapped()
    QFutureWatcher<bool>*           m_contentWatcher = nullptr;      // running search
};

#endif // DIRECTORYSCANNER_H


