#include "schemastore.h"

#include "appsettings.h"
#include "builtinschemas.h"
#include "logpattern.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace {

constexpr auto kSchemaGlob = "*.json";

QByteArray readAllFrom(const QString& path, bool* ok)
{
    QFile f(path);
    *ok = f.open(QIODevice::ReadOnly);
    return *ok ? f.readAll() : QByteArray();
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

// Stem of a file name the store derived from a display name: the suggested
// name itself or its _N variant (see uniqueBasename).
bool isDerivedFileName(const QString& basename, const QString& name)
{
    QString stem = SchemaStore::suggestedFileName(name);
    stem.chop(5);   // ".json"
    if (!basename.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        return false;
    const QString base = basename.left(basename.size() - 5);
    if (base.compare(stem, Qt::CaseInsensitive) == 0)
        return true;
    if (!base.startsWith(stem + QLatin1Char('_'), Qt::CaseInsensitive))
        return false;
    bool numeric = false;
    base.mid(stem.size() + 1).toInt(&numeric);
    return numeric;
}

} // namespace

namespace SchemaStore {

bool isSchemaFormatKey(const QString& key)
{
    // What LogPattern::serializeDefinition writes, plus the display name.
    return key == QLatin1String("name")
        || key == QLatin1String("version")
        || key == QLatin1String("type")
        || key == QLatin1String("linePrefix")
        || key == QLatin1String("blocks");
}

QString SchemaEntry::description() const
{
    return meta.value(QStringLiteral("description")).toString();
}

void SchemaEntry::setDescription(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        meta.remove(QStringLiteral("description"));
    else
        meta.insert(QStringLiteral("description"), trimmed);
}

bool parseEntry(const QByteArray& bytes, const QString& fallbackName, SchemaEntry* entry)
{
    // Lenient (strict=false): an in-progress schema that was saved must load
    // without being silently replaced; real validity is enforced at use time.
    SchemaEntry parsed;
    if (!BuiltinSchemas::parseSchemaFile(bytes, fallbackName, &parsed.name, &parsed.schema,
                                         /*strict=*/false))
        return false;
    // Same text parseSchemaFile looks at (trimmed, BOM-safe).
    const QJsonDocument doc =
        QJsonDocument::fromJson(QString::fromUtf8(bytes).trimmed().toUtf8());
    if (doc.isObject()) {
        const QJsonObject root = doc.object();
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!isSchemaFormatKey(it.key()))
                parsed.meta.insert(it.key(), it.value());
        }
    }
    if (entry)
        *entry = parsed;
    return true;
}

