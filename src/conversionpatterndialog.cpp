#include "conversionpatterndialog.h"
#include "ui_conversionpatterndialog.h"

#include "appsettings.h"
#include "patternblockcard.h"
#include "patternheuristics.h"
#include "separatornode.h"

#include <QCloseEvent>
#include <QEventLoop>
#include <QFontDatabase>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QtConcurrent>

namespace {

constexpr int SchemaColName = 0;
constexpr int SchemaColSummary = 1;
constexpr int SchemaDataRole = Qt::UserRole;

constexpr int kMaxPreviewLines = 50;
constexpr int kMaxPreviewLineLength = 4000;
constexpr int kPreviewDebounceMs = 250;
constexpr int kPreviewWatchdogMs = 2500;
constexpr int kApplyProbeTimeoutMs = 1500;

const char* kPresetsGroup = "PatternPresets";
const char* kEditorGroup  = "PatternEditor";

// Accent colours for cards and preview highlights (cycled).
const QColor kAccentColors[] = {
    QColor(0x29, 0x80, 0xB9), // blue
    QColor(0x27, 0xAE, 0x60), // green
    QColor(0xE6, 0x7E, 0x22), // orange
    QColor(0x8E, 0x44, 0xAD), // purple
    QColor(0x16, 0xA0, 0x85), // teal
    QColor(0xC0, 0x39, 0x2B), // red
    QColor(0xD4, 0xAC, 0x0D), // yellow
    QColor(0x2C, 0x3E, 0x50), // dark slate
    QColor(0xD3, 0x54, 0x00), // pumpkin
    QColor(0x7F, 0x8C, 0x8D), // gray
};

QColor accentColorForIndex(int index)
{
    constexpr int count = int(sizeof(kAccentColors) / sizeof(kAccentColors[0]));
    return kAccentColors[((index % count) + count) % count];
}

QColor highlightColorForIndex(int index)
{
    QColor color = accentColorForIndex(index);
    color.setAlpha(70);
    return color;
}

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
  ul    { margin: 3px 0; padding-left: 16px; }
  li    { margin-bottom: 3px; }
</style></head>
<body>

<h3>Concept</h3>
<p>A schema is a <b>chain</b>: data blocks linked by separators —
<code>[Block] ─ (separator) ─ [Block]</code>. The link to the next block sits at the
right end of each block row. By default it is <i>auto</i> (the <code>· · ·</code> icon):
any spaces or tabs are accepted there and you do not have to think about them. Click the
icon to set an explicit separator — a literal like <code> - </code> or, with the
<code>.*</code> toggle, a regular expression.</p>
<p>Blocks with a coloured border (<code>Timestamp</code>, <code>Level</code>,
<code>Integer</code>, <code>Hex</code>, <code>IP</code>) are <b>self-delimiting</b>: the
token shape ends the match by itself, so they need no separator at all — an explicit one
is still allowed when you want extra strictness.</p>
<p>When a value is wrapped in brackets — e.g. <code>[0x000034cc]</code> — that is a
property of the block itself: open its <code>⚙</code> advanced row and fill
<i>Enclosed by</i> <code>[</code> <i>and</i> <code>]</code>.</p>
<p>Paste real log lines into the live preview — every block match is highlighted with
the colour of its card while you edit.</p>

<h3>Forgiving by design</h3>
<ul>
  <li><b>Best-effort parsing.</b> If a line stops following the schema mid-way, the fields
      matched so far are kept and the unparsed tail goes into the last free-text field
      (usually <code>Message</code>). The tail is shown in red in the preview.</li>
  <li><b>Anchors.</b> <code>Timestamp</code>, <code>Level</code> and <code>IP address</code>
      are found even when unexpected text precedes them, and they need no separator
      after them — these tokens are distinctive on their own. Two blocks may stand
      back-to-back when one of them is an anchor.</li>
  <li><b>Whitespace collapses.</b> Extra spaces and tabs between blocks are ignored
      automatically; you do not need separators for padding.</li>
</ul>

<h3>Block types</h3>
<table>
<tr><th>Type</th><th>Meaning</th></tr>
<tr><td><code>Text</code></td><td>Shortest text until the next separator / block. May be empty — a missing field does not break the line.</td></tr>
<tr><td><code>Greedy text</code></td><td>Longest text before the next separator; use when the value may contain the separator text.</td></tr>
<tr><td><code>Timestamp</code></td><td>Date/time token (ISO, EU, US, time-only). Anchor.</td></tr>
<tr><td><code>Level</code></td><td>TRACE / DEBUG / INFO / WARN / ERROR / FATAL… Anchor.</td></tr>
<tr><td><code>Integer</code>, <code>Hex</code></td><td>Numeric tokens; suffix optional.</td></tr>
<tr><td><code>IP address</code></td><td>IPv4 with optional :port. Anchor.</td></tr>
<tr><td><code>File path</code></td><td>Windows / UNC / Unix-like path.</td></tr>
<tr><td><code>Constant text</code></td><td>Exact literal at this position. Name it to expose it as a column; leave the name empty to keep it pure structure.</td></tr>
<tr><td><code>Custom regex</code></td><td>Your own regular expression for this block only.</td></tr>
<tr><td><code>Remainder of line</code></td><td>Everything to the end. Must be last.</td></tr>
</table>

<h3>Blind spots</h3>
<p>Tick <code>hide</code> on a block to match and consume its text without creating a
field column — useful for session IDs and other noise you never analyse.</p>

<h3>Power tools</h3>
<ul>
  <li><b>Suggest schema</b> builds a draft schema from the first preview line.</li>
  <li><b>Presets</b> replace the blocks with a ready-made layout; your own schemas can be
      saved as presets too (stored in <code>LogViewer.ini</code>).</li>
  <li><b>Grok…</b> imports Logstash patterns such as
      <code>%{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} %{GREEDYDATA:msg}</code>.</li>
</ul>

</body>
</html>
)HTML";

