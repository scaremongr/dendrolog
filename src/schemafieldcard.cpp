#include "schemafieldcard.h"

#include "cardframe.h"
#include "schemafieldstrip.h"
#include "schemafit.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QToolButton>
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

    // Свой разделитель: любой текст, на котором поле кончается.
    auto* textRow = new QHBoxLayout();
    auto* textLabel = new QLabel(tr("or at the text"), m_card);
    textRow->addWidget(textLabel);
    m_textEnd = new QComboBox(m_card);
    m_textEnd->setObjectName(QStringLiteral("fieldTextEndEdit"));
    m_textEnd->setEditable(true);
    m_textEnd->setInsertPolicy(QComboBox::NoInsert);
    m_textEnd->lineEdit()->setPlaceholderText(tr("any text, e.g. ~~ or =>"));
    m_textEnd->setToolTip(tr("Type the text the field ends at — any characters. The field ends there, and what "
                             "follows it becomes a new field. Spaces you type around it count: “ - ” is a dash "
                             "with spaces, “-” is any dash."));
    textLabel->setBuddy(m_textEnd);
    textRow->addWidget(m_textEnd, 1);
    m_textEndApply = new QPushButton(tr("Apply"), m_card);
    m_textEndApply->setObjectName(QStringLiteral("fieldTextEndBtn"));
    textRow->addWidget(m_textEndApply);
    rows->addLayout(textRow);
    m_textEndHint = mutedLabel(m_card);
    m_textEndHint->setObjectName(QStringLiteral("fieldTextEndHint"));
    m_textEndHint->hide();
    rows->addWidget(m_textEndHint);

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
    m_split = new QPushButton(tr("Split…"), m_card);
    m_split->setObjectName(QStringLiteral("fieldSplitBtn"));
    m_split->setToolTip(tr("Split this field into two — this is how a new field is added"));
    actions->addWidget(m_split, 0, 0);
    actions->addWidget(m_merge, 0, 1);
    actions->addWidget(m_hide, 1, 0);
    actions->addWidget(m_remove, 1, 1);
    actions->addWidget(m_addValue, 2, 0);
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
    connect(m_split, &QPushButton::clicked, this, [this]() {
        if (m_field.column >= 0)
            emit splitRequested(m_field.column);
    });
    connect(m_textEnd, &QComboBox::editTextChanged, this, [this](const QString& text) {
        m_textEndApply->setEnabled(!m_busy && !text.trimmed().isEmpty());
        if (text.trimmed().isEmpty())
            setTextEndHint(QString());
        if (m_field.column >= 0)
            emit textEndEdited(m_field.column, text);
    });
    connect(m_textEnd->lineEdit(), &QLineEdit::returnPressed, this, &SchemaFieldCard::requestTextEnd);
    connect(m_textEndApply, &QPushButton::clicked, this, &SchemaFieldCard::requestTextEnd);
    m_textEndApply->setEnabled(false);

    // ---- Разбить поле на два ------------------------------------------------------
    m_splitCard = new CardFrame(m_stack);
    m_splitCard->setObjectName(QStringLiteral("splitCard"));
    m_splitCard->setAccentBorder(true);
    QVBoxLayout* splitRows = m_splitCard->rowsLayout();
    splitRows->setContentsMargins(6, 6, 4, 6);
    splitRows->setSpacing(6);
    m_splitTitle = titleLabel(QString(), m_splitCard);
    m_splitTitle->setObjectName(QStringLiteral("splitTitle"));
    m_splitTitle->setWordWrap(true);
    splitRows->addWidget(m_splitTitle);
    auto* namesRow = new QHBoxLayout();
    m_splitFirst = new QLineEdit(m_splitCard);
    m_splitFirst->setObjectName(QStringLiteral("splitFirstName"));
    m_splitFirst->setToolTip(tr("The name of the first part"));
    m_splitSwap = new QToolButton(m_splitCard);
    m_splitSwap->setObjectName(QStringLiteral("splitSwapNames"));
    m_splitSwap->setText(QString(QChar(0x21C4)));   // ⇄
    m_splitSwap->setAutoRaise(true);
    m_splitSwap->setToolTip(tr("Swap the names of the two parts"));
    m_splitSecond = new QLineEdit(m_splitCard);
    m_splitSecond->setObjectName(QStringLiteral("splitSecondName"));
    m_splitSecond->setToolTip(tr("The name of the second part"));
    namesRow->addWidget(m_splitFirst, 1);
    namesRow->addWidget(m_splitSwap);
    namesRow->addWidget(m_splitSecond, 1);
    splitRows->addLayout(namesRow);
    auto* splitHead = new QHBoxLayout();
    splitHead->addWidget(titleLabel(tr("Where the first part ends"), m_splitCard));
    m_splitStatus = mutedLabel(m_splitCard);
    m_splitStatus->setObjectName(QStringLiteral("splitStatus"));
    m_splitStatus->setWordWrap(false);
    splitHead->addWidget(m_splitStatus, 1, Qt::AlignRight);
    splitRows->addLayout(splitHead);
    m_splitBox = new QWidget(m_splitCard);
    m_splitLayout = new QGridLayout(m_splitBox);
    m_splitLayout->setContentsMargins(0, 0, 0, 0);
    m_splitLayout->setHorizontalSpacing(8);
    m_splitLayout->setVerticalSpacing(1);
    m_splitLayout->setColumnStretch(0, 1);
    splitRows->addWidget(m_splitBox);
    m_splitGroup = new QButtonGroup(this);
    auto* splitTextRow = new QHBoxLayout();
    m_splitTextRadio = new QRadioButton(tr("at the text"), m_splitCard);
    m_splitTextRadio->setObjectName(QStringLiteral("splitOptionText"));
    m_splitGroup->addButton(m_splitTextRadio, kTextOptionId);
    splitTextRow->addWidget(m_splitTextRadio);
    m_splitText = new QComboBox(m_splitCard);
    m_splitText->setObjectName(QStringLiteral("splitTextEdit"));
    m_splitText->setEditable(true);
    m_splitText->setInsertPolicy(QComboBox::NoInsert);
    m_splitText->lineEdit()->setPlaceholderText(tr("any text, e.g. ~~ or =>"));
    m_splitText->setToolTip(tr("Type the text that separates the two parts — any characters. Spaces you type "
                               "around it count."));
    splitTextRow->addWidget(m_splitText, 1);
    splitRows->addLayout(splitTextRow);
    m_splitTextResult = mutedLabel(m_splitCard);
    m_splitTextResult->setObjectName(QStringLiteral("splitResultText"));
    m_splitTextResult->hide();
    splitRows->addWidget(m_splitTextResult);
    m_splitPreview = new QLabel(m_splitCard);
    m_splitPreview->setObjectName(QStringLiteral("splitPreview"));
    m_splitPreview->setTextFormat(Qt::RichText);
    m_splitPreview->setWordWrap(true);
    splitRows->addWidget(m_splitPreview);
    auto* splitButtons = new QHBoxLayout();
    m_splitApply = new QPushButton(tr("Split"), m_splitCard);
    m_splitApply->setObjectName(QStringLiteral("splitApplyBtn"));
    m_splitCancel = new QPushButton(tr("Cancel"), m_splitCard);
    m_splitCancel->setObjectName(QStringLiteral("splitCancelBtn"));
    splitButtons->addWidget(m_splitApply);
    splitButtons->addWidget(m_splitCancel);
    splitRows->addLayout(splitButtons);
    m_stack->addWidget(m_splitCard);

    connect(m_splitGroup, &QButtonGroup::idClicked, this, [this](int) { updateSplitSelection(); });
    connect(m_splitText, &QComboBox::editTextChanged, this, [this](const QString& text) {
        // Набор текста — это и выбор пункта «at the text».
        if (!text.trimmed().isEmpty())
            m_splitTextRadio->setChecked(true);
        m_splitTextReady = false;
        m_splitTextOption = SchemaEdit::SplitOption();
        m_splitTextResult->setVisible(!text.trimmed().isEmpty());
        m_splitTextResult->setText(tr("checking on the lines…"));
        updateSplitSelection();
        emit splitTextEdited(text);
    });
    connect(m_splitFirst, &QLineEdit::textEdited, this, [this]() { m_splitNamesTouched = true; });
    connect(m_splitSecond, &QLineEdit::textEdited, this, [this]() { m_splitNamesTouched = true; });
    connect(m_splitSwap, &QToolButton::clicked, this, [this]() {
        const QString first = m_splitFirst->text();
        m_splitFirst->setText(m_splitSecond->text());
        m_splitSecond->setText(first);
        m_splitNamesTouched = true;
    });
    connect(m_splitApply, &QPushButton::clicked, this, [this]() {
        if (const SchemaEdit::SplitOption* option = selectedSplit())
            emit splitConfirmed(*option, m_splitFirst->text().trimmed(), m_splitSecond->text().trimmed());
    });
    connect(m_splitCancel, &QPushButton::clicked, this, &SchemaFieldCard::splitCancelled);

    // ---- Текст между полями --------------------------------------------------------
    m_literalCard = new CardFrame(m_stack);
    m_literalCard->setObjectName(QStringLiteral("literalCard"));
    QVBoxLayout* literalRows = m_literalCard->rowsLayout();
    literalRows->setContentsMargins(6, 6, 4, 6);
    literalRows->setSpacing(6);
    literalRows->addWidget(titleLabel(tr("Text between the fields"), m_literalCard));
    m_literalInfo = mutedLabel(m_literalCard);
    m_literalInfo->setObjectName(QStringLiteral("literalInfo"));
    literalRows->addWidget(m_literalInfo);
    m_literalEdit = new QLineEdit(m_literalCard);
    m_literalEdit->setObjectName(QStringLiteral("literalEdit"));
    m_literalEdit->setPlaceholderText(tr("the text between the fields"));
    literalRows->addWidget(m_literalEdit);
    auto* literalButtons = new QHBoxLayout();
    m_literalApply = new QPushButton(tr("Apply"), m_literalCard);
    m_literalApply->setObjectName(QStringLiteral("literalApplyBtn"));
    m_literalRemove = new QPushButton(tr("Remove the Text"), m_literalCard);
    m_literalRemove->setObjectName(QStringLiteral("literalRemoveBtn"));
    m_literalRemove->setToolTip(tr("The fields will be separated by spaces only"));
    literalButtons->addWidget(m_literalApply);
    literalButtons->addWidget(m_literalRemove);
    literalRows->addLayout(literalButtons);
    m_stack->addWidget(m_literalCard);
    const auto applyLiteral = [this]() {
        if (m_literalPiece >= 0 && !m_literalEdit->text().trimmed().isEmpty()) {
            m_literalEdit->setModified(false);
            emit literalChangeRequested(m_literalPiece, m_literalEdit->text());
        }
    };
    connect(m_literalApply, &QPushButton::clicked, this, applyLiteral);
    connect(m_literalEdit, &QLineEdit::returnPressed, this, applyLiteral);
    connect(m_literalRemove, &QPushButton::clicked, this, [this]() {
        if (m_literalPiece >= 0)
            emit literalChangeRequested(m_literalPiece, QString());
    });

    for (int i = 0; i < m_stack->count(); ++i)
        m_pagePolicies.append(m_stack->widget(i)->sizePolicy());
    setField(Field());
}

