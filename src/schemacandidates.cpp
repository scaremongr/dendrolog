#include "schemacandidates.h"

#include "fieldlayout.h"
#include "linetemplate.h"
#include "logmodel.h"
#include "logscan.h"
#include "logmodel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

SchemaCandidates::SchemaCandidates(QObject* parent)
    : QObject(parent)
    , m_watcher(new QFutureWatcher<Output>(this))
    , m_resetTimer(new QTimer(this))
    , m_client(new AiSchemaClient(this))
    , m_checkWatcher(new QFutureWatcher<AiCheck>(this))
{
    connect(m_watcher, &QFutureWatcher<Output>::finished, this, &SchemaCandidates::onComputed);
    connect(m_checkWatcher, &QFutureWatcher<AiCheck>::finished, this, &SchemaCandidates::onAiChecked);
    connect(m_client, &AiSchemaClient::finished, this, &SchemaCandidates::onAiFinished);
    m_resetTimer->setSingleShot(true);
    m_resetTimer->setInterval(500);
    connect(m_resetTimer, &QTimer::timeout, this, &SchemaCandidates::recompute);
}

SchemaCandidates::~SchemaCandidates()
{
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    if (m_checkCancel)
        m_checkCancel->store(true, std::memory_order_relaxed);
    m_watcher->waitForFinished();
    m_checkWatcher->waitForFinished();
}

void SchemaCandidates::setCatalogue(const SchemaStore::SchemaList& catalogue)
{
    m_catalogue = catalogue;
    m_catalogueExtractors.clear();
    m_catalogueExtractors.reserve(catalogue.size());
    for (const SchemaStore::SchemaEntry& entry : catalogue)
        m_catalogueExtractors.append(FieldLayout::extractorFor(entry.schema));
}

void SchemaCandidates::setSource(LogModel* model, const LogFile* file)
{
    if (m_resetConnection)
        disconnect(m_resetConnection);
    // Другой файл — ответы модели и ждущее уточнение были о прежнем.
    if (model != m_model || file != m_file)
        resetAi();
    m_model = model;
    m_file = file;
    if (model)
        m_resetConnection = connect(model, &QAbstractItemModel::modelReset, m_resetTimer,
                                    qOverload<>(&QTimer::start));
    recompute();
}

bool SchemaCandidates::isComputing() const
{
    return m_watcher->isRunning() || m_resetTimer->isActive();
}

void SchemaCandidates::recompute()
{
    m_resetTimer->stop();
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    const int generation = ++m_generation;
    if (!m_model) {
        m_samples.clear();
        m_ranked.clear();
        m_inference = SchemaInference::Result();
        m_status = tr("No log is open.");
        rebuildVariants();
        emit computingChanged();
        return;
    }
    m_status = tr("Checking the lines of the log…");
    auto flag = std::make_shared<std::atomic_bool>(false);
    m_cancel = flag;
    LogScanSnapshot snapshot = m_model->scanSnapshot(/*filteredOnly=*/false);
    const QVector<FieldExtractorPtr> extractors = m_catalogueExtractors;
    const LogFile* file = m_file;
    // Файл живёт, пока он во вкладке; снапшот держит свои ссылки на файлы.
    m_watcher->setFuture(QtConcurrent::run([snapshot = std::move(snapshot), extractors, file, flag, generation]() {
        Output out;
        out.generation = generation;
        out.samples = SchemaFitJob::sampleLines(snapshot, file, flag.get());
        const SchemaFit::Corpus corpus = SchemaFit::corpus(out.samples);
        SchemaFit::Budget budget;
        budget.cancel = flag.get();
        out.catalogue = SchemaFit::rank(extractors, corpus, budget);
        SchemaInference::Options options;
        options.cancel = flag.get();
        out.inference = SchemaInference::infer(out.samples, options);
        out.cancelled = flag->load(std::memory_order_relaxed);
        return out;
    }));
    emit computingChanged();
}

