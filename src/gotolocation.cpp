#include "gotolocation.h"

#include "lineclassifier.h"
#include "logmodel.h"

#include <QRegularExpression>

namespace GoToLocation {

GoToTarget parse(const QString& input, const QDateTime& reference)
{
    GoToTarget target;
    const QString text = input.trimmed();
    if (text.isEmpty())
        return target;

    // Номер строки: только цифры, пробелы между ними — разделители разрядов.
    static const QRegularExpression lineShape(QStringLiteral("^[0-9][0-9  ]*$"));
    if (lineShape.match(text).hasMatch()) {
        QString digits = text;
        digits.remove(QLatin1Char(' ')).remove(QChar(0x00A0));
        bool ok = false;
        const qint64 line = digits.toLongLong(&ok);
        if (ok) {
            target.kind = GoToTarget::Kind::Line;
            target.line = qMax<qint64>(1, line);
        }
        return target;
    }

    // Время: [дата][T или пробел][время[:сек[.дробь]]][зона].
    static const QRegularExpression timeShape(QStringLiteral(
        "^(?:(\\d{4})-(\\d{2})-(\\d{2}))?"
        "(?:[T ]\\s*)?"
        "(?:(\\d{1,2}):(\\d{2})(?::(\\d{2})(?:[.,](\\d+))?)?"
        "\\s*(Z|UTC|GMT|[+-]\\d{2}(?::?\\d{2})?)?)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = timeShape.match(text);
    if (!m.hasMatch())
        return target;
    const bool hasDate = m.hasCaptured(1);
    const bool hasTime = m.hasCaptured(4);
    if (!hasDate && !hasTime)
        return target;

    // Зону приводим к одному виду — Z или ±hh:mm вплотную: классификатор
    // молча отбросил бы непонятый суффикс, и время стало бы локальным.
    QString zoneSuffix;
    int zoneOffsetSeconds = 0;
    const bool hasZone = m.hasCaptured(8);
    if (hasZone) {
        const QString zone = m.captured(8).toUpper();
        if (zone.at(0).isLetter()) {
            zoneSuffix = QStringLiteral("Z");
        } else {
            const QString digits = QString(zone).remove(QLatin1Char(':')).mid(1);
            const int hours = digits.left(2).toInt();
            const int minutes = digits.mid(2).toInt();
            if (minutes > 59 || hours * 60 + minutes > 14 * 60)
                return target;
            zoneSuffix = zone.at(0) + digits.left(2) + QLatin1Char(':')
                       + QStringLiteral("%1").arg(minutes, 2, 10, QLatin1Char('0'));
            zoneOffsetSeconds = (hours * 60 + minutes) * 60
                              * (zone.at(0) == QLatin1Char('-') ? -1 : 1);
        }
    }

    // Дата для времени без даты — день опорного момента В ТОЙ ЖЕ зоне, что и
    // набранное время: локальный день и день по UTC около полуночи расходятся.
    QDate date;
    if (hasDate)
        date = QDate(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    else if (reference.isValid())
        date = hasZone ? reference.toOffsetFromUtc(zoneOffsetSeconds).date()
                       : reference.toLocalTime().date();
    const QTime time = hasTime
        ? QTime(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt())
        : QTime(0, 0);
    if (!date.isValid() || !time.isValid())
        return target;

    // Канонический вид — и в LineClassifier: момент (зона, дробь) считает он,
    // чтобы переход понимал время так же, как разбор лога.
    QString canonical = date.toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char(' ')
                      + time.toString(QStringLiteral("HH:mm:ss"));
    if (m.hasCaptured(7))
        canonical += QLatin1Char('.') + m.captured(7);
    canonical += zoneSuffix;
    QDateTime moment;
    if (!LineClassifier().detectTimestamp(canonical, moment) || !moment.isValid())
        return target;

    target.kind = GoToTarget::Kind::Time;
    target.time = moment;
    return target;
}

QDateTime referenceTime(const LogModel& model, int currentRow)
{
    if (currentRow >= 0 && currentRow < model.rowCount()) {
        const QDateTime current = model.visibleTimestampAt(currentRow);
        if (current.isValid())
            return current;
    }
    return model.fullTimeRange().first;
}

GoToResult resolve(const LogModel& model, const GoToTarget& target, const LogFile* file)
{
    GoToResult result;
    const int rows = model.rowCount();
    if (rows == 0)
        return result;

    int row = -1;
    if (target.kind == GoToTarget::Kind::Line) {
        if (!file) {
            const LogModel::EntryKey first = model.keyForRow(0);
            file = first.sourceFile;
        }
        row = model.nearestVisibleRowForLine(target.line, file);
    } else if (target.kind == GoToTarget::Kind::Time && target.time.isValid()) {
        row = model.firstVisibleRowAtOrAfter(target.time);
        if (row >= rows || !model.visibleTimestampAt(row).isValid()) {
            // Позже последней метки: строки без метки — в конце списка, перед
            // ними — последняя строка с меткой. На одно-файловой индексной
            // вкладке (порядок файла) поиск мог встать и на строку без метки
            // посреди файла — тогда берём ближайшую строку с меткой после неё.
            int found = -1;
            if (row > 0 && model.visibleTimestampAt(row - 1).isValid()
                && model.visibleTimestampAt(row - 1) < target.time) {
                found = row - 1;
            }
            const int scanEnd = qMin(rows, row + 4096);
            for (int next = row; next < scanEnd; ++next) {
                if (model.visibleTimestampAt(next).isValid()) {
                    found = next;
                    break;
                }
            }
            row = found;
        }
    }
    if (row < 0 || row >= rows)
        return result;

    const std::shared_ptr<LogEntry> entry = model.entryAt(row);
    if (!entry)
        return result;
    result.row = row;
    result.file = entry->sourceFile();
    result.line = entry->originalLineNumber();
    result.time = model.visibleTimestampAt(row);
    result.exact = target.kind == GoToTarget::Kind::Line
        ? (entry->sourceFile().get() == file && result.line == target.line)
        : (result.time == target.time);
    return result;
}

} // namespace GoToLocation
