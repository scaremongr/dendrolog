#include "markerpanelwidget.h"
#include "highlightpalette.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

// ===========================================================================
// MarkerCard
// ===========================================================================

MarkerCard::MarkerCard(const HighlightPattern& pattern, QWidget* parent)
    : CardFrame(parent)
    , m_color(pattern.color.isValid() ? pattern.color : HighlightPalette::colorAt(0))
{
    QVBoxLayout* rows = rowsLayout();

    // ---- Строка 1: вкл/выкл, ключевое слово, действия ------------------ //
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(4);
    rows->addLayout(headerRow);

    m_enabledCheckBox = new QCheckBox(this);
    m_enabledCheckBox->setChecked(pattern.enabled);
    m_enabledCheckBox->setToolTip(tr("Enable/disable this marker without deleting it"));
    headerRow->addWidget(m_enabledCheckBox);

    m_textEdit = new QLineEdit(this);
    m_textEdit->setPlaceholderText(tr("Keyword to mark..."));
    m_textEdit->setText(pattern.text);
    m_textEdit->setClearButtonEnabled(true);
    headerRow->addWidget(m_textEdit, /*stretch=*/1);

    m_gearButton = makeToolButton(QStringLiteral("⚙"),
        tr("Advanced: case sensitivity and regular-expression mode."));
    m_gearButton->setCheckable(true);
    headerRow->addWidget(m_gearButton);

    m_colorButton = makeToolButton(QString(), tr("Row colour (click to change)"));
    m_colorButton->setFixedSize(22, 22);
    m_colorButton->setAutoRaise(false);
    headerRow->addWidget(m_colorButton);

    m_removeButton = makeToolButton(QStringLiteral("✕"), tr("Remove this marker"));
    headerRow->addWidget(m_removeButton);

    // ---- Строка 2 (⚙): опции поиска ------------------------------------ //
    m_advancedRow = new QWidget(this);
    auto* advLayout = new QHBoxLayout(m_advancedRow);
    advLayout->setContentsMargins(4, 0, 0, 0);
    advLayout->setSpacing(10);

    m_caseSensitiveCheckBox = new QCheckBox(tr("Case sensitive"), m_advancedRow);
    m_caseSensitiveCheckBox->setChecked(pattern.caseSensitivity == Qt::CaseSensitive);
    advLayout->addWidget(m_caseSensitiveCheckBox);

    m_regexCheckBox = new QCheckBox(tr("Regular expression"), m_advancedRow);
    m_regexCheckBox->setChecked(pattern.isRegex);
    m_regexCheckBox->setToolTip(tr("Treat the keyword as a regular expression.\n"
                                   "An invalid expression marks nothing."));
    advLayout->addWidget(m_regexCheckBox);
    advLayout->addStretch(1);

    m_advancedRow->setVisible(false);
    rows->addWidget(m_advancedRow);

    // ---- Сигналы -------------------------------------------------------- //
    // Применение явное (Apply/Reset на панели), поэтому правки контролов
    // никуда не репортятся — панель читает состояние карточек по Apply.
    connect(m_textEdit, &QLineEdit::returnPressed, this, &MarkerCard::applyShortcutPressed);
    connect(m_caseSensitiveCheckBox, &QCheckBox::toggled, this, [this]() { updateGearHighlight(); });
    connect(m_regexCheckBox, &QCheckBox::toggled, this, [this]() { updateGearHighlight(); });
    connect(m_gearButton, &QToolButton::toggled, this, [this](bool on) {
        m_advancedRow->setVisible(on);
    });
    connect(m_colorButton, &QToolButton::clicked, this, &MarkerCard::chooseColor);
    connect(m_removeButton, &QToolButton::clicked, this, &MarkerCard::removeRequested);

    setAccentColor(m_color);
    updateColorButton();
    updateGearHighlight();
}

HighlightPattern MarkerCard::pattern() const
{
    HighlightPattern p;
    p.text            = m_textEdit->text();
    p.color           = m_color;
    p.enabled         = m_enabledCheckBox->isChecked();
    p.isRegex         = m_regexCheckBox->isChecked();
    p.caseSensitivity = m_caseSensitiveCheckBox->isChecked() ? Qt::CaseSensitive
                                                             : Qt::CaseInsensitive;
    return p;
}

