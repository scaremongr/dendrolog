#include "schemaproposal.h"

#include "linetemplate.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>

#include <memory>

// Ресурс системного текста живёт в статической библиотеке ядра: без явной
// инициализации линкер выбросил бы его инициализатор (CLAUDE.md, «Архитектура»).
static void initAiResources()
{
    Q_INIT_RESOURCE(ai);
}

namespace SchemaProposal {

namespace {

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isHex(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
}

bool isEmailLocal(QChar c)
{
    const char16_t u = c.unicode();
    return (u < 0x80 && (isWordChar(c) || u == u'.' || u == u'%' || u == u'+' || u == u'-'));
}

bool isDomainChar(QChar c)
{
    const char16_t u = c.unicode();
    return u < 0x80 && (c.isLetterOrNumber() || u == u'-');
}

// Почта с позиции: local@label(.label)+, за ней не буква и не цифра.
qsizetype emailAt(const QString& s, qsizetype pos)
{
    const qsizetype n = s.size();
    qsizetype i = pos;
    while (i < n && isEmailLocal(s[i]))
        ++i;
    if (i == pos || i >= n || s[i] != QLatin1Char('@'))
        return -1;
    ++i;
    int labels = 0;
    qsizetype end = -1;
    while (i < n) {
        const qsizetype start = i;
        while (i < n && isDomainChar(s[i]))
            ++i;
        if (i == start)
            break;
        ++labels;
        end = i;
        if (i < n && s[i] == QLatin1Char('.') && i + 1 < n && isDomainChar(s[i + 1]))
            ++i;
        else
            break;
    }
    if (labels < 2 || end < 0 || (end < n && isWordChar(s[end])))
        return -1;
    return end - pos;
}

// Слово из hex-знаков (с «0x»), целиком: за ним не буква, цифра и «_».
qsizetype hexWordAt(const QString& s, qsizetype pos)
{
    const qsizetype n = s.size();
    qsizetype i = pos;
    if (i + 1 < n && s[i] == QLatin1Char('0') && (s[i + 1] == QLatin1Char('x') || s[i + 1] == QLatin1Char('X')))
        i += 2;
    const bool prefixed = i > pos;
    const qsizetype digits = i;
    bool letter = false;
    while (i < n && isHex(s[i])) {
        letter |= !s[i].isDigit();
        ++i;
    }
    if (i == digits || (i < n && isWordChar(s[i])))
        return -1;
    // Только цифры — число (эпоха, id): его маскирует правило «цифры — 0».
    if (!letter && !prefixed)
        return -1;
    return i - pos;
}

QString docAddress(const QString& ip, QHash<QString, QString>* map)
{
    const auto it = map->constFind(ip);
    if (it != map->constEnd())
        return *it;
    const bool v6 = ip.contains(QLatin1Char(':'));
    int index = 0;
    for (const QString& v : std::as_const(*map))
        if (v.contains(QLatin1Char(':')) == v6)
            ++index;
    QString masked;
    if (v6) {
        masked = QStringLiteral("2001:db8::") + QString::number(index + 1, 16);
    } else {
        // Три документационных диапазона IPv4 (RFC 5737) по 254 адреса.
        static const char* const kNets[] = {"192.0.2.", "198.51.100.", "203.0.113."};
        const int slot = index % (3 * 254);
        masked = QString::fromLatin1(kNets[slot / 254]) + QString::number(slot % 254 + 1);
    }
    map->insert(ip, masked);
    return masked;
}

QString preparedLine(const QString& line, const MaskOptions& options, QHash<QString, QString>* ipMap)
{
    QString text = line;
    // Конец строки и управляющие символы в запрос не идут.
    while (!text.isEmpty() && (text.back() == QLatin1Char('\r') || text.back() == QLatin1Char('\n')))
        text.chop(1);
    if (options.mask)
        text = maskLine(text, ipMap);
    if (options.cut && options.cutChars > 0 && text.size() > options.cutChars)
        text.truncate(options.cutChars);
    return text;
}

QJsonObject message(const QString& role, const QString& text)
{
    return QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), text}};
}

QJsonObject outputSchema()
{
    const QJsonObject stringType{{QStringLiteral("type"), QStringLiteral("string")}};
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("template"), stringType}, {QStringLiteral("comment"), stringType}}},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("template"), QStringLiteral("comment")}},
        {QStringLiteral("additionalProperties"), false},
    };
}

