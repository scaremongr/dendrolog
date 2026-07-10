#include "filterpanelwidget.h"
#include "highlightpalette.h"
#include "toggleswitch.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
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

    // Единый образец цвета: левый клик = выбрать цвет, правый клик = вкл/выкл
    // раскраску совпадений правила. Залит цветом (вкл) или полый (выкл).
    m_highlightEnabled = rule.highlightEnabled;
    m_colorButton = makeToolButton(QString(), QString());
    m_colorButton->setFixedSize(22, 22);
    m_colorButton->setAutoRaise(false);
    m_colorButton->setContextMenuPolicy(Qt::CustomContextMenu);
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
    connect(m_colorButton, &QToolButton::customContextMenuRequested,
            this, [this]() { toggleHighlightEnabled(); });
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
    rule.enabled          = m_enabledCheckBox->isChecked();
    rule.action           = static_cast<FilterRule::Action>(m_actionCombo->currentData().toInt());
    rule.connector        = static_cast<FilterRule::Connector>(m_connectorCombo->currentData().toInt());
    rule.text             = m_textEdit->text();
    rule.fieldName        = m_fieldCombo->currentData().toString();
    rule.caseSensitive    = m_caseSensitiveCheckBox->isChecked();
    rule.isRegex          = m_regexCheckBox->isChecked();
    rule.highlightColor   = m_color;
    rule.highlightEnabled = m_highlightEnabled;
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

void FilterRuleCard::toggleHighlightEnabled()
{
    m_highlightEnabled = !m_highlightEnabled;
    updateColorButton();
}

