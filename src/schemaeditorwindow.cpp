#include "schemaeditorwindow.h"

#include "fieldlayout.h"
#include "fieldschemacontroller.h"
#include "linetemplate.h"
#include "LogListView.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "schemacandidateswindow.h"
#include "schemaconverter.h"
#include "schemainference.h"
#include "schemasamplemodel.h"

#include <QAction>
#include <QCloseEvent>
#include <QStyledItemDelegate>
#include <QtConcurrent/QtConcurrentRun>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTableView>
#include <QTableWidget>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr int kSampleLines = 200;
constexpr int kRecomputeMs = 300;

enum Field { NameField = 1, DescriptionField = 2, TemplateField = 3, AllFields = 4 };

QLabel* mutedLabel(QWidget* parent)
{
    // Роль палитры, а не строка стиля: следует за темой сама (theme_mode).
    auto* label = new QLabel(parent);
    label->setForegroundRole(QPalette::PlaceholderText);
    label->setWordWrap(true);
    return label;
}

QString templateOf(const QString& definition)
{
    QString text;
    LineTemplateSchema::templateOf(definition, &text);
    return text;
}

QStringList columnsOf(const QString& templateText)
{
    if (templateText.isEmpty())
        return {};
    const LineTemplate tpl(templateText);
    return tpl.isValid() ? tpl.columns() : QStringList();
}

// Строка Template: одна строка текста, ошибка компиляции подчёркнута на месте.
class TemplateEdit : public QPlainTextEdit {
public:
    explicit TemplateEdit(QWidget* parent)
        : QPlainTextEdit(parent)
    {
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTabChangesFocus(true);
        setUndoRedoEnabled(false);   // правки ведёт QUndoStack окна
        QFont mono = font();
        mono.setFamily(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        setFont(mono);
        const int line = fontMetrics().lineSpacing();
        setFixedHeight(line + 2 * int(document()->documentMargin()) + frameWidth() * 2
                       + horizontalScrollBar()->sizeHint().height());
    }

    void setErrorAt(int position)
    {
        QList<QTextEdit::ExtraSelection> selections;
        if (position >= 0) {
            QTextEdit::ExtraSelection sel;
            sel.format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
            sel.format.setUnderlineColor(QColor(0xd0, 0x30, 0x30));
            QTextCursor c(document());
            const int length = document()->characterCount() - 1;
            const int from = qBound(0, position, qMax(0, length - 1));
            c.setPosition(from);
            c.setPosition(qMin(length, from + 1), QTextCursor::KeepAnchor);
            if (length == 0) {
                c.setPosition(0);
            }
            sel.cursor = c;
            selections << sel;
        }
        setExtraSelections(selections);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        // Одна строка: Enter не переносит.
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            e->ignore();
            return;
        }
        QPlainTextEdit::keyPressEvent(e);
    }
    void insertFromMimeData(const QMimeData* source) override
    {
        QString text = source->text();
        text.replace(QLatin1Char('\n'), QLatin1Char(' ')).remove(QLatin1Char('\r'));
        insertPlainText(text);
    }
};

// «Редактор» ячейки таблицы образцов — только для выделения текста (Make
// Field): поле только для чтения, значение как в строке.
class CellSelectDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<void(QLineEdit*, const QModelIndex&)> onCreate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex& index) const override
    {
        auto* e = new QLineEdit(parent);
        e->setObjectName(QStringLiteral("cellSelectEdit"));
        e->setReadOnly(true);
        e->setFrame(false);
        e->setToolTip(QObject::tr("Select part of the value and press Make Field (Ctrl+M)"));
        if (onCreate)
            onCreate(e, index);
        return e;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        auto* e = static_cast<QLineEdit*>(editor);
        e->setText(index.data(Qt::EditRole).toString());
        e->selectAll();
    }
    void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override {}
};

} // namespace

// ---------------------------------------------------------------------------
// Команда правки: снимок «имя, описание, шаблон» до и после; подряд идущие
// правки одного поля сливаются (набор текста — одна отмена на серию).
// ---------------------------------------------------------------------------
class SchemaEditorWindow::EditCommand : public QUndoCommand {
public:
    EditCommand(SchemaEditorWindow* w, int field, EditState before, EditState after)
        : m_window(w), m_field(field), m_before(std::move(before)), m_after(std::move(after))
    {
        setText(field == TemplateField ? QObject::tr("Edit template")
                : field == NameField   ? QObject::tr("Rename schema")
                : field == DescriptionField ? QObject::tr("Edit description")
                                            : QObject::tr("Replace schema"));
    }
    int id() const override { return m_field == AllFields ? -1 : m_field; }
    /// Номер команды за время жизни окна (Other Variant узнаёт «свою»
    /// команду Make Field по нему, а не по адресу: удалённую сменит новая).
    quint64 serial() const { return m_serial; }
    bool mergeWith(const QUndoCommand* other) override
    {
        const auto* o = static_cast<const EditCommand*>(other);
        if (o->m_field != m_field)
            return false;
        m_after = o->m_after;
        return true;
    }
    void undo() override { m_window->setState(m_before); }
    void redo() override
    {
        if (m_first) {   // уже в полях: пользователь только что ввёл
            m_first = false;
            return;
        }
        m_window->setState(m_after);
    }

private:
    SchemaEditorWindow* m_window;
    int m_field;
    EditState m_before;
    EditState m_after;
    bool m_first = true;
    quint64 m_serial = nextSerial();
    static quint64 nextSerial()
    {
        static quint64 n = 0;
        return ++n;
    }
};

