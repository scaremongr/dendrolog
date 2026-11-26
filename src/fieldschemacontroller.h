#ifndef FIELDSCHEMACONTROLLER_H
#define FIELDSCHEMACONTROLLER_H

#include "schemastore.h"

#include <QObject>
#include <QStringList>
#include <QVector>

#include <functional>

class FieldReextraction;
class FilterRuleSet;
class LogViewWidget;
class QSettings;
struct SessionState;

// ============================================================================
// FieldSchemaController — каталог схем полей и состояние дока Log Fields без
// виджетов (M-2). Окно держит только вид: комбобокс схем, галочку «Filter
// blocks» и чекбоксы колонок; всё, что они значат, — здесь.
//
//   • Каталог: load — порядок из ini, файлы <configDir>/patterns/*.json
//     (SchemaStore), разовая миграция списков старых версий из ini, установка
//     встроенных схем (BuiltinSchemas; исправленная встроенная заменяет
//     нетронутую копию, и активная схема следует за ней); reloadCatalogue —
//     перечитать каталог, сохранив порядок; replaceCatalogue — итог
//     редактора схем; save — порядок, активная схема, флаг и колонки в ini.
//   • Активная схема — строка схемы и её индекс в каталоге отдельно: схема
//     может действовать и без пункта в списке (её удалили в редакторе,
//     схема сессии не записалась в каталог).
//   • Колонки: выбор хранится по ИМЕНАМ и переживает смену схемы — при
//     новой схеме остаются выбранные имена, которые в ней есть; ни одного
//     (или выбор пуст) — выбраны все.
//   • applyToAllViews — схема, флаг и колонки на всех вкладках. Инвариант 4:
//     поля резидентных записей меняются только на GUI-потоке при
//     остановленных читателях полей — фильтр-воркеры вкладок (здесь) и
//     читатели вне вкладок, выдача Search и Save View As (Window::
//     stopFieldReaders у окна). До 4000 записей — синхронно, больше —
//     FieldReextraction в пуле. Пока он считает, показ колонок выключен на
//     всех вкладках, что бы ни выбрали в доке: колонки уже новой схемы, а
//     поля записей ещё старой, — поэтому Save View As, начатый в это время,
//     полей не читает, а поиск окно не запускает. По концу расчёта фильтры
//     вкладок, запущенные за это время, останавливаются снова, затем
//     apply(), перефильтрация (reapplyFilterIfStale), показ колонок и
//     перепривязка применённых правил вкладок к новой схеме. Индексная вкладка полей не хранит —
//     ей только новая схема. Ключевая схема (F-4) идёт тем же путём: это
//     свойство LogPattern.
//
// До F-5 схема одна на окно (инвариант 7: Fields — глобальный). F-5, PR 1
// сделает её состоянием вкладки: тогда applyToAllViews станет применением к
// одной вкладке, а этот класс — источником схемы по умолчанию.
// ============================================================================
class FieldSchemaController : public QObject {
    Q_OBJECT

public:
    struct Window {
        // Вкладки окна (порядок не важен).
        std::function<QList<LogViewWidget*>()> views;
        // Остановить с ожиданием читателей полей вне вкладок перед сменой
        // схемы: выдачу Search (делит записи с источником; её повторят по
        // schemaApplied) и фоновый Save View As.
        std::function<void()> stopFieldReaders;
    };

    explicit FieldSchemaController(QObject* parent = nullptr);
    ~FieldSchemaController() override;

    void setWindow(Window window) { m_window = std::move(window); }

    // ---- Каталог -------------------------------------------------------------
    // ini: группа [View] уже открыта вызывающим.
    void load(QSettings& view);
    void save(QSettings& view) const;
    const SchemaStore::SchemaList& schemas() const { return m_list; }
    // Перечитать каталог (файлы могли добавить или поправить руками),
    // сохранив порядок списка.
    void reloadCatalogue();
    // Итог редактора схем: список пишется в каталог; chosen — «Use Selected»
    // (chosenIndex — его место в списке), применяется ко вкладкам; пустая —
    // активная остаётся (без пункта, если её больше нет в списке) и заново не
    // применяется. Возвращает ошибку записи каталога (или пустую).
    QString replaceCatalogue(const SchemaStore::SchemaList& list, const QString& chosen,
                             int chosenIndex);

    // ---- Активная схема ------------------------------------------------------
    int activeIndex() const { return m_activeIndex; }
    QString activeSchema() const { return m_active; }
    // Имя для сессии (SessionController::schemaNameFor).
    QString activeSchemaName() const;
    // Выбор в комбобоксе (-1 — без схемы).
    void selectSchema(int index);
    // Схема, флаг и колонки сессии (до вкладок сессии: правила привязываются
    // к схеме вкладки). Сессия без схемы ничего не меняет. Возвращает ошибку
    // записи схемы в каталог (или пустую).
    QString applySessionSchema(const SessionState& session);

    // ---- «Filter blocks» и колонки -------------------------------------------
    bool extractionEnabled() const { return m_extraction; }
    void setExtractionEnabled(bool enabled);
    QStringList fieldNames() const { return m_fieldNames; }
    QStringList visibleFieldNames() const { return m_visible; }
    QVector<int> visibleFieldIndexes() const;
    // Показ колонок включён: флаг и у схемы есть колонки.
    bool fieldDisplayEnabled() const { return m_extraction && !m_fieldNames.isEmpty(); }
    void setVisibleFieldNames(const QStringList& names);
    // Привязать правила фильтра к колонкам активной схемы.
    void bindRules(FilterRuleSet& rules) const;

    // ---- Вкладки -------------------------------------------------------------
    // Новая вкладка: схема, флаг и колонки окна.
    void configureView(LogViewWidget* view) const;
    void applyToAllViews();
    bool isReextracting() const { return m_reextraction != nullptr; }
    // Остановить фоновое переизвлечение с ожиданием воркеров (выход, новая
    // схема); записи остаются со старыми полями, показ колонок — выключенным
    // до следующего применения. Шлёт reextractionFinished, если расчёт шёл.
    void cancelReextraction();

signals:
    // Список схем или активный индекс изменились — перестроить комбобокс.
    void catalogueChanged();
    // Колонки схемы, выбор колонок или флаг изменились.
    void fieldsChanged();
    // Применённые правила вкладки перепривязаны к новой схеме.
    void filtersRebound(LogViewWidget* view);
    // Схема применена ко всем вкладкам, поля на месте.
    void schemaApplied();
    void reextractionStarted(int entries);
    void reextractionProgress(int value);
    // Расчёт закончен или отменён (cancelReextraction, новая схема).
    void reextractionFinished();

private:
    QList<LogViewWidget*> views() const;
    QStringList listOrder() const;
    void reload(const QString& extraName);
    void setFieldNames(const QStringList& names);
    void applyFieldVisibility();
    void cancelFilterJobs();
    void onReextractionFinished();
    void finishApplication();

    Window m_window;
    SchemaStore::SchemaList m_list;
    QString m_active;
    int m_activeIndex = -1;
    bool m_extraction = false;
    QStringList m_fieldNames;
    QStringList m_visible;
    FieldReextraction* m_reextraction = nullptr;
};

#endif // FIELDSCHEMACONTROLLER_H
