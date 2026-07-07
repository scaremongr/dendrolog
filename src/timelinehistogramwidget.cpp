#include "timelinehistogramwidget.h"
#include "apptheme.h"
#include "logmodel.h"

#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygon>
#include <QTimer>

TimelineHistogramWidget::TimelineHistogramWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);

    m_totalPrefix.resize(kBins + 1);
    m_warnPrefix.resize(kBins + 1);
    m_errorPrefix.resize(kBins + 1);
    m_fatalPrefix.resize(kBins + 1);

    m_rebuildTimer = new QTimer(this);
    m_rebuildTimer->setSingleShot(true);
    m_rebuildTimer->setInterval(200);
    connect(m_rebuildTimer, &QTimer::timeout, this, [this]() {
        rebuildHistogram();
        update();
    });
}

void TimelineHistogramWidget::setModel(LogModel* model)
{
    if (m_model == model)
        return;

    if (m_model)
        disconnect(m_model, nullptr, this, nullptr);

    m_model = model;
    m_currentTime = QDateTime();

    if (m_model) {
        connect(m_model, &LogModel::modelFiltered,
                this, &TimelineHistogramWidget::scheduleRebuild);
        connect(m_model, &QAbstractItemModel::rowsInserted,
                this, &TimelineHistogramWidget::scheduleRebuild);
        connect(m_model, &QAbstractItemModel::rowsRemoved,
                this, &TimelineHistogramWidget::scheduleRebuild);
        connect(m_model, &QAbstractItemModel::modelReset,
                this, &TimelineHistogramWidget::scheduleRebuild);
        // Страховка: вкладка может быть закрыта без setModel(nullptr).
        connect(m_model, &QObject::destroyed, this, [this]() {
            m_model = nullptr;
            m_rebuildTimer->stop();
            rebuildHistogram();
            update();
        });
    }

    // Смена вкладки — перестроить сразу, без дебаунса.
    m_rebuildTimer->stop();
    rebuildHistogram();
    update();
}

void TimelineHistogramWidget::setCurrentTime(const QDateTime& time)
{
    if (m_currentTime == time)
        return;
    m_currentTime = time;
    update(); // маркер — динамический оверлей, кэш не трогаем
}

void TimelineHistogramWidget::scheduleRebuild()
{
    m_rebuildTimer->start();
}

void TimelineHistogramWidget::rebuildHistogram()
{
    m_totalPrefix.fill(0);
    m_warnPrefix.fill(0);
    m_errorPrefix.fill(0);
    m_fatalPrefix.fill(0);
    m_hasData = false;

    const QVector<std::shared_ptr<LogEntry>>* entriesPtr =
        m_model ? &m_model->filteredEntries() : nullptr;

    // Список отсортирован по времени, записи без валидной метки — в конце:
    // min — первая запись, max — последняя валидная с конца.
    int validEnd = entriesPtr ? entriesPtr->size() : 0;
    while (validEnd > 0) {
        const auto& e = (*entriesPtr)[validEnd - 1];
        if (e && e->timestamp.isValid())
            break;
        --validEnd;
    }

    if (validEnd > 0 && entriesPtr->first() && entriesPtr->first()->timestamp.isValid()) {
        const QVector<std::shared_ptr<LogEntry>>& entries = *entriesPtr;
        m_tMin = entries.first()->timestamp.toMSecsSinceEpoch();
        m_tMax = entries[validEnd - 1]->timestamp.toMSecsSinceEpoch();
        const qint64 span = qMax<qint64>(1, m_tMax - m_tMin);
        m_hasData = true;

        // Считаем логические записи, а не строки: у многострочного сообщения
        // все строки несут время/уровень первой строки и идут в списке подряд,
        // иначе одно длинное исключение выглядело бы как всплеск плотности.
        int prevId = 0;
        const LogFile* prevFile = nullptr;
        bool firstRow = true;
        for (int i = 0; i < validEnd; ++i) {
            const LogEntry* e = entries[i].get();
            if (!e)
                continue;
            const LogFile* file = e->sourceFile.get();
            if (!firstRow && e->logicalEntryId == prevId && file == prevFile)
                continue;
            firstRow = false;
            prevId = e->logicalEntryId;
            prevFile = file;

            const qint64 ms = e->timestamp.toMSecsSinceEpoch();
            const int bin = qBound(0, int((ms - m_tMin) * kBins / span), kBins - 1);
            ++m_totalPrefix[bin + 1];
            switch (e->level) {
                case LogLevel::Warn:  ++m_warnPrefix[bin + 1];  break;
                case LogLevel::Error: ++m_errorPrefix[bin + 1]; break;
                case LogLevel::Fatal: ++m_fatalPrefix[bin + 1]; break;
                default: break;
            }
        }

        // Счётчики корзин → префикс-суммы (in place).
        for (int b = 1; b <= kBins; ++b) {
            m_totalPrefix[b] += m_totalPrefix[b - 1];
            m_warnPrefix[b]  += m_warnPrefix[b - 1];
            m_errorPrefix[b] += m_errorPrefix[b - 1];
            m_fatalPrefix[b] += m_fatalPrefix[b - 1];
        }
    }

    m_cacheDirty = true;
}

