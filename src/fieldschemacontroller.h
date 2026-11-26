#ifndef FIELDSCHEMACONTROLLER_H
#define FIELDSCHEMACONTROLLER_H

#include "documentstate.h"
#include "logfile.h"
#include "schemastore.h"

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVector>

#include <functional>
#include <optional>

class FilterRuleSet;
class LogViewWidget;
class QSettings;

// ============================================================================
// FieldSchemaController — каталог схем полей и состояние дока Log Fields без
// виджетов (M-2). Окно держит только вид: комбобокс схем, галочку «Show
// selected fields only» и чекбоксы колонок; всё, что они значат, — здесь.
//
//   • Каталог: load — порядок из ini, файлы <configDir>/patterns/*.json
//     (SchemaStore), разовая миграция списков старых версий из ini, установка
//     встроенных схем (BuiltinSchemas; исправленная встроенная заменяет
//     нетронутую копию, и схема по умолчанию следует за ней); reloadCatalogue —
//     перечитать каталог, сохранив порядок; replaceCatalogue — итог
//     редактора схем; save — порядок и схема по умолчанию в ini.
//   • Схема на вкладку (F-5, PR 1): схема, показ полей и выбор колонок —
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
//   • applyToView — схема, флаг и колонки на одной вкладке. Поля — функция
//     текста и схемы (F-17, шаг 3): записи их не хранят, поэтому смена схемы
//     — O(1): схема в модели вкладки (FieldSource — поколение, кэш пуст),
//     перерисовка видимого, перепривязка применённых правил вкладки к новой
//     раскладке и перефильтрация, только если среди них есть правила по
//     полю. Ни переизвлечения, ни остановки читателей полей, ни reset.
//     Ключевая схема (F-4) идёт тем же путём; смену того, что она решает при
//     чтении, вкладка перечитывает сама (до шага 7 F-17).
//   • «Show selected fields only» (FieldSettings::showFields) — только показ
//     выбранных колонок во view; правила по полю и Entry Details от неё не
//     зависят.
//   • Схема файла слитой вкладки (F-5, PR 3): «Applies to» дока — все файлы
//     вкладки (targetFile пуст) или один файл (filePath). Цель — файл: комбобокс
//     показывает и меняет схему этого файла (applyToFile: своя схема файла
//     вкладки, тот же путь applyToView — O(1) для полей, перечитывание
//     файла при смене ключевой части); показ полей и колонки — вкладки.
//     Цель — все файлы: выбор схемы ставит её вкладке и снимает свои схемы
//     файлов; при своих схемах у файлов комбобокс — «(mixed)» (isMixed).
//     Выбор схемы файла схемой по умолчанию не становится. Своя схема файла
//     из снимка (TabSource.schema) приходит сигналом вкладки
//     savedFileSchemaFound и ищется в каталоге, как схема вкладки.
// ============================================================================
class FieldSchemaController : public QObject {
    Q_OBJECT

public:
    struct Window {
        // Вкладки окна (порядок не важен).
        std::function<QList<LogViewWidget*>()> views;
    };

    explicit FieldSchemaController(QObject* parent = nullptr);
    ~FieldSchemaController() override;

    void setWindow(Window window) { m_window = std::move(window); }

    // ---- Каталог -------------------------------------------------------------
    // ini: группа [View] уже открыта вызывающим.
    void load(QSettings& view);
    void save(QSettings& view) const;
    const SchemaStore::SchemaList& schemas() const { return m_list; }
    // Файлы версии 3, которые перевод при последнем load не перевёл («файл:
    // почему»): файлы целы, в каталоге их нет — окно говорит об этом.
    const QStringList& conversionFailures() const { return m_conversionFailures; }
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
    // Схема вкладки дока (без цели «Applies to») или схема по умолчанию.
    FieldSettings tabSettings() const;

    // ---- Схема дока ----------------------------------------------------------
    int activeIndex() const { return isMixed() ? -1 : indexOf(shownSettings()); }
    // Схема вкладки дока (не цели «Applies to»): верхний уровень сессии.
    QString activeSchema() const { return tabSettings().schema; }
    // Имя для сессии (SessionController::schemaNameFor).
    QString activeSchemaName() const;
    // Схема, которую показывает комбобокс: цели «Applies to» (редактор схем).
    QString shownSchema() const { return shownSettings().schema; }
    // Выбор в комбобоксе (-1 — без схемы).
    void selectSchema(int index);