SchemaEditorWindow::SchemaEditorWindow(FieldSchemaController* schemas, QWidget* parent)
    : QWidget(parent, Qt::Window)
    , m_schemas(schemas)
    , m_undo(new QUndoStack(this))
    , m_samples(new SchemaSampleModel(this))
    , m_fileJob(new SchemaFitJob(this))
    , m_catalogueJob(new SchemaFitJob(this))
    , m_recompute(new QTimer(this))
    , m_opWatcher(new QFutureWatcher<OpOutcome>(this))
    , m_endsWatcher(new QFutureWatcher<EndsOutcome>(this))
{
    setObjectName(QStringLiteral("schemaEditorWindow"));
    setWindowTitle(tr("Field Schemas"));
    resize(1100, 700);
    m_recompute->setSingleShot(true);
    m_recompute->setInterval(kRecomputeMs);

    auto* root = new QVBoxLayout(this);
    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    root->addWidget(m_mainSplitter, 1);

    // ---- Слева: каталог ------------------------------------------------------
    auto* left = new QWidget(m_mainSplitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    m_list = new QListWidget(left);
    m_list->setObjectName(QStringLiteral("schemaList"));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    leftLayout->addWidget(m_list, 1);
    auto* listButtons = new QHBoxLayout();
    m_newButton = new QToolButton(left);
    m_newButton->setObjectName(QStringLiteral("newSchemaBtn"));
    m_newButton->setText(tr("New"));
    m_newButton->setPopupMode(QToolButton::InstantPopup);
    auto* newMenu = new QMenu(m_newButton);
    QAction* emptyAction = newMenu->addAction(tr("Empty"));
    emptyAction->setObjectName(QStringLiteral("newEmptyAction"));
    QAction* samplesAction = newMenu->addAction(tr("From Sample Lines"));
    samplesAction->setObjectName(QStringLiteral("newFromSamplesAction"));
    QAction* templateAction = newMenu->addAction(tr("From Template…"));
    templateAction->setObjectName(QStringLiteral("newFromTemplateAction"));
    QAction* grokAction = newMenu->addAction(tr("Import Grok…"));
    grokAction->setObjectName(QStringLiteral("importGrokAction"));
    m_newButton->setMenu(newMenu);
    m_moreButton = new QToolButton(left);
    m_moreButton->setObjectName(QStringLiteral("schemaMenuBtn"));
    m_moreButton->setText(QStringLiteral("⋯"));
    m_moreButton->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(m_moreButton);
    QAction* deleteAction = moreMenu->addAction(tr("Delete Schema…"));
    deleteAction->setObjectName(QStringLiteral("deleteSchemaAction"));
    QAction* importAction = moreMenu->addAction(tr("Import File…"));
    QAction* exportAction = moreMenu->addAction(tr("Export…"));
    QAction* folderAction = moreMenu->addAction(tr("Open Schemas Folder"));
    m_moreButton->setMenu(moreMenu);
    listButtons->addWidget(m_newButton);
    listButtons->addWidget(m_moreButton);
    listButtons->addStretch(1);
    leftLayout->addLayout(listButtons);

    // ---- Справа: схема -------------------------------------------------------
    auto* right = new QWidget(m_mainSplitter);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    auto* form = new QFormLayout();
    auto* nameRow = new QHBoxLayout();
    m_name = new QLineEdit(right);
    m_name->setObjectName(QStringLiteral("nameEdit"));
    nameRow->addWidget(m_name, 1);
    m_findButton = new QPushButton(tr("Find a Schema"), right);
    m_findButton->setObjectName(QStringLiteral("findSchemaBtn"));
    m_findButton->setCheckable(true);
    m_findButton->setToolTip(tr("Show the schemas of the catalogue, the inferred ones and the AI suggestion "
                                "for the lines of the tab"));
    nameRow->addWidget(m_findButton);
    form->addRow(tr("Name"), nameRow);
    m_descriptionEdit = new QLineEdit(right);
    m_descriptionEdit->setObjectName(QStringLiteral("descriptionEdit"));
    form->addRow(tr("Description"), m_descriptionEdit);
    m_template = new TemplateEdit(right);
    m_template->setObjectName(QStringLiteral("templateEdit"));
    m_template->setToolTip(tr(
        "The line template: text stays as in the line, {Name:kind} is a field.\n"
        "Kinds: time, level, int, num, hex, uuid, ip, path, quoted — the field ends where its form ends;\n"
        "word — at a space; text — at what follows in the template; greedy — at the last occurrence;\n"
        "padded — at two or more spaces; width(N); words(N); of(a|b|c) — one of the values;\n"
        "rest — at the end of the line; /regex/ — a regular expression (slow: fields come in the background).\n"
        "{? … ?} — an optional part; {_} — parsed, but not a column."));
    form->addRow(tr("Template"), m_template);
    rightLayout->addLayout(form);
    m_error = new QLabel(right);
    m_error->setObjectName(QStringLiteral("templateError"));
    m_error->setWordWrap(true);
    {
        QPalette p = m_error->palette();
        p.setColor(QPalette::WindowText, QColor(0xc0, 0x30, 0x30));
        m_error->setPalette(p);
    }
    m_error->hide();
    rightLayout->addWidget(m_error);

    // Правка на строке (шаг 10).
    auto* editRow = new QHBoxLayout();
    m_makeFieldButton = new QPushButton(tr("Make Field"), right);
    m_makeFieldButton->setObjectName(QStringLiteral("makeFieldBtn"));
    m_makeFieldButton->setFocusPolicy(Qt::NoFocus);   // щелчок не закрывает поле выделения
    m_makeFieldButton->setToolTip(tr("Double-click a cell (or press F2), select part of its value and make it "
                                     "a field (Ctrl+M)"));
    m_otherVariantButton = new QPushButton(tr("Other Variant"), right);
    m_otherVariantButton->setObjectName(QStringLiteral("otherVariantBtn"));
    m_otherVariantButton->setFocusPolicy(Qt::NoFocus);
    m_otherVariantButton->setToolTip(tr("The next way to end the new field (Ctrl+Shift+M)"));
    m_editLabel = mutedLabel(right);
    m_editLabel->setObjectName(QStringLiteral("editMessage"));
    m_editLabel->setText(tr("Double-click a cell, select part of its value and press Make Field. "
                            "Right-click a column header for where the field ends."));
    editRow->addWidget(m_makeFieldButton);
    editRow->addWidget(m_otherVariantButton);
    editRow->addWidget(m_editLabel, 1);
    rightLayout->addLayout(editRow);

    auto* tables = new QSplitter(Qt::Vertical, right);
    m_table = new QTableView(tables);
    m_table->setObjectName(QStringLiteral("sampleTable"));
    m_table->setModel(m_samples);
    {
        auto* delegate = new CellSelectDelegate(m_table);
        delegate->onCreate = [this](QLineEdit* e, const QModelIndex& index) {
            m_cellEditor = e;
            m_cellEditorIndex = index;
            // Выделение запоминается: щелчок по кнопке или правый щелчок
            // уводит фокус, и поле теряет выделение (и закрывается).
            m_cellSelection = CellSelection();
            const QPersistentModelIndex cell(index);
            connect(e, &QLineEdit::selectionChanged, this, [this, e, cell]() {
                if (!e->hasSelectedText() || !cell.isValid())
                    return;
                m_cellSelection = CellSelection{cell.row(), cell.column(), e->selectionStart(),
                                                int(e->selectedText().size())};
            });
        };
        m_table->setItemDelegate(delegate);
    }
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    auto* missBox = new QWidget(tables);
    auto* missLayout = new QVBoxLayout(missBox);
    missLayout->setContentsMargins(0, 0, 0, 0);
    m_missesLabel = new QLabel(missBox);
    m_missesLabel->setObjectName(QStringLiteral("missesLabel"));
    missLayout->addWidget(m_missesLabel);
    m_misses = new QTableWidget(missBox);
    m_misses->setObjectName(QStringLiteral("missesTable"));
    m_misses->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_misses->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_misses->verticalHeader()->setVisible(false);
    m_misses->setWordWrap(false);
    m_misses->setColumnCount(3);
    m_misses->setHorizontalHeaderLabels({tr("Line"), tr("Why"), tr("Text")});
    m_misses->horizontalHeader()->setStretchLastSection(true);
    missLayout->addWidget(m_misses, 1);
    tables->setStretchFactor(0, 3);
    tables->setStretchFactor(1, 1);
    rightLayout->addWidget(tables, 1);
    m_fileLabel = mutedLabel(right);
    m_fileLabel->setObjectName(QStringLiteral("fileFitLabel"));
    rightLayout->addWidget(m_fileLabel);

    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 4);
    m_mainSplitter->setSizes({240, 860});

    // ---- Кнопки --------------------------------------------------------------
    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    m_apply = new QPushButton(tr("Apply to Tab"), this);
    m_apply->setObjectName(QStringLiteral("applyToTabBtn"));
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("saveBtn"));
    m_saveAs = new QPushButton(tr("Save As…"), this);
    m_saveAs->setObjectName(QStringLiteral("saveAsBtn"));
    auto* close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("closeBtn"));
    buttons->addWidget(m_apply);
    buttons->addWidget(m_save);
    buttons->addWidget(m_saveAs);
    buttons->addWidget(close);
    root->addLayout(buttons);

    // ---- Отмена правок -------------------------------------------------------
    QAction* undo = m_undo->createUndoAction(this, tr("Undo"));
    undo->setShortcut(QKeySequence::Undo);
    QAction* redo = m_undo->createRedoAction(this, tr("Redo"));
    redo->setShortcut(QKeySequence::Redo);
    addAction(undo);
    addAction(redo);
    // Правка на строке: клавиатура (колонка — текущая ячейка таблицы).
    m_makeFieldAction = new QAction(tr("Make Field"), this);
    m_makeFieldAction->setObjectName(QStringLiteral("makeFieldAction"));
    m_makeFieldAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
    m_otherVariantAction = new QAction(tr("Other Variant"), this);
    m_otherVariantAction->setObjectName(QStringLiteral("otherVariantAction"));
    m_otherVariantAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M));
    auto* columnMenuAction = new QAction(tr("Column Menu"), this);
    columnMenuAction->setObjectName(QStringLiteral("columnMenuAction"));
    columnMenuAction->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F10));
    auto* mergeAction = new QAction(tr("Merge with Next"), this);
    mergeAction->setObjectName(QStringLiteral("mergeFieldShortcut"));
    mergeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));
    auto* renameAction = new QAction(tr("Rename Field…"), this);
    renameAction->setObjectName(QStringLiteral("renameFieldShortcut"));
    renameAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    auto* hideAction = new QAction(tr("Hide Field"), this);
    hideAction->setObjectName(QStringLiteral("hideFieldShortcut"));
    hideAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H));
    auto* removeAction = new QAction(tr("Remove Field"), this);
    removeAction->setObjectName(QStringLiteral("removeFieldShortcut"));
    removeAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
    auto* addValueAction = new QAction(tr("Add Value…"), this);
    addValueAction->setObjectName(QStringLiteral("addValueShortcut"));
    addValueAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    // Действия над колонкой — только в таблице (и её поле выделения): в
    // полях имени и шаблона Shift+F10 и Ctrl+R… остаются их собственными.
    addAction(m_otherVariantAction);
    for (QAction* a : {m_makeFieldAction, columnMenuAction, mergeAction, renameAction, hideAction, removeAction,
                       addValueAction}) {
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        m_table->addAction(a);
    }
    connect(m_makeFieldAction, &QAction::triggered, this, &SchemaEditorWindow::makeFieldFromSelection);
    connect(m_makeFieldButton, &QPushButton::clicked, this, &SchemaEditorWindow::makeFieldFromSelection);
    connect(m_otherVariantAction, &QAction::triggered, this, &SchemaEditorWindow::otherVariant);
    connect(m_otherVariantButton, &QPushButton::clicked, this, &SchemaEditorWindow::otherVariant);
    connect(columnMenuAction, &QAction::triggered, this, [this]() {
        const int column = currentColumn();
        if (column < 0)
            return;
        QMenu* menu = columnMenu(column);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        const QRect r = m_table->visualRect(m_table->currentIndex());
        menu->popup(m_table->viewport()->mapToGlobal(r.isValid() ? r.bottomLeft() : QPoint()));
    });
    connect(mergeAction, &QAction::triggered, this, [this]() { mergeWithNext(currentColumn()); });
    connect(hideAction, &QAction::triggered, this, [this]() { hideField(currentColumn()); });
    connect(removeAction, &QAction::triggered, this, [this]() { removeField(currentColumn()); });
    connect(renameAction, &QAction::triggered, this, [this]() {
        const int column = currentColumn();
        const QStringList columns = columnsOf(m_state.templateText.trimmed());
        if (column < 0 || column >= columns.size())
            return;
        const QString name = askText(tr("Rename Field"), tr("Name:"), columns[column]);
        if (!name.isEmpty())
            renameField(column, name);
    });
    connect(addValueAction, &QAction::triggered, this, [this]() {
        const int column = currentColumn();
        if (column < 0)
            return;
        const QStringList items = suggestedValues(column);
        const QString value = askText(tr("Add Value"), tr("Value:"), items.value(0), items);
        if (!value.isEmpty())
            addValue(column, value);
    });
    connect(m_table->horizontalHeader(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int column = m_table->horizontalHeader()->logicalIndexAt(pos);
        if (column < 0)
            return;
        QMenu* menu = columnMenu(column);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_table->horizontalHeader()->mapToGlobal(pos));
    });
    connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QModelIndex index = m_table->indexAt(pos);
        if (!index.isValid())
            index = m_table->currentIndex();
        if (!index.isValid())
            return;
        QMenu* menu = columnMenu(index.column());
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(m_table->viewport()->mapToGlobal(pos));
    });
    connect(m_opWatcher, &QFutureWatcher<OpOutcome>::finished, this, &SchemaEditorWindow::onOperationFinished);
    connect(m_endsWatcher, &QFutureWatcher<EndsOutcome>::finished, this, &SchemaEditorWindow::onEndsFinished);
    for (QWidget* w : {static_cast<QWidget*>(m_name), static_cast<QWidget*>(m_descriptionEdit),
                       static_cast<QWidget*>(m_template)})
        w->installEventFilter(this);

    // ---- Связи ---------------------------------------------------------------
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& text) {
        EditState next = m_state;
        next.name = text;
        pushEdit(NameField, next);
    });
    connect(m_descriptionEdit, &QLineEdit::textEdited, this, [this](const QString& text) {
        EditState next = m_state;
        next.description = text;
        pushEdit(DescriptionField, next);
    });
    connect(m_template, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_syncing)
            return;
        EditState next = m_state;
        next.templateText = m_template->toPlainText();
        pushEdit(TemplateField, next);
    });
    connect(m_undo, &QUndoStack::cleanChanged, this, &SchemaEditorWindow::updateChrome);
    connect(m_undo, &QUndoStack::indexChanged, this, &SchemaEditorWindow::updateChrome);
    connect(m_list, &QListWidget::currentRowChanged, this, &SchemaEditorWindow::onListRowChanged);
    connect(m_recompute, &QTimer::timeout, this, &SchemaEditorWindow::recompute);
    // Таблица пересчитана — прежнее выделение указывает на другие ячейки.
    connect(m_samples, &QAbstractItemModel::modelReset, this, [this]() { m_cellSelection = CellSelection(); });
    connect(m_samples, &SchemaSampleModel::computed, this, [this]() {
        m_table->resizeColumnsToContents();
        showMisses();
    });
    connect(m_fileJob, &SchemaFitJob::finished, this, [this]() {
        const auto& out = m_fileJob->outcome();
        m_fileFit = out.results.value(0);
        m_fileFitReady = true;
        m_fileSamples = out.samples;
        startEndOptions();
        m_samples->setMissLines(m_fileFit.misses);
        updateChrome();
    });
    connect(m_fileJob, &SchemaFitJob::invalidated, this, [this]() {
        m_fileFitReady = false;
        m_recompute->start();
    });
    connect(m_catalogueJob, &SchemaFitJob::finished, this, [this]() {
        m_catalogueFits = m_catalogueJob->outcome().results;
        rebuildList();
    });
    connect(m_catalogueJob, &SchemaFitJob::invalidated, this, &SchemaEditorWindow::startCatalogueFit);

    connect(emptyAction, &QAction::triggered, this, &SchemaEditorWindow::newEmpty);
    connect(samplesAction, &QAction::triggered, this, &SchemaEditorWindow::newFromSampleLines);
    connect(templateAction, &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("New Schema from Template"),
            tr("Line template, e.g. {Timestamp:time} [{Thread}] {Level:level} {Message}:"),
            QLineEdit::Normal, templateText(), &ok);
        if (ok && !text.trimmed().isEmpty())
            newFromTemplate(text.trimmed());
    });
    connect(grokAction, &QAction::triggered, this, [this]() {
        bool ok = false;
        const QString grok = QInputDialog::getMultiLineText(this, tr("Import Grok"),
            tr("Grok expression, e.g. %{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} %{GREEDYDATA:message}:"),
            QString(), &ok);
        if (!ok || grok.trimmed().isEmpty())
            return;
        QString error;
        if (!importGrok(grok.trimmed(), &error))
            warn(tr("Import Grok"), error);
    });
    connect(deleteAction, &QAction::triggered, this, [this]() {
        if (m_isNew || !m_schemas)
            return;
        if (ask(tr("Delete the schema “%1”? Its file is removed from the schemas folder.").arg(m_original.name),
                false) != Answer::Yes)
            return;
        SchemaStore::SchemaList list = m_schemas->schemas();
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].name == m_original.name) {
                list.removeAt(i);
                break;
            }
        }
        const QString error = m_schemas->replaceCatalogue(list, QString(), -1);
        if (!error.isEmpty())
            warn(tr("Field Schemas"), error);
        m_undo->clear();
        if (!list.isEmpty())
            loadEntry(list.first(), false);
        else
            newEmpty();
    });
    connect(importAction, &QAction::triggered, this, [this]() {
        if (!m_schemas || !resolveModified())
            return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Import Schema"), QString(),
                                                          tr("Schemas (*.json)"));
        if (path.isEmpty())
            return;
        QString error;
        const SchemaStore::SchemaEntry entry = SchemaStore::importFile(path, &error);
        if (entry.isEmpty()) {
            warn(tr("Import Schema"), error);
            return;
        }
        m_schemas->reloadCatalogue();
        openSchema(entry.name);
    });
    connect(exportAction, &QAction::triggered, this, [this]() {
        const QString def = definition();
        if (def.isEmpty())
            return;
        const QString path = QFileDialog::getSaveFileName(this, tr("Export Schema"),
            SchemaStore::suggestedFileName(schemaName()), tr("Schemas (*.json)"));
        if (path.isEmpty())
            return;
        SchemaStore::SchemaEntry entry = m_original;
        entry.name = schemaName();
        entry.schema = def;
        entry.setDescription(description());
        QString error;
        if (!SchemaStore::exportToFile(entry, path, &error))
            warn(tr("Export Schema"), error);
    });
    connect(folderAction, &QAction::triggered, this, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(SchemaStore::patternsDir()));
    });
    connect(m_findButton, &QPushButton::toggled, this, [this](bool on) {
        if (on && !m_find) {
            m_find = new SchemaCandidatesWindow(m_schemas, m_mainSplitter);
            m_find->setEmbedded(true);
            m_mainSplitter->addWidget(m_find);
            connect(m_find, &SchemaCandidatesWindow::settingsRequested, this,
                    &SchemaEditorWindow::settingsRequested);
            connect(m_find, &SchemaCandidatesWindow::editRequested, this,
                    [this](const QString& text, const QString& title) { newFromTemplate(text, title); });
        }
        if (m_find) {
            if (on)
                m_find->setView(m_view);
            m_find->setVisible(on);
        }
    });
    connect(m_apply, &QPushButton::clicked, this, &SchemaEditorWindow::applyToTab);
    connect(m_save, &QPushButton::clicked, this, &SchemaEditorWindow::save);
    connect(m_saveAs, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Schema As"), tr("Name:"),
                                                   QLineEdit::Normal, tr("%1 (copy)").arg(schemaName()), &ok);
        if (ok && !name.trimmed().isEmpty())
            saveAs(name.trimmed());
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    if (m_schemas)
        connect(m_schemas, &FieldSchemaController::catalogueChanged, this, [this]() {
            rebuildList();
            startCatalogueFit();
        });

    rebuildList();
    newEmpty();
    m_undo->clear();
}

