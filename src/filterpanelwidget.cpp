#include "filterpanelwidget.h"
#include "highlightpalette.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

// ===========================================================================
// FilterRuleCard
// ===========================================================================

FilterRuleCard::FilterRuleCard(const FilterRule& rule, QWidget* parent)
    : CardFrame(parent)
    , m_color(rule.highlightColor.isValid() ? rule.highlightColor : HighlightPalette::colorAt(0))
{
    QVBoxLayout* rows = rowsLayout();

    // ---- Строка 1: единый для всех карточек порядок контролов --------- //
    // ☑ → Contains → AND/OR (у первой карточки засерен) → Field → действия.
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(4);
    rows->addLayout(headerRow);

    m_enabledCheckBox = new QCheckBox(this);
    m_enabledCheckBox->setChecked(rule.enabled);
    m_enabledCheckBox->setToolTip(tr("Enable/disable this rule without deleting it"));
    headerRow->addWidget(m_enabledCheckBox);

    m_actionCombo = new QComboBox(this);
    m_actionCombo->addItem(tr("Contains"),     static_cast<int>(FilterRule::Action::Include));
    m_actionCombo->addItem(tr("Not contains"), static_cast<int>(FilterRule::Action::Exclude));
    m_actionCombo->setCurrentIndex(rule.action == FilterRule::Action::Exclude ? 1 : 0);
    m_actionCombo->setFocusPolicy(Qt::StrongFocus);
    headerRow->addWidget(m_actionCombo);

    m_connectorCombo = new QComboBox(this);
    m_connectorCombo->addItem(tr("AND"), static_cast<int>(FilterRule::Connector::And));
    m_connectorCombo->addItem(tr("OR"),  static_cast<int>(FilterRule::Connector::Or));
    m_connectorCombo->setCurrentIndex(rule.connector == FilterRule::Connector::Or ? 1 : 0);
    m_connectorCombo->setToolTip(tr("Logical link to the previous rule.\n"
                                    "AND binds tighter than OR: A AND B OR C = (A AND B) OR C."));
    m_connectorCombo->setFixedWidth(58);
    m_connectorCombo->setFocusPolicy(Qt::StrongFocus);
    headerRow->addWidget(m_connectorCombo);

    m_fieldCombo = new QComboBox(this);
    m_fieldCombo->setToolTip(tr("Bind the rule to a Log Fields column.\n"
                                "Active only while 'Filter blocks' is checked in the Log Fields panel."));
    m_fieldCombo->setMinimumWidth(80);
    m_fieldCombo->setFocusPolicy(Qt::StrongFocus);
    headerRow->addWidget(m_fieldCombo, /*stretch=*/1);

    m_gearButton = makeToolButton(QStringLiteral("⚙"),
        tr("Advanced: case sensitivity and regular-expression mode."));
    m_gearButton->setCheckable(true);
    headerRow->addWidget(m_gearButton);

    m_colorButton = makeToolButton(QString(),
        tr("Match highlight colour (click to change)"));
    m_colorButton->setFixedSize(22, 22);
    m_colorButton->setAutoRaise(false);
    headerRow->addWidget(m_colorButton);

    m_removeButton = makeToolButton(QStringLiteral("✕"), tr("Remove this rule"));
    headerRow->addWidget(m_removeButton);

    // ---- Строка 2: текст правила на всю ширину ------------------------ //
    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText(tr("Filter text..."));
    m_textEdit->setText(rule.text);
    m_textEdit->setClearButtonEnabled(true);
    rows->addWidget(m_textEdit);

    // ---- Строка 3 (⚙): пер-правильные опции поиска -------------------- //
    m_advancedRow = new QWidget(this);
    auto* advLayout = new QHBoxLayout(m_advancedRow);
    advLayout->setContentsMargins(4, 0, 0, 0);
    advLayout->setSpacing(10);

    m_caseSensitiveCheckBox = new QCheckBox(tr("Case sensitive"), m_advancedRow);
    m_caseSensitiveCheckBox->setChecked(rule.caseSensitive);
    advLayout->addWidget(m_caseSensitiveCheckBox);

    m_regexCheckBox = new QCheckBox(tr("Regular expression"), m_advancedRow);
    m_regexCheckBox->setChecked(rule.isRegex);
    m_regexCheckBox->setToolTip(tr("Treat the filter text as a regular expression.\n"
                                   "An invalid expression makes the rule neutral."));
    advLayout->addWidget(m_regexCheckBox);
    advLayout->addStretch(1);

    m_advancedRow->setVisible(false);
    rows->addWidget(m_advancedRow);

    // ---- Сигналы ------------------------------------------------------ //
    connect(m_textEdit, &QLineEdit::returnPressed, this, &FilterRuleCard::applyShortcutPressed);
    connect(m_gearButton, &QToolButton::toggled, this, [this](bool on) {
        m_advancedRow->setVisible(on);
    });
    connect(m_colorButton, &QToolButton::clicked, this, &FilterRuleCard::chooseColor);
    connect(m_removeButton, &QToolButton::clicked, this, &FilterRuleCard::removeRequested);
    connect(m_caseSensitiveCheckBox, &QCheckBox::toggled, this, [this]() { updateGearHighlight(); });
    connect(m_regexCheckBox, &QCheckBox::toggled, this, [this]() { updateGearHighlight(); });

    // Восстанавливаем привязку к колонке: пока схема не передана через
    // setFieldNames(), сохраняем имя как единственный пункт после "(вся строка)".
    m_fieldCombo->addItem(tr("(entire row)"), QString());
    if (!rule.fieldName.isEmpty()) {
        m_fieldCombo->addItem(rule.fieldName, rule.fieldName);
        m_fieldCombo->setCurrentIndex(1);
    }

    setAccentColor(m_color);
    updateColorButton();
    updateGearHighlight();
}

