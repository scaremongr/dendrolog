#include "conversionpatterndialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QTextBrowser>
#include <QLineEdit>
#include <QLabel>
#include <QGroupBox>
#include <QToolButton>
#include <QPushButton>
#include <QMenu>
#include <QFont>

// ---------------------------------------------------------------------------
// Reference HTML shown in the right panel
// ---------------------------------------------------------------------------
static const char* kHelpHtml = R"HTML(
<html>
<head><style>
  body  { font-family: 'Segoe UI', Arial, sans-serif; font-size: 10pt; margin: 6px 8px; }
  h3    { margin: 10px 0 3px 0; color: #333; }
  table { border-collapse: collapse; }
  td, th { padding: 2px 8px 2px 2px; vertical-align: top; }
  th    { text-align: left; border-bottom: 1px solid #ccc; color: #555; }
  code  { font-family: Consolas, 'Courier New', monospace;
          background: #f0f0f0; padding: 0 3px; border-radius: 2px; font-size: 9.5pt; }
  .dim  { color: #888; font-size: 9pt; }
  ul    { margin: 3px 0; padding-left: 16px; }
  li    { margin-bottom: 3px; }
</style></head>
<body>

<h3>Specifiers</h3>
<table>
<tr><th>Spec</th><th>Field</th></tr>
<tr><td><code>%d</code></td>
    <td>Timestamp<br><span class="dim">e.g. <code>%d{yyyy-MM-dd HH:mm:ss,zzz}</code></span></td></tr>
<tr><td><code>%t</code></td><td>Thread ID / name</td></tr>
<tr><td><code>%c</code></td>
    <td>Logger / category<br><span class="dim">e.g. <code>%c{2}</code> &mdash; last 2 components</span></td></tr>
<tr><td><code>%p</code></td><td>Level (INFO, WARN, ERROR&hellip;)</td></tr>
<tr><td><code>%m</code></td><td>Message body</td></tr>
<tr><td><code>%x</code></td><td>NDC context</td></tr>
<tr><td><code>%F</code></td><td>Source file name</td></tr>
<tr><td><code>%L</code></td><td>Source line number</td></tr>
<tr><td><code>%n</code></td><td>End-of-line (stop parsing)</td></tr>
<tr><td><code>%%</code></td><td>Literal &percnt;</td></tr>
</table>

<h3>Alignment modifiers</h3>
<table>
<tr><th>Example</th><th>Meaning</th></tr>
<tr><td><code>%-20c</code></td><td>Left-align, min&nbsp;20&nbsp;chars</td></tr>
<tr><td><code>%10t</code></td><td>Right-align, min&nbsp;10&nbsp;chars</td></tr>
<tr><td><code>%-8p</code></td><td>Left-align, min&nbsp;8&nbsp;chars</td></tr>
</table>

<h3>Typical patterns</h3>
<p><code>%d [%-10t] %-20c&nbsp;&nbsp;%-8p %m ~~ %x {%F:%L}%n</code></p>
<p><code>%d %-5p [%t] %c &mdash; %m%n</code></p>
<p><code>%d{HH:mm:ss} %-5p %m%n</code></p>

<h3>Tips</h3>
<ul>
  <li>Specifiers are matched left-to-right using the literal text between them.</li>
  <li><code>%n</code> stops extraction; text after it is ignored.</li>
  <li>Alignment padding is stripped automatically from extracted values.</li>
  <li>An optional leading prefix before <code>%d</code> (e.g.&nbsp;<code>&gt;&nbsp;</code>) is fine &mdash; omit it if your lines don&apos;t start with it.</li>
  <li>Windows paths in <code>%F</code> (e.g.&nbsp;<code>C:\file.cpp:42</code>) are handled correctly.</li>
</ul>

</body>
</html>
)HTML";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ConversionPatternDialog::ConversionPatternDialog(const PatternList& patterns,
                                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Manage Conversion Patterns"));
    setMinimumSize(800, 480);
    resize(960, 580);

    // ---- Pattern table ----
    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Pattern")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->resizeSection(0, 200);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); // edited via form below
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);

    // ---- Table toolbar ----
    m_addBtn      = new QPushButton(tr("+  Add"),    this);
    m_removeBtn   = new QPushButton(tr("\u2212  Remove"), this); // − Remove
    m_moveUpBtn   = new QPushButton(tr("\u2191"), this);         // ↑
    m_moveDownBtn = new QPushButton(tr("\u2193"), this);         // ↓
    m_moveUpBtn->setFixedWidth(30);
    m_moveDownBtn->setFixedWidth(30);

    auto* tableToolbar = new QHBoxLayout();
    tableToolbar->setSpacing(4);
    tableToolbar->addWidget(m_addBtn);
    tableToolbar->addWidget(m_removeBtn);
    tableToolbar->addStretch();
    tableToolbar->addWidget(m_moveUpBtn);
    tableToolbar->addWidget(m_moveDownBtn);

    // ---- Edit fields ----
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Display name for this pattern"));

    m_patternEdit = new QLineEdit(this);
    m_patternEdit->setPlaceholderText(
        tr("e.g.  %d [%-10t] %-20c  %-8p %m ~~ %x {%F:%L}%n"));
    QFont mono;
    mono.setFamily(QStringLiteral("Consolas"));
    mono.setPointSize(9);
    m_patternEdit->setFont(mono);

    // "Insert specifier" tool-button with popup menu
    m_insertBtn = new QToolButton(this);
    m_insertBtn->setText(tr("Insert \u25be")); // ▾
    m_insertBtn->setPopupMode(QToolButton::InstantPopup);

    auto* insertMenu = new QMenu(m_insertBtn);
    struct SpecItem { const char* spec; const char* label; };
    static constexpr SpecItem kSpecs[] = {
        { "%d",  "Timestamp" },
        { "%t",  "Thread ID" },
        { "%c",  "Logger name" },
        { "%p",  "Level" },
        { "%m",  "Message" },
        { "%x",  "NDC context" },
        { "%F",  "Source file" },
        { "%L",  "Source line" },
        { "%n",  "End-of-line (stop)" },
        { "%%",  "Literal %" },
    };
    for (const auto& si : kSpecs) {
        const QString spec = QString::fromLatin1(si.spec);
        const QString lbl  = QString::fromLatin1(si.label);
        insertMenu->addAction(
            spec + QStringLiteral("  \u2014  ") + lbl,   // "  —  "
            this,
            [this, spec]() {
                const int cur = m_patternEdit->cursorPosition();
                QString   txt = m_patternEdit->text();
                txt.insert(cur, spec);
                m_patternEdit->setText(txt);
                m_patternEdit->setCursorPosition(cur + spec.length());
                m_patternEdit->setFocus();
            });
    }
    m_insertBtn->setMenu(insertMenu);

    // Edit form grouped in a box
    auto* editGroup = new QGroupBox(tr("Edit selected entry"), this);
    auto* editForm  = new QFormLayout(editGroup);
    editForm->setSpacing(5);
    editForm->addRow(tr("Name:"), m_nameEdit);
    auto* patRow = new QHBoxLayout();
    patRow->setSpacing(4);
    patRow->addWidget(m_patternEdit, 1);
    patRow->addWidget(m_insertBtn);
    editForm->addRow(tr("Pattern:"), patRow);

    // ---- Left panel ----
    auto* leftWidget = new QWidget(this);
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setSpacing(5);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_table, 1);
    leftLayout->addLayout(tableToolbar);
    leftLayout->addWidget(editGroup);

    // ---- Right panel (help) ----
    auto* helpBrowser = new QTextBrowser(this);
    helpBrowser->setOpenExternalLinks(false);
    helpBrowser->setHtml(QString::fromLatin1(kHelpHtml));
    helpBrowser->setMinimumWidth(240);

    // ---- Splitter ----
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(leftWidget);
    splitter->addWidget(helpBrowser);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // ---- Bottom buttons ----
    m_useBtn       = new QPushButton(tr("Use Selected"), this);
    auto* closeBtn = new QPushButton(tr("Close"),        this);
    closeBtn->setDefault(true);

    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(m_useBtn);
    btnRow->addWidget(closeBtn);

    // ---- Main layout ----
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->addWidget(splitter, 1);
    mainLayout->addLayout(btnRow);

    // ---- Connections ----
    connect(m_addBtn,      &QPushButton::clicked,
            this, &ConversionPatternDialog::onAdd);
    connect(m_removeBtn,   &QPushButton::clicked,
            this, &ConversionPatternDialog::onRemove);
    connect(m_moveUpBtn,   &QPushButton::clicked,
            this, &ConversionPatternDialog::onMoveUp);
    connect(m_moveDownBtn, &QPushButton::clicked,
            this, &ConversionPatternDialog::onMoveDown);
    connect(m_table,       &QTableWidget::itemSelectionChanged,
            this, &ConversionPatternDialog::onSelectionChanged);
    connect(m_nameEdit,    &QLineEdit::textEdited,
            this, &ConversionPatternDialog::onNameEdited);
    connect(m_patternEdit, &QLineEdit::textEdited,
            this, &ConversionPatternDialog::onPatternEdited);
    connect(m_useBtn,      &QPushButton::clicked,
            this, &ConversionPatternDialog::onUseSelected);
    connect(closeBtn,      &QPushButton::clicked,
            this, &QDialog::accept);

    populateTable(patterns);
    updateButtonStates();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
ConversionPatternDialog::PatternList ConversionPatternDialog::resultPatterns() const
{
    PatternList list;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString name    = m_table->item(r, 0) ? m_table->item(r, 0)->text()         : QString();
        const QString pattern = m_table->item(r, 1) ? m_table->item(r, 1)->text().trimmed() : QString();
        if (!pattern.isEmpty())
            list.append({name, pattern});
    }
    return list;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
void ConversionPatternDialog::populateTable(const PatternList& list)
{
    m_table->setRowCount(0);
    for (const auto& entry : list) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(entry.first));
        m_table->setItem(row, 1, new QTableWidgetItem(entry.second));
    }
}

