#include "schemafieldcard.h"

#include "cardframe.h"
#include "schemafieldstrip.h"
#include "schemafit.h"

#include <QButtonGroup>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

QLabel* mutedLabel(QWidget* parent)
{
    // Роль палитры, а не строка стиля: следует за темой сама.
    auto* label = new QLabel(parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setWordWrap(true);
    return label;
}

// Без %n: переводов нет, а «3 value(s)» в интерфейсе — мусор.
QString valuesText(int n, bool different)
{
    if (n > SchemaFit::kDistinctCap)
        return different ? QObject::tr("%1+ different values").arg(SchemaFit::kDistinctCap)
                         : QObject::tr("%1+ values").arg(SchemaFit::kDistinctCap);
    if (n == 1)
        return QObject::tr("1 value");
    return different ? QObject::tr("%1 different values").arg(n) : QObject::tr("%1 values").arg(n);
}

QLabel* titleLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    QFont f = label->font();
    f.setBold(true);
    label->setFont(f);
    return label;
}

} // namespace

SchemaFieldCard::SchemaFieldCard(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);
    root->addStretch(1);

    // ---- Поле не выбрано ------------------------------------------------------
    m_hint = mutedLabel(m_stack);
    m_hint->setObjectName(QStringLiteral("fieldCardHint"));
    m_hint->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_hint->setTextFormat(Qt::RichText);
    m_hint->setContentsMargins(4, 4, 4, 4);
    m_stack->addWidget(m_hint);

    // ---- Поле -----------------------------------------------------------------
    m_card = new CardFrame(m_stack);
    m_card->setObjectName(QStringLiteral("fieldCard"));
    m_card->setAccentBorder(true);
    QVBoxLayout* rows = m_card->rowsLayout();
    rows->setContentsMargins(6, 6, 4, 6);
    rows->setSpacing(6);

    auto* nameRow = new QHBoxLayout();
    nameRow->addWidget(titleLabel(tr("Name"), m_card));
    m_name = new QLineEdit(m_card);
    m_name->setObjectName(QStringLiteral("fieldNameEdit"));
    m_name->setPlaceholderText(tr("Name of the column"));
    m_name->setToolTip(tr("The name of the column: it is shown in Log Fields, Entry Details and in the filter rules"));
    m_name->installEventFilter(this);
    nameRow->addWidget(m_name, 1);
    rows->addLayout(nameRow);

    auto* endsHead = new QHBoxLayout();
    m_endsTitle = titleLabel(tr("Where it ends"), m_card);
    m_endsTitle->setToolTip(tr("A field starts where the previous one ends. Choose where this one ends: next to "
                               "every choice — how many different values the field gets and how many lines of "
                               "the log are read with it."));
    endsHead->addWidget(m_endsTitle);
    m_endsStatus = mutedLabel(m_card);
    m_endsStatus->setObjectName(QStringLiteral("fieldEndsStatus"));
    m_endsStatus->setWordWrap(false);
    endsHead->addWidget(m_endsStatus, 1, Qt::AlignRight);
    rows->addLayout(endsHead);
    m_endsBox = new QWidget(m_card);
    m_endsLayout = new QGridLayout(m_endsBox);
    m_endsLayout->setContentsMargins(0, 0, 0, 0);
    m_endsLayout->setHorizontalSpacing(8);
    m_endsLayout->setVerticalSpacing(1);
    m_endsLayout->setColumnStretch(0, 1);
    rows->addWidget(m_endsBox);
    m_endsGroup = new QButtonGroup(this);
    connect(m_endsGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_field.column >= 0 && m_optionsReady)
            emit endRequested(m_field.column, id);
    });

    m_valuesTitle = titleLabel(tr("Values in the lines"), m_card);
    rows->addWidget(m_valuesTitle);
    m_values = mutedLabel(m_card);
    m_values->setObjectName(QStringLiteral("fieldValues"));
    m_values->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rows->addWidget(m_values);

    auto* actions = new QGridLayout();
    actions->setContentsMargins(0, 2, 0, 0);
    m_hide = new QPushButton(tr("Hide"), m_card);
    m_hide->setObjectName(QStringLiteral("fieldHideBtn"));
    m_hide->setToolTip(tr("Keep reading this part of the line, but do not show it as a column"));
    m_merge = new QPushButton(tr("Merge with Next"), m_card);
    m_merge->setObjectName(QStringLiteral("fieldMergeBtn"));
    m_merge->setToolTip(tr("Join this field and the next one into one field"));
    m_remove = new QPushButton(tr("Remove"), m_card);
    m_remove->setObjectName(QStringLiteral("fieldRemoveBtn"));
    m_remove->setToolTip(tr("Remove the field: its text goes to the neighbouring field"));
    m_addValue = new QPushButton(tr("Add Value…"), m_card);
    m_addValue->setObjectName(QStringLiteral("fieldAddValueBtn"));
    m_addValue->setToolTip(tr("Add one more value to the list of values of this field"));
    actions->addWidget(m_hide, 0, 0);
    actions->addWidget(m_merge, 0, 1);
    actions->addWidget(m_remove, 1, 0);
    actions->addWidget(m_addValue, 1, 1);
    rows->addLayout(actions);
    m_stack->addWidget(m_card);

    // ---- Часть строки без колонки ---------------------------------------------
    m_hiddenCard = new CardFrame(m_stack);
    m_hiddenCard->setObjectName(QStringLiteral("hiddenFieldCard"));
    QVBoxLayout* hiddenRows = m_hiddenCard->rowsLayout();
    hiddenRows->setContentsMargins(6, 6, 4, 6);
    hiddenRows->setSpacing(6);
    hiddenRows->addWidget(titleLabel(tr("Hidden part of the line"), m_hiddenCard));
    m_hiddenText = mutedLabel(m_hiddenCard);
    hiddenRows->addWidget(m_hiddenText);
    m_show = new QPushButton(tr("Show as a Column"), m_hiddenCard);
    m_show->setObjectName(QStringLiteral("fieldShowBtn"));
    hiddenRows->addWidget(m_show, 0, Qt::AlignLeft);
    m_stack->addWidget(m_hiddenCard);

    connect(m_name, &QLineEdit::editingFinished, this, &SchemaFieldCard::commitName);
    connect(m_hide, &QPushButton::clicked, this, [this]() {
        if (m_field.column >= 0)
            emit hideRequested(m_field.column);
    });
    connect(m_merge, &QPushButton::clicked, this, [this]() {
        if (m_field.column >= 0)
            emit mergeRequested(m_field.column);
    });
    connect(m_remove, &QPushButton::clicked, this, [this]() {
        if (m_field.column >= 0)
            emit removeRequested(m_field.column);
    });
    connect(m_addValue, &QPushButton::clicked, this, [this]() {
        if (m_field.column >= 0)
            emit addValueRequested(m_field.column);
    });
    connect(m_show, &QPushButton::clicked, this, [this]() {
        if (m_field.isHidden())
            emit showRequested(m_field.piece);
    });

    setField(Field());
}