QString kindDisplayName(PatternBlock::MatchKind kind)
{
    switch (kind) {
    case PatternBlock::MatchKind::ConstantText:              return QObject::tr("Constant text");
    case PatternBlock::MatchKind::TextUntilSeparator:         return QObject::tr("Text");
    case PatternBlock::MatchKind::GreedyTextUntilSeparator:   return QObject::tr("Greedy text");
    case PatternBlock::MatchKind::Timestamp:                  return QObject::tr("Timestamp");
    case PatternBlock::MatchKind::Level:                      return QObject::tr("Level");
    case PatternBlock::MatchKind::HexText:                    return QObject::tr("Hex");
    case PatternBlock::MatchKind::Integer:                    return QObject::tr("Integer");
    case PatternBlock::MatchKind::IpAddress:                  return QObject::tr("IP address");
    case PatternBlock::MatchKind::FilePath:                   return QObject::tr("File path");
    case PatternBlock::MatchKind::CustomRegex:                return QObject::tr("Custom regex");
    case PatternBlock::MatchKind::Remainder:                  return QObject::tr("Remainder of line");
    }
    return QObject::tr("Text");
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
    if (block.matchKind == PatternBlock::MatchKind::ConstantText) {
        return block.name.trimmed().isEmpty()
            ? QStringLiteral("\"") + block.customRegex + QStringLiteral("\"")
            : block.name;
    }
    QString label = block.leadingText + block.name + block.closingText;
    if (block.ignored)
        label += QStringLiteral(" (hidden)");
    return label;
}

QString definitionSummary(const PatternDefinition& definition)
{
    if (definition.blocks.isEmpty())
        return QObject::tr("No blocks");

    auto compactName = [](const QString& text) {
        constexpr int kMaxLen = 16;
        if (text.size() <= kMaxLen)
            return text;
        return text.left(kMaxLen - 3) + QStringLiteral("...");
    };

    QStringList parts;
    parts.reserve(definition.blocks.size() * 2 + (definition.linePrefix.isEmpty() ? 0 : 1));
    if (!definition.linePrefix.isEmpty())
        parts.append(QStringLiteral("\"") + compactName(definition.linePrefix) + QStringLiteral("\""));
    for (const PatternBlock& block : definition.blocks) {
        parts.append(compactName(displayLabelForBlock(block)));
        if (!block.separator.isEmpty())
            parts.append(QStringLiteral("'") + compactName(block.separator) + QStringLiteral("'"));
    }
    return parts.join(QStringLiteral(" "));
}

// Runs the compiled pattern against the sample lines in a worker thread and
// waits at most timeoutMs. Returns false when matching did not finish in
// time — i.e. the schema is pathologically slow (ReDoS-like) and must not
// be applied.
bool probeMatchesWithinTimeout(const LogPattern& pattern,
                               QStringList lines,
                               int timeoutMs)
{
    if (lines.isEmpty()) {
        // Synthetic worst-case-ish sample so even an empty preview gives
        // the regex something to chew on.
        lines << QString(1024, QLatin1Char('a')) + QStringLiteral(" 12:34:56 [x] msg");
    }

    QFutureWatcher<void> watcher;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    watcher.setFuture(QtConcurrent::run([pattern, lines]() {
        for (const QString& line : lines)
            pattern.matchLine(line);
    }));
    timer.start(timeoutMs);
    loop.exec();
    return watcher.isFinished();
}

} // namespace

