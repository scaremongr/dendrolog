#include "gotodialog.h"

#include "logmodel.h"

#include <QDir>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString timeText(const QDateTime& time)
{
    return time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

} // namespace

GoToDialog::GoToDialog(LogModel* model, const QList<LogFilePtr>& files, int currentRow,
                       QWidget* parent)
    : QDialog(parent)
    , m_model(model)
    , m_reference(model ? GoToLocation::referenceTime(*model, currentRow) : QDateTime())
{
    setWindowTitle(tr("Go to Line / Time"));

    // Поля и промежутки корневой раскладки даёт CompactStyle.
    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    root->addLayout(form);

    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("goToInput"));
    m_input->setPlaceholderText(tr("1234, 10:02:03 or 2026-03-05 10:02"));
    m_input->setClearButtonEnabled(true);
    m_input->setMinimumWidth(fontMetrics().averageCharWidth() * 48);
    form->addRow(tr("Line or time:"), m_input);

    m_fileCombo = new QComboBox(this);
    m_fileCombo->setObjectName(QStringLiteral("goToFile"));
    const LogFile* currentFile = model ? model->keyForRow(currentRow).sourceFile : nullptr;
    for (const LogFilePtr& file : files) {
        if (!file)
            continue;
        m_fileCombo->addItem(file->shortName(), QVariant::fromValue(file));
        m_fileCombo->setItemData(m_fileCombo->count() - 1, file->nativeDisplayPath(), Qt::ToolTipRole);
        if (file.get() == currentFile)
            m_fileCombo->setCurrentIndex(m_fileCombo->count() - 1);
    }
    m_fileLabel = new QLabel(tr("Line of file:"), this);
    form->addRow(m_fileLabel, m_fileCombo);
    // Один файл — номер строки однозначен, выбирать нечего.
    const bool severalFiles = m_fileCombo->count() > 1;
    m_fileLabel->setVisible(severalFiles);
    m_fileCombo->setVisible(severalFiles);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("goToPreview"));
    m_preview->setWordWrap(true);
    m_preview->setTextFormat(Qt::PlainText);
    // Две строки: подпись не должна менять высоту диалога при вводе.
    m_preview->setMinimumHeight(fontMetrics().lineSpacing() * 2);
    m_preview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    root->addWidget(m_preview);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_goButton = buttons->button(QDialogButtonBox::Ok);
    m_goButton->setText(tr("Go"));
    m_goButton->setObjectName(QStringLiteral("goToButton"));
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &GoToDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &GoToDialog::reject);
    connect(m_input, &QLineEdit::textChanged, this, &GoToDialog::updatePreview);
    connect(m_fileCombo, &QComboBox::currentIndexChanged, this, &GoToDialog::updatePreview);

    updatePreview();
    m_input->setFocus();
}

GoToResult GoToDialog::resolveInput(QString* description, bool* inexact) const
{
    *inexact = false;
    if (!m_model) {
        description->clear();
        return {};
    }
    const QString text = m_input->text().trimmed();
    if (text.isEmpty()) {
        *description = tr("A line number of the file, or a time: 10:02, 10:02:03.250, "
                          "2026-03-05, 2026-03-05 10:02.");
        return {};
    }

    const GoToTarget target = GoToLocation::parse(text, m_reference);
    if (target.kind == GoToTarget::Kind::Invalid) {
        // Время без даты в логе без единой метки: ввод верный, перейти некуда.
        const QDateTime anyDay(QDate(2000, 1, 1), QTime(0, 0));
        const bool timeWithoutDate = !m_reference.isValid()
            && GoToLocation::parse(text, anyDay).kind == GoToTarget::Kind::Time;
        *description = timeWithoutDate ? tr("This log has no timestamps.")
                                       : tr("Not a line number or a time.");
        return {};
    }

    const LogFilePtr file = m_fileCombo->currentData().value<LogFilePtr>();
    const GoToResult found = GoToLocation::resolve(*m_model, target, file.get());
    if (found.row < 0) {
        *description = m_model->rowCount() == 0
            ? tr("No lines are shown.")
            : (target.kind == GoToTarget::Kind::Time ? tr("Shown lines have no timestamps.")
                                                     : tr("This file has no lines here."));
        return found;
    }

    const QLocale locale;
    const bool severalFiles = m_fileCombo->count() > 1;
    QString where = tr("line %1").arg(locale.toString(found.line));
    if (severalFiles && found.file)
        where += tr(" of %1").arg(found.file->shortName());
    if (found.time.isValid())
        where += QStringLiteral(", ") + timeText(found.time);

    if (found.exact) {
        *description = tr("Goes to %1.").arg(where);
        return found;
    }
    *inexact = true;
    if (target.kind == GoToTarget::Kind::Line) {
        const bool sameFile = found.file == file;
        if (sameFile && found.line < target.line && m_model->rowCount() == m_model->totalLineCount())
            *description = tr("The file ends earlier — goes to its last line: %1.").arg(where);
        else
            *description = tr("Line %1 is not shown — goes to the nearest shown: %2.")
                               .arg(locale.toString(target.line), where);
    } else if (found.time < target.time) {
        *description = tr("Later than the last shown timestamp — goes to %1.").arg(where);
    } else {
        *description = tr("No shown line at %1 — goes to the next one: %2.")
                           .arg(timeText(target.time), where);
    }
    return found;
}

void GoToDialog::updatePreview()
{
    QString description;
    bool inexact = false;
    const GoToResult found = resolveInput(&description, &inexact);
    m_preview->setText(description);
    m_goButton->setEnabled(found.row >= 0);
}

void GoToDialog::accept()
{
    // Вкладка могла измениться, пока диалог открыт, — ищем заново.
    QString description;
    bool inexact = false;
    m_result = resolveInput(&description, &inexact);
    if (m_result.row < 0) {
        m_preview->setText(description);
        m_goButton->setEnabled(false);
        return;
    }
    m_resultNote = inexact ? description : QString();
    QDialog::accept();
}
