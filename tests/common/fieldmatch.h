#ifndef TESTS_COMMON_FIELDMATCH_H
#define TESTS_COMMON_FIELDMATCH_H

// Одна строка против набора правил со схемой (F-17, шаг 3): строка — сама
// себе первая строка записи, поля — FieldScanner по схеме.
#include "fieldlayout.h"
#include "fieldsource.h"
#include "filterruleset.h"

namespace testfields {

inline FieldScanner scannerFor(const LogPattern& pattern)
{
    FieldLayout layout;
    FileSchemaSet set;
    set.fallback = layout.schemaFor(pattern);
    return FieldScanner(set, nullptr);
}

inline bool matchesOwnRecord(const FilterRuleSet& rules, const FieldScanner& scanner,
                             const QString& line)
{
    FieldRecordCursor record(&scanner);
    record.setLine(1, nullptr, 1, &line);
    return rules.matchesLine(line, &record);
}

} // namespace testfields

#endif
