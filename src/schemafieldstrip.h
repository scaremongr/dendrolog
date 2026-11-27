#ifndef SCHEMAFIELDSTRIP_H
#define SCHEMAFIELDSTRIP_H

#include <QColor>
#include <QPalette>
#include <QString>
#include <QVector>
#include <QWidget>

// ============================================================================
// SchemaFieldStrip — схема полей цветными блоками в ряд (редактор схем,
// U-28): то, что раньше было цепочкой карточек, а между шагами 9 и U-28 —
// только строкой шаблона. Блок поля — цвет поля, имя и подпись «где оно
// кончается» словами; между блоками — текст, который стоит в строке между
// полями (скобки, разделители; пробел — «␣»). Те же цвета — у полей в
// раскрашенных строках лога (SchemaLinePreview) и у карточки поля.
//
// Один виджет, рисует сам (палитра — при отрисовке: смена темы ничего не
// кэширует). Блоки переносятся по ширине (heightForWidth).
//
//   • щелчок по блоку — columnActivated (поле с колонкой) или pieceActivated
//     (поле без колонки, «{_}»); правый щелчок и клавиша меню —
//     menuRequested;
//   • щелчок по тексту между блоками — literalActivated (окно даёт его
//     поправить);
//   • последним в ряду — «+ Add field» (addRequested): новому блоку нужно
//     место, окно разбивает последнее поле на два;
//   • клавиатура: ← / → — по полям, Enter / Пробел — выбрать, «+» и Insert —
//     добавить поле.
//
// Только вид: что значит выбор, решает окно (SchemaEditorWindow).
// ============================================================================
class SchemaFieldStrip : public QWidget {
    Q_OBJECT
public:
    struct Item {
        enum class Type { Field, Literal, Add };
        Type type = Type::Field;
        QString name;         ///< Field: имя колонки; пусто — поле без колонки
        QString caption;      ///< Field: где кончается, словами («until “]”»)
        QString text;         ///< Literal: текст между полями, как в строке; Add: подпись
        int column = -1;      ///< Field: колонка схемы; -1 — без колонки
        int piece = -1;       ///< номер элемента шаблона (SchemaEdit::piecesOf)
        bool optional = false;   ///< внутри участка {? … ?}
        QString toolTip;
    };

    explicit SchemaFieldStrip(QWidget* parent = nullptr);

    void setItems(const QVector<Item>& items);
    const QVector<Item>& items() const { return m_items; }
    /// Что показать, когда блоков нет.
    void setPlaceholderText(const QString& text);

    /// Выбранное поле (рамка); -1 — нет.
    void setCurrentColumn(int column);
    int currentColumn() const { return m_current; }
    /// Выбранный элемент без колонки (текст между полями, скрытая часть); -1 — нет.
    void setCurrentPiece(int piece);
    int currentPiece() const { return m_currentPiece; }

    /// Прямоугольник блока (координаты виджета); пустой — нет такого.
    QRect itemRect(int index) const;
    QRect columnRect(int column) const;
    /// Блок элемента шаблона piece (текст между полями, скрытая часть).
    QRect pieceRect(int piece) const;
    /// Блок «+ Add field»; пустой — его нет.
    QRect addRect() const;
    int itemAt(const QPoint& pos) const;

    /// Цвет поля №column под эту палитру — один на блок, строки и карточку.
    static QColor fieldColor(int column, const QPalette& palette);
    /// Текст литерала для показа: пробел — «␣», табуляция — «⇥».
    static QString visibleLiteral(const QString& text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

signals:
    void columnActivated(int column);
    void pieceActivated(int piece);
    void literalActivated(int piece);
    void addRequested();
    void menuRequested(int column, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    QSize itemSize(const Item& item) const;
    int layoutItems(int width, QVector<QRect>* rects) const;
    void activate(int index);
    int indexOfColumn(int column) const;

    QVector<Item> m_items;
    QVector<QRect> m_rects;
    QString m_placeholder;
    int m_current = -1;
    int m_currentPiece = -1;
    int m_hover = -1;
};

#endif // SCHEMAFIELDSTRIP_H
