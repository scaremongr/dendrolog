#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "appsettings.h"
#include "apptheme.h"
#include "cardframe.h"
#include "filememorystore.h"
#include "schemaproposal.h"
#include "secretstore.h"
#include "shortcutmanager.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SettingsDialog)
{
    ui->setupUi(this);

    // Remove the "?" help button that Windows adds to dialogs by default.
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    // Тема окна; данные пункта — ThemeMode.
    ui->themeComboBox->addItem(tr("Same as system"), int(ThemeMode::System));
    ui->themeComboBox->addItem(tr("Light"), int(ThemeMode::Light));
    ui->themeComboBox->addItem(tr("Dark"), int(ThemeMode::Dark));

    buildColorsTab();
    buildShortcutsTab();
    buildPerformanceControls();
    buildFileMemoryControls();
    buildAiTab();
    loadFromSettings();

    // Live font preview.
    connect(ui->fontFamilyComboBox, &QFontComboBox::currentFontChanged,
            this, &SettingsDialog::updateFontPreview);
    connect(ui->fontSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SettingsDialog::updateFontPreview);
}

// ---------------------------------------------------------------------------
SettingsDialog::~SettingsDialog()
{
    delete ui;
}

// ---------------------------------------------------------------------------
// buildColorsTab
//
// Dynamically constructs the content widget for the Colors scroll area.
// All editable AppTheme colors are registered here in logical groups.
// ---------------------------------------------------------------------------
void SettingsDialog::buildColorsTab()
{
    AppTheme& t = AppTheme::instance();

    // ---- Register all editable colours ----------------------------------- //
    // Each struct: { display label, working-copy value, pointer to AppTheme field }
    struct Group {
        QString              title;
        QVector<ColorEntry>  entries;
    };

    QVector<Group> groups = {
        {
            tr("Log Levels"),
            {
                { tr("Fatal"),  t.logFatal,  &t.logFatal  },
                { tr("Error"),  t.logError,  &t.logError  },
                { tr("Warn"),   t.logWarn,   &t.logWarn   },
                { tr("Info"),   t.logInfo,   &t.logInfo   },
                { tr("Debug"),  t.logDebug,  &t.logDebug  },
                { tr("Trace"),  t.logTrace,  &t.logTrace  },
            }
        },
        {
            tr("Selection"),
            {
                { tr("Highlight fill"),   t.selectionFill,  &t.selectionFill  },
                { tr("Row background"),   t.selectionRowBg, &t.selectionRowBg },
            }
        },
        {
            tr("Syntax Highlight"),
            {
                { tr("String literals"), t.syntaxString, &t.syntaxString },
                { tr("Numbers"),         t.syntaxNumber, &t.syntaxNumber },
                { tr("URLs"),            t.syntaxUrl,    &t.syntaxUrl    },
                { tr("Hex values"),      t.syntaxHex,    &t.syntaxHex    },
                { tr("File paths"),      t.syntaxPath,   &t.syntaxPath   },
                { tr("UUID / GUID"),     t.syntaxGuid,   &t.syntaxGuid   },
                { tr("Timestamps"),      t.syntaxTime,   &t.syntaxTime   },
            }
        },
        {
            tr("UI Elements"),
            {
                { tr("Gutter marker"),  t.gutterMarker, &t.gutterMarker },
                { tr("Bookmark"),       t.gutterBookmark, &t.gutterBookmark },
                { tr("Badge background"), t.badgeBg,    &t.badgeBg      },
                { tr("Badge text"),     t.badgeFg,      &t.badgeFg      },
                { tr("Bracket match"),  t.bracketMatch, &t.bracketMatch },
            }
        },
    };

    // Flatten into m_colorEntries (buttons are set below).
    for (auto& group : groups)
        for (auto& ce : group.entries)
            m_colorEntries.append(ce);

    // ---- Build the widget tree ------------------------------------------- //
    auto* content = new QWidget;
    auto* mainVLayout = new QVBoxLayout(content);
    mainVLayout->setSpacing(6);
    mainVLayout->setContentsMargins(4, 4, 4, 4);

    // Keep a flat index into m_colorEntries as we iterate.
    int entryIdx = 0;
    for (auto& group : groups) {
        auto* groupBox = new QGroupBox(group.title, content);
        auto* formLayout = new QFormLayout(groupBox);
        formLayout->setHorizontalSpacing(8);
        formLayout->setVerticalSpacing(3);

        for (int i = 0; i < group.entries.size(); ++i, ++entryIdx) {
            ColorEntry& ce = m_colorEntries[entryIdx];

            auto* btn = new QPushButton(groupBox);
            btn->setFixedSize(64, 18);
            btn->setFlat(false);
            ce.button = btn;
            updateColorButton(ce);

            // Capture entryIdx by value for the lambda.
            const int idx = entryIdx;
            connect(btn, &QPushButton::clicked, this, [this, idx]() {
                ColorEntry& entry = m_colorEntries[idx];
                const QColor chosen =
                    QColorDialog::getColor(entry.value, this,
                                           tr("Choose Color"),
                                           QColorDialog::ShowAlphaChannel);
                if (chosen.isValid()) {
                    entry.value = chosen;
                    updateColorButton(entry);
                }
            });

            formLayout->addRow(new QLabel(ce.label, groupBox), btn);
        }

        mainVLayout->addWidget(groupBox);
    }

    mainVLayout->addStretch();
    ui->colorsScrollArea->setWidget(content);
}

