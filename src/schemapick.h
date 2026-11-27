#ifndef SCHEMAPICK_H
#define SCHEMAPICK_H

#include "schemafit.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>
#include <utility>

// ============================================================================
// SchemaPick — подбор схемы полей при открытии лога (F-17, шаг 8; бывшая F-6;
// docs/design/F-17.md, раздел 7). Чистые функции и данные, без Qt Widgets и
// без состояния вкладки: решение по готовым мерам SchemaFit (их считает
// SchemaFitJob в пуле по первым строкам вкладки — файл не перечитывается),
// таблица «имя файла → схема», которую пользователь выбирал раньше.
//
// Порядок (choose):
//   1. сохранённое состояние вкладки — решает вызывающий (подбор не идёт);
//   2. имя: пользователь выбирал «без схемы» для такого имени — без схемы
//      (не оспаривается); выбирал схему — она, если покрытие ≥ 0,50;
//   3. содержимое: лучшая схема каталога по SchemaFit::better, если она
//      «подходит» (Verdict::Fits);
//   4. схема по умолчанию (последняя выбранная в доке), если покрытие ≥ 0,50;
//   5. иначе — без схемы.
//
// Ключ имени (nameKey) — имя файла в нижнем регистре без «.gz» и хвоста
// ротации (`.1`, `-20260101`, `.2026-01-01`), серии цифр заменены на «#»:
// «App-2026-11-26.log.3.gz» и «app-2025-01-02.log» — один ключ.
// ============================================================================
namespace SchemaPick {

constexpr double kNameCoverage = 0.50;      ///< схема по имени — если покрытие не ниже
constexpr double kDefaultCoverage = 0.50;   ///< схема по умолчанию — то же
constexpr int kMaxNames = 200;              ///< записей в таблице имён

QString nameKey(const QString& fileName);

enum class Source {
    None,       ///< ничего не подошло — без схемы
    NameNone,   ///< по имени пользователь выбирал «без схемы»
    Name,       ///< по имени
    Content,    ///< лучшая из каталога по содержимому
    Default,    ///< схема по умолчанию
};

struct Choice {
    Source source = Source::None;
    int index = -1;   ///< номер в results (-1 — без схемы)
};

struct Input {
    // Меры по схемам в порядке вызывающего: первые catalogueCount — каталог
    // (кандидаты «по содержимому»), за ними могут идти ещё (схема по
    // умолчанию не из каталога).
    QVector<SchemaFit::Result> results;
    int catalogueCount = 0;
    int nameIndex = -1;       ///< схема из таблицы имён (-1 — нет записи или схемы)
    bool nameNone = false;    ///< в таблице имён — «без схемы»
    int defaultIndex = -1;    ///< схема по умолчанию (-1 — нет)
};

Choice choose(const Input& input);

/// Номера схем: «подходящие» (Verdict::Fits) первыми — лучшая первой
/// (SchemaFit::better, устойчиво), остальные — в исходном порядке.
QVector<int> fitsFirst(const QVector<SchemaFit::Result>& results);

// Таблица «ключ имени → имя схемы» (пусто — «без схемы»), свежая первой,
// не больше kMaxNames. В ini — [SchemaByName] entries (JSON).
class NameTable {
public:
    std::optional<QString> lookup(const QString& key) const;
    void remember(const QString& key, const QString& schemaName);
    void clear() { m_entries.clear(); }
    int size() const { return int(m_entries.size()); }

    QString toJson() const;
    static NameTable fromJson(const QString& json);

private:
    QVector<std::pair<QString, QString>> m_entries;
};

} // namespace SchemaPick

#endif // SCHEMAPICK_H