SchemaEditorWindow::~SchemaEditorWindow()
{
    // Задания держат только свои копии (шаблон, строки, флаг), но пул
    // переживает окно — дождаться, чтобы не держать работу после закрытия.
    if (m_opCancel)
        m_opCancel->store(true, std::memory_order_relaxed);
    if (m_endsCancel)
        m_endsCancel->store(true, std::memory_order_relaxed);
    m_opWatcher->waitForFinished();
    m_endsWatcher->waitForFinished();
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------

QString SchemaEditorWindow::schemaName() const { return m_state.name.trimmed(); }
QString SchemaEditorWindow::description() const { return m_state.description.trimmed(); }
QString SchemaEditorWindow::templateText() const { return m_state.templateText; }

QString SchemaEditorWindow::definition() const
{
    const QString text = m_state.templateText.trimmed();
    if (text.isEmpty() || !LineTemplate(text).isValid())
        return QString();
    return LineTemplateSchema::definition(text);
}

QString SchemaEditorWindow::templateError() const
{
    const QString text = m_state.templateText.trimmed();
    if (text.isEmpty())
        return tr("The template is empty.");
    const LineTemplate tpl(text);
    return tpl.isValid() ? QString() : tpl.error().message;
}

bool SchemaEditorWindow::isModified() const
{
    // Новая схема из образцов, шаблона, Grok или Find a Schema — уже работа
    // пользователя; пустая новая — нет, пока её не правили.
    return m_state != stateOf(m_original) || (m_isNew && m_newIsWork);
}

QString SchemaEditorWindow::savedName() const
{
    return m_isNew ? QString() : m_original.name;
}

bool SchemaEditorWindow::isComputing() const
{
    return m_recompute->isActive() || m_samples->isComputing() || m_fileJob->isRunning()
        || m_catalogueJob->isRunning() || m_opPending || m_endsPending;
}

SchemaEditorWindow::EditState SchemaEditorWindow::stateOf(const SchemaStore::SchemaEntry& entry)
{
    return EditState{entry.name, entry.description(), templateOf(entry.schema)};
}

void SchemaEditorWindow::pushEdit(int field, const EditState& next)
{
    if (m_syncing || next == m_state)
        return;
    const EditState before = m_state;
    m_state = next;
    m_undo->push(new EditCommand(this, field, before, next));
    if (field == TemplateField || field == AllFields)
        onTemplateChanged();
    updateChrome();
}

void SchemaEditorWindow::setState(const EditState& state)
{
    const bool templateChanged = state.templateText != m_state.templateText;
    m_state = state;
    m_syncing = true;
    if (m_name->text() != state.name)
        m_name->setText(state.name);
    if (m_descriptionEdit->text() != state.description)
        m_descriptionEdit->setText(state.description);
    if (m_template->toPlainText() != state.templateText)
        m_template->setPlainText(state.templateText);
    m_syncing = false;
    if (templateChanged)
        onTemplateChanged();
    updateChrome();
}

void SchemaEditorWindow::setTemplateText(const QString& text)
{
    EditState next = m_state;
    next.templateText = text;
    pushEdit(TemplateField, next);
    if (m_template->toPlainText() != text) {
        m_syncing = true;
        m_template->setPlainText(text);
        m_syncing = false;
    }
}

void SchemaEditorWindow::setSchemaName(const QString& name)
{
    EditState next = m_state;
    next.name = name;
    pushEdit(NameField, next);
    if (m_name->text() != name)
        m_name->setText(name);
}

void SchemaEditorWindow::setDescription(const QString& text)
{
    EditState next = m_state;
    next.description = text;
    pushEdit(DescriptionField, next);
    if (m_descriptionEdit->text() != text)
        m_descriptionEdit->setText(text);
}

void SchemaEditorWindow::loadEntry(const SchemaStore::SchemaEntry& entry, bool isNew)
{
    m_original = entry;
    m_isNew = isNew;
    m_undo->clear();
    resetMakeField();
    m_state = EditState{QStringLiteral("\x01"), QString(), QStringLiteral("\x01")};   // всё — заново
    m_newIsWork = false;
    setState(stateOf(entry));
    rebuildList();
}

// ---------------------------------------------------------------------------
// Источник
// ---------------------------------------------------------------------------

void SchemaEditorWindow::setView(LogViewWidget* view)
{
    m_view = view;
    m_fileJob->setModel(view ? view->model() : nullptr);
    m_catalogueJob->setModel(view ? view->model() : nullptr);
    m_fileFitReady = false;
    m_fileFit = SchemaFit::Result();
    m_fileSamples.clear();
    m_catalogueFits.clear();
    refreshSamples();
    if (m_schemas && !isModified()) {
        // Схема вкладки — её пункт каталога, иначе — как есть, новой.
        const QString shown = m_schemas->shownSchema();
        bool found = false;
        for (const auto& e : m_schemas->schemas()) {
            if (!shown.isEmpty() && e.schema == shown) {
                loadEntry(e, false);
                found = true;
                break;
            }
        }
        if (!found && FieldLayout::isTemplateDefinition(shown)) {
            SchemaStore::SchemaEntry e;
            e.name = m_schemas->shownSettings().schemaName;
            e.schema = shown;
            loadEntry(e, true);
        }
    }
    if (m_find && m_find->isVisible())
        m_find->setView(view);
    startCatalogueFit();
    m_recompute->start();
}

void SchemaEditorWindow::refreshSamples()
{
    QVector<SchemaFit::Sample> lines;
    LogModel* model = m_view ? m_view->model() : nullptr;
    if (model) {
        const int total = model->rowCount();
        int row = m_view->view() ? m_view->view()->currentIndex().row() : -1;
        if (row < 0)
            row = 0;
        for (; row < total && lines.size() < kSampleLines; ++row) {
            const QString text = model->messageAt(row);
            if (text.trimmed().isEmpty())
                continue;
            lines.append(SchemaFit::Sample{model->keyAt(row).line, text});
        }
    }
    m_samples->setLines(lines);
}

// ---------------------------------------------------------------------------
// Пересчёт
// ---------------------------------------------------------------------------

void SchemaEditorWindow::onTemplateChanged()
{
    const QString text = m_state.templateText.trimmed();
    const LineTemplate tpl(text);
    auto* edit = static_cast<TemplateEdit*>(m_template);
    if (text.isEmpty() || tpl.isValid()) {
        edit->setErrorAt(-1);
        m_error->hide();
    } else {
        const LineTemplateError& e = tpl.error();
        edit->setErrorAt(e.position);
        m_error->setText(e.position >= 0 ? tr("Character %1: %2").arg(e.position + 1).arg(e.message) : e.message);
        m_error->show();
    }
    m_recompute->start();
}

void SchemaEditorWindow::recompute()
{
    const QString def = definition();
    FieldExtractorPtr extractor = def.isEmpty() ? nullptr : FieldLayout::extractorFor(def);
    m_samples->setExtractor(extractor);
    startFileFit();
}

void SchemaEditorWindow::startFileFit()
{
    m_fileFitReady = false;
    const QString def = definition();
    if (!m_view || !m_view->model() || def.isEmpty()) {
        m_fileJob->cancel();
        m_fileFit = SchemaFit::Result();
        m_fileSamples.clear();
        m_samples->setMissLines({});
        startEndOptions();
        updateChrome();
        return;
    }
    const LogFile* file = nullptr;
    LogModel* model = m_view->model();
    const int row = m_view->view() ? m_view->view()->currentIndex().row() : -1;
    if (row >= 0) {
        if (const auto key = model->keyAt(row); key.file)
            file = key.file.get();
    }
    m_fileJob->start({FieldLayout::extractorFor(def)}, file, m_fitBudget);
    updateChrome();
}

void SchemaEditorWindow::startCatalogueFit()
{
    if (!m_schemas || !m_view || !m_view->model())
        return;
    QVector<FieldExtractorPtr> extractors;
    for (const auto& e : m_schemas->schemas())
        extractors.append(FieldLayout::extractorFor(e.schema));
    m_catalogueJob->start(extractors, nullptr, m_fitBudget);
}

// ---------------------------------------------------------------------------
// Вид
// ---------------------------------------------------------------------------

void SchemaEditorWindow::rebuildList()
{
    if (!m_schemas)
        return;
    const QSignalBlocker block(m_list);
    m_list->clear();
    const auto& list = m_schemas->schemas();
    int current = -1;
    for (int i = 0; i < list.size(); ++i) {
        QString text = list[i].name;
        if (i < m_catalogueFits.size() && m_catalogueFits[i].valid && m_catalogueFits[i].candidates > 0)
            text += QStringLiteral("  ") + SchemaFit::percent(m_catalogueFits[i].coverage()) + QLatin1Char('%');
        auto* item = new QListWidgetItem(text, m_list);
        item->setData(Qt::UserRole, list[i].name);
        item->setToolTip(list[i].description());
        if (!m_isNew && list[i].name == m_original.name)
            current = i;
    }
    if (m_isNew) {
        auto* item = new QListWidgetItem(tr("%1 (not saved)").arg(m_state.name.isEmpty() ? tr("New schema")
                                                                                          : m_state.name), m_list);
        item->setData(Qt::UserRole, QString());
        QFont f = item->font();
        f.setItalic(true);
        item->setFont(f);
        current = m_list->count() - 1;
    }
    m_list->setCurrentRow(current);
}

void SchemaEditorWindow::onListRowChanged(int row)
{
    QListWidgetItem* item = m_list->item(row);
    if (!item)
        return;
    const QString name = item->data(Qt::UserRole).toString();
    if (name.isEmpty() || (!m_isNew && name == m_original.name))
        return;
    if (!openSchema(name))
        rebuildList();   // отменено — выбор назад
}

void SchemaEditorWindow::updateChrome()
{
    const QString def = definition();
    const bool valid = !def.isEmpty();
    m_apply->setEnabled(valid && m_view);
    m_save->setEnabled(valid && !schemaName().isEmpty() && (isModified() || m_isNew));
    m_saveAs->setEnabled(valid);
    updateEditActions();
    QString title = tr("Field Schemas");
    if (!schemaName().isEmpty())
        title = tr("%1 — %2").arg(schemaName(), title);
    if (isModified())
        title.prepend(QLatin1Char('*'));
    setWindowTitle(title);
    if (!m_view) {
        m_fileLabel->setText(tr("Open a log to check the schema on its lines."));
    } else if (!valid) {
        m_fileLabel->setText(QString());
    } else if (!m_fileFitReady) {
        m_fileLabel->setText(tr("In the file: checking…"));
    } else {
        m_fileLabel->setText(tr("In the file: %1").arg(SchemaFit::summary(m_fileFit)));
    }
}

void SchemaEditorWindow::showMisses()
{
    const auto& misses = m_samples->misses();
    m_missesLabel->setText(tr("Did not match (%1)").arg(misses.size()));
    m_misses->setRowCount(int(misses.size()));
    for (int i = 0; i < misses.size(); ++i) {
        const auto& m = misses[i];
        m_misses->setItem(i, 0, new QTableWidgetItem(m.line > 0 ? QString::number(m.line) : QString()));
        m_misses->setItem(i, 1, new QTableWidgetItem(m.reason));
        auto* text = new QTableWidgetItem(m.text);
        text->setToolTip(m.text);
        m_misses->setItem(i, 2, text);
    }
    m_misses->resizeColumnToContents(0);
}

// ---------------------------------------------------------------------------
// Действия
// ---------------------------------------------------------------------------

void SchemaEditorWindow::warn(const QString& title, const QString& text)
{
    m_lastWarning = text;
    if (m_asker) {
        m_asker(text, false);   // тест: без модального окна
        return;
    }
    QMessageBox::warning(this, title, text);
}

SchemaEditorWindow::Answer SchemaEditorWindow::ask(const QString& question, bool saveDiscard)
{
    if (m_asker)
        return m_asker(question, saveDiscard);
    if (saveDiscard) {
        const auto b = QMessageBox::question(this, tr("Field Schemas"), question,
                                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                             QMessageBox::Save);
        return b == QMessageBox::Save ? Answer::Save : b == QMessageBox::Discard ? Answer::Discard
                                                                                  : Answer::Cancel;
    }
    const auto b = QMessageBox::question(this, tr("Field Schemas"), question,
                                         QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                                         QMessageBox::Yes);
    return b == QMessageBox::Yes ? Answer::Yes : b == QMessageBox::No ? Answer::No : Answer::Cancel;
}

bool SchemaEditorWindow::resolveModified()
{
    if (!isModified())
        return true;
    const Answer a = ask(tr("The schema “%1” has unsaved changes. Save them?")
                             .arg(schemaName().isEmpty() ? tr("New schema") : schemaName()),
                         true);
    if (a == Answer::Cancel)
        return false;
    if (a == Answer::Save)
        return save();
    // Отброшено: назад к сохранённому (новая — к тому, с чем создана).
    m_newIsWork = false;
    m_undo->clear();
    resetMakeField();
    setState(stateOf(m_original));
    return true;
}

bool SchemaEditorWindow::openSchema(const QString& name)
{
    if (!m_schemas)
        return false;
    if (!m_isNew && name == m_original.name)
        return true;
    if (!resolveModified())
        return false;
    for (const auto& e : m_schemas->schemas()) {
        if (e.name == name) {
            loadEntry(e, false);
            return true;
        }
    }
    return false;
}

bool SchemaEditorWindow::newEmpty()
{
    if (!newFromTemplate(QStringLiteral("{Message}"), tr("New schema")))
        return false;
    m_newIsWork = false;
    updateChrome();
    return true;
}

bool SchemaEditorWindow::newFromSampleLines()
{
    const SchemaInference::Result inferred = SchemaInference::infer(m_samples->lines());
    if (!inferred.ok()) {
        warn(tr("New Schema from Sample Lines"),
                                 inferred.failureText.isEmpty() ? SchemaInference::failureText(inferred.failure)
                                                                : inferred.failureText);
        return false;
    }
    return newFromTemplate(inferred.variants.first().templateText, tr("Inferred schema"));
}

bool SchemaEditorWindow::newFromTemplate(const QString& text, const QString& name)
{
    if (!resolveModified())
        return false;
    SchemaStore::SchemaEntry e;
    e.name = name.isEmpty() ? tr("New schema") : name;
    e.schema = LineTemplateSchema::definition(text);
    loadEntry(e, true);
    m_newIsWork = true;
    updateChrome();
    return true;
}

bool SchemaEditorWindow::importGrok(const QString& grok, QString* error)
{
    const SchemaConverter::Result r = SchemaConverter::fromGrok(grok);
    if (!r.ok) {
        if (error)
            *error = r.error;
        return false;
    }
    if (!newFromTemplate(r.templateText, tr("Imported from Grok")))
        return false;
    if (!r.notes.isEmpty())
        setDescription(r.notes.join(QLatin1Char(' ')));
    return true;
}

bool SchemaEditorWindow::confirmRenames(const QString& oldDefinition, const QString& newDefinition)
{
    if (!m_uses.count || oldDefinition.isEmpty())
        return true;
    const QStringList before = columnsOf(templateOf(oldDefinition));
    const QStringList after = columnsOf(templateOf(newDefinition));
    if (before.size() != after.size())
        return true;
    for (int i = 0; i < before.size(); ++i) {
        if (before[i] == after[i] || after.contains(before[i]) || before.contains(after[i]))
            continue;
        const int uses = m_uses.count(before[i], oldDefinition);
        if (uses <= 0)
            continue;
        const Answer a = ask(tr("The field “%1” is now “%2”. %n filter rule(s) and profile rule(s) use "
                                "“%1”. Rename the field there too?", nullptr, uses)
                                 .arg(before[i], after[i]),
                             false);
        if (a == Answer::Cancel)
            return false;
        if (a == Answer::Yes && m_uses.rename)
            m_uses.rename(before[i], after[i], oldDefinition);
    }
    return true;
}

bool SchemaEditorWindow::applyToTab()
{
    const QString def = definition();
    if (def.isEmpty() || !m_view || !m_schemas)
        return false;
    if (!confirmRenames(m_schemas->shownSchema(), def))
        return false;
    // Тот же путь, что выбор в доке: схема вкладке (или файлу «Applies to»),
    // без перечитывания; пункт каталога — если правка сохранена.
    SchemaStore::SchemaList list = m_schemas->schemas();
    int index = -1;
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].schema == def && (m_isNew || list[i].name == m_original.name || index < 0))
            index = i;
    }
    const QString error = m_schemas->replaceCatalogue(list, def, index);
    if (!error.isEmpty())
        warn(tr("Field Schemas"), error);
    emit appliedToTab(m_view);
    return true;
}