void SchemaCandidates::onComputed()
{
    const Output out = m_watcher->result();
    if (out.generation != m_generation || out.cancelled)
        return;
    m_samples = out.samples;
    m_ranked = out.catalogue;
    m_inference = out.inference;
    if (m_samples.isEmpty())
        m_status = tr("The log has no lines yet.");
    else
        m_status = tr("Checked on %1 lines of the log.").arg(m_samples.size());
    rebuildVariants();
    // AI-варианты посчитаны на прежних строках: пересчитать меру.
    emit computingChanged();
}

void SchemaCandidates::rebuildVariants()
{
    m_variants.clear();
    int shown = 0;
    for (const SchemaFit::Ranked& ranked : std::as_const(m_ranked)) {
        if (shown >= kCatalogueVariants)
            break;
        const SchemaFit::Result& r = ranked.result;
        if (!r.valid || r.coverage() <= 0 || r.specificity() < SchemaFit::kMinSpecificity)
            continue;
        if (ranked.index < 0 || ranked.index >= m_catalogue.size())
            continue;
        Variant v;
        v.source = Variant::Source::Catalogue;
        v.entry = m_catalogue[ranked.index];
        v.title = v.entry.name;
        v.extractor = m_catalogueExtractors.value(ranked.index);
        v.fit = r;
        v.slow = v.extractor && v.extractor->cost() == FieldExtractor::Cost::Background;
        m_variants.append(v);
        ++shown;
    }
    for (const SchemaInference::Variant& inferred : std::as_const(m_inference.variants)) {
        Variant v;
        v.source = Variant::Source::Inferred;
        v.title = inferred.kind == SchemaInference::Variant::Kind::Full ? tr("Inferred from the lines")
                                                                       : tr("Inferred, cautious");
        v.templateText = inferred.templateText;
        v.extractor = std::make_shared<TemplateExtractor>(inferred.templateText);
        v.fit = inferred.fit;
        for (const SchemaInference::Note& note : inferred.notes)
            v.notes << note.text;
        v.slow = v.extractor->cost() == FieldExtractor::Cost::Background;
        m_variants.append(v);
    }
    if (!m_samples.isEmpty() && !m_inference.ok() && m_inference.failure != SchemaInference::Failure::None
        && m_inference.failure != SchemaInference::Failure::Cancelled)
        m_status += QLatin1Char(' ') + tr("No structure inferred: %1").arg(m_inference.failureText);
    // AI-варианты — на текущих строках (мера пересчитывается: дёшево, шаблон).
    for (Variant v : std::as_const(m_aiVariants)) {
        if (v.extractor && !m_samples.isEmpty() && !v.slow)
            v.fit = SchemaFit::evaluate(*v.extractor, m_samples);
        m_variants.append(v);
    }
    emit variantsChanged();
}

QStringList SchemaCandidates::columns(int variant) const
{
    if (variant < 0 || variant >= m_variants.size() || !m_variants[variant].extractor)
        return {};
    return m_variants[variant].extractor->fields();
}

QVector<SchemaCandidates::Row> SchemaCandidates::sampleRows(int variant, int maxRows) const
{
    QVector<Row> rows;
    if (variant < 0 || variant >= m_variants.size() || !m_variants[variant].extractor)
        return rows;
    const FieldExtractor& extractor = *m_variants[variant].extractor;
    const SchemaFit::Corpus corpus = SchemaFit::corpus(m_samples);
    const int columns = int(extractor.fields().size());
    LogEntryFields fields;
    for (int i = 0; i < corpus.samples.size() && rows.size() < maxRows; ++i) {
        if (!corpus.nonEmpty.value(i) || (corpus.candidateCount > 0 && !corpus.candidate.value(i)))
            continue;
        const QString& text = corpus.samples[i].text;
        const FieldExtractor::Outcome outcome = extractor.extract(text, &fields);
        if (!FieldExtractor::accepted(outcome))
            continue;
        Row row;
        row.line = corpus.samples[i].line;
        row.text = text;
        row.partial = outcome == FieldExtractor::Outcome::Partial;
        for (int c = 0; c < columns; ++c)
            row.values << fields.value(c, text, /*displayLine=*/true);
        rows.append(row);
    }
    return rows;
}

