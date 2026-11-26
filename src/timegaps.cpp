#include "timegaps.h"

#include <limits>

TimeGaps::TimeGaps(qint64 tMinMs, qint64 tMaxMs)
    : m_tMin(tMinMs),
      m_span(qMax<qint64>(0, tMaxMs - tMinMs) + 1),
      m_lo(kBins, std::numeric_limits<qint64>::max()),
      m_hi(kBins, std::numeric_limits<qint64>::min())
{
}

void TimeGaps::add(qint64 ms)
{
    // Вне шкалы — в крайнюю корзину, но со своим значением: концы паузы —
    // настоящие моменты (точность внутри крайней корзины тогда не обещана).
    const qint64 offset = ms < m_tMin ? 0 : qMin<qint64>(ms - m_tMin, m_span - 1);
    const int b = int(offset * kBins / m_span);
    if (ms < m_lo[b])
        m_lo[b] = ms;
    if (ms > m_hi[b])
        m_hi[b] = ms;
}

QVector<TimeGaps::Gap> TimeGaps::largest(int count, qint64 minMs) const
{
    QVector<Gap> top;
    if (count <= 0)
        return top;
    bool havePrev = false;
    qint64 prevHi = 0;
    for (int b = 0; b < kBins; ++b) {
        if (m_lo[b] > m_hi[b])
            continue; // пустая
        if (havePrev) {
            const Gap g{m_lo[b] - prevHi, prevHi, m_lo[b]};
            if (g.ms >= minMs && (top.size() < count || g.ms > top.constLast().ms)) {
                // Строгое «больше»: из равных первым остаётся более ранний.
                int at = int(top.size());
                while (at > 0 && g.ms > top[at - 1].ms)
                    --at;
                top.insert(at, g);
                if (top.size() > count)
                    top.removeLast();
            }
        }
        havePrev = true;
        prevHi = m_hi[b];
    }
    return top;
}
