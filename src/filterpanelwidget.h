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
    // Фокус в поле текста правила (новая карточка по «+ Add rule»).
    void focusText();
    // Подсказка в пустом поле текста (без регекса): у фильтра и поиска своя.
    void setTextPlaceholder(const QString& text);

signals:
    void removeRequested();
    void applyShortcutPressed(); // Enter в поле текста

private:
    void chooseColor();
    void toggleHighlightEnabled();  // правый клик по образцу цвета
    void updateColorButton();
    void updateGearHighlight();
    // Почему правило не работает или работает не так, как показано:
    // неверный регекс, колонки нет в схеме, Log Fields выключены.
    void updateRegexValidity();

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
    QLabel*      m_regexErrorLabel;   // пояснение под текстом (обычно скрыто)
    QStringList  m_schemaFields;      // колонки текущей схемы (setFieldNames)
    bool         m_schemaKnown = false;
    bool         m_fieldScopeEnabled = true;
    QString      m_textPlaceholder;   // подсказка поля текста без регекса

    QColor       m_color;
    bool         m_highlightEnabled = true;
};

// ============================================================================
// FilterPanelWidget — редактор набора текстовых правил с профилями.
//
// Инкапсулирует весь динамический список правил; наружу отдаёт только
// FilterRuleSet и сигналы applyRequested()/resetRequested(). MainWindow
// не знает о внутренних layout'ах и контролах.
//
// Две панели на одном редакторе — роль задаётся при создании и не меняется
// (прежний переключатель «Non-destructive search» внутри одной панели
// новичку был неочевиден):
//   Filter — док Text Filters: Apply скрывает несовпавшие строки активного
//            документа, Reset снимает с него фильтр;
//   Search — запрос панели Search (SearchPanelWidget): Search находит
//            строки, лог остаётся полным, совпадения идут в список
//            результатов рядом; Clear очищает выдачу.
// Правила в обоих случаях остаются в панели — их можно применить снова.
// У каждой роли свои профили, подписи и связь новых правил по умолчанию.
// ============================================================================

class FilterPanelWidget : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Filter, Search };

    explicit FilterPanelWidget(Mode mode, QWidget* parent = nullptr);

    // Собрать набор правил из текущего состояния UI.
    FilterRuleSet ruleSet() const;
    // Перестроить UI из набора (загрузка настроек).
    void setRuleSet(const FilterRuleSet& set);

    // Обновить список колонок в комбобоксах правил. fieldScopeEnabled = false
    // (галочка "Filter blocks" снята) блокирует выбор колонки.
    void setFieldNames(const QStringList& fieldNames, bool fieldScopeEnabled);

    // ---- Роль и подсветка ---------------------------------------------------
    Mode mode() const { return m_mode; }
    // Подсвечивать ли совпадения в ОСНОВНОМ view (у поиска список
    // результатов подсвечивает их всегда).
    bool highlightInMainView() const;
    void setHighlightInMainView(bool on); // тихо, для восстановления настроек

    // Фокус в поле текста первого правила (Ctrl+Shift+F без текста).
    void focusFirstRule();
    // Запрос из одного правила «Contains text» вместо всех карточек —
    // «Find All» из тулбара и контекстного меню.
    void setSingleRule(const QString& text, bool caseSensitive);

    // ---- Профили фильтрации -------------------------------------------------
    // Именованные конфигурации правил. Сериализуются целиком (все профили +
    // активный). Всегда есть ≥1 профиль. Загрузка тихая (без диалогов).
    QJsonObject profilesToJson() const;
    void profilesFromJson(const QJsonObject& json);

    // Правило из контекстного меню лога («показать/скрыть строки с текстом»):
    // quickRule строит его (цвет — следующий свободный, связь — AND, в поиске
    // для «показать» — OR), addQuickRule добавляет в панель (одинокая пустая
    // стартовая карточка заменяется). Применяет его окно — как по Apply.
    FilterRule quickRule(const QString& text, bool exclude) const;
    void addQuickRule(const QString& text, bool exclude);

signals:
    // Пользователь нажал Apply (или Enter в поле правила).
    void applyRequested();
    // Пользователь нажал Reset (фильтр) / Clear (поиск).
    void resetRequested();
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
    Mode          m_mode = Mode::Filter;
    QCheckBox*    m_highlightMainCheckBox = nullptr;
    CardListArea* m_rulesArea = nullptr;
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
