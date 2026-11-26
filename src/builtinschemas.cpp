#include "builtinschemas.h"
#include "linetemplate.h"

#include "logpattern.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

constexpr int kManifestVersion = 1;
const QLatin1String kStateInstalled("installed");
const QLatin1String kStateDeclined("declined");

struct ParsedSchema {
    QString name;
    QString canonical;
    QString fingerprint;
};

// Отпечаток — от отображаемого имени и канонической схемы, а не от байтов
// файла: переформатирование, другой перевод строк или описание не делают
// схему «изменённой пользователем».
QString fingerprintOf(const QString& name, const QString& canonical)
{
    const QByteArray key = (name + QLatin1Char('\n') + canonical).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

bool readFile(const QString& path, QByteArray* bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    *bytes = f.readAll();
    return true;
}

bool parseFileAt(const QString& path, ParsedSchema* out, bool strict)
{
    QByteArray bytes;
    if (!readFile(path, &bytes))
        return false;
    if (!BuiltinSchemas::parseSchemaFile(bytes, QFileInfo(path).completeBaseName(),
                                         &out->name, &out->canonical, strict))
        return false;
    out->fingerprint = fingerprintOf(out->name, out->canonical);
    return true;
}

bool writeFile(const QString& path, const QByteArray& bytes, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        *error = QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        return false;
    }
    return true;
}

} // namespace

namespace BuiltinSchemas {

bool parseSchemaFile(const QByteArray& bytes, const QString& fallbackName,
                     QString* name, QString* canonical, bool strict)
{
    const QString text = QString::fromUtf8(bytes).trimmed();
    if (text.isEmpty())
        return false;

    // Шаблон строки, версия 4 (F-17): определение — {type, version, template}
    // компактно, имя — отдельно, как у блочных схем.
    {
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        const QJsonObject obj = doc.object();
        if (doc.isObject() && obj.value(QStringLiteral("type")).toString()
                                  == QLatin1String("line-template")) {
            QString templateText;
            if (!LineTemplateSchema::templateOf(text, &templateText))
                return false;
            if (strict && !LineTemplate(templateText).isValid())
                return false;
            const QString embedded = obj.value(QStringLiteral("name")).toString().trimmed();
            if (name)
                *name = embedded.isEmpty() ? fallbackName.trimmed() : embedded;
            if (canonical)
                *canonical = LineTemplateSchema::definition(templateText);
            return true;
        }
    }

    PatternDefinition def;
    if (!LogPattern::deserializeDefinition(text, &def, /*allowLegacy=*/true, strict))
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

InstallReport install(const QString& sourceDir, const QString& targetDir)
{
    InstallReport report;

    const QDir source(sourceDir);
    const QStringList shipped =
        source.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    if (shipped.isEmpty())
        return report;

    QDir target(targetDir);
    if (!target.exists() && !target.mkpath(QStringLiteral("."))) {
        report.errors << QStringLiteral("%1: cannot create the folder")
                             .arg(QDir::toNativeSeparators(targetDir));
        return report;
    }

    // Манифест. Нечитаемый или незнакомой версии — как отсутствующий: это
    // безопасно (см. заголовок), пользовательские файлы не перезаписываются.
    const QString manifestPath = target.filePath(QString::fromLatin1(kManifestFileName));
    QJsonObject entries;
    {
        QByteArray bytes;
        if (readFile(manifestPath, &bytes)) {
            const QJsonObject root = QJsonDocument::fromJson(bytes).object();
            if (root.value(QStringLiteral("version")).toInt() == kManifestVersion)
                entries = root.value(QStringLiteral("schemas")).toObject();
        }
    }
    bool manifestDirty = false;

    // Отображаемые имена схем, уже лежащих в каталоге, — лениво, один раз.
    QHash<QString, QString> fileOfName;   // имя схемы -> файл
    bool namesScanned = false;
    const auto scanNames = [&] {
        if (namesScanned)
            return;
        namesScanned = true;
        const QStringList files =
            target.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& file : files) {
            ParsedSchema p;
            if (parseFileAt(target.filePath(file), &p, /*strict=*/false))
                fileOfName.insert(p.name, file);
        }
    };

    for (const QString& file : shipped) {
        QByteArray shippedBytes;
        ParsedSchema builtin;
        if (!readFile(source.filePath(file), &shippedBytes)
                || !parseSchemaFile(shippedBytes, QFileInfo(file).completeBaseName(),
                                    &builtin.name, &builtin.canonical, /*strict=*/true)) {
            report.errors << QStringLiteral("%1: not a valid schema").arg(file);
            continue;
        }
        builtin.fingerprint = fingerprintOf(builtin.name, builtin.canonical);

        const QString targetPath = target.filePath(file);
        const bool exists = QFileInfo::exists(targetPath);
        const QJsonObject record = entries.value(file).toObject();
        const QString state = record.value(QStringLiteral("state")).toString();

        const auto markInstalled = [&] {
            QJsonObject r;
            r.insert(QStringLiteral("state"), kStateInstalled);
            r.insert(QStringLiteral("fingerprint"), builtin.fingerprint);
            entries.insert(file, r);
            manifestDirty = true;
        };
        const auto markDeclined = [&] {
            QJsonObject r;
            r.insert(QStringLiteral("state"), kStateDeclined);
            entries.insert(file, r);
            manifestDirty = true;
            report.declined << file;
        };

        if (state.isEmpty()) {
            // Первая встреча с этой встроенной схемой.
            if (exists) {
                ParsedSchema onDisk;
                if (parseFileAt(targetPath, &onDisk, false)
                        && onDisk.fingerprint == builtin.fingerprint)
                    markInstalled();
                else
                    markDeclined();
                continue;
            }
            scanNames();
            if (fileOfName.contains(builtin.name)) {
                markDeclined();
                continue;
            }
            QString error;
            if (!writeFile(targetPath, shippedBytes, &error)) {
                report.errors << error;   // манифест не трогаем — попытка в следующий раз
                continue;
            }
            fileOfName.insert(builtin.name, file);
            report.installed << file;
            markInstalled();
            continue;
        }

        if (state != kStateInstalled || !exists)
            continue;   // declined, или пользователь удалил схему

        const QString recorded = record.value(QStringLiteral("fingerprint")).toString();
        if (recorded == builtin.fingerprint)
            continue;   // встроенная не менялась

        ParsedSchema onDisk;
        if (!parseFileAt(targetPath, &onDisk, false) || onDisk.fingerprint != recorded)
            continue;   // пользователь изменил схему — она его

        QString error;
        if (!writeFile(targetPath, shippedBytes, &error)) {
            report.errors << error;
            continue;
        }
        report.updated << file;
        report.replaced.append({onDisk.canonical, builtin.canonical});
        markInstalled();
    }

    if (manifestDirty) {
        QJsonObject root;
        root.insert(QStringLiteral("version"), kManifestVersion);
        root.insert(QStringLiteral("schemas"), entries);
        QString error;
        if (!writeFile(manifestPath, QJsonDocument(root).toJson(QJsonDocument::Indented), &error))
            report.errors << error;
    }
    return report;
}

} // namespace BuiltinSchemas
