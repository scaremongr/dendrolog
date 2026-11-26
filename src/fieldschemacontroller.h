#ifndef FIELDSCHEMACONTROLLER_H
#define FIELDSCHEMACONTROLLER_H

#include "documentstate.h"
#include "schemastore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

class FieldReextraction;
class FilterRuleSet;
class LogViewWidget;
class QSettings;

// ============================================================================
// FieldSchemaController — каталог схем полей и состояние дока Log Fields без
// виджетов (M-2). Окно держит только вид: комбобокс схем, галочку «Filter
// blocks» и чекбоксы колонок; всё, что они значат, — здесь.
//
//   • Каталог: load — порядок из ini, файлы <configDir>/patterns/*.json
//     (SchemaStore), разовая миграция списков старых версий из ini, установка
//     встроенных схем (BuiltinSchemas; исправленная встроенная заменяет
//     нетронутую копию, и схема по умолчанию следует за ней); reloadCatalogue —
//     перечитать каталог, сохранив порядок; replaceCatalogue — итог
//     редактора схем; save — порядок и схема по умолчанию в ini.
//   • Схема на вкладку (F-5, PR 1): схема, «Filter blocks» и выбор колонок —
//     состояние вкладки (LogViewWidget::fieldSettings, FieldSettings).
//     Док показывает вкладку setCurrentView (активную вкладку окна); без
//     вкладки — схему по умолчанию. Выбор в доке меняет ТОЛЬКО эту вкладку и
//     становится схемой по умолчанию — её получает новая вкладка
//     (configureView) и её хранит ini ([View] conversionPattern,
//     fieldFilterEnabled, fieldVisibleNames). applyToAllViews — «Apply
//     Schema to All Tabs»: схема дока на все вкладки (поведение до F-5).
//   • Схема определением, а не пунктом: пункт в каталоге (activeIndex) —
//     с тем же именем и определением, иначе первый с тем же определением;
//     нет такого — схема действует без пункта (её удалили или поправили в
//     редакторе, схема сессии не записалась в каталог).
//   • Колонки: выбор хранится по ИМЕНАМ и переживает смену схемы — при
//     новой схеме остаются выбранные имена, которые в ней есть; ни одного
//     (или выбор пуст) — выбраны все.
//   • Сохранённая схема вкладки (снимок: сессия, Ctrl+Shift+T, память
//     файла) приходит сигналом вкладки savedFieldSettingsFound: схема
//     ищется в каталоге по определению (SessionController::chooseSchema);
//     нет — сессия и закрытая вкладка добавляют её «(from session)», память
//     файла каталог не засоряет: вкладка остаётся со схемой по умолчанию,
//     statusMessage сообщает.
//   • applyToView — схема, флаг и колонки на одной вкладке. Инвариант 4:
//     поля резидентных записей меняются только на GUI-потоке при
//     остановленных читателях полей — фильтр-воркер этой вкладки (здесь) и
//     читатели вне вкладок, выдача Search этой вкладки и Save View As
//     (Window::stopFieldReaders у окна). До 4000 записей — синхронно, больше
//     — FieldReextraction в пуле, свой на вкладку. Пока он считает, показ
//     колонок этой вкладки выключен, что бы ни выбрали в доке: колонки уже
//     новой схемы, а поля записей ещё старой (поиск окно в это время не
//     запускает). По концу расчёта читатели, запущенные за это время,
//     останавливаются снова (фильтр вкладки; у окна — Save View As в CSV /
//     JSON Lines, который читает поля всегда; текстовый при выключенном
//     показе полей не читает), затем apply(), перефильтрация
//     (reapplyFilterIfStale), показ колонок и перепривязка применённых правил
//     вкладки к новой схеме. Те же схема и флаг (смена только колонок) —
//     без переизвлечения. Индексная вкладка полей не хранит — ей только
//     новая схема. Ключевая схема (F-4) идёт тем же путём: это свойство
//     LogPattern.
// ============================================================================
class FieldSchemaController : public QObject {
    Q_OBJECT

public:
    struct Window {
        // Вкладки окна (порядок не важен).
        std::function<QList<LogViewWidget*>()> views;
        // Остановить с ожиданием читателей полей вкладки view вне её самой:
        // выдачу Search (делит записи с источником) и фоновый Save View As.
        // starting — начало применения схемы (выдачу можно сбросить: её
        // повторят по schemaApplied); иначе — перед установкой полей фонового
        // расчёта (останавливать тех, кто читает поля и при выключенном показе).
        std::function<void(LogViewWidget* view, bool starting)> stopFieldReaders;
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
    // (chosenIndex — его место в списке), применяется к вкладке дока; пустая
    // — её схема остаётся (без пункта, если её больше нет в списке) и заново
    // не применяется. Возвращает ошибку записи каталога (или пустую).
    QString replaceCatalogue(const SchemaStore::SchemaList& list, const QString& chosen,
                             int chosenIndex);

