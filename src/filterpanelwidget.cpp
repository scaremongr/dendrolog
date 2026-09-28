#include "filterpanelwidget.h"
#include "apptheme.h"
#include "highlightpalette.h"

#include <QApplication>
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
    , m_color(rule.highlightColor.isValid()
                  ? rule.highlightColor
                  : HighlightPalette::colorAt(0, QApplication::palette().color(QPalette::Base)))
{
    QVBoxLayout* rows = rowsLayout();

    // ---- Строка 1: единый для всех карточек порядок контролов --------- //
    // ☑ → Contains → AND/OR (у первой карточки засерен) → Field → действия.
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(3);
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
    m_colorButton->setFixedSize(20, 20);
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
    advLayout->setSpacing(8);

    m_caseSensitiveCheckBox = new QCheckBox(tr("Case sensitive"), m_advancedRow);
    m_caseSensitiveCheckBox->setChecked(rule.caseSensitive);
    advLayout->addWidget(m_caseSensitiveCheckBox);

    m_regexCheckBox = new QCheckBox(tr("Regular expression"), m_advancedRow);
    m_regexCheckBox->setChecked(rule.isRegex);
    m_regexCheckBox->setToolTip(tr("Treat the filter text as a Perl-style regular expression\n"
                                   "(not a shell wildcard): '*' repeats the previous character,\n"
                                   "'.' is any character, so 'entry number .*' — not 'entry number *'.\n"
                                   "The rule matches when the expression is found ANYWHERE in the row.\n"
                                   "An invalid expression is reported under the text field and the\n"
                                   "rule stays neutral."));
    advLayout->addWidget(m_regexCheckBox);
    advLayout->addStretch(1);

    m_advancedRow->setVisible(false);
    rows->addWidget(m_advancedRow);

    // ---- Строка 4: ошибка компиляции регекса (обычно скрыта) ---------- //
    m_regexErrorLabel = new QLabel(this);
    m_regexErrorLabel->setWordWrap(true);
    m_regexErrorLabel->setVisible(false);
    rows->addWidget(m_regexErrorLabel);

    // ---- Сигналы ------------------------------------------------------ //
    connect(m_textEdit, &QLineEdit::returnPressed, this, &FilterRuleCard::applyShortcutPressed);
    connect(m_textEdit, &QLineEdit::textChanged, this, [this]() { updateRegexValidity(); });
    connect(m_fieldCombo, &QComboBox::currentIndexChanged, this, [this]() { updateRegexValidity(); });
    connect(m_gearButton, &QToolButton::toggled, this, [this](bool on) {
        m_advancedRow->setVisible(on);
    });
    connect(m_colorButton, &QToolButton::clicked, this, &FilterRuleCard::chooseColor);
    connect(m_colorButton, &QToolButton::customContextMenuRequested,
            this, [this]() { toggleHighlightEnabled(); });
    connect(m_removeButton, &QToolButton::clicked, this, &FilterRuleCard::removeRequested);
    connect(m_caseSensitiveCheckBox, &QCheckBox::toggled, this, [this]() { updateGearHighlight(); });
    connect(m_regexCheckBox, &QCheckBox::toggled, this, [this]() {
        updateGearHighlight();
        updateRegexValidity();
    });

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
    updateRegexValidity();
}

void FilterRuleCard::focusText()
{
    m_textEdit->setFocus(Qt::OtherFocusReason);
    m_textEdit->selectAll();
}

void FilterRuleCard::setTextPlaceholder(const QString& text)
{
    m_textPlaceholder = text;
    updateRegexValidity();
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

    // Сохраняем выбор пользователя, даже если колонка ушла из схемы: правило
    // с осиротевшим именем нейтрально (FilterRuleSet::fieldMissing) и помечено
    // под текстом, а при возврате схемы привязка оживёт.
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
    m_schemaFields = fieldNames;
    m_schemaKnown = true;
    m_fieldScopeEnabled = fieldScopeEnabled;
    updateRegexValidity();
}

