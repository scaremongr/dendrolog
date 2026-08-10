#ifndef DIRECTORYSCANNER_H
#define DIRECTORYSCANNER_H

#include "logparser.h"

#include <QObject>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QThreadPool>
#include <QTreeWidget>
#include <QHeaderView>
#include <QTreeWidgetItem>
#include <QFutureWatcher>
#include <QPair>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <Qt>

#include <atomic>
#include <memory>

class QFileSystemWatcher;

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
    constexpr int FileSize  = Qt::UserRole + 4;   // qint64   – size seen by the last scan
    constexpr int FileMTime = Qt::UserRole + 5;   // qint64   – mtime (ms) seen by the last scan
}

// Natural, case-insensitive name ordering: "Alpha" < "beta" and "log2" < "log10".
// Returns <0 / 0 / >0 like strcmp. Case only breaks exact ties, so the order is
// stable and never depends on how the file system happens to spell a name.
int scannerCompareNames(QStringView a, QStringView b);

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

    // Override: compare by SortKey when available, else natural name order.
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

        return scannerCompareNames(text(col), other.text(col)) < 0;
    }
};

// ─── DirectoryScanner ────────────────────────────────────────────────────────
//
//  Self-contained controller that owns all directory-scanning behaviour:
//    • Tree setup (column headers, sort, resize policy)
//    • Lazy one-level population on item expand
//    • Parallel file-stat analysis (up to idealThreadCount() workers)
//    • Column-click sorting (first click descending; toggle on repeat)
//    • Incremental re-scan (rescan()) plus optional automatic refresh
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
    // How the tree keeps up with changes on disk after the first scan.
    enum class RefreshMode {
        Manual = 0,  // Only rescan() refreshes; changes are watched and flagged
        Auto   = 1,  // Watcher + periodic timer rescan automatically
        Off    = 2   // Nothing is watched; only an explicit rescan() refreshes
    };
    Q_ENUM(RefreshMode)

    explicit DirectoryScanner(QTreeWidget* tree, QObject* parent = nullptr);
    ~DirectoryScanner() override;

    // Call before scan() to pick up the latest settings.
    void setFileExtensions(const QStringList& extensions);
    void setConversionPattern(const QString& pattern);

    // Clears the tree and starts a fresh scan of rootPath.
    // Running workers finish gracefully; stale results are silently discarded.
    void scan(const QString& rootPath);

    QString rootPath() const { return m_rootPath; }

    // ---- Refresh ------------------------------------------------------------
    // Re-reads every directory level that is currently populated and reconciles
    // it with the tree: new files appear, deleted ones disappear and files whose
    // size/mtime moved are re-analysed (append-only growth is read from the last
    // known offset instead of from the top). Untouched files keep their stats,
    // so a rescan over a big tree costs one directory listing per level.
    void rescan();
    bool isRescanning() const { return m_rescanInFlight; }

    void        setRefreshMode(RefreshMode mode);
    RefreshMode refreshMode() const { return m_refreshMode; }

    void setAutoRefreshIntervalSecs(int secs);
    int  autoRefreshIntervalSecs() const { return m_autoRefreshSecs; }

    // True when the watcher saw the directory change but the tree has not been
    // refreshed yet (Manual mode, or Auto mode while the panel is hidden).
    bool hasPendingChanges() const { return m_pendingChanges; }

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

    // Background content-search progress / completion. Progress is measured in
    // bytes because file sizes differ by orders of magnitude — a file counter
    // stalls on the one huge log and then jumps to the end.
    void contentFilterProgress(qint64 bytesDone, qint64 bytesTotal,
                               int filesDone, int filesTotal);
    void contentFilterFinished(int matched, int total);

    // Incremental refresh lifecycle.
    void rescanStarted();
    void rescanFinished(int added, int removed, int updated);
    // The watcher noticed (or stopped seeing) unapplied changes on disk.
    void pendingChangesChanged(bool pending);

private slots:
    void onItemExpanded(QTreeWidgetItem* item);
    void onItemDoubleClicked(QTreeWidgetItem* item, int col);
    void onContextMenuRequested(const QPoint& pos);
    void onHeaderClicked(int column);
    void onWatchedPathChanged();

