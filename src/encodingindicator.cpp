#include "encodingindicator.h"

#include "logviewwidget.h"

#include <QActionGroup>
#include <QFileInfo>
#include <QMenu>

EncodingIndicator::EncodingIndicator(QWidget* parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("encodingIndicator"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::NoFocus);

    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("encodingMenu"));
    m_group = new QActionGroup(this);
    m_group->setExclusive(true);
    for (const TextEncoding encoding : choices()) {
        QAction* action = m_menu->addAction(TextEncodings::displayName(encoding));
        action->setCheckable(true);
        action->setData(int(encoding));
        action->setObjectName(QStringLiteral("encodingAction_") + TextEncodings::displayName(encoding));
        m_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, encoding]() { choose(encoding); });
    }
    m_menu->addSeparator();
    m_autoAction = m_menu->addAction(tr("Detect automatically"));
    m_autoAction->setObjectName(QStringLiteral("encodingAutoAction"));
    m_autoAction->setCheckable(true);
    m_group->addAction(m_autoAction);
    connect(m_autoAction, &QAction::triggered, this, &EncodingIndicator::chooseAutomatic);

    hide();
}

QList<TextEncoding> EncodingIndicator::choices()
{
    return TextEncodings::selectable();
}

QString EncodingIndicator::label(const FileEncoding& encoding, TextEncoding stream)
{
    if (!TextEncodings::isDecodable(stream))
        return tr("%1 (BOM)").arg(TextEncodings::displayName(stream));
    const QString name = TextEncodings::displayName(encoding.decoder.encoding());
    switch (encoding.source) {
    case FileEncoding::Source::Undecided:
        return tr("ASCII (auto)");
    case FileEncoding::Source::Auto:
        return encoding.provisional ? tr("%1 (auto, provisional)").arg(name)
                                    : tr("%1 (auto)").arg(name);
    case FileEncoding::Source::Bom:
        return tr("%1 (BOM)").arg(name);
    case FileEncoding::Source::User:
        return tr("%1 (manual)").arg(name);
    }
    return name;
}

void EncodingIndicator::setTab(LogViewWidget* tab)
{
    if (tab == m_tab && (tab || isHidden()))
        return;
    for (const QMetaObject::Connection& c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_tab = tab;
    if (tab) {
        const auto refreshNow = [this]() { refresh(); };
        m_connections
            << connect(tab, &LogViewWidget::currentRowChanged, this, refreshNow)
            << connect(tab, &LogViewWidget::fileEncodingChanged, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingStarted, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingFinished, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingFailed, this, refreshNow)
            << connect(tab->model(), &QAbstractItemModel::modelReset, this, refreshNow)
            << connect(tab->model(), &QAbstractItemModel::rowsInserted, this, refreshNow)
            << connect(tab->model(), &QAbstractItemModel::rowsRemoved, this, refreshNow)
            << connect(tab, &QObject::destroyed, this, [this]() {
                   // Объект уже не LogViewWidget: setTab сравнил бы его с
                   // QPointer<LogViewWidget> — приведение разрушенного (UBSan;
                   // так закрывалось окно с открытыми вкладками).
                   for (const QMetaObject::Connection& c : std::as_const(m_connections))
                       disconnect(c);
                   m_connections.clear();
                   m_tab.clear();
                   refresh();
               });
    }
    refresh();
}

void EncodingIndicator::refresh()
{
    LogViewWidget* tab = m_tab;
    LogFilePtr file;
    if (tab) {
        const QModelIndex current = tab->view()->currentIndex();
        const QList<LogFilePtr> files = tab->loadedFiles();
        if (current.isValid())
            file = tab->model()->keyAt(current.row()).file;
        if ((!file || !files.contains(file)) && !files.isEmpty())
            file = files.first();
        else if (!files.contains(file))
            file.reset();
    }
    m_file = file;
    if (!file) {
        hide();
        return;
    }

    const FileEncoding encoding = tab->fileEncoding(file);
    const TextEncoding stream = tab->fileStreamEncoding(file);
    const bool choosable = TextEncodings::isDecodable(stream);
    const QString text = label(encoding, stream);
    // Слитая вкладка: подпись называет файл текущей строки (U-23) — чья
    // строка, видно при любом режиме меток файлов.
    const QString shown = tab->fileCount() > 1
        ? file->shortName() + QStringLiteral(": ") + text : text;
    if (this->text() != shown)
        setText(shown);

    QString tip;
    if (!choosable) {
        tip = tr("Encoding of %1: %2, from the byte order mark. Such a file is read "
                 "in this encoding as a whole; it cannot be changed.");
    } else {
        switch (encoding.source) {
        case FileEncoding::Source::Undecided:
            tip = tr("Encoding of %1: only ASCII so far, so the text is the same in any "
                     "encoding. The encoding is detected at the first non-ASCII byte. "
                     "Click to choose one now.");
            break;
        case FileEncoding::Source::Auto:
            tip = encoding.provisional
                ? tr("Encoding of %1: %2, detected from few non-ASCII bytes and "
                     "re-checked as more arrive. Click to choose another one.")
                : tr("Encoding of %1: %2, detected automatically. Click to choose "
                     "another one.");
            break;
        case FileEncoding::Source::Bom:
            tip = tr("Encoding of %1: %2, from the byte order mark. Click to choose "
                     "another one.");
            break;
        case FileEncoding::Source::User:
            tip = tr("Encoding of %1: %2, chosen manually for as long as the tab is "
                     "open. Click to choose another one or to detect it automatically.");
            break;
        }
    }
    const QString fileName = QFileInfo(file->filePath).fileName();
    setToolTip(tip.contains(QLatin1String("%2")) ? tip.arg(fileName, text) : tip.arg(fileName));

    if (choosable) {
        if (menu() != m_menu) {
            setMenu(m_menu);
            setPopupMode(QToolButton::InstantPopup);
        }
        const bool manual = encoding.source == FileEncoding::Source::User;
        for (QAction* action : m_group->actions()) {
            if (action == m_autoAction)
                action->setChecked(!manual);
            else
                action->setChecked(manual
                                   && action->data().toInt() == int(encoding.decoder.encoding()));
        }
    } else if (menu()) {
        setMenu(nullptr);
    }
    show();
}

void EncodingIndicator::choose(TextEncoding encoding)
{
    if (m_tab && m_file)
        m_tab->setFileEncoding(m_file, encoding);
    refresh();
}

void EncodingIndicator::chooseAutomatic()
{
    if (m_tab && m_file)
        m_tab->restoreAutomaticEncoding(m_file);
    refresh();
}