FilterRule FilterRuleCard::rule() const
{
    FilterRule rule;
    rule.enabled        = m_enabledCheckBox->isChecked();
    rule.action         = static_cast<FilterRule::Action>(m_actionCombo->currentData().toInt());
    rule.connector      = static_cast<FilterRule::Connector>(m_connectorCombo->currentData().toInt());
    rule.text           = m_textEdit->text();
    rule.fieldName      = m_fieldCombo->currentData().toString();
    rule.caseSensitive  = m_caseSensitiveCheckBox->isChecked();
    rule.isRegex        = m_regexCheckBox->isChecked();
    rule.highlightColor = m_color;
    return rule;
}

void FilterRuleCard::setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled)
{
    const QString previousField = m_fieldCombo->currentData().toString();

    m_fieldCombo->blockSignals(true);
    m_fieldCombo->clear();
    m_fieldCombo->addItem(tr("(entire row)"), QString());
    for (const QString& name : fieldNames)
        m_fieldCombo->addItem(name, name);

    // Сохраняем выбор пользователя, даже если колонка ушла из схемы:
    // правило с осиротевшим именем продолжит искать по всей строке
    // (bindFields даст -1), а при возврате схемы привязка оживёт.
    if (!previousField.isEmpty()) {
        int idx = m_fieldCombo->findData(previousField);
        if (idx < 0) {
            m_fieldCombo->addItem(previousField, previousField);
            idx = m_fieldCombo->count() - 1;
        }
        m_fieldCombo->setCurrentIndex(idx);
    }
    m_fieldCombo->blockSignals(false);

    m_fieldCombo->setEnabled(fieldScopeEnabled);
}

void FilterRuleCard::setIsFirstRow(bool first)
{
    // Layout всех карточек одинаковый: у первой коннектор не скрывается,
    // а засеривается — связи с предыдущим правилом у неё нет.
    m_connectorCombo->setEnabled(!first);
    m_connectorCombo->setToolTip(first
        ? tr("The first rule has no link to a previous one.")
        : tr("Logical link to the previous rule.\n"
             "AND binds tighter than OR: A AND B OR C = (A AND B) OR C."));
}

void FilterRuleCard::chooseColor()
{
    const QColor chosen = QColorDialog::getColor(m_color, this, tr("Match highlight colour"));
    if (chosen.isValid()) {
        m_color = chosen;
        setAccentColor(m_color);
        updateColorButton();
        updateGearHighlight();
    }
}

void FilterRuleCard::updateColorButton()
{
    m_colorButton->setStyleSheet(
        QStringLiteral("QToolButton { background-color: %1; border: 1px solid palette(mid); border-radius: 3px; }")
            .arg(m_color.name()));
}