bool SchemaEditorWindow::save()
{
    if (m_isNew)
        return saveAs(schemaName());
    const QString def = definition();
    if (def.isEmpty() || !m_schemas || schemaName().isEmpty())
        return false;
    SchemaStore::SchemaList list = m_schemas->schemas();
    int index = -1;
    for (int i = 0; i < list.size(); ++i) {
        if (list[i].name == m_original.name)
            index = i;
    }
    for (int i = 0; i < list.size(); ++i) {
        if (i != index && list[i].name == schemaName()) {
            warn(tr("Field Schemas"),
                                 tr("There is already a schema named “%1”.").arg(schemaName()));
            return false;
        }
    }
    if (!confirmRenames(m_original.schema, def))
        return false;
    SchemaStore::SchemaEntry entry = index >= 0 ? list[index] : m_original;
    entry.name = schemaName();
    entry.schema = def;
    entry.setDescription(description());
    // Вкладка с прежней версией схемы получает новую.
    const QString chosen = m_view && m_schemas->shownSchema() == m_original.schema ? def : QString();
    if (index >= 0)
        list[index] = entry;
    else
        list.append(entry);
    const QString error = m_schemas->replaceCatalogue(list, chosen, index >= 0 ? index : int(list.size()) - 1);
    if (!error.isEmpty()) {
        warn(tr("Field Schemas"),
                             tr("Could not save schemas to the patterns folder:\n%1").arg(error));
        return false;
    }
    for (const auto& e : m_schemas->schemas()) {
        if (e.name == entry.name)
            entry = e;
    }
    loadEntry(entry, false);
    emit schemaSaved(entry.name);
    return true;
}