QColor TimelineHistogramWidget::levelBarColor(LogLevel level) const
{
    const AppTheme& t = AppTheme::instance();
    const bool darkTheme = palette().color(QPalette::Base).lightness() < 128;
    switch (level) {
        case LogLevel::Warn:  return darkTheme ? t.logWarn  : t.treeBadgeWarn;
        case LogLevel::Error: return darkTheme ? t.logError : t.treeBadgeError;
        case LogLevel::Fatal: return darkTheme ? QColor(210, 60, 45) : t.logFatal;
        default:              return t.forLevel(level);
    }
}

TimelineHistogramWidget::Layout TimelineHistogramWidget::layoutForSize(const QSize& size) const
{
    Layout l;
    const QFontMetrics fm = fontMetrics();
    const int mLeft = 8, mRight = 8, mTop = 3, laneGap = 5;
    const int labelH = fm.height();
    const int axisH  = fm.height() + 4;

    l.plotX = mLeft;
    l.plotW = qMax(1, size.width() - mLeft - mRight);

    const int availH = qMax(18, size.height() - mTop - 2 * labelH - laneGap - axisH - 2);
    const int lane1H = qMax(12, availH * 3 / 5);
    const int lane2H = qMax(6, availH - lane1H);

    int y = mTop;
    l.label1 = QRect(l.plotX, y, l.plotW, labelH);
    y += labelH;
    l.plot1 = QRect(l.plotX, y, l.plotW, lane1H);
    y += lane1H + laneGap;
    l.label2 = QRect(l.plotX, y, l.plotW, labelH);
    y += labelH;
    l.plot2 = QRect(l.plotX, y, l.plotW, lane2H);
    y += lane2H;
    l.axis = QRect(l.plotX, y, l.plotW, axisH);
    return l;
}

void TimelineHistogramWidget::columnBins(int x, int plotW, int& b0, int& b1) const
{
    b0 = int(qint64(x) * kBins / plotW);
    b1 = int(qint64(x + 1) * kBins / plotW);
    if (b1 <= b0)
        b1 = b0 + 1;
    b0 = qMin(b0, kBins - 1);
    b1 = qMin(b1, kBins);
}

qint64 TimelineHistogramWidget::timeAtX(int x, const Layout& l) const
{
    const qint64 span = qMax<qint64>(1, m_tMax - m_tMin);
    const int col = qBound(0, x - l.plotX, l.plotW - 1);
    return m_tMin + (2 * qint64(col) + 1) * span / (2 * l.plotW);
}

