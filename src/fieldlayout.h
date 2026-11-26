#ifndef FIELDLAYOUT_H
#define FIELDLAYOUT_H

#include "logfield.h"
#include "logpattern.h"

#include <QBitArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

struct LogFile;

// ============================================================================
// Колонки вкладки и схема файла (F-5, PR 2; docs/design/F-5.md, «Решение»).
//
// У вкладки одна раскладка колонок — FieldLayout: объединение колонок схем
// её файлов по имени, в порядке схем; колонки только добавляются (новая
// схема дописывает недостающие имена в конец), поэтому индексы уже
// разобранных записей не устаревают. Пересборка раскладки — только при
// смене схемы пользователем (тогда поля вкладки переизвлекаются).
//
// FileSchema — схема, которой читается файл, и таблица «поле схемы →
// колонка вкладки». Все, кто кладёт поля (резидентный парсер, индексный стор
// по требованию, переизвлечение), кладут их СРАЗУ в пространстве колонок
// вкладки (toLayout); дальше фильтр, показ, Save View As, Search Results и
// Entry Details работают индексами колонок вкладки. У вкладки с одной схемой
// columnOf тождественен (identity) — поля те же, байт в байт.
//
// Значение неизменяемое и делится между потоками через FileSchemaPtr (как
// LogPattern копией сейчас): воркер получает указатель на старте прохода.
// Ловушка MSVC: FileSchema, FieldLayout и FileSchemaSet — struct везде,
// forward-декларации совпадают.
// ============================================================================

struct FileSchema {
    LogPattern pattern;
    // Индекс поля схемы → индекс колонки вкладки.
    QVector<int> columnOf;
    // Какие колонки вкладки есть у этой схемы (длина — число колонок
    // раскладки на момент построения; колонки, добавленные позже, — чужие).
    QBitArray hasColumn;
    // columnOf[i] == i для всех полей: перенос спанов не нужен.
    bool identity = true;

    bool isValid() const noexcept { return pattern.isValid(); }
    bool has(int column) const noexcept
    {
        return column >= 0 && column < hasColumn.size() && hasColumn.testBit(column);
    }
    // Есть ли у схемы хотя бы одна из колонок (показ слитой вкладки: строка,
    // в схеме которой нет ни одной выбранной колонки, — сырым текстом).
    bool hasAnyOf(const QVector<int>& columns) const noexcept;

    // Поля, извлечённые схемой (индексы её полей), — в колонки вкладки.
    void toLayout(LogEntryFields& fields) const;
    // pattern.extractFields + toLayout.
    LogEntryFields extractFields(const QString& line) const
    {
        LogEntryFields fields = pattern.extractFields(line);
        toLayout(fields);
        return fields;
    }
};

using FileSchemaPtr = std::shared_ptr<const FileSchema>;

struct FieldLayout {
    // Колонки вкладки: объединение по имени, только растёт.
    QStringList columns;

    // Схема файла в этой раскладке; недостающие колонки дописываются в
    // конец. k-е поле схемы с именем N — k-я колонка с именем N (схема с
    // повторяющимся именем не склеивает свои поля в одну колонку).
    FileSchemaPtr schemaFor(const LogPattern& pattern);
    FileSchemaPtr schemaFor(const QString& patternString)
    {
        return schemaFor(LogPattern(patternString));
    }
    void clear() { columns.clear(); }
};

// Схемы файлов вкладки: своя у файла (по пути) либо общая (fallback).
// nullptr — файл читается без схемы (извлечение полей выключено). Значение —
// копируемое: модель отдаёт его воркерам (Save View As, переизвлечение).
struct FileSchemaSet {
    QHash<QString, FileSchemaPtr> byPath;
    FileSchemaPtr fallback;

    FileSchemaPtr forPath(const QString& path) const
    {
        if (!byPath.isEmpty()) {
            const auto it = byPath.constFind(path);
            if (it != byPath.constEnd())
                return it.value();
        }
        return fallback;
    }
    const FileSchema* forFile(const LogFile* file) const;
    bool operator==(const FileSchemaSet& other) const
    {
        return fallback == other.fallback && byPath == other.byPath;
    }
};

#endif // FIELDLAYOUT_H