void SchemaFieldCard::showPage(QWidget* page)
{
    // Высоту стопки задаёт только показанная страница: иначе короткая карточка
    // растягивалась бы до самой высокой.
    for (int i = 0; i < m_stack->count() && i < m_pagePolicies.size(); ++i) {
        QWidget* w = m_stack->widget(i);
        w->setSizePolicy(w == page ? m_pagePolicies[i] : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
    }
    m_stack->setCurrentWidget(page);
    m_stack->updateGeometry();
    updateGeometry();
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
        showPage(m_hint);
        clearOptions();
        return;
    }
    if (field.isHidden()) {
        m_hiddenText->setText(tr("This part of the line (%1) is read, but it is not a column: it is not shown "
                                 "in Log Fields and cannot be used in filters.")
                                  .arg(field.caption));
        m_hiddenText->setToolTip(field.templatePart);
        showPage(m_hiddenCard);
        clearOptions();
        updateActions();
        return;
    }
    if (!sameColumn) {
        clearOptions();
        m_values->setText(QString());
        const QSignalBlocker block(m_textEnd);
        m_textEnd->clear();
        m_textEnd->clearEditText();
        m_textEndApply->setEnabled(false);
        setTextEndHint(QString());
    }
    // Имя, которое пользователь сейчас набирает, не затирается пересчётом.
    if (!m_name->hasFocus() || !sameColumn || !m_name->isModified())
        m_name->setText(field.name);
    m_name->setModified(false);
    m_card->setToolTip(QString());
    m_endsTitle->setText(tr("Where it ends"));
    showPage(m_card);
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
    // Все значения разные (свободный текст) — хватит трёх примеров.
    const int limit = top.first().second <= 1 ? 3 : int(top.size());
    for (const auto& [value, count] : top) {
        if (parts.size() >= limit)
            break;
        QString shown = value.size() > 30 ? value.left(29) + QChar(0x2026) : value;
        parts << tr("%1 — %2").arg(shown.toHtmlEscaped()).arg(count);
    }
    QString text = tr("%1 in the sample lines:").arg(valuesText(distinct, true));
    text += QStringLiteral("<br>") + parts.join(QStringLiteral("<br>"));
    if (distinct > parts.size())
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
    m_split->setEnabled(on);
    m_textEnd->setEnabled(on);
    m_textEndApply->setEnabled(on && !m_textEnd->currentText().trimmed().isEmpty());
    m_literalApply->setEnabled(!m_busy);
    m_literalRemove->setEnabled(!m_busy);
    if (isSplitShown())
        updateSplitSelection();
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
    if (m_splitField.column >= 0) {
        const QColor fill = SchemaFieldStrip::fieldColor(m_splitField.column, palette());
        m_splitCard->setAccentColor(CardFrame::mixedColor(fill, palette().color(QPalette::Text), 0.35));
    }
}

