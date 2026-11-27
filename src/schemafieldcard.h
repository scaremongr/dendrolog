#ifndef SCHEMAFIELDCARD_H
#define SCHEMAFIELDCARD_H

#include "schemaedit.h"

#include <QPair>
#include <QVector>
#include <QWidget>

class CardFrame;
class QButtonGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QToolButton;

// ============================================================================
// SchemaFieldCard — карточка выбранного поля редактора схем (U-28): всё, что
// можно сделать с полем, — на виду и словами, без синтаксиса шаблона.
//
//   • Name — имя колонки (Enter или уход из поля — renameRequested);
//   • Where it ends — доступные виды конца поля переключателями, у каждого —
//     итог на строках лога («3 values · 100%»): сколько разных значений
//     получится и какая доля строк разберётся. Это те же пункты, что «Ends at»
//     меню колонки (SchemaEdit::endOptions), недоступные не показаны;
//   • Values — сколько разных значений у поля в образцах и самые частые: по
//     ним видно, верно ли стоит граница;
//   • «or at the text» — свой разделитель («~~»): поле кончается на нём, то,
//     что за ним, становится новым полем (textEndRequested); под строкой —
//     в скольких значениях образцов этот текст есть;
//   • Split…, Merge with Next, Hide, Remove, Add Value… (только у списка
//     значений).
//
// Поле не выбрано — подсказка, что делать. Выбрана часть строки без колонки
// («{_}») — предложение показать её колонкой (showRequested).
//
// Ещё две страницы (U-28, создание и правка блоков):
//   • «Split … into two fields» (showSplit) — так в схеме появляется новое
//     поле: имена двух частей (⇄ меняет их местами), где кончается первая
//     (пункты SchemaEdit::splitOptions с итогом и свой разделитель), пример
//     «первая ▏ вторая» на строках лога, Split / Cancel;
//   • «Text between the fields» (showLiteral) — текст между блоками можно
//     заменить или убрать.
//
// Только вид: операции выполняет окно (SchemaEditorWindow). Цвет акцента —
// цвет поля (SchemaFieldStrip::fieldColor), берётся из палитры и
// пересчитывается на PaletteChange.
// ============================================================================
class SchemaFieldCard : public QWidget {
    Q_OBJECT
public:
    struct Field {
        int column = -1;        ///< колонка схемы; -1 — нет
        int piece = -1;         ///< элемент шаблона (поле без колонки)
        QString name;
        QString caption;        ///< где кончается, словами
        QString templatePart;   ///< как записано в шаблоне: «{Level:level}»
        bool isList = false;    ///< вид of: Add Value…
        bool canMerge = false;  ///< за полем есть ещё поле
        bool isValid() const { return column >= 0 || piece >= 0; }
        bool isHidden() const { return column < 0 && piece >= 0; }
    };

    explicit SchemaFieldCard(QWidget* parent = nullptr);

    void setField(const Field& field);
    const Field& field() const { return m_field; }
    /// Подсказка, когда поле не выбрано.
    void setHint(const QString& text);
    /// Пункты «где кончается»; ready = false — считаются (прежние недоступны).
    void setEndOptions(const QVector<SchemaEdit::EndOption>& options, bool ready);
    /// Значения поля в образцах; ready = false — считаются.
    void setValues(int distinct, const QVector<QPair<QString, int>>& top, bool ready);
    /// Идёт операция — действия недоступны.
    void setBusy(bool busy);

    // ---- «or at the text» страницы поля ----------------------------------------
    /// Разделители, найденные в значениях поля, — подсказкой в списке.
    void setTextEndSuggestions(const QStringList& delimiters);
    /// Что даст набранный текст, словами (в скольких значениях он есть).
    void setTextEndHint(const QString& text);

    // ---- Разбить поле на два -----------------------------------------------------
    /// Страница разбиения поля field; newName — имя новой части по умолчанию.
    void showSplit(const Field& field, const QString& newName);
    bool isSplitShown() const;
    /// Пункты «где кончается первая часть»; ready = false — считаются.
    void setSplitOptions(const QVector<SchemaEdit::SplitOption>& options, bool ready);
    void setSplitDelimiters(const QStringList& delimiters);
    /// Итог своего разделителя (набран в поле страницы); ready = false — считается.
    void setSplitTextOption(const SchemaEdit::SplitOption& option, bool ready);

