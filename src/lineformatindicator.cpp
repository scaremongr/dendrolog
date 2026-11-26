#include "lineformatindicator.h"

#include "logviewwidget.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMenu>

namespace {

QStringList splitKeys(const QString& text)
{
    QStringList keys;
    for (const QString& part : text.split(QLatin1Char(','))) {
        const QString key = part.trimmed();
        if (!key.isEmpty())
            keys << key;
    }
    return keys;
}

QString joinKeys(const QStringList& keys)
{
    return keys.join(QStringLiteral(", "));
}

} // namespace

LineFormatIndicator::LineFormatIndicator(QWidget* parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("lineFormatIndicator"));
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    setFocusPolicy(Qt::NoFocus);

    m_menu = new QMenu(this);
    m_menu->setObjectName(QStringLiteral("lineFormatMenu"));
    m_group = new QActionGroup(this);
    m_group->setExclusive(true);
    const auto add = [this](const QString& text, const QString& name, LineFormat::Kind kind) {
        QAction* action = m_menu->addAction(text);
        action->setObjectName(name);
        action->setCheckable(true);
        m_group->addAction(action);
        connect(action, &QAction::triggered, this, [this, kind]() { choose(kind); });
        return action;
    };
    m_plainAction = add(tr("Plain text"), QStringLiteral("lineFormatPlain"), LineFormat::Kind::Plain);
    m_jsonAction = add(tr("JSON Lines"), QStringLiteral("lineFormatJson"), LineFormat::Kind::Json);
    m_logfmtAction = add(tr("logfmt"), QStringLiteral("lineFormatLogfmt"), LineFormat::Kind::Logfmt);
    m_keysAction = m_menu->addAction(tr("Keys…"));
    m_keysAction->setObjectName(QStringLiteral("lineFormatKeys"));
    connect(m_keysAction, &QAction::triggered, this, &LineFormatIndicator::chooseKeys);
    m_menu->addSeparator();
    m_autoAction = m_menu->addAction(tr("Detect automatically"));
    m_autoAction->setObjectName(QStringLiteral("lineFormatAuto"));
    m_autoAction->setCheckable(true);
    m_group->addAction(m_autoAction);
    connect(m_autoAction, &QAction::triggered, this, &LineFormatIndicator::chooseAutomatic);
    setMenu(m_menu);
    setPopupMode(QToolButton::InstantPopup);
    hide();
}

QString LineFormatIndicator::label(const LineFormat& format)
{
    const QString name = format.displayName();
    switch (format.origin) {
    case LineFormat::Origin::Undecided:
        return tr("%1 (auto, few lines)").arg(name);
    case LineFormat::Origin::Auto:
        return tr("%1 (auto)").arg(name);
    case LineFormat::Origin::User:
        return tr("%1 (manual)").arg(name);
    }
    return name;
}

void LineFormatIndicator::setTab(LogViewWidget* tab)
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
            << connect(tab, &LogViewWidget::lineFormatChanged, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingStarted, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingFinished, this, refreshNow)
            << connect(tab, &LogViewWidget::fileParsingFailed, this, refreshNow)
            << connect(tab->model(), &QAbstractItemModel::modelReset, this, refreshNow)
            << connect(tab, &QObject::destroyed, this, [this]() {
                   for (const QMetaObject::Connection& c : std::as_const(m_connections))
                       disconnect(c);
                   m_connections.clear();
                   m_tab.clear();
                   refresh();
               });
    }
    refresh();
}

void LineFormatIndicator::refresh()
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
    const LineFormat format = tab->lineFormat(file);
    const QString text = label(format);
    const QString shown = tab->fileCount() > 1 ? file->shortName() + QStringLiteral(": ") + text : text;
    if (this->text() != shown)
        setText(shown);

    QString tip;
    switch (format.origin) {
    case LineFormat::Origin::Undecided:
        tip = tr("Line format of %1: read as plain text until the file has 20 lines; then the "
                 "format is detected (JSON Lines, logfmt or plain text). Click to choose one now.");
        break;
    case LineFormat::Origin::Auto:
        tip = format.isStructured()
            ? tr("Line format of %1: %2, detected automatically — time, level and message come "
                 "from the keys. Click to choose another one.")
            : format.levelStartsRecord
                ? tr("Line format of %1: plain text, detected automatically; the lines have no "
                     "timestamps, so a line starting with a level word starts a record. Click "
                     "to choose another one.")
                : tr("Line format of %1: plain text, detected automatically. Click to choose "
                     "another one.");
        break;
    case LineFormat::Origin::User:
        tip = tr("Line format of %1: %2, chosen manually (kept with the file's state). Click to "
                 "choose another one or to detect it automatically.");
        break;
    }
    setToolTip(tip.contains(QLatin1String("%2")) ? tip.arg(file->shortName(), format.displayName())
                                                 : tip.arg(file->shortName()));
    const bool manual = format.isUser();
    m_plainAction->setChecked(manual && format.kind == LineFormat::Kind::Plain);
    m_jsonAction->setChecked(manual && format.kind == LineFormat::Kind::Json);
    m_logfmtAction->setChecked(manual && format.kind == LineFormat::Kind::Logfmt);
    m_autoAction->setChecked(!manual);
    show();
}

