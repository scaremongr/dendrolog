#ifndef LOGLISTVIEW_H
#define LOGLISTVIEW_H

#include <QListView>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QStringView>
#include <QTimer>
#include <QVector>
#include <limits>
#include "syntaxhighlighter.h"
#include "textmatchhighlighter.h"
#include "tabexpander.h"
#include "linewrapper.h"

struct LogFile;

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
    Info,         // Информационная плашка (например, имя файла)
    Warning       // Предупреждение о несовпадении со схемой
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
    bool multiLine = false;     // Текст реально переносится на несколько визуальных строк
    bool showCollapseBadge = false; // Нужно ли рисовать бейдж "-" для сворачивания строки
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
    int rowHeight = 0;
    QPoint localPos;  // позиция мыши относительно начала строки
    bool valid = false;
};

// ============================================================================
// TextSelection — всё состояние текстового выделения в одном месте.
// Инкапсулирует нормализацию, диапазоны и запросы так, чтобы вызывающий
// код никогда не занимался ручной арифметикой min/max над полями.
// ============================================================================
struct TextSelection {
    int  anchorRow  = -1;  // строка, где началось выделение (якорь — не меняется во время drag)
    int  anchorPos  = -1;  // символьная позиция начала в anchorRow
    int  activeRow  = -1;  // строка под курсором (меняется во время drag)
    int  activePos  = -1;  // символьная позиция курсора в activeRow
    bool isDragging = false;

    bool isValid()    const { return anchorRow >= 0 && activeRow >= 0 && anchorPos >= 0 && activePos >= 0; }
    bool isEmpty()    const { return !isValid() || (anchorRow == activeRow && anchorPos == activePos); }
    bool isMultiRow() const { return isValid() && anchorRow != activeRow; }
    bool containsRow(int row) const { return isValid() && row >= topRow() && row <= bottomRow(); }

    // Нормализованные границы диапазона строк
    int topRow()    const { return std::min(anchorRow, activeRow); }
    int bottomRow() const { return std::max(anchorRow, activeRow); }

    // Позиция начала выделения (в topRow)
    int firstPos() const {
        if (anchorRow == activeRow) return std::min(anchorPos, activePos);
        return (anchorRow < activeRow) ? anchorPos : activePos;
    }
    // Позиция конца выделения (в bottomRow)
    int lastPos() const {
        if (anchorRow == activeRow) return std::max(anchorPos, activePos);
        return (anchorRow > activeRow) ? anchorPos : activePos;
    }

    // Границы подсветки [selStart, selEnd] для указанной строки.
    // Вызывать только если containsRow(row) == true.
    void rangeForRow(int row, int textLen, int& selStart, int& selEnd) const {
        if (anchorRow == activeRow) {
            selStart = std::min(anchorPos, activePos);
            selEnd   = std::max(anchorPos, activePos);
        } else if (row == topRow()) {
            selStart = firstPos();
            selEnd   = textLen;
        } else if (row == bottomRow()) {
            selStart = 0;
            selEnd   = lastPos();
        } else {
            selStart = 0;       // промежуточная строка — выделена целиком
            selEnd   = textLen;
        }
    }

    void clear() { *this = TextSelection{}; }

    // Начать новое выделение: якорь = курсор = (row, pos), drag активен
    void start(int row, int pos) {
        anchorRow = activeRow = row;
        anchorPos = activePos = pos;
        isDragging = true;
    }
    void moveTo(int row, int pos) { activeRow = row; activePos = pos; }
    void finish() { isDragging = false; }
};

class LogListView : public QListView {
    Q_OBJECT
public:
    explicit LogListView(QWidget *parent = nullptr);
    ~LogListView() override;
    void setWordWrap(bool enabled);
    bool isWordWrap() const { return m_wordWrapEnabled; }
    void setModel(QAbstractItemModel *model) override;
    void scrollTo(const QModelIndex& index, ScrollHint hint = EnsureVisible) override;