void FilterRuleCard::updateGearHighlight()
{
    const bool hasContent = m_caseSensitiveCheckBox->isChecked() || m_regexCheckBox->isChecked();
    tintToolButton(m_gearButton, hasContent);
    m_gearButton->setToolTip(hasContent
        ? tr("Advanced settings contain values — click to view.")
        : tr("Advanced: case sensitivity and regular-expression mode."));
}

// ===========================================================================
// FilterPanelWidget
// ===========================================================================

FilterPanelWidget::FilterPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(5, 5, 5, 5);
    rootLayout->setSpacing(5);

    auto* controlsLayout = new QHBoxLayout();
    m_addButton = new QPushButton(tr("+ Add rule"), this);
    m_addButton->setToolTip(tr("Add a filter rule"));
    connect(m_addButton, &QPushButton::clicked, this, &FilterPanelWidget::onAddRuleClicked);
    controlsLayout->addWidget(m_addButton);

    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setToolTip(tr("Apply the rules to the CURRENT document"));
    m_applyButton->setDefault(true);
    connect(m_applyButton, &QPushButton::clicked, this, &FilterPanelWidget::applyRequested);
    controlsLayout->addWidget(m_applyButton);

    m_resetButton = new QPushButton(tr("Reset"), this);
    m_resetButton->setToolTip(tr("Remove all filters from the CURRENT document.\n"
                                 "The rules stay in the panel for re-applying."));
    connect(m_resetButton, &QPushButton::clicked, this, &FilterPanelWidget::resetRequested);
    controlsLayout->addWidget(m_resetButton);

    controlsLayout->addStretch();
    rootLayout->addLayout(controlsLayout);

    m_rulesLayout = new QVBoxLayout();
    m_rulesLayout->setSpacing(4);
    rootLayout->addLayout(m_rulesLayout);
    rootLayout->addStretch(1);

    addRule(FilterRule{}); // панель всегда начинается с одной пустой карточки
}

FilterRuleSet FilterPanelWidget::ruleSet() const
{
    FilterRuleSet set;
    set.rules.reserve(m_cards.size());
    for (const auto* card : m_cards)
        set.rules.append(card->rule());
    return set;
}

void FilterPanelWidget::setRuleSet(const FilterRuleSet& set)
{
    while (!m_cards.isEmpty())
        removeCard(m_cards.last());

    for (const auto& rule : set.rules)
        addRule(rule);

    if (m_cards.isEmpty())
        addRule(FilterRule{});
}

void FilterPanelWidget::setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled)
{
    m_fieldNames = fieldNames;
    m_fieldScopeEnabled = fieldScopeEnabled;
    for (auto* card : m_cards)
        card->setFieldNames(fieldNames, fieldScopeEnabled);
}

void FilterPanelWidget::onAddRuleClicked()
{
    FilterRule rule;
    rule.highlightColor = nextFreeColor();
    addRule(rule);
}

void FilterPanelWidget::addRule(const FilterRule& rule)
{
    FilterRule prepared = rule;
    if (!prepared.highlightColor.isValid())
        prepared.highlightColor = nextFreeColor();

    auto* card = new FilterRuleCard(prepared, this);
    card->setFieldNames(m_fieldNames, m_fieldScopeEnabled);

    connect(card, &FilterRuleCard::removeRequested, this, [this, card]() {
        removeCard(card);
        if (m_cards.isEmpty())
            addRule(FilterRule{});
    });
    connect(card, &FilterRuleCard::applyShortcutPressed,
            this, &FilterPanelWidget::applyRequested);

    m_rulesLayout->addWidget(card);
    m_cards.append(card);
    renumberRows();
}

void FilterPanelWidget::removeCard(FilterRuleCard* card)
{
    m_cards.removeOne(card);
    m_rulesLayout->removeWidget(card);
    card->deleteLater();
    renumberRows();
}

void FilterPanelWidget::renumberRows()
{
    for (int i = 0; i < m_cards.size(); ++i)
        m_cards[i]->setIsFirstRow(i == 0);
}

QColor FilterPanelWidget::nextFreeColor() const
{
    // Первый цвет палитры, ещё не занятый существующими правилами;
    // при исчерпании палитры — просто следующий по кругу.
    for (int i = 0; i < 10; ++i) {
        const QColor candidate = HighlightPalette::colorAt(i);
        bool used = false;
        for (const auto* card : m_cards) {
            if (card->rule().highlightColor == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
    return HighlightPalette::colorAt(m_cards.size());
}