// ---------------------------------------------------------------------------
void SettingsDialog::updateColorButton(ColorEntry& entry)
{
    if (!entry.button)
        return;

    // Use the color as the button background; choose a contrasting border.
    const bool dark = entry.value.lightness() < 128;
    entry.button->setStyleSheet(
        QStringLiteral("QPushButton { background-color: %1; border: 1px solid %2; }")
            .arg(entry.value.name(QColor::HexArgb),
                 dark ? QStringLiteral("#999999") : QStringLiteral("#555555")));
}

// ---------------------------------------------------------------------------
// buildShortcutsTab
//
// One row per configurable command: a label and a QKeySequenceEdit pre-filled
// with the command's current sequence. Edits are committed in applyToSettings().
// ---------------------------------------------------------------------------
void SettingsDialog::buildShortcutsTab()
{
    const ShortcutManager& mgr = ShortcutManager::instance();

    auto* content = new QWidget;
    auto* form = new QFormLayout(content);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(3);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    for (const auto& cmd : mgr.commands()) {
        auto* edit = new QKeySequenceEdit(content);
        edit->setObjectName(QStringLiteral("shortcut_") + cmd.id);
        edit->setKeySequence(mgr.sequence(cmd.id));
        // A single combination is enough for application shortcuts.
        edit->setMaximumSequenceLength(1);
        // Without the clear button a shortcut could not be removed at all:
        // Backspace/Delete are recorded as the new combination.
        edit->setClearButtonEnabled(true);
        connect(edit, &QKeySequenceEdit::keySequenceChanged,
                this, [this]() { updateShortcutConflicts(); });

        m_shortcutRows.append({ cmd.id, edit, edit->keySequence() });
        m_shortcutLabels.insert(cmd.id, cmd.label);
        form->addRow(new QLabel(cmd.label, content), edit);
    }

    ui->shortcutsScrollArea->setWidget(content);

    // Conflicts are listed under the table (hidden while there are none).
    m_shortcutConflictLabel = new QLabel(ui->shortcutsTab);
    m_shortcutConflictLabel->setWordWrap(true);
    m_shortcutConflictLabel->setStyleSheet(QStringLiteral("color: %1;")
        .arg(AppTheme::instance().logError.darker(115).name()));
    m_shortcutConflictLabel->hide();
    if (auto* box = qobject_cast<QBoxLayout*>(ui->shortcutsTab->layout()))
        box->insertWidget(box->indexOf(ui->shortcutsScrollArea) + 1, m_shortcutConflictLabel);

    connect(ui->resetShortcutsButton, &QPushButton::clicked,
            this, &SettingsDialog::resetShortcutsToDefaults);
    updateShortcutConflicts();
}