void LineFormatIndicator::choose(LineFormat::Kind kind)
{
    if (!m_tab || !m_file)
        return;
    const LineFormat current = m_tab->lineFormat(m_file);
    LineFormat chosen;
    if (kind == LineFormat::Kind::Plain) {
        chosen = LineFormat::plain(LineFormat::Origin::User);
        // Решение «ведущий уровень начинает запись» — часть plain того же файла.
        chosen.levelStartsRecord = current.kind == LineFormat::Kind::Plain && current.levelStartsRecord;
    } else {
        // Тот же вид — ключи прежние (решённые по файлу), иначе по умолчанию.
        KeyedDefinition keys = current.kind == kind ? current.keys : KeyedDefinition();
        chosen = LineFormat::structured(kind, keys, LineFormat::Origin::User);
    }
    m_tab->setLineFormat(m_file, chosen);
    refresh();
}

void LineFormatIndicator::chooseKeys()
{
    if (!m_tab || !m_file)
        return;
    LineFormatKeysDialog dialog(m_tab->lineFormat(m_file), window());
    if (dialog.exec() != QDialog::Accepted || !m_tab || !m_file)
        return;
    m_tab->setLineFormat(m_file, dialog.format());
    refresh();
}

void LineFormatIndicator::chooseAutomatic()
{
    if (m_tab && m_file)
        m_tab->restoreAutomaticLineFormat(m_file);
    refresh();
}

// ============================================================================
// LineFormatKeysDialog
// ============================================================================

LineFormatKeysDialog::LineFormatKeysDialog(const LineFormat& current, QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("lineFormatKeysDialog"));
    setWindowTitle(tr("Line Format Keys"));
    const KeyedDefinition keys = current.isStructured() ? current.keys : KeyedDefinition();
    m_columns = keys;
    auto* form = new QFormLayout;
    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("lineFormatKind"));
    m_kind->addItem(tr("JSON Lines"), int(LineFormat::Kind::Json));
    m_kind->addItem(tr("logfmt"), int(LineFormat::Kind::Logfmt));
    m_kind->setCurrentIndex(current.kind == LineFormat::Kind::Logfmt ? 1 : 0);
    form->addRow(tr("Format:"), m_kind);
    const auto edit = [this, form](const QString& label, const QString& name, const QStringList& value) {
        auto* e = new QLineEdit(joinKeys(value), this);
        e->setObjectName(name);
        e->setToolTip(tr("Keys in order of priority, separated by commas; a dotted key is also a "
                         "path into nested objects (log.level)."));
        form->addRow(label, e);
        return e;
    };
    m_time = edit(tr("Time keys:"), QStringLiteral("lineFormatTimeKeys"), keys.timeKeys);
    m_level = edit(tr("Level keys:"), QStringLiteral("lineFormatLevelKeys"), keys.levelKeys);
    m_message = edit(tr("Message keys:"), QStringLiteral("lineFormatMessageKeys"), keys.messageKeys);
    m_epoch = new QComboBox(this);
    m_epoch->setObjectName(QStringLiteral("lineFormatEpoch"));
    using Unit = KeyedDefinition::EpochUnit;
    for (const auto& u : {std::pair{Unit::Auto, tr("By magnitude")}, std::pair{Unit::Seconds, tr("Seconds")},
                          std::pair{Unit::Millis, tr("Milliseconds")}, std::pair{Unit::Micros, tr("Microseconds")},
                          std::pair{Unit::Nanos, tr("Nanoseconds")}})
        m_epoch->addItem(u.second, int(u.first));
    m_epoch->setCurrentIndex(qMax(0, m_epoch->findData(int(keys.epochUnit))));
    form->addRow(tr("Epoch numbers:"), m_epoch);
    m_levels = new QComboBox(this);
    m_levels->setObjectName(QStringLiteral("lineFormatLevels"));
    using Levels = KeyedDefinition::NumericLevels;
    for (const auto& l : {std::pair{Levels::Auto, tr("Auto (syslog 0–7, pino 10–60)")},
                          std::pair{Levels::Syslog, tr("syslog 0–7")},
                          std::pair{Levels::Python, tr("Python 10–50")}})
        m_levels->addItem(l.second, int(l.first));
    m_levels->setCurrentIndex(qMax(0, m_levels->findData(int(keys.numericLevels))));
    form->addRow(tr("Numeric levels:"), m_levels);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
    setLayout(form);
}

LineFormat LineFormatKeysDialog::format() const
{
    KeyedDefinition k = m_columns;
    k.timeKeys = splitKeys(m_time->text());
    k.levelKeys = splitKeys(m_level->text());
    k.messageKeys = splitKeys(m_message->text());
    k.epochUnit = KeyedDefinition::EpochUnit(m_epoch->currentData().toInt());
    k.numericLevels = KeyedDefinition::NumericLevels(m_levels->currentData().toInt());
    return LineFormat::structured(LineFormat::Kind(m_kind->currentData().toInt()), k,
                                  LineFormat::Origin::User);
}
