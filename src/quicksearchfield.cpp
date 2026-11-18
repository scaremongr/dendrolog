#include "quicksearchfield.h"

#include "apptheme.h"
#include "cardframe.h"

#include <QAction>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>

QuickSearchField::QuickSearchField(QWidget* parent)
    : QLineEdit(parent)
{
    // Буквенные переключатели в стиле «Aa»: «ab» подчёркнутое — слово
    // целиком, «.*» — регулярное выражение.
    m_matchCase = new QAction(QStringLiteral("Aa"), this);
    m_matchCase->setObjectName(QStringLiteral("quickSearchMatchCase"));
    m_matchCase->setCheckable(true);
    m_matchCase->setToolTip(tr("Match case"));

    m_wholeWord = new QAction(QStringLiteral("ab"), this);
    m_wholeWord->setObjectName(QStringLiteral("quickSearchWholeWord"));
    m_wholeWord->setCheckable(true);
    m_wholeWord->setToolTip(tr("Whole word: the match may not touch a letter,\n"
                               "digit or underscore on either side"));
    QFont underlined = m_wholeWord->font();
    underlined.setUnderline(true);
    m_wholeWord->setFont(underlined);

    m_regex = new QAction(QStringLiteral(".*"), this);
    m_regex->setObjectName(QStringLiteral("quickSearchRegex"));
    m_regex->setCheckable(true);
    m_regex->setToolTip(tr("Regular expression: the same syntax as in Text Filters,\n"
                           "found anywhere in the line"));

    for (QAction* toggle : {m_matchCase, m_wholeWord, m_regex}) {
        connect(toggle, &QAction::toggled, this, [this]() {
            setNotFound(false);
            updateValidity();
            emit optionsChanged();
        });
    }

    m_historyButton = new QAction(tr("Recent searches"), this);
    m_historyButton->setObjectName(QStringLiteral("quickSearchHistoryButton"));
    m_historyButton->setToolTip(tr("Recent searches (Down arrow)"));
    addAction(m_historyButton, QLineEdit::TrailingPosition);
    connect(m_historyButton, &QAction::triggered, this, &QuickSearchField::showHistory);

    m_historyMenu = new QMenu(this);
    m_historyMenu->setObjectName(QStringLiteral("quickSearchHistoryMenu"));

    connect(this, &QLineEdit::textChanged, this, [this]() {
        setNotFound(false);
        updateValidity();
    });
}

TextQuery QuickSearchField::query() const
{
    TextQuery q;
    q.text = text();
    q.caseSensitive = m_matchCase->isChecked();
    q.wholeWord = m_wholeWord->isChecked();
    q.regex = m_regex->isChecked();
    return q;
}

void QuickSearchField::setHistory(const QStringList& entries)
{
    m_history = QuickSearchHistory(entries);
}

void QuickSearchField::remember(const QString& text)
{
    if (m_history.add(text))
        emit historyChanged(m_history.entries());
}

void QuickSearchField::clearHistory()
{
    if (m_history.isEmpty())
        return;
    m_history.clear();
    emit historyChanged(m_history.entries());
}

void QuickSearchField::showHistory()
{
    m_historyMenu->clear();
    const QFontMetrics metrics(m_historyMenu->font());
    // Длинный запрос (до 500 символов) не растягивает меню на весь экран.
    const int maxWidth = qMax(width(), metrics.horizontalAdvance(QLatin1Char('x')) * 60);
    for (const QString& entry : m_history.entries()) {
        QString label = metrics.elidedText(entry, Qt::ElideRight, maxWidth);
        label.replace(QLatin1Char('&'), QLatin1String("&&"));
        label.replace(QLatin1Char('\t'), QLatin1Char(' '));
        QAction* item = m_historyMenu->addAction(label);
        item->setData(entry);
        if (label != entry)
            item->setToolTip(entry);
        connect(item, &QAction::triggered, this, [this, entry]() {
            setText(entry);
            setFocus(Qt::OtherFocusReason);
            emit historyActivated(entry);
        });
    }
    if (m_history.isEmpty()) {
        QAction* none = m_historyMenu->addAction(tr("No recent searches"));
        none->setEnabled(false);
    }
    m_historyMenu->addSeparator();
    QAction* clear = m_historyMenu->addAction(tr("Clear search history"));
    clear->setObjectName(QStringLiteral("quickSearchClearHistory"));
    clear->setEnabled(!m_history.isEmpty());
    connect(clear, &QAction::triggered, this, &QuickSearchField::clearHistory);

    if (QAction* first = m_historyMenu->actions().value(0); first && first->isEnabled())
        m_historyMenu->setActiveAction(first);
    m_historyMenu->popup(mapToGlobal(rect().bottomLeft()));
}

void QuickSearchField::setNotFound(bool notFound)
{
    if (m_notFound == notFound)
        return;
    m_notFound = notFound;
    refreshStyle();
}

void QuickSearchField::setBaseToolTip(const QString& tip)
{
    m_baseToolTip = tip;
    refreshStyle();
}

void QuickSearchField::updateValidity()
{
    QString message;
    QString error;
    int offset = -1;
    if (!query().isValid(&error, &offset)) {
        // Те же слова, что у правила в панели Text Filters.
        message = offset >= 0
            ? tr("Invalid regular expression at position %1: %2").arg(offset).arg(error)
            : tr("Invalid regular expression: %1").arg(error);
    }
    if (message == m_error)
        return;
    m_error = message;
    refreshStyle();
}

void QuickSearchField::refreshStyle()
{
    const AppTheme& theme = AppTheme::instance();
    QString style;
    if (!m_error.isEmpty()) {
        style = QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(theme.logError.name());
    } else if (m_notFound) {
        const QColor bg = CardFrame::mixedColor(theme.logError, palette().color(QPalette::Base), 0.72);
        style = QStringLiteral("QLineEdit { background-color: %1; }").arg(bg.name());
    }
    if (styleSheet() != style)
        setStyleSheet(style);
    const QString tip = m_error.isEmpty() ? m_baseToolTip : m_error;
    if (toolTip() != tip)
        setToolTip(tip);
}

void QuickSearchField::keyPressEvent(QKeyEvent* event)
{
    // Стрелка вниз — история, в пустом поле и в набранном: у однострочного
    // поля стрелке больше нечего делать.
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    if (event->key() == Qt::Key_Down && (mods == Qt::NoModifier || mods == Qt::AltModifier)) {
        showHistory();
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}