// ---------------------------------------------------------------------------
// One combination bound to two commands would silently drive only one of
// them: such fields get a red border, the clashes are listed under the table
// and OK refuses to apply the settings until they are resolved.
// ---------------------------------------------------------------------------
QStringList SettingsDialog::updateShortcutConflicts()
{
    QHash<QString, QStringList> commandsByKeys;
    for (const ShortcutRow& row : std::as_const(m_shortcutRows)) {
        const QKeySequence seq = row.edit->keySequence();
        if (!seq.isEmpty())
            commandsByKeys[seq.toString(QKeySequence::NativeText)]
                << m_shortcutLabels.value(row.id);
    }

    const QString red = AppTheme::instance().logError.darker(115).name();
    for (const ShortcutRow& row : std::as_const(m_shortcutRows)) {
        const QString keys = row.edit->keySequence().toString(QKeySequence::NativeText);
        const QStringList users = keys.isEmpty() ? QStringList() : commandsByKeys.value(keys);
        const bool clash = users.size() > 1;
        row.edit->setStyleSheet(clash
            ? QStringLiteral("QLineEdit { border: 1px solid %1; }").arg(red)
            : QString());
        row.edit->setToolTip(clash
            ? tr("%1 is also used by: %2").arg(keys, users.join(QStringLiteral(", ")))
            : QString());
    }

    QStringList conflicts;
    for (auto it = commandsByKeys.constBegin(); it != commandsByKeys.constEnd(); ++it) {
        if (it.value().size() > 1)
            conflicts << tr("%1 — %2").arg(it.key(), it.value().join(QStringLiteral(", ")));
    }
    conflicts.sort();
    if (m_shortcutConflictLabel) {
        m_shortcutConflictLabel->setText(tr("Each shortcut can drive only one command:\n%1")
                                             .arg(conflicts.join(QLatin1Char('\n'))));
        m_shortcutConflictLabel->setVisible(!conflicts.isEmpty());
    }
    return conflicts;
}

// ---------------------------------------------------------------------------
// buildPerformanceControls
//
// Настройки индексного бэкенда больших файлов. Добавляются в код (рядом с
// контролами auto-reload), чтобы не редактировать .ui: если разметка General
// не форма — заворачиваются в собственный QGroupBox.
// ---------------------------------------------------------------------------
void SettingsDialog::buildPerformanceControls()
{
    QWidget* page = ui->autoReloadIntervalSpinBox
        ? ui->autoReloadIntervalSpinBox->parentWidget() : nullptr;
    if (!page)
        return;

    m_indexedThresholdSpinBox = new QSpinBox(page);
    m_indexedThresholdSpinBox->setObjectName(QStringLiteral("indexedThresholdSpinBox"));
    m_indexedThresholdSpinBox->setRange(0, 1 << 20);
    m_indexedThresholdSpinBox->setSuffix(tr(" MB"));
    m_indexedThresholdSpinBox->setSpecialValueText(tr("always"));
    m_indexedThresholdSpinBox->setToolTip(
        tr("Files at or above this size open through the on-disk line index\n"
           "(text stays on disk, memory usage stays flat). 0 = always.\n"
           "Applies to files opened after the change."));

    m_textCacheSpinBox = new QSpinBox(page);
    m_textCacheSpinBox->setRange(64, 2048);
    m_textCacheSpinBox->setSuffix(tr(" MB"));
    m_textCacheSpinBox->setToolTip(
        tr("Memory budget for cached text of indexed files."));

    // Запасная кодировка (F-1): что автоматика берёт для файла не в UTF-8.
    m_fallbackEncodingCombo = new QComboBox(page);
    m_fallbackEncodingCombo->setObjectName(QStringLiteral("fallbackEncodingComboBox"));
    m_fallbackEncodingCombo->addItem(
        tr("By system language (%1)").arg(TextEncodings::displayName(TextEncodings::systemFallback())),
        -1);
    for (const TextEncoding e : TextEncodings::selectable())
        if (TextEncodings::isSingleByte(e))
            m_fallbackEncodingCombo->addItem(TextEncodings::displayName(e), int(e));
    m_fallbackEncodingCombo->setToolTip(
        tr("Encoding for files that are not UTF-8. With a Cyrillic choice, Windows-1251,\n"
           "IBM866 and KOI8-R are told apart automatically. Applies to new files and to\n"
           "open files whose encoding is not settled yet; a file's encoding can always\n"
           "be changed from the status bar."));

    auto* thresholdLabel = new QLabel(tr("Index files larger than:"), page);
    auto* cacheLabel = new QLabel(tr("Indexed text cache:"), page);
    auto* encodingLabel = new QLabel(tr("Non-UTF-8 files:"), page);

    if (auto* form = qobject_cast<QFormLayout*>(page->layout())) {
        form->addRow(thresholdLabel, m_indexedThresholdSpinBox);
        form->addRow(cacheLabel, m_textCacheSpinBox);
        form->addRow(encodingLabel, m_fallbackEncodingCombo);
    } else if (QLayout* layout = page->layout()) {
        auto* group = new QGroupBox(tr("Large files"), page);
        auto* groupForm = new QFormLayout(group);
        groupForm->addRow(thresholdLabel, m_indexedThresholdSpinBox);
        groupForm->addRow(cacheLabel, m_textCacheSpinBox);
        layout->addWidget(group);
        auto* encodingGroup = new QGroupBox(tr("Encoding"), page);
        auto* encodingForm = new QFormLayout(encodingGroup);
        encodingForm->addRow(encodingLabel, m_fallbackEncodingCombo);
        layout->addWidget(encodingGroup);
    }
}