QString SchemaFieldCard::resultText(const SchemaEdit::EndOption& option)
{
    if (!option.available)
        return option.reason;
    return tr("%1 · %2%").arg(valuesText(option.distinct(), false), SchemaFit::percent(option.fit.coverage()));
}

void SchemaFieldCard::setHint(const QString& text)
{
    m_hint->setText(text);
}

void SchemaFieldCard::setField(const Field& field)
{
    const bool sameColumn = field.column == m_field.column && field.column >= 0;
    m_field = field;
    if (!field.isValid()) {
        m_stack->setCurrentWidget(m_hint);
        clearOptions();
        return;
    }
    if (field.isHidden()) {
        m_hiddenText->setText(tr("This part of the line (%1) is read, but it is not a column: it is not shown "
                                 "in Log Fields and cannot be used in filters.")
                                  .arg(field.caption));
        m_hiddenText->setToolTip(field.templatePart);
        m_stack->setCurrentWidget(m_hiddenCard);
        clearOptions();
        updateActions();
        return;
    }
    if (!sameColumn) {
        clearOptions();
        m_values->setText(QString());
    }
    // Имя, которое пользователь сейчас набирает, не затирается пересчётом.
    if (!m_name->hasFocus() || !sameColumn || !m_name->isModified())
        m_name->setText(field.name);
    m_name->setModified(false);
    m_card->setToolTip(QString());
    m_endsTitle->setText(tr("Where it ends"));
    m_stack->setCurrentWidget(m_card);
    applyAccent();
    updateActions();
}