QString stripFences(QString text)
{
    text = text.trimmed();
    if (text.startsWith(QLatin1String("```"))) {
        const qsizetype nl = text.indexOf(QLatin1Char('\n'));
        text = nl < 0 ? QString() : text.mid(nl + 1);
        if (text.endsWith(QLatin1String("```")))
            text.chop(3);
        text = text.trimmed();
    }
    return text;
}

} // namespace

QString maskLine(const QString& line, QHash<QString, QString>* ipMap)
{
    QHash<QString, QString> local;
    QHash<QString, QString>* map = ipMap ? ipMap : &local;
    QString out;
    out.reserve(line.size());
    const qsizetype n = line.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = line[i];
        const bool boundary = i == 0 || !isWordChar(line[i - 1]);
        if (boundary) {
            qsizetype len = TokenScan::timeAt(line, i);
            if (len > 0) {
                out += QStringView(line).mid(i, len);
                i += len;
                continue;
            }
            if (c.isLetter()) {
                len = TokenScan::levelAt(line, i);
                if (len > 0 && (i + len >= n || !isWordChar(line[i + len]))) {
                    out += QStringView(line).mid(i, len);
                    i += len;
                    continue;
                }
            }
            len = emailAt(line, i);
            if (len > 0) {
                out += QStringLiteral("user@example.com");
                i += len;
                continue;
            }
            len = TokenScan::uuidAt(line, i);
            if (len > 0) {
                for (qsizetype k = i; k < i + len; ++k)
                    out += line[k] == QLatin1Char('-') ? QLatin1Char('-') : QLatin1Char('a');
                i += len;
                continue;
            }
            len = TokenScan::ipAt(line, i);
            if (len > 0) {
                out += docAddress(line.mid(i, len), map);
                i += len;
                continue;
            }
            len = hexWordAt(line, i);
            if (len >= 8) {
                out += QString(len, QLatin1Char('a'));
                i += len;
                continue;
            }
        }
        out += c.isDigit() ? QLatin1Char('0') : c;
        ++i;
    }
    return out;
}

QStringList prepareLines(const QStringList& lines, const MaskOptions& options, QHash<QString, QString>* ipMap)
{
    QHash<QString, QString> local;
    QStringList out;
    out.reserve(lines.size());
    for (const QString& line : lines)
        out.append(preparedLine(line, options, ipMap ? ipMap : &local));
    return out;
}

QStringList pickLines(const QVector<SchemaFit::Sample>& samples, const MaskOptions& options, int maxLines,
                      QHash<QString, QString>* ipMapOut)
{
    if (maxLines <= 0 || samples.isEmpty())
        return {};
    const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
    QVector<int> pool;
    for (int i = 0; i < corpus.samples.size(); ++i)
        if (corpus.nonEmpty.value(i) && (corpus.candidateCount == 0 || corpus.candidate.value(i)))
            pool.append(i);

    // Порядок просмотра: начало файла, затем равномерно по остальным, затем
    // остальные по порядку (если повторы съели место).
    QVector<int> order;
    QVector<bool> taken(pool.size(), false);
    const int head = qMin(int(pool.size()), maxLines / 2);
    for (int k = 0; k < head; ++k) {
        order.append(pool[k]);
        taken[k] = true;
    }
    const int rest = int(pool.size()) - head;
    const int spread = maxLines - head;
    if (rest > 0 && spread > 0) {
        for (int k = 0; k < spread; ++k) {
            const int at = head + int(qint64(k) * rest / spread);
            if (!taken[at]) {
                taken[at] = true;
                order.append(pool[at]);
            }
        }
    }
    for (int k = 0; k < pool.size(); ++k)
        if (!taken[k])
            order.append(pool[k]);

    QHash<QString, QString> local;
    QHash<QString, QString>* ipMap = ipMapOut ? ipMapOut : &local;
    QSet<QString> seen;
    QStringList out;
    for (int index : std::as_const(order)) {
        if (out.size() >= maxLines)
            break;
        const QString text = preparedLine(corpus.samples[index].text, options, ipMap);
        if (text.trimmed().isEmpty() || seen.contains(text))
            continue;
        seen.insert(text);
        out.append(text);
    }
    return out;
}

