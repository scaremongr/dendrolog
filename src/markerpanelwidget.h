#ifndef MARKERPANELWIDGET_H
#define MARKERPANELWIDGET_H

#include "cardframe.h"
#include "textmatchhighlighter.h"

#include <QWidget>

class QCheckBox;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

// ============================================================================
// MarkerCard — один маркер в виде карточки (CardFrame).
//
// Полоска-акцент слева показывает цвет, в который красятся строки.
//
//   ▌ ☑ [ключевое слово...................]  ⚙  ▦  ✕
//   ▌   ☐ Case sensitive  ☐ Regular expression   (⚙ строка)
// ============================================================================

class MarkerCard : public CardFrame {
    Q_OBJECT
public:
    explicit MarkerCard(const HighlightPattern& pattern, QWidget* parent = nullptr);

    HighlightPattern pattern() const;

signals:
    void removeRequested();
    void applyShortcutPressed(); // Enter в поле ключевого слова

private:
    void chooseColor();
    void updateColorButton();
    void updateGearHighlight();

    QCheckBox*   m_enabledCheckBox;
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
// MarkerPanelWidget — недеструктивные маркеры строк (содержимое дока).
//
// Пользователь задаёт ключевое слово и цвет; совпавшие строки лога
// окрашиваются, но НЕ фильтруются — остальной лог остаётся видимым.
//
// Применение явное, как в конструкторе фильтров: Apply красит строки
// АКТИВНОГО документа, Reset снимает окраску с него (маркеры в панели
// остаются). Enter в поле ключевого слова работает как Apply.
// ============================================================================

class MarkerPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit MarkerPanelWidget(QWidget* parent = nullptr);

    QVector<HighlightPattern> markers() const;
    void setMarkers(const QVector<HighlightPattern>& markers);

signals:
    // Пользователь нажал Apply (или Enter в поле маркера).
    void applyRequested();
    // Пользователь нажал Reset — снять окраску с активного документа.
    void resetRequested();

private:
    void addMarker(const HighlightPattern& pattern);
    void onAddMarkerClicked();
    void removeCard(MarkerCard* card);
    QColor nextFreeColor() const;

    QVBoxLayout* m_rowsLayout;
    QVector<MarkerCard*> m_cards;
    QPushButton* m_addButton;
    QPushButton* m_applyButton;
    QPushButton* m_resetButton;
};

#endif // MARKERPANELWIDGET_H