bool SchemaEditorWindow::saveAs(const QString& name)
{
    const QString def = definition();
    const QString trimmed = name.trimmed();
    if (def.isEmpty() || !m_schemas || trimmed.isEmpty())
        return false;
    SchemaStore::SchemaList list = m_schemas->schemas();
    for (const auto& e : list) {
        if (e.name == trimmed) {
            warn(tr("Field Schemas"),
                                 tr("There is already a schema named “%1”.").arg(trimmed));
            return false;
        }
    }
    SchemaStore::SchemaEntry entry;
    entry.name = trimmed;
    entry.schema = def;
    entry.setDescription(description());
    list.append(entry);
    const QString error = m_schemas->replaceCatalogue(list, QString(), -1);
    if (!error.isEmpty()) {
        warn(tr("Field Schemas"),
                             tr("Could not save schemas to the patterns folder:\n%1").arg(error));
        return false;
    }
    for (const auto& e : m_schemas->schemas()) {
        if (e.name == trimmed)
            entry = e;
    }
    loadEntry(entry, false);
    emit schemaSaved(trimmed);
    return true;
}

// ---------------------------------------------------------------------------
// Правка на строке (F-17, шаг 10)
// ---------------------------------------------------------------------------

QVector<SchemaFit::Sample> SchemaEditorWindow::editSamples() const
{
    return m_fileSamples.isEmpty() ? m_samples->lines() : m_fileSamples;
}

