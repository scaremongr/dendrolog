#ifndef SCHEMAEDITORWINDOW_H
#define SCHEMAEDITORWINDOW_H

#include "schemafit.h"
#include "schemastore.h"

#include <QPointer>
#include <QWidget>

#include <functional>

class FieldSchemaController;
class LogViewWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableView;
class QTableWidget;
class QTimer;
class QToolButton;
class QUndoStack;
class SchemaCandidatesWindow;
class SchemaSampleModel;

// ============================================================================
// SchemaEditorWindow — редактор схем полей (F-17, шаг 9; раздел 8 F-17.md),
// немодальное окно вместо прежнего ConversionPatternDialog (блочной модели
// больше нет). Схема — шаблон строки (linetemplate.h), правится текстом:
//
//   • слева — каталог (FieldSchemaController::schemas) с покрытием файла
//     вкладки у каждой схемы (SchemaFitJob), New ▾ (Empty, From Sample
//     Lines — SchemaInference, From Template…, Import Grok… —
//     SchemaConverter::fromGrok) и ⋯ (Delete, Import, Export, Open Folder);
//   • справа — Name, Description, Template: ошибка компиляции подчёркнута на
//     месте и названа словами (LineTemplateError);
//   • таблица образцов (SchemaSampleModel): строки вкладки с текущей, ячейки
//     — ровно то, что увидит вкладка; в заголовке колонки — число разных
//     значений, в подсказке — частые со счётчиками;
//   • «Did not match» — строки файла вкладки, которые схема не приняла, с
//     причиной словами; под ним — покрытие файла (SchemaFit::summary);
//   • Find a Schema — панелью (SchemaCandidatesWindow встроенным видом):
//     вариант открывается в редакторе новой схемой;
//   • одно сохранение: Apply to Tab (схема вкладке, без перечитывания —
//     FieldSchemaController), Save (в свой файл каталога), Save As…;
//     закрытие и переход к другой схеме с правками спрашивают;
//   • QUndoStack на имя, описание и шаблон (Ctrl+Z / Ctrl+Shift+Z);
//   • переименование поля (колонка на том же месте под другим именем)
//     предупреждает о правилах фильтров вкладок и профилях с этим именем и
//     предлагает переименовать там (FieldUses — окно даёт обход панелей).
//
// Вся работа извлекателя — в пуле (SchemaSampleModel, SchemaFitJob): схема с
// /регексом/ на GUI-потоке не вызывается (инвариант 12).
// ============================================================================
class SchemaEditorWindow : public QWidget {
    Q_OBJECT
public:
    /// Где ещё живут имена колонок: правила фильтров вкладок со схемой
    /// schema (прежнее определение правимой) и правила панелей и их
    /// профилей. count — сколько правил ссылаются на колонку, rename —
    /// переименовать их.
    struct FieldUses {
        std::function<int(const QString& field, const QString& schema)> count;
        std::function<void(const QString& from, const QString& to, const QString& schema)> rename;
    };
    /// Ответ на вопрос окна (тест: без диалога; предупреждения приходят туда
    /// же, ответ не важен). Пусто — QMessageBox.
    enum class Answer { Save, Discard, Cancel, Yes, No };
    using Asker = std::function<Answer(const QString& question, bool saveDiscard)>;

    explicit SchemaEditorWindow(FieldSchemaController* schemas, QWidget* parent = nullptr);
    ~SchemaEditorWindow() override;

    void setFieldUses(FieldUses uses) { m_uses = std::move(uses); }
    void setAsker(Asker asker) { m_asker = std::move(asker); }
    /// Бюджет проверки на файле вкладки (шов теста: 0 — без предела).
    void setFitBudget(const SchemaFit::Budget& budget) { m_fitBudget = budget; }

    /// Вкладка-источник: образцы — её строки с текущей, покрытие — по файлу
    /// текущей строки. Открывает схему вкладки (если правок нет).
    void setView(LogViewWidget* view);
    LogViewWidget* view() const { return m_view; }

