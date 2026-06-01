#include "conversionpatterndialog.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

constexpr int SchemaColName = 0;
constexpr int SchemaColSummary = 1;
constexpr int BlockColName = 0;
constexpr int BlockColRule = 1;
constexpr int BlockColMode  = 2;   // Boundary-type combo only
constexpr int BlockColValue = 3;   // Context-sensitive value inputs
constexpr int SchemaDataRole = Qt::UserRole;
constexpr int BoundaryModeSeparator = 0;
constexpr int BoundaryModeEnclosingPair = 1;
// Object names for widgets inside the Details (BlockColValue) cell widget
const char* kDetailsPatternObjectName = "detailsPattern"; // literal (ConstantText) or regex (CustomRegex)
const char* kDetailsSepObjectName     = "detailsSep";     // trailing separator
const char* kDetailsOpenObjectName    = "detailsOpen";    // enclosing-pair opening bracket
const char* kDetailsCloseObjectName   = "detailsClose";   // enclosing-pair closing bracket

static const char* kHelpHtml = R"HTML(
<html>
<head><style>
  body  { font-family: 'Segoe UI', Arial, sans-serif; font-size: 10pt; margin: 6px 8px; }
  h3    { margin: 10px 0 3px 0; color: #333; }
  table { border-collapse: collapse; }
  td, th { padding: 3px 8px 3px 2px; vertical-align: top; }
  th    { text-align: left; border-bottom: 1px solid #ccc; color: #555; }
  code  { font-family: Consolas, 'Courier New', monospace;
          background: #f0f0f0; padding: 0 3px; border-radius: 2px; font-size: 9.5pt; }
  .dim  { color: #888; font-size: 9pt; }
  ul    { margin: 3px 0; padding-left: 16px; }
  li    { margin-bottom: 3px; }
</style></head>
<body>

<h3>Concept</h3>
<p>A schema is an ordered list of blocks. Parsing walks the line from left to right. Each block may have either plain trailing text or an enclosing pair such as <code>[ ... ]</code> or <code>{ ... }</code>. If you need standalone fixed punctuation such as <code>&gt; </code>, use a <code>Constant text</code> block.</p>

<h3>Block rules</h3>
<table>
<tr><th>Rule</th><th>Meaning</th></tr>
<tr><td><code>Text until separator</code></td><td>Shortest text until the separator after the block.</td></tr>
<tr><td><code>Optional text until separator</code></td><td>Same, but the block may be empty.</td></tr>
<tr><td><code>Constant text</code></td><td>Exact literal text. Useful for prefixes, brackets and fixed separators that are part of the structure, not a field value.</td></tr>
<tr><td><code>Greedy text until separator</code></td><td>Longest text before the separator. Useful for messages that may contain the separator text.</td></tr>
<tr><td><code>Timestamp</code></td><td>Typed timestamp matcher for common log prefixes.</td></tr>
<tr><td><code>Level</code></td><td>TRACE / DEBUG / INFO / WARN / ERROR / FATAL.</td></tr>
<tr><td><code>Hex text</code></td><td>Hex tokens such as <code>0x00006d40</code> or <code>ABCDEF</code>.</td></tr>
<tr><td><code>Integer</code></td><td>One or more digits.</td></tr>
<tr><td><code>File path</code></td><td>Windows / UNC / Unix-like path text.</td></tr>
<tr><td><code>Optional file path</code></td><td>Same, but the path may be empty, e.g. <code>{:0}</code>.</td></tr>
<tr><td><code>Custom regex</code></td><td>Use your own regex for this block only.</td></tr>
<tr><td><code>Remainder of line</code></td><td>Capture everything to the end of the line. Must be the last block.</td></tr>
</table>

<h3>Design advice</h3>
<ul>
  <li>Prefer explicit separators between neighbouring blocks. It keeps parsing deterministic and fast.</li>
    <li>Use typed rules for stable pieces such as timestamp, level and line number.</li>
    <li>Use <code>Enclosing pair</code> for wrappers like <code>[thread]</code> or <code>{path:line}</code>; use <code>Separator</code> when only trailing text matters.</li>
    <li>Use <code>Constant text</code> instead of a special prefix field. It keeps the whole grammar in one ordered list.</li>
    <li>Use <code>Optional file path</code> or <code>Optional text until separator</code> when a block may legally be empty, such as <code>{:0}</code>.</li>
    <li>Use <code>Greedy text until separator</code> for free-form messages that may contain delimiter text.</li>
  <li>Use <code>Custom regex</code> only for blocks that truly need it; fixed rules are cheaper and easier to understand.</li>
  <li>When the last block is free-form, prefer <code>Remainder of line</code>.</li>
</ul>

</body>
</html>
)HTML";

QString kindDisplayName(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::ConstantText:
        return QObject::tr("Constant text");
    case PatternBlock::MatchKind::TextUntilSeparator:
        return QObject::tr("Text until separator");
    case PatternBlock::MatchKind::OptionalTextUntilSeparator:
        return QObject::tr("Optional text until separator");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:
        return QObject::tr("Greedy text until separator");
    case PatternBlock::MatchKind::Timestamp:
        return QObject::tr("Timestamp");
    case PatternBlock::MatchKind::Level:
        return QObject::tr("Level");
    case PatternBlock::MatchKind::HexText:
        return QObject::tr("Hex text");
    case PatternBlock::MatchKind::Integer:
        return QObject::tr("Integer");
    case PatternBlock::MatchKind::FilePath:
        return QObject::tr("File path");
    case PatternBlock::MatchKind::OptionalFilePath:
        return QObject::tr("Optional file path");
    case PatternBlock::MatchKind::CustomRegex:
        return QObject::tr("Custom regex");
    case PatternBlock::MatchKind::Remainder:
        return QObject::tr("Remainder of line");
    }
    return QObject::tr("Text until separator");
}

QComboBox* createKindCombo(QWidget* parent, PatternBlock::MatchKind currentKind)
{
    auto* combo = new QComboBox(parent);
    const QList<PatternBlock::MatchKind> kinds = {
        PatternBlock::MatchKind::ConstantText,
        PatternBlock::MatchKind::TextUntilSeparator,
        PatternBlock::MatchKind::OptionalTextUntilSeparator,
        PatternBlock::MatchKind::GreedyTextUntilSeparator,
        PatternBlock::MatchKind::Timestamp,
        PatternBlock::MatchKind::Level,
        PatternBlock::MatchKind::HexText,
        PatternBlock::MatchKind::Integer,
        PatternBlock::MatchKind::FilePath,
        PatternBlock::MatchKind::OptionalFilePath,
        PatternBlock::MatchKind::CustomRegex,
        PatternBlock::MatchKind::Remainder,
    };

    for (const PatternBlock::MatchKind kind : kinds)
        combo->addItem(kindDisplayName(kind), static_cast<int>(kind));

    const int index = combo->findData(static_cast<int>(currentKind));
    combo->setCurrentIndex(index >= 0 ? index : 0);
    return combo;
}

PatternBlock::MatchKind comboKind(const QComboBox* combo)
{
    return combo
        ? static_cast<PatternBlock::MatchKind>(combo->currentData().toInt())
        : PatternBlock::MatchKind::TextUntilSeparator;
}

PatternDefinition defaultDefinition()
{
    PatternDefinition def;
    PatternBlock message;
    message.name = QObject::tr("Message");
    message.matchKind = PatternBlock::MatchKind::Remainder;
    def.blocks.push_back(message);
    return def;
}

QString displayLabelForBlock(const PatternBlock& block)
{
    if (block.matchKind == PatternBlock::MatchKind::ConstantText)
        return QStringLiteral("\"") + block.customRegex + QStringLiteral("\"");
    if (!block.leadingText.isEmpty())
        return block.leadingText + block.name + block.separator;
    return block.name;
}

// Returns true for rule kinds that support a boundary (separator or enclosing pair).
bool kindHasBoundary(PatternBlock::MatchKind kind)
{
    return kind != PatternBlock::MatchKind::ConstantText
        && kind != PatternBlock::MatchKind::Remainder;
}

// Shows / hides the four QLineEdits inside a Details cell widget.
void refreshValueWidgetVisibility(QWidget* container,
                                   PatternBlock::MatchKind kind,
                                   bool enclosing)
{
    const bool hasPattern  = kind == PatternBlock::MatchKind::ConstantText
                          || kind == PatternBlock::MatchKind::CustomRegex;
    const bool hasBoundary = kindHasBoundary(kind);

    if (auto* w = container->findChild<QLineEdit*>(QLatin1String(kDetailsPatternObjectName))) {
        w->setVisible(hasPattern);
        w->setPlaceholderText(kind == PatternBlock::MatchKind::ConstantText
            ? QObject::tr("exact text to match")
            : QObject::tr("regex pattern"));
    }
    if (auto* w = container->findChild<QLineEdit*>(QLatin1String(kDetailsSepObjectName)))
        w->setVisible(hasBoundary && !enclosing);
    if (auto* w = container->findChild<QLineEdit*>(QLatin1String(kDetailsOpenObjectName)))
        w->setVisible(hasBoundary && enclosing);
    if (auto* w = container->findChild<QLineEdit*>(QLatin1String(kDetailsCloseObjectName)))
        w->setVisible(hasBoundary && enclosing);
}

QString definitionSummary(const PatternDefinition& definition)
{
    if (definition.blocks.isEmpty())
        return QObject::tr("No blocks");

    auto compactName = [](const QString& text) {
        constexpr int kMaxLen = 14;
        if (text.size() <= kMaxLen)
            return text;
        return text.left(kMaxLen - 3) + QStringLiteral("...");
    };

    QStringList parts;
    parts.reserve(definition.blocks.size() + (definition.linePrefix.isEmpty() ? 0 : 1));
    if (!definition.linePrefix.isEmpty())
        parts.append(QStringLiteral("\"") + compactName(definition.linePrefix) + QStringLiteral("\""));
    for (const PatternBlock& block : definition.blocks)
        parts.append(compactName(displayLabelForBlock(block)));
    return parts.join(QStringLiteral(" | "));
}

void applyCompactTableStyle(QTableWidget* table, qreal fontScale)
{
    if (!table)
        return;

    QFont compactFont = table->font();
    compactFont.setPointSizeF(qMax(8.0, compactFont.pointSizeF() * fontScale));
    table->setFont(compactFont);
    table->setTextElideMode(Qt::ElideMiddle);
    table->verticalHeader()->setDefaultSectionSize(QFontMetrics(compactFont).height() + 6);
}

} // namespace

ConversionPatternDialog::ConversionPatternDialog(const PatternList& patterns,
                                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Manage Field Schemas"));
    setMinimumSize(980, 560);
    resize(1160, 680);

    m_schemaTable = new QTableWidget(0, 2, this);
    m_schemaTable->setHorizontalHeaderLabels({tr("Schema"), tr("Summary")});
    m_schemaTable->horizontalHeader()->setStretchLastSection(true);
    m_schemaTable->horizontalHeader()->resizeSection(SchemaColName, 220);
    m_schemaTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_schemaTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_schemaTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_schemaTable->setAlternatingRowColors(true);
    m_schemaTable->verticalHeader()->setVisible(false);
    applyCompactTableStyle(m_schemaTable, 0.92);

    m_addSchemaBtn = new QPushButton(tr("+  Add"), this);
    m_removeSchemaBtn = new QPushButton(tr("−  Remove"), this);
    m_moveSchemaUpBtn = new QPushButton(tr("↑"), this);
    m_moveSchemaDownBtn = new QPushButton(tr("↓"), this);
    m_moveSchemaUpBtn->setFixedWidth(30);
    m_moveSchemaDownBtn->setFixedWidth(30);

    auto* schemaToolbar = new QHBoxLayout();
    schemaToolbar->setSpacing(4);
    schemaToolbar->addWidget(m_addSchemaBtn);
    schemaToolbar->addWidget(m_removeSchemaBtn);
    schemaToolbar->addStretch();
    schemaToolbar->addWidget(m_moveSchemaUpBtn);
    schemaToolbar->addWidget(m_moveSchemaDownBtn);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Display name for this schema"));

    auto* schemaInfoGroup = new QGroupBox(tr("Schema properties"), this);
    auto* schemaInfoForm = new QFormLayout(schemaInfoGroup);
    schemaInfoForm->addRow(tr("Name:"), m_nameEdit);

    m_blockTable = new QTableWidget(0, 4, this);
    m_blockTable->setHorizontalHeaderLabels({
        tr("Block name"), tr("Rule"), tr("Boundary"), tr("Details")
    });
    m_blockTable->horizontalHeader()->setStretchLastSection(true);
    m_blockTable->horizontalHeader()->resizeSection(BlockColName,  160);
    m_blockTable->horizontalHeader()->resizeSection(BlockColRule,  210);
    m_blockTable->horizontalHeader()->resizeSection(BlockColMode,  120);
    m_blockTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_blockTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_blockTable->setAlternatingRowColors(true);
    m_blockTable->verticalHeader()->setVisible(false);
    applyCompactTableStyle(m_blockTable, 0.92);
    if (QTableWidgetItem* item = m_blockTable->horizontalHeaderItem(BlockColMode)) {
        item->setToolTip(tr(
            "Boundary type between this block and the next block.\n"
            "- Separator: the block value is followed by literal trailing text.\n"
            "- Enclosing pair: the value is wrapped by two literals, e.g. '[' and ']'.\n"
            "Last block does not require a trailing separator.\n"
            "Disabled for Constant text and Remainder of line."));
    }
    if (QTableWidgetItem* item = m_blockTable->horizontalHeaderItem(BlockColValue)) {
        item->setToolTip(tr(
            "Context-sensitive details for this block:\n"
            "- Most rules (Separator mode): trailing text after the block value.\n"
            "- Most rules (Enclosing pair): opening and closing bracket texts.\n"
            "- Constant text: the exact literal to match.\n"
            "- Custom regex: the regex pattern (plus optional separator)."));
    }

    m_addBlockBtn = new QPushButton(tr("+  Add block"), this);
    m_removeBlockBtn = new QPushButton(tr("−  Remove block"), this);
    m_moveBlockUpBtn = new QPushButton(tr("↑"), this);
    m_moveBlockDownBtn = new QPushButton(tr("↓"), this);
    m_moveBlockUpBtn->setFixedWidth(30);
    m_moveBlockDownBtn->setFixedWidth(30);

    auto* blockToolbar = new QHBoxLayout();
    blockToolbar->setSpacing(4);
    blockToolbar->addWidget(m_addBlockBtn);
    blockToolbar->addWidget(m_removeBlockBtn);
    blockToolbar->addStretch();
    blockToolbar->addWidget(m_moveBlockUpBtn);
    blockToolbar->addWidget(m_moveBlockDownBtn);

    auto* blockGroup = new QGroupBox(tr("Blocks"), this);
    auto* blockLayout = new QVBoxLayout(blockGroup);
    blockLayout->setContentsMargins(8, 8, 8, 8);
    blockLayout->addWidget(m_blockTable, 1);
    blockLayout->addLayout(blockToolbar);

    auto* editorWidget = new QWidget(this);
    auto* editorLayout = new QVBoxLayout(editorWidget);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(6);
    editorLayout->addWidget(schemaInfoGroup);
    editorLayout->addWidget(blockGroup, 1);

    auto* leftWidget = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setSpacing(6);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_schemaTable, 1);
    leftLayout->addLayout(schemaToolbar);
    leftLayout->addWidget(editorWidget, 1);

    auto* helpBrowser = new QTextBrowser(this);
    helpBrowser->setOpenExternalLinks(false);
    helpBrowser->setHtml(QString::fromLatin1(kHelpHtml));
    helpBrowser->setMinimumWidth(280);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftWidget);
    splitter->addWidget(helpBrowser);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 2);

    m_useBtn = new QPushButton(tr("Use Selected"), this);
    auto* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setDefault(true);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(m_useBtn);
    btnRow->addWidget(closeBtn);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addLayout(btnRow);

    connect(m_addSchemaBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onAddSchema);
    connect(m_removeSchemaBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onRemoveSchema);
    connect(m_moveSchemaUpBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveSchemaUp);
    connect(m_moveSchemaDownBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveSchemaDown);
    connect(m_schemaTable, &QTableWidget::currentCellChanged,
            this, &ConversionPatternDialog::onSchemaSelectionChanged);
    connect(m_nameEdit, &QLineEdit::textEdited, this, &ConversionPatternDialog::onSchemaNameEdited);
    connect(m_addBlockBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onAddBlock);
    connect(m_removeBlockBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onRemoveBlock);
    connect(m_moveBlockUpBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveBlockUp);
    connect(m_moveBlockDownBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveBlockDown);
    connect(m_blockTable, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { updateButtonStates(); });
    connect(m_blockTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) {
        onBlockItemChanged();
    });
    connect(m_useBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onUseSelected);
    connect(closeBtn, &QPushButton::clicked, this, [this]() {
        // Force-commit any open inline cell editor, then flush the schema.
        m_syncing = true;
        m_blockTable->setCurrentIndex(QModelIndex());
        m_syncing = false;
        saveSchemaRow(m_schemaTable->currentRow());
        accept();
    });

    populateSchemas(patterns);
    if (m_schemaTable->rowCount() == 0)
        onAddSchema();
    updateButtonStates();
}

