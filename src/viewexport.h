#ifndef VIEWEXPORT_H
#define VIEWEXPORT_H

#include <QFuture>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>

class LogModel;
class QIODevice;
struct LogFile;
class QTimer;

// Export of the already formatted visible rows. File publication is atomic,
// and open log files (including filesystem aliases) are protected. The
// functions are synchronous and thread-agnostic; ViewExportJob runs them in
// the pool over a snapshot of the view.
namespace ViewExport {

enum class Error { None, SourceFile, IdentityCheck, Open, Write, Commit, Cancelled };

// F-9. Text — the displayed text, one line per row, as before (platform line
// ends). Csv — RFC 4180: comma, every value quoted only when needed (comma,
// quote, CR or LF inside), quotes doubled, CRLF between records, UTF-8 with a
// BOM (Excel otherwise reads it in the ANSI code page); line breaks inside a
// value stay LF. JsonLines — one JSON object per row, UTF-8 without a BOM, LF;
// streamable and readable by jq, pandas and every log shipper, unlike a JSON
// array, which is invalid until its closing bracket is written.
enum class Format { Text, Csv, JsonLines };

// Format by the file name suffix (.csv; .jsonl, .ndjson; .log, .txt), else fallback.
Format formatForFileName(const QString& fileName, Format fallback = Format::Text);
QString defaultSuffix(Format format);

// The columns of a CSV / JSON Lines export (F-9):
//   • `_time` — the row's moment in ISO 8601 with milliseconds and the local
//     offset (as the view orders rows), empty / null without a timestamp;
//   • `_file` — the source file (displayPath, native separators), only in a
//     tab with more than one file;
//   • the schema columns the view shows (Log Fields selection), or all schema
//     columns when the view shows the raw line;
//   • a Message column is always there: if the selection lacks one, the
//     schema's Message column (case-insensitive) is appended, and if the schema
//     has none, a column named Message holding only the raw text of the rows
//     described below.
// A row without recognised fields (a continuation line, junk, field
// extraction off) puts its raw text into the Message column; its other field
// columns are empty. So does a row the schema matched only in part, without
// its Message (a prefix variant): the text never disappears. Header names
// are unique: a schema column clashing with `_time` / `_file` or another
// column gets "~2", "~3". CSV values are written as they are — no apostrophe
// against spreadsheet formulas: the export is data, it must round-trip. Field values are decoded (LogEntryFields::value): a
// keyed schema (F-4) gives `\"`, `\n`, `\uXXXX` as the characters.
struct Columns {
    QStringList headers;          // in order, `_time` and `_file` included
    bool time = true;             // headers[0] is `_time`
    bool file = false;            // the next header is `_file`
    QVector<int> fieldIndexes;    // schema index of each field column; -1 — the synthetic Message
    int messageColumn = 0;        // index into fieldIndexes of the Message column
};
Columns columnsFor(const QStringList& schemaFields, bool fieldsShown,
                   const QVector<int>& visibleIndexes, bool multiFile);

// One record of the format (no terminator). values are per header; for JSON a
// null entry of `nulls` (same size, may be empty) writes JSON null.
QString csvRecord(const QStringList& values);
QString jsonRecord(const QStringList& headers, const QStringList& values,
                   const QVector<bool>& nulls = {});
void appendJsonString(QString& out, QStringView text);

// How the file is written: Text — text mode (CRLF on Windows), LF rows;
// Csv — binary, CRLF rows, BOM; JsonLines — binary, LF rows.
struct WriteOptions {
    bool textMode = true;
    bool utf8Bom = false;
    QString terminator = QStringLiteral("\n");
    static WriteOptions forFormat(Format format);
};

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
                 const std::atomic_bool* cancel = nullptr,
                 const WriteOptions& options = {});
Result writeRows(QIODevice& output, int rowCount, const RowText& rowText);
// A cancelled or failed export leaves an existing destination untouched.
Result save(const QString& destination, const QStringList& sourcePaths,
            const RowSource& rows, const std::atomic_bool* cancel = nullptr,
            const WriteOptions& options = {});
Result save(const QString& destination, const QStringList& sourcePaths,
            int rowCount, const RowText& rowText);

// Paths an export must not overwrite for these open files: what is read
// (filePath) and, for a compressed source (F-2), the archive the user opened
// as well — both its temporary copy and the .gz are protected.
QStringList protectedPaths(const QList<std::shared_ptr<LogFile>>& files);

} // namespace ViewExport

// Save View As in the background: the visible rows and the Log Fields
// selection are snapshotted at start (the view may change or close while the
// export runs), the text is formatted exactly as LogModel shows it and
// written by ViewExport::save in the pool. Cancelling keeps the destination
// as it was. The snapshot of a resident model reads entry fields, so a field
// schema switch must cancel (and wait for) a running export first.
// CSV and JSON Lines (F-9) go through the same job: the same snapshot, the
// column layout (ViewExport::columnsFor) taken at start, fields read the same
// way (the indexed backend extracts them on demand from the line text).
class ViewExportJob : public QObject {
    Q_OBJECT

public:
    explicit ViewExportJob(QObject* parent = nullptr);
    // Cancels and waits: QSaveFile must not outlive the application.
    ~ViewExportJob() override;

    // false — an export is already running.
    bool start(const LogModel& model, const QString& destination,
               const QStringList& sourcePaths,
               ViewExport::Format format = ViewExport::Format::Text);
    // finished() still arrives, with Error::Cancelled unless the export had
    // already completed.
    void cancel(bool wait = false);
    bool isRunning() const;
    QString destination() const { return m_destination; }
    ViewExport::Format format() const { return m_format; }
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
    ViewExport::Format m_format = ViewExport::Format::Text;
    int m_rowCount = 0;
    int m_generation = 0;
};

#endif // VIEWEXPORT_H