QString systemPrompt()
{
    initAiResources();
    QFile file(QStringLiteral(":/ai/schema-system.txt"));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QString userText(const QStringList& preparedLines)
{
    QString text = QStringLiteral(
        "Here are %1 sample lines of one log file, one line per row between <lines> and </lines>. "
        "Write a line template that splits them into fields.\n<lines>\n")
                       .arg(preparedLines.size());
    for (const QString& line : preparedLines) {
        text += line;
        text += QLatin1Char('\n');
    }
    text += QStringLiteral("</lines>");
    return text;
}

QString previousAnswerText(const Refinement& refinement)
{
    const QJsonObject previous{{QStringLiteral("template"), refinement.previousTemplate},
                               {QStringLiteral("comment"), refinement.previousComment}};
    return QString::fromUtf8(QJsonDocument(previous).toJson(QJsonDocument::Compact));
}

QString refinementText(const Refinement& refinement)
{
    QString text = QStringLiteral("The template did not work: %1").arg(refinement.problem);
    if (!refinement.failedLines.isEmpty()) {
        text += QStringLiteral("\nLines it does not parse:\n<lines>\n");
        for (const QString& line : refinement.failedLines) {
            text += line;
            text += QLatin1Char('\n');
        }
        text += QStringLiteral("</lines>");
    }
    text += QStringLiteral("\nReturn a corrected template.");
    return text;
}

static bool supportsFallbacks(const QString& model)
{
    return model.startsWith(QLatin1String("claude-opus-5")) || model.startsWith(QLatin1String("claude-fable-5"))
        || model.startsWith(QLatin1String("claude-mythos-5")) || model == QLatin1String("claude-sonnet-5-5");
}

QByteArray requestBody(const QStringList& preparedLines, const RequestOptions& options,
                       const Refinement& refinement)
{
    QJsonArray messages;
    messages.append(message(QStringLiteral("user"), userText(preparedLines)));
    if (!refinement.isEmpty()) {
        messages.append(message(QStringLiteral("assistant"), previousAnswerText(refinement)));
        messages.append(message(QStringLiteral("user"), refinementText(refinement)));
    }
    QJsonObject body{
        {QStringLiteral("model"), options.model},
        {QStringLiteral("max_tokens"), options.maxTokens},
        {QStringLiteral("system"), systemPrompt()},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("output_config"),
         QJsonObject{{QStringLiteral("effort"), options.effort},
                     {QStringLiteral("format"),
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                  {QStringLiteral("schema"), outputSchema()}}}}},
    };
    if (options.effort.isEmpty())
        body.insert(QStringLiteral("output_config"),
                    QJsonObject{{QStringLiteral("format"),
                                 QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                                             {QStringLiteral("schema"), outputSchema()}}}});
    // Классификатор безопасности может отклонить строки журнала — сервер сам
    // повторит на запасной модели (заголовок беты — requestHeaders).
    if (supportsFallbacks(options.model))
        body.insert(QStringLiteral("fallbacks"), QStringLiteral("default"));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QList<QPair<QByteArray, QByteArray>> requestHeaders(const RequestOptions& options)
{
    QList<QPair<QByteArray, QByteArray>> headers;
    headers.append({QByteArrayLiteral("content-type"), QByteArrayLiteral("application/json")});
    headers.append({QByteArrayLiteral("anthropic-version"), QByteArray(kApiVersion)});
    if (supportsFallbacks(options.model))
        headers.append({QByteArrayLiteral("anthropic-beta"), QByteArray(kBetaFallback)});
    return headers;
}

Reply parseReply(const QByteArray& body)
{
    Reply reply;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        reply.status = Reply::Status::NotJson;
        reply.message = QStringLiteral("The server's answer is not a Messages API response.");
        return reply;
    }
    const QJsonObject root = doc.object();
    reply.model = root.value(QStringLiteral("model")).toString();
    const QString stop = root.value(QStringLiteral("stop_reason")).toString();
    // stop_reason — до чтения содержимого.
    if (stop == QLatin1String("refusal")) {
        reply.status = Reply::Status::Refused;
        const QString why = root.value(QStringLiteral("stop_details")).toObject()
                                .value(QStringLiteral("explanation")).toString();
        reply.message = QStringLiteral("The model declined the request.");
        if (!why.isEmpty())
            reply.message += QLatin1Char(' ') + why;
        return reply;
    }
    if (stop == QLatin1String("max_tokens")) {
        reply.status = Reply::Status::Truncated;
        reply.message = QStringLiteral("The answer was cut off (the output limit was reached).");
        return reply;
    }
    QString text;
    bool found = false;
    for (const QJsonValue& block : root.value(QStringLiteral("content")).toArray()) {
        const QJsonObject b = block.toObject();
        if (b.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
            text = b.value(QStringLiteral("text")).toString();
            found = true;
            break;
        }
    }
    if (!found) {
        reply.status = Reply::Status::NoTemplate;
        reply.message = QStringLiteral("The answer has no text.");
        return reply;
    }
    const QJsonDocument inner = QJsonDocument::fromJson(stripFences(text).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !inner.isObject()) {
        reply.status = Reply::Status::NotJson;
        reply.message = QStringLiteral("The answer is not the expected JSON.");
        return reply;
    }
    const QJsonValue tpl = inner.object().value(QStringLiteral("template"));
    if (!tpl.isString() || tpl.toString().trimmed().isEmpty()) {
        reply.status = Reply::Status::NoTemplate;
        reply.message = QStringLiteral("The answer has no template.");
        return reply;
    }
    reply.status = Reply::Status::Ok;
    reply.templateText = tpl.toString();
    reply.comment = inner.object().value(QStringLiteral("comment")).toString();
    return reply;
}