ConversionPatternDialog::PatternList ConversionPatternDialog::resultPatterns() const
{
    PatternList list;
    for (int row = 0; row < m_schemaTable->rowCount(); ++row) {
        const QString name = m_schemaTable->item(row, SchemaColName)
            ? m_schemaTable->item(row, SchemaColName)->text().trimmed()
            : QString();
        const QString schema = m_schemaTable->item(row, SchemaColName)
            ? m_schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString().trimmed()
            : QString();
        if (!schema.isEmpty())
            list.append({name, schema});
    }
    return list;
}

void ConversionPatternDialog::populateSchemas(const PatternList& list)
{
    m_schemaTable->setRowCount(0);
    for (const auto& entry : list) {
        const int row = m_schemaTable->rowCount();
        m_schemaTable->insertRow(row);
        setSchemaRow(row, entry.first, entry.second);
    }

    if (m_schemaTable->rowCount() > 0)
        m_schemaTable->setCurrentCell(0, 0);
}

void ConversionPatternDialog::setSchemaRow(int row,
                                           const QString& name,
                                           const QString& serializedSchema)
{
    if (row < 0 || row >= m_schemaTable->rowCount())
        return;

    if (!m_schemaTable->item(row, SchemaColName))
        m_schemaTable->setItem(row, SchemaColName, new QTableWidgetItem());
    if (!m_schemaTable->item(row, SchemaColSummary))
        m_schemaTable->setItem(row, SchemaColSummary, new QTableWidgetItem());

    m_schemaTable->item(row, SchemaColName)->setText(name.trimmed().isEmpty() ? tr("Untitled schema") : name);
    m_schemaTable->item(row, SchemaColName)->setData(SchemaDataRole, serializedSchema);
    updateSchemaSummary(row);
}