QVector<SchemaCandidates::MissRow> SchemaCandidates::misses(int variant) const
{
    QVector<MissRow> rows;
    if (variant < 0 || variant >= m_variants.size() || !m_variants[variant].extractor)
        return rows;
    const Variant& v = m_variants[variant];
    for (const SchemaFit::Miss& miss : v.fit.misses) {
        MissRow row;
        row.line = miss.line;
        row.text = miss.text;
        row.partial = miss.partial;
        row.reason = reasonText(v.extractor->explain(miss.text));
        rows.append(row);
    }
    return rows;
}

QString SchemaCandidates::reasonText(const MatchReport& report, bool addValueHint)
{
    using E = MatchReport::Expect;
    QString what;
    switch (report.expect) {
    case E::Nothing: return tr("parsed");
    case E::Literal: what = tr("“%1”").arg(report.literal); break;
    case E::Timestamp: what = tr("a timestamp"); break;
    case E::Level: what = tr("a level word"); break;
    case E::Number: what = tr("a number"); break;
    case E::Hex: what = tr("a hex value"); break;
    case E::Ip: what = tr("an IP address"); break;
    case E::Path: what = tr("a path"); break;
    case E::Text: what = tr("text"); break;
    case E::Regex: what = tr("text matching the pattern"); break;
    case E::Record: what = tr("a JSON or logfmt record"); break;
    case E::Uuid: what = tr("a UUID"); break;
    case E::Quoted: what = tr("a quoted string"); break;
    case E::Value: what = tr("one of: %1").arg(QString(report.literal).replace(QLatin1Char('|'), QStringLiteral(", "))); break;
    case E::Spaces: what = tr("a space"); break;
    case E::Word: what = tr("a word"); break;
    }
    QString text = report.field.isEmpty() ? tr("expected %1").arg(what)
                                          : tr("expected %1 for %2").arg(what, report.field);
    if (report.stopOffset > 0)
        text += tr(" at character %1").arg(report.stopOffset + 1);
    if (!report.found.isEmpty())
        text += tr(", found “%1”").arg(report.found);
    else
        text += tr(", the line ended");
    if (report.expect == E::Value && addValueHint)
        text += tr(" — add it with Add Value… in the menu of the column");
    if (report.partial)
        text = tr("parsed partly: %1").arg(text);
    return text;
}

// ---------------------------------------------------------------------------
// AI
// ---------------------------------------------------------------------------

void SchemaCandidates::resetAi()
{
    if (m_aiState == AiState::Sending)
        m_client->cancel();
    if (m_checkCancel)
        m_checkCancel->store(true, std::memory_order_relaxed);
    ++m_aiGeneration;
    m_aiVariants.clear();
    m_aiRound = 0;
    m_refinement = SchemaProposal::Refinement();
    m_aiState = AiState::Ready;   // updateAiIdleState решает заново
    m_aiMessage.clear();
    updateAiIdleState();
    emit aiStateChanged();
}

void SchemaCandidates::setAiSettings(const AiSettings& settings)
{
    m_ai = settings;
    AiSchemaClient::Config config = m_client->config();
    config.enabled = settings.enabled;
    config.apiKey = settings.apiKey;
    m_client->setConfig(config);
    m_client->setProvider(std::make_shared<ClaudeProvider>(settings.model, settings.endpoint));
    updateAiIdleState();
}

void SchemaCandidates::updateAiIdleState()
{
    // Идущая работа не прерывается сменой настроек.
    if (m_aiState == AiState::Sending || m_aiState == AiState::Checking || m_aiState == AiState::NeedsConsent)
        return;
    if (m_aiState == AiState::Done || m_aiState == AiState::Failed) {
        if (m_ai.enabled && !m_ai.apiKey.trimmed().isEmpty())
            return;
    }
    if (!m_ai.enabled)
        setAiState(AiState::Off);
    else if (m_ai.apiKey.trimmed().isEmpty())
        setAiState(AiState::NoKey);
    else
        setAiState(AiState::Ready);
}

void SchemaCandidates::setAiState(AiState state, const QString& message)
{
    if (m_aiState == state && m_aiMessage == message)
        return;
    m_aiState = state;
    m_aiMessage = message;
    emit aiStateChanged();
}

