#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "appsettings.h"
#include "apptheme.h"
#include "shortcutmanager.h"

#include <QColorDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
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

    buildColorsTab();
    buildShortcutsTab();
    buildPerformanceControls();
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
        edit->setKeySequence(mgr.sequence(cmd.id));
        // A single combination is enough for application shortcuts.
        edit->setMaximumSequenceLength(1);
        // Without the clear button a shortcut could not be removed at all:
        // Backspace/Delete are recorded as the new combination.
        edit->setClearButtonEnabled(true);
        connect(edit, &QKeySequenceEdit::keySequenceChanged,
                this, [this]() { updateShortcutConflicts(); });

        m_shortcutRows.append({ cmd.id, edit });
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

    auto* thresholdLabel = new QLabel(tr("Index files larger than:"), page);
    auto* cacheLabel = new QLabel(tr("Indexed text cache:"), page);

    if (auto* form = qobject_cast<QFormLayout*>(page->layout())) {
        form->addRow(thresholdLabel, m_indexedThresholdSpinBox);
        form->addRow(cacheLabel, m_textCacheSpinBox);
    } else if (QLayout* layout = page->layout()) {
        auto* group = new QGroupBox(tr("Large files"), page);
        auto* groupForm = new QFormLayout(group);
        groupForm->addRow(thresholdLabel, m_indexedThresholdSpinBox);
        groupForm->addRow(cacheLabel, m_textCacheSpinBox);
        layout->addWidget(group);
    }
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

    // --- Appearance tab: default word wrap ---
    ui->wordWrapCheckBox->setChecked(s.wordWrap());

    // --- General tab: reload ---
    ui->autoReloadCheckBox->setChecked(s.autoReload());
    ui->autoReloadIntervalSpinBox->setValue(s.autoReloadIntervalSecs());

    // --- Performance (большие файлы) ---
    if (m_indexedThresholdSpinBox)
        m_indexedThresholdSpinBox->setValue(s.indexedThresholdMB());
    if (m_textCacheSpinBox)
        m_textCacheSpinBox->setValue(s.textCacheBudgetMB());
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

    // ---- Appearance tab: default word wrap -------------------------------- //
    s.setWordWrap(ui->wordWrapCheckBox->isChecked());

    // ---- General tab: reload --------------------------------------------- //
    s.setAutoReload(ui->autoReloadCheckBox->isChecked());
    s.setAutoReloadIntervalSecs(ui->autoReloadIntervalSpinBox->value());

    // ---- Performance (большие файлы) -------------------------------------- //
    if (m_indexedThresholdSpinBox)
        s.setIndexedThresholdMB(m_indexedThresholdSpinBox->value());
    if (m_textCacheSpinBox)
        s.setTextCacheBudgetMB(m_textCacheSpinBox->value());

    // ---- Shortcuts tab ---------------------------------------------------- //
    ShortcutManager& sm = ShortcutManager::instance();
    for (const ShortcutRow& row : std::as_const(m_shortcutRows)) {
        if (row.edit)
            sm.setSequence(row.id, row.edit->keySequence());
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
