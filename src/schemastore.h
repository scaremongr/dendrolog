#ifndef SCHEMASTORE_H
#define SCHEMASTORE_H

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// ---------------------------------------------------------------------------
// SchemaStore — file-based persistence for field schemas.
//
// Each schema is one *.json file in <exeDir>/patterns/. A file holds the
// serialized LogPattern schema (see LogPattern::serializeDefinition) plus a
// top-level "name" key carrying the display name, so a schema file is fully
// self-describing and portable between machines.
//
// The folder is the single source of truth for the schema *list*; the INI
// file only remembers the display order and which schema is active. Any
// *.json dropped into the folder by hand is picked up on the next load
// ("drop-in"), and Import simply copies a file into the folder.
// ---------------------------------------------------------------------------
namespace SchemaStore {

/// A single schema entry: first = display name, second = serialized schema
/// JSON (canonical, compact — exactly what LogPattern consumes).
using SchemaEntry = QPair<QString, QString>;
using SchemaList  = QList<SchemaEntry>;

/// Absolute path to <exeDir>/patterns, created on demand.
/// Returns an empty string if the directory could not be created.
QString patternsDir();

/// Every readable schema in the folder, normalised to canonical JSON.
/// Files that do not parse as a schema are skipped. Entries whose display
/// name appears in \a orderNames come first in that order; the rest follow
/// sorted by name (case-insensitive).
SchemaList loadAll(const QStringList& orderNames = QStringList());

/// Makes the folder contain exactly \a list: writes/refreshes a file per
/// entry and deletes files of schemas no longer present. Files whose content
/// and name already match an entry are reused as-is (no rewrite, so manually
/// curated file names survive untouched edits). Returns false and sets
/// \a errorMessage if any file operation failed.
bool sync(const SchemaList& list, QString* errorMessage = nullptr);

/// Copies \a srcPath into the folder after checking it parses as a schema.
/// On success returns the resulting entry (name + canonical JSON); on failure
/// returns an empty entry and sets \a errorMessage.
SchemaEntry importFile(const QString& srcPath, QString* errorMessage = nullptr);

/// Writes a single schema to an arbitrary path chosen by the user (pretty
/// JSON, with the embedded name). Returns false and sets \a errorMessage on
/// error.
bool exportToFile(const QString& name,
                  const QString& serialized,
                  const QString& destPath,
                  QString* errorMessage = nullptr);

/// Suggested file name (no directory, includes the .json suffix) for a
/// schema display name. Sanitises characters invalid in file names.
QString suggestedFileName(const QString& name);

} // namespace SchemaStore

#endif // SCHEMASTORE_H