void SchemaCandidates::askAi()
{
    if (!m_ai.enabled || m_ai.apiKey.trimmed().isEmpty() || m_samples.isEmpty()
        || m_aiState == AiState::Sending || m_aiState == AiState::Checking)
        return;
    m_aiRound = 0;
    m_refinement = SchemaProposal::Refinement();
    setAiState(AiState::NeedsConsent);
    emit consentNeeded();
}

SchemaCandidates::ConsentRequest SchemaCandidates::prepareAi(const SchemaProposal::MaskOptions& options) const
{
    ConsentRequest request;
    QHash<QString, QString> ipMap;   // один IP — один адрес на весь запрос
    const QStringList lines = SchemaProposal::pickLines(m_samples, options, SchemaProposal::kDefaultLines, &ipMap);
    if (lines.isEmpty())
        return request;
    SchemaProposal::RequestOptions requestOptions;
    requestOptions.model = m_ai.model;
    SchemaProposal::Refinement refinement;
    if (m_aiRound > 0) {
        refinement = m_refinement;
        // Не подошедшие строки — теми же правилами, что и образцы.
        refinement.failedLines = SchemaProposal::prepareLines(m_refinement.failedLines, options, &ipMap);
        request.refinement = true;
    }
    request.host = m_ai.endpoint.host();
    request.model = m_ai.model;
    request.body = SchemaProposal::requestBody(lines, requestOptions, refinement);
    request.lines = int(lines.size() + refinement.failedLines.size());
    for (const QString& line : lines)
        request.characters += int(line.size());
    for (const QString& line : std::as_const(refinement.failedLines))
        request.characters += int(line.size());
    request.text = SchemaProposal::userText(lines);
    if (request.refinement) {
        request.text += QStringLiteral("\n\n— %1 —\n").arg(tr("the previous answer"));
        request.text += SchemaProposal::previousAnswerText(refinement);
        request.text += QStringLiteral("\n\n— %1 —\n").arg(tr("then"));
        request.text += SchemaProposal::refinementText(refinement);
    }
    return request;
}

bool SchemaCandidates::sendAi(const ConsentRequest& request, AiConsent& consent)
{
    if (m_aiState != AiState::NeedsConsent || request.isEmpty())
        return false;
    setAiState(AiState::Sending);
    return m_client->send(request.body, consent);
}

void SchemaCandidates::cancelAi()
{
    if (m_aiState == AiState::Sending)
        m_client->cancel();     // onAiFinished: Cancelled
    if (m_aiState == AiState::Checking && m_checkCancel) {
        m_checkCancel->store(true, std::memory_order_relaxed);
        ++m_aiGeneration;
    }
    if (m_aiState == AiState::NeedsConsent || m_aiState == AiState::Checking) {
        m_aiRound = 0;
        setAiState(AiState::Ready);
        updateAiIdleState();
    }
}

void SchemaCandidates::onAiFinished(const AiSchemaClient::Result& result)
{
    if (m_aiState != AiState::Sending)
        return;
    if (result.status == AiSchemaClient::Result::Status::Cancelled) {
        m_aiRound = 0;
        setAiState(AiState::Ready);
        updateAiIdleState();
        return;
    }
    if (!result.ok()) {
        setAiState(AiState::Failed, result.message);
        return;
    }
    const SchemaProposal::Reply reply = SchemaProposal::parseReply(result.body);
    if (!reply.ok()) {
        setAiState(AiState::Failed, reply.message);
        return;
    }
    setAiState(AiState::Checking);
    auto flag = std::make_shared<std::atomic_bool>(false);
    m_checkCancel = flag;
    const int generation = ++m_aiGeneration;
    const QVector<SchemaFit::Sample> samples = m_samples;
    const bool finalRound = m_aiRound > 0;
    const QString model = reply.model;
    m_checkWatcher->setFuture(QtConcurrent::run([reply, samples, finalRound, flag, generation, model]() {
        AiCheck out;
        out.generation = generation;
        out.model = model;
        SchemaFit::Budget budget;
        budget.maxMs = SchemaProposal::kCheckBudgetMs;
        budget.cancel = flag.get();
        out.check = SchemaProposal::checkTemplate(reply.templateText, reply.comment, samples, finalRound, &budget);
        return out;
    }));
}