void TimelineHistogramWidget::renderCache()
{
    const qreal dpr = devicePixelRatio();
    m_cache = QPixmap(size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    m_cache.fill(Qt::transparent);

    QPainter p(&m_cache);
    p.fillRect(rect(), palette().color(QPalette::Base));

    const QColor mutedInk = palette().color(QPalette::PlaceholderText);

    if (!m_hasData) {
        p.setPen(mutedInk);
        p.drawText(rect(), Qt::AlignCenter, tr("No timestamped entries"));
        m_cacheDirty = false;
        return;
    }

    const Layout l = layoutForSize(size());
    const AppTheme& t = AppTheme::instance();
    const QLocale loc;

    // Базовые линии дорожек — рекессивные, под столбиками.
    QColor baseline = mutedInk;
    baseline.setAlpha(90);
    p.setPen(baseline);
    p.drawLine(l.plot1.left(), l.plot1.bottom(), l.plot1.right(), l.plot1.bottom());
    p.drawLine(l.plot2.left(), l.plot2.bottom(), l.plot2.right(), l.plot2.bottom());

    // Максимумы по пиксельным колонкам; масштаб каждой дорожки свой.
    quint32 max1 = 0, max2 = 0;
    for (int x = 0; x < l.plotW; ++x) {
        int b0, b1;
        columnBins(x, l.plotW, b0, b1);
        max1 = qMax(max1, rangeCount(m_totalPrefix, b0, b1));
        max2 = qMax(max2, rangeCount(m_warnPrefix, b0, b1)
                          + rangeCount(m_errorPrefix, b0, b1)
                          + rangeCount(m_fatalPrefix, b0, b1));
    }

    QColor totalColor = t.logDebug;
    totalColor.setAlpha(180);
    const QColor warnColor  = levelBarColor(LogLevel::Warn);
    const QColor errorColor = levelBarColor(LogLevel::Error);
    const QColor fatalColor = levelBarColor(LogLevel::Fatal);

    for (int x = 0; x < l.plotW; ++x) {
        int b0, b1;
        columnBins(x, l.plotW, b0, b1);
        const int px = l.plotX + x;

        if (max1 > 0) {
            const quint32 c = rangeCount(m_totalPrefix, b0, b1);
            if (c > 0) {
                // Ненулевые колонки — минимум 1px: одиночная запись должна быть видна.
                const int h = qMax(1, qRound(qreal(c) / max1 * (l.plot1.height() - 1)));
                p.fillRect(px, l.plot1.bottom() - h, 1, h, totalColor);
            }
        }
        if (max2 > 0) {
            int y = l.plot2.bottom();
            const int maxH = l.plot2.height() - 1;
            const auto stack = [&](quint32 c, const QColor& color) {
                if (c == 0)
                    return;
                int h = qMax(1, qRound(qreal(c) / max2 * maxH));
                h = qMin(h, y - l.plot2.top()); // стопка не выше дорожки
                if (h <= 0)
                    return;
                p.fillRect(px, y - h, 1, h, color);
                y -= h;
            };
            stack(rangeCount(m_warnPrefix, b0, b1), warnColor);
            stack(rangeCount(m_errorPrefix, b0, b1), errorColor);
            stack(rangeCount(m_fatalPrefix, b0, b1), fatalColor);
        }
    }

    // Подписи дорожек. Идентичность серий — не только цветом: чип + имя.
    const QFontMetrics fm = fontMetrics();
    p.setPen(mutedInk);
    p.drawText(l.label1, Qt::AlignLeft | Qt::AlignVCenter,
               tr("All entries — %1").arg(loc.toString(qlonglong(m_totalPrefix.last()))));

    struct LegendItem {
        QColor  color;
        QString text;
    };
    const LegendItem legend[] = {
        { warnColor,  tr("Warn %1").arg(loc.toString(qlonglong(m_warnPrefix.last()))) },
        { errorColor, tr("Error %1").arg(loc.toString(qlonglong(m_errorPrefix.last()))) },
        { fatalColor, tr("Fatal %1").arg(loc.toString(qlonglong(m_fatalPrefix.last()))) },
    };
    const int chip = qBound(7, fm.ascent() - 2, 10);
    int lx = l.label2.left();
    for (const LegendItem& item : legend) {
        p.fillRect(QRect(lx, l.label2.center().y() - chip / 2, chip, chip), item.color);
        lx += chip + 4;
        p.drawText(QRect(lx, l.label2.top(), qMax(0, l.label2.right() - lx), l.label2.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, item.text);
        lx += fm.horizontalAdvance(item.text) + 12;
    }

    paintAxis(p, l);
    m_cacheDirty = false;
}

void TimelineHistogramWidget::paintAxis(QPainter& p, const Layout& l) const
{
    const QColor mutedInk = palette().color(QPalette::PlaceholderText);
    const qint64 span = qMax<qint64>(1, m_tMax - m_tMin);

    QFont f = font();
    f.setPointSizeF(qMax(6.0, f.pointSizeF() - 1.0));
    p.setFont(f);
    const QFontMetrics fm(f);
    p.setPen(mutedInk);

    // Шаг тиков: первый «круглый» интервал, дающий ≥ ~80px между подписями.
    static const qint64 kTickSteps[] = {
        100, 250, 500,                                            // < 1 c
        1000, 2000, 5000, 10000, 15000, 30000,                    // секунды
        60000, 120000, 300000, 600000, 900000, 1800000,           // минуты
        3600000, 7200000, 10800000, 21600000, 43200000,           // часы
        86400000LL, 172800000LL, 604800000LL, 1209600000LL,       // дни/недели
        2592000000LL, 7776000000LL, 31536000000LL,                // месяцы/год
    };
    const qint64 wanted = span * 80 / l.plotW;
    qint64 step = kTickSteps[sizeof(kTickSteps) / sizeof(kTickSteps[0]) - 1];
    for (qint64 s : kTickSteps) {
        if (s >= wanted) {
            step = s;
            break;
        }
    }

    const char* fmt = step < 1000       ? "HH:mm:ss.zzz"
                    : step < 60000      ? "HH:mm:ss"
                    : step < 86400000LL ? "HH:mm"
                                        : "dd.MM";

    // Выравнивание тиков по «круглому» локальному времени (offsetFromUtc),
    // иначе часовые/суточные тики попадают на некруглые локальные значения.
    const qint64 utcOffsetMs =
        qint64(QDateTime::fromMSecsSinceEpoch(m_tMin).offsetFromUtc()) * 1000;
    qint64 tick = ((m_tMin + utcOffsetMs + step - 1) / step) * step - utcOffsetMs;

    for (; tick <= m_tMax; tick += step) {
        const int x = l.plotX + int((tick - m_tMin) * l.plotW / span);
        p.drawLine(x, l.axis.top(), x, l.axis.top() + 3);
        const QString text = QDateTime::fromMSecsSinceEpoch(tick).toString(QLatin1String(fmt));
        const int tw = fm.horizontalAdvance(text);
        const int tx = x - tw / 2;
        if (tx < 1 || tx + tw > width() - 1)
            continue; // подпись не влезает у края — тик остаётся без текста
        p.drawText(tx, l.axis.top() + 4 + fm.ascent(), text);
    }
}

void TimelineHistogramWidget::paintHoverOverlay(QPainter& p, const Layout& l) const
{
    const int hx = m_hoverPos.x();
    if (hx < l.plotX || hx >= l.plotX + l.plotW || m_hoverPos.y() < 0)
        return;

    // Перекрестие через обе дорожки до оси.
    QColor cross = palette().color(QPalette::Text);
    cross.setAlpha(80);
    p.setPen(cross);
    p.drawLine(hx, l.plot1.top(), hx, l.axis.top() + 3);

    const int col = hx - l.plotX;
    int b0, b1;
    columnBins(col, l.plotW, b0, b1);
    const quint32 total = rangeCount(m_totalPrefix, b0, b1);
    const quint32 warn  = rangeCount(m_warnPrefix, b0, b1);
    const quint32 error = rangeCount(m_errorPrefix, b0, b1);
    const quint32 fatal = rangeCount(m_fatalPrefix, b0, b1);

    // Временной интервал колонки.
    const qint64 span = qMax<qint64>(1, m_tMax - m_tMin);
    const qint64 t0 = m_tMin + qint64(col) * span / l.plotW;
    const qint64 t1 = m_tMin + qint64(col + 1) * span / l.plotW;
    const QDateTime from = QDateTime::fromMSecsSinceEpoch(t0);
    const QDateTime to   = QDateTime::fromMSecsSinceEpoch(t1);
    const qint64 colSpan = qMax<qint64>(1, t1 - t0);
    const QString timeFmt = colSpan < 1000  ? QStringLiteral("HH:mm:ss.zzz")
                          : colSpan < 60000 ? QStringLiteral("HH:mm:ss")
                                            : QStringLiteral("HH:mm");
    const QString timeText =
        from.toString(QStringLiteral("dd.MM.yyyy ") + timeFmt) + QStringLiteral(" – ")
        + to.toString(from.date() == to.date() ? timeFmt
                                               : QStringLiteral("dd.MM.yyyy ") + timeFmt);

    const QLocale loc;
    struct InfoLine {
        QColor  chip; // invalid — строка без чипа
        QString text;
    };
    QVector<InfoLine> lines;
    lines.append({ QColor(), timeText });
    lines.append({ QColor(), tr("Records: %1").arg(loc.toString(qlonglong(total))) });
    if (warn > 0)
        lines.append({ levelBarColor(LogLevel::Warn),
                       tr("Warn: %1").arg(loc.toString(qlonglong(warn))) });
    if (error > 0)
        lines.append({ levelBarColor(LogLevel::Error),
                       tr("Error: %1").arg(loc.toString(qlonglong(error))) });
    if (fatal > 0)
        lines.append({ levelBarColor(LogLevel::Fatal),
                       tr("Fatal: %1").arg(loc.toString(qlonglong(fatal))) });

    const QFontMetrics fm = fontMetrics();
    const int chip = 8, pad = 6;
    const int lineH = fm.height();
    int boxW = 0;
    for (const InfoLine& ln : lines)
        boxW = qMax(boxW, (ln.chip.isValid() ? chip + 4 : 0) + fm.horizontalAdvance(ln.text));
    boxW += pad * 2;
    const int boxH = pad * 2 + lineH * lines.size();

    int bx = hx + 10;
    if (bx + boxW > width() - 2)
        bx = hx - 10 - boxW;
    bx = qMax(2, bx);
    const int by = qBound(2, l.plot1.top(), qMax(2, height() - boxH - 2));

    QColor bg = palette().color(QPalette::ToolTipBase);
    bg.setAlpha(245);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(palette().color(QPalette::Mid));
    p.setBrush(bg);
    p.drawRoundedRect(QRect(bx, by, boxW, boxH), 3, 3);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setBrush(Qt::NoBrush);

    int ty = by + pad;
    for (const InfoLine& ln : lines) {
        int tx = bx + pad;
        if (ln.chip.isValid()) {
            p.fillRect(QRect(tx, ty + (lineH - chip) / 2, chip, chip), ln.chip);
            tx += chip + 4;
        }
        p.setPen(palette().color(QPalette::ToolTipText));
        p.drawText(tx, ty + fm.ascent(), ln.text);
        ty += lineH;
    }
}

void TimelineHistogramWidget::paintEvent(QPaintEvent* /*event*/)
{
    if (m_cacheDirty || m_cache.size() != size() * devicePixelRatio())
        renderCache();

    QPainter p(this);
    p.drawPixmap(0, 0, m_cache);

    if (!m_hasData)
        return;

    const Layout l = layoutForSize(size());

    // Маркер позиции текущей строки списка.
    if (m_currentTime.isValid()) {
        const qint64 ms = m_currentTime.toMSecsSinceEpoch();
        if (ms >= m_tMin && ms <= m_tMax) {
            const qint64 span = qMax<qint64>(1, m_tMax - m_tMin);
            const int x = l.plotX
                + qMin(l.plotW - 1, int((ms - m_tMin) * l.plotW / span));
            QColor c = AppTheme::instance().badgeBg;
            c.setAlpha(170);
            p.setPen(c);
            p.drawLine(x, l.plot1.top(), x, l.plot2.bottom());
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawPolygon(QPolygon({ QPoint(x - 3, l.plot1.top() - 4),
                                     QPoint(x + 3, l.plot1.top() - 4),
                                     QPoint(x, l.plot1.top() + 1) }));
            p.setRenderHint(QPainter::Antialiasing, false);
            p.setBrush(Qt::NoBrush);
        }
    }

    paintHoverOverlay(p, l);
}

void TimelineHistogramWidget::resizeEvent(QResizeEvent* event)
{
    m_cacheDirty = true;
    QWidget::resizeEvent(event);
}

void TimelineHistogramWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        m_cacheDirty = true;
    QWidget::changeEvent(event);
}

void TimelineHistogramWidget::mouseMoveEvent(QMouseEvent* event)
{
    m_hoverPos = event->pos();
    update();
    QWidget::mouseMoveEvent(event);
}

void TimelineHistogramWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_hasData) {
        const Layout l = layoutForSize(size());
        const int x = event->pos().x();
        if (x >= l.plotX && x < l.plotX + l.plotW) {
            const bool preferErrors = event->pos().y() >= l.label2.top()
                                   && event->pos().y() <= l.plot2.bottom();
            emit timeClicked(QDateTime::fromMSecsSinceEpoch(timeAtX(x, l)), preferErrors);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void TimelineHistogramWidget::leaveEvent(QEvent* event)
{
    m_hoverPos = QPoint(-1, -1);
    update();
    QWidget::leaveEvent(event);
}

QSize TimelineHistogramWidget::sizeHint() const
{
    return QSize(800, 132);
}

QSize TimelineHistogramWidget::minimumSizeHint() const
{
    return QSize(240, 104);
}