void ConversionPatternDialog::syncFromRow(int row)
{
    m_syncing = true;
    if (row >= 0 && row < m_table->rowCount()) {
        m_nameEdit->setText(
            m_table->item(row, 0) ? m_table->item(row, 0)->text() : QString());
        m_patternEdit->setText(
            m_table->item(row, 1) ? m_table->item(row, 1)->text() : QString());
    } else {
        m_nameEdit->clear();
        m_patternEdit->clear();
    }
    m_syncing = false;
}

void ConversionPatternDialog::syncToRow(int row)
{
    if (row < 0 || row >= m_table->rowCount()) return;
    if (!m_table->item(row, 0)) m_table->setItem(row, 0, new QTableWidgetItem());
    if (!m_table->item(row, 1)) m_table->setItem(row, 1, new QTableWidgetItem());
    m_table->item(row, 0)->setText(m_nameEdit->text());
    m_table->item(row, 1)->setText(m_patternEdit->text());
}

void ConversionPatternDialog::updateButtonStates()
{
    const int  row    = m_table->currentRow();
    const int  nRows  = m_table->rowCount();
    const bool hasRow = (row >= 0);
    m_removeBtn->setEnabled(hasRow);
    m_moveUpBtn->setEnabled(row > 0);
    m_moveDownBtn->setEnabled(hasRow && row < nRows - 1);
    m_useBtn->setEnabled(hasRow);
    m_nameEdit->setEnabled(hasRow);
    m_patternEdit->setEnabled(hasRow);
    m_insertBtn->setEnabled(hasRow);
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void ConversionPatternDialog::onAdd()
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(tr("New Pattern")));
    m_table->setItem(row, 1, new QTableWidgetItem(QString()));
    m_table->setCurrentCell(row, 0);
    m_nameEdit->setFocus();
    m_nameEdit->selectAll();
}