    // ---- Inline-подсветка совпадений (универсальная) ----------------------
    // Источник паттернов внешний (фильтры, поиск, любой модуль) — view лишь
    // рисует фоновые заливки под текстом, не затрагивая раскраску
    // SyntaxHighlighter. Заливки попадают в кэш пиксмапов строк, поэтому
    // стоимость при скролле не растёт.
    void setTextHighlightPatterns(const QVector<HighlightPattern>& patterns);
    const TextMatchHighlighter& textHighlighter() const { return m_textHighlighter; }

    // ---- Подсветка результата поиска (одна строка) -------------------------
    // Раскрывает строку row (если она свёрнута в однострочный режим) и
    // подсвечивает в ней — и только в ней — все вхождения term.
    // Предыдущая поисковая подсветка снимается автоматически.
    void showSearchMatch(int row, const QString& term, bool caseSensitive);
    void clearSearchMatch();

    // ---- Follow-tail --------------------------------------------------------
    // Автопрокрутка к концу при догрузке строк (живые логи, stdin). Уход
    // пользователя от низа (колесо вверх, drag скроллбара, Up/PgUp/Home)
    // выключает режим; программные вставки строк — нет.
    void setFollowTail(bool enabled);
    bool followTail() const { return m_followTail; }

signals:
    void badgeClicked(int row, BadgeType type, const QString& text);

    // Режим follow-tail включён/выключен (в т.ч. автоматически при уходе
    // пользователя от низа) — для синхронизации тогла в тулбаре.
    void followTailChanged(bool enabled);

    // Пользователь выбрал «использовать таймстамп как границу фильтра по времени»
    // из контекстного меню. isStart == true → нижняя граница (From), иначе верхняя (To).
    void timeFilterBoundRequested(const QDateTime& dt, bool isStart);

protected:
    // Кастомная отрисовка элементов
    void paintEvent(QPaintEvent *event) override;
    // Для поддержки выделения текста мышью
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    int sizeHintForRow(int row) const override;  // фиксированная высота — предотвращает O(N) в базовом QListView
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void updateGeometries() override;
    void changeEvent(QEvent* event) override;
    // Нейтрализация внутренней раскладки QListView (flowPositions): она O(N)
    // по строкам на каждый rowsInserted/reset — сотни мс на десятках миллионов
    // строк при каждом батче загрузки. Вся геометрия строк у view своя
    // (rowAtY/rowYOffset/prefix-суммы), базовые visualRect/indexAt не
    // используются нигде; единственный их потребитель — базовый moveCursor —
    // замещён собственным.
    void doItemsLayout() override;
    QModelIndex moveCursor(CursorAction cursorAction,
                           Qt::KeyboardModifiers modifiers) override;

private:
    // ========== Подсветка парных скобок ======================================
    struct BracketMatch {
        int row = -1;  // строка, где найдена парная скобка (-1 = нет совпадения)
        int pos = -1;  // позиция закрывающей скобки в строке
        bool isValid() const { return row >= 0 && pos >= 0; }
        void clear()         { row = -1; pos = -1; }
    };
    BracketMatch m_bracketMatch;          // текущее совпадение парной скобки
    void updateBracketMatch();            // пересчитать совпадение по текущему выделению

    QSet<int> m_toggledRows; // строки, состояние которых инвертировано относительно m_wordWrapEnabled
    TextSelection m_selection;  // всё состояние текстового выделения

    // Соединения с текущей моделью. connect(model, ..., this, ...) живёт, пока
    // живы ОБА объекта — при замене модели соединения нужно рвать явно, иначе
    // старая модель продолжит дёргать кэши view.
    QList<QMetaObject::Connection> m_modelConnections;
    bool m_wordWrapEnabled = false; // состояние по умолчанию для всех строк
    bool m_inUpdateGeometries = false; // предотвращение рекурсии

    // ========== Левый отступ для маркера начала элемента ==========
    static constexpr int kLeftMarginWidth = 12; // Ширина полоски слева, где рисуется "›"

