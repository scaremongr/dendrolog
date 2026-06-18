#include "schemastore.h"

#include "logpattern.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace {

constexpr auto kSchemaGlob = "*.json";

// Parses raw file/blob contents as a schema. On success fills *name (from the
// embedded "name" key, falling back to \a fallbackName) and *canonical (the
// re-serialized, canonical JSON the rest of the app uses). Returns false for
// anything that is not a recognisable schema.
bool parseSchemaBlob(const QByteArray& bytes,
                     const QString& fallbackName,
                     QString* name,
                     QString* canonical)
{
    const QString text = QString::fromUtf8(bytes).trimmed();
    if (text.isEmpty())
        return false;

    PatternDefinition def;
    // Lenient (strict=false): an in-progress schema that was saved must load
    // without being silently replaced; real validity is enforced at use time.
    if (!LogPattern::deserializeDefinition(text, &def, /*allowLegacy=*/true, /*strict=*/false))
        return false;

    QString embeddedName;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (doc.isObject())
        embeddedName = doc.object().value(QStringLiteral("name")).toString().trimmed();

    if (name)
        *name = embeddedName.isEmpty() ? fallbackName.trimmed() : embeddedName;
    if (canonical)
        *canonical = LogPattern::serializeDefinition(def);
    return true;
}