void ConversionPatternDialog::onRemove()
{
    const int row = m_table->currentRow();
    if (row < 0) return;
    m_table->removeRow(row);
    if (m_table->rowCount() > 0)
        m_table->setCurrentCell(qMin(row, m_table->rowCount() - 1), 0);
    updateButtonStates();
}

void ConversionPatternDialog::onMoveUp()
{
    const int row = m_table->currentRow();
    if (row <= 0) return;
    for (int col = 0; col < 2; ++col) {
        auto* a = m_table->takeItem(row - 1, col);
        auto* b = m_table->takeItem(row,     col);
        m_table->setItem(row - 1, col, b);
        m_table->setItem(row,     col, a);
    }
    m_table->setCurrentCell(row - 1, 0);
}

void ConversionPatternDialog::onMoveDown()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_table->rowCount() - 1) return;
    for (int col = 0; col < 2; ++col) {
        auto* a = m_table->takeItem(row,     col);
        auto* b = m_table->takeItem(row + 1, col);
        m_table->setItem(row,     col, b);
        m_table->setItem(row + 1, col, a);
    }
    m_table->setCurrentCell(row + 1, 0);
}

void ConversionPatternDialog::onSelectionChanged()
{
    syncFromRow(m_table->currentRow());
    updateButtonStates();
}

void ConversionPatternDialog::onNameEdited(const QString&)
{
    if (!m_syncing) syncToRow(m_table->currentRow());
}

void ConversionPatternDialog::onPatternEdited(const QString&)
{
    if (!m_syncing) syncToRow(m_table->currentRow());
}

void ConversionPatternDialog::onUseSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_table->rowCount()) return;
    m_chosenPattern = m_table->item(row, 1)
                          ? m_table->item(row, 1)->text().trimmed()
                          : QString();

    // Compute the index in resultPatterns() — skips rows with empty patterns.
    m_chosenResultIndex = -1;
    int resultIdx = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString p = m_table->item(r, 1)
                              ? m_table->item(r, 1)->text().trimmed()
                              : QString();
        if (!p.isEmpty()) {
            if (r == row) {
                m_chosenResultIndex = resultIdx;
                break;
            }
            ++resultIdx;
        }
    }
    accept();
}
