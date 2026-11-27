#ifndef SCHEMAFIELDCARD_H
#define SCHEMAFIELDCARD_H

#include "schemaedit.h"

#include <QPair>
#include <QVector>
#include <QWidget>

class CardFrame;
class QButtonGroup;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

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
//   • Hide, Merge with Next, Remove, Add Value… (только у списка значений).
//
// Поле не выбрано — подсказка, что делать. Выбрана часть строки без колонки
// («{_}») — предложение показать её колонкой (showRequested).
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

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void commitName();
    void applyAccent();
    void updateActions();
    void clearOptions();

    Field m_field;
    bool m_busy = false;
    bool m_optionsReady = false;

    QStackedWidget* m_stack = nullptr;
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
};

#endif // SCHEMAFIELDCARD_H