    // ---- Вкладка дока --------------------------------------------------------
    // Вкладка, которую показывает док (активная вкладка окна); nullptr — схема
    // по умолчанию. Шлёт catalogueChanged и fieldsChanged.
    void setCurrentView(LogViewWidget* view);
    LogViewWidget* currentView() const { return m_current; }
    // Схема по умолчанию для новых вкладок — последняя выбранная в доке.
    const FieldSettings& defaults() const { return m_defaults; }
    // Что показывает док: схема вкладки дока или схема по умолчанию.
    FieldSettings shownSettings() const;

    // ---- Схема дока ----------------------------------------------------------
    int activeIndex() const { return indexOf(shownSettings()); }
    QString activeSchema() const { return shownSettings().schema; }
    // Имя для сессии (SessionController::schemaNameFor).
    QString activeSchemaName() const;
    // Выбор в комбобоксе (-1 — без схемы).
    void selectSchema(int index);

    // ---- «Filter blocks» и колонки дока --------------------------------------
    bool extractionEnabled() const { return shownSettings().extraction; }
    void setExtractionEnabled(bool enabled);
    QStringList fieldNames() const;
    QStringList visibleFieldNames() const { return shownSettings().visibleFields; }
    QVector<int> visibleFieldIndexes() const;
    // Показ колонок включён: флаг и у схемы есть колонки.
    bool fieldDisplayEnabled() const { return extractionEnabled() && !fieldNames().isEmpty(); }
    void setVisibleFieldNames(const QStringList& names);
    // Привязать правила фильтра к колонкам схемы вкладки view (Text Filters,
    // Search — вкладке, к которой их применяют).
    static void bindRules(const LogViewWidget* view, FilterRuleSet& rules);

    // ---- Вкладки -------------------------------------------------------------
    // Новая вкладка: схема по умолчанию; подписка на её сохранённую схему.
    void configureView(LogViewWidget* view);
    // Схема, флаг и колонки на вкладке view (см. заголовок).
    void applyToView(LogViewWidget* view, const FieldSettings& settings);
    // «Apply Schema to All Tabs»: схема дока на все вкладки.
    void applyToAllViews();
    // Сохранённая схема вкладки против каталога: найдена по определению —
    // её пункт; нет — addMissing: добавить «(from session)» (ошибка записи
    // каталога — в error; схема действует и без пункта), иначе nullopt.
    std::optional<FieldSettings> resolveSaved(const FieldSettings& saved, bool addMissing,
                                              QString* error = nullptr);
    // Идёт фоновое переизвлечение — у вкладки view (nullptr — у любой).
    bool isReextracting(const LogViewWidget* view = nullptr) const;
    // Остановить фоновые переизвлечения с ожиданием воркеров (выход); записи
    // остаются со старыми полями, показ колонок — выключенным до следующего
    // применения. Шлёт reextractionFinished на каждый шедший расчёт.
    void cancelReextraction();

signals:
    // Список схем или схема дока изменились — перестроить комбобокс.
    void catalogueChanged();
    // Колонки схемы дока, выбор колонок или флаг изменились.
    void fieldsChanged();
    // Применённые правила вкладки перепривязаны к новой схеме.
    void filtersRebound(LogViewWidget* view);
    // Схема применена к вкладке, поля на месте.
    void schemaApplied(LogViewWidget* view);
    void reextractionStarted(int entries);
    void reextractionProgress(int value);
    // Расчёт закончен или отменён (cancelReextraction, новая схема вкладки).
    void reextractionFinished();
    // Для строки состояния (схема памяти файла не в каталоге, ошибка записи).
    void statusMessage(const QString& text);

private:
    QList<LogViewWidget*> views() const;
    QStringList listOrder() const;
    int indexOf(const FieldSettings& settings) const;
    void reload(const QString& extraName);
    // Выбор колонок для схемы: выбранные имена, которые в ней есть; ни
    // одного (или выбор пуст) — все колонки.
    static QStringList keptColumns(const QStringList& visible, const QStringList& fieldNames);
    // Выбор в доке: вкладке дока и в схему по умолчанию.
    void chooseShown(const FieldSettings& settings);
    void applyFieldVisibility(LogViewWidget* view);
    void cancelReextraction(const LogViewWidget* view);
    void onReextractionFinished(LogViewWidget* view);
    void finishApplication(LogViewWidget* view);
    void onSavedFieldSettings(LogViewWidget* view, const FieldSettings& saved, bool fromMemory);

    Window m_window;
    SchemaStore::SchemaList m_list;
    FieldSettings m_defaults;
    QPointer<LogViewWidget> m_current;
    QHash<const LogViewWidget*, FieldReextraction*> m_jobs;
    QHash<const FieldReextraction*, int> m_progress; // последнее значение прогресса расчёта
};

#endif // FIELDSCHEMACONTROLLER_H
