#ifndef VIEWEXPORT_H
#define VIEWEXPORT_H

#include <QString>
#include <QStringList>
#include <functional>

class QIODevice;

// Synchronous export of the already formatted visible rows. The caller owns
// the row source; no events are pumped while reading it. File publication is
// atomic, and open log files (including filesystem aliases) are protected.
namespace ViewExport {

enum class Error { None, SourceFile, IdentityCheck, Open, Write, Commit };

struct Result {
    Error error = Error::None;
    QString detail;
    bool ok() const { return error == Error::None; }
};

using RowText = std::function<QString(int)>;

// Does not close the device. Checks buffered writes as well as the final flush.
Result writeRows(QIODevice& output, int rowCount, const RowText& rowText);
Result save(const QString& destination, const QStringList& sourcePaths,
            int rowCount, const RowText& rowText);

} // namespace ViewExport

#endif // VIEWEXPORT_H