    // ========== Константы геометрии и отрисовки ==========
    // Именование устраняет magic numbers и делает связи между функциями явными.
    static constexpr int kLineVerticalPadding = 2; // вертикальный зазор строки сверху+снизу (добавляется к m_lineHeight)
    static constexpr int kTextPaddingX        = 4; // отступ текстового контента от левого (и правого) края text rect
    static constexpr int kTextBadgeGap        = 8; // зазор между правым краем текста и ближайшей плашкой
    static constexpr int kBadgeHPadding       = 10; // суммарный горизонтальный внутренний отступ плашки (5px слева + 5px справа)
    static constexpr int kBadgeGap            = 4;  // зазор между соседними плашками (и от края viewport до последней)

    // ========== Geometry helpers — единственная точка правды о геометрии столбцов viewport ==========
    // Все, кто хочет знать «где начинается текст» или «как мышь→символ», идут сюда.
    // Добавить второй желобок = изменить textAreaLeft() и paintGutter(). Ничего больше.
    int textAreaLeft() const;                                          // X начала текстовой области
    int viewportToTextX(int viewportX) const;                          // viewport-X → text-local-X
    QRect textAreaScreenRect(int y, int height, int badgesWidth) const; // экранный rect текстовой области
    void paintGutter(QPainter& painter, int row, QRect gutterRect);    // отрисовка всего содержимого желобка
    int singleRowHeight() const;                                       // m_lineHeight + kLineVerticalPadding

    // ========== Кэш метрик моноширинного шрифта ==========
    qreal m_charWidth = 8.0; // Ширина одного символа (дробная для точного позиционирования)
    int m_lineHeight = 16;   // Высота строки
    int m_collapseBadgeWidth = 0;     // ширина бейджа "-" (+ паддинг и зазор) — не мерить шрифтом на каждую строку
    int m_hiddenToggleBadgeWidth = 0; // резерв под бейдж "+9999" (+ паддинг и зазор)
    void updateFontMetricsCache();  // Обновить кэш при смене шрифта

    // ========== Раскрытие табуляции ==========
    // Ширина табстопа в колонках (моноширинных ячейках). Вся tab-aware геометрия
    // инкапсулирована в TabExpander — единственном источнике правила табстопа.
    static constexpr int kTabWidth = 4;
    TabExpander m_tabExpander{kTabWidth};

    // Перенос по словам. Зависит от m_tabExpander (хранит ссылку) — поэтому
    // объявлен СТРОГО после него, чтобы порядок инициализации членов был верным.
    LineWrapper m_lineWrapper{m_tabExpander};

    // ========== Кэш состояний строк ==========
    // Ограничен по размеру: RowState хранит копию текста строки, безлимитный
    // кэш при скролле накапливал бы копию всего файла. Лимит с запасом
    // покрывает видимые строки + окно точечных обновлений (dataChanged ≤ 500).
    static constexpr int kMaxCachedRowStates = 2048;
    mutable QHash<int, RowState> m_rowStateCache;
    mutable int m_cachedViewportWidth = -1;

