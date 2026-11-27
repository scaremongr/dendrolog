#include "schemalinepreview.h"

#include "cardframe.h"
#include "schemafieldstrip.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStyleHints>
#include <QTextBlock>
#include <QToolButton>
#include <QToolTip>

SchemaLinePreview::SchemaLinePreview(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setUndoRedoEnabled(false);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    viewport()->setMouseTracking(true);

    m_makeButton = new QToolButton(viewport());
    m_makeButton->setObjectName(QStringLiteral("previewMakeFieldBtn"));
    m_makeButton->setText(tr("Make Field"));
    m_makeButton->setToolTip(tr("Make the selected text a field of its own (Ctrl+M)"));
    m_makeButton->setCursor(Qt::ArrowCursor);
    m_makeButton->setFocusPolicy(Qt::NoFocus);   // щелчок не снимает выделение
    m_makeButton->setAutoRaise(false);
    m_makeButton->setFont(QApplication::font());
    m_makeButton->hide();
    connect(m_makeButton, &QToolButton::clicked, this, &SchemaLinePreview::makeFieldRequested);

    connect(this, &QPlainTextEdit::selectionChanged, this, [this]() {
        placeMakeButton();
        emit lineSelectionChanged();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &SchemaLinePreview::placeMakeButton);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &SchemaLinePreview::placeMakeButton);
}

void SchemaLinePreview::setLines(const QVector<Line>& lines)
{
    bool sameText = lines.size() == m_lines.size();
    for (int i = 0; sameText && i < lines.size(); ++i)
        sameText = lines[i].text == m_lines[i].text;
    m_lines = lines;
    if (!sameText) {
        QStringList text;
        text.reserve(lines.size());
        for (const Line& line : lines) {
            // Знаки, которые документ счёл бы концом абзаца, — пробелами: строка
            // лога — один блок, позиции в нём — позиции в строке.
            QString shown = line.text;
            for (QChar& ch : shown) {
                if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r') || ch == QChar(0x2028)
                    || ch == QChar(0x2029) || ch == QChar(0x000C))
                    ch = QLatin1Char(' ');
            }
            text << shown;
        }
        // Тот же текст — документ не трогается: прокрутка и выделение на
        // месте, меняется только раскраска (правка схемы — частый случай).
        const QSignalBlocker block(this);
        setPlainText(text.join(QLatin1Char('\n')));
    }
    recolor();
    placeMakeButton();
    if (!sameText)
        emit lineSelectionChanged();
}

void SchemaLinePreview::setColumnNames(const QStringList& names)
{
    m_names = names;
}

void SchemaLinePreview::setCurrentColumn(int column)
{
    if (m_current == column)
        return;
    m_current = column;
    recolor();
}

void SchemaLinePreview::recolor()
{
    const QPalette pal = palette();
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor missBg = CardFrame::mixedColor(base, QColor(0xd0, 0x30, 0x30), 0.20);
    const QColor partialBg = CardFrame::mixedColor(base, QColor(0xd0, 0x30, 0x30), 0.09);
    const QColor muted = CardFrame::mutedTextColor(pal);

    QList<QTextEdit::ExtraSelection> selections;
    QTextBlock block = document()->firstBlock();
    for (int i = 0; i < m_lines.size() && block.isValid(); ++i, block = block.next()) {
        const Line& line = m_lines[i];
        if (line.state == Line::State::Missed || line.state == Line::State::Partial) {
            // Не прочитана вовсе — гуще; прочитана до половины (остаток ушёл
            // в последнее поле) — бледнее, поля поверх.
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(line.state == Line::State::Missed ? missBg : partialBg);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(block);
            selections << sel;
        } else if (line.state == Line::State::Other) {
            QTextEdit::ExtraSelection sel;
            sel.format.setForeground(muted);
            QTextCursor c(block);
            c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            sel.cursor = c;
            selections << sel;
        }
        for (const Span& span : line.spans) {
            if (span.column < 0 || span.length <= 0 || span.start < 0 || span.start + span.length > block.length())
                continue;
            QTextEdit::ExtraSelection sel;
            QColor fill = SchemaFieldStrip::fieldColor(span.column, pal);
            if (span.column == m_current) {
                // Выбранное поле — гуще и подчёркнуто: видно во всех строках сразу.
                fill = CardFrame::mixedColor(fill, text, 0.18);
                sel.format.setFontUnderline(true);
            }
            sel.format.setBackground(fill);
            sel.format.setForeground(text);
            QTextCursor c(block);
            c.setPosition(block.position() + span.start);
            c.setPosition(block.position() + span.start + span.length, QTextCursor::KeepAnchor);
            sel.cursor = c;
            selections << sel;
        }
    }
    setExtraSelections(selections);
}

SchemaLinePreview::Selection SchemaLinePreview::lineSelection() const
{
    const QTextCursor c = textCursor();
    if (!c.hasSelection())
        return {};
    const QTextBlock a = document()->findBlock(c.selectionStart());
    const QTextBlock b = document()->findBlock(c.selectionEnd());
    if (!a.isValid() || a != b)
        return {};
    Selection s;
    s.line = a.blockNumber();
    s.start = c.selectionStart() - a.position();
    s.length = c.selectionEnd() - c.selectionStart();
    return s;
}