ConversionPatternDialog::ConversionPatternDialog(const PatternList& patterns,
                                                 QWidget* parent,
                                                 const QStringList& sampleLines)
    : QDialog(parent)
    , ui(new Ui::ConversionPatternDialog)
{
    ui->setupUi(this);

    // Schema table
    ui->schemaTable->setHorizontalHeaderLabels({tr("Schema"), tr("Summary")});
    ui->schemaTable->horizontalHeader()->resizeSection(0, 180);
    ui->schemaTable->verticalHeader()->setVisible(false);
    ui->schemaTable->verticalHeader()->setDefaultSectionSize(
        ui->schemaTable->fontMetrics().height() + 8);

    // Block cards scroll area
    auto* cardsHost = new QWidget(ui->blocksScroll);
    m_cardsLayout = new QVBoxLayout(cardsHost);
    m_cardsLayout->setContentsMargins(2, 2, 2, 2);
    m_cardsLayout->setSpacing(4);
    m_cardsLayout->addStretch(1);
    ui->blocksScroll->setWidget(cardsHost);

    // Live preview
    ui->sampleEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ui->sampleEdit->setTabChangesFocus(true);

    // Help
    ui->helpBrowser->setHtml(QString::fromLatin1(kHelpHtml));

    // Splitter proportions
    ui->mainSplitter->setStretchFactor(0, 2);
    ui->mainSplitter->setStretchFactor(1, 5);
    ui->vSplitter->setStretchFactor(0, 4);
    ui->vSplitter->setStretchFactor(1, 1);

    // Preview machinery: debounce timer, worker watcher, watchdog.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(kPreviewDebounceMs);
    connect(m_previewTimer, &QTimer::timeout, this, &ConversionPatternDialog::startPreviewJob);

    m_previewWatcher = new QFutureWatcher<PreviewJob>(this);
    connect(m_previewWatcher, &QFutureWatcherBase::finished,
            this, &ConversionPatternDialog::onPreviewReady);

    m_previewWatchdog = new QTimer(this);
    m_previewWatchdog->setSingleShot(true);
    m_previewWatchdog->setInterval(kPreviewWatchdogMs);
    connect(m_previewWatchdog, &QTimer::timeout, this, [this]() {
        m_previewSlow = true;
        ui->previewStatusLabel->setStyleSheet(QStringLiteral("color:#C0392B;"));
        ui->previewStatusLabel->setText(
            tr("Preview is taking too long — the schema looks pathologically slow. Simplify custom regexes."));
    });

    // Signals
    connect(ui->addSchemaBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onAddSchema);
    connect(ui->removeSchemaBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onRemoveSchema);
    connect(ui->moveSchemaUpBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveSchemaUp);
    connect(ui->moveSchemaDownBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onMoveSchemaDown);
    connect(ui->schemaTable, &QTableWidget::currentCellChanged,
            this, &ConversionPatternDialog::onSchemaSelectionChanged);
    connect(ui->nameEdit, &QLineEdit::textEdited, this, &ConversionPatternDialog::onSchemaNameEdited);
    connect(ui->applyBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onApplyChanges);
    connect(ui->revertBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onRevertChanges);
    connect(ui->addBlockBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onAddBlock);
    connect(ui->autoDetectBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onAutoDetect);
    connect(ui->grokBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onGrokImport);
    connect(ui->savePresetBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onSavePreset);
    connect(ui->sampleEdit, &QPlainTextEdit::textChanged,
            this, &ConversionPatternDialog::schedulePreviewUpdate);
    connect(ui->useBtn, &QPushButton::clicked, this, &ConversionPatternDialog::onUseSelected);
    connect(ui->closeBtn, &QPushButton::clicked, this, [this]() {
        resolveDirtyState(ui->schemaTable->currentRow());
        accept();
    });

    rebuildPresetMenu();

    // Restore the last sample text; fall back to lines from the current log.
    {
        QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
        s.beginGroup(QLatin1String(kEditorGroup));
        const QString savedSample = s.value(QStringLiteral("sampleText")).toString();
        s.endGroup();
        if (!savedSample.isEmpty())
            ui->sampleEdit->setPlainText(savedSample);
        else if (!sampleLines.isEmpty())
            ui->sampleEdit->setPlainText(sampleLines.mid(0, 8).join(QLatin1Char('\n')));
    }

    populateSchemas(patterns);
    if (ui->schemaTable->rowCount() == 0)
        onAddSchema();
    updateButtonStates();
    schedulePreviewUpdate();
}

ConversionPatternDialog::~ConversionPatternDialog()
{
    // Persist the sample text so the next session starts with it.
    // The destructor covers every close path (Close, X, Use Selected, Esc).
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(QLatin1String(kEditorGroup));
    QString sample = ui->sampleEdit->toPlainText();
    if (sample.size() > 20000)
        sample.truncate(20000);
    s.setValue(QStringLiteral("sampleText"), sample);
    s.endGroup();

    delete ui;
}

ConversionPatternDialog::PatternList ConversionPatternDialog::resultPatterns() const
{
    PatternList list;
    for (int row = 0; row < ui->schemaTable->rowCount(); ++row) {
        const QTableWidgetItem* item = ui->schemaTable->item(row, SchemaColName);
        if (!item)
            continue;
        const QString name = item->text().trimmed();
        const QString schema = item->data(SchemaDataRole).toString().trimmed();
        if (!schema.isEmpty())
            list.append({name, schema});
    }
    return list;
}

// ---------------------------------------------------------------- //
// Schema list
// ---------------------------------------------------------------- //

void ConversionPatternDialog::populateSchemas(const PatternList& list)
{
    ui->schemaTable->setRowCount(0);
    for (const auto& entry : list) {
        const int row = ui->schemaTable->rowCount();
        ui->schemaTable->insertRow(row);
        setSchemaRow(row, entry.first, entry.second);
    }

    if (ui->schemaTable->rowCount() > 0)
        ui->schemaTable->setCurrentCell(0, 0);
}

void ConversionPatternDialog::setSchemaRow(int row,
                                           const QString& name,
                                           const QString& serializedSchema)
{
    if (row < 0 || row >= ui->schemaTable->rowCount())
        return;

    if (!ui->schemaTable->item(row, SchemaColName))
        ui->schemaTable->setItem(row, SchemaColName, new QTableWidgetItem());
    if (!ui->schemaTable->item(row, SchemaColSummary))
        ui->schemaTable->setItem(row, SchemaColSummary, new QTableWidgetItem());

    ui->schemaTable->item(row, SchemaColName)->setText(
        name.trimmed().isEmpty() ? tr("Untitled schema") : name);
    ui->schemaTable->item(row, SchemaColName)->setData(SchemaDataRole, serializedSchema);
    updateSchemaSummary(row);
}