    // ========== Кэш высот строк (O(log N) поиск) ==========
    // Пиксельные координаты КОНТЕНТА 64-битные: суммарная высота большого лога
    // (особенно в wrap-режиме) переполняет int (~2.1 млрд px). Высота одной
    // строки остаётся int. Скроллбар Qt 32-битный — при превышении диапазона
    // его значения масштабируются через m_scrollScale (см. updateScrollbar).
    QVector<int> m_rowHeights;       // m_rowHeights[i] = высота строки i
    QVector<qint64> m_rowPrefixY;    // m_rowPrefixY[i] = Y-смещение строки i; размер rowCount+1
    qint64 m_totalHeight = 0;
    bool m_heightsDirty = true;
    bool m_uniformHeights = false;   // все строки однострочные → O(1) вместо prefix-sum
    // row → длина display-текста; -1 = неизвестно. Плотный вектор вместо хэша:
    // на миллионах строк QHash<int,int> занимал бы в разы больше памяти.
    // Переживает resize, сбрасывается при изменении данных модели.
    mutable QVector<int> m_rowTextLengths;
    QTimer* m_heightUpdateTimer = nullptr;    // отложенный пересчёт prefix-sum после ленивого уточнения высот
    // Первая строка, начиная с которой prefix-суммы устарели (INT_MAX = актуальны).
    // Все изменения m_rowHeights[i] без немедленного пересчёта сумм обязаны
    // отметиться здесь — rebuildPrefixSums() пересчитывает только суффикс от
    // этой строки (при уточнении высот видимых строк это O(N−row), а не O(N)).
    int m_prefixDirtyFrom = std::numeric_limits<int>::max();
    void markPrefixDirtyFrom(int row) { m_prefixDirtyFrom = qMin(m_prefixDirtyFrom, row); }
    void rebuildHeightCache();       // пересчитать кэш высот без обращения к модели (только GUI-поток)
    void rebuildPrefixSums();        // достроить prefix-суммы из m_rowHeights (частично, от m_prefixDirtyFrom)
    void cacheTextLength(int row, int length) const; // записать длину в m_rowTextLengths (с ростом вектора)
    int wrapCharsPerLine() const;    // сколько моноширинных колонок помещается в строку wrap-режима
    int estimateWrappedRowHeight(int length, int charsPerLine) const; // оценка высоты по длине; length < 0 → заглушка
    void handleRowsInserted(int first, int last); // инкрементальный append / полная инвалидация для вставки в середину
    qint64 rowYOffset(int row) const;    // O(1) Y-позиция начала строки (координаты контента)
    int rowAtY(qint64 contentY) const;   // O(log N) индекс строки по Y-позиции
    int targetScrollValueForRow(int row, ScrollHint hint) const;      // → значение скроллбара
    int targetScrollValueForRowOffset(int row, int viewportOffset) const; // → значение скроллбара
    bool captureVisibleRowOffset(int row, int& viewportOffset) const;

    // ========== Отображение скроллбар ↔ координаты контента ==========
    // Пока контент помещается в int, масштаб равен 1 и значение скроллбара —
    // это пиксель контента (прежнее поведение бит-в-бит). За пределами int
    // скроллбар работает в сжатой шкале [0, kMaxScrollRange].
    static constexpr int kMaxScrollRange = std::numeric_limits<int>::max() / 2;
    qreal m_scrollScale = 1.0;               // contentY ≈ scrollValue * m_scrollScale
    qint64 scrollContentY() const;           // текущая позиция скролла в координатах контента
    int contentYToScrollValue(qint64 contentY) const; // обратное преобразование (с клампом)
    // Заменяет ОЦЕНКИ высот точными значениями для строк над row (на глубину
    // вьюпорта). Нужно перед якорением скролла на row: rebuildHeightCache даёт
    // приближённые высоты, и накопленная ошибка строк выше якоря сместила бы
    // его при реальной отрисовке.
    void refineHeightsAbove(int row);

    // ========== Debounce для resize ==========
    QTimer* m_resizeDebounceTimer = nullptr;
    int m_anchorRow = -1;           // Строка-якорь для сохранения позиции при resize
    int m_anchorOffsetInViewport = 0; // Смещение якорной строки относительно viewport

    // ========== Сохранение выделения и позиции при фильтрации ==========
    struct SelectionId {
        int logicalEntryId = -1;
        // Идентичность записи = пара (logicalEntryId, sourceFile):
        // id уникален только внутри одного файла.
        const LogFile* sourceFile = nullptr;
        bool isValid() const { return logicalEntryId >= 0; }
        void clear() { logicalEntryId = -1; sourceFile = nullptr; }
    };
    // Долгоживущий якорь: ЗАПИСЬ (не номер строки), которую пользователь выбрал
    // последней. Обновляется в currentChanged() и переживает любые reset модели.
    // Благодаря этому выделение и позиция восстанавливаются даже если запись
    // была временно скрыта фильтром, а затем фильтр сняли.
    SelectionId m_persistentSelection;
    // Транзиентный якорь вьюпорта: первая видимая строка на момент
    // modelAboutToBeReset. Используется, когда выделения нет (или выделенная
    // запись исчезла из данных), чтобы прокрутка не сбрасывалась на начало.
    SelectionId m_resetViewportAnchor;
    int m_resetViewportOffset = 0; // scrollY - rowTop якорной строки

    // Получить кэшированное состояние строки (вычисляет если нужно)
    // Возвращает по значению, чтобы избежать dangling reference при инвалидации кэша
    RowState getRowState(int row) const;