void FilterRuleCard::setIsFirstRow(bool first)
{
    // Layout всех карточек одинаковый: у первой коннектор не скрывается,
    // а засеривается — связи с предыдущим правилом у неё нет. Семантика AND/OR
    // одинакова в обоих режимах (Filter и Search), поэтому комбобокс активен и
    // в поиске: пользователь волен искать пересечение (AND) или объединение (OR).
    m_connectorCombo->setEnabled(!first);
    m_connectorCombo->setToolTip(first
        ? tr("The first rule has no link to a previous one.")
        : tr("Logical link to the previous rule.\n"
             "AND — a row must match BOTH rules; OR — EITHER of them.\n"
             "AND binds tighter than OR: A AND B OR C = (A AND B) OR C.\n"
             "Works the same in Filter and Search modes."));
}

void FilterRuleCard::updateRegexValidity()
{
    // Невалидный регекс или колонка, которой нет в схеме, молча выключают
    // правило — без этой подсказки поиск просто «ничего не находит», и
    // причина неочевидна.
    QString error;
    QString note; // не ошибка, но правило работает не так, как видно в карточке
    if (m_regexCheckBox->isChecked() && !m_textEdit->text().isEmpty()) {
        const QRegularExpression re(m_textEdit->text());
        if (!re.isValid()) {
            error = re.patternErrorOffset() >= 0
                ? tr("Invalid regular expression at position %1: %2")
                      .arg(re.patternErrorOffset()).arg(re.errorString())
                : tr("Invalid regular expression: %1").arg(re.errorString());
        }
    }

    const QString field = m_fieldCombo->currentData().toString();
    if (error.isEmpty() && !field.isEmpty() && m_schemaKnown) {
        if (!m_fieldScopeEnabled)
            note = tr("Log Fields are off: this rule searches the entire row.");
        else if (!m_schemaFields.contains(field))
            error = tr("Column \"%1\" is not in the current schema: the rule is ignored "
                       "until the column is back.").arg(field);
    }

    m_textEdit->setPlaceholderText(m_regexCheckBox->isChecked()
        ? tr("Regular expression...")
        : (m_textPlaceholder.isEmpty() ? tr("Filter text...") : m_textPlaceholder));

    if (error.isEmpty() && note.isEmpty()) {
        m_regexErrorLabel->clear();
        m_regexErrorLabel->setVisible(false);
        m_textEdit->setStyleSheet(QString());
        return;
    }
    if (error.isEmpty()) {
        m_regexErrorLabel->setText(note);
        m_regexErrorLabel->setStyleSheet(QStringLiteral("color: %1;")
            .arg(AppTheme::instance().logDebug.name()));
        m_regexErrorLabel->setVisible(true);
        m_textEdit->setStyleSheet(QString());
        return;
    }
    m_regexErrorLabel->setText(error);
    m_regexErrorLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(AppTheme::instance().logError.name()));
    m_regexErrorLabel->setVisible(true);
    m_textEdit->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid %1; }")
        .arg(AppTheme::instance().logError.name()));
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