    // ---- Состояние (тест и окно) ---------------------------------------------
    QString schemaName() const;
    QString description() const;
    QString templateText() const;
    /// Определение правимой схемы (шаблон версии 4); шаблон с ошибкой — пусто.
    QString definition() const;
    QString templateError() const;
    bool isModified() const;
    /// Правится схема каталога с этим именем (пусто — новая, не сохранённая).
    QString savedName() const;
    bool isComputing() const;
    /// Последнее предупреждение окна (тест: Asker получает его вопросом).
    QString lastWarning() const { return m_lastWarning; }
    const SchemaFit::Result& fileFit() const { return m_fileFit; }
    bool fileFitReady() const { return m_fileFitReady; }
    SchemaSampleModel* sampleModel() const { return m_samples; }
    QUndoStack* undoStack() const { return m_undo; }

    // ---- Действия ------------------------------------------------------------
    /// Открыть схему каталога (с вопросом о правках). false — отменено.
    bool openSchema(const QString& name);
    void setTemplateText(const QString& text);
    void setSchemaName(const QString& name);
    void setDescription(const QString& text);
    bool newEmpty();
    bool newFromSampleLines();
    bool newFromTemplate(const QString& templateText, const QString& name = QString());
    /// Grok → шаблон новой схемы; error — почему нет.
    bool importGrok(const QString& grok, QString* error = nullptr);
    bool applyToTab();
    bool save();
    bool saveAs(const QString& name);

signals:
    /// Схема записана в каталог (имя).
    void schemaSaved(const QString& name);
    void appliedToTab(LogViewWidget* view);
    /// Панель Find a Schema: «Set Up AI…» — Settings → AI.
    void settingsRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct EditState {
        QString name;
        QString description;
        QString templateText;
        bool operator==(const EditState& o) const
        {
            return name == o.name && description == o.description && templateText == o.templateText;
        }
        bool operator!=(const EditState& o) const { return !(*this == o); }
    };
    class EditCommand;
    friend class EditCommand;

    static EditState stateOf(const SchemaStore::SchemaEntry& entry);
    void pushEdit(int field, const EditState& next);
    void setState(const EditState& state);   // в поля, без команды
    void loadEntry(const SchemaStore::SchemaEntry& entry, bool isNew);
    bool resolveModified();                  // правки: сохранить / отбросить / отмена
    Answer ask(const QString& question, bool saveDiscard);
    void warn(const QString& title, const QString& text);
    bool confirmRenames(const QString& oldDefinition, const QString& newDefinition);

    void rebuildList();
    void onTemplateChanged();
    void recompute();
    void startFileFit();
    void startCatalogueFit();
    void refreshSamples();
    void updateChrome();
    void showMisses();
    void onListRowChanged(int row);

    FieldSchemaController* m_schemas = nullptr;
    QPointer<LogViewWidget> m_view;
    FieldUses m_uses;
    Asker m_asker;
    QString m_lastWarning;

    SchemaStore::SchemaEntry m_original;   ///< сохранённая версия (новая — пустое имя)
    bool m_isNew = false;
    bool m_newIsWork = false;   ///< новая не пустая: закрытие спрашивает
    EditState m_state;
    bool m_syncing = false;

    QUndoStack* m_undo = nullptr;
    SchemaSampleModel* m_samples = nullptr;
    SchemaFitJob* m_fileJob = nullptr;
    SchemaFitJob* m_catalogueJob = nullptr;
    SchemaFit::Result m_fileFit;
    SchemaFit::Budget m_fitBudget;
    bool m_fileFitReady = false;
    QVector<SchemaFit::Result> m_catalogueFits;
    QTimer* m_recompute = nullptr;

    QListWidget* m_list = nullptr;
    QToolButton* m_newButton = nullptr;
    QToolButton* m_moreButton = nullptr;
    QLineEdit* m_name = nullptr;
    QLineEdit* m_descriptionEdit = nullptr;
    QPlainTextEdit* m_template = nullptr;
    QLabel* m_error = nullptr;
    QTableView* m_table = nullptr;
    QLabel* m_missesLabel = nullptr;
    QTableWidget* m_misses = nullptr;
    QLabel* m_fileLabel = nullptr;
    QPushButton* m_findButton = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_save = nullptr;
    QPushButton* m_saveAs = nullptr;
    QSplitter* m_mainSplitter = nullptr;
    SchemaCandidatesWindow* m_find = nullptr;
};

#endif // SCHEMAEDITORWINDOW_H
