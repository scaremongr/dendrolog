#ifndef FILTERPANELWIDGET_H
#define FILTERPANELWIDGET_H

#include "cardframe.h"
#include "filterruleset.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class ToggleSwitch;

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
    void toggleHighlightEnabled();  // правый клик по образцу цвета
    void updateColorButton();
    void updateGearHighlight();
    void updateRegexValidity();     // подсветка ошибки в тексте регекса

    QComboBox*   m_connectorCombo;
    QCheckBox*   m_enabledCheckBox;
    QComboBox*   m_actionCombo;
    QComboBox*   m_fieldCombo;
    QLineEdit*   m_textEdit;
    QToolButton* m_gearButton;
    // Единая кнопка-образец цвета: левый клик = выбрать цвет, правый клик =
    // вкл/выкл подсветку правила. Залита цветом (вкл) или полый контур (выкл).
    QToolButton* m_colorButton;
    QToolButton* m_removeButton;

    QWidget*     m_advancedRow;
    QCheckBox*   m_caseSensitiveCheckBox;
    QCheckBox*   m_regexCheckBox;
    QLabel*      m_regexErrorLabel;   // виден только при неверном регексе

    QColor       m_color;
    bool         m_highlightEnabled = true;
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
    // Режим работы панели текстовых фильтров:
    //   Filter — Apply скрывает несовпавшие строки в основном view (разрушающий);
    //   Search — Apply оставляет основной view полным, а совпадения выводит
    //            списком в нижней панели результатов (неразрушающий, klogg-style).
    enum class Mode { Filter, Search };

    explicit FilterPanelWidget(QWidget* parent = nullptr);

    // Собрать набор правил из текущего состояния UI.
    FilterRuleSet ruleSet() const;
    // Перестроить UI из набора (загрузка настроек).
    void setRuleSet(const FilterRuleSet& set);

    // Обновить список колонок в комбобоксах правил. fieldScopeEnabled = false
    // (галочка "Filter blocks" снята) блокирует выбор колонки.
    void setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled);

    // ---- Режим работы -------------------------------------------------------
    Mode mode() const;
    void setMode(Mode mode);      // тихая установка (для восстановления настроек)
    // Подсвечивать ли совпадения в ОСНОВНОМ view в режиме Search
    // (в панели результатов совпадения подсвечиваются всегда).
    bool highlightInMainView() const;
    void setHighlightInMainView(bool on);

    // ---- Профили фильтрации -------------------------------------------------
    // Именованные конфигурации правил. Сериализуются целиком (все профили +
    // активный). Всегда есть ≥1 профиль. Загрузка тихая (без диалогов).
    QJsonObject profilesToJson() const;
    void profilesFromJson(const QJsonObject& json);

signals:
    // Пользователь нажал Apply (или Enter в поле правила).
    void applyRequested();
    // Пользователь нажал Reset — снять фильтры с активного документа.
    void resetRequested();
    // Пользователь сменил режим работы панели.
    void modeChanged(Mode mode);
    // Пользователь переключил галочку подсветки в основном view.
    void highlightInMainViewChanged(bool on);
    // Список/содержимое профилей изменились (save/new/rename/delete/switch).
    void profilesChanged();

private:
    void addRule(const FilterRule& rule);
    void onAddRuleClicked();
    void removeCard(FilterRuleCard* card);
    void renumberRows();      // актуализирует видимость коннектора первой карточки
    QColor nextFreeColor() const;
    void updateModeDependentUi(); // тексты кнопок под текущий режим

    // ---- Профили ------------------------------------------------------------
    struct Profile {
        QString       name;
        FilterRuleSet ruleSet;
    };
    void buildProfileMenu();          // наполнить меню кнопки «⋯»
    void refreshProfileCombo();       // синхронизировать комбо со списком/активным
    bool currentRulesAreDirty() const;// правила в карточках ≠ сохранённому профилю
    void onProfileSelected(int index);// смена активного профиля (с dirty-prompt)
    void saveActiveProfile();         // зафиксировать правки карточек в профиль
    void saveAsNewProfile();          // создать новый профиль из текущих правил
    void renameActiveProfile();
    void deleteActiveProfile();
    QString uniqueProfileName(const QString& base, int skipIndex = -1) const;
    void ensureAtLeastOneProfile();   // гарантировать наличие «Default»

    CardFrame*    m_settingsCard = nullptr;
    QComboBox*    m_profileCombo = nullptr;
    QToolButton*  m_profileMenuButton = nullptr;
    ToggleSwitch* m_modeSwitch = nullptr;         // off = Filter, on = Search
    QCheckBox*    m_highlightMainCheckBox = nullptr;
    QVBoxLayout* m_rulesLayout;
    QVector<FilterRuleCard*> m_cards;
    QToolButton* m_addButton;
    QToolButton* m_applyButton;
    QToolButton* m_resetButton;

    QVector<Profile> m_profiles;
    int m_activeProfileIndex = 0;

    QStringList m_fieldNames;
    bool m_fieldScopeEnabled = false;
};

#endif // FILTERPANELWIDGET_H