// Serializes one schema for on-disk storage: the canonical schema JSON with
// the display name injected, pretty-printed for hand-editing.
QByteArray fileBytesFor(const QString& name, const QString& serialized)
{
    QJsonDocument doc = QJsonDocument::fromJson(serialized.toUtf8());
    QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject();
    obj.insert(QStringLiteral("name"), name);
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

bool writeFileAtomic(const QString& path, const QByteArray& bytes, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    const qint64 written = file.write(bytes);
    file.close();
    if (written != bytes.size()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

// Returns a basename (with .json) not present in \a usedLower, deriving
// candidates from \a preferred by appending _2, _3, … before the suffix.
QString uniqueBasename(const QString& preferred, const QSet<QString>& usedLower)
{
    if (!usedLower.contains(preferred.toLower()))
        return preferred;

    QString stem = preferred;
    if (stem.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        stem.chop(5);
    for (int i = 2; ; ++i) {
        const QString candidate = stem + QStringLiteral("_%1.json").arg(i);
        if (!usedLower.contains(candidate.toLower()))
            return candidate;
    }
}

} // namespace

namespace SchemaStore {

QString patternsDir()
{
    const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/patterns");
    QDir dir(path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return QString();
    return path;
}

QString suggestedFileName(const QString& name)
{
    QString base = name.trimmed();
    // Replace characters that are invalid in Windows/Unix file names.
    for (QChar& ch : base) {
        if (QStringLiteral("<>:\"/\\|?*").contains(ch) || ch < QChar(0x20))
            ch = QLatin1Char('_');
    }
    // Windows forbids trailing dots and spaces.
    while (!base.isEmpty() && (base.endsWith(QLatin1Char('.')) || base.endsWith(QLatin1Char(' '))))
        base.chop(1);
    if (base.size() > 80)
        base.truncate(80);
    if (base.isEmpty())
        base = QStringLiteral("schema");
    return base + QStringLiteral(".json");
}

SchemaList loadAll(const QStringList& orderNames)
{
    SchemaList result;
    const QString dirPath = patternsDir();
    if (dirPath.isEmpty())
        return result;

    QDir dir(dirPath);
    const QStringList files =
        dir.entryList(QStringList() << QString::fromLatin1(kSchemaGlob), QDir::Files, QDir::Name);

    SchemaList loaded;
    for (const QString& file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = f.readAll();
        f.close();

        QString name, canonical;
        if (parseSchemaBlob(bytes, QFileInfo(file).completeBaseName(), &name, &canonical))
            loaded.append({name, canonical});
    }

    // Apply the saved display order: listed names first (in order), the rest
    // appended sorted by name.
    QList<bool> taken(loaded.size(), false);
    for (const QString& wanted : orderNames) {
        for (int i = 0; i < loaded.size(); ++i) {
            if (!taken[i] && loaded[i].first == wanted) {
                result.append(loaded[i]);
                taken[i] = true;
                break;
            }
        }
    }
    SchemaList rest;
    for (int i = 0; i < loaded.size(); ++i)
        if (!taken[i])
            rest.append(loaded[i]);
    std::stable_sort(rest.begin(), rest.end(),
        [](const SchemaEntry& a, const SchemaEntry& b) {
            return a.first.compare(b.first, Qt::CaseInsensitive) < 0;
        });
    result.append(rest);
    return result;
}

bool sync(const SchemaList& list, QString* errorMessage)
{
    const QString dirPath = patternsDir();
    if (dirPath.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Could not create the patterns folder.");
        return false;
    }

    QDir dir(dirPath);
    const QStringList files =
        dir.entryList(QStringList() << QString::fromLatin1(kSchemaGlob), QDir::Files, QDir::Name);

    struct Existing {
        QString basename;
        QString name;
        QString canonical;
        bool    claimed = false;
    };
    QList<Existing> existing;
    for (const QString& file : files) {
        QFile f(dir.filePath(file));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QByteArray bytes = f.readAll();
        f.close();
        Existing ex;
        ex.basename = file;
        if (parseSchemaBlob(bytes, QFileInfo(file).completeBaseName(), &ex.name, &ex.canonical))
            existing.append(ex);
    }

    QVector<QString> basenameForEntry(list.size());
    QSet<QString> usedLower;

    // Pass 1 — reuse a matching existing file for each entry (no rewrite).
    for (int e = 0; e < list.size(); ++e) {
        for (Existing& ex : existing) {
            if (!ex.claimed
                    && ex.name == list[e].first
                    && ex.canonical == list[e].second
                    && !usedLower.contains(ex.basename.toLower())) {
                ex.claimed = true;
                basenameForEntry[e] = ex.basename;
                usedLower.insert(ex.basename.toLower());
                break;
            }
        }
    }

    // Pass 2 — write a fresh file for every entry not matched above.
    bool ok = true;
    for (int e = 0; e < list.size(); ++e) {
        if (!basenameForEntry[e].isEmpty())
            continue;
        const QString base = uniqueBasename(suggestedFileName(list[e].first), usedLower);
        usedLower.insert(base.toLower());
        basenameForEntry[e] = base;
        QString writeError;
        if (!writeFileAtomic(dir.filePath(base),
                             fileBytesFor(list[e].first, list[e].second), &writeError)) {
            ok = false;
            if (errorMessage)
                *errorMessage = writeError;
        }
    }

    // Delete files that no longer back any entry.
    for (const QString& file : files) {
        if (!usedLower.contains(file.toLower()))
            dir.remove(file);
    }

    return ok;
}

SchemaEntry importFile(const QString& srcPath, QString* errorMessage)
{
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly)) {
        if (errorMessage)
            *errorMessage = src.errorString();
        return {};
    }
    const QByteArray bytes = src.readAll();
    src.close();

    QString name, canonical;
    if (!parseSchemaBlob(bytes, QFileInfo(srcPath).completeBaseName(), &name, &canonical)) {
        if (errorMessage)
            *errorMessage = QStringLiteral("The file is not a valid schema.");
        return {};
    }

    const QString dirPath = patternsDir();
    if (dirPath.isEmpty()) {
        if (errorMessage)
            *errorMessage = QStringLiteral("Could not create the patterns folder.");
        return {};
    }

    QDir dir(dirPath);
    QSet<QString> usedLower;
    const QStringList existing =
        dir.entryList(QStringList() << QString::fromLatin1(kSchemaGlob), QDir::Files);
    for (const QString& file : existing)
        usedLower.insert(file.toLower());

    const QString base = uniqueBasename(suggestedFileName(name), usedLower);
    QString writeError;
    if (!writeFileAtomic(dir.filePath(base), fileBytesFor(name, canonical), &writeError)) {
        if (errorMessage)
            *errorMessage = writeError;
        return {};
    }
    return {name, canonical};
}

bool exportToFile(const QString& name,
                  const QString& serialized,
                  const QString& destPath,
                  QString* errorMessage)
{
    return writeFileAtomic(destPath, fileBytesFor(name, serialized), errorMessage);
}

} // namespace SchemaStore