FilterPanelWidget::FilterPanelWidget(Mode mode, QWidget* parent)
    : QWidget(parent)
    , m_mode(mode)
{
    const bool search = (mode == Mode::Search);

    // Поля и промежутки — от CompactStyle, как у остальных панелей.
    auto* rootLayout = new QVBoxLayout(this);

    // ================= Компактный блок настроек (CardFrame) =============== //
    // Назначение, профиль и действия собраны в один аккуратный блок с
    // плоскими tool-кнопками — вместо россыпи крупных текстовых кнопок.
    m_settingsCard = new CardFrame(this);
    m_settingsCard->setAccentColor(palette().color(QPalette::Mid)); // нейтральный акцент
    QVBoxLayout* cardRows = m_settingsCard->rowsLayout();
    cardRows->setSpacing(3);

    // ---- Ряд 1: что делает панель — одной строкой ---------------------- //
    auto* purpose = new QLabel(search
        ? tr("Finds lines and lists them; the log itself stays complete.")
        : tr("Hides the lines that do not match the rules."), m_settingsCard);
    purpose->setWordWrap(true);
    purpose->setStyleSheet(QStringLiteral("color: %1;")
        .arg(CardFrame::mutedTextColor(palette()).name()));
    cardRows->addWidget(purpose);

    // ---- Ряд 2: профиль + меню действий ------------------------------- //
    auto* profileRow = new QHBoxLayout();
    profileRow->addWidget(new QLabel(tr("Profile:"), m_settingsCard));
    m_profileCombo = new QComboBox(m_settingsCard);
    m_profileCombo->setToolTip(search ? tr("Saved searches") : tr("Saved filter profiles"));
    connect(m_profileCombo, QOverload<int>::of(&QComboBox::activated),
            this, &FilterPanelWidget::onProfileSelected);
    profileRow->addWidget(m_profileCombo, /*stretch=*/1);
    m_profileMenuButton = m_settingsCard->makeToolButton(QStringLiteral("⋯"),
        tr("Profile actions: save, save as new, rename, delete"));
    m_profileMenuButton->setPopupMode(QToolButton::InstantPopup);
    buildProfileMenu();
    profileRow->addWidget(m_profileMenuButton);
    cardRows->addLayout(profileRow);

    // ---- Ряд 3: добавить правило | подсветка, сброс, запуск ------------ //
    auto* actionRow = new QHBoxLayout();
    m_addButton = m_settingsCard->makeToolButton(QStringLiteral("＋ ") + tr("Add rule"),
        search ? tr("Add a search rule") : tr("Add a filter rule"));
    connect(m_addButton, &QToolButton::clicked, this, &FilterPanelWidget::onAddRuleClicked);
    actionRow->addWidget(m_addButton);
    actionRow->addStretch(1);

    m_highlightMainCheckBox = new QCheckBox(tr("Highlight"), m_settingsCard);
    m_highlightMainCheckBox->setChecked(true);
    m_highlightMainCheckBox->setToolTip(search
        ? tr("Also highlight the found text in the log.\n"
             "The results list always highlights it.")
        : tr("Highlight the matched text in the filtered log."));
    connect(m_highlightMainCheckBox, &QCheckBox::toggled,
            this, &FilterPanelWidget::highlightInMainViewChanged);
    actionRow->addWidget(m_highlightMainCheckBox);

    m_resetButton = m_settingsCard->makeToolButton(
        search ? QStringLiteral("✕ ") + tr("Clear") : QStringLiteral("⟲ ") + tr("Reset"),
        search ? tr("Clear the results. The rules stay here for the next search.")
               : tr("Remove the filter from the CURRENT tab.\n"
                    "The rules stay here for re-applying."));
    connect(m_resetButton, &QToolButton::clicked, this, &FilterPanelWidget::resetRequested);
    actionRow->addWidget(m_resetButton);

    m_applyButton = m_settingsCard->makeToolButton(
        QStringLiteral("▶ ") + (search ? tr("Search") : tr("Apply")),
        search ? tr("Find the matching lines of the CURRENT tab (Enter in a rule does the same)")
               : tr("Filter the CURRENT tab by the rules (Enter in a rule does the same)"));
    connect(m_applyButton, &QToolButton::clicked, this, &FilterPanelWidget::applyRequested);
    actionRow->addWidget(m_applyButton);
    cardRows->addLayout(actionRow);

    rootLayout->addWidget(m_settingsCard);

    // ================= Список правил ===================================== //
    // Прокручивается сам: карточка настроек остаётся на месте, а длинный
    // список правил не растягивает док за край окна.
    m_rulesArea = new CardListArea(this);
    m_rulesLayout = m_rulesArea->cardsLayout();
    rootLayout->addWidget(m_rulesArea, /*stretch=*/1);

    // Стартовый профиль «Default» + одна пустая карточка правила.
    ensureAtLeastOneProfile();
    refreshProfileCombo();
    addRule(FilterRule{});
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

void FilterPanelWidget::focusFirstRule()
{
    if (m_cards.isEmpty())
        addRule(FilterRule{});
    m_rulesArea->revealLater(m_cards.first());
    m_cards.first()->focusText();
}

void FilterPanelWidget::setSingleRule(const QString& text, bool caseSensitive)
{
    while (!m_cards.isEmpty())
        removeCard(m_cards.last());
    FilterRule rule = quickRule(text, /*exclude=*/false);
    rule.caseSensitive = caseSensitive;
    addRule(rule);
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
    // Удобный дефолт под режим: в поиске чаще нужно «показать всё, что нашло
    // ЛЮБОЕ правило» (OR/объединение), в фильтре — сузить (AND). Пользователь
    // может переключить коннектор вручную — оба варианта доступны везде.
    // Для первой карточки коннектор всё равно игнорируется.
    rule.connector = (mode() == Mode::Search) ? FilterRule::Connector::Or
                                              : FilterRule::Connector::And;
    addRule(rule);
    // Новую карточку — на виду и сразу с курсором в поле текста.
    FilterRuleCard* card = m_cards.last();
    m_rulesArea->revealLater(card);
    card->focusText();
}

FilterRule FilterPanelWidget::quickRule(const QString& text, bool exclude) const
{
    FilterRule rule;
    rule.text = text;
    rule.action = exclude ? FilterRule::Action::Exclude : FilterRule::Action::Include;
    // «Скрыть X» и «только X» в фильтре сужают текущий набор (AND); в поиске
    // новый запрос дополняет выдачу (OR), как и правило по «+ Add rule».
    rule.connector = (mode() == Mode::Search && !exclude) ? FilterRule::Connector::Or
                                                           : FilterRule::Connector::And;
    rule.highlightColor = nextFreeColor();
    return rule;
}

void FilterPanelWidget::addQuickRule(const QString& text, bool exclude)
{
    const FilterRule rule = quickRule(text, exclude);
    // Одинокая пустая стартовая карточка — заменить, а не копить пустые правила.
    if (m_cards.size() == 1 && m_cards.first()->rule().text.isEmpty())
        removeCard(m_cards.first());
    addRule(rule);
    m_rulesArea->revealLater(m_cards.last());
}

void FilterPanelWidget::addRule(const FilterRule& rule)
{
    FilterRule prepared = rule;
    if (!prepared.highlightColor.isValid())
        prepared.highlightColor = nextFreeColor();

    auto* card = new FilterRuleCard(prepared, this);
    card->setFieldNames(m_fieldNames, m_fieldScopeEnabled);
    card->setTextPlaceholder(m_mode == Mode::Search ? tr("Text to find...")
                                                    : tr("Text to filter by..."));

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
    m_rulesArea->cardsChanged();
}

void FilterPanelWidget::removeCard(FilterRuleCard* card)
{
    m_cards.removeOne(card);
    m_rulesLayout->removeWidget(card);
    // Вне раскладки карточка осталась бы видна на старом месте до удаления.
    card->hide();
    card->deleteLater();
    renumberRows();
    m_rulesArea->cardsChanged();
}

void FilterPanelWidget::renumberRows()
{
    for (int i = 0; i < m_cards.size(); ++i)
        m_cards[i]->setIsFirstRow(i == 0);
}

QColor FilterPanelWidget::nextFreeColor() const
{
    // Первый цвет палитры, ещё не занятый существующими правилами;
    // при исчерпании палитры — просто следующий по кругу. Ряд палитры
    // выбирается по фону лога (QPalette::Base): в тёмной теме — тёмные
    // заливки, иначе подсветка «съедает» светлый текст.
    const QColor background = palette().color(QPalette::Base);
    for (int i = 0; i < 10; ++i) {
        const QColor candidate = HighlightPalette::colorAt(i, background);
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
    return HighlightPalette::colorAt(m_cards.size(), background);
}