    // ---- «Applies to» (F-5, PR 3) --------------------------------------------
    // Цель выбора схемы у вкладки дока: пусто — все файлы, иначе filePath
    // файла вкладки. Смена вкладки дока возвращает «все файлы». Шлёт
    // catalogueChanged и fieldsChanged.
    void setTargetFile(const QString& filePath);
    QString targetFile() const;
    // У вкладки дока при цели «все файлы» схемы файлов разные: комбобокс —
    // «(mixed)», activeIndex — -1.
    bool isMixed() const;
    // Своя схема файла file вкладки view (ref — имя и определение).
    void applyToFile(LogViewWidget* view, const LogFilePtr& file, const FieldSettings& ref);

    // ---- «Show selected fields only» и колонки дока --------------------------
    bool showFields() const { return shownSettings().showFields; }
    void setShowFields(bool enabled);
    QStringList fieldNames() const;
    QStringList visibleFieldNames() const { return shownSettings().visibleFields; }
    QVector<int> visibleFieldIndexes() const;
    // Показ колонок включён: флаг и у схемы есть колонки.
    bool fieldDisplayEnabled() const { return showFields() && !fieldNames().isEmpty(); }
    void setVisibleFieldNames(const QStringList& names);
    // Привязать правила фильтра к колонкам схемы вкладки view (Text Filters,
    // Search — вкладке, к которой их применяют).
    static void bindRules(const LogViewWidget* view, FilterRuleSet& rules);

    // ---- Вкладки -------------------------------------------------------------
    // Новая вкладка: схема по умолчанию; подписка на её сохранённую схему.
    void configureView(LogViewWidget* view);
    // Схема, флаг и колонки на вкладке view (см. заголовок); files — свои
    // схемы файлов (filePath → схема; nullptr — оставить как есть).
    void applyToView(LogViewWidget* view, const FieldSettings& settings,
                     const QHash<QString, FieldSettings>* files = nullptr);
    // «Apply Schema to All Tabs»: схема дока на все вкладки.
    void applyToAllViews();
    // Сохранённая схема вкладки против каталога: найдена по определению —
    // её пункт; нет — addMissing: добавить «(from session)» (ошибка записи
    // каталога — в error; схема действует и без пункта), иначе nullopt.
    std::optional<FieldSettings> resolveSaved(const FieldSettings& saved, bool addMissing,
                                              QString* error = nullptr);

signals:
    // Список схем или схема дока изменились — перестроить комбобокс.
    void catalogueChanged();
    // Колонки схемы дока, выбор колонок или флаг изменились.
    void fieldsChanged();
    // Применённые правила вкладки перепривязаны к новой схеме.
    void filtersRebound(LogViewWidget* view);
    // Схема применена к вкладке (окно повторяет поиск по ней).
    void schemaApplied(LogViewWidget* view);
    // Для строки состояния (схема памяти файла не в каталоге, ошибка записи).
    void statusMessage(const QString& text);

private:
    QList<LogViewWidget*> views() const;
    QStringList listOrder() const;
    // Пресеты редактора из ini — схемами каталога (один раз); true — каталог изменился.
    static bool migratePresets(QSettings& s);
    int indexOf(const FieldSettings& settings) const;
    void reload(const QString& extraName);
    // Выбор колонок для схемы: выбранные имена, которые в ней есть; ни
    // одного (или выбор пуст) — все колонки.
    static QStringList keptColumns(const QStringList& visible, const QStringList& fieldNames);
    // Выбор в доке: вкладке дока и в схему по умолчанию; clearFiles — снять
    // свои схемы файлов (выбор схемы для всех файлов).
    void chooseShown(const FieldSettings& settings, bool clearFiles = false);
    LogFilePtr targetLogFile() const;
    void onSavedFileSchema(LogViewWidget* view, const QString& sourceKey,
                           const FieldSettings& saved, bool fromMemory);
    void onLayoutGrown(LogViewWidget* view);
    void applyFieldVisibility(LogViewWidget* view);
    void finishApplication(LogViewWidget* view);
    void onSavedFieldSettings(LogViewWidget* view, const FieldSettings& saved, bool fromMemory);

    Window m_window;
    SchemaStore::SchemaList m_list;
    QStringList m_conversionFailures;
    FieldSettings m_defaults;
    QPointer<LogViewWidget> m_current;
    QString m_targetFile; // «Applies to»: пусто — все файлы
};

#endif // FIELDSCHEMACONTROLLER_H
