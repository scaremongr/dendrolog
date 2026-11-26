#include "timedelta.h"

#include "logmodel.h"

namespace TimeDelta {

namespace {

constexpr qint64 kSecond = 1000;
constexpr qint64 kMinute = 60 * kSecond;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kDay = 24 * kHour;

QString two(qint64 v)
{
    return QString::number(v).rightJustified(2, QLatin1Char('0'));
}

bool sameRecord(const LogEntryMeta& a, const LogEntryMeta& b)
{
    return a.sourceFile == b.sourceFile && a.logicalEntryId == b.logicalEntryId;
}

} // namespace

QString format(qint64 deltaMs)
{
    // Модуль без переполнения: разность моментов 0001–9999 далека от
    // минимума qint64, но и он не должен ломать строку.
    const bool negative = deltaMs < 0;
    const quint64 a = negative ? quint64(0) - quint64(deltaMs) : quint64(deltaMs);
    const QString sign = negative ? QString(QChar(0x2212)) : QStringLiteral("+");
    QString body;
    if (a < quint64(kSecond)) {
        body = QStringLiteral("%1 ms").arg(a);
    } else if (a < quint64(kMinute)) {
        body = QStringLiteral("%1.%2 s").arg(a / kSecond)
                   .arg(QString::number(a % kSecond).rightJustified(3, QLatin1Char('0')));
    } else if (a < quint64(kHour)) {
        body = QStringLiteral("%1 m %2 s").arg(a / kMinute).arg(two((a % kMinute) / kSecond));
    } else if (a < quint64(kDay)) {
        body = QStringLiteral("%1 h %2 m").arg(a / kHour).arg(two((a % kHour) / kMinute));
    } else if (a < quint64(1000 * kDay)) {
        body = QStringLiteral("%1 d %2 h").arg(a / kDay).arg(two((a % kDay) / kHour));
    } else {
        body = QStringLiteral("%1 d").arg(a / kDay);
    }
    return sign + body;
}

QString formatDetailed(qint64 deltaMs)
{
    QString text = format(deltaMs);
    if (deltaMs >= kMinute || deltaMs <= -kMinute)
        text += QStringLiteral(" (%1 ms)").arg(deltaMs);
    return text;
}

int previousTimedRow(const LogModel& model, int row, int limit, bool* exhausted)
{
    if (exhausted)
        *exhausted = false;
    if (row <= 0 || row >= model.rowCount())
        return -1;
    const LogEntryMeta self = model.visibleMetaAt(row);
    const int stop = limit > 0 && row > limit ? row - limit : 0;
    for (int r = row - 1; r >= stop; --r) {
        const LogEntryMeta m = model.visibleMetaAt(r);
        if (hasTimestampMs(m.timestampMs) && !sameRecord(m, self))
            return r;
    }
    if (exhausted && stop > 0)
        *exhausted = true;
    return -1;
}

bool needsPrevious(const LogModel& model, int row, bool ignoreZero)
{
    if (row < 0 || row >= model.rowCount() || (!ignoreZero && model.hasTimeZero()))
        return false;
    const LogEntryMeta self = model.visibleMetaAt(row);
    return hasTimestampMs(self.timestampMs)
        && (row == 0 || !sameRecord(model.visibleMetaAt(row - 1), self));
}

RowDelta forRow(const LogModel& model, int row, int previousRow)
{
    RowDelta d;
    if (row < 0 || row >= model.rowCount())
        return d;
    d.valid = true;
    const LogEntryMeta self = model.visibleMetaAt(row);
    d.ms = self.timestampMs;
    d.hasTimestamp = hasTimestampMs(self.timestampMs);
    d.firstOfRecord = row == 0 || !sameRecord(model.visibleMetaAt(row - 1), self);
    d.isZero = model.isTimeZeroRow(row);
    if (!d.hasTimestamp)
        return d;
    if (previousRow == -2)
        previousRow = previousTimedRow(model, row);
    if (previousRow >= 0) {
        const qint64 prev = model.visibleMetaAt(previousRow).timestampMs;
        if (hasTimestampMs(prev)) {
            d.hasPrevious = true;
            d.previousRow = previousRow;
            d.fromPrevious = d.ms - prev;
        }
    }
    if (model.hasTimeZero()) {
        d.hasZero = true;
        d.fromZero = d.ms - model.timeZero().ms;
    }
    return d;
}

QString columnText(const RowDelta& delta)
{
    if (delta.isZero)
        return QStringLiteral("= 0");
    if (!delta.hasTimestamp || !delta.firstOfRecord)
        return QString();
    if (delta.hasZero)
        return format(delta.fromZero);
    return delta.hasPrevious ? format(delta.fromPrevious) : QString();
}

} // namespace TimeDelta
