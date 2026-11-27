#ifndef SCHEMALINEPREVIEW_H
#define SCHEMALINEPREVIEW_H

#include <QPlainTextEdit>
#include <QStringList>
#include <QVector>

class QToolButton;

// ============================================================================
// SchemaLinePreview — строки лога, раскрашенные по полям (редактор схем,
// U-28): текст каждого поля залит цветом его блока (SchemaFieldStrip::
// fieldColor), строка, которую схема не приняла, — на красноватом фоне,
// продолжение записи — приглушено. Это главная поверхность правки:
//
//   • щелчок по цветному участку — columnClicked (окно выбирает поле);
//   • выделение мышью или с клавиатуры ВНУТРИ одной строки — у конца
//     выделения появляется кнопка «Make Field» (makeFieldRequested); двойной
//     щелчок выделяет слово, как в любом поле текста;
//   • подсказка над участком — «Имя: значение».
//
// Только вид: спаны полей даёт окно (SchemaSampleModel), операцию выполняет
// оно же. Текст не правится. Цвета — из палитры при каждой раскраске; на
// PaletteChange раскраска строится заново.
// ============================================================================
class SchemaLinePreview : public QPlainTextEdit {
    Q_OBJECT
public:
    struct Span {
        int column = -1;
        int start = 0;
        int length = 0;
    };
    struct Line {
        enum class State { Accepted, Partial, Missed, Other };
        QString text;
        qint64 number = 0;   ///< строка файла, 0 — нет
        State state = State::Other;
        int row = -1;        ///< строка таблицы образцов, -1 — не принята
        QVector<Span> spans;
    };
    struct Selection {
        int line = -1;
        int start = 0;
        int length = 0;
        bool isValid() const { return line >= 0 && length > 0; }
    };

    explicit SchemaLinePreview(QWidget* parent = nullptr);

    void setLines(const QVector<Line>& lines);
    const QVector<Line>& lines() const { return m_lines; }
    void setColumnNames(const QStringList& names);
    void setCurrentColumn(int column);
    int currentColumn() const { return m_current; }

    /// Выделение внутри одной строки (иначе — невалидное).
    Selection lineSelection() const;
    /// Выделить [start, start + length) строки line (клавиатура и тест).
    void selectInLine(int line, int start, int length);
    void clearLineSelection();
    /// Колонка поля в позиции строки; -1 — не поле.
    int columnAt(int line, int position) const;
    /// Позиция в строке под точкой вьюпорта: line = -1 — мимо текста.
    Selection positionAt(const QPoint& viewportPos) const;
    /// Середина участка поля column в строке line (координаты вьюпорта).
    QPoint spanCenter(int line, int column) const;

    QToolButton* makeFieldButton() const { return m_makeButton; }

signals:
    void columnClicked(int column);
    void lineSelectionChanged();
    void makeFieldRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void recolor();
    void placeMakeButton();

    QVector<Line> m_lines;
    QStringList m_names;
    int m_current = -1;
    QToolButton* m_makeButton = nullptr;
    QPoint m_pressPos;
    bool m_pressed = false;
};

#endif // SCHEMALINEPREVIEW_H