    // ---- Текст между полями --------------------------------------------------------
    /// between — «между Level и Message», словами.
    void showLiteral(int piece, const QString& text, const QString& between);
    bool isLiteralShown() const;

    /// Итог пункта без синтаксиса: «3 values · 100%».
    static QString resultText(const SchemaEdit::EndOption& option);

signals:
    void renameRequested(int column, const QString& name);
    void endRequested(int column, int option);
    void hideRequested(int column);
    void showRequested(int piece);
    void mergeRequested(int column);
    void removeRequested(int column);
    void addValueRequested(int column);
    void textEndEdited(int column, const QString& text);
    void textEndRequested(int column, const QString& text);
    void splitRequested(int column);
    void splitTextEdited(const QString& text);
    void splitConfirmed(const SchemaEdit::SplitOption& option, const QString& first, const QString& second);
    void splitCancelled();
    void literalChangeRequested(int piece, const QString& text);
    /// Содержимое изменило нужную ему ширину (пункт длиннее, появилась
    /// подсказка) — панель окна подстраивает свой минимум.
    void neededWidthChanged();

protected:
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void commitName();
    void applyAccent();
    void updateActions();
    void clearOptions();
    void clearSplitOptions();
    const SchemaEdit::SplitOption* selectedSplit() const;
    void updateSplitSelection();
    void requestTextEnd();
    void showPage(QWidget* page);

    Field m_field;
    bool m_busy = false;
    bool m_optionsReady = false;

    QStackedWidget* m_stack = nullptr;
    QVector<QSizePolicy> m_pagePolicies;   ///< политики страниц: скрытые не задают высоту карточки
    QLabel* m_hint = nullptr;
    CardFrame* m_card = nullptr;
    QLineEdit* m_name = nullptr;
    QLabel* m_endsTitle = nullptr;
    QLabel* m_endsStatus = nullptr;
    QWidget* m_endsBox = nullptr;
    QGridLayout* m_endsLayout = nullptr;
    QButtonGroup* m_endsGroup = nullptr;
    QLabel* m_valuesTitle = nullptr;
    QLabel* m_values = nullptr;
    QPushButton* m_hide = nullptr;
    QPushButton* m_merge = nullptr;
    QPushButton* m_remove = nullptr;
    QPushButton* m_addValue = nullptr;
    CardFrame* m_hiddenCard = nullptr;
    QLabel* m_hiddenText = nullptr;
    QPushButton* m_show = nullptr;

    // «or at the text» и Split… страницы поля.
    QComboBox* m_textEnd = nullptr;
    QPushButton* m_textEndApply = nullptr;
    QLabel* m_textEndHint = nullptr;
    QPushButton* m_split = nullptr;

    // Страница разбиения.
    static constexpr int kTextOptionId = 1000;
    CardFrame* m_splitCard = nullptr;
    QLabel* m_splitTitle = nullptr;
    QLineEdit* m_splitFirst = nullptr;
    QLineEdit* m_splitSecond = nullptr;
    QToolButton* m_splitSwap = nullptr;
    QLabel* m_splitStatus = nullptr;
    QWidget* m_splitBox = nullptr;
    QGridLayout* m_splitLayout = nullptr;
    QButtonGroup* m_splitGroup = nullptr;
    QRadioButton* m_splitTextRadio = nullptr;
    QComboBox* m_splitText = nullptr;
    QLabel* m_splitTextResult = nullptr;
    QLabel* m_splitPreview = nullptr;
    QPushButton* m_splitApply = nullptr;
    QPushButton* m_splitCancel = nullptr;
    QVector<SchemaEdit::SplitOption> m_splitOptions;
    SchemaEdit::SplitOption m_splitTextOption;
    bool m_splitTextReady = false;
    bool m_splitReady = false;
    bool m_splitNamesTouched = false;
    QString m_splitNewName;
    Field m_splitField;

    // Страница текста между полями.
    CardFrame* m_literalCard = nullptr;
    QLabel* m_literalInfo = nullptr;
    QLineEdit* m_literalEdit = nullptr;
    QPushButton* m_literalApply = nullptr;
    QPushButton* m_literalRemove = nullptr;
    int m_literalPiece = -1;
};

#endif // SCHEMAFIELDCARD_H