    // Инвалидировать кэш строки (или всех строк если row == -1)
    // preserveTextLengths=true: данные модели не изменились (resize/wrap-toggle) — длины текстов оставить
    void invalidateRowState(int row = -1, bool preserveTextLengths = false);

    // Сдвинуть состояние, привязанное к номерам строк (toggled-строки, текстовое
    // выделение, подсветка поиска и скобок), после вставки count строк начиная
    // с first. Вставка в конец (обычный случай) ничего не сдвигает.
    void shiftRowKeyedState(int first, int count);
    // Симметрично для удаления count строк начиная с first: сдвиг вверх,
    // состояние из удалённого диапазона сбрасывается.
    void removeRowKeyedState(int first, int count);
    
    // Вычислить состояние строки (внутренний метод)
    RowState computeRowState(int row) const;
    
    // Удобный хелпер для получения высоты строки (использует только height, копия RowState не нужна)
    int getRowHeight(int row) const;

    // ========== Кэш растров для отрисовки ==========
    // Кэш ограничен бюджетом в байтах и адаптивным лимитом записей; вытеснение —
    // строки, далёкие от позиции скролла, а не полный сброс (иначе каждый новый
    // рендер выбрасывал бы и видимые строки). Все операции над кэшем — только
    // через removeRowPixmap()/clearRowPixmaps()/getRowPixmap(), чтобы счётчик
    // байт не разошёлся с содержимым.
    mutable QHash<int, QPixmap> m_rowPixmapCache;
    mutable qint64 m_rowPixmapCacheBytes = 0;
    static constexpr qint64 kMaxPixmapCacheBytes = 64ll * 1024 * 1024;
    // Растры крупнее порога не кэшируем (гигантские wrapped-строки): один такой
    // растр съел бы весь бюджет, а за пределами ~32K пикселей вообще не создастся.
    // Такие строки рисуются напрямую с отсечением невидимых фрагментов.
    static constexpr qint64 kMaxSingleRowPixmapBytes = 8ll * 1024 * 1024;
    int maxCachedPixmaps() const;       // адаптивный лимит записей (≥ 3 экранов строк)
    bool isRowPixmapCacheable(const RowState& state) const; // false — строка рисуется напрямую каждый кадр
    void removeRowPixmap(int row) const;   // также сбрасывает кэш разбора строки
    void clearRowPixmaps() const;          // также сбрасывает кэш разбора
    void evictDistantPixmaps() const;   // вытеснить строки, далёкие от вьюпорта

    // ========== Кэш разбора строк для прямой отрисовки ==========
    // Строки, не влезающие в пиксмап-кэш (isRowPixmapCacheable() == false),
    // рисуются напрямую на каждый кадр — токенизация и заливки совпадений для
    // них кэшируются, иначе каждый кадр стоил бы O(длина строки).
    // Жизненный цикл строго совпадает с пиксмап-кэшем: инвалидация только
    // через removeRowPixmap()/clearRowPixmaps().
    struct RowParseCache {
        QList<HighlightToken> tokens;      // синтаксическая подсветка
        QVector<HighlightSpan> matchSpans; // заливки m_textHighlighter
    };
    mutable QHash<int, RowParseCache> m_rowParseCache;
    static constexpr int kMaxParseCacheRows = 16; // гигантских строк на экране единицы

    // Универсальная функция для определения позиции текста по координатам мыши
    int getTextPositionFromMouse(const QPoint& mousePos, const RowState& state) const;

    // Хелпер для корректного определения строки под Y-координатой контента
    bool getRowAtContentY(qint64 contentY, int& row, qint64& rowTop) const;

private:
    // Helper-функции для эффективной работы с QStringView
    int getCharIndexAt(const QStringView& text, int x) const;
    int calculateWidth(const QStringView& text) const;
    int textWidthUntil(const QStringView& text, int count) const;
    void drawTextWithHighlights(QPainter& painter, int x, int y, const QStringView& text, int fragmentStartPos, const QList<HighlightToken>& tokens) const;