private:
    using FileStats  = LogParser::FileStats;

    // Data returned by background directory listing.
    struct DirEntryInfo {
        QString name;
        QString path;
        bool    isFile = false;
        qint64  size = 0;
        qint64  mtimeMs = 0;
        QString suffix;
    };

    // One directory's contents, tagged with the directory it came from so the
    // GUI thread can find the owning item again without holding a raw pointer
    // across the async hop (the tree may have been cleared meanwhile).
    struct DirListing {
        QString            dirPath;
        QList<DirEntryInfo> entries;
        bool               readable = true;
    };

    // What the last completed analysis of a file left behind. `resumeOffset` is
    // the byte offset the stats cover, and is only set when that offset is a
    // line boundary — otherwise a tail read would mis-count the split line.
    struct ScanState {
        FileStats stats;
        qint64    resumeOffset = 0;
        quint64   fingerprint  = 0;   // boundary hash of the bytes ending there
    };

    // A queued file analysis. `resumeOffset` > 0 means the previous stats are
    // still valid up to that byte and only the tail has to be read.
    struct ScanRequest {
        QString   path;
        qint64    resumeOffset = 0;
        quint64   fingerprint  = 0;   // boundary hash captured at resumeOffset
        FileStats base;               // stats covering [0, resumeOffset)
    };

    // What a worker hands back: the stats plus a fresh resume anchor for the
    // next refresh (captured on the worker thread, never on the GUI thread).
    struct ScanResult {
        QString   path;
        ScanState state;
    };

    // One file to look inside during a content search, with the size used both
    // for progress accounting and for the result cache.
    struct SearchItem {
        QString path;
        qint64  size = 0;
        qint64  mtimeMs = 0;
    };

    // Result of a previous content search for one file. Reused as long as the
    // file has not moved, so re-applying a filter after a rescan is nearly free.
    struct ContentCacheEntry {
        qint64 size = 0;
        qint64 mtimeMs = 0;
        bool   matched = false;
    };

    // Shared, lock-free progress/cancellation channel between the search
    // workers and the GUI thread (polled by m_contentProgressTimer).
    struct SearchProgress {
        std::atomic<qint64> bytesDone{0};
        std::atomic<int>    filesDone{0};
        std::atomic<bool>   cancelled{false};
    };

    // Functor handed to QtConcurrent::mapped: decides whether one file matches
    // the content filter. Self-contained so it can run on any pool thread.
    struct ContentMatcher {
        using result_type = bool;

        QString             needle;
        bool                isRegex = false;
        Qt::CaseSensitivity cs = Qt::CaseInsensitive;
        QRegularExpression  re;   // precompiled when isRegex
        std::shared_ptr<SearchProgress> progress;

        bool operator()(const SearchItem& item) const;
    };

    // ---- Tree ----
    void setupTreeWidget();
    static DirListing listDirectory(const QString& dirPath);
    void populateLevelAsync(const QString& dirPath);
    void handlePopulateResult(const DirListing& listing, QTreeWidgetItem* parent);
    QTreeWidgetItem* itemForDir(const QString& dirPath) const;
    QTreeWidgetItem* createEntryItem(const DirEntryInfo& info, QTreeWidgetItem* parent);
    void forgetItem(QTreeWidgetItem* item);   // drop item + subtree from the maps

    // ---- Parallel scanning pool ----
    void scheduleScans();
    void dispatchOneScan(const ScanRequest& request);
    void applyStats(const ScanResult& result);
    void queueFileScan(QTreeWidgetItem* item, const DirEntryInfo& info, bool allowResume);
    void scheduleResort();
    void scheduleResize();
    void scheduleBoundsUpdate();
    void recomputeDateBounds();

    // ---- Refresh ----
    void applyRescanResult(const QList<DirListing>& listings);
    void reconcileLevel(QTreeWidgetItem* parent, const DirListing& listing,
                        int& added, int& removed, int& updated);
    void updateWatchedPaths();
    void applyRefreshMode();
    void setPendingChanges(bool pending);

    // ---- Filtering ----
    void cancelContentSearch();
    void startContentSearch();
    QString contentQueryKey() const;
    bool passesDateFilter(QTreeWidgetItem* item) const;
    // Recursively recompute item visibility; returns true if `item` (or any
    // descendant) stays visible, so empty directories collapse out of view.
    bool refreshVisibility(QTreeWidgetItem* item);
    void refreshAllVisibility();
    // Debounced re-evaluation after the tree gained or lost items.
    void scheduleFilterRefresh();

    // ---- Utilities ----
    bool    matchesExtension(const QString& lowerSuffix) const;
    static QString formatAlerts(int fatals, int errors, int warns);
    static QString buildTooltip(const FileStats& stats, const QString& filePath);

    // ---- State ----
    QTreeWidget*                    m_tree;
    QStringList                     m_extensions;
    QString                         m_pattern;
    QString                         m_rootPath;
    QList<ScanRequest>              m_pending;           // Files waiting to be analysed
    QMap<QString, QTreeWidgetItem*> m_itemMap;           // filePath → tree item (files only)
    QHash<QString, QTreeWidgetItem*> m_dirMap;           // dirPath  → tree item (dirs only)
    QHash<QString, ScanState>       m_scanState;         // last analysis result per file
    int                             m_activeScans = 0;
    // Bumped by every scan(); async results tagged with an older generation are
    // dropped instead of being written into a tree that no longer owns them.
    int                             m_generation  = 0;
    int                             m_sortColumn  = -1;
    Qt::SortOrder                   m_sortOrder   = Qt::DescendingOrder;
    bool                            m_resortScheduled = false;
    bool                            m_resizeScheduled = false;
    bool                            m_boundsScheduled = false;
    bool                            m_filterRefreshScheduled = false;

    // ---- Global date bounds across scanned files ----
    QDateTime                       m_globalMin;
    QDateTime                       m_globalMax;

    // ---- Refresh state ----
    RefreshMode                     m_refreshMode = RefreshMode::Manual;
    int                             m_autoRefreshSecs = 30;
    bool                            m_pendingChanges  = false;
    bool                            m_rescanInFlight  = false;
    QFileSystemWatcher*             m_watcher        = nullptr;
    QTimer*                         m_watchDebounce  = nullptr;  // coalesce FS events
    QTimer*                         m_periodicTimer  = nullptr;  // Auto-mode heartbeat

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
    QList<SearchItem>               m_contentTodo;   // files being searched, in result order
    int                             m_contentCandidates = 0;         // incl. cache hits
    qint64                          m_contentTotalBytes = 0;
    QFutureWatcher<bool>*           m_contentWatcher = nullptr;      // running search
    QTimer*                         m_contentProgressTimer = nullptr;
    std::shared_ptr<SearchProgress> m_contentProgress;
    QString                         m_contentCacheKey;               // query the cache holds
    QHash<QString, ContentCacheEntry> m_contentCache;
    // Reading files is I/O-bound and would otherwise queue behind the stat
    // analysis on the global pool — which is exactly what made the progress bar
    // look frozen on machines with few cores.
    QThreadPool                     m_contentPool;
};

#endif // DIRECTORYSCANNER_H
