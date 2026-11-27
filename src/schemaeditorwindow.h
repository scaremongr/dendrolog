#ifndef SCHEMAEDITORWINDOW_H
#define SCHEMAEDITORWINDOW_H

#include "schemaedit.h"
#include "schemafit.h"
#include "schemastore.h"

#include <QFutureWatcher>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>

class FieldSchemaController;
class LogViewWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabWidget;
class QTableView;
class QTableWidget;
class QTimer;
class QToolButton;
class QUndoStack;
class SchemaCandidatesWindow;
class SchemaFieldCard;
class SchemaFieldStrip;
class SchemaLinePreview;
class SchemaSampleModel;

// ============================================================================
// SchemaEditorWindow — редактор схем полей (F-17, шаг 9; раздел 8 F-17.md;
// графическая правка — U-28), немодальное окно. Схема — шаблон строки
// (linetemplate.h), но правится она НЕ текстом: пользователь видит строки
// своего лога, раскрашенные по полям, и работает с ними; текст шаблона — в
// свёрнутом разделе Advanced.
//
//   • слева — каталог (FieldSchemaController::schemas); у схем, которые
//     читают файл вкладки, — покрытие (SchemaFitJob); New ▾ (Empty, From
//     Sample Lines — SchemaInference, From Template…, Import Grok… —
//     SchemaConverter::fromGrok) и ⋯ (Delete, Import, Export, Open Folder);
//   • сверху справа — Name, Description и подсказка «что делать сейчас»
//     (guideText: нет вкладки; JSON / logfmt; строки не разбиты; схема не
//     читает лог; поля определены сами; обычная) с кнопками Detect Fields
//     (detectFields — SchemaInference по строкам вкладки) и Find a Schema;
//   • поля схемы цветными блоками в ряд (SchemaFieldStrip): имя и «где
//     кончается» словами, между блоками — текст между полями;
//   • строки вкладки, раскрашенные теми же цветами (SchemaLinePreview,
//     вкладка Lines): щелчок по участку выбирает поле, выделение текста —
//     кнопка Make Field у выделения; строка, которую схема не читает, —
//     красным. Вкладка Table — прежняя таблица образцов (SchemaSampleModel:
//     ячейки — то, что увидит вкладка; в заголовке — число разных значений);
//   • карточка выбранного поля (SchemaFieldCard): имя, «где кончается»
//     переключателями с итогом на строках (SchemaEdit::endOptions), значения,
//     Hide / Merge with Next / Remove / Add Value…; у части строки без
//     колонки — Show as a Column;
//   • новое поле — «+ Add field» в конце ряда блоков (и Split… карточки, и
//     меню поля): страница разбиения (beginSplit — пункты
//     SchemaEdit::splitOptions в пуле: найденные разделители, первое слово, N
//     слов, набор значений, форма; свой разделитель — любой текст,
//     splitAtText; пример «первая ▏ вторая»; имена частей), applySplit;
//   • свой разделитель у поля — строка «or at the text» карточки
//     (endFieldAtText: поле кончается на тексте, остальное — новое поле);
//     текст между блоками правится щелчком по нему (selectLiteral,
//     setLiteralText);
//   • покрытие файла (SchemaFit::summary) и, по кнопке, строки, которые схема
//     не приняла, с причиной словами;
//   • Advanced — текст шаблона: ошибка компиляции подчёркнута на месте и
//     названа словами (LineTemplateError), рядом — памятка по синтаксису;
//     раздел раскрывается сам, когда в шаблоне ошибка;
//   • Find a Schema — панелью (SchemaCandidatesWindow встроенным видом):
//     вариант открывается в редакторе новой схемой;
//   • одно сохранение: Apply to Tab (схема вкладке, без перечитывания —
//     FieldSchemaController), Save (в свой файл каталога), Save As…;
//     закрытие и переход к другой схеме с правками спрашивают;
//   • вкладка без схемы — редактор сам разбивает её строки (autoDetect в
//     setView): новая несохранённая схема, закрытие о ней не спрашивает. То
//     же — когда схема вкладки её лог не читает (SchemaFit::Verdict::NoFit по
//     итогу проверки файла): вместо неё показано разбиение, подсказка
//     называет схему и её покрытие; сама схема каталога не тронута и
//     открывается из списка слева. Если же в каталоге есть схема, которая
//     этот лог читает (вердикт Fits по проверке каталога), вместо догадки
//     показана она (preferCatalogueSchema);
//   • правка на строке (шаг 10, SchemaEdit): Make Field — выделение в строке
//     (или в ячейке таблицы: двойной щелчок или F2), затем кнопка или Ctrl+M;
//     Other Variant (Ctrl+Shift+M) — следующая гипотеза конца; меню колонки
//     (блок, заголовок и ячейка таблицы, Shift+F10): Ends at с итогом каждого
//     пункта, Rename…, Add Value…, Merge with Next, Hide, Remove. Образцы
//     оценки — выборка файла вкладки (SchemaFitJob, до 2300 строк); всё
//     считается в пуле, итог «Ends at» — заранее, после каждой правки;
//   • QUndoStack на имя, описание и шаблон (Ctrl+Z / Ctrl+Shift+Z); каждая
//     операция правки на строке — отдельный шаг отмены;
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
    /// Detect Fields: разбить строки вкладки на поля (SchemaInference). У
    /// новой схемы — шаг отмены, у схемы каталога — новая несохранённая
    /// (с вопросом о правках). false — не вышло (причина — editMessage()).
    bool detectFields();
    /// Схема на экране — итог Detect Fields (или разбиения при открытии),
    /// ещё не правленный.
    bool isDetected() const;
    bool newFromTemplate(const QString& templateText, const QString& name = QString());
    /// Grok → шаблон новой схемы; error — почему нет.
    bool importGrok(const QString& grok, QString* error = nullptr);
    bool applyToTab();
    bool save();
    bool saveAs(const QString& name);

    // ---- Правка на строке (F-17, шаг 10) ---------------------------------------
    /// Образцы оценки правок: выборка файла вкладки (SchemaFitJob), без
    /// вкладки — строки таблицы.
    QVector<SchemaFit::Sample> editSamples() const;
    /// Make Field: [from, from + length) значения ячейки (row, column) таблицы
    /// образцов — полем (в пуле; итог — новый шаблон, шаг отмены). false — не
    /// начато (причина — editMessage()).
    bool makeField(int row, int column, int from, int length);
    /// Make Field по раскрашенным строкам: [start, start + length) образца
    /// №line (SchemaLinePreview::lines). false — не начато (editMessage()).
    bool makeFieldInLine(int line, int start, int length);
    /// Выделение в раскрашенных строках (вкладка Lines) или в открытом
    /// редакторе ячейки таблицы (двойной щелчок или F2).
    bool makeFieldFromSelection();
    /// Следующая гипотеза последнего Make Field вместо текущей (тот же шаг
    /// отмены). false — нечего или шаблон с тех пор правили.
    bool otherVariant();
    const SchemaEdit::MakeFieldResult& makeFieldResult() const { return m_make; }
    int variantIndex() const { return m_variantIndex; }
    QString editMessage() const { return m_editText; }
    /// Итог «Ends at» посчитан для нынешнего шаблона.
    bool endOptionsReady() const;
    QVector<SchemaEdit::EndOption> endOptions(int column) const;
    bool setFieldEnd(int column, int option);
    bool renameField(int column, const QString& name);
    bool hideField(int column);
    bool removeField(int column);
    bool mergeWithNext(int column);
    bool addValue(int column, const QString& value);
    /// Значения для Add Value… (по строкам, где разбор остановился на поле).
    QStringList suggestedValues(int column) const;
    /// Часть строки без колонки («{_}», элемент шаблона piece) — колонкой.
    bool showHiddenField(int piece);
    /// Меню колонки — то же, что у заголовка; владелец — вызывающий.
    QMenu* columnMenu(int column);

    // ---- Графическая правка (U-28) ---------------------------------------------
    /// Выбранное поле: рамка блока, подчёркивание в строках, карточка. -1 — нет.
    int selectedColumn() const { return m_selectedColumn; }
    void selectColumn(int column);
    /// Подсказка «что делать сейчас» (как на экране).
    QString guideText() const { return m_guideText; }
    /// Раздел Advanced (текст шаблона) раскрыт.
    bool isAdvancedShown() const;
    void setAdvancedShown(bool shown);

    // ---- Создание и правка блоков (U-28) -----------------------------------------
    /// Страница «разбить поле на два» карточки — так в схеме появляется новое
    /// поле («+ Add field» ряда блоков — для последнего поля, Split… карточки и
    /// меню поля — для выбранного). false — нечего разбивать.
    bool beginSplit(int column);
    void cancelSplit();
    int splitColumn() const { return m_splitColumn; }
    /// Пункты разбиения посчитаны для нынешнего шаблона.
    bool splitOptionsReady() const;
    QVector<SchemaEdit::SplitOption> splitOptions() const;
    /// Разбить выбранным способом; имена частей: пустое — как у пункта.
    bool applySplit(const SchemaEdit::SplitOption& option, const QString& first = QString(),
                    const QString& second = QString());
    /// Поле кончается на тексте text — любом; то, что за ним, становится новым
    /// полем (в пуле; итог — шаг отмены). false — не начато (editMessage()).
    bool endFieldAtText(int column, const QString& text);
    /// Текст между полями (элемент шаблона piece) — другим; пустой — убрать.
    bool setLiteralText(int piece, const QString& text);
    /// Выбрать текст между полями (карточка даёт его поправить); -1 — нет.
    void selectLiteral(int piece);
    int selectedLiteral() const { return m_selectedLiteral; }
    /// Вопрос текста (Rename…, Add Value…). Тест: без диалога; пусто — отмена.
    using TextAsker = std::function<QString(const QString& title, const QString& label, const QString& text,
                                            const QStringList& items)>;
    void setTextAsker(TextAsker asker) { m_textAsker = std::move(asker); }

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
    void rebuildStrip();
    void rebuildPreview();
    void updateCard();
    void updateGuide();
    bool autoDetect(bool keepIfNone = false);
    void preferCatalogueSchema();
    QString suggestedName() const;
    void selectHiddenPiece(int piece);
    void onTemplateChanged();
    void recompute();
    void startFileFit();
    void startCatalogueFit();
    void refreshSamples();
    void updateChrome();
    void showMisses();
    void onListRowChanged(int row);

    struct OpOutcome {
        int generation = 0;
        QString base;            ///< шаблон на старте: правили с тех пор — итог отброшен
        QString label;
        bool make = false;
        bool deselect = false;   ///< поле ушло (Remove): выбор снять
        int select = -1;         ///< колонка, которую выбрать после (новое поле)
        QString note;            ///< что получилось, словами (вместо общего «done»)
        SchemaEdit::MakeFieldResult result;
        SchemaEdit::Edit edit;
    };
    struct EndsOutcome {
        int generation = 0;
        QString base;
        QVector<QVector<SchemaEdit::EndOption>> options;   ///< по колонке
        QVector<QStringList> suggestions;                   ///< по колонке (только of)
        QVector<QStringList> delimiters;                    ///< по колонке: разделители в значениях
    };
    struct SplitOutcome {
        int generation = 0;
        QString base;
        int column = -1;
        QVector<SchemaEdit::SplitOption> options;
        QStringList delimiters;
    };
    struct SplitTextOutcome {
        int generation = 0;
        QString base;
        int column = -1;
        QString text;
        SchemaEdit::SplitOption option;
    };
    void startSplitOptions();
    void onSplitFinished();
    void startSplitText();
    void onSplitTextFinished();
    void updateTextEndHint(int column, const QString& text);
    using OpWork = std::function<OpOutcome(const SchemaEdit::Options&)>;
    void startOperation(const QString& label, bool make, OpWork work);
    void onOperationFinished();
    void applyOperation(const QString& text, const QString& label);
    void applyEdit(const SchemaEdit::Edit& edit, const QString& label);
    void startEndOptions();
    void onEndsFinished();
    int currentColumn() const;
    QString askText(const QString& title, const QString& label, const QString& text,
                    const QStringList& items = {});
    void setEditMessage(const QString& text);
    bool variantOnTop() const;
    void resetMakeField();
    void updateEditActions();

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

    // Правка на строке.
    TextAsker m_textAsker;
    QVector<SchemaFit::Sample> m_fileSamples;   ///< выборка файла последнего SchemaFitJob
    QFutureWatcher<OpOutcome>* m_opWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_opCancel;
    int m_opGeneration = 0;
    bool m_opPending = false;     ///< задание запущено, итог ещё не доставлен
    QFutureWatcher<EndsOutcome>* m_endsWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_endsCancel;
    int m_endsGeneration = 0;
    bool m_endsPending = false;
    EndsOutcome m_ends;
    SchemaEdit::MakeFieldResult m_make;
    int m_variantIndex = -1;
    int m_variantUndoIndex = -1;
    quint64 m_variantSerial = 0;
    quint64 m_lastOperationSerial = 0;
    struct CellSelection {
        int row = -1;
        int column = -1;
        int from = 0;
        int length = 0;
    };
    CellSelection m_cellSelection;   ///< последнее выделение в поле ячейки
    QString m_editText;
    QPointer<QLineEdit> m_cellEditor;
    QPersistentModelIndex m_cellEditorIndex;
    QLabel* m_editLabel = nullptr;
    QPushButton* m_makeFieldButton = nullptr;
    QPushButton* m_otherVariantButton = nullptr;
    QAction* m_makeFieldAction = nullptr;
    QAction* m_otherVariantAction = nullptr;

    // Графическая правка (U-28).
    QLabel* m_guide = nullptr;
    QString m_guideText;
    QPushButton* m_detectButton = nullptr;
    SchemaFieldStrip* m_strip = nullptr;
    SchemaLinePreview* m_preview = nullptr;
    SchemaFieldCard* m_card = nullptr;
    QScrollArea* m_cardScroll = nullptr;
    QTabWidget* m_sampleTabs = nullptr;
    QToolButton* m_missesToggle = nullptr;
    QWidget* m_missesBox = nullptr;
    QToolButton* m_advancedToggle = nullptr;
    QWidget* m_advancedBox = nullptr;
    int m_selectedColumn = -1;
    int m_selectedPiece = -1;      ///< выбранная часть строки без колонки
    bool m_selecting = false;      ///< выбор расходится по видам — без петли
    QString m_detectedTemplate;    ///< шаблон последнего Detect Fields
    QString m_detectFailure;       ///< почему Detect Fields не вышел (до правки)
    // Разбиение поля и текст между полями.
    int m_splitColumn = -1;        ///< поле, которое разбивается (страница карточки); -1 — нет
    int m_selectedLiteral = -1;    ///< выбранный текст между полями (элемент шаблона)
    QFutureWatcher<SplitOutcome>* m_splitWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_splitCancel;
    int m_splitGeneration = 0;
    bool m_splitPending = false;
    SplitOutcome m_split;
    QFutureWatcher<SplitTextOutcome>* m_splitTextWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_splitTextCancel;
    int m_splitTextGeneration = 0;
    bool m_splitTextPending = false;
    QString m_splitTextWanted;     ///< свой разделитель, набранный на странице разбиения
    QTimer* m_splitTextTimer = nullptr;
    QString m_checkTabSchema;      ///< схема вкладки, загруженная setView: не читает лог — разбить самому
    QString m_replacedSchema;      ///< имя схемы вкладки, вместо которой показано разбиение
    QString m_replacedPercent;     ///< её покрытие
    bool m_detectedOnOpen = false; ///< разбиение показано само: схема каталога, читающая лог, главнее
    QString m_offeredSchema;       ///< схема каталога, показанная вместо разбиения
    QString m_offeredPercent;
};

#endif // SCHEMAEDITORWINDOW_H