void ConversionPatternDialog::updateSchemaSummary(int row)
{
    if (row < 0 || row >= ui->schemaTable->rowCount())
        return;

    const QString serializedSchema = ui->schemaTable->item(row, SchemaColName)
        ? ui->schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString()
        : QString();

    PatternDefinition definition;
    const bool ok = LogPattern::deserializeDefinition(serializedSchema, &definition, true, false);
    const QString summary = ok ? definitionSummary(definition) : tr("Invalid schema");
    ui->schemaTable->item(row, SchemaColSummary)->setText(summary);

    if (ok) {
        QStringList tooltipLines;
        if (!definition.linePrefix.isEmpty())
            tooltipLines.append(tr("Constant text: %1").arg(definition.linePrefix));
        for (const PatternBlock& block : definition.blocks) {
            const QString label = block.matchKind == PatternBlock::MatchKind::ConstantText
                ? tr("Constant text")
                : block.name;
            QString line = tr("%1: %2").arg(label, kindDisplayName(block.matchKind));
            if (!block.leadingText.isEmpty() || !block.closingText.isEmpty())
                line += tr(" enclosed by '%1' and '%2'").arg(block.leadingText, block.closingText);
            if (!block.separator.isEmpty())
                line += block.separatorIsRegex
                    ? tr(" then regex '%1'").arg(block.separator)
                    : tr(" then '%1'").arg(block.separator);
            if ((block.matchKind == PatternBlock::MatchKind::ConstantText
                    || block.matchKind == PatternBlock::MatchKind::CustomRegex)
                && !block.customRegex.isEmpty())
                line += tr(" = %1").arg(block.customRegex);
            if (block.ignored)
                line += tr(" [hidden]");
            tooltipLines.append(line);
        }
        ui->schemaTable->item(row, SchemaColSummary)->setToolTip(tooltipLines.join(QLatin1Char('\n')));
    } else {
        ui->schemaTable->item(row, SchemaColSummary)->setToolTip(summary);
    }
}

void ConversionPatternDialog::loadSchemaRow(int row)
{
    m_syncing = true;
    if (row >= 0 && row < ui->schemaTable->rowCount()) {
        const QString name = ui->schemaTable->item(row, SchemaColName)
            ? ui->schemaTable->item(row, SchemaColName)->text()
            : QString();
        const QString serializedSchema = ui->schemaTable->item(row, SchemaColName)
            ? ui->schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString()
            : QString();

        PatternDefinition definition;
        // strict=false: in-progress schemas load as-is instead of being
        // replaced by the default definition.
        if (!LogPattern::deserializeDefinition(serializedSchema, &definition, true, false))
            definition = defaultDefinition();

        if (!definition.linePrefix.isEmpty()) {
            PatternBlock prefixBlock;
            prefixBlock.matchKind = PatternBlock::MatchKind::ConstantText;
            prefixBlock.customRegex = definition.linePrefix;
            definition.blocks.prepend(prefixBlock);
            definition.linePrefix.clear();
        }

        // The chain has no connector after the last card, so a legacy
        // trailing separator would be lost on save. Materialize it as a
        // trailing block instead (constant literal / hidden regex).
        if (!definition.blocks.isEmpty()
                && !definition.blocks.last().separator.isEmpty()) {
            PatternBlock& last = definition.blocks.last();
            PatternBlock tailBlock;
            if (last.separatorIsRegex) {
                tailBlock.matchKind = PatternBlock::MatchKind::CustomRegex;
                tailBlock.customRegex = last.separator;
                tailBlock.ignored = true;
            } else {
                tailBlock.matchKind = PatternBlock::MatchKind::ConstantText;
                tailBlock.customRegex = last.separator;
            }
            last.separator.clear();
            last.separatorIsRegex = false;
            definition.blocks.push_back(tailBlock);
        }

        ui->nameEdit->setText(name);
        rebuildCards(definition);
    } else {
        ui->nameEdit->clear();
        rebuildCards(PatternDefinition());
    }
    m_syncing = false;
    m_dirty = false;
    updateButtonStates();
    updateValidationLabel();
    schedulePreviewUpdate();
}

void ConversionPatternDialog::saveSchemaRow(int row)
{
    if (m_syncing || row < 0 || row >= ui->schemaTable->rowCount())
        return;

    const QString name = ui->nameEdit->text().trimmed().isEmpty()
        ? tr("Untitled schema")
        : ui->nameEdit->text().trimmed();
    const QString serializedSchema = LogPattern::serializeDefinition(currentDefinitionFromEditor());
    setSchemaRow(row, name, serializedSchema);
}

// ---------------------------------------------------------------- //
// Block cards
// ---------------------------------------------------------------- //