void SchemaFieldCard::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyAccent();
}

bool SchemaFieldCard::event(QEvent* event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::LayoutRequest)
        emit neededWidthChanged();
    return handled;
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

// ---------------------------------------------------------------------------
// «or at the text» страницы поля
// ---------------------------------------------------------------------------

void SchemaFieldCard::setTextEndSuggestions(const QStringList& delimiters)
{
    QStringList current;
    for (int i = 0; i < m_textEnd->count(); ++i)
        current << m_textEnd->itemText(i);
    if (current == delimiters)
        return;
    const QString typed = m_textEnd->currentText();
    const QSignalBlocker block(m_textEnd);
    m_textEnd->clear();
    m_textEnd->addItems(delimiters);
    m_textEnd->setCurrentIndex(-1);
    m_textEnd->setEditText(typed);
}

void SchemaFieldCard::setTextEndHint(const QString& text)
{
    m_textEndHint->setText(text);
    m_textEndHint->setVisible(!text.isEmpty());
}

void SchemaFieldCard::requestTextEnd()
{
    const QString text = m_textEnd->currentText();
    if (m_field.column < 0 || m_busy || text.trimmed().isEmpty())
        return;
    emit textEndRequested(m_field.column, text);
}

// ---------------------------------------------------------------------------
// Разбить поле на два
// ---------------------------------------------------------------------------

