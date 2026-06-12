#include "patternblockcard.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

struct KindEntry {
    PatternBlock::MatchKind kind;
    const char* label;
    const char* toolTip;
};

const KindEntry kKindEntries[] = {
    { PatternBlock::MatchKind::TextUntilSeparator, "Text",
      "Shortest text until the next separator / block.\n"
      "The value may be empty — a missing field does not break the line." },
    { PatternBlock::MatchKind::GreedyTextUntilSeparator, "Greedy text",
      "Longest text before the next separator. Use for values that may contain the separator text." },
    { PatternBlock::MatchKind::Timestamp, "Timestamp",
      "Date/time token. Acts as an anchor: it is found even after leading garbage,\n"
      "and needs no separator after it." },
    { PatternBlock::MatchKind::Level, "Level",
      "TRACE / DEBUG / INFO / WARN / ERROR / FATAL... Acts as an anchor." },
    { PatternBlock::MatchKind::Integer, "Integer",
      "One or more digits with an optional sign." },
    { PatternBlock::MatchKind::HexText, "Hex",
      "Hex token such as 0x00006d40 or ABCDEF." },
    { PatternBlock::MatchKind::IpAddress, "IP address",
      "IPv4 address with an optional :port. Acts as an anchor." },
    { PatternBlock::MatchKind::FilePath, "File path",
      "Windows / UNC / Unix-like path text." },
    { PatternBlock::MatchKind::ConstantText, "Constant text",
      "Exact literal text at this position (set it under ⚙ Advanced).\n"
      "Give the block a name to expose the literal as a field column;\n"
      "leave the name empty to keep it pure line structure." },
    { PatternBlock::MatchKind::CustomRegex, "Custom regex",
      "Your own regular expression for this block (set it under ⚙ Advanced)." },
    { PatternBlock::MatchKind::Remainder, "Remainder of line",
      "Everything to the end of the line. Must be the last block." },
};

} // namespace