void ConversionPatternDialog::rebuildCards(const PatternDefinition& definition)
{
    const bool wasSyncing = m_syncing;
    m_syncing = true;

    for (PatternBlockCard* card : m_cards)
        card->deleteLater();
    m_cards.clear();
    for (SeparatorNode* node : m_separators)
        node->deleteLater();
    m_separators.clear();

    for (int blockIndex = 0; blockIndex < definition.blocks.size(); ++blockIndex) {
        const PatternBlock& block = definition.blocks[blockIndex];

        auto* card = new PatternBlockCard(this);
        card->setBlock(block);

        // Connector ("glue") to the next block, embedded at the right end
        // of this card's row. The last block has nothing to glue to.
        if (blockIndex < definition.blocks.size() - 1) {
            auto* node = new SeparatorNode(card);
            node->setSeparator(block.separator, block.separatorIsRegex);
            connect(node, &SeparatorNode::edited,
                    this, &ConversionPatternDialog::onBlocksEdited);
            card->attachGlueWidget(node);
            m_separators.push_back(node);
        }

        connect(card, &PatternBlockCard::blockEdited,
                this, &ConversionPatternDialog::onBlocksEdited);
        connect(card, &PatternBlockCard::moveUpRequested, this, [this, card]() {
            const int index = m_cards.indexOf(card);
            PatternDefinition def = currentDefinitionFromEditor();
            if (index > 0 && index < def.blocks.size()) {
                def.blocks.swapItemsAt(index, index - 1);
                replaceCurrentBlocks(def);
            }
        });
        connect(card, &PatternBlockCard::moveDownRequested, this, [this, card]() {
            const int index = m_cards.indexOf(card);
            PatternDefinition def = currentDefinitionFromEditor();
            if (index >= 0 && index < def.blocks.size() - 1) {
                def.blocks.swapItemsAt(index, index + 1);
                replaceCurrentBlocks(def);
            }
        });
        connect(card, &PatternBlockCard::removeRequested, this, [this, card]() {
            const int index = m_cards.indexOf(card);
            PatternDefinition def = currentDefinitionFromEditor();
            if (index >= 0 && index < def.blocks.size()) {
                def.blocks.removeAt(index);
                replaceCurrentBlocks(def);
            }
        });

        m_cardsLayout->insertWidget(m_cardsLayout->count() - 1, card);
        m_cards.push_back(card);
    }

    refreshCardChrome();
    m_syncing = wasSyncing;
}

void ConversionPatternDialog::refreshCardChrome()
{
    for (int i = 0; i < m_cards.size(); ++i) {
        m_cards[i]->setAccentColor(accentColorForIndex(i));
        m_cards[i]->setPosition(i, m_cards.size());
    }
}

PatternDefinition ConversionPatternDialog::currentDefinitionFromEditor() const
{
    PatternDefinition definition;
    definition.blocks.reserve(m_cards.size());
    for (int i = 0; i < m_cards.size(); ++i) {
        PatternBlock block = m_cards[i]->block();
        // Glue to the next block comes from the SeparatorNode after this card.
        if (i < m_separators.size()) {
            block.separator = m_separators[i]->separatorText();
            block.separatorIsRegex = m_separators[i]->isRegex();
        }
        definition.blocks.push_back(block);
    }
    return definition;
}

void ConversionPatternDialog::replaceCurrentBlocks(const PatternDefinition& definition)
{
    rebuildCards(definition);
    setDirty();
    updateValidationLabel();
    schedulePreviewUpdate();
}

void ConversionPatternDialog::setDirty()
{
    m_dirty = true;
    updateButtonStates();
    updateValidationLabel();
}

void ConversionPatternDialog::resolveDirtyState(int row)
{
    if (!m_dirty)
        return;
    m_dirty = false;

    if (row < 0 || row >= ui->schemaTable->rowCount())
        return;

    const QString name = ui->schemaTable->item(row, SchemaColName)
        ? ui->schemaTable->item(row, SchemaColName)->text()
        : tr("schema");
    const auto answer = QMessageBox::question(
        this, tr("Unapplied changes"),
        tr("Schema '%1' has changes that are not applied.\nApply them now?").arg(name),
        QMessageBox::Apply | QMessageBox::Discard, QMessageBox::Apply);
    if (answer == QMessageBox::Apply)
        saveSchemaRow(row);
    updateButtonStates();
}

void ConversionPatternDialog::onApplyChanges()
{
    const int row = ui->schemaTable->currentRow();
    if (row < 0)
        return;
    saveSchemaRow(row);
    m_dirty = false;
    updateButtonStates();
    updateValidationLabel();
}

void ConversionPatternDialog::onRevertChanges()
{
    const int row = ui->schemaTable->currentRow();
    if (row < 0)
        return;
    loadSchemaRow(row); // clears the dirty flag
}

// ---------------------------------------------------------------- //
// Presets
// ---------------------------------------------------------------- //

ConversionPatternDialog::PatternList ConversionPatternDialog::userPresets() const
{
    PatternList presets;
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(QLatin1String(kPresetsGroup));
    const QStringList names = s.value(QStringLiteral("names")).toStringList();
    const QStringList values = s.value(QStringLiteral("values")).toStringList();
    s.endGroup();
    const int count = qMin(names.size(), values.size());
    presets.reserve(count);
    for (int i = 0; i < count; ++i)
        presets.append({names[i], values[i]});
    return presets;
}

void ConversionPatternDialog::saveUserPresets(const PatternList& presets)
{
    QStringList names, values;
    for (const auto& preset : presets) {
        names << preset.first;
        values << preset.second;
    }
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(QLatin1String(kPresetsGroup));
    s.setValue(QStringLiteral("names"), names);
    s.setValue(QStringLiteral("values"), values);
    s.endGroup();
}