void ConversionPatternDialog::updateSchemaSummary(int row)
{
    if (row < 0 || row >= m_schemaTable->rowCount())
        return;

    const QString serializedSchema = m_schemaTable->item(row, SchemaColName)
        ? m_schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString()
        : QString();

    PatternDefinition definition;
    const bool ok = LogPattern::deserializeDefinition(serializedSchema, &definition, true);
    const QString summary = ok ? definitionSummary(definition) : tr("Invalid schema");
    m_schemaTable->item(row, SchemaColSummary)->setText(summary);
    if (ok) {
        QStringList tooltipLines;
        if (!definition.linePrefix.isEmpty())
            tooltipLines.append(tr("Constant text: %1").arg(definition.linePrefix));
        for (const PatternBlock& block : definition.blocks) {
            const QString label = block.matchKind == PatternBlock::MatchKind::ConstantText
                ? tr("Constant text")
                : block.name;
            QString line = tr("%1: %2").arg(label, kindDisplayName(block.matchKind));
            if (!block.leadingText.isEmpty())
                line += tr(" between '%1' and '%2'").arg(block.leadingText, block.separator);
            else if (!block.separator.isEmpty())
                line += tr(" then '%1'").arg(block.separator);
            if ((block.matchKind == PatternBlock::MatchKind::ConstantText
                    || block.matchKind == PatternBlock::MatchKind::CustomRegex)
                && !block.customRegex.isEmpty())
                line += tr(" = %1").arg(block.customRegex);
            tooltipLines.append(line);
        }
        m_schemaTable->item(row, SchemaColSummary)->setToolTip(tooltipLines.join(QLatin1Char('\n')));
    } else {
        m_schemaTable->item(row, SchemaColSummary)->setToolTip(summary);
    }
}

