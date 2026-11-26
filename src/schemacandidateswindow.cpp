#include "schemacandidateswindow.h"

#include "aiconsentdialog.h"
#include "appsettings.h"
#include "cardframe.h"
#include "fieldschemacontroller.h"
#include "LogListView.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "secretstore.h"
#include "linetemplate.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

QUrl& endpointOverride()
{
    static QUrl url;
    return url;
}

constexpr int kVariantRole = Qt::UserRole + 1;

QString coverageText(const SchemaFit::Result& fit)
{
    if (fit.timedOut)
        return QCoreApplication::translate("SchemaCandidatesWindow", "too slow");
    if (!fit.valid || fit.candidates == 0)
        return QStringLiteral("—");
    return SchemaFit::percent(fit.coverage()) + QLatin1Char('%');
}

} // namespace

void SchemaCandidatesWindow::setAiEndpointForTesting(const QUrl& endpoint)
{
    endpointOverride() = endpoint;
}

SchemaCandidatesWindow::SchemaCandidatesWindow(FieldSchemaController* schemas, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_schemas(schemas)
    , m_candidates(new SchemaCandidates(this))
{
    setObjectName(QStringLiteral("schemaCandidatesWindow"));
    setWindowTitle(tr("Find a Schema"));
    resize(1000, 640);

    auto* root = new QVBoxLayout(this);
    m_sourceLabel = new MutedLabel(QString(), this);
    m_sourceLabel->setObjectName(QStringLiteral("schemaSourceLabel"));
    root->addWidget(m_sourceLabel);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(splitter, 1);

    // ---- Слева: варианты ---------------------------------------------------
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(left);
    m_list->setObjectName(QStringLiteral("schemaVariantList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    leftLayout->addWidget(m_list, 1);
    m_status = new MutedLabel(QString(), left);
    m_status->setObjectName(QStringLiteral("schemaVariantStatus"));
    m_status->setWordWrap(true);
    leftLayout->addWidget(m_status);
    m_askAi = new QPushButton(left);
    m_askAi->setObjectName(QStringLiteral("askAiButton"));
    leftLayout->addWidget(m_askAi);
    m_aiStatus = new MutedLabel(QString(), left);
    m_aiStatus->setObjectName(QStringLiteral("aiStatusLabel"));
    m_aiStatus->setWordWrap(true);
    m_aiStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    leftLayout->addWidget(m_aiStatus);

    // ---- Справа: выбранный вариант -----------------------------------------
    auto* right = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    m_title = new QLabel(right);
    m_title->setObjectName(QStringLiteral("schemaVariantTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    rightLayout->addWidget(m_title);
    m_template = new QLineEdit(right);
    m_template->setObjectName(QStringLiteral("schemaTemplateEdit"));
    m_template->setReadOnly(true);
    m_template->setToolTip(tr("The line template: literal text stays as in the line, {Name:kind} is a field"));
    rightLayout->addWidget(m_template);
    m_summary = new QLabel(right);
    m_summary->setObjectName(QStringLiteral("schemaVariantSummary"));
    m_summary->setWordWrap(true);
    rightLayout->addWidget(m_summary);
    m_notes = new MutedLabel(QString(), right);
    m_notes->setObjectName(QStringLiteral("schemaVariantNotes"));
    m_notes->setWordWrap(true);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rightLayout->addWidget(m_notes);

    auto* tables = new QSplitter(Qt::Vertical, right);
    m_samples = new QTableWidget(tables);
    m_samples->setObjectName(QStringLiteral("schemaSamplesTable"));
    m_samples->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_samples->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_samples->verticalHeader()->setVisible(false);
    m_samples->setWordWrap(false);
    auto* missBox = new QWidget(tables);
    auto* missLayout = new QVBoxLayout(missBox);
    missLayout->setContentsMargins(0, 0, 0, 0);
    m_missesLabel = new QLabel(missBox);
    missLayout->addWidget(m_missesLabel);
    m_misses = new QTableWidget(missBox);
    m_misses->setObjectName(QStringLiteral("schemaMissesTable"));
    m_misses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_misses->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_misses->verticalHeader()->setVisible(false);
    m_misses->setWordWrap(false);
    m_misses->setColumnCount(3);
    m_misses->setHorizontalHeaderLabels({tr("Line"), tr("Why"), tr("Text")});
    m_misses->horizontalHeader()->setStretchLastSection(true);
    missLayout->addWidget(m_misses, 1);
    tables->setStretchFactor(0, 3);
    tables->setStretchFactor(1, 1);
    rightLayout->addWidget(tables, 1);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);
    splitter->setSizes({260, 740});

    // ---- Кнопки ------------------------------------------------------------
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_use = new QPushButton(tr("Use for This Tab"), this);
    m_use->setObjectName(QStringLiteral("useSchemaForTabButton"));
    m_save = new QPushButton(tr("Save to My Schemas…"), this);
    m_save->setObjectName(QStringLiteral("saveSchemaButton"));
    auto* close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("closeSchemaWindowButton"));
    m_close = close;
    m_edit = new QPushButton(tr("Edit in Editor"), this);
    m_edit->setObjectName(QStringLiteral("editSchemaVariantButton"));
    m_edit->hide();
    buttons->addWidget(m_edit);
    buttons->addWidget(m_use);
    buttons->addWidget(m_save);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QWidget::close);
    connect(m_use, &QPushButton::clicked, this, &SchemaCandidatesWindow::useForTab);
    connect(m_edit, &QPushButton::clicked, this, [this]() {
        const int v = currentVariant();
        const auto& variants = m_candidates->variants();
        if (v < 0 || v >= variants.size())
            return;
        QString text = variants[v].templateText;
        if (text.isEmpty())
            LineTemplateSchema::templateOf(variants[v].entry.schema, &text);
        if (!text.isEmpty())
            emit editRequested(text, variants[v].title);
    });
    connect(m_save, &QPushButton::clicked, this, &SchemaCandidatesWindow::saveToSchemas);
    connect(m_askAi, &QPushButton::clicked, this, &SchemaCandidatesWindow::onAskAi);
    connect(m_list, &QListWidget::currentRowChanged, this, &SchemaCandidatesWindow::showVariant);
    connect(m_candidates, &SchemaCandidates::variantsChanged, this, &SchemaCandidatesWindow::rebuildList);
    connect(m_candidates, &SchemaCandidates::computingChanged, this, [this]() {
        m_status->setText(m_candidates->statusText());
    });
    connect(m_candidates, &SchemaCandidates::aiStateChanged, this, &SchemaCandidatesWindow::updateAiControls);
    connect(m_candidates, &SchemaCandidates::consentNeeded, this, &SchemaCandidatesWindow::onConsentNeeded,
            Qt::QueuedConnection);
    connect(&AppSettings::instance(), &AppSettings::settingsChanged, this,
            &SchemaCandidatesWindow::refreshAiSettings);

    if (m_schemas)
        m_candidates->setCatalogue(m_schemas->schemas());
    refreshAiSettings();
    rebuildList();
}

SchemaCandidatesWindow::~SchemaCandidatesWindow() = default;

void SchemaCandidatesWindow::setEmbedded(bool embedded)
{
    setWindowFlags(embedded ? Qt::Widget : Qt::Window);
    m_close->setVisible(!embedded);
    m_edit->setVisible(embedded);
}

void SchemaCandidatesWindow::setView(LogViewWidget* view)
{
    m_view = view;
    if (m_schemas) {
        m_schemas->reloadCatalogue();
        m_candidates->setCatalogue(m_schemas->schemas());
    }
    LogModel* model = view ? view->model() : nullptr;
    const LogFile* file = nullptr;
    if (model && view->view()) {
        const int row = qMax(0, view->view()->currentIndex().row());
        if (row < model->rowCount())
            file = model->keyAt(row).file.get();
    }
    m_sourceLabel->setText(file ? tr("Lines of %1").arg(file->shortName())
                                : (model ? tr("Lines of the current tab") : tr("No log is open")));
    m_candidates->setSource(model, file);
    updateAiControls();
}

void SchemaCandidatesWindow::setSourceModel(LogModel* model, const LogFile* file)
{
    m_view = nullptr;
    m_sourceLabel->setText(file ? tr("Lines of %1").arg(file->shortName()) : tr("Lines of the current tab"));
    m_candidates->setSource(model, file);
}

int SchemaCandidatesWindow::currentVariant() const
{
    const QListWidgetItem* item = m_list->currentItem();
    return item ? item->data(kVariantRole).toInt() : -1;
}

void SchemaCandidatesWindow::selectVariant(int variant)
{
    for (int row = 0; row < m_list->count(); ++row) {
        if (m_list->item(row)->data(kVariantRole).toInt() == variant) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

void SchemaCandidatesWindow::refreshAiSettings()
{
    const AppSettings& settings = AppSettings::instance();
    SchemaCandidates::AiSettings ai;
    ai.enabled = settings.aiEnabled();
    ai.model = settings.aiModel();
    ai.apiKey = SecretStore::effectiveKey(SecretStore::forApplication());
    if (endpointOverride().isValid())
        ai.endpoint = endpointOverride();
    m_candidates->setAiSettings(ai);
    updateAiControls();
}

void SchemaCandidatesWindow::rebuildList()
{
    const int keep = currentVariant();
    const QSignalBlocker block(m_list);
    m_list->clear();
    const auto& variants = m_candidates->variants();
    const auto addHeader = [this](const QString& text) {
        auto* item = new QListWidgetItem(text, m_list);
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        item->setFlags(Qt::NoItemFlags);
        item->setData(kVariantRole, -1);
    };
    const struct {
        SchemaCandidates::Variant::Source source;
        QString title;
    } sections[] = {
        {SchemaCandidates::Variant::Source::Catalogue, tr("Your schemas")},
        {SchemaCandidates::Variant::Source::Inferred, tr("From the structure of the lines")},
        {SchemaCandidates::Variant::Source::Ai, tr("AI")},
    };
    int firstSelectable = -1;
    for (const auto& section : sections) {
        bool header = false;
        for (int i = 0; i < variants.size(); ++i) {
            if (variants[i].source != section.source)
                continue;
            if (!header) {
                addHeader(section.title);
                header = true;
            }
            auto* item = new QListWidgetItem(
                QStringLiteral("%1 — %2").arg(variants[i].title, coverageText(variants[i].fit)), m_list);
            item->setData(kVariantRole, i);
            item->setToolTip(SchemaFit::summary(variants[i].fit));
            if (firstSelectable < 0)
                firstSelectable = m_list->count() - 1;
        }
    }
    m_status->setText(m_candidates->statusText());
    int row = -1;
    for (int r = 0; r < m_list->count(); ++r)
        if (keep >= 0 && m_list->item(r)->data(kVariantRole).toInt() == keep)
            row = r;
    m_list->setCurrentRow(row >= 0 ? row : firstSelectable);
    showVariant();
}

void SchemaCandidatesWindow::showVariant()
{
    const int v = currentVariant();
    const auto& variants = m_candidates->variants();
    const bool has = v >= 0 && v < variants.size();
    m_samples->clear();
    m_misses->setRowCount(0);
    if (!has) {
        m_title->setText(variants.isEmpty() ? tr("No variants yet") : QString());
        m_template->clear();
        m_summary->clear();
        m_notes->clear();
        m_samples->setRowCount(0);
        m_samples->setColumnCount(0);
        m_missesLabel->setText(tr("Did not match"));
        m_use->setEnabled(false);
        m_save->setEnabled(false);
        return;
    }
    const SchemaCandidates::Variant& variant = variants[v];
    m_title->setText(variant.title);
    m_template->setText(variant.templateText);
    // Шаблон — с начала: setText ставит курсор в конец, и длинный шаблон
    // показывался прокрученным к хвосту; прокрутка к тому же зависела от
    // ширины текста при смене темы (theme_mode на Windows, S-29).
    m_template->setCursorPosition(0);
    m_template->setVisible(!variant.templateText.isEmpty());
    m_summary->setText(SchemaFit::summary(variant.fit));
    QStringList notes = variant.notes;
    if (!variant.comment.isEmpty())
        notes.prepend(tr("Model's comment: %1").arg(variant.comment));
    m_notes->setText(notes.join(QLatin1Char('\n')));
    m_notes->setVisible(!notes.isEmpty());

    const QStringList columns = m_candidates->columns(v);
    QStringList headers{tr("Line")};
    for (int c = 0; c < columns.size(); ++c) {
        QString header = columns[c];
        if (c < variant.fit.distinct.size())
            header += QStringLiteral(" (%1)").arg(variant.fit.distinctText(c));
        headers << header;
    }
    const QVector<SchemaCandidates::Row> rows = m_candidates->sampleRows(v);
    m_samples->setColumnCount(int(headers.size()));
    m_samples->setHorizontalHeaderLabels(headers);
    m_samples->setRowCount(int(rows.size()));
    for (int r = 0; r < rows.size(); ++r) {
        m_samples->setItem(r, 0, new QTableWidgetItem(QString::number(rows[r].line)));
        for (int c = 0; c < rows[r].values.size(); ++c) {
            auto* item = new QTableWidgetItem(rows[r].values[c]);
            item->setToolTip(rows[r].text);
            m_samples->setItem(r, c + 1, item);
        }
    }
    m_samples->resizeColumnsToContents();
    for (int c = 0; c < m_samples->columnCount(); ++c)
        m_samples->setColumnWidth(c, qMin(m_samples->columnWidth(c), 320));

    const QVector<SchemaCandidates::MissRow> misses = m_candidates->misses(v);
    m_missesLabel->setText(variant.fit.missed + variant.fit.partial > 0
                               ? tr("Did not match or matched partly (%1 shown)").arg(misses.size())
                               : tr("Every checked record line matched"));
    m_misses->setRowCount(int(misses.size()));
    for (int r = 0; r < misses.size(); ++r) {
        m_misses->setItem(r, 0, new QTableWidgetItem(QString::number(misses[r].line)));
        m_misses->setItem(r, 1, new QTableWidgetItem(misses[r].reason));
        m_misses->setItem(r, 2, new QTableWidgetItem(misses[r].text));
    }
    m_misses->resizeColumnToContents(0);
    m_misses->resizeColumnToContents(1);
    m_misses->setColumnWidth(1, qMin(m_misses->columnWidth(1), 360));

    const bool catalogue = variant.source == SchemaCandidates::Variant::Source::Catalogue;
    m_use->setEnabled(m_view && m_schemas && (catalogue || !variant.templateText.isEmpty()));
    m_use->setToolTip(tr("Use this schema in the tab the window was opened for (the file is not read again)"));
    m_save->setEnabled(!catalogue && !variant.templateText.isEmpty());
}

void SchemaCandidatesWindow::updateAiControls()
{
    using S = SchemaCandidates::AiState;
    const S state = m_candidates->aiState();
    switch (state) {
    case S::Off:
    case S::NoKey:
        m_askAi->setText(tr("Set Up AI…"));
        m_askAi->setEnabled(true);
        m_aiStatus->setText(state == S::Off ? tr("Suggestions by AI are off. They send sample lines of this log "
                                                 "to Anthropic when you ask.")
                                            : tr("AI suggestions need an API key."));
        break;
    case S::Ready:
    case S::Done:
    case S::Failed:
        m_askAi->setText(tr("Ask AI…"));
        m_askAi->setEnabled(m_candidates->sampleCount() > 0 && !m_consentOpen);
        m_aiStatus->setText(state == S::Failed ? m_candidates->aiMessage()
                                               : (state == S::Done ? tr("The suggestion is in the list.")
                                                                   : QString()));
        break;
    case S::NeedsConsent:
        m_askAi->setText(tr("Ask AI…"));
        m_askAi->setEnabled(false);
        m_aiStatus->setText(m_candidates->aiMessage());
        break;
    case S::Sending:
    case S::Checking:
        m_askAi->setText(tr("Cancel"));
        m_askAi->setEnabled(true);
        m_aiStatus->setText(state == S::Sending ? tr("Waiting for the model…")
                                                : tr("Checking the suggestion on the lines…"));
        break;
    }
}

void SchemaCandidatesWindow::onAskAi()
{
    using S = SchemaCandidates::AiState;
    switch (m_candidates->aiState()) {
    case S::Off:
    case S::NoKey:
        emit settingsRequested();
        return;
    case S::Sending:
    case S::Checking:
        m_candidates->cancelAi();
        return;
    default:
        m_candidates->askAi();
        return;
    }
}

void SchemaCandidatesWindow::onConsentNeeded()
{
    if (m_consentOpen || m_candidates->aiState() != SchemaCandidates::AiState::NeedsConsent)
        return;
    m_consentOpen = true;
    updateAiControls();
    SchemaCandidates* candidates = m_candidates;
    AiConsentDialog dialog([candidates](const SchemaProposal::MaskOptions& o) { return candidates->prepareAi(o); },
                           m_candidates->aiMessage().isEmpty() ? false : true, m_candidates->aiMessage(), this);
    const bool send = dialog.exec() == QDialog::Accepted;
    m_consentOpen = false;
    if (send)
        m_candidates->sendAi(dialog.request(), dialog.consent());
    else
        m_candidates->cancelAi();
    updateAiControls();
}

void SchemaCandidatesWindow::useForTab()
{
    const int v = currentVariant();
    const auto& variants = m_candidates->variants();
    if (v < 0 || v >= variants.size() || !m_view || !m_schemas)
        return;
    const SchemaCandidates::Variant& variant = variants[v];
    // Поля ленивые (F-17, шаг 3): схема — вкладке за O(1), файл не
    // перечитывается; шаблон (вывод структуры, AI) — определением версии 4.
    FieldSettings settings = m_view->fieldSettings();
    if (variant.source == SchemaCandidates::Variant::Source::Catalogue) {
        settings.schemaName = variant.entry.name;
        settings.schema = variant.entry.schema;
    } else {
        if (variant.templateText.isEmpty())
            return;
        settings.schemaName = variant.title;
        settings.schema = LineTemplateSchema::definition(variant.templateText);
    }
    settings.showFields = true;
    settings.visibleFields.clear();
    m_schemas->applyToView(m_view, settings);
    emit schemaUsed(m_view);
}

void SchemaCandidatesWindow::saveToSchemas()
{
    const int v = currentVariant();
    const auto& variants = m_candidates->variants();
    if (v < 0 || v >= variants.size())
        return;
    const QString suggested = variants[v].source == SchemaCandidates::Variant::Source::Ai ? tr("AI schema")
                                                                                          : tr("Inferred schema");
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save to My Schemas"), tr("Schema name:"),
                                               QLineEdit::Normal, suggested, &ok);
    if (!ok)
        return;
    QString error;
    const QString saved = m_candidates->saveTemplate(v, name, &error);
    if (saved.isEmpty()) {
        QMessageBox::warning(this, tr("Save to My Schemas"), error);
        return;
    }
    m_aiStatus->setText(tr("Saved as “%1”.").arg(saved));
    emit schemaSaved(saved);
}