int SchemaEditorWindow::currentColumn() const
{
    const QModelIndex index = m_table->currentIndex();
    return index.isValid() ? index.column() : -1;
}

void SchemaEditorWindow::setEditMessage(const QString& text)
{
    m_editText = text;
    m_editLabel->setText(text);
}

void SchemaEditorWindow::updateEditActions()
{
    if (!m_makeFieldButton)
        return;
    const bool valid = !definition().isEmpty();
    const bool busy = m_opPending;
    m_makeFieldButton->setEnabled(valid && !busy);
    m_makeFieldAction->setEnabled(valid && !busy);
    const bool other = !busy && variantOnTop();
    m_otherVariantButton->setEnabled(other);
    m_otherVariantAction->setEnabled(other);
    m_otherVariantButton->setText(other ? tr("Other Variant (%1/%2)").arg(m_variantIndex + 1).arg(m_make.variants.size())
                                        : tr("Other Variant"));
}

QString SchemaEditorWindow::askText(const QString& title, const QString& label, const QString& text,
                                    const QStringList& items)
{
    if (m_textAsker)
        return m_textAsker(title, label, text, items).trimmed();
    bool ok = false;
    QString value;
    if (items.isEmpty())
        value = QInputDialog::getText(this, title, label, QLineEdit::Normal, text, &ok);
    else
        value = QInputDialog::getItem(this, title, label, items, 0, /*editable=*/true, &ok);
    return ok ? value.trimmed() : QString();
}

void SchemaEditorWindow::startOperation(const QString& label, bool make, OpWork work)
{
    if (m_opCancel)
        m_opCancel->store(true, std::memory_order_relaxed);
    const int generation = ++m_opGeneration;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_opCancel = cancel;
    const QString base = m_state.templateText.trimmed();
    SchemaEdit::Options options;
    options.budget = m_fitBudget;
    options.budget.cancel = cancel.get();
    options.cancel = cancel.get();
    // Итог доставляется событием после конца задания: «идёт» — до доставки.
    m_opPending = true;
    m_opWatcher->setFuture(QtConcurrent::run([work, options, cancel, generation, base, label, make]() {
        OpOutcome out = work(options);
        out.generation = generation;
        out.base = base;
        out.label = label;
        out.make = make;
        return out;
    }));
    setEditMessage(tr("%1…").arg(label));
    updateEditActions();
}