void ConversionPatternDialog::clearBlockTable()
{
    m_blockTable->setRowCount(0);
}

void ConversionPatternDialog::addBlockRow(const PatternBlock& block)
{
    const int row = m_blockTable->rowCount();
    m_blockTable->insertRow(row);

    // Col 0: Block name
    m_blockTable->setItem(row, BlockColName, new QTableWidgetItem(block.name));

    // Col 1: Rule combo
    QComboBox* ruleCombo = createKindCombo(m_blockTable, block.matchKind);
    ruleCombo->setFixedHeight(ruleCombo->fontMetrics().height() + 8);
    ruleCombo->setFocusPolicy(Qt::StrongFocus); // prevent accidental wheel change
    m_blockTable->setCellWidget(row, BlockColRule, ruleCombo);

    // Col 2: Boundary-type combo (Separator / Enclosing pair)
    {
        auto* modeCombo = new QComboBox(m_blockTable);
        modeCombo->addItem(tr("Separator"),     BoundaryModeSeparator);
        modeCombo->addItem(tr("Enclosing pair"), BoundaryModeEnclosingPair);
        modeCombo->setFocusPolicy(Qt::StrongFocus); // prevent accidental wheel change
        const bool enclosing = !block.leadingText.isEmpty();
        modeCombo->setCurrentIndex(enclosing ? 1 : 0);
        modeCombo->setEnabled(kindHasBoundary(block.matchKind));
        modeCombo->setFixedHeight(modeCombo->fontMetrics().height() + 8);
        m_blockTable->setCellWidget(row, BlockColMode, modeCombo);
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, row](int) {
                    updateRowValue(row);
                    onBlockItemChanged();
                });
    }

    // Col 3: Details widget (context-sensitive value inputs)
    {
        const bool enclosing = !block.leadingText.isEmpty();
        auto* container = new QWidget(m_blockTable);
        auto* layout    = new QHBoxLayout(container);
        layout->setContentsMargins(2, 0, 2, 0);
        layout->setSpacing(3);

        // Pattern/literal input – visible for ConstantText and CustomRegex
        auto* patternEdit = new QLineEdit(container);
        patternEdit->setObjectName(QLatin1String(kDetailsPatternObjectName));
        patternEdit->setText(block.customRegex);
        layout->addWidget(patternEdit, 1);

        // Separator text – visible for boundary-capable rules in Separator mode
        auto* sepEdit = new QLineEdit(container);
        sepEdit->setObjectName(QLatin1String(kDetailsSepObjectName));
        sepEdit->setText(enclosing ? QString() : block.separator);
        sepEdit->setPlaceholderText(tr("trailing text after block"));
        layout->addWidget(sepEdit, 1);

        // Opening bracket – visible in Enclosing pair mode
        auto* openEdit = new QLineEdit(container);
        openEdit->setObjectName(QLatin1String(kDetailsOpenObjectName));
        openEdit->setText(block.leadingText);
        openEdit->setPlaceholderText(tr("opening, e.g. ["));
        layout->addWidget(openEdit, 1);

        // Closing bracket – visible in Enclosing pair mode
        auto* closeEdit = new QLineEdit(container);
        closeEdit->setObjectName(QLatin1String(kDetailsCloseObjectName));
        closeEdit->setText(enclosing ? block.separator : QString());
        closeEdit->setPlaceholderText(tr("closing, e.g. ]"));
        layout->addWidget(closeEdit, 1);

        refreshValueWidgetVisibility(container, block.matchKind, enclosing);
        m_blockTable->setCellWidget(row, BlockColValue, container);

        for (auto* edit : container->findChildren<QLineEdit*>()) {
            connect(edit, &QLineEdit::textEdited,
                    this, [this](const QString&) { onBlockItemChanged(); });
        }
    }

    // Connect rule-combo change AFTER mode and value widgets are in the table
    connect(ruleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, row](int) {
                updateRowMode(row);
                onBlockItemChanged();
            });

    // Keep row-level UI constraints in sync with row positions.
    updateRowMode(row);
    if (row > 0)
        updateRowMode(row - 1);
}

