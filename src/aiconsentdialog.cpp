#include "aiconsentdialog.h"

#include "cardframe.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

AiConsentDialog::AiConsentDialog(Builder builder, bool refinement, const QString& note, QWidget* parent)
    : QDialog(parent)
    , m_builder(std::move(builder))
{
    setObjectName(QStringLiteral("aiConsentDialog"));
    setWindowTitle(refinement ? tr("Send a Follow-up Request?") : tr("Send Log Lines to AI?"));
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    resize(760, 560);

    auto* layout = new QVBoxLayout(this);
    if (!note.isEmpty()) {
        auto* noteLabel = new QLabel(note, this);
        noteLabel->setWordWrap(true);
        layout->addWidget(noteLabel);
    }
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("aiConsentSummary"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_summary);

    auto* options = new QHBoxLayout();
    m_mask = new QCheckBox(tr("Mask values"), this);
    m_mask->setObjectName(QStringLiteral("aiMaskCheckBox"));
    m_mask->setChecked(true);
    m_mask->setToolTip(tr("Digits become 0, long hex values and UUIDs become \"a\", e-mail addresses become\n"
                          "user@example.com, IP addresses become documentation addresses.\n"
                          "Timestamps and level words are kept."));
    m_cut = new QCheckBox(tr("Cut lines to %1 characters").arg(SchemaProposal::kCutChars), this);
    m_cut->setObjectName(QStringLiteral("aiCutCheckBox"));
    m_cut->setChecked(true);
    options->addWidget(m_mask);
    options->addWidget(m_cut);
    options->addStretch(1);
    layout->addLayout(options);

    auto* warning = new MutedLabel(
        tr("Masking reduces what leaves your computer, it does not exclude it: words, names, paths and "
           "anything else in the lines below are sent as shown. The file name, its folder, your user name "
           "and computer name are not sent. Nothing is sent without this confirmation."),
        this);
    warning->setWordWrap(true);
    layout->addWidget(warning);

    m_text = new QPlainTextEdit(this);
    m_text->setObjectName(QStringLiteral("aiConsentText"));
    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(m_text, 1);

    auto* systemToggle = new QToolButton(this);
    systemToggle->setText(tr("Instructions sent with every request"));
    systemToggle->setCheckable(true);
    systemToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    systemToggle->setArrowType(Qt::RightArrow);
    auto* systemText = new QPlainTextEdit(SchemaProposal::systemPrompt(), this);
    systemText->setObjectName(QStringLiteral("aiConsentSystemText"));
    systemText->setReadOnly(true);
    systemText->hide();
    connect(systemToggle, &QToolButton::toggled, this, [systemToggle, systemText](bool on) {
        systemToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        systemText->setVisible(on);
    });
    layout->addWidget(systemToggle);
    layout->addWidget(systemText, 1);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* send = buttons->addButton(tr("Send"), QDialogButtonBox::AcceptRole);
    send->setObjectName(QStringLiteral("aiConsentSendButton"));
    buttons->addButton(QDialogButtonBox::Cancel);
    // Отправка — только явным нажатием: Enter её не делает.
    send->setAutoDefault(false);
    send->setDefault(false);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_mask, &QCheckBox::toggled, this, &AiConsentDialog::rebuild);
    connect(m_cut, &QCheckBox::toggled, this, &AiConsentDialog::rebuild);
    rebuild();
}

void AiConsentDialog::rebuild()
{
    SchemaProposal::MaskOptions options;
    options.mask = m_mask->isChecked();
    options.cut = m_cut->isChecked();
    m_request = m_builder ? m_builder(options) : SchemaCandidates::ConsentRequest();
    m_text->setPlainText(m_request.text);
    m_summary->setText(tr("<b>%1</b> lines (%2 characters) will be sent to <b>%3</b> for the model <b>%4</b>. "
                          "This is exactly the text of the request:")
                           .arg(m_request.lines)
                           .arg(m_request.characters)
                           .arg(m_request.host.toHtmlEscaped(), m_request.model.toHtmlEscaped()));
}

void AiConsentDialog::accept()
{
    if (m_request.isEmpty())
        return;
    m_consent = AiConsent::grant(m_request.body);
    QDialog::accept();
}