void FilterRuleCard::updateColorButton()
{
    if (m_highlightEnabled) {
        // Подсветка вкл — образец залит цветом правила.
        m_colorButton->setStyleSheet(
            QStringLiteral("QToolButton { background-color: %1; border: 1px solid palette(mid); border-radius: 3px; }")
                .arg(m_color.name()));
        m_colorButton->setToolTip(tr("Highlighting this rule's matches.\n"
                                     "Left-click: choose colour.  Right-click: turn off."));
    } else {
        // Подсветка выкл — полый образец (кольцо цвета на нейтральном фоне).
        m_colorButton->setStyleSheet(
            QStringLiteral("QToolButton { background-color: palette(base); border: 2px solid %1; border-radius: 3px; }")
                .arg(m_color.name()));
        m_colorButton->setToolTip(tr("Not highlighting this rule's matches.\n"
                                     "Left-click: choose colour.  Right-click: turn on."));
    }
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

    // ================= Компактный блок настроек (CardFrame) =============== //
    // Профиль, режим и действия собраны в один аккуратный блок с плоскими
    // tool-кнопками — вместо россыпи крупных текстовых кнопок.
    m_settingsCard = new CardFrame(this);
    m_settingsCard->setAccentColor(palette().color(QPalette::Mid)); // нейтральный акцент
    QVBoxLayout* cardRows = m_settingsCard->rowsLayout();
    cardRows->setSpacing(4);

    // ---- Ряд 1: профиль + меню действий ------------------------------- //
    auto* profileRow = new QHBoxLayout();
    profileRow->setSpacing(4);
    profileRow->addWidget(new QLabel(tr("Profile:"), m_settingsCard));
    m_profileCombo = new QComboBox(m_settingsCard);
    m_profileCombo->setToolTip(tr("Saved filter profiles"));
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::activated),
            this, &FilterPanelWidget::onProfileSelected);
    profileRow->addWidget(m_profileCombo, /*stretch=*/1);
    m_profileMenuButton = m_settingsCard->makeToolButton(QStringLiteral("⋯"),
        tr("Profile actions: save, save as new, rename, delete"));
    m_profileMenuButton->setPopupMode(QToolButton::InstantPopup);
    buildProfileMenu();
    profileRow->addWidget(m_profileMenuButton);
    cardRows->addLayout(profileRow);

    // ---- Ряд 2: режим (toggle switch) + подсветка в одну строку ------- //
    // Порядок как у чекбокса: сам переключатель слева, подпись справа.
    auto* modeRow = new QHBoxLayout();
    modeRow->setSpacing(6);
    m_modeSwitch = new ToggleSwitch(m_settingsCard); // off = Filter, on = Search
    const QString modeTip = tr("On — non-destructive search: keep all rows; matches go to\n"
                               "the Search Results panel (click one to jump there).\n"
                               "Off — filter: hide non-matching rows in the main view.");
    m_modeSwitch->setToolTip(modeTip);
    connect(m_modeSwitch, &ToggleSwitch::toggled, this, [this]() {
        updateModeDependentUi();
        emit modeChanged(mode());
    });
    modeRow->addWidget(m_modeSwitch);
    auto* modeLabel = new QLabel(tr("Non-destructive search"), m_settingsCard);
    modeLabel->setToolTip(modeTip);
    modeRow->addWidget(modeLabel);
    modeRow->addStretch(1);

    // Галочка подсветки — теперь всегда в этой же строке (шестерёнка не нужна);
    // действует в обоих режимах, поэтому вёрстка не «скачет».
    m_highlightMainCheckBox = new QCheckBox(tr("Highlight in main view"), m_settingsCard);
    m_highlightMainCheckBox->setChecked(true);
    m_highlightMainCheckBox->setToolTip(tr("Highlight the matched text in the main view.\n"
                                           "Applies in both modes (no rows are hidden by it).\n"
                                           "In Search mode the results panel always highlights matches."));
    connect(m_highlightMainCheckBox, &QCheckBox::toggled,
            this, &FilterPanelWidget::highlightInMainViewChanged);
    modeRow->addWidget(m_highlightMainCheckBox);
    cardRows->addLayout(modeRow);

    // ---- Ряд 3: действия ---------------------------------------------- //
    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(4);
    m_addButton = m_settingsCard->makeToolButton(QStringLiteral("＋ Add rule"),
        tr("Add a filter rule"));
    connect(m_addButton, &QToolButton::clicked, this, &FilterPanelWidget::onAddRuleClicked);
    actionRow->addWidget(m_addButton);
    actionRow->addStretch(1);
    m_resetButton = m_settingsCard->makeToolButton(QStringLiteral("⟲ Reset"), QString());
    connect(m_resetButton, &QToolButton::clicked, this, &FilterPanelWidget::resetRequested);
    actionRow->addWidget(m_resetButton);
    m_applyButton = m_settingsCard->makeToolButton(QString(), QString());
    connect(m_applyButton, &QToolButton::clicked, this, &FilterPanelWidget::applyRequested);
    actionRow->addWidget(m_applyButton);
    cardRows->addLayout(actionRow);

    rootLayout->addWidget(m_settingsCard);

    // ================= Список правил ===================================== //
    m_rulesLayout = new QVBoxLayout();
    m_rulesLayout->setSpacing(4);
    rootLayout->addLayout(m_rulesLayout);
    rootLayout->addStretch(1);

    // Стартовый профиль «Default» + одна пустая карточка правила.
    ensureAtLeastOneProfile();
    refreshProfileCombo();
    addRule(FilterRule{});
    updateModeDependentUi();
}

FilterPanelWidget::Mode FilterPanelWidget::mode() const
{
    return m_modeSwitch->isChecked() ? Mode::Search : Mode::Filter;
}

void FilterPanelWidget::setMode(Mode mode)
{
    // Тихая установка (восстановление настроек): не эмитим modeChanged.
    // Сигналы заблокированы → анимация не сработает, поэтому позицию кружка
    // выставляем явно под новое состояние.
    m_modeSwitch->blockSignals(true);
    m_modeSwitch->setChecked(mode == Mode::Search);
    m_modeSwitch->blockSignals(false);
    m_modeSwitch->setKnobPosition(mode == Mode::Search ? 1.0 : 0.0);
    updateModeDependentUi();
}

