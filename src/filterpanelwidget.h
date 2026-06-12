#ifndef FILTERPANELWIDGET_H
#define FILTERPANELWIDGET_H

#include "cardframe.h"
#include "filterruleset.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// ============================================================================
// FilterRuleCard — одно правило фильтра в виде карточки (CardFrame).
//
// Полоска-акцент слева показывает цвет подсветки совпадений правила.
// Layout всех карточек одинаковый (у первой AND/OR засерен):
//
//   ▌ ☑ [Contains ▾] [AND/OR] [колонка ▾]        ⚙  ▦  ✕
//   ▌ [текст правила..............................]
//   ▌   ☐ Case sensitive  ☐ Regular expression       (⚙ строка)
//
// Виджеты карточки — единственный источник правды о её состоянии;
// rule() собирает FilterRule из текущих значений контролов.
// ============================================================================

class FilterRuleCard : public CardFrame {
    Q_OBJECT
public:
    explicit FilterRuleCard(const FilterRule& rule, QWidget* parent = nullptr);

    FilterRule rule() const;
    void setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled);
    // Первая карточка не имеет связи с предыдущей — коннектор скрыт.
    void setIsFirstRow(bool first);

signals:
    void removeRequested();
    void applyShortcutPressed(); // Enter в поле текста

private:
    void chooseColor();
    void updateColorButton();
    void updateGearHighlight();

    QComboBox*   m_connectorCombo;
    QCheckBox*   m_enabledCheckBox;
    QComboBox*   m_actionCombo;
    QComboBox*   m_fieldCombo;
    QLineEdit*   m_textEdit;
    QToolButton* m_gearButton;
    QToolButton* m_colorButton;
    QToolButton* m_removeButton;

    QWidget*     m_advancedRow;
    QCheckBox*   m_caseSensitiveCheckBox;
    QCheckBox*   m_regexCheckBox;

    QColor       m_color;
};

// ============================================================================
// FilterPanelWidget — конструктор текстовых фильтров (содержимое дока).
//
// Инкапсулирует весь динамический список правил; наружу отдаёт только
// FilterRuleSet и сигналы applyRequested()/resetRequested(). MainWindow
// не знает о внутренних layout'ах и контролах.
//
// Фильтры применяются к АКТИВНОМУ документу по Apply; Reset снимает
// фильтры с активного документа, не очищая сами правила в панели.
// ============================================================================

class FilterPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit FilterPanelWidget(QWidget* parent = nullptr);

    // Собрать набор правил из текущего состояния UI.
    FilterRuleSet ruleSet() const;
    // Перестроить UI из набора (загрузка настроек).
    void setRuleSet(const FilterRuleSet& set);

    // Обновить список колонок в комбобоксах правил. fieldScopeEnabled = false
    // (галочка "Filter blocks" снята) блокирует выбор колонки.
    void setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled);

signals:
    // Пользователь нажал Apply (или Enter в поле правила).
    void applyRequested();
    // Пользователь нажал Reset — снять фильтры с активного документа.
    void resetRequested();

private:
    void addRule(const FilterRule& rule);
    void onAddRuleClicked();
    void removeCard(FilterRuleCard* card);
    void renumberRows();      // актуализирует видимость коннектора первой карточки
    QColor nextFreeColor() const;

    QVBoxLayout* m_rulesLayout;
    QVector<FilterRuleCard*> m_cards;
    QPushButton* m_addButton;
    QPushButton* m_applyButton;
    QPushButton* m_resetButton;

    QStringList m_fieldNames;
    bool m_fieldScopeEnabled = false;
};

#endif // FILTERPANELWIDGET_H
