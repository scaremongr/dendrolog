#include "sessionreportdialog.h"

#include "sessioncontroller.h"

#include "logviewwidget.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

SessionReportDialog::SessionReportDialog(SessionController* controller, QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
{
    setWindowTitle(tr("Session Report"));
    setObjectName(QStringLiteral("sessionReportDialog"));
    setModal(false);
    resize(760, 300);

    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("What was opened differently from the saved session:"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("sessionReportList"));
    m_list->setColumnCount(3);
    m_list->setHeaderLabels({ tr("Tab"), tr("File"), tr("What happened") });
    m_list->setRootIsDecorated(false);
    m_list->setWordWrap(true);
    m_list->header()->setStretchLastSection(true);
    layout->addWidget(m_list, 1);

    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("sessionReportMessage"));
    m_message->setWordWrap(true);
    m_message->hide();
    layout->addWidget(m_message);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_locate = buttons->addButton(tr("Locate File…"), QDialogButtonBox::ActionRole);
    m_locate->setObjectName(QStringLiteral("sessionReportLocate"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    connect(m_locate, &QPushButton::clicked, this, &SessionReportDialog::locateSelected);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &SessionReportDialog::updateButtons);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this]() {
        if (m_locate->isEnabled())
            locateSelected();
    });
    if (m_controller)
        connect(m_controller, &SessionController::reportChanged, this, &SessionReportDialog::refresh);

    m_chooser = [](QWidget* parentWidget, const QString& savedPath) {
        return QFileDialog::getOpenFileName(parentWidget, tr("Locate %1").arg(QFileInfo(savedPath).fileName()),
                                            QFileInfo(savedPath).absolutePath());
    };
    refresh();
}

void SessionReportDialog::refresh()
{
    const int selected = m_list->currentIndex().row();
    m_list->clear();
    if (!m_controller) {
        updateButtons();
        return;
    }
    for (const SessionController::ReportEntry& e : m_controller->report()) {
        auto* item = new QTreeWidgetItem(m_list);
        item->setText(0, e.tabTitle);
        item->setText(1, QDir::toNativeSeparators(e.savedPath));
        item->setToolTip(1, QDir::toNativeSeparators(e.savedPath));
        item->setText(2, SessionController::describe(e));
        item->setToolTip(2, SessionController::describe(e));
        item->setData(0, Qt::UserRole, int(e.kind));
    }
    m_list->resizeColumnToContents(0);
    if (selected >= 0 && selected < m_list->topLevelItemCount())
        m_list->setCurrentItem(m_list->topLevelItem(selected));
    else if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));
    updateButtons();
}

void SessionReportDialog::updateButtons()
{
    const QTreeWidgetItem* item = m_list->currentItem();
    m_locate->setEnabled(item
        && item->data(0, Qt::UserRole).toInt() == int(SessionController::ReportEntry::Kind::Missing));
}

void SessionReportDialog::locateSelected()
{
    if (!m_controller)
        return;
    const int row = m_list->currentIndex().row();
    const QVector<SessionController::ReportEntry> entries = m_controller->report();
    if (row < 0 || row >= entries.size() || !m_chooser)
        return;
    const QPointer<LogViewWidget> tab = entries[row].tab;
    const QString savedPath = entries[row].savedPath;
    const QString path = m_chooser(this, savedPath);
    if (path.isEmpty() || !m_controller)
        return;
    // Пока шёл выбор файла (вложенный цикл событий), отчёт мог измениться:
    // вкладку закрыли, открыли другую сессию — запись ищется заново.
    const QVector<SessionController::ReportEntry> now = m_controller->report();
    int index = -1;
    for (int i = 0; i < now.size(); ++i)
        if (now[i].tab == tab && now[i].savedPath == savedPath)
            index = i;
    if (index < 0)
        return;
    QString error;
    if (m_controller->locate(index, path, &error)) {
        m_message->hide();
        return;
    }
    m_message->setText(error);
    m_message->show();
}