PatternDefinition ConversionPatternDialog::currentDefinitionFromEditor() const
{
    PatternDefinition definition;
    definition.linePrefix.clear();

    for (int row = 0; row < m_blockTable->rowCount(); ++row) {
        PatternBlock block;

        block.name = m_blockTable->item(row, BlockColName)
            ? m_blockTable->item(row, BlockColName)->text().trimmed()
            : QString();

        auto* ruleCombo = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColRule));
        if (ruleCombo)
            block.matchKind = comboKind(ruleCombo);

        QWidget*   valueWidget = m_blockTable->cellWidget(row, BlockColValue);
        QComboBox* modeCombo   = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColMode));

        // Pattern / literal  (ConstantText and CustomRegex only)
        if (block.matchKind == PatternBlock::MatchKind::ConstantText
                || block.matchKind == PatternBlock::MatchKind::CustomRegex) {
            if (auto* e = valueWidget
                    ? valueWidget->findChild<QLineEdit*>(QLatin1String(kDetailsPatternObjectName))
                    : nullptr)
                block.customRegex = e->text();
        }

        // Separator / enclosing pair  (all boundary-capable rules)
        if (kindHasBoundary(block.matchKind) && modeCombo) {
            if (modeCombo->currentData().toInt() == BoundaryModeEnclosingPair) {
                if (auto* e = valueWidget
                        ? valueWidget->findChild<QLineEdit*>(QLatin1String(kDetailsOpenObjectName))
                        : nullptr)
                    block.leadingText = e->text();
                if (auto* e = valueWidget
                        ? valueWidget->findChild<QLineEdit*>(QLatin1String(kDetailsCloseObjectName))
                        : nullptr)
                    block.separator = e->text();
            } else {
                if (auto* e = valueWidget
                        ? valueWidget->findChild<QLineEdit*>(QLatin1String(kDetailsSepObjectName))
                        : nullptr)
                    block.separator = e->text();
            }
        }

        // ConstantText blocks have no meaningful name

        definition.blocks.push_back(block);
    }

    return definition;
}

