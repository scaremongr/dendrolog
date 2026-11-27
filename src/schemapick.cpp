#include "schemapick.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <numeric>

namespace SchemaPick {

QString nameKey(const QString& fileName)
{
    QString name = fileName.trimmed().toLower();
    static const QString kGzip = QStringLiteral(".gz");
    if (name.endsWith(kGzip) && name.size() > kGzip.size())
        name.chop(kGzip.size());
    // Хвост ротации: разделитель и группы цифр (app.log.1, app.log-20260101,
    // app.log.2026-01-01); от имени что-то должно остаться.
    static const QRegularExpression tail(QStringLiteral("(?:[._-]\\d+)+$"));
    const QRegularExpressionMatch m = tail.match(name);
    if (m.hasMatch() && m.capturedStart() > 0)
        name.truncate(m.capturedStart());
    static const QRegularExpression digits(QStringLiteral("\\d+"));
    name.replace(digits, QStringLiteral("#"));
    return name;
}

static bool coverageAtLeast(const SchemaFit::Result& r, double share)
{
    return r.valid && r.specificity() >= SchemaFit::kMinSpecificity && r.coverage() >= share;
}

Choice choose(const Input& in)
{
    const auto at = [&in](int i) -> const SchemaFit::Result* {
        return i >= 0 && i < in.results.size() ? &in.results[i] : nullptr;
    };
    if (in.nameNone)
        return {Source::NameNone, -1};
    if (const SchemaFit::Result* r = at(in.nameIndex); r && coverageAtLeast(*r, kNameCoverage))
        return {Source::Name, in.nameIndex};
    const int count = int(qMin<qsizetype>(in.catalogueCount, in.results.size()));
    int best = -1;
    for (int i = 0; i < count; ++i)
        if (best < 0 || SchemaFit::better(in.results[i], in.results[best]))
            best = i;
    if (best >= 0 && SchemaFit::verdict(in.results[best]) == SchemaFit::Verdict::Fits)
        return {Source::Content, best};
    if (const SchemaFit::Result* r = at(in.defaultIndex); r && coverageAtLeast(*r, kDefaultCoverage))
        return {Source::Default, in.defaultIndex};
    return {};
}

QVector<int> fitsFirst(const QVector<SchemaFit::Result>& results)
{
    QVector<int> fits, rest;
    for (int i = 0; i < results.size(); ++i)
        (SchemaFit::verdict(results[i]) == SchemaFit::Verdict::Fits ? fits : rest).push_back(i);
    std::stable_sort(fits.begin(), fits.end(),
                     [&results](int a, int b) { return SchemaFit::better(results[a], results[b]); });
    return fits + rest;
}

std::optional<QString> NameTable::lookup(const QString& key) const
{
    for (const auto& e : m_entries)
        if (e.first == key)
            return e.second;
    return std::nullopt;
}

void NameTable::remember(const QString& key, const QString& schemaName)
{
    if (key.isEmpty())
        return;
    for (qsizetype i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].first == key) {
            m_entries.removeAt(i);
            break;
        }
    m_entries.prepend({key, schemaName});
    while (m_entries.size() > kMaxNames)
        m_entries.removeLast();
}

QString NameTable::toJson() const
{
    QJsonArray array;
    for (const auto& e : m_entries)
        array.append(QJsonObject{{QStringLiteral("name"), e.first}, {QStringLiteral("schema"), e.second}});
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

NameTable NameTable::fromJson(const QString& json)
{
    NameTable table;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isArray())
        return table;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString key = o.value(QLatin1String("name")).toString();
        if (key.isEmpty() || !o.value(QLatin1String("schema")).isString()
            || table.lookup(key) || table.m_entries.size() >= kMaxNames)
            continue;
        table.m_entries.append({key, o.value(QLatin1String("schema")).toString()});
    }
    return table;
}

} // namespace SchemaPick
