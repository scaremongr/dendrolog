#include "settingsdialog.h"
#include "ui_settingsdialog.h"
#include "appsettings.h"
#include "apptheme.h"
#include "shortcutmanager.h"

#include <QColorDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
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
    mainVLayout->setSpacing(8);
    mainVLayout->setContentsMargins(6, 6, 6, 6);

    // Keep a flat index into m_colorEntries as we iterate.
    int entryIdx = 0;
    for (auto& group : groups) {
        auto* groupBox = new QGroupBox(group.title, content);
        auto* formLayout = new QFormLayout(groupBox);
        formLayout->setHorizontalSpacing(12);
        formLayout->setVerticalSpacing(4);

        for (int i = 0; i < group.entries.size(); ++i, ++entryIdx) {
            ColorEntry& ce = m_colorEntries[entryIdx];

            auto* btn = new QPushButton(groupBox);
            btn->setFixedSize(72, 22);
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
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    for (const auto& cmd : mgr.commands()) {
        auto* edit = new QKeySequenceEdit(content);
        edit->setKeySequence(mgr.sequence(cmd.id));
        // A single combination is enough for application shortcuts.
        edit->setMaximumSequenceLength(1);

        m_shortcutRows.append({ cmd.id, edit });
        form->addRow(new QLabel(cmd.label, content), edit);
    }

    ui->shortcutsScrollArea->setWidget(content);

    connect(ui->resetShortcutsButton, &QPushButton::clicked,
            this, &SettingsDialog::resetShortcutsToDefaults);
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

    // --- View tab ---
    ui->wordWrapCheckBox->setChecked(s.wordWrap());

    // --- General tab: reload ---
    ui->autoReloadCheckBox->setChecked(s.autoReload());
    ui->autoReloadIntervalSpinBox->setValue(s.autoReloadIntervalSecs());
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

    // ---- View tab --------------------------------------------------------- //
    s.setWordWrap(ui->wordWrapCheckBox->isChecked());

    // ---- General tab: reload --------------------------------------------- //
    s.setAutoReload(ui->autoReloadCheckBox->isChecked());
    s.setAutoReloadIntervalSecs(ui->autoReloadIntervalSpinBox->value());

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
    applyToSettings();
    QDialog::accept();
}