void MarkerCard::chooseColor()
{
    const QColor chosen = QColorDialog::getColor(m_color, this, tr("Row colour"));
    if (chosen.isValid()) {
        m_color = chosen;
        setAccentColor(m_color);
        updateColorButton();
    }
}

void MarkerCard::updateColorButton()
{
    m_colorButton->setStyleSheet(
        QStringLiteral("QToolButton { background-color: %1; border: 1px solid palette(mid); border-radius: 3px; }")
            .arg(m_color.name()));
}

void MarkerCard::updateGearHighlight()
{
    const bool hasContent = m_caseSensitiveCheckBox->isChecked() || m_regexCheckBox->isChecked();
    tintToolButton(m_gearButton, hasContent);
    m_gearButton->setToolTip(hasContent
        ? tr("Advanced settings contain values — click to view.")
        : tr("Advanced: case sensitivity and regular-expression mode."));
}

// ===========================================================================
// MarkerPanelWidget
// ===========================================================================

MarkerPanelWidget::MarkerPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(5, 5, 5, 5);
    rootLayout->setSpacing(5);

    auto* controlsLayout = new QHBoxLayout();
    m_addButton = new QPushButton(tr("+ Add marker"), this);
    m_addButton->setToolTip(tr("Mark rows containing a keyword with a colour.\n"
                               "Rows are NOT filtered out — the rest of the log stays visible."));
    connect(m_addButton, &QPushButton::clicked, this, &MarkerPanelWidget::onAddMarkerClicked);
    controlsLayout->addWidget(m_addButton);

    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setToolTip(tr("Apply the markers to the CURRENT document"));
    m_applyButton->setDefault(true);
    connect(m_applyButton, &QPushButton::clicked, this, &MarkerPanelWidget::applyRequested);
    controlsLayout->addWidget(m_applyButton);

    m_resetButton = new QPushButton(tr("Reset"), this);
    m_resetButton->setToolTip(tr("Remove all row markers from the CURRENT document.\n"
                                 "The markers stay in the panel for re-applying."));
    connect(m_resetButton, &QPushButton::clicked, this, &MarkerPanelWidget::resetRequested);
    controlsLayout->addWidget(m_resetButton);

    controlsLayout->addStretch();
    rootLayout->addLayout(controlsLayout);

    m_rowsLayout = new QVBoxLayout();
    m_rowsLayout->setSpacing(4);
    rootLayout->addLayout(m_rowsLayout);
    rootLayout->addStretch(1);
}

QVector<HighlightPattern> MarkerPanelWidget::markers() const
{
    QVector<HighlightPattern> result;
    result.reserve(m_cards.size());
    for (const auto* card : m_cards)
        result.append(card->pattern());
    return result;
}

void MarkerPanelWidget::setMarkers(const QVector<HighlightPattern>& markers)
{
    while (!m_cards.isEmpty())
        removeCard(m_cards.last());
    for (const auto& marker : markers)
        addMarker(marker);
}

void MarkerPanelWidget::onAddMarkerClicked()
{
    HighlightPattern pattern;
    pattern.color = nextFreeColor();
    addMarker(pattern);
}

void MarkerPanelWidget::addMarker(const HighlightPattern& pattern)
{
    auto* card = new MarkerCard(pattern, this);

    connect(card, &MarkerCard::removeRequested, this, [this, card]() {
        removeCard(card);
    });
    connect(card, &MarkerCard::applyShortcutPressed,
            this, &MarkerPanelWidget::applyRequested);

    m_rowsLayout->addWidget(card);
    m_cards.append(card);
}

void MarkerPanelWidget::removeCard(MarkerCard* card)
{
    m_cards.removeOne(card);
    m_rowsLayout->removeWidget(card);
    card->deleteLater();
}

QColor MarkerPanelWidget::nextFreeColor() const
{
    for (int i = 0; i < 10; ++i) {
        const QColor candidate = HighlightPalette::colorAt(i);
        bool used = false;
        for (const auto* card : m_cards) {
            if (card->pattern().color == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
    return HighlightPalette::colorAt(m_cards.size());
}