Check checkTemplate(const QString& templateText, const QString& comment,
                    const QVector<SchemaFit::Sample>& lines, bool finalRound,
                    const SchemaFit::Budget* budget)
{
    Check check;
    check.templateText = templateText;
    // Перевод строки в конце — частая обёртка, а не часть шаблона.
    while (!check.templateText.isEmpty()
           && (check.templateText.back() == QLatin1Char('\n') || check.templateText.back() == QLatin1Char('\r')))
        check.templateText.chop(1);
    check.comment = comment;
    check.refinement.previousTemplate = check.templateText;
    check.refinement.previousComment = comment;

    if (check.templateText.size() > kMaxTemplateChars) {
        check.status = Check::Status::Rejected;
        check.message = QStringLiteral("The suggested template is too long (%1 characters; the limit is %2).")
                            .arg(check.templateText.size())
                            .arg(kMaxTemplateChars);
        return check;
    }
    auto tpl = std::make_shared<const LineTemplate>(check.templateText);
    int fields = 0;
    for (const LineTemplateElement& e : tpl->elements())
        if (e.type == LineTemplateElement::Type::Field)
            ++fields;
    if (fields > kMaxFields) {
        check.status = Check::Status::Rejected;
        check.message = QStringLiteral("The suggested template has %1 fields; the limit is %2.")
                            .arg(fields)
                            .arg(kMaxFields);
        return check;
    }
    if (!tpl->isValid()) {
        const LineTemplateError& error = tpl->error();
        check.message = QStringLiteral("it does not compile: %1 (at character %2)")
                            .arg(error.message)
                            .arg(error.position + 1);
        check.refinement.problem = check.message;
        check.status = finalRound ? Check::Status::Rejected : Check::Status::NeedsRefinement;
        if (finalRound)
            check.message = QStringLiteral("The suggested template does not compile: %1 (at character %2).")
                                .arg(error.message)
                                .arg(error.position + 1);
        return check;
    }
    check.slow = tpl->hasRegex();

    SchemaFit::Budget local;
    local.maxMs = kCheckBudgetMs;
    const SchemaFit::Budget& b = budget ? *budget : local;
    const TemplateExtractor extractor(tpl);
    check.fit = SchemaFit::evaluate(extractor, lines, b);
    if (check.fit.cancelled) {
        check.status = Check::Status::Rejected;
        check.message = QStringLiteral("Cancelled.");
        return check;
    }
    if (check.fit.timedOut) {
        check.status = Check::Status::Rejected;
        check.message = QStringLiteral("The suggested template is too slow on the lines of this log "
                                       "(a heavy regular expression).");
        return check;
    }
    const double coverage = check.fit.coverage();
    const bool tooFew = check.fit.specificity() < SchemaFit::kMinSpecificity;
    if (coverage >= kAcceptCoverage && !tooFew) {
        check.status = Check::Status::Accepted;
        return check;
    }
    if (tooFew)
        check.refinement.problem = QStringLiteral("it does not split the lines into fields: "
                                                  "the lines must give at least two fields besides the last one");
    else
        check.refinement.problem = QStringLiteral("it parses %1% of the record lines; most of them must parse")
                                       .arg(SchemaFit::percent(coverage));
    for (const SchemaFit::Miss& miss : std::as_const(check.fit.misses)) {
        if (check.refinement.failedLines.size() >= kRefineLines)
            break;
        check.refinement.failedLines.append(miss.text);
    }
    if (!finalRound) {
        check.status = Check::Status::NeedsRefinement;
        check.message = check.refinement.problem;
        return check;
    }
    if (tooFew) {
        check.status = Check::Status::Rejected;
        check.message = QStringLiteral("The suggested template does not split the lines into fields.");
        return check;
    }
    // Второй круг: компилирующийся шаблон показывается с тем покрытием, что
    // есть, — решает пользователь.
    check.status = Check::Status::Accepted;
    return check;
}

} // namespace SchemaProposal