void SchemaCandidates::onAiChecked()
{
    const AiCheck out = m_checkWatcher->result();
    if (out.generation != m_aiGeneration || m_aiState != AiState::Checking)
        return;
    const SchemaProposal::Check& check = out.check;
    switch (check.status) {
    case SchemaProposal::Check::Status::Accepted: {
        Variant v;
        v.source = Variant::Source::Ai;
        v.title = m_aiVariants.isEmpty() ? tr("AI suggestion") : tr("AI suggestion %1").arg(m_aiVariants.size() + 1);
        v.templateText = check.templateText;
        v.extractor = std::make_shared<TemplateExtractor>(check.templateText);
        v.fit = check.fit;
        v.comment = check.comment;
        v.slow = check.slow;
        v.model = out.model;
        if (check.slow)
            v.notes << tr("The template uses a regular expression: fields of such a schema are slower.");
        m_aiVariants.append(v);
        m_aiRound = 0;
        rebuildVariants();
        setAiState(AiState::Done);
        return;
    }
    case SchemaProposal::Check::Status::NeedsRefinement:
        m_aiRound = 1;
        m_refinement = check.refinement;
        setAiState(AiState::NeedsConsent, tr("The suggested template needs a correction (%1). "
                                              "Ask the model to correct it?")
                                              .arg(check.refinement.problem));
        emit consentNeeded();
        return;
    case SchemaProposal::Check::Status::Rejected:
        m_aiRound = 0;
        setAiState(AiState::Failed, check.message);
        return;
    }
}

// ---------------------------------------------------------------------------
// Сохранение
// ---------------------------------------------------------------------------

QString SchemaCandidates::saveTemplate(int variant, const QString& name, QString* error) const
{
    const auto fail = [error](const QString& text) {
        if (error)
            *error = text;
        return QString();
    };
    if (variant < 0 || variant >= m_variants.size() || m_variants[variant].templateText.isEmpty())
        return fail(tr("Only an inferred or AI template can be saved."));
    const QString base = name.trimmed();
    if (base.isEmpty())
        return fail(tr("The schema needs a name."));
    const QString dir = SchemaStore::patternsDir();
    if (dir.isEmpty())
        return fail(tr("The schemas folder could not be created."));

    // Имена каталога и уже лежащих файлов (и тех, что каталог не прочёл).
    QSet<QString> taken;
    for (const SchemaStore::SchemaEntry& entry : m_catalogue)
        taken.insert(entry.name.toCaseFolded());
    const QStringList files = QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& file : files) {
        QFile f(QDir(dir).filePath(file));
        if (f.open(QIODevice::ReadOnly)) {
            const QString n = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("name")).toString();
            if (!n.isEmpty())
                taken.insert(n.toCaseFolded());
        }
    }
    QString unique = base;
    for (int k = 2; taken.contains(unique.toCaseFolded()); ++k)
        unique = QStringLiteral("%1 (%2)").arg(base).arg(k);

    QString fileName = SchemaStore::suggestedFileName(unique);
    const QString stem = QFileInfo(fileName).completeBaseName();
    for (int k = 2; QFileInfo::exists(QDir(dir).filePath(fileName)); ++k)
        fileName = QStringLiteral("%1_%2.json").arg(stem).arg(k);

    const Variant& v = m_variants[variant];
    QJsonObject json{
        {QStringLiteral("name"), unique},
        {QStringLiteral("version"), LineTemplateSchema::kVersion},
        {QStringLiteral("type"), QStringLiteral("line-template")},
        {QStringLiteral("template"), v.templateText},
    };
    QString description = v.source == Variant::Source::Ai ? tr("Suggested by AI (%1).").arg(v.model.isEmpty() ? m_ai.model : v.model)
                                                         : tr("Inferred from log lines.");
    if (!v.comment.isEmpty())
        description += QLatin1Char(' ') + v.comment;
    json.insert(QStringLiteral("description"), description);
    QSaveFile file(QDir(dir).filePath(fileName));
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(tr("The schema could not be saved to %1.").arg(QDir::toNativeSeparators(dir)));
    return unique;
}