PatternBlockCard::PatternBlockCard(QWidget* parent)
    : CardFrame(parent)
{
    // Контейнер (полоска-акцент, рамка, скругление) живёт в CardFrame.
    QVBoxLayout* rows = rowsLayout();

    // ---- Main row: identical columns for every block type ------------ //
    m_mainRow = new QHBoxLayout();
    m_mainRow->setContentsMargins(0, 0, 0, 0);
    m_mainRow->setSpacing(5);
    rows->addLayout(m_mainRow);

    // Column 1: the type. Sized to the widest entry so no label is ever
    // cut off — neither in the field nor in the popup list.
    m_kindCombo = new QComboBox(this);
    for (const KindEntry& entry : kKindEntries) {
        m_kindCombo->addItem(tr(entry.label), static_cast<int>(entry.kind));
        m_kindCombo->setItemData(m_kindCombo->count() - 1, tr(entry.toolTip), Qt::ToolTipRole);
    }
    m_kindCombo->setFocusPolicy(Qt::StrongFocus); // prevent accidental wheel change
    {
        int textWidth = 0;
        const QFontMetrics fm = m_kindCombo->fontMetrics();
        for (int i = 0; i < m_kindCombo->count(); ++i)
            textWidth = qMax(textWidth, fm.horizontalAdvance(m_kindCombo->itemText(i)));
        const int comboWidth = textWidth + 56; // frame + arrow + padding
        m_kindCombo->setFixedWidth(comboWidth);
        m_kindCombo->view()->setMinimumWidth(comboWidth);
    }
    m_mainRow->addWidget(m_kindCombo);

    // Column 2: field name.
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setToolTip(tr("Column name for the extracted value."));
    m_nameEdit->setMinimumWidth(90);
    m_mainRow->addWidget(m_nameEdit, 1);

    // Column 3: blind-spot toggle.
    m_ignoreCheck = new QCheckBox(tr("hide"), this);
    m_ignoreCheck->setToolTip(tr("Blind spot: the text is matched and consumed,\n"
                                 "but not shown as a field column."));
    m_mainRow->addWidget(m_ignoreCheck);

    // Column 4: row actions (единый стиль кнопок — CardFrame::makeToolButton).
    m_wrapBtn = makeToolButton(QStringLiteral("⚙"),
        tr("Advanced: the matched text / regex and wrappers around the value."));
    m_wrapBtn->setCheckable(true);
    m_upBtn     = makeToolButton(QStringLiteral("↑"), tr("Move block up"));
    m_downBtn   = makeToolButton(QStringLiteral("↓"), tr("Move block down"));
    m_removeBtn = makeToolButton(QStringLiteral("✕"), tr("Remove block"));
    m_mainRow->addWidget(m_wrapBtn);
    m_mainRow->addWidget(m_upBtn);
    m_mainRow->addWidget(m_downBtn);
    m_mainRow->addWidget(m_removeBtn);
    // The glue widget (SeparatorNode) is appended here by attachGlueWidget().

    // ---- Advanced row (⚙): value pattern + wrappers ------------------- //
    m_wrapRow = new QWidget(this);
    auto* wrapLayout = new QHBoxLayout(m_wrapRow);
    wrapLayout->setContentsMargins(4, 0, 0, 0);
    wrapLayout->setSpacing(5);

    m_patternLabel = new QLabel(tr("Text"), m_wrapRow);
    m_patternLabel->setStyleSheet(QStringLiteral("color: palette(mid); border: none;"));
    wrapLayout->addWidget(m_patternLabel);

    m_patternEdit = new QLineEdit(m_wrapRow);
    m_patternEdit->setMinimumWidth(120);
    wrapLayout->addWidget(m_patternEdit, 1);

    m_wrapLabel = new QLabel(tr("Enclosed by"), m_wrapRow);
    m_wrapLabel->setStyleSheet(QStringLiteral("color: palette(mid); border: none;"));
    wrapLayout->addWidget(m_wrapLabel);

    m_openEdit = new QLineEdit(m_wrapRow);
    m_openEdit->setPlaceholderText(tr("opening, e.g. ["));
    m_openEdit->setToolTip(tr("Literal immediately before the value.\n"
                              "Part of the block: for anchors the bracket pair is searched together with the token."));
    m_openEdit->setFixedWidth(90);
    wrapLayout->addWidget(m_openEdit);

    m_andLabel = new QLabel(tr("and"), m_wrapRow);
    m_andLabel->setStyleSheet(QStringLiteral("color: palette(mid); border: none;"));
    wrapLayout->addWidget(m_andLabel);

    m_closeEdit = new QLineEdit(m_wrapRow);
    m_closeEdit->setPlaceholderText(tr("closing, e.g. ]"));
    m_closeEdit->setToolTip(tr("Literal immediately after the value."));
    m_closeEdit->setFixedWidth(90);
    wrapLayout->addWidget(m_closeEdit);

    wrapLayout->addStretch(1);
    m_wrapRow->setVisible(false);
    rows->addWidget(m_wrapRow);

    // ---- Signals ------------------------------------------------------ //
    connect(m_nameEdit,    &QLineEdit::textEdited, this, &PatternBlockCard::blockEdited);
    connect(m_patternEdit, &QLineEdit::textEdited, this, &PatternBlockCard::blockEdited);
    connect(m_openEdit,    &QLineEdit::textEdited, this, &PatternBlockCard::blockEdited);
    connect(m_closeEdit,   &QLineEdit::textEdited, this, &PatternBlockCard::blockEdited);
    // Advanced content affects the gear hint and (for the closing
    // wrapper) the self-delimiting border.
    connect(m_patternEdit, &QLineEdit::textEdited, this, [this]() { updateGearHighlight(); });
    connect(m_openEdit,    &QLineEdit::textEdited, this, [this]() { updateGearHighlight(); });
    connect(m_closeEdit,   &QLineEdit::textEdited, this, [this]() {
        updateGearHighlight();
        applyFrameStyle();
    });
    connect(m_ignoreCheck, &QCheckBox::clicked,    this, &PatternBlockCard::blockEdited);
    connect(m_kindCombo, QOverload<int>::of(&QComboBox::activated),
            this, &PatternBlockCard::onKindChanged);
    connect(m_wrapBtn, &QToolButton::toggled, this, [this](bool on) {
        m_wrapRow->setVisible(on);
    });
    connect(m_upBtn,     &QToolButton::clicked, this, &PatternBlockCard::moveUpRequested);
    connect(m_downBtn,   &QToolButton::clicked, this, &PatternBlockCard::moveDownRequested);
    connect(m_removeBtn, &QToolButton::clicked, this, &PatternBlockCard::removeRequested);

    refreshFieldStates();
}

void PatternBlockCard::setBlock(const PatternBlock& block)
{
    m_nameEdit->setText(block.name);
    m_patternEdit->setText(block.customRegex);
    m_openEdit->setText(block.leadingText);
    m_closeEdit->setText(block.closingText);
    m_ignoreCheck->setChecked(block.ignored);

    const int index = m_kindCombo->findData(static_cast<int>(block.matchKind));
    m_kindCombo->setCurrentIndex(index >= 0 ? index : 0);

    // Loaded blocks stay collapsed; the highlighted ⚙ hints that the
    // advanced row carries content.
    m_wrapBtn->setChecked(false);
    m_wrapRow->setVisible(false);

    refreshFieldStates();
}

PatternBlock PatternBlockCard::block() const
{
    PatternBlock block;
    block.matchKind = static_cast<PatternBlock::MatchKind>(m_kindCombo->currentData().toInt());
    block.name = m_nameEdit->text().trimmed();
    block.leadingText = m_openEdit->text();
    block.closingText = m_closeEdit->text();
    block.customRegex = m_patternEdit->text();
    block.ignored = m_ignoreCheck->isChecked();
    // block.separator stays empty: separators are SeparatorNode links
    // owned by the dialog, not card properties.

    // Drop values from inputs hidden for the current kind so that stale
    // text never leaks into the compiled schema. (The widgets keep their
    // text so switching the kind back restores the configuration.)
    if (block.matchKind != PatternBlock::MatchKind::ConstantText
            && block.matchKind != PatternBlock::MatchKind::CustomRegex)
        block.customRegex.clear();
    if (block.matchKind == PatternBlock::MatchKind::ConstantText) {
        // The constant's literal IS its own wrapper.
        block.leadingText.clear();
        block.closingText.clear();
    }
    return block;
}

