#ifndef LOGLISTVIEW_H
#define LOGLISTVIEW_H

#include <QListView>
#include <QMap>
#include <QSet>
#include <QStringView>
#include <QTimer>
#include <QVector>

// Структура для хранения информации о выделяемом токене (число, строка в кавычках)
struct HighlightToken {
    int start;          // Начальная позиция в оригинальной строке
    int end;            // Конечная позиция в оригинальной строке
    QColor color;       // Цвет для подсветки
};

// Структура для хранения информации о фрагменте текста (одной строке в режиме word wrap)
struct TextFragment {
    int startPos;       // Начальная позиция в оригинальной строке
    int length;         // Длина фрагмента
    QStringView text;   // "Вид" на фрагмент текста (не владеет данными)
    QRect rect;         // Геометрия фрагмента
};

// Типы плашек, которые рисуются справа от строки
enum class BadgeType {
    HiddenToggle, // "+N" или "-" для скрытых символов / разворота строки
    Info          // Информационная плашка (например, имя файла)
};

struct BadgeSpec {
    BadgeType type;
    QString text;
    QColor bg;
    QColor fg;
};

struct BadgeLayout {
    BadgeSpec spec;
    QRect rect;
};

// ============================================================================
// RowState — кэшированное состояние строки (вычисляется один раз при изменении)
// ============================================================================
struct RowState {
    // Идентификация
    int row = -1;
    
    // Данные из модели (для валидации кэша)
    QString text;
    int viewportWidth = 0;
    
    // Вычисленные значения
    bool multiLine = false;     // Многострочный режим
    int height = 0;             // Высота строки в пикселях
    int hiddenChars = 0;        // Количество скрытых символов (для однострочного)
    int visibleChars = 0;       // Количество видимых символов (для ограничения выделения)
    
    // Плашки
    QList<BadgeLayout> badges;
    int badgesWidth = 0;        // Суммарная ширина плашек + gaps
    
    // Область для текста (относительно начала строки, y=0)
    QRect textRect;
    
    // Фрагменты для многострочного режима
    QList<TextFragment> fragments;
    
    void invalidate() {
        row = -1;
        text.clear();
        viewportWidth = 0;
    }
};

// Вспомогательная структура для информации о строке под курсором
struct RowHitInfo {
    int row = -1;
    int rowTop = 0;
    int rowHeight = 0;
    QPoint localPos;  // позиция мыши относительно начала строки
    bool valid = false;
};

class LogListView : public QListView {
    Q_OBJECT
public:
    explicit LogListView(QWidget *parent = nullptr);
    ~LogListView() override;
    void setWordWrap(bool enabled);
    void setModel(QAbstractItemModel *model) override;

signals:
    void badgeClicked(int row, BadgeType type, const QString& text);

protected:
    // Кастомная отрисовка элементов
    void paintEvent(QPaintEvent *event) override;
    // Для поддержки выделения текста мышью
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    int sizeHintForRow(int row) const override;  // фиксированная высота — предотвращает O(N) в базовом QListView
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool viewportEvent(QEvent *event) override;

private:
    QSet<int> m_toggledRows; // строки, состояние которых инвертировано относительно m_wordWrapEnabled
    int m_selRow = -1;
    int m_selStart = -1;
    int m_selEnd = -1;
    bool m_selecting = false;
    int m_selStartFragment = -1;  // Индекс фрагмента, где начали выделение
    int m_selEndFragment = -1;    // Индекс текущего фрагмента
    bool m_wordWrapEnabled = false; // состояние по умолчанию для всех строк

    // ========== Кэш метрик моноширинного шрифта ==========
    qreal m_charWidth = 8.0; // Ширина одного символа (дробная для точного позиционирования)
    int m_lineHeight = 16;   // Высота строки
    void updateFontMetricsCache();  // Обновить кэш при смене шрифта

    // ========== Кэш состояний строк ==========
    mutable QHash<int, RowState> m_rowStateCache;
    mutable int m_cachedViewportWidth = -1;