// ---------------------------------------------------------------------------
void SettingsDialog::buildFileMemoryControls()
{
    QWidget* page = ui->autoReloadIntervalSpinBox
        ? ui->autoReloadIntervalSpinBox->parentWidget() : nullptr;
    if (!page || !page->layout())
        return;

    auto* group = new QGroupBox(tr("File memory"), page);
    auto* groupLayout = new QVBoxLayout(group);
    m_rememberFileStateCheckBox = new QCheckBox(
        tr("Remember bookmarks, markers, position and filters of each file"), group);
    m_rememberFileStateCheckBox->setObjectName(QStringLiteral("rememberFileStateCheckBox"));
    m_rememberFileStateCheckBox->setToolTip(
        tr("A file opened again gets its bookmarks, row highlighters and position back;\n"
           "its filters are offered with one click. The state is kept in the settings\n"
           "folder (memory/) and includes file paths and fragments of bookmarked lines."));
    groupLayout->addWidget(m_rememberFileStateCheckBox);

    auto* row = new QHBoxLayout();
    auto* clearButton = new QPushButton(tr("Clear Memory..."), group);
    clearButton->setObjectName(QStringLiteral("clearFileMemoryButton"));
    clearButton->setToolTip(tr("Forget the saved state of all files, bookmarks included"));
    auto* clearedLabel = new QLabel(group);
    clearedLabel->setObjectName(QStringLiteral("clearFileMemoryLabel"));
    row->addWidget(clearButton);
    row->addWidget(clearedLabel, 1);
    groupLayout->addLayout(row);
    connect(clearButton, &QPushButton::clicked, this, [this, clearedLabel]() {
        const auto answer = QMessageBox::question(this, tr("Clear Memory"),
            tr("Forget the saved bookmarks, markers, positions and filters of all files?\n"
               "Tabs that are open now save their state again when they are closed."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        const int removed = m_fileMemory ? m_fileMemory->clear()
                                         : FileMemoryStore::clearDirectory(AppSettings::fileMemoryDir());
        clearedLabel->setText(tr("Forgot the state of %n file(s).", nullptr, removed));
    });

    // Сессии (R-5): вкладки прошлого сеанса при запуске.
    auto* sessions = new QGroupBox(tr("Sessions"), page);
    auto* sessionsLayout = new QVBoxLayout(sessions);
    m_restoreSessionCheckBox = new QCheckBox(tr("Restore the tabs of the previous session on startup"), sessions);
    m_restoreSessionCheckBox->setObjectName(QStringLiteral("restoreSessionCheckBox"));
    m_restoreSessionCheckBox->setToolTip(
        tr("Off: the start screen offers \"Restore previous session\" instead.\n"
           "The session is saved when DendroLog is closed normally (last-session.json\n"
           "in the settings folder); windows opened with --new-instance or reading\n"
           "standard input neither restore nor save it."));
    sessionsLayout->addWidget(m_restoreSessionCheckBox);

    for (QGroupBox* box : { group, sessions }) {
        if (auto* form = qobject_cast<QFormLayout*>(page->layout()))
            form->addRow(box);
        else
            page->layout()->addWidget(box);
    }
}

// ---------------------------------------------------------------------------
// AI (F-17, шаг 5): подсказчик схем полей. Ключ — в SecretStore, не в ini.
// ---------------------------------------------------------------------------
void SettingsDialog::buildAiTab()
{
    m_aiTab = new QWidget(this);
    m_aiTab->setObjectName(QStringLiteral("aiTab"));
    auto* layout = new QVBoxLayout(m_aiTab);

    m_aiEnabledCheckBox = new QCheckBox(tr("Suggest field schemas with AI (Claude)"), m_aiTab);
    m_aiEnabledCheckBox->setObjectName(QStringLiteral("aiEnabledCheckBox"));
    layout->addWidget(m_aiEnabledCheckBox);

    auto* about = new MutedLabel(
        tr("Find a Schema → Ask AI sends a few dozen sample lines of the open log to Anthropic "
           "(api.anthropic.com) and gets back a line template. Every request is shown to you in full "
           "before it is sent, with values masked by default; nothing is sent when a log is opened "
           "or in the background. Requests are billed to your API key."),
        m_aiTab);
    about->setWordWrap(true);
    layout->addWidget(about);

    auto* form = new QFormLayout();
    m_aiModelCombo = new QComboBox(m_aiTab);
    m_aiModelCombo->setObjectName(QStringLiteral("aiModelCombo"));
    m_aiModelCombo->setEditable(true);
    m_aiModelCombo->addItems({QString::fromLatin1(SchemaProposal::kDefaultModel), QStringLiteral("claude-sonnet-5-5"),
                              QStringLiteral("claude-haiku-5-5")});
    m_aiModelCombo->setToolTip(tr("Model id of the Claude API"));
    form->addRow(tr("Model:"), m_aiModelCombo);

    auto* keyRow = new QHBoxLayout();
    m_aiKeyEdit = new QLineEdit(m_aiTab);
    m_aiKeyEdit->setObjectName(QStringLiteral("aiKeyEdit"));
    m_aiKeyEdit->setEchoMode(QLineEdit::Password);
    m_aiKeyEdit->setPlaceholderText(tr("sk-ant-… (leave empty to keep the current key)"));
    keyRow->addWidget(m_aiKeyEdit, 1);
    m_aiRemoveKeyButton = new QPushButton(tr("Remove Key"), m_aiTab);
    m_aiRemoveKeyButton->setObjectName(QStringLiteral("aiRemoveKeyButton"));
    keyRow->addWidget(m_aiRemoveKeyButton);
    form->addRow(tr("API key:"), keyRow);
    layout->addLayout(form);

    m_aiKeyStatus = new MutedLabel(QString(), m_aiTab);
    m_aiKeyStatus->setObjectName(QStringLiteral("aiKeyStatus"));
    m_aiKeyStatus->setWordWrap(true);
    layout->addWidget(m_aiKeyStatus);
    layout->addStretch(1);

    connect(m_aiRemoveKeyButton, &QPushButton::clicked, this, [this]() {
        SecretStore store = SecretStore::forApplication();
        QString error;
        if (!store.removeKey(&error))
            QMessageBox::warning(this, tr("Settings"), error);
        SecretStore::setSessionKey(QString());
        m_aiKeyEdit->clear();
        updateAiKeyStatus();
        emit AppSettings::instance().settingsChanged();
    });
    ui->tabWidget->addTab(m_aiTab, tr("AI"));
    updateAiKeyStatus();
}

void SettingsDialog::updateAiKeyStatus()
{
    if (!m_aiKeyStatus)
        return;
    const SecretStore store = SecretStore::forApplication();
    SecretStore::Source source = SecretStore::Source::None;
    SecretStore::effectiveKey(store, &source);
    QString text;
    switch (source) {
    case SecretStore::Source::Environment:
        text = tr("The key comes from the ANTHROPIC_API_KEY environment variable.");
        break;
    case SecretStore::Source::Stored:
        text = tr("A key is saved.");
        break;
    case SecretStore::Source::Session:
        text = tr("A key is set for this session only.");
        break;
    case SecretStore::Source::None:
        text = tr("No key is set.");
        break;
    }
    text += QLatin1Char(' ') + store.description();
    m_aiKeyStatus->setText(text);
    m_aiRemoveKeyButton->setEnabled(source == SecretStore::Source::Stored || source == SecretStore::Source::Session);
}

void SettingsDialog::showAiPage()
{
    if (m_aiTab)
        ui->tabWidget->setCurrentWidget(m_aiTab);
}

// ---------------------------------------------------------------------------
void SettingsDialog::resetShortcutsToDefaults()
{
    const ShortcutManager& mgr = ShortcutManager::instance();
    for (const ShortcutRow& row : m_shortcutRows) {
        if (row.edit)
            row.edit->setKeySequence(mgr.defaultSequence(row.id));
    }
}

// ---------------------------------------------------------------------------
void SettingsDialog::loadFromSettings()
{
    const AppSettings& s = AppSettings::instance();

    // --- General tab ---
    ui->extensionsLineEdit->setText(s.scanExtensions().join(QStringLiteral(", ")));

    // --- Font tab ---
    ui->fontFamilyComboBox->setCurrentFont(QFont(s.fontFamily()));
    ui->fontSizeSpinBox->setValue(s.fontSize());
    updateFontPreview();

    // --- Colors tab: working copies are already set in buildColorsTab() ---

    // --- Appearance tab: theme ---
    ui->themeComboBox->setCurrentIndex(
        qMax(0, ui->themeComboBox->findData(int(s.themeMode()))));

    // --- Appearance tab: default word wrap ---
    ui->wordWrapCheckBox->setChecked(s.wordWrap());
    ui->ansiColorsCheckBox->setChecked(s.ansiColors());

    // --- General tab: reload ---
    ui->autoReloadCheckBox->setChecked(s.autoReload());
    ui->autoReloadIntervalSpinBox->setValue(s.autoReloadIntervalSecs());

    // --- Performance (большие файлы) ---
    if (m_indexedThresholdSpinBox)
        m_indexedThresholdSpinBox->setValue(s.indexedThresholdMB());
    if (m_textCacheSpinBox)
        m_textCacheSpinBox->setValue(s.textCacheBudgetMB());
    if (m_rememberFileStateCheckBox)
        m_rememberFileStateCheckBox->setChecked(s.rememberFileState());
    if (m_restoreSessionCheckBox)
        m_restoreSessionCheckBox->setChecked(s.restoreSessionOnStartup());
    if (m_aiEnabledCheckBox) {
        m_aiEnabledCheckBox->setChecked(s.aiEnabled());
        m_aiModelCombo->setCurrentText(s.aiModel());
    }
    if (m_fallbackEncodingCombo) {
        const auto choice = s.fallbackEncodingChoice();
        m_fallbackEncodingCombo->setCurrentIndex(
            qMax(0, m_fallbackEncodingCombo->findData(choice ? int(*choice) : -1)));
    }
}

// ---------------------------------------------------------------------------
void SettingsDialog::updateFontPreview()
{
    QFont previewFont = ui->fontFamilyComboBox->currentFont();
    previewFont.setPointSize(ui->fontSizeSpinBox->value());
    ui->fontPreviewLabel->setFont(previewFont);
}

// ---------------------------------------------------------------------------
void SettingsDialog::applyToSettings()
{
    AppSettings& s = AppSettings::instance();

    // ---- General tab: parse the comma-separated extension string ---------- //
    QStringList extensions;
    for (const QString& part :
         ui->extensionsLineEdit->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString ext = part.trimmed().toLower();
        if (!ext.isEmpty())
            extensions << ext;
    }
    extensions.removeDuplicates();
    s.setScanExtensions(extensions);

    // ---- Font tab --------------------------------------------------------- //
    s.setFontFamily(ui->fontFamilyComboBox->currentFont().family());
    s.setFontSize(ui->fontSizeSpinBox->value());

    // ---- Colors tab: write working copies directly into AppTheme ---------- //
    for (const ColorEntry& ce : std::as_const(m_colorEntries)) {
        if (ce.target)
            *ce.target = ce.value;
    }

    // ---- Appearance tab: theme (применяется сразу, ColorSchemeController) - //
    s.setThemeMode(ThemeMode(ui->themeComboBox->currentData().toInt()));

    // ---- Appearance tab: default word wrap -------------------------------- //
    s.setWordWrap(ui->wordWrapCheckBox->isChecked());
    s.setAnsiColors(ui->ansiColorsCheckBox->isChecked());

    // ---- General tab: reload --------------------------------------------- //
    s.setAutoReload(ui->autoReloadCheckBox->isChecked());
    s.setAutoReloadIntervalSecs(ui->autoReloadIntervalSpinBox->value());

    // ---- Performance (большие файлы) -------------------------------------- //
    if (m_indexedThresholdSpinBox)
        s.setIndexedThresholdMB(m_indexedThresholdSpinBox->value());
    if (m_textCacheSpinBox)
        s.setTextCacheBudgetMB(m_textCacheSpinBox->value());
    if (m_rememberFileStateCheckBox)
        s.setRememberFileState(m_rememberFileStateCheckBox->isChecked());
    if (m_restoreSessionCheckBox)
        s.setRestoreSessionOnStartup(m_restoreSessionCheckBox->isChecked());
    if (m_fallbackEncodingCombo) {
        const int data = m_fallbackEncodingCombo->currentData().toInt();
        s.setFallbackEncodingChoice(data < 0 ? std::nullopt
                                             : std::optional<TextEncoding>(TextEncoding(data)));
    }

    // ---- AI: ключ — в SecretStore (или на этот запуск), не в ini ---------- //
    // Ключ — ДО флага и модели: их сеттеры шлют settingsChanged, и окно
    // «Find a Schema» должно увидеть уже новый ключ.
    if (m_aiEnabledCheckBox) {
        const QString key = m_aiKeyEdit->text().trimmed();
        bool keyChanged = false;
        if (!key.isEmpty()) {
            SecretStore store = SecretStore::forApplication();
            QString error;
            if (!store.isAvailable()) {
                SecretStore::setSessionKey(key);
            } else if (store.writeKey(key, &error)) {
                SecretStore::setSessionKey(QString());
            } else {
                // Сохранённый ключ главнее ключа на этот запуск: прежний
                // убирается, иначе запросы шли бы со старым.
                store.removeKey();
                SecretStore::setSessionKey(key);
                QMessageBox::warning(this, tr("Settings"),
                                     tr("%1 The new key is used for this session only.").arg(error));
            }
            m_aiKeyEdit->clear();
            keyChanged = true;
        }
        const bool enabledBefore = s.aiEnabled();
        const QString modelBefore = s.aiModel();
        s.setAiEnabled(m_aiEnabledCheckBox->isChecked());
        s.setAiModel(m_aiModelCombo->currentText());
        if (keyChanged && enabledBefore == s.aiEnabled() && modelBefore == s.aiModel())
            emit s.settingsChanged();
    }

    // ---- Shortcuts tab ---------------------------------------------------- //
    ShortcutManager& sm = ShortcutManager::instance();
    for (ShortcutRow& row : m_shortcutRows) {
        if (row.edit && row.edit->keySequence() != row.initial) {
            sm.setSequence(row.id, row.edit->keySequence());
            row.initial = row.edit->keySequence();
        }
    }
    sm.save();

    // Persist everything (AppSettings::save() also delegates to AppTheme::save).
    s.save();
}

// ---------------------------------------------------------------------------
void SettingsDialog::accept()
{
    if (!updateShortcutConflicts().isEmpty()) {
        ui->tabWidget->setCurrentWidget(ui->shortcutsTab);
        QMessageBox::warning(this, tr("Settings"),
            tr("Some shortcuts are assigned to more than one command. "
               "Change or clear them before applying the settings."));
        return;
    }
    applyToSettings();
    QDialog::accept();
}
