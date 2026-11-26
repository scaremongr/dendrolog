#ifndef TIMEHISTOGRAM_H
#define TIMEHISTOGRAM_H

#include "logscan.h"

#include <QVector>
#include <atomic>

// ============================================================================
// Гистограмма записей по времени — расчёт таймлайна без виджетов (S-13).
//
// Один проход по метаданным снапшота видимых строк: логические записи (не
// строки) в bins корзин по метке, отдельно Warn / Error / Fatal, итог —
// префикс-суммы (окно рисует колонку за O(1)). Шкала — настоящие минимум и
// максимум меток (LogScanSnapshot::timeRange: в любом порядке вкладки,
// одно-файловая индексная — порядок файла), расширенные до активного фильтра
// по времени.
//
// Арифметика: шкала прижата к supportedMinMs()…supportedMaxMs() (годы 0001 —
// 9999 с запасом в двое суток на зону): метки классификатор вне них не даёт,
// а время фильтра пользователь может задать любое (QDateTime — сотни
// миллионов лет), и разность границ, умноженная на число корзин или ширину в
// пикселях, переполнила бы qint64. Внутри — span ≤ 3,2·10^14 мс, ×4096
// корзин ≈ 1,3·10^18 — без переполнения.
// ============================================================================

namespace TimeBins {
qint64 supportedMinMs(); // 0001-01-01T00:00Z − 2 суток
qint64 supportedMaxMs(); // 9999-12-31T00:00Z + 2 суток
inline qint64 clampMs(qint64 ms)
{
    return qBound(supportedMinMs(), ms, supportedMaxMs());
}
// Корзина момента ms на шкале [tMin, tMin + span) из bins корзин; момент вне
// шкалы прижимается к крайней. span > 0, шкала — в пределах clampMs.
inline int binOf(qint64 ms, qint64 tMin, qint64 span, int bins)
{
    const qint64 clamped = clampMs(ms);
    return int(qBound<qint64>(0, (clamped - tMin) * qint64(bins) / span, qint64(bins) - 1));
}
} // namespace TimeBins

struct TimeHistogram {
    // Размер bins + 1: [b + 1] − [b] — записи корзины b.
    QVector<quint32> totalPrefix, warnPrefix, errorPrefix, fatalPrefix;
    qint64 tMin = 0, tMax = 0; // границы шкалы (в пределах TimeBins::clampMs)
    bool   hasData = false;    // есть метки или действует фильтр по времени

    // Чистый расчёт по снапшоту — безопасен в воркере. filter*Ms — активный
    // фильтр по времени (расширяет шкалу); cancel — кооперативная отмена
    // (обрезанный результат отбрасывает вызывающий).
    static TimeHistogram build(const LogScanSnapshot& snap, int bins,
                               qint64 filterMinMs, qint64 filterMaxMs, bool filterValid,
                               const std::atomic_bool* cancel = nullptr);
};

#endif // TIMEHISTOGRAM_H
