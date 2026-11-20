#include "filelegend.h"

#include "LogListView.h"
#include "appsettings.h"
#include "apptheme.h"
#include "logmodel.h"
#include "shortcount.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QHelpEvent>
#include <QMenu>
#include <QPainter>
#include <QToolTip>
#include <QUrl>

namespace {
constexpr int kMarginX = 6;
constexpr int kMarginY = 3;
constexpr int kChipGap = 6;
constexpr int kChipPadX = 6;
constexpr int kSwatch = 10;
constexpr int kSwatchGap = 5;
constexpr int kCountGap = 6;
}

FileLegend::FileLegend(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("fileLegend"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFocusPolicy(Qt::NoFocus);
    connect(&AppSettings::instance(), &AppSettings::settingsChanged, this, [this]() {
        emit visibilityWanted(shouldShow());
    });
}

void FileLegend::setModel(LogModel* model)
{
    disconnect(m_setConnection);
    disconnect(m_linesConnection);
    m_model = model;
    if (model) {
        m_setConnection = connect(model, &LogModel::fileSetChanged, this, &FileLegend::rebuild);
        m_linesConnection = connect(model, &LogModel::fileLinesChanged, this, &FileLegend::updateCounts);
    }
    rebuild();
}

bool FileLegend::shouldShow() const
{
    return m_model && m_chips.size() >= 2
        && AppSettings::instance().fileLabelMode() != FileLabelMode::Hidden;
}

void FileLegend::rebuild()
{
    m_chips.clear();
    if (m_model) {
        for (const LogModel::LabeledFile& f : m_model->labeledFiles()) {
            Chip chip;
            chip.file = f.file.get();
            chip.name = f.file->shortName();
            chip.path = f.file->displayPath();
            chip.colorOrdinal = f.colorOrdinal;
            chip.lines = f.lines;
            chip.count = ShortCount::format(f.lines, 100000, true);
            m_chips.append(chip);
        }
    }
    layoutChips(width(), &m_chips);
    updateGeometry();
    update();
    emit visibilityWanted(shouldShow());
}

void FileLegend::updateCounts()
{
    if (!m_model)
        return;
    const QVector<LogModel::LabeledFile>& files = m_model->labeledFiles();
    if (files.size() != m_chips.size()) {
        rebuild();
        return;
    }
    for (int i = 0; i < files.size(); ++i) {
        m_chips[i].lines = files[i].lines;
        m_chips[i].count = ShortCount::format(files[i].lines, 100000, true);
    }
    // Ширина числа в пикселях могла смениться и при той же длине —
    // раскладка O(числа чипов).
    if (layoutChips(width(), &m_chips) != height())
        updateGeometry();
    update();
}

int FileLegend::chipWidth(const Chip& chip) const
{
    const QFontMetrics fm(font());
    return kChipPadX + kSwatch + kSwatchGap + fm.horizontalAdvance(chip.name) + kCountGap
         + fm.horizontalAdvance(chip.count) + kChipPadX;
}

int FileLegend::layoutChips(int width, QVector<Chip>* chips) const
{
    const QFontMetrics fm(font());
    const int chipHeight = fm.height() + 4;
    int x = kMarginX;
    int y = kMarginY;
    const int right = qMax(width - kMarginX, kMarginX + 1);
    for (Chip& chip : *chips) {
        const int w = chipWidth(chip);
        if (x > kMarginX && x + w > right) {
            x = kMarginX;
            y += chipHeight + kMarginY;
        }
        chip.rect = QRect(x, y, w, chipHeight);
        x += w + kChipGap;
    }
    return chips->isEmpty() ? 0 : y + chipHeight + kMarginY;
}

int FileLegend::heightForWidth(int width) const
{
    QVector<Chip> chips = m_chips;
    return layoutChips(width, &chips);
}

QSize FileLegend::sizeHint() const
{
    const int w = width() > 0 ? width() : 400;
    return QSize(w, heightForWidth(w));
}

QSize FileLegend::minimumSizeHint() const
{
    return QSize(0, heightForWidth(width() > 0 ? width() : 400));
}

void FileLegend::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const int h = layoutChips(width(), &m_chips);
    if (h != height())
        updateGeometry();
}