void SchemaLinePreview::selectInLine(int line, int start, int length)
{
    const QTextBlock block = document()->findBlockByNumber(line);
    if (!block.isValid())
        return;
    const int max = block.length() - 1;
    start = qBound(0, start, max);
    length = qBound(0, length, max - start);
    QTextCursor c(block);
    c.setPosition(block.position() + start);
    c.setPosition(block.position() + start + length, QTextCursor::KeepAnchor);
    setTextCursor(c);
}

void SchemaLinePreview::clearLineSelection()
{
    QTextCursor c = textCursor();
    if (!c.hasSelection())
        return;
    c.clearSelection();
    setTextCursor(c);
}

int SchemaLinePreview::columnAt(int line, int position) const
{
    if (line < 0 || line >= m_lines.size())
        return -1;
    for (const Span& span : m_lines[line].spans) {
        if (position >= span.start && position < span.start + span.length)
            return span.column;
    }
    return -1;
}

SchemaLinePreview::Selection SchemaLinePreview::positionAt(const QPoint& viewportPos) const
{
    const QTextCursor c = cursorForPosition(viewportPos);
    const QTextBlock block = c.block();
    if (!block.isValid() || block.blockNumber() >= m_lines.size())
        return {};   // пустой документ — один блок без строки
    // cursorForPosition отвечает ближайшей позицией и мимо текста — сверяем
    // строку и правый край текста.
    const QRectF br = blockBoundingGeometry(block).translated(contentOffset());
    if (viewportPos.y() < br.top() || viewportPos.y() >= br.bottom())
        return {};
    QTextCursor end(block);
    end.movePosition(QTextCursor::EndOfBlock);
    if (viewportPos.x() > cursorRect(end).right())
        return {};
    Selection s;
    s.line = block.blockNumber();
    s.start = qMax(0, c.positionInBlock() - (cursorRect(c).left() > viewportPos.x() ? 1 : 0));
    s.length = 0;
    return s;
}

QPoint SchemaLinePreview::spanCenter(int line, int column) const
{
    if (line < 0 || line >= m_lines.size())
        return {};
    const QTextBlock block = document()->findBlockByNumber(line);
    for (const Span& span : m_lines[line].spans) {
        if (span.column != column || !block.isValid())
            continue;
        QTextCursor c(block);
        c.setPosition(block.position() + span.start + span.length / 2);
        return cursorRect(c).center();
    }
    return {};
}

void SchemaLinePreview::placeMakeButton()
{
    const Selection s = lineSelection();
    if (!s.isValid()) {
        m_makeButton->hide();
        return;
    }
    QTextCursor end(document());
    end.setPosition(textCursor().selectionEnd());
    const QRect r = cursorRect(end);
    m_makeButton->adjustSize();
    const QSize size = m_makeButton->size();
    int x = r.right() + 8;
    int y = r.bottom() + 2;
    const QRect area = viewport()->rect();
    if (x + size.width() > area.right())
        x = qMax(area.left(), area.right() - size.width());
    if (y + size.height() > area.bottom())
        y = r.top() - size.height() - 2;   // у нижнего края — над строкой
    if (y < area.top())
        y = area.top();
    m_makeButton->move(x, y);
    m_makeButton->show();
    m_makeButton->raise();
}

void SchemaLinePreview::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressPos = event->pos();
        m_pressed = true;
    }
    QPlainTextEdit::mousePressEvent(event);
}

void SchemaLinePreview::mouseReleaseEvent(QMouseEvent* event)
{
    QPlainTextEdit::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton || !m_pressed)
        return;
    m_pressed = false;
    // Щелчок (не протяжка и не двойной щелчок со словом) по цветному участку
    // — выбор поля.
    if ((event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()
        || textCursor().hasSelection())
        return;
    const Selection at = positionAt(event->pos());
    if (at.line < 0)
        return;
    const int column = columnAt(at.line, at.start);
    if (column >= 0)
        emit columnClicked(column);
}

void SchemaLinePreview::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    placeMakeButton();
}

void SchemaLinePreview::changeEvent(QEvent* event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange)
        recolor();
}

bool SchemaLinePreview::viewportEvent(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const Selection at = positionAt(help->pos());
        const int column = at.line >= 0 ? columnAt(at.line, at.start) : -1;
        if (column >= 0) {
            QString value;
            for (const Span& span : m_lines[at.line].spans) {
                if (span.column == column)
                    value = m_lines[at.line].text.mid(span.start, span.length);
            }
            QString tip = tr("%1: %2").arg(m_names.value(column, tr("Field %1").arg(column + 1)), value);
            if (m_lines[at.line].state == Line::State::Partial)
                tip += QLatin1Char('\n') + tr("The schema reads this line only in part: the rest of it went "
                                              "into the last field.");
            QToolTip::showText(help->globalPos(), tip, viewport());
        } else if (at.line >= 0 && m_lines[at.line].state == Line::State::Missed) {
            QToolTip::showText(help->globalPos(), tr("The schema does not read this line"), viewport());
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QPlainTextEdit::viewportEvent(event);
}

void SchemaLinePreview::contextMenuEvent(QContextMenuEvent* event)
{
    QMenu* menu = createStandardContextMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    QAction* make = new QAction(tr("Make Field from Selection"), menu);
    make->setEnabled(lineSelection().isValid());
    connect(make, &QAction::triggered, this, &SchemaLinePreview::makeFieldRequested);
    menu->insertAction(menu->actions().value(0), make);
    menu->insertSeparator(menu->actions().value(1));
    menu->popup(event->globalPos());
}