void SchemaFieldCard::clearOptions()
{
    const QList<QAbstractButton*> buttons = m_endsGroup->buttons();
    for (QAbstractButton* b : buttons)
        m_endsGroup->removeButton(b);
    while (QLayoutItem* item = m_endsLayout->takeAt(0)) {
        // Не delete: список перестраивается и из обработчика щелчка по
        // самому переключателю. Имя снимается сразу — findChild не найдёт
        // уходящий виджет вместо нового.
        if (QWidget* widget = item->widget()) {
            widget->setObjectName(QString());
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_optionsReady = false;
    m_endsStatus->setText(QString());
}

void SchemaFieldCard::setEndOptions(const QVector<SchemaEdit::EndOption>& options, bool ready)
{
    if (!m_field.isValid() || m_field.isHidden())
        return;
    if (!ready) {
        // Прежние пункты остаются на месте недоступными: список не мигает на
        // каждой правке.
        m_optionsReady = false;
        m_endsStatus->setText(tr("checking on the lines…"));
        m_endsBox->setEnabled(false);
        return;
    }
    clearOptions();
    m_optionsReady = true;
    m_endsBox->setEnabled(true);
    int row = 0;
    for (int i = 0; i < options.size(); ++i) {
        const SchemaEdit::EndOption& o = options[i];
        // Недоступные — в меню колонки, с причиной; пункт, при котором у поля
        // не остаётся ни одного значения, — не выбор.
        if (!o.current && (!o.available || o.distinct() == 0))
            continue;
        auto* radio = new QRadioButton(o.label, m_endsBox);
        radio->setObjectName(QStringLiteral("fieldEnd%1").arg(i));
        radio->setChecked(o.current);
        radio->setToolTip(tr("In the template: %1").arg(o.kind));
        m_endsGroup->addButton(radio, i);
        auto* result = mutedLabel(m_endsBox);
        result->setObjectName(QStringLiteral("fieldEndResult%1").arg(i));
        result->setWordWrap(false);
        result->setText(resultText(o));
        result->setToolTip(tr("With this choice the field has %1 in the lines of the log, and %2% of the lines "
                              "are read by the schema.")
                               .arg(valuesText(o.distinct(), true), SchemaFit::percent(o.fit.coverage())));
        m_endsLayout->addWidget(radio, row, 0);
        m_endsLayout->addWidget(result, row, 1, Qt::AlignRight | Qt::AlignVCenter);
        ++row;
    }
    m_endsStatus->setText(row == 0 ? tr("no choices for this field") : tr("values · lines read"));
    m_endsStatus->setToolTip(tr("For every choice: how many different values the field gets, and what share "
                                "of the lines of the log the schema reads with it"));
    updateGeometry();
}

void SchemaFieldCard::setValues(int distinct, const QVector<QPair<QString, int>>& top, bool ready)
{
    if (!m_field.isValid() || m_field.isHidden())
        return;
    if (!ready) {
        if (m_values->text().isEmpty())
            m_values->setText(tr("reading the lines…"));
        return;
    }
    if (distinct <= 0 || top.isEmpty()) {
        m_values->setText(tr("No values in the sample lines."));
        return;
    }
    QStringList parts;
    for (const auto& [value, count] : top) {
        QString shown = value.size() > 40 ? value.left(39) + QChar(0x2026) : value;
        parts << tr("%1 — %2").arg(shown.toHtmlEscaped()).arg(count);
    }
    QString text = tr("%1 in the sample lines:").arg(valuesText(distinct, true));
    text += QStringLiteral("<br>") + parts.join(QStringLiteral("<br>"));
    if (distinct > top.size())
        text += QStringLiteral("<br>…");
    m_values->setTextFormat(Qt::RichText);
    m_values->setText(text);
}

void SchemaFieldCard::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    updateActions();
}

void SchemaFieldCard::updateActions()
{
    const bool on = !m_busy && m_field.column >= 0;
    m_hide->setEnabled(on);
    m_remove->setEnabled(on);
    m_merge->setEnabled(on && m_field.canMerge);
    m_addValue->setVisible(m_field.isList);
    m_addValue->setEnabled(on && m_field.isList);
    m_show->setEnabled(!m_busy && m_field.isHidden());
}

void SchemaFieldCard::commitName()
{
    if (m_field.column < 0)
        return;
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        m_name->setText(m_field.name);
        return;
    }
    if (name == m_field.name)
        return;
    // Имя принято к исполнению: дальше карточку наполняет окно (новое имя или,
    // при отказе, прежнее).
    m_name->setModified(false);
    emit renameRequested(m_field.column, name);
}

void SchemaFieldCard::applyAccent()
{
    if (m_field.column >= 0) {
        const QColor fill = SchemaFieldStrip::fieldColor(m_field.column, palette());
        m_card->setAccentColor(CardFrame::mixedColor(fill, palette().color(QPalette::Text), 0.35));
    }
}

void SchemaFieldCard::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyAccent();
}

bool SchemaFieldCard::eventFilter(QObject* watched, QEvent* event)
{
    // Esc в поле имени — прежнее имя, без переименования.
    if (watched == m_name && event->type() == QEvent::KeyPress
        && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && m_name->text() != m_field.name) {
        m_name->setText(m_field.name);
        m_name->setModified(false);
        return true;
    }
    return QWidget::eventFilter(watched, event);
}