void ConversionPatternDialog::rebuildPresetMenu()
{
    QMenu* menu = ui->presetBtn->menu();
    if (!menu) {
        menu = new QMenu(ui->presetBtn);
        ui->presetBtn->setMenu(menu);
    }
    menu->clear();

    for (const PatternHeuristics::Preset& preset : PatternHeuristics::builtInPresets()) {
        const PatternDefinition definition = preset.definition;
        menu->addAction(preset.name, this, [this, definition]() {
            applyPreset(definition);
        });
    }

    const PatternList user = userPresets();
    if (!user.isEmpty()) {
        menu->addSeparator();
        for (const auto& preset : user) {
            const QString serialized = preset.second;
            menu->addAction(preset.first, this, [this, serialized]() {
                PatternDefinition definition;
                if (LogPattern::deserializeDefinition(serialized, &definition, true, false))
                    applyPreset(definition);
            });
        }

        menu->addSeparator();
        QMenu* deleteMenu = menu->addMenu(tr("Delete preset"));
        for (const auto& preset : user) {
            const QString name = preset.first;
            deleteMenu->addAction(name, this, [this, name]() {
                PatternList presets = userPresets();
                for (int i = presets.size() - 1; i >= 0; --i)
                    if (presets[i].first == name)
                        presets.removeAt(i);
                saveUserPresets(presets);
                rebuildPresetMenu();
            });
        }
    }
}

void ConversionPatternDialog::applyPreset(const PatternDefinition& definition)
{
    if (ui->schemaTable->currentRow() < 0)
        return;
    replaceCurrentBlocks(definition);
}