void SchemaEditorWindow::onOperationFinished()
{
    const QFuture<OpOutcome> future = m_opWatcher->future();
    m_opPending = false;
    updateEditActions();
    if (future.resultCount() == 0)
        return;
    const OpOutcome out = future.result();
    if (out.generation != m_opGeneration)
        return;
    if (out.base != m_state.templateText.trimmed()) {
        setEditMessage(tr("The template changed meanwhile — %1 was not applied.").arg(out.label));
        return;
    }
    if (!out.make) {
        applyEdit(out.edit, out.label);
        return;
    }
    m_make = out.result;
    m_variantIndex = -1;
    m_variantUndoIndex = -1;
    if (!m_make.ok()) {
        setEditMessage(m_make.error);
        updateEditActions();
        return;
    }
    m_variantIndex = 0;
    applyOperation(m_make.variants.first().templateText, tr("Make Field"));
    m_variantUndoIndex = m_undo->index();
    m_variantSerial = m_lastOperationSerial;
    QString text = m_make.message;
    if (m_make.variants.size() > 1)
        text += QLatin1Char(' ') + tr("Other variants: %1 (Ctrl+Shift+M).").arg(m_make.variants.size() - 1);
    setEditMessage(text);
    updateEditActions();
}

void SchemaEditorWindow::applyOperation(const QString& text, const QString& label)
{
    EditState next = m_state;
    next.templateText = text;
    if (next == m_state)
        return;
    const EditState before = m_state;
    m_state = next;
    m_syncing = true;
    if (m_template->toPlainText() != text)
        m_template->setPlainText(text);
    m_syncing = false;
    auto* command = new EditCommand(this, AllFields, before, next);
    command->setText(label);
    m_lastOperationSerial = command->serial();
    m_undo->push(command);
    onTemplateChanged();
    updateChrome();
}

void SchemaEditorWindow::applyEdit(const SchemaEdit::Edit& edit, const QString& label)
{
    if (!edit.ok()) {
        setEditMessage(edit.error);
        return;
    }
    applyOperation(edit.templateText, label);
    setEditMessage(tr("%1: %2").arg(label, edit.templateText));
}

bool SchemaEditorWindow::makeField(int row, int column, int from, int length)
{
    if (definition().isEmpty()) {
        setEditMessage(tr("Fix the template first."));
        return false;
    }
    if (m_recompute->isActive() || m_samples->isComputing()) {
        setEditMessage(tr("The sample table is being updated — try again in a moment."));
        return false;
    }
    const int start = m_samples->cellStart(row, column);
    const QString raw = m_samples->rawCell(row, column);
    if (start < 0 || from < 0 || length <= 0 || from + length > raw.size()) {
        setEditMessage(tr("Select part of a field value first."));
        return false;
    }
    const QString line = m_samples->rowText(row);
    const QString tpl = m_state.templateText.trimmed();
    const QVector<SchemaFit::Sample> samples = editSamples();
    const qsizetype a = start + from;
    startOperation(tr("Make Field"), true, [tpl, line, a, length, samples](const SchemaEdit::Options& o) {
        OpOutcome out;
        out.result = SchemaEdit::makeField(tpl, line, a, length, samples, o);
        return out;
    });
    return true;
}

bool SchemaEditorWindow::makeFieldFromSelection()
{
    if (m_cellEditor && m_cellEditorIndex.isValid() && m_cellEditor->hasSelectedText()) {
        const int row = m_cellEditorIndex.row();
        const int column = m_cellEditorIndex.column();
        const int from = m_cellEditor->selectionStart();
        const int length = int(m_cellEditor->selectedText().size());
        return makeField(row, column, from, length);
    }
    if (m_cellSelection.length > 0)
        return makeField(m_cellSelection.row, m_cellSelection.column, m_cellSelection.from, m_cellSelection.length);
    setEditMessage(tr("Double-click a cell (or press F2), select part of its value and press Make Field "
                      "(Ctrl+M)."));
    return false;
}

bool SchemaEditorWindow::variantOnTop() const
{
    // Последний шаг отмены — та самая команда Make Field (не просто тот же
    // номер шага: после отмены и новой правки номер совпадёт).
    if (m_make.variants.size() < 2 || m_variantUndoIndex <= 0 || m_undo->index() != m_variantUndoIndex)
        return false;
    const auto* top = dynamic_cast<const EditCommand*>(m_undo->command(m_undo->index() - 1));
    return top && top->serial() == m_variantSerial;
}

void SchemaEditorWindow::resetMakeField()
{
    m_make = SchemaEdit::MakeFieldResult();
    m_variantIndex = -1;
    m_variantUndoIndex = -1;
    m_variantSerial = 0;
    m_cellSelection = CellSelection();
}

bool SchemaEditorWindow::otherVariant()
{
    if (!variantOnTop() || m_opPending)
        return false;
    // Тот же шаг отмены: прежний вариант снимается, следующий ставится.
    m_variantIndex = (m_variantIndex + 1) % int(m_make.variants.size());
    m_undo->undo();
    const SchemaEdit::Variant& v = m_make.variants[m_variantIndex];
    applyOperation(v.templateText, tr("Make Field"));
    m_variantUndoIndex = m_undo->index();
    m_variantSerial = m_lastOperationSerial;
    setEditMessage(tr("Variant %1 of %2: %3 ends at %4 (%5)%6")
                       .arg(m_variantIndex + 1)
                       .arg(m_make.variants.size())
                       .arg(v.field, SchemaEdit::evidenceText(v.evidence), v.summary(),
                            v.supported ? QString() : tr(" — no evidence in the lines")));
    updateEditActions();
    return true;
}

void SchemaEditorWindow::startEndOptions()
{
    if (m_endsCancel)
        m_endsCancel->store(true, std::memory_order_relaxed);
    const int generation = ++m_endsGeneration;
    const QString tpl = m_state.templateText.trimmed();
    if (definition().isEmpty()) {
        m_ends = EndsOutcome();
        m_endsPending = false;
        return;
    }
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_endsCancel = cancel;
    const QVector<SchemaFit::Sample> samples = editSamples();
    SchemaEdit::Options options;
    options.budget = m_fitBudget;
    options.budget.cancel = cancel.get();
    options.cancel = cancel.get();
    m_endsPending = true;
    m_endsWatcher->setFuture(QtConcurrent::run([tpl, samples, options, cancel, generation]() {
        EndsOutcome out;
        out.generation = generation;
        out.base = tpl;
        const LineTemplate t(tpl);
        const int columns = int(t.columns().size());
        out.options.resize(columns);
        out.suggestions.resize(columns);
        const QVector<SchemaEdit::Piece> pieces = SchemaEdit::piecesOf(t);
        int column = 0;
        for (const auto& p : pieces) {
            if (!p.isField() || p.name.isEmpty())
                continue;
            if (cancel->load(std::memory_order_relaxed))
                break;
            out.options[column] = SchemaEdit::endOptions(tpl, column, samples, options);
            if (p.kind == LineTemplateElement::Kind::Of)
                out.suggestions[column] = SchemaEdit::suggestValues(tpl, column, samples, options);
            ++column;
        }
        return out;
    }));
}

void SchemaEditorWindow::onEndsFinished()
{
    const QFuture<EndsOutcome> future = m_endsWatcher->future();
    m_endsPending = false;
    if (future.resultCount() == 0)
        return;
    EndsOutcome out = future.result();
    if (out.generation != m_endsGeneration)
        return;
    m_ends = std::move(out);
}

bool SchemaEditorWindow::endOptionsReady() const
{
    return !m_endsPending && !m_recompute->isActive() && !m_ends.base.isEmpty()
        && m_ends.base == m_state.templateText.trimmed();
}

