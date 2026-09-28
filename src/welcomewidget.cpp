#include "welcomewidget.h"

#include "cardframe.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

// Путь каталога для строки недавнего файла: длинный — с многоточием в
// середине, чтобы были видны и диск, и ближайшие к файлу папки.
QString elidedDir(const QString& dir)
{
    constexpr int kMax = 64;
    if (dir.size() <= kMax)
        return dir;
    const int head = 24;
    return dir.left(head) + QStringLiteral("…") + dir.right(kMax - head - 1);
}

} // namespace

WelcomeWidget::WelcomeWidget(QWidget* parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    // Колонка фиксированной ширины по центру: на широком окне строки не
    // расползаются, на узком — сжимаются вместе с ним.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->addStretch(2);

    auto* row = new QHBoxLayout();
    row->addStretch(1);
    auto* column = new QWidget(this);
    column->setMaximumWidth(560);
    column->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    row->addWidget(column, 4);
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(3);

    auto* col = new QVBoxLayout(column);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(4);

    auto* icon = new QLabel(column);
    icon->setPixmap(QIcon(QStringLiteral(":/icons/dendrolog.svg")).pixmap(64, 64));
    col->addWidget(icon, 0, Qt::AlignHCenter);

    auto* title = new QLabel(QStringLiteral("DendroLog"), column);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.9);
    titleFont.setBold(true);
    title->setFont(titleFont);
    col->addWidget(title, 0, Qt::AlignHCenter);

    m_subtitle = new QLabel(tr("Open a log file to get started."), column);
    col->addWidget(m_subtitle, 0, Qt::AlignHCenter);
    col->addSpacing(10);

    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);
    buttons->addStretch(1);
    m_openButton = new QPushButton(tr("Open Log Files..."), column);
    m_openButton->setDefault(true);
    m_openButton->setMinimumHeight(26);
    connect(m_openButton, &QPushButton::clicked, this, &WelcomeWidget::openFilesRequested);
    buttons->addWidget(m_openButton);
    m_scanButton = new QPushButton(tr("Scan a Folder..."), column);
    m_scanButton->setMinimumHeight(26);
    m_scanButton->setToolTip(tr("List the log files of a folder in the Directory Scanner panel"));
    connect(m_scanButton, &QPushButton::clicked, this, &WelcomeWidget::scanDirectoryRequested);
    buttons->addWidget(m_scanButton);
    buttons->addStretch(1);
    col->addLayout(buttons);

    m_dropHint = new QLabel(tr("…or drop log files anywhere in this window."), column);
    col->addWidget(m_dropHint, 0, Qt::AlignHCenter);
    col->addSpacing(12);

    // ---- Недавние файлы ------------------------------------------------ //
    m_recentSection = new QWidget(column);
    auto* recent = new QVBoxLayout(m_recentSection);
    recent->setContentsMargins(0, 0, 0, 0);
    recent->setSpacing(3);
    auto* header = new QHBoxLayout();
    m_recentHeader = new QLabel(tr("Recent files"), m_recentSection);
    QFont headerFont = m_recentHeader->font();
    headerFont.setBold(true);
    m_recentHeader->setFont(headerFont);
    header->addWidget(m_recentHeader);
    header->addStretch(1);
    m_clearLink = new QLabel(m_recentSection);
    m_clearLink->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                         | Qt::LinksAccessibleByKeyboard);
    connect(m_clearLink, &QLabel::linkActivated, this, &WelcomeWidget::clearRecentRequested);
    header->addWidget(m_clearLink);
    recent->addLayout(header);
    m_recentList = new QVBoxLayout();
    m_recentList->setSpacing(2);
    recent->addLayout(m_recentList);
    col->addWidget(m_recentSection);
    col->addSpacing(12);

    m_hints = new QLabel(column);
    m_hints->setWordWrap(true);
    m_hints->setAlignment(Qt::AlignHCenter);
    col->addWidget(m_hints);

    m_recentSection->setVisible(false);
    applyColors();
    updateHints();
}

void WelcomeWidget::setRecentFiles(const QStringList& paths)
{
    if (paths == m_recentFiles)
        return;
    m_recentFiles = paths;
    rebuildRecent();
}

void WelcomeWidget::setShortcutHints(const QKeySequence& open, const QKeySequence& find)
{
    m_openSeq = open;
    m_findSeq = find;
    updateHints();
}

void WelcomeWidget::rebuildRecent()
{
    while (QLayoutItem* item = m_recentList->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QString muted = CardFrame::mutedTextColor(palette()).name();
    const QString link = palette().color(QPalette::Link).name();
    for (const QString& path : std::as_const(m_recentFiles)) {
        const QFileInfo info(path);
        auto* label = new QLabel(m_recentSection);
        label->setTextFormat(Qt::RichText);
        label->setTextInteractionFlags(Qt::LinksAccessibleByMouse
                                       | Qt::LinksAccessibleByKeyboard);
        label->setText(QStringLiteral(
            "<a href=\"open\" style=\"color:%1; text-decoration:none;\">%2</a>"
            "&nbsp;&nbsp;<span style=\"color:%3;\">%4</span>")
                .arg(link, info.fileName().toHtmlEscaped(), muted,
                     elidedDir(QDir::toNativeSeparators(info.absolutePath())).toHtmlEscaped()));
        label->setToolTip(QDir::toNativeSeparators(path));
        connect(label, &QLabel::linkActivated, this,
                [this, path]() { emit recentFileRequested(path); });
        m_recentList->addWidget(label);
    }
    m_recentSection->setVisible(!m_recentFiles.isEmpty());
}

void WelcomeWidget::updateHints()
{
    QStringList parts;
    if (!m_openSeq.isEmpty())
        parts << tr("%1 — open").arg(m_openSeq.toString(QKeySequence::NativeText));
    if (!m_findSeq.isEmpty())
        parts << tr("%1 — find").arg(m_findSeq.toString(QKeySequence::NativeText));
    parts << tr("F1 — help");
    m_hints->setText(parts.join(QStringLiteral("   ·   "))
                     + QStringLiteral("<br>")
                     + tr("Read a stream: <code>program | DendroLog -</code>"));
}

void WelcomeWidget::applyColors()
{
    const QString muted = CardFrame::mutedTextColor(palette()).name();
    const QString style = QStringLiteral("color: %1;").arg(muted);
    m_subtitle->setStyleSheet(style);
    m_dropHint->setStyleSheet(style);
    m_hints->setStyleSheet(style);
    m_openButton->setIcon(CardFrame::tintedIcon(QStringLiteral(":/icons/open.svg"),
                                                palette().color(QPalette::ButtonText)));
    m_clearLink->setText(QStringLiteral(
        "<a href=\"clear\" style=\"color:%1;\">%2</a>").arg(muted, tr("Clear")));
    rebuildRecent();
}

void WelcomeWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    // setBackgroundRole в конструкторе шлёт PaletteChange раньше, чем
    // созданы подписи, — их перекрашивает сам конструктор.
    if (event->type() == QEvent::PaletteChange && m_hints)
        applyColors();
}