void ConversionPatternDialog::loadSchemaRow(int row)
{
    m_syncing = true;
    if (row >= 0 && row < m_schemaTable->rowCount()) {
        const QString name = m_schemaTable->item(row, SchemaColName)
            ? m_schemaTable->item(row, SchemaColName)->text()
            : QString();
        const QString serializedSchema = m_schemaTable->item(row, SchemaColName)
            ? m_schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString()
            : QString();

        PatternDefinition definition;
        // Use strict=false so that in-progress schemas (e.g. Remainder block not
        // yet moved to the last position) are loaded as-is instead of being
        // discarded in favour of defaultDefinition().
        if (!LogPattern::deserializeDefinition(serializedSchema, &definition, true, false))
            definition = defaultDefinition();

        if (!definition.linePrefix.isEmpty()) {
            PatternBlock prefixBlock;
            prefixBlock.matchKind = PatternBlock::MatchKind::ConstantText;
            prefixBlock.customRegex = definition.linePrefix;
            definition.blocks.prepend(prefixBlock);
            definition.linePrefix.clear();
        }

        m_nameEdit->setText(name);
        clearBlockTable();
        for (const PatternBlock& block : definition.blocks)
            addBlockRow(block);
        if (m_blockTable->rowCount() > 0)
            m_blockTable->setCurrentCell(0, 0);
    } else {
        m_nameEdit->clear();
        clearBlockTable();
    }
    m_syncing = false;
    updateButtonStates();
}

