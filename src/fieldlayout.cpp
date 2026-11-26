#include "fieldlayout.h"
#include "lineformat.h"
#include "linetemplate.h"
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

FieldExtractor::Outcome FileSchema::extract(QStringView line, LogEntryFields* out,
                                            int upToColumn) const
{
    if (!extractor) {
        out->spans.clear();
        return FieldExtractor::Outcome::Rejected;
    }
    int upTo = -1;
    if (upToColumn >= 0) {
        // Колонки вкладки нет у схемы — разбирать её не нужно, но принята ли
        // строка, решает весь разбор; такой вызов не делают (правило по
        // отсутствующей колонке нейтрально).
        upTo = (upToColumn < fieldOf.size()) ? fieldOf[upToColumn] : -1;
    }
    const FieldExtractor::Outcome outcome = extractor->extract(line, out, upTo);
    if (outcome == FieldExtractor::Outcome::Rejected) {
        out->spans.clear();
        return outcome;
    }
    toLayout(*out);
    return outcome;
}

bool FieldLayout::isTemplateDefinition(const QString& definition)
{
    return LineTemplateSchema::templateOf(definition, nullptr);
}

FieldExtractorPtr FieldLayout::extractorFor(const LogPattern& pattern)
{
    QString templateText;
    if (LineTemplateSchema::templateOf(pattern.patternString(), &templateText))
        return std::make_shared<const TemplateExtractor>(templateText);
    if (!pattern.isValid())
        return nullptr;
    return std::make_shared<const LegacyPatternExtractor>(pattern);
}

FileSchemaPtr FieldLayout::schemaFor(const LogPattern& pattern)
{
    return schemaFor(extractorFor(pattern), pattern.patternString());
}

FileSchemaPtr FieldLayout::schemaFor(const QString& definition)
{
    KeyedDefinition keys;
    if (LineFormat::keysFromFieldDefinition(definition, &keys))
        return schemaFor(std::make_shared<const KeyedExtractor>(keys), definition);
    return schemaFor(LogPattern(definition));
}

FileSchemaPtr FieldLayout::schemaFor(FieldExtractorPtr extractor, const QString& definition)
{
    auto schema = std::make_shared<FileSchema>();
    schema->extractor = std::move(extractor);
    schema->definition = definition;
    const QStringList names = schema->isValid() ? schema->extractor->fields() : QStringList();
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
    schema->fieldOf.fill(-1, int(columns.size()));
    for (int i = 0; i < schema->columnOf.size(); ++i) {
        schema->hasColumn.setBit(schema->columnOf[i]);
        schema->fieldOf[schema->columnOf[i]] = i;
    }
    return schema;
}

const FileSchema* FileSchemaSet::forFile(const LogFile* file) const
{
    if (byPath.isEmpty() || !file)
        return fallback.get();
    return forPath(file->filePath).get();
}