    // ========== Кэш высот строк (O(log N) поиск) ==========
    QVector<int> m_rowHeights;       // m_rowHeights[i] = высота строки i
    QVector<int> m_rowPrefixY;       // m_rowPrefixY[i] = Y-смещение строки i; размер rowCount+1
    int m_totalHeight = 0;
    bool m_heightsDirty = true;
    bool m_uniformHeights = false;   // все строки однострочные → O(1) вместо prefix-sum
    mutable QHash<int, int> m_rowTextLengths; // row → длина текста; переживает resize, сбрасывается только при смене модели
    QTimer* m_heightUpdateTimer = nullptr;    // отложенный пересчёт prefix-sum после ленивого уточнения высот
    void rebuildHeightCache();       // пересчитать кэш высот без обращения к модели (только GUI-поток)
    void rebuildPrefixSums();        // пересчитать prefix-суммы из m_rowHeights
    int rowYOffset(int row) const;   // O(1) Y-позиция начала строки
    int rowAtY(int contentY) const;  // O(log N) индекс строки по Y-позиции

    // ========== Debounce для resize ==========
    QTimer* m_resizeDebounceTimer = nullptr;
    int m_anchorRow = -1;           // Строка-якорь для сохранения позиции при resize
    int m_anchorOffsetInViewport = 0; // Смещение якорной строки относительно viewport

    // Получить кэшированное состояние строки (вычисляет если нужно)
    // Возвращает по значению, чтобы избежать dangling reference при инвалидации кэша
    RowState getRowState(int row) const;

    // Инвалидировать кэш строки (или всех строк если row == -1)
    // preserveTextLengths=true: данные модели не изменились (resize/wrap-toggle) — длины текстов оставить
    void invalidateRowState(int row = -1, bool preserveTextLengths = false);
    
    // Вычислить состояние строки (внутренний метод)
    RowState computeRowState(int row) const;
    
    // Удобный хелпер для получения высоты строки (использует только height, копия RowState не нужна)
    int getRowHeight(int row) const;

    // ========== Кэш растров для отрисовки ==========
    mutable QHash<int, QPixmap> m_rowPixmapCache;
    static const int MAX_CACHED_ROWS = 100;

    // Универсальная функция для определения позиции текста по координатам мыши
    int getTextPositionFromMouse(const QPoint& mousePos, const RowState& state) const;

    // Хелперы для корректного определения строки и фрагмента
    bool getRowAtContentY(int contentY, int& row, int& rowTop) const;
    int getFragmentIndexForPosition(int pos, const QList<TextFragment>& fragments) const;

private:
    // Helper-функции для эффективной работы с QStringView
    int getCharIndexAt(const QStringView& text, int x) const;
    int calculateWidth(const QStringView& text) const;
    int textWidthUntil(const QStringView& text, int count) const;
    void drawTextWithHighlights(QPainter& painter, int x, int y, const QStringView& text, int fragmentStartPos, const QList<HighlightToken>& tokens) const;
    QList<HighlightToken> findHighlightTokens(const QString& text) const;

    // Вспомогательные методы для вычисления состояния строки
    QList<TextFragment> splitTextIntoLines(const QString& text, const QRect& rect) const;
    int computeRowHeight(const QString& text, bool multiLine, int availableWidth = 0) const;
    QList<BadgeSpec> collectBadgeSpecs(int row, const QString& text, int availableWidth, bool multiLine, int& hiddenCount) const;
    QList<BadgeLayout> layoutBadges(const QList<BadgeSpec>& specs, int rowHeight) const;
    
    // Отрисовка
    void drawLogLine(QPainter& painter, const QRect& rect, const QString& text, const RowState& state);
    void drawSelectionHighlight(QPainter& painter, const QRect& rect, const QString& text, int selStart, int selEnd, const RowState& state);
    void drawHighlightedText(QPainter& painter, int x, int y, const QString& text, const QList<QPair<int, int>>& highlights, const QColor& highlightColor, const QColor& defaultColor);
    void updateScrollbar();
    
    // Кэширование растров строк
    QPixmap getRowPixmap(int row, const RowState& state);
    
    // Hit-testing
    bool hitTestBadge(const QList<BadgeLayout>& layouts, const QPoint& pos, int& badgeIndex) const;
    
    // Определяет, многострочная ли строка (m_wordWrapEnabled XOR toggled)
    bool isRowMultiLine(int row) const { 
        return m_wordWrapEnabled != m_toggledRows.contains(row); 
    }
    void toggleRowMultiLine(int row);
};

#endif // LOGLISTVIEW_H