void ConversionPatternDialog::onSavePreset()
{
    if (ui->schemaTable->currentRow() < 0)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Save preset"), tr("Preset name:"),
        QLineEdit::Normal, ui->nameEdit->text().trimmed(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    PatternList presets = userPresets();
    const QString serialized = LogPattern::serializeDefinition(currentDefinitionFromEditor());
    bool replaced = false;
    for (auto& preset : presets) {
        if (preset.first == name) {
            preset.second = serialized;
            replaced = true;
        }
    }
    if (!replaced)
        presets.append({name, serialized});
    saveUserPresets(presets);
    rebuildPresetMenu();
}

// ---------------------------------------------------------------- //
// Magic: auto-detect & Grok
// ---------------------------------------------------------------- //

void ConversionPatternDialog::onAutoDetect()
{
    if (ui->schemaTable->currentRow() < 0)
        return;

    QString sample;
    for (const QString& line : previewLines()) {
        if (!line.trimmed().isEmpty()) {
            sample = line;
            break;
        }
    }
    if (sample.isEmpty()) {
        QMessageBox::information(this, tr("Suggest schema"),
            tr("Paste a real log line into the live preview field first."));
        return;
    }

    replaceCurrentBlocks(PatternHeuristics::suggestSchema(sample));
}

void ConversionPatternDialog::onGrokImport()
{
    if (ui->schemaTable->currentRow() < 0)
        return;

    bool ok = false;
    const QString grok = QInputDialog::getText(
        this, tr("Import Grok expression"),
        tr("Grok pattern (e.g. %{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} %{GREEDYDATA:msg}):"),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || grok.isEmpty())
        return;

    PatternDefinition definition;
    QString warnings;
    if (!PatternHeuristics::definitionFromGrok(grok, &definition, &warnings)) {
        QMessageBox::warning(this, tr("Import Grok expression"),
            tr("No %{PATTERN:field} tokens found in the expression."));
        return;
    }

    replaceCurrentBlocks(definition);
    if (!warnings.isEmpty())
        QMessageBox::information(this, tr("Import Grok expression"), warnings);
}

// ---------------------------------------------------------------- //
// Live preview
// ---------------------------------------------------------------- //

QStringList ConversionPatternDialog::previewLines() const
{
    QStringList lines = ui->sampleEdit->toPlainText().split(QLatin1Char('\n'));
    if (lines.size() > kMaxPreviewLines)
        lines = lines.mid(0, kMaxPreviewLines);
    for (QString& line : lines) {
        if (line.size() > kMaxPreviewLineLength)
            line.truncate(kMaxPreviewLineLength);
    }
    return lines;
}

void ConversionPatternDialog::schedulePreviewUpdate()
{
    m_previewTimer->start();
}

void ConversionPatternDialog::startPreviewJob()
{
    ++m_previewGeneration;
    const int generation = m_previewGeneration;

    // Preview follows the live editor state (including unapplied edits),
    // not the last-applied schema row.
    const int row = ui->schemaTable->currentRow();
    const QString serialized = (row >= 0)
        ? LogPattern::serializeDefinition(currentDefinitionFromEditor())
        : QString();

    const LogPattern pattern(serialized);
    const QStringList lines = previewLines();

    if (!pattern.isValid() || lines.isEmpty()
            || ui->sampleEdit->toPlainText().trimmed().isEmpty()) {
        ui->sampleEdit->setExtraSelections({});
        ui->previewStatusLabel->setStyleSheet(QString());
        ui->previewStatusLabel->setText(!pattern.isValid()
            ? tr("Schema is incomplete — fix it to see the preview.")
            : QString());
        return;
    }

    m_previewSlow = false;
    m_previewWatchdog->start();

    // Match in a worker thread: a slow regex must never freeze the UI.
    m_previewWatcher->setFuture(QtConcurrent::run([pattern, lines, generation]() {
        PreviewJob job;
        job.generation = generation;
        job.lines.reserve(lines.size());
        for (const QString& line : lines)
            job.lines.append(pattern.matchLine(line));
        return job;
    }));
}

void ConversionPatternDialog::onPreviewReady()
{
    const PreviewJob job = m_previewWatcher->result();
    if (job.generation != m_previewGeneration)
        return; // stale result of an older schema/text state

    m_previewWatchdog->stop();
    m_previewSlow = false;

    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument* doc = ui->sampleEdit->document();

    int fullCount = 0, partialCount = 0, failedCount = 0, emptyCount = 0;
    const QStringList lines = previewLines();

    for (int i = 0; i < job.lines.size() && i < doc->blockCount(); ++i) {
        const QTextBlock textBlock = doc->findBlockByNumber(i);
        const int basePos = textBlock.position();
        const LineMatchResult& match = job.lines[i];
        const QString& lineText = (i < lines.size()) ? lines[i] : QString();

        if (lineText.trimmed().isEmpty()) {
            ++emptyCount;
            continue;
        }

        if (!match.ok) {
            ++failedCount;
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(textBlock);
            sel.cursor.setPosition(basePos);
            sel.cursor.setPosition(basePos + textBlock.length() - 1, QTextCursor::KeepAnchor);
            sel.format.setBackground(QColor(0, 0, 0, 25));
            selections.append(sel);
            continue;
        }

        for (const BlockSpan& span : match.spans) {
            if (span.length <= 0)
                continue;
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(textBlock);
            sel.cursor.setPosition(basePos + span.start);
            sel.cursor.setPosition(basePos + span.start + span.length, QTextCursor::KeepAnchor);
            sel.format.setBackground(highlightColorForIndex(span.blockIndex));
            selections.append(sel);
        }

        if (match.unparsedStart >= 0 && match.unparsedStart < lineText.size()) {
            ++partialCount;
            QTextEdit::ExtraSelection sel;
            sel.cursor = QTextCursor(textBlock);
            sel.cursor.setPosition(basePos + match.unparsedStart);
            sel.cursor.setPosition(basePos + textBlock.length() - 1, QTextCursor::KeepAnchor);
            sel.format.setBackground(QColor(0xC0, 0x39, 0x2B, 60));
            sel.format.setFontUnderline(true);
            sel.format.setUnderlineColor(QColor(0xC0, 0x39, 0x2B));
            selections.append(sel);
        } else {
            ++fullCount;
        }
    }

    ui->sampleEdit->setExtraSelections(selections);

    QStringList statusParts;
    statusParts << tr("%1 matched").arg(fullCount);
    if (partialCount > 0)
        statusParts << tr("%1 partial (red tail goes to the last text field)").arg(partialCount);
    if (failedCount > 0)
        statusParts << tr("%1 not recognized").arg(failedCount);
    ui->previewStatusLabel->setStyleSheet(
        failedCount > 0 ? QStringLiteral("color:#C0392B;")
        : partialCount > 0 ? QStringLiteral("color:#B9770E;")
        : QStringLiteral("color:#1E8449;"));
    ui->previewStatusLabel->setText(statusParts.join(QStringLiteral(", ")));
}

// ---------------------------------------------------------------- //
// Validation & buttons
// ---------------------------------------------------------------- //

void ConversionPatternDialog::updateValidationLabel()
{
    if (ui->schemaTable->currentRow() < 0) {
        ui->validationLabel->clear();
        return;
    }

    QString error;
    if (!LogPattern::validateDefinition(currentDefinitionFromEditor(), &error)) {
        ui->validationLabel->setStyleSheet(QStringLiteral("color:#C0392B;"));
        ui->validationLabel->setText(m_dirty ? error + tr(" — not applied") : error);
    } else if (m_dirty) {
        ui->validationLabel->setStyleSheet(QStringLiteral("color:#B9770E;"));
        ui->validationLabel->setText(tr("Schema is valid — press Apply to save the changes."));
    } else {
        ui->validationLabel->setStyleSheet(QStringLiteral("color:#1E8449;"));
        ui->validationLabel->setText(tr("Schema is valid."));
    }
}

void ConversionPatternDialog::updateButtonStates()
{
    const int schemaRow = ui->schemaTable->currentRow();
    const bool hasSchema = schemaRow >= 0;

    ui->removeSchemaBtn->setEnabled(hasSchema);
    ui->moveSchemaUpBtn->setEnabled(schemaRow > 0);
    ui->moveSchemaDownBtn->setEnabled(hasSchema && schemaRow < ui->schemaTable->rowCount() - 1);
    ui->useBtn->setEnabled(hasSchema);

    ui->nameEdit->setEnabled(hasSchema);
    ui->applyBtn->setEnabled(hasSchema && m_dirty);
    ui->revertBtn->setEnabled(hasSchema && m_dirty);
    ui->addBlockBtn->setEnabled(hasSchema);
    ui->autoDetectBtn->setEnabled(hasSchema);
    ui->presetBtn->setEnabled(hasSchema);
    ui->grokBtn->setEnabled(hasSchema);
    ui->savePresetBtn->setEnabled(hasSchema);
    ui->blocksScroll->setEnabled(hasSchema);
}

// ---------------------------------------------------------------- //
// Slots: schema list operations
// ---------------------------------------------------------------- //

void ConversionPatternDialog::onAddSchema()
{
    resolveDirtyState(ui->schemaTable->currentRow());

    const int row = ui->schemaTable->rowCount();
    ui->schemaTable->insertRow(row);
    setSchemaRow(row,
                 tr("New schema"),
                 LogPattern::serializeDefinition(defaultDefinition()));
    ui->schemaTable->setCurrentCell(row, 0);
    ui->nameEdit->setFocus();
    ui->nameEdit->selectAll();
}

void ConversionPatternDialog::onRemoveSchema()
{
    const int row = ui->schemaTable->currentRow();
    if (row < 0)
        return;

    ui->schemaTable->removeRow(row);
    if (ui->schemaTable->rowCount() > 0)
        ui->schemaTable->setCurrentCell(qMin(row, ui->schemaTable->rowCount() - 1), 0);
    else
        loadSchemaRow(-1);
    updateButtonStates();
}

void ConversionPatternDialog::onMoveSchemaUp()
{
    const int row = ui->schemaTable->currentRow();
    if (row <= 0)
        return;

    resolveDirtyState(row);
    for (int col = 0; col < ui->schemaTable->columnCount(); ++col) {
        auto* a = ui->schemaTable->takeItem(row - 1, col);
        auto* b = ui->schemaTable->takeItem(row, col);
        ui->schemaTable->setItem(row - 1, col, b);
        ui->schemaTable->setItem(row, col, a);
    }
    ui->schemaTable->setCurrentCell(row - 1, 0);
}

void ConversionPatternDialog::onMoveSchemaDown()
{
    const int row = ui->schemaTable->currentRow();
    if (row < 0 || row >= ui->schemaTable->rowCount() - 1)
        return;

    resolveDirtyState(row);
    for (int col = 0; col < ui->schemaTable->columnCount(); ++col) {
        auto* a = ui->schemaTable->takeItem(row, col);
        auto* b = ui->schemaTable->takeItem(row + 1, col);
        ui->schemaTable->setItem(row, col, b);
        ui->schemaTable->setItem(row + 1, col, a);
    }
    ui->schemaTable->setCurrentCell(row + 1, 0);
}

void ConversionPatternDialog::onSchemaSelectionChanged(int currentRow, int, int previousRow, int)
{
    if (m_syncing)
        return;

    resolveDirtyState(previousRow);
    loadSchemaRow(currentRow);
}

void ConversionPatternDialog::onSchemaNameEdited(const QString&)
{
    if (m_syncing)
        return;
    setDirty();
}

// ---------------------------------------------------------------- //
// Slots: block operations
// ---------------------------------------------------------------- //

void ConversionPatternDialog::onAddBlock()
{
    if (ui->schemaTable->currentRow() < 0)
        return;

    PatternDefinition definition = currentDefinitionFromEditor();

    // Pick the first free "Field N" — duplicate field names break parsing.
    QSet<QString> usedNames;
    for (const PatternBlock& existing : definition.blocks)
        usedNames.insert(existing.name.trimmed().toLower());
    int nameIndex = 1;
    while (usedNames.contains(tr("Field %1").arg(nameIndex).toLower()))
        ++nameIndex;

    PatternBlock block;
    block.name = tr("Field %1").arg(nameIndex);
    block.matchKind = PatternBlock::MatchKind::TextUntilSeparator;

    // Keep "Remainder of line" last: insert the new block before it.
    int insertAt = definition.blocks.size();
    if (!definition.blocks.isEmpty()
            && definition.blocks.last().matchKind == PatternBlock::MatchKind::Remainder)
        --insertAt;
    definition.blocks.insert(insertAt, block);

    replaceCurrentBlocks(definition);
}

void ConversionPatternDialog::onBlocksEdited()
{
    if (m_syncing)
        return;

    setDirty();
    updateValidationLabel();
    schedulePreviewUpdate();
}

// ---------------------------------------------------------------- //
// Apply / close
// ---------------------------------------------------------------- //

void ConversionPatternDialog::onUseSelected()
{
    const int row = ui->schemaTable->currentRow();
    if (row < 0 || row >= ui->schemaTable->rowCount())
        return;

    // "Use Selected" is an explicit commit: apply the edits silently.
    saveSchemaRow(row);
    m_dirty = false;
    updateButtonStates();
    const QString schema = ui->schemaTable->item(row, SchemaColName)
        ? ui->schemaTable->item(row, SchemaColName)->data(SchemaDataRole).toString().trimmed()
        : QString();

    // Atomic apply: full validation before the schema leaves the dialog.
    PatternDefinition definition;
    QString error;
    if (!LogPattern::deserializeDefinition(schema, &definition, true, false)
            || !LogPattern::validateDefinition(definition, &error)) {
        QMessageBox::warning(this, tr("Invalid schema"),
            error.isEmpty()
                ? tr("The selected schema is incomplete or inconsistent.")
                : error);
        return;
    }

    const LogPattern pattern(schema);
    if (!pattern.isValid()) {
        QMessageBox::warning(this, tr("Invalid schema"),
            tr("The schema could not be compiled into a matcher.\n"
               "Check custom regex blocks."));
        return;
    }

    // ReDoS guard: refuse schemas whose matching does not finish quickly.
    if (!probeMatchesWithinTimeout(pattern, previewLines(), kApplyProbeTimeoutMs)) {
        QMessageBox::warning(this, tr("Schema too slow"),
            tr("Matching the preview lines did not finish within %1 ms.\n"
               "Simplify custom regex blocks before applying this schema.")
                .arg(kApplyProbeTimeoutMs));
        return;
    }

    m_chosenPattern = schema;
    m_chosenResultIndex = -1;
    int resultIndex = 0;
    for (int r = 0; r < ui->schemaTable->rowCount(); ++r) {
        const QString currentSchema = ui->schemaTable->item(r, SchemaColName)
            ? ui->schemaTable->item(r, SchemaColName)->data(SchemaDataRole).toString().trimmed()
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

void ConversionPatternDialog::closeEvent(QCloseEvent* event)
{
    // Same as the Close button: offer to apply unapplied edits first.
    resolveDirtyState(ui->schemaTable->currentRow());

    QDialog::accept();   // set result = Accepted, emit finished(Accepted)
    event->accept();     // let Qt proceed with closing
}
