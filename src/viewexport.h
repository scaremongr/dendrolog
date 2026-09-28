#ifndef VIEWEXPORT_H
#define VIEWEXPORT_H

#include <QFuture>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <atomic>
#include <functional>
#include <memory>

class LogModel;
class QIODevice;
class QTimer;

// Export of the already formatted visible rows. File publication is atomic,
// and open log files (including filesystem aliases) are protected. The
// functions are synchronous and thread-agnostic; ViewExportJob runs them in
// the pool over a snapshot of the view.
namespace ViewExport {

enum class Error { None, SourceFile, IdentityCheck, Open, Write, Commit, Cancelled };

struct Result {
    Error error = Error::None;
    QString detail;
    bool ok() const { return error == Error::None; }
};

// Rows in order: the source calls sink for each row and stops as soon as the
// sink returns false (a write error or cancellation).
using RowSink = std::function<bool(QStringView)>;
using RowSource = std::function<void(const RowSink&)>;
using RowText = std::function<QString(int)>;

// Does not close the device. Checks buffered writes as well as the final flush.
Result writeRows(QIODevice& output, const RowSource& rows,
                 const std::atomic_bool* cancel = nullptr);
Result writeRows(QIODevice& output, int rowCount, const RowText& rowText);
// A cancelled or failed export leaves an existing destination untouched.
Result save(const QString& destination, const QStringList& sourcePaths,
            const RowSource& rows, const std::atomic_bool* cancel = nullptr);
Result save(const QString& destination, const QStringList& sourcePaths,
            int rowCount, const RowText& rowText);

} // namespace ViewExport

// Save View As in the background: the visible rows and the Log Fields
// selection are snapshotted at start (the view may change or close while the
// export runs), the text is formatted exactly as LogModel shows it and
// written by ViewExport::save in the pool. Cancelling keeps the destination
// as it was. The snapshot of a resident model reads entry fields, so a field
// schema switch must cancel (and wait for) a running export first.
class ViewExportJob : public QObject {
    Q_OBJECT

public:
    explicit ViewExportJob(QObject* parent = nullptr);
    // Cancels and waits: QSaveFile must not outlive the application.
    ~ViewExportJob() override;

    // false — an export is already running.
    bool start(const LogModel& model, const QString& destination,
               const QStringList& sourcePaths);
    // finished() still arrives, with Error::Cancelled unless the export had
    // already completed.
    void cancel(bool wait = false);
    bool isRunning() const;
    QString destination() const { return m_destination; }
    int rowCount() const { return m_rowCount; }

signals:
    void progress(int percent);
    void finished(const ViewExport::Result& result);

private:
    QFuture<ViewExport::Result> m_future;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::shared_ptr<std::atomic<int>> m_progress;
    QTimer* m_progressTimer = nullptr;
    QString m_destination;
    int m_rowCount = 0;
    int m_generation = 0;
};

#endif // VIEWEXPORT_H