QRect FileLegend::chipRect(int i) const
{
    return i >= 0 && i < m_chips.size() ? m_chips[i].rect : QRect();
}

QString FileLegend::chipText(int i) const
{
    return i >= 0 && i < m_chips.size() ? m_chips[i].name + QLatin1Char(' ') + m_chips[i].count : QString();
}

const LogFile* FileLegend::chipFile(int i) const
{
    return i >= 0 && i < m_chips.size() ? m_chips[i].file : nullptr;
}

int FileLegend::chipAt(const QPoint& pos) const
{
    for (int i = 0; i < m_chips.size(); ++i)
        if (m_chips[i].rect.contains(pos))
            return i;
    return -1;
}

void FileLegend::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette& pal = palette();
    const QFontMetrics fm(font());
    const QColor border = AppTheme::wrapRail(pal);
    for (const Chip& chip : std::as_const(m_chips)) {
        const QRectF r = QRectF(chip.rect).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(border);
        p.setBrush(pal.color(QPalette::Base));
        p.drawRoundedRect(r, 3, 3);
        const QColor color = AppTheme::fileColor(chip.colorOrdinal, pal);
        const QRect swatch(chip.rect.left() + kChipPadX, chip.rect.center().y() - kSwatch / 2 + 1,
                           kSwatch, kSwatch);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(swatch, 2, 2);
        int x = swatch.right() + 1 + kSwatchGap;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRect(x, chip.rect.top(), fm.horizontalAdvance(chip.name) + 1, chip.rect.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, chip.name);
        x += fm.horizontalAdvance(chip.name) + kCountGap;
        p.setPen(AppTheme::wrapToggleFg(pal));
        p.drawText(QRect(x, chip.rect.top(), fm.horizontalAdvance(chip.count) + 1, chip.rect.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, chip.count);
    }
}

bool FileLegend::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const int i = chipAt(help->pos());
        if (i >= 0) {
            QLocale c = QLocale::c();
            c.setNumberOptions(QLocale::DefaultNumberOptions);
            const Chip& chip = m_chips[i];
            QToolTip::showText(help->globalPos(),
                               tr("%1\n%2 line(s) in this tab").arg(chip.path, c.toString(chip.lines)),
                               this, chip.rect);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}

void FileLegend::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        layoutChips(width(), &m_chips);
        updateGeometry();
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        update();
}

bool FileLegend::goToFileLine(int chip, bool forward)
{
    if (!m_model || !m_view || chip < 0 || chip >= m_chips.size())
        return false;
    int from = m_view->userSelectedRow();
    if (from < 0)
        from = m_view->viewAnchor().row;
    const int row = m_model->findRowOfFile(m_chips[chip].file, from, forward);
    if (row < 0)
        return false;
    m_view->goToRow(row, NavKind::Other);
    m_view->setFocus(Qt::OtherFocusReason);
    return true;
}

void FileLegend::contextMenuEvent(QContextMenuEvent* event)
{
    const int i = chipAt(event->pos());
    if (i < 0)
        return;
    const QString path = m_chips[i].path;
    // Пока меню открыто, чипы могли перестроиться (догрузка файла) — чип
    // ищется заново по файлу.
    const LogFile* file = m_chips[i].file;
    const auto go = [this, file](bool forward) {
        for (int c = 0; c < m_chips.size(); ++c)
            if (m_chips[c].file == file)
                goToFileLine(c, forward);
    };
    QMenu menu(this);
    menu.addAction(tr("Go to Next Line of This File"), this, [go]() { go(true); });
    menu.addAction(tr("Go to Previous Line of This File"), this, [go]() { go(false); });
    menu.addSeparator();
    menu.addAction(tr("Copy Path"), this, [path]() { QApplication::clipboard()->setText(path); });
    QAction* folder = menu.addAction(tr("Show in Folder"), this, [path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    folder->setEnabled(QFileInfo::exists(path));
    menu.exec(event->globalPos());
}