void ConversionPatternDialog::saveSchemaRow(int row)
{
    if (m_syncing || row < 0 || row >= m_schemaTable->rowCount())
        return;

    const QString name = m_nameEdit->text().trimmed().isEmpty()
        ? tr("Untitled schema")
        : m_nameEdit->text().trimmed();
    const QString serializedSchema = LogPattern::serializeDefinition(currentDefinitionFromEditor());
    setSchemaRow(row, name, serializedSchema);
}

void ConversionPatternDialog::updateButtonStates()
{
    const int schemaRow = m_schemaTable->currentRow();
    const int blockRow = m_blockTable->currentRow();
    const bool hasSchema = schemaRow >= 0;
    const bool hasBlock = blockRow >= 0;

    m_removeSchemaBtn->setEnabled(hasSchema);
    m_moveSchemaUpBtn->setEnabled(schemaRow > 0);
    m_moveSchemaDownBtn->setEnabled(hasSchema && schemaRow < m_schemaTable->rowCount() - 1);
    m_useBtn->setEnabled(hasSchema);

    m_nameEdit->setEnabled(hasSchema);
    m_blockTable->setEnabled(hasSchema);
    m_addBlockBtn->setEnabled(hasSchema);
    m_removeBlockBtn->setEnabled(hasBlock);
    m_moveBlockUpBtn->setEnabled(blockRow > 0);
    m_moveBlockDownBtn->setEnabled(hasBlock && blockRow < m_blockTable->rowCount() - 1);
}

void ConversionPatternDialog::onAddSchema()
{
    saveSchemaRow(m_schemaTable->currentRow());

    const int row = m_schemaTable->rowCount();
    m_schemaTable->insertRow(row);
    setSchemaRow(row,
                 tr("New schema"),
                 LogPattern::serializeDefinition(defaultDefinition()));
    m_schemaTable->setCurrentCell(row, 0);
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void ConversionPatternDialog::onRemoveSchema()
{
    const int row = m_schemaTable->currentRow();
    if (row < 0)
        return;

    m_schemaTable->removeRow(row);
    if (m_schemaTable->rowCount() > 0)
        m_schemaTable->setCurrentCell(qMin(row, m_schemaTable->rowCount() - 1), 0);
    else
        loadSchemaRow(-1);
    updateButtonStates();
}

void ConversionPatternDialog::onMoveSchemaUp()
{
    const int row = m_schemaTable->currentRow();
    if (row <= 0)
        return;

    saveSchemaRow(row);
    for (int col = 0; col < m_schemaTable->columnCount(); ++col) {
        auto* a = m_schemaTable->takeItem(row - 1, col);
        auto* b = m_schemaTable->takeItem(row, col);
        m_schemaTable->setItem(row - 1, col, b);
        m_schemaTable->setItem(row, col, a);
    }
    m_schemaTable->setCurrentCell(row - 1, 0);
}

void ConversionPatternDialog::onMoveSchemaDown()
{
    const int row = m_schemaTable->currentRow();
    if (row < 0 || row >= m_schemaTable->rowCount() - 1)
        return;

    saveSchemaRow(row);
    for (int col = 0; col < m_schemaTable->columnCount(); ++col) {
        auto* a = m_schemaTable->takeItem(row, col);
        auto* b = m_schemaTable->takeItem(row + 1, col);
        m_schemaTable->setItem(row, col, b);
        m_schemaTable->setItem(row + 1, col, a);
    }
    m_schemaTable->setCurrentCell(row + 1, 0);
}

void ConversionPatternDialog::closeEvent(QCloseEvent* event)
{
    // Also save on X button / window manager close — same as the Close button.
    m_syncing = true;
    m_blockTable->setCurrentIndex(QModelIndex());
    m_syncing = false;
    saveSchemaRow(m_schemaTable->currentRow());
    QDialog::accept();   // set result = Accepted, emit finished(Accepted)
    event->accept();     // let Qt proceed with closing
}

void ConversionPatternDialog::onSchemaSelectionChanged(int currentRow, int, int previousRow, int)
{
    if (m_syncing)
        return;

    // Qt delivers currentCellChanged on the schema table BEFORE the block table
    // receives its focusOut event, so any open inline cell editor has not yet
    // committed its text into the QTableWidgetItem.  Force-close it now so that
    // saveSchemaRow() reads the up-to-date value.
    m_syncing = true;
    m_blockTable->setCurrentIndex(QModelIndex());
    m_syncing = false;

    saveSchemaRow(previousRow);
    loadSchemaRow(currentRow);
}

void ConversionPatternDialog::updateRowMode(int row)
{
    auto* ruleCombo  = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColRule));
    auto* modeCombo  = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColMode));
    auto* valueWidget = m_blockTable->cellWidget(row, BlockColValue);
    if (!ruleCombo) return;

    const PatternBlock::MatchKind kind = comboKind(ruleCombo);
    const bool hasBoundary = kindHasBoundary(kind);

    // Enable/disable mode combo
    if (modeCombo) modeCombo->setEnabled(hasBoundary);

    // Refresh value widget visibility
    if (valueWidget) {
        const bool enclosing = modeCombo && modeCombo->isEnabled()
                            && modeCombo->currentData().toInt() == BoundaryModeEnclosingPair;
        refreshValueWidgetVisibility(valueWidget, kind, enclosing);
    }
}