QByteArray fileBytes(const SchemaEntry& entry)
{
    // Metadata first, so a stray format key in it can never shadow the schema.
    QJsonObject obj = entry.meta;
    const QJsonDocument doc = QJsonDocument::fromJson(entry.schema.toUtf8());
    if (doc.isObject()) {
        const QJsonObject schema = doc.object();
        for (auto it = schema.begin(); it != schema.end(); ++it)
            obj.insert(it.key(), it.value());
    }
    obj.insert(QStringLiteral("name"), entry.name);
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QString patternsDir()
{
    const QString path = AppSettings::configDir() + QStringLiteral("/patterns");
    QDir dir(path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return QString();

    // Одноразовая миграция схем из старого расположения <exe>/patterns
    // (до v0.1 схемы всегда жили рядом с исполняемым файлом). В portable-
    // режиме старый и новый каталоги совпадают — цикл ничего не копирует.
    const QDir legacy(QCoreApplication::applicationDirPath() + QStringLiteral("/patterns"));
    if (legacy.exists() && legacy.absolutePath() != dir.absolutePath()) {
        const QStringList jsons = legacy.entryList({QStringLiteral("*.json")}, QDir::Files);
        for (const QString& name : jsons) {
            if (!dir.exists(name))
                QFile::copy(legacy.absoluteFilePath(name), dir.absoluteFilePath(name));
        }
    }
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

BuiltinSchemas::InstallReport installBuiltins()
{
    const QString dirPath = patternsDir();
    if (dirPath.isEmpty())
        return {};
    BuiltinSchemas::InstallReport report =
        BuiltinSchemas::install(QStringLiteral(":/patterns"), dirPath);
    for (const QString& error : std::as_const(report.errors))
        qWarning().noquote() << "Built-in schemas:" << error;
    return report;
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
        bool ok = false;
        const QByteArray bytes = readAllFrom(dir.filePath(file), &ok);
        SchemaEntry entry;
        if (ok && parseEntry(bytes, QFileInfo(file).completeBaseName(), &entry)) {
            entry.fileName = file;
            loaded.append(entry);
        }
    }

    // Apply the saved display order: listed names first (in order), the rest
    // appended sorted by name.
    QList<bool> taken(loaded.size(), false);
    for (const QString& wanted : orderNames) {
        for (int i = 0; i < loaded.size(); ++i) {
            if (!taken[i] && loaded[i].name == wanted) {
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
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
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
        QString     basename;
        SchemaEntry entry;
        bool        parsed = false;   // unparsable files are still claimable by fileName
    };
    QList<Existing> existing;
    QHash<QString, int> existingByLower;
    for (const QString& file : files) {
        Existing ex;
        ex.basename = file;
        bool ok = false;
        const QByteArray bytes = readAllFrom(dir.filePath(file), &ok);
        ex.parsed = ok && parseEntry(bytes, QFileInfo(file).completeBaseName(), &ex.entry);
        existingByLower.insert(file.toLower(), existing.size());
        existing.append(ex);
    }

    QVector<QString> basenameForEntry(list.size());
    QVector<bool> needsWrite(list.size(), false);
    QSet<QString> usedLower;

    // Pass 1 — the entry's own file, rewritten in place when its content
    // changed. A rename moves a file whose name was derived from the old
    // display name (pass 3 gives it a new one); a curated or built-in file
    // name stays.
    for (int e = 0; e < list.size(); ++e) {
        const SchemaEntry& entry = list[e];
        if (entry.fileName.isEmpty())
            continue;
        const QString lower = entry.fileName.toLower();
        const auto it = existingByLower.constFind(lower);
        if (it == existingByLower.constEnd() || usedLower.contains(lower))
            continue;
        const Existing& ex = existing[*it];
        if (ex.parsed && ex.entry.name != entry.name
                && isDerivedFileName(ex.basename, ex.entry.name)
                && !isDerivedFileName(ex.basename, entry.name))
            continue;
        basenameForEntry[e] = ex.basename;
        usedLower.insert(lower);
        needsWrite[e] = !(ex.parsed && ex.entry.sameContent(entry));
    }

    // Pass 2 — an entry without a file of its own reuses a file with the same
    // content (no rewrite).
    for (int e = 0; e < list.size(); ++e) {
        if (!basenameForEntry[e].isEmpty())
            continue;
        for (const Existing& ex : std::as_const(existing)) {
            if (ex.parsed && ex.entry.sameContent(list[e])
                    && !usedLower.contains(ex.basename.toLower())) {
                basenameForEntry[e] = ex.basename;
                usedLower.insert(ex.basename.toLower());
                break;
            }
        }
    }

    // A file that did not parse (a schema of a newer format, a file broken by
    // a manual edit) is not ours: it is neither deleted below nor overwritten
    // by a fresh file of pass 3. An entry can still claim it by its fileName
    // (pass 1) — then it was rewritten above as that entry's file.
    for (const Existing& ex : std::as_const(existing)) {
        if (!ex.parsed)
            usedLower.insert(ex.basename.toLower());
    }

    // Pass 3 — a fresh file for every entry still without one.
    for (int e = 0; e < list.size(); ++e) {
        if (!basenameForEntry[e].isEmpty())
            continue;
        const QString base = uniqueBasename(suggestedFileName(list[e].name), usedLower);
        usedLower.insert(base.toLower());
        basenameForEntry[e] = base;
        needsWrite[e] = true;
    }

    bool ok = true;
    for (int e = 0; e < list.size(); ++e) {
        if (!needsWrite[e])
            continue;
        QString writeError;
        if (!writeFileAtomic(dir.filePath(basenameForEntry[e]), fileBytes(list[e]), &writeError)) {
            ok = false;
            if (errorMessage)
                *errorMessage = writeError;
        }
    }

    // Delete files of schemas that no longer back any entry (unparsed files
    // are in usedLower and stay).
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

    SchemaEntry entry;
    if (!parseEntry(bytes, QFileInfo(srcPath).completeBaseName(), &entry)) {
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

    const QString base = uniqueBasename(suggestedFileName(entry.name), usedLower);
    QString writeError;
    if (!writeFileAtomic(dir.filePath(base), fileBytes(entry), &writeError)) {
        if (errorMessage)
            *errorMessage = writeError;
        return {};
    }
    entry.fileName = base;
    return entry;
}

bool exportToFile(const SchemaEntry& entry,
                  const QString& destPath,
                  QString* errorMessage)
{
    return writeFileAtomic(destPath, fileBytes(entry), errorMessage);
}

} // namespace SchemaStore