QVector<SchemaEdit::EndOption> SchemaEditorWindow::endOptions(int column) const
{
    return endOptionsReady() ? m_ends.options.value(column) : QVector<SchemaEdit::EndOption>();
}

QStringList SchemaEditorWindow::suggestedValues(int column) const
{
    return endOptionsReady() ? m_ends.suggestions.value(column) : QStringList();
}

bool SchemaEditorWindow::setFieldEnd(int column, int option)
{
    const QVector<SchemaEdit::EndOption> options = endOptions(column);
    if (option < 0 || option >= options.size() || !options[option].available)
        return false;
    applyOperation(options[option].templateText, tr("Ends at"));
    setEditMessage(tr("Ends %1: %2").arg(options[option].label, options[option].summary()));
    return true;
}

bool SchemaEditorWindow::renameField(int column, const QString& name)
{
    const SchemaEdit::Edit e = SchemaEdit::rename(m_state.templateText, column, name);
    applyEdit(e, tr("Rename Field"));
    return e.ok();
}

bool SchemaEditorWindow::hideField(int column)
{
    const SchemaEdit::Edit e = SchemaEdit::hide(m_state.templateText, column);
    applyEdit(e, tr("Hide Field"));
    return e.ok();
}

bool SchemaEditorWindow::addValue(int column, const QString& value)
{
    const SchemaEdit::Edit e = SchemaEdit::addValue(m_state.templateText, column, value);
    applyEdit(e, tr("Add Value"));
    return e.ok();
}

bool SchemaEditorWindow::removeField(int column)
{
    if (column < 0 || definition().isEmpty())
        return false;
    const QString tpl = m_state.templateText.trimmed();
    const QVector<SchemaFit::Sample> samples = editSamples();
    startOperation(tr("Remove Field"), false, [tpl, column, samples](const SchemaEdit::Options& o) {
        OpOutcome out;
        out.edit = SchemaEdit::remove(tpl, column, samples, o);
        return out;
    });
    return true;
}

bool SchemaEditorWindow::mergeWithNext(int column)
{
    if (column < 0 || definition().isEmpty())
        return false;
    const QString tpl = m_state.templateText.trimmed();
    const QVector<SchemaFit::Sample> samples = editSamples();
    startOperation(tr("Merge with Next"), false, [tpl, column, samples](const SchemaEdit::Options& o) {
        OpOutcome out;
        out.edit = SchemaEdit::mergeWithNext(tpl, column, samples, o);
        return out;
    });
    return true;
}

QMenu* SchemaEditorWindow::columnMenu(int column)
{
    auto* menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("columnMenu"));
    const QString tpl = m_state.templateText.trimmed();
    const LineTemplate t(tpl);
    if (!t.isValid() || column < 0 || column >= t.columns().size()) {
        menu->addAction(tr("Fix the template first"))->setEnabled(false);
        return menu;
    }
    const QString name = t.columns()[column];
    const QVector<SchemaEdit::Piece> pieces = SchemaEdit::piecesOf(t);
    SchemaEdit::Piece field;
    for (int c = 0, i = 0; i < pieces.size(); ++i) {
        if (pieces[i].isField() && !pieces[i].name.isEmpty() && c++ == column)
            field = pieces[i];
    }
    menu->setTitle(name);
    QMenu* ends = menu->addMenu(tr("Ends at"));
    ends->setObjectName(QStringLiteral("endsAtMenu"));
    // Пункты — из итога, посчитанного после правки шаблона; меню, открытое
    // до конца расчёта, наполняется при показе.
    const auto fill = [this, ends, column]() {
        ends->clear();
        const QVector<SchemaEdit::EndOption> options = endOptions(column);
        if (options.isEmpty()) {
            ends->addAction(tr("Computing on the sample lines…"))->setEnabled(false);
            return;
        }
        for (int i = 0; i < options.size(); ++i) {
            const auto& o = options[i];
            QAction* a = ends->addAction(o.available ? tr("%1 — %2").arg(o.label, o.summary())
                                                     : tr("%1 (%2)").arg(o.label, o.reason));
            a->setObjectName(QStringLiteral("endsAt%1").arg(i));
            a->setCheckable(true);
            a->setChecked(o.current);
            a->setEnabled(o.available && !o.current);
            connect(a, &QAction::triggered, this, [this, column, i]() { setFieldEnd(column, i); });
        }
    };
    fill();
    connect(ends, &QMenu::aboutToShow, this, fill);
    connect(m_endsWatcher, &QFutureWatcher<EndsOutcome>::finished, ends, [fill, ends]() {
        if (ends->isVisible())
            fill();
    });
    menu->addSeparator();
    // Сочетания — у действий окна; в меню только подписаны (иначе то же
    // сочетание было бы у двух действий).
    const auto keys = [this](const char* name) {
        const QAction* a = findChild<QAction*>(QLatin1String(name));
        return a ? QLatin1Char('\t') + a->shortcut().toString(QKeySequence::NativeText) : QString();
    };
    QAction* make = menu->addAction(tr("Make Field from Selection") + keys("makeFieldAction"));
    make->setObjectName(QStringLiteral("makeFieldMenuAction"));
    make->setEnabled((m_cellEditor && m_cellEditor->hasSelectedText()) || m_cellSelection.length > 0);
    connect(make, &QAction::triggered, this, &SchemaEditorWindow::makeFieldFromSelection);
    QAction* rename = menu->addAction(tr("Rename…") + keys("renameFieldShortcut"));
    rename->setObjectName(QStringLiteral("renameFieldAction"));
    connect(rename, &QAction::triggered, this, [this, column, name]() {
        const QString n = askText(tr("Rename Field"), tr("Name:"), name);
        if (!n.isEmpty())
            renameField(column, n);
    });
    QAction* addValueAction = menu->addAction(tr("Add Value…") + keys("addValueShortcut"));
    addValueAction->setObjectName(QStringLiteral("addValueAction"));
    addValueAction->setEnabled(field.kind == LineTemplateElement::Kind::Of);
    connect(addValueAction, &QAction::triggered, this, [this, column]() {
        const QStringList items = suggestedValues(column);
        const QString value = askText(tr("Add Value"), tr("Value:"), items.value(0), items);
        if (!value.isEmpty())
            addValue(column, value);
    });
    QAction* merge = menu->addAction(tr("Merge with Next") + keys("mergeFieldShortcut"));
    merge->setObjectName(QStringLiteral("mergeFieldAction"));
    connect(merge, &QAction::triggered, this, [this, column]() { mergeWithNext(column); });
    QAction* hide = menu->addAction(tr("Hide") + keys("hideFieldShortcut"));
    hide->setObjectName(QStringLiteral("hideFieldAction"));
    hide->setToolTip(tr("Keep parsing this part of the line, but not as a column"));
    connect(hide, &QAction::triggered, this, [this, column]() { hideField(column); });
    QAction* remove = menu->addAction(tr("Remove") + keys("removeFieldShortcut"));
    remove->setObjectName(QStringLiteral("removeFieldAction"));
    remove->setToolTip(tr("The text of the field goes to the field before it"));
    connect(remove, &QAction::triggered, this, [this, column]() { removeField(column); });
    return menu;
}

// ---------------------------------------------------------------------------
// События
// ---------------------------------------------------------------------------

void SchemaEditorWindow::closeEvent(QCloseEvent* event)
{
    if (!resolveModified()) {
        event->ignore();
        return;
    }
    m_fileJob->cancel();
    m_catalogueJob->cancel();
    event->accept();
}

bool SchemaEditorWindow::eventFilter(QObject* watched, QEvent* event)
{
    // Ctrl+Z / Ctrl+Shift+Z в полях — QUndoStack окна, а не собственная отмена
    // поля (у шаблона её нет вовсе).
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        const bool undo = key->matches(QKeySequence::Undo);
        const bool redo = key->matches(QKeySequence::Redo);
        if (undo || redo) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (undo)
                m_undo->undo();
            else
                m_undo->redo();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
