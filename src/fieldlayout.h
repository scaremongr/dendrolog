#ifndef FIELDLAYOUT_H
#define FIELDLAYOUT_H

#include "fieldextractor.h"
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
// схема дописывает недостающие имена в конец), поэтому индексы привязанных
// правил не устаревают. Пересборка раскладки — только при смене схемы
// пользователем.
//
// FileSchema — схема файла: извлекатель (F-17, шаг 3: поля — функция текста
// строки и схемы, в записях не хранятся) и таблица «поле схемы → колонка
// вкладки». FileSchema::extract отдаёт поля СРАЗУ в пространстве колонок
// вкладки (toLayout); дальше фильтр, показ, Save View As, Search Results и
// Entry Details работают индексами колонок вкладки. Получают поля через
// FieldSource / FieldScanner (fieldsource.h). У вкладки с одной схемой
// columnOf тождественен (identity) — поля те же, байт в байт.
//
// Значение неизменяемое и делится между потоками через FileSchemaPtr (как
// LogPattern копией сейчас): воркер получает указатель на старте прохода.
// Ловушка MSVC: FileSchema, FieldLayout и FileSchemaSet — struct везде,
// forward-декларации совпадают.
// ============================================================================

struct FileSchema {
    // Определение, из которого схема построена (FieldLayout::schemaFor):
    // блочная схема, шаблон строки версии 4 или колонки формата строк
    // (LineFormat::fieldDefinition). Загрузка схему не видит (F-17, шаг 7).
    QString definition;
    // Поля строки (F-17, шаг 3): extractor->extract(текст) — единственный
    // способ их получить; в записях поля не хранятся.
    FieldExtractorPtr extractor;
    // Индекс поля схемы → индекс колонки вкладки.
    QVector<int> columnOf;
    // Колонка вкладки → поле схемы (-1 — колонки у схемы нет).
    QVector<int> fieldOf;
    // Какие колонки вкладки есть у этой схемы (длина — число колонок
    // раскладки на момент построения; колонки, добавленные позже, — чужие).
    QBitArray hasColumn;
    // columnOf[i] == i для всех полей: перенос спанов не нужен.
    bool identity = true;

    bool isValid() const noexcept { return extractor && extractor->isValid(); }
    bool has(int column) const noexcept
    {
        return column >= 0 && column < hasColumn.size() && hasColumn.testBit(column);
    }
    // Есть ли у схемы хотя бы одна из колонок (показ слитой вкладки: строка,
    // в схеме которой нет ни одной выбранной колонки, — сырым текстом).
    bool hasAnyOf(const QVector<int>& columns) const noexcept;

    // Поля, извлечённые схемой (индексы её полей), — в колонки вкладки.
    void toLayout(LogEntryFields& fields) const;
    // Поля строки в колонках вкладки. upToColumn >= 0 — нужна только эта
    // колонка вкладки (и левее по схеме): разбор кончается раньше
    // (FieldExtractor::extract, upTo). Строка не принята — out пуст
    // (isEmpty), Rejected.
    FieldExtractor::Outcome extract(QStringView line, LogEntryFields* out,
                                    int upToColumn = -1) const;
    LogEntryFields extractFields(QStringView line) const
    {
        LogEntryFields fields;
        extract(line, &fields);
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
    // Определение — блочная схема (LogPattern), шаблон строки версии 4
    // (LineTemplateSchema, F-17: TemplateExtractor) либо колонки формата
    // строк JSON / logfmt (LineFormat::fieldDefinition: KeyedExtractor по
    // ключам ролей и ключам начала файла, F-17, шаг 7).
    FileSchemaPtr schemaFor(const LogPattern& pattern);
    FileSchemaPtr schemaFor(const QString& definition);
    FileSchemaPtr schemaFor(FieldExtractorPtr extractor, const QString& definition);
    // Извлекатель по определению схемы (шаблон версии 4 или LogPattern).
    static FieldExtractorPtr extractorFor(const LogPattern& pattern);
    // Определение — файл шаблона строки версии 4.
    static bool isTemplateDefinition(const QString& definition);
    void clear() { columns.clear(); }
};

// Схемы файлов вкладки: своя у файла (по пути) либо общая (fallback).
// nullptr — у файла нет схемы (полей нет). Значение — копируемое: модель
// отдаёт его воркерам (FieldScanner — фильтр, Save View As, поиск).
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
