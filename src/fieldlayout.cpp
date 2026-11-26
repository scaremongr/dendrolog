#include "fieldlayout.h"
#include "logfile.h"

bool FileSchema::hasAnyOf(const QVector<int>& columns) const noexcept
{
    for (const int column : columns)
        if (has(column))
            return true;
    return false;
}

void FileSchema::toLayout(LogEntryFields& fields) const
{
    if (identity || fields.spans.isEmpty())
        return;
    int width = 0;
    const int n = int(qMin(fields.spans.size(), columnOf.size()));
    for (int i = 0; i < n; ++i)
        width = qMax(width, columnOf[i] + 1);
    QVector<FieldSpan> spans(width);
    for (int i = 0; i < n; ++i)
        spans[columnOf[i]] = fields.spans[i];
    fields.spans = std::move(spans);
}

FileSchemaPtr FieldLayout::schemaFor(const LogPattern& pattern)
{
    auto schema = std::make_shared<FileSchema>();
    schema->pattern = pattern;
    const QStringList names = pattern.isValid() ? pattern.fieldNames() : QStringList();
    schema->columnOf.reserve(names.size());
    for (int i = 0; i < names.size(); ++i) {
        // k-е поле с этим именем в схеме ↔ k-я колонка с ним в раскладке.
        int occurrence = 0;
        for (int j = 0; j < i; ++j)
            if (names[j] == names[i])
                ++occurrence;
        int column = -1;
        for (int c = 0, seen = 0; c < columns.size(); ++c) {
            if (columns[c] != names[i])
                continue;
            if (seen++ == occurrence) {
                column = c;
                break;
            }
        }
        if (column < 0) {
            column = int(columns.size());
            columns.append(names[i]);
        }
        schema->columnOf.append(column);
        if (column != i)
            schema->identity = false;
    }
    schema->hasColumn.resize(int(columns.size()));
    for (const int column : std::as_const(schema->columnOf))
        schema->hasColumn.setBit(column);
    return schema;
}

const FileSchema* FileSchemaSet::forFile(const LogFile* file) const
{
    if (byPath.isEmpty() || !file)
        return fallback.get();
    return forPath(file->filePath).get();
}