void PatternBlockCard::setAccentColor(const QColor& color)
{
    CardFrame::setAccentColor(color);
    updateGearHighlight();
    applyFrameStyle();
}

void PatternBlockCard::attachGlueWidget(QWidget* widget)
{
    if (!widget)
        return;
    widget->setParent(this);
    m_mainRow->addSpacing(8);
    m_mainRow->addWidget(widget);
}

void PatternBlockCard::setPosition(int index, int count)
{
    m_upBtn->setEnabled(index > 0);
    m_downBtn->setEnabled(index < count - 1);
}

void PatternBlockCard::onKindChanged()
{
    refreshFieldStates();

    // Switching to a kind whose essence lives in the advanced row
    // (Constant text / Custom regex) opens it so the user sees where
    // to type. Loaded blocks stay collapsed (see setBlock).
    const auto kind = static_cast<PatternBlock::MatchKind>(m_kindCombo->currentData().toInt());
    if (kind == PatternBlock::MatchKind::ConstantText
            || kind == PatternBlock::MatchKind::CustomRegex) {
        m_wrapBtn->setChecked(true);
        m_wrapRow->setVisible(true);
    }

    emit blockEdited();
}

void PatternBlockCard::refreshFieldStates()
{
    const auto kind = static_cast<PatternBlock::MatchKind>(m_kindCombo->currentData().toInt());
    const bool isConstant = kind == PatternBlock::MatchKind::ConstantText;
    const bool isRegex    = kind == PatternBlock::MatchKind::CustomRegex;

    m_nameEdit->setPlaceholderText(isConstant ? tr("name (optional)")
                                              : tr("field name"));
    m_nameEdit->setToolTip(isConstant
        ? tr("Optional: name the constant to expose it as a field column.\n"
             "Leave empty to keep it pure line structure.")
        : tr("Column name for the extracted value."));

    // Advanced row: the pattern input exists only for the two kinds that
    // need it. Wrappers make no sense for a constant (its literal IS the
    // wrapper), so they are hidden there.
    m_patternLabel->setVisible(isConstant || isRegex);
    m_patternEdit->setVisible(isConstant || isRegex);
    m_wrapLabel->setVisible(!isConstant);
    m_openEdit->setVisible(!isConstant);
    m_andLabel->setVisible(!isConstant);
    m_closeEdit->setVisible(!isConstant);
    if (isConstant) {
        m_patternLabel->setText(tr("Text"));
        m_patternEdit->setPlaceholderText(tr("exact text to match"));
        m_patternEdit->setToolTip(tr("Literal text matched at this position, e.g. ' - ' or 'REQ>'."));
    } else if (isRegex) {
        m_patternLabel->setText(tr("Regex"));
        m_patternEdit->setPlaceholderText(tr("regex pattern"));
        m_patternEdit->setToolTip(tr("Regular expression for the value of this block."));
    }

    updateGearHighlight();
    applyFrameStyle();
}

void PatternBlockCard::updateGearHighlight()
{
    const auto kind = static_cast<PatternBlock::MatchKind>(m_kindCombo->currentData().toInt());
    const bool patternRelevant = kind == PatternBlock::MatchKind::ConstantText
                              || kind == PatternBlock::MatchKind::CustomRegex;
    const bool hasContent =
        (patternRelevant && !m_patternEdit->text().isEmpty())
        || (kind != PatternBlock::MatchKind::ConstantText
            && (!m_openEdit->text().isEmpty() || !m_closeEdit->text().isEmpty()));

    tintToolButton(m_wrapBtn, hasContent);
    m_wrapBtn->setToolTip(hasContent
        ? tr("Advanced settings contain values — click to view.")
        : tr("Advanced: the matched text / regex and wrappers around the value."));
}

void PatternBlockCard::applyFrameStyle()
{
    // Self-delimiting is a property of the whole block: a distinctive
    // token kind, a constant literal, or a closing wrapper.
    PatternBlock probe;
    probe.matchKind = static_cast<PatternBlock::MatchKind>(m_kindCombo->currentData().toInt());
    probe.closingText = (probe.matchKind != PatternBlock::MatchKind::ConstantText)
        ? m_closeEdit->text() : QString();
    const bool selfDelimiting = LogPattern::blockIsSelfDelimiting(probe);

    // Self-delimiting blocks carry a border in their accent colour — a
    // visual hint that the token ends by itself and needs no separator.
    setAccentBorder(selfDelimiting);

    setToolTip(selfDelimiting
        ? tr("Self-delimiting block: the token shape ends the match by itself,\n"
             "no separator is needed after it (an explicit one is still allowed).")
        : QString());
}
