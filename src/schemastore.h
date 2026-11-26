#ifndef SCHEMASTORE_H
#define SCHEMASTORE_H

#include "builtinschemas.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// ---------------------------------------------------------------------------
// SchemaStore — file-based persistence for field schemas.
//
// Each schema is one *.json file in <configDir>/patterns/. A file holds a line
// template (version 4: {"type":"line-template","version":4,"template":…},
// linetemplate.h) plus a top-level "name" key carrying the display name and
// metadata ("description", unknown keys — kept verbatim), so a schema file is
// fully self-describing and portable between machines.
//
// Files of the previous format (version 3 blocks) are translated once at
// start-up (migrateToTemplates, F-17 step 9): the original goes to
// converted-from-v3/, the file becomes version 4 under the same name with
// the same description and unknown keys. Until then (a file dropped in by
// hand) parseEntry reads it translated.
//
// The folder is the single source of truth for the schema *list*; the INI
// file only remembers the display order and which schema is active. Any
// *.json dropped into the folder by hand is picked up on the next load
// ("drop-in"), and Import simply copies a file into the folder.
// ---------------------------------------------------------------------------
namespace SchemaStore {

/// Top-level keys of a schema definition (version 4, and version 3 when one is
/// read) plus "name": never metadata.
bool isSchemaFormatKey(const QString& key);

/// A single schema entry.
struct SchemaEntry {
    QString name;        ///< Display name ("name" key of the file).
    QString schema;      ///< Definition (canonical, compact: LineTemplateSchema::definition).
    QJsonObject meta;    ///< "description" and unknown top-level keys, kept verbatim.
    /// File in patternsDir() the entry was loaded from (basename, with .json);
    /// empty for a schema that has no file yet. sync() rewrites this file in
    /// place, so an edited built-in schema stays where the manifest of
    /// built-in schemas looks for it.
    QString fileName;

    QString description() const;
    /// Empty text removes the key.
    void setDescription(const QString& text);

    bool isEmpty() const { return name.isEmpty() && schema.isEmpty(); }
    /// Same content on disk: name, schema and metadata (not the file name).
    bool sameContent(const SchemaEntry& other) const
    {
        return name == other.name && schema == other.schema && meta == other.meta;
    }
};
using SchemaList = QList<SchemaEntry>;

/// Parses raw file contents as a schema entry (lenient: an in-progress schema
/// loads as-is). \a fallbackName is used when the file has no "name". The
/// file name of the result is left empty. Returns false for anything that is
/// not a recognisable schema.
bool parseEntry(const QByteArray& bytes, const QString& fallbackName, SchemaEntry* entry);

/// Bytes of the on-disk file for \a entry: canonical schema + name + metadata,
/// pretty-printed for hand-editing.
QByteArray fileBytes(const SchemaEntry& entry);

/// Absolute path to <exeDir>/patterns, created on demand.
/// Returns an empty string if the directory could not be created.
QString patternsDir();

/// Installs the built-in schemas shipped as resources (:/patterns) into the
/// folder — see BuiltinSchemas for the rules (user files are never
/// overwritten, deleted built-ins are not resurrected). Errors are logged and
/// returned in the report.
BuiltinSchemas::InstallReport installBuiltins();

/// Sub-folder of the catalogue with the originals of translated files.
inline constexpr const char* kConvertedDirName = "converted-from-v3";

struct MigrationReport {
    QStringList converted;   ///< files rewritten as version 4 (basenames)
    QStringList failed;      ///< "file: why" — left as they were
    QStringList notes;       ///< "file: what changed in meaning" (SchemaConverter notes)
};
/// Translates every version 3 file of \a dirPath into version 4 (see the
/// header); version 4 files are not touched, so a second run does nothing.
MigrationReport migrateToTemplates(const QString& dirPath);
/// The same on patternsDir(); failures are logged.
MigrationReport migrateToTemplates();

/// Every readable schema in the folder, normalised to canonical JSON.
/// Files that do not parse as a schema are skipped. Entries whose display
/// name appears in \a orderNames come first in that order; the rest follow
/// sorted by name (case-insensitive).
SchemaList loadAll(const QStringList& orderNames = QStringList());

/// Makes the folder contain exactly \a list: writes/refreshes a file per
/// entry and deletes files of schemas no longer present.
///   • An entry with a fileName that still exists is written to that file —
///     only if its content differs (an untouched schema is not rewritten).
///     Exception: a renamed schema whose file name was derived from its old
///     name (suggestedFileName, possibly with _N) moves to a file named after
///     the new name, as before.
///   • An entry without a usable fileName reuses a file with the same content,
///     otherwise gets a fresh file named after it.
///   • A file that does not parse as a schema (a newer format, a file broken
///     by hand) is left alone: not deleted, and no new schema takes its name.
/// Returns false and sets \a errorMessage if any file operation failed.
bool sync(const SchemaList& list, QString* errorMessage = nullptr);

/// Copies \a srcPath into the folder after checking it parses as a schema.
/// On success returns the resulting entry (with metadata and the new file
/// name); on failure
/// returns an empty entry and sets \a errorMessage.
SchemaEntry importFile(const QString& srcPath, QString* errorMessage = nullptr);

/// Writes a single schema to an arbitrary path chosen by the user (pretty
/// JSON, with the embedded name and metadata). Returns false and sets
/// \a errorMessage on error.
bool exportToFile(const SchemaEntry& entry,
                  const QString& destPath,
                  QString* errorMessage = nullptr);

/// Suggested file name (no directory, includes the .json suffix) for a
/// schema display name. Sanitises characters invalid in file names.
QString suggestedFileName(const QString& name);

} // namespace SchemaStore

#endif // SCHEMASTORE_H