bool FilterPanelWidget::highlightInMainView() const
{
    return m_highlightMainCheckBox->isChecked();
}

void FilterPanelWidget::setHighlightInMainView(bool on)
{
    m_highlightMainCheckBox->blockSignals(true);
    m_highlightMainCheckBox->setChecked(on);
    m_highlightMainCheckBox->blockSignals(false);
}

void FilterPanelWidget::updateModeDependentUi()
{
    const bool search = (mode() == Mode::Search);

    // Подпись переключателя постоянная; меняется только основная кнопка действия.
    m_applyButton->setText(search ? tr("▶ Search") : tr("▶ Apply"));
    m_applyButton->setToolTip(search
        ? tr("Search the CURRENT document; list matches in the results panel\n"
             "without hiding any rows.")
        : tr("Apply the rules to the CURRENT document"));
    m_resetButton->setToolTip(search
        ? tr("Clear the results panel.\n"
             "The rules stay in the panel for re-searching.")
        : tr("Remove all filters from the CURRENT document.\n"
             "The rules stay in the panel for re-applying."));
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

// ===========================================================================
// FilterPanelWidget — профили фильтрации
// ===========================================================================

void FilterPanelWidget::ensureAtLeastOneProfile()
{
    if (m_profiles.isEmpty())
        m_profiles.append(Profile{tr("Default"), FilterRuleSet{}});
    m_activeProfileIndex = qBound(0, m_activeProfileIndex, m_profiles.size() - 1);
}

void FilterPanelWidget::buildProfileMenu()
{
    auto* menu = new QMenu(m_profileMenuButton);
    menu->addAction(tr("Save"),          this, &FilterPanelWidget::saveActiveProfile);
    menu->addAction(tr("Save as new…"),  this, &FilterPanelWidget::saveAsNewProfile);
    menu->addSeparator();
    menu->addAction(tr("Rename…"),       this, &FilterPanelWidget::renameActiveProfile);
    menu->addAction(tr("Delete"),        this, &FilterPanelWidget::deleteActiveProfile);
    m_profileMenuButton->setMenu(menu);
}

void FilterPanelWidget::refreshProfileCombo()
{
    m_profileCombo->blockSignals(true);
    m_profileCombo->clear();
    for (const auto& p : m_profiles)
        m_profileCombo->addItem(p.name);
    m_profileCombo->setCurrentIndex(m_activeProfileIndex);
    m_profileCombo->blockSignals(false);
}

bool FilterPanelWidget::currentRulesAreDirty() const
{
    if (m_activeProfileIndex < 0 || m_activeProfileIndex >= m_profiles.size())
        return false;
    return !(ruleSet() == m_profiles[m_activeProfileIndex].ruleSet);
}

QString FilterPanelWidget::uniqueProfileName(const QString& base, int skipIndex) const
{
    QString candidate = base.trimmed();
    if (candidate.isEmpty())
        candidate = tr("Profile");
    const auto taken = [this, skipIndex](const QString& name) {
        for (int i = 0; i < m_profiles.size(); ++i)
            if (i != skipIndex && m_profiles[i].name.compare(name, Qt::CaseInsensitive) == 0)
                return true;
        return false;
    };
    if (!taken(candidate))
        return candidate;
    for (int n = 2; ; ++n) {
        const QString numbered = QStringLiteral("%1 %2").arg(candidate).arg(n);
        if (!taken(numbered))
            return numbered;
    }
}

void FilterPanelWidget::onProfileSelected(int index)
{
    if (index < 0 || index >= m_profiles.size() || index == m_activeProfileIndex)
        return;

    // Несохранённые правки текущего профиля — спросить перед переключением.
    if (currentRulesAreDirty()) {
        const auto answer = QMessageBox::question(this, tr("Unsaved changes"),
            tr("Profile \"%1\" has unsaved changes. Save them before switching?")
                .arg(m_profiles[m_activeProfileIndex].name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            refreshProfileCombo(); // вернуть комбо на активный профиль
            return;
        }
        if (answer == QMessageBox::Save)
            m_profiles[m_activeProfileIndex].ruleSet = ruleSet();
    }

    m_activeProfileIndex = index;
    setRuleSet(m_profiles[index].ruleSet);
    refreshProfileCombo();
    emit profilesChanged();
}

void FilterPanelWidget::saveActiveProfile()
{
    ensureAtLeastOneProfile();
    m_profiles[m_activeProfileIndex].ruleSet = ruleSet();
    emit profilesChanged();
}

void FilterPanelWidget::saveAsNewProfile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save as new profile"),
        tr("Profile name:"), QLineEdit::Normal, tr("New profile"), &ok);
    if (!ok)
        return;
    m_profiles.append(Profile{uniqueProfileName(name), ruleSet()});
    m_activeProfileIndex = m_profiles.size() - 1;
    refreshProfileCombo();
    emit profilesChanged();
}

