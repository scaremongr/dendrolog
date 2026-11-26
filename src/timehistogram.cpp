#include "timehistogram.h"

#include <QDateTime>
#include <QTimeZone>

namespace TimeBins {

qint64 supportedMinMs()
{
    static const qint64 v =
        QDateTime(QDate(1, 1, 1), QTime(0, 0), QTimeZone::UTC).addDays(-2).toMSecsSinceEpoch();
    return v;
}

qint64 supportedMaxMs()
{
    static const qint64 v =
        QDateTime(QDate(9999, 12, 31), QTime(0, 0), QTimeZone::UTC).addDays(2).toMSecsSinceEpoch();
    return v;
}

} // namespace TimeBins

TimeHistogram TimeHistogram::build(const LogScanSnapshot& snap, int bins,
                                   qint64 filterMinMs, qint64 filterMaxMs, bool filterValid,
                                   const std::atomic_bool* cancel)
{
    TimeHistogram h;
    h.totalPrefix = QVector<quint32>(bins + 1, 0);
    h.warnPrefix  = QVector<quint32>(bins + 1, 0);
    h.errorPrefix = QVector<quint32>(bins + 1, 0);
    h.fatalPrefix = QVector<quint32>(bins + 1, 0);

    // Настоящие минимум и максимум меток (в порядке файла — тоже, S-13);
    // метка до 1970 года — отрицательный момент, а не «метки нет».
    qint64 validEnd = 0;
    qint64 dataMin = 0, dataMax = 0;
    const bool haveEntries = snap.timeRange(&dataMin, &dataMax, &validEnd, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed))
        return h;

    // Диапазон шкалы: границы данных, РАСШИРЕННЫЕ до границ активного
    // фильтра по времени. Так zoom-out за пределы загруженных данных виден
    // как пустые поля по краям (иначе шкала прижимается к данным и отдаление
    // «не применяется» визуально), а зум в пустой промежуток не блокирует
    // дальнейшую работу колеса.
    qint64 viewMin = 0, viewMax = 0;
    if (haveEntries) {
        viewMin = dataMin;
        viewMax = dataMax;
    }
    if (filterValid) {
        if (haveEntries) {
            viewMin = qMin(viewMin, filterMinMs);
            viewMax = qMax(viewMax, filterMaxMs);
        } else {
            viewMin = filterMinMs;
            viewMax = filterMaxMs;
        }
        h.hasData = true; // пустое окно фильтра — шкала и зум живут
    }

    if (!haveEntries && !h.hasData)
        return h;

    h.tMin = TimeBins::clampMs(viewMin);
    h.tMax = TimeBins::clampMs(viewMax);
    h.hasData = true;
    const qint64 span = qMax<qint64>(1, h.tMax - h.tMin);

    // Считаем логические записи, а не строки: у многострочного сообщения
    // все строки несут время/уровень первой строки и идут в списке подряд,
    // иначе одно длинное исключение выглядело бы как всплеск плотности.
    int prevId = 0;
    const LogFile* prevFile = nullptr;
    bool firstRow = true;
    snap.forEachMeta(0, [&](qint64 i, const LogEntryMeta& m) -> bool {
        if (i >= validEnd)
            return false;
        if (cancel && (i & 0x3FFF) == 0
            && cancel->load(std::memory_order_relaxed))
            return false;
        if (!firstRow && m.logicalEntryId == prevId && m.sourceFile == prevFile)
            return true;
        firstRow = false;
        prevId = m.logicalEntryId;
        prevFile = m.sourceFile;
        // Строка без метки посреди порядка файла (преамбула одно-файловой
        // индексной вкладки) — не точка на шкале времени.
        if (!hasTimestampMs(m.timestampMs))
            return true;

        const int bin = TimeBins::binOf(m.timestampMs, h.tMin, span, bins);
        ++h.totalPrefix[bin + 1];
        switch (m.level) {
            case LogLevel::Warn:  ++h.warnPrefix[bin + 1];  break;
            case LogLevel::Error: ++h.errorPrefix[bin + 1]; break;
            case LogLevel::Fatal: ++h.fatalPrefix[bin + 1]; break;
            default: break;
        }
        return true;
    });

    // Счётчики корзин → префикс-суммы (in place).
    for (int b = 1; b <= bins; ++b) {
        h.totalPrefix[b] += h.totalPrefix[b - 1];
        h.warnPrefix[b]  += h.warnPrefix[b - 1];
        h.errorPrefix[b] += h.errorPrefix[b - 1];
        h.fatalPrefix[b] += h.fatalPrefix[b - 1];
    }
    return h;
}