    // Вспомогательные методы для вычисления состояния строки
    QList<TextFragment> splitTextIntoLines(const QString& text, const QRect& rect) const;
    int computeRowHeight(const QString& text, bool multiLine, int availableWidth = 0) const;
    QList<BadgeSpec> collectBadgeSpecs(int row, const QString& text, int availableWidth, bool multiLine, bool showCollapseBadge, int& hiddenCount) const;
    QList<BadgeLayout> layoutBadges(const QList<BadgeSpec>& specs, int rowHeight) const;
    
    // ========== Inline-подсветка совпадений ==========
    TextMatchHighlighter m_textHighlighter;

    // ========== Подсветка результата поиска ==========
    // Действует ровно на одну строку (m_searchMatchRow); сбрасывается при
    // reset модели, т.к. индексы строк меняются.
    TextMatchHighlighter m_searchHighlighter;
    int m_searchMatchRow = -1;

    // Отрисовка
    void drawLogLine(QPainter& painter, const QRect& rect, const QString& text, const RowState& state);
    // Фоновые заливки совпадений; рисуются ДО текста, чтобы не конфликтовать
    // с цветами SyntaxHighlighter и выделением.
    void drawMatchHighlightSpans(QPainter& painter, const QRect& rect, const QString& text,
                                 const RowState& state, const QVector<HighlightSpan>& spans) const;
    void drawSelectionHighlight(QPainter& painter, const QRect& rect, const QString& text, int selStart, int selEnd, const RowState& state);
    void drawBracketHighlight(QPainter& painter, const QRect& textRect, const RowState& state) const;
    qint64 estimateTotalHeightForDirtyCache(int rows) const;
    void updateScrollbar();
    void invalidateSelectionRange(int topRow, int bottomRow);  // точечная инвалидация диапазона строк
    
    // Кэширование растров строк
    QPixmap getRowPixmap(int row, const RowState& state);
    
    // Hit-testing
    bool hitTestBadge(const QList<BadgeLayout>& layouts, const QPoint& pos, int& badgeIndex) const;
    
    // ---- Follow-tail ---------------------------------------------------------
    bool m_followTail = false;
    void scrollToBottomFollow();

    // «Огромный режим»: выше порога строк плотные пер-строчные кэши
    // (m_rowHeights/m_rowPrefixY/m_rowTextLengths — гигабайты на сотнях
    // миллионов строк) не ведутся: высоты принудительно uniform (wrap и
    // точечные развороты отключены), длины запрашиваются у модели напрямую.
    // Индексная вкладка — huge С ПЕРВОЙ СТРОКИ: иначе, пока загрузка не
    // пересекла порог, view на каждый батч строит плотные кэши (O(батч) на
    // GUI-потоке — сотни мс при быстрой индексации), а на пороге выбрасывает
    // их целиком. Флаг кэшируется в setModel/modelReset — смена бэкенда
    // всегда сопровождается reset модели.
    static constexpr int kHugeRowCountThreshold = 2000000;
    bool m_indexedBackendModel = false;
    bool hugeRowMode() const {
        return model() && (m_indexedBackendModel
                           || model()->rowCount() > kHugeRowCountThreshold);
    }

    // Определяет, развернута ли строка относительно глобального WordWrap
    // (effective wrap XOR toggled); в огромном режиме всегда однострочно.
    bool isRowMultiLine(int row) const {
        if (hugeRowMode())
            return false;
        return m_wordWrapEnabled != m_toggledRows.contains(row);
    }
    void toggleRowMultiLine(int row);

    // True if the row currently shows a HiddenToggle badge, i.e. it can be
    // expanded/collapsed (longer than the visible width, or already multi-line).
    bool rowHasWrapToggle(int row) const;

    // Copies the current text selection to the clipboard (multi-row aware).
    // No-op when the selection is empty.
    void copySelectionToClipboard() const;

    // Returns the currently selected text (rows joined with '\n'), or empty.
    QString selectedText() const;

    // Расширяет (двигает активный конец) текстового выделения с клавиатуры.
    // direction: +1 вправо, −1 влево. byToken: true — на токен/блок, false — на символ.
    void extendSelectionByKeyboard(int direction, bool byToken);
};

#endif // LOGLISTVIEW_H