void FilterPanelWidget::renameActiveProfile()
{
    ensureAtLeastOneProfile();
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename profile"),
        tr("Profile name:"), QLineEdit::Normal,
        m_profiles[m_activeProfileIndex].name, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;
    m_profiles[m_activeProfileIndex].name = uniqueProfileName(name, m_activeProfileIndex);
    refreshProfileCombo();
    emit profilesChanged();
}

void FilterPanelWidget::deleteActiveProfile()
{
    ensureAtLeastOneProfile();
    if (m_profiles.size() == 1) {
        // Последний профиль не удаляем — сбрасываем его в пустой «Default».
        m_profiles[0] = Profile{tr("Default"), FilterRuleSet{}};
        m_activeProfileIndex = 0;
    } else {
        m_profiles.removeAt(m_activeProfileIndex);
        m_activeProfileIndex = qBound(0, m_activeProfileIndex, m_profiles.size() - 1);
    }
    setRuleSet(m_profiles[m_activeProfileIndex].ruleSet);
    refreshProfileCombo();
    emit profilesChanged();
}

QJsonObject FilterPanelWidget::profilesToJson() const
{
    // Активные (несохранённые) правки карточек фиксируем в активный профиль,
    // чтобы при выходе не потерять текущую конфигурацию.
    QVector<Profile> profiles = m_profiles;
    if (m_activeProfileIndex >= 0 && m_activeProfileIndex < profiles.size())
        profiles[m_activeProfileIndex].ruleSet = ruleSet();

    QJsonArray arr;
    for (const auto& p : profiles) {
        QJsonObject o;
        o[QStringLiteral("name")]    = p.name;
        o[QStringLiteral("ruleSet")] = p.ruleSet.toJson();
        arr.append(o);
    }
    QJsonObject json;
    json[QStringLiteral("profiles")] = arr;
    json[QStringLiteral("active")]   = (m_activeProfileIndex >= 0 && m_activeProfileIndex < profiles.size())
        ? profiles[m_activeProfileIndex].name : QString();
    return json;
}

void FilterPanelWidget::profilesFromJson(const QJsonObject& json)
{
    const QJsonArray arr = json[QStringLiteral("profiles")].toArray();
    m_profiles.clear();
    for (const auto& v : arr) {
        const QJsonObject o = v.toObject();
        m_profiles.append(Profile{
            o[QStringLiteral("name")].toString(),
            FilterRuleSet::fromJson(o[QStringLiteral("ruleSet")].toObject())});
    }
    ensureAtLeastOneProfile();

    const QString active = json[QStringLiteral("active")].toString();
    m_activeProfileIndex = 0;
    for (int i = 0; i < m_profiles.size(); ++i)
        if (m_profiles[i].name == active) { m_activeProfileIndex = i; break; }

    setRuleSet(m_profiles[m_activeProfileIndex].ruleSet);
    refreshProfileCombo();
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