bool SchemaFieldCard::isSplitShown() const
{
    return m_stack->currentWidget() == m_splitCard;
}

void SchemaFieldCard::showSplit(const Field& field, const QString& newName)
{
    const bool same = isSplitShown() && m_splitField.column == field.column && m_splitField.name == field.name;
    m_splitField = field;
    m_splitNewName = newName;
    m_splitTitle->setText(tr("Split “%1” into two fields").arg(field.name));
    if (!same) {
        // Новое разбиение: прежние пункты, набранный текст и имена — прочь.
        clearSplitOptions();
        m_splitNamesTouched = false;
        m_splitTextReady = false;
        m_splitTextOption = SchemaEdit::SplitOption();
        {
            const QSignalBlocker block(m_splitText);
            m_splitText->clear();
            m_splitText->clearEditText();
        }
        m_splitTextResult->hide();
        m_splitTextRadio->setChecked(false);
        m_splitFirst->setText(newName);
        m_splitSecond->setText(field.name);
        m_splitPreview->setText(QString());
    }
    showPage(m_splitCard);
    applyAccent();
    updateSplitSelection();
}

void SchemaFieldCard::clearSplitOptions()
{
    const QList<QAbstractButton*> buttons = m_splitGroup->buttons();
    for (QAbstractButton* b : buttons) {
        if (b != m_splitTextRadio)
            m_splitGroup->removeButton(b);
    }
    while (QLayoutItem* item = m_splitLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->setObjectName(QString());
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_splitOptions.clear();
    m_splitReady = false;
}

void SchemaFieldCard::setSplitOptions(const QVector<SchemaEdit::SplitOption>& options, bool ready)
{
    if (!ready) {
        m_splitReady = false;
        m_splitStatus->setText(tr("checking on the lines…"));
        m_splitBox->setEnabled(false);
        updateSplitSelection();
        return;
    }
    // Выбор пользователя переживает пересчёт (по подписи пункта).
    QString chosen;
    const int checked = m_splitGroup->checkedId();
    if (checked >= 0 && checked < m_splitOptions.size())
        chosen = m_splitOptions[checked].label;
    const bool textChosen = checked == kTextOptionId;
    clearSplitOptions();
    m_splitOptions = options;
    m_splitReady = true;
    m_splitBox->setEnabled(true);
    int row = 0;
    int select = -1;
    for (int i = 0; i < options.size(); ++i) {
        const SchemaEdit::SplitOption& o = options[i];
        if (!o.available)
            continue;
        auto* radio = new QRadioButton(o.label, m_splitBox);
        radio->setObjectName(QStringLiteral("splitOption%1").arg(i));
        m_splitGroup->addButton(radio, i);
        auto* result = mutedLabel(m_splitBox);
        result->setObjectName(QStringLiteral("splitResult%1").arg(i));
        result->setWordWrap(false);
        result->setText(resultText(o));
        result->setToolTip(tr("With this choice the first part has %1 in the lines of the log, and %2% of the "
                              "lines are read by the schema.")
                               .arg(valuesText(o.distinct(), true), SchemaFit::percent(o.fit.coverage())));
        m_splitLayout->addWidget(radio, row, 0);
        m_splitLayout->addWidget(result, row, 1, Qt::AlignRight | Qt::AlignVCenter);
        ++row;
        if (!chosen.isEmpty() ? o.label == chosen : (select < 0 && o.recommended))
            select = i;
    }
    if (textChosen) {
        m_splitTextRadio->setChecked(true);
    } else if (select >= 0) {
        if (QAbstractButton* b = m_splitGroup->button(select))
            b->setChecked(true);
    } else if (QAbstractButton* b = row > 0 ? m_splitGroup->buttons().value(1) : nullptr) {
        b->setChecked(true);
    }
    m_splitStatus->setText(tr("values · lines read"));
    updateGeometry();
    updateSplitSelection();
}

void SchemaFieldCard::setSplitDelimiters(const QStringList& delimiters)
{
    QStringList current;
    for (int i = 0; i < m_splitText->count(); ++i)
        current << m_splitText->itemText(i);
    if (current == delimiters)
        return;
    const QString typed = m_splitText->currentText();
    const QSignalBlocker block(m_splitText);
    m_splitText->clear();
    m_splitText->addItems(delimiters);
    m_splitText->setCurrentIndex(-1);
    m_splitText->setEditText(typed);
}

void SchemaFieldCard::setSplitTextOption(const SchemaEdit::SplitOption& option, bool ready)
{
    m_splitTextReady = ready;
    m_splitTextOption = ready ? option : SchemaEdit::SplitOption();
    const bool typed = !m_splitText->currentText().trimmed().isEmpty();
    m_splitTextResult->setVisible(typed);
    if (!ready)
        m_splitTextResult->setText(tr("checking on the lines…"));
    else if (option.available)
        m_splitTextResult->setText(tr("%1 — %2").arg(option.label, resultText(option)));
    else
        m_splitTextResult->setText(option.reason);
    updateSplitSelection();
}

const SchemaEdit::SplitOption* SchemaFieldCard::selectedSplit() const
{
    const int id = m_splitGroup->checkedId();
    if (id == kTextOptionId)
        return m_splitTextReady && m_splitTextOption.available ? &m_splitTextOption : nullptr;
    if (m_splitReady && id >= 0 && id < m_splitOptions.size() && m_splitOptions[id].available)
        return &m_splitOptions[id];
    return nullptr;
}

void SchemaFieldCard::updateSplitSelection()
{
    const SchemaEdit::SplitOption* option = selectedSplit();
    m_splitApply->setEnabled(!m_busy && option);
    // Имена по умолчанию: у разделителя прежнее имя остаётся первой части
    // («Message кончается на ~~»), у остальных — второй (от начала поля
    // отделяется новое).
    if (option && !m_splitNamesTouched) {
        const bool keepFirst = option->choice == SchemaEdit::EndOption::Choice::Literal;
        m_splitFirst->setText(keepFirst ? m_splitField.name : m_splitNewName);
        m_splitSecond->setText(keepFirst ? m_splitNewName : m_splitField.name);
    }
    if (!option) {
        m_splitPreview->setText(m_splitGroup->checkedId() == kTextOptionId && m_splitTextReady
                                    ? QString()
                                    : tr("<i>Choose where the first part ends.</i>"));
        return;
    }
    const auto shorten = [](const QString& v) {
        return (v.size() > 26 ? v.left(25) + QChar(0x2026) : v).toHtmlEscaped();
    };
    QString text = tr("For example:");
    for (const auto& [first, second] : option->examples)
        text += QStringLiteral("<br><b>%1</b> &nbsp;%2&nbsp; %3").arg(shorten(first), QString(QChar(0x258F)), shorten(second));
    if (option->optionalTail)
        text += QStringLiteral("<br><i>%1</i>").arg(tr("Lines without this text stay whole in the first part."));
    m_splitPreview->setText(text);
}

// ---------------------------------------------------------------------------
// Текст между полями
// ---------------------------------------------------------------------------

bool SchemaFieldCard::isLiteralShown() const
{
    return m_stack->currentWidget() == m_literalCard;
}

void SchemaFieldCard::showLiteral(int piece, const QString& text, const QString& between)
{
    const bool same = isLiteralShown() && m_literalPiece == piece;
    m_literalPiece = piece;
    m_literalInfo->setText(tr("This text stands %1 in the line. If the fields are separated by something else, "
                              "type it here — any characters.").arg(between));
    // Набираемый текст не затирается пересчётом.
    if (!same || !m_literalEdit->hasFocus() || !m_literalEdit->isModified()) {
        m_literalEdit->setText(text);
        m_literalEdit->setModified(false);
    }
    showPage(m_literalCard);
}