void ConversionPatternDialog::updateRowValue(int row)
{
    auto* ruleCombo  = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColRule));
    auto* modeCombo  = qobject_cast<QComboBox*>(m_blockTable->cellWidget(row, BlockColMode));
    auto* valueWidget = m_blockTable->cellWidget(row, BlockColValue);
    if (!ruleCombo || !modeCombo || !valueWidget) return;

    const PatternBlock::MatchKind kind = comboKind(ruleCombo);
    const bool enclosing = modeCombo->currentData().toInt() == BoundaryModeEnclosingPair;
    refreshValueWidgetVisibility(valueWidget, kind, enclosing);
}

void ConversionPatternDialog::onSchemaNameEdited(const QString&)
{
    saveSchemaRow(m_schemaTable->currentRow());
}

void ConversionPatternDialog::onAddBlock()
{
    if (m_schemaTable->currentRow() < 0)
        return;

    PatternDefinition definition = currentDefinitionFromEditor();
    PatternBlock block;
    block.name = tr("Field %1").arg(definition.blocks.size() + 1);
    block.matchKind = PatternBlock::MatchKind::TextUntilSeparator;
    block.separator = QString();
    definition.blocks.push_back(block);

    m_syncing = true;
    clearBlockTable();
    for (const PatternBlock& currentBlock : definition.blocks)
        addBlockRow(currentBlock);
    m_syncing = false;
    m_blockTable->setCurrentCell(m_blockTable->rowCount() - 1, 0);
    saveSchemaRow(m_schemaTable->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onRemoveBlock()
{
    const int row = m_blockTable->currentRow();
    if (row < 0)
        return;

    PatternDefinition definition = currentDefinitionFromEditor();
    if (row >= 0 && row < definition.blocks.size())
        definition.blocks.removeAt(row);

    m_syncing = true;
    clearBlockTable();
    for (const PatternBlock& currentBlock : definition.blocks)
        addBlockRow(currentBlock);
    m_syncing = false;

    if (m_blockTable->rowCount() > 0)
        m_blockTable->setCurrentCell(qMin(row, m_blockTable->rowCount() - 1), 0);
    saveSchemaRow(m_schemaTable->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onMoveBlockUp()
{
    const int row = m_blockTable->currentRow();
    if (row <= 0)
        return;

    PatternDefinition definition = currentDefinitionFromEditor();
    definition.blocks.swapItemsAt(row, row - 1);

    m_syncing = true;
    clearBlockTable();
    for (const PatternBlock& currentBlock : definition.blocks)
        addBlockRow(currentBlock);
    m_syncing = false;
    m_blockTable->setCurrentCell(row - 1, 0);
    saveSchemaRow(m_schemaTable->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onMoveBlockDown()
{
    const int row = m_blockTable->currentRow();
    if (row < 0 || row >= m_blockTable->rowCount() - 1)
        return;

    PatternDefinition definition = currentDefinitionFromEditor();
    definition.blocks.swapItemsAt(row, row + 1);

    m_syncing = true;
    clearBlockTable();
    for (const PatternBlock& currentBlock : definition.blocks)
        addBlockRow(currentBlock);
    m_syncing = false;
    m_blockTable->setCurrentCell(row + 1, 0);
    saveSchemaRow(m_schemaTable->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onBlockItemChanged()
{
    if (m_syncing)
        return;

    saveSchemaRow(m_schemaTable->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onUseSelected()
{
    const int row = m_schemaTable->currentRow();
    if (row < 0 || row >= m_schemaTable->rowCount())
        return;

    saveSchemaRow(row);
    const QString schema = m_schemaTable->item(row, SchemaColName)
        ? m_schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString().trimmed()
        : QString();

    LogPattern pattern(schema);
    if (!pattern.isValid()) {
        QMessageBox::warning(this,
                             tr("Invalid schema"),
                             tr("The selected schema is incomplete or inconsistent.\n"
                                "Check block names, separators and custom regex rules."));
        return;
    }

    m_chosenPattern = schema;
    m_chosenResultIndex = -1;
    int resultIndex = 0;
    for (int r = 0; r < m_schemaTable->rowCount(); ++r) {
        const QString currentSchema = m_schemaTable->item(r, SchemaColName)
            ? m_schemaTable->item(r, SchemaColName)->data(SchemaDataRole).toString().trimmed()
            : QString();
        if (!currentSchema.isEmpty()) {
            if (r == row) {
                m_chosenResultIndex = resultIndex;
                break;
            }
            ++resultIndex;
        }
    }
    accept();
}
