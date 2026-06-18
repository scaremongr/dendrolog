#include "LogListView.h"
#include "logmodel.h"
#include "apptheme.h"
#include "appsettings.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QClipboard>
#include <QApplication>
#include <QScrollBar>
#include <QTextLayout>
#include <QFontDatabase>
#include <QTimer>
#include <algorithm>
#include <limits>
#include "texttoken.h"

// Кастомный виджет для отображения логов с подсветкой синтаксиса и поддержкой выделения текста
LogListView::LogListView(QWidget *parent)
    : QListView(parent)
{
    // Полностью отключаем стандартную отрисовку
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Предотвращаем O(N) обход всех строк внутри QListView при resize/layout
    setUniformItemSizes(true);
    
    // Применяем шрифт из настроек приложения (AppSettings::load() вызывается
    // в MainWindow до создания виджетов, поэтому значение уже корректное).
    QFont monoFont(AppSettings::instance().fontFamily());
    monoFont.setPointSize(AppSettings::instance().fontSize());
    monoFont.setWeight(QFont::Medium);
    setFont(monoFont);
    // updateFontMetricsCache() вызовется через changeEvent(FontChange)
    
    // Подключаем сигнал скроллбара для плавной прокрутки
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        viewport()->update();
    });

    // Таймер для отложенного пересчёта prefix-сумм после ленивого уточнения высот строк
    m_heightUpdateTimer = new QTimer(this);
    m_heightUpdateTimer->setSingleShot(true);
    connect(m_heightUpdateTimer, &QTimer::timeout, this, [this]() {
        rebuildPrefixSums();
        updateScrollbar();
        viewport()->update();
    });

    // Debounce таймер для resize событий
    m_resizeDebounceTimer = new QTimer(this);
    m_resizeDebounceTimer->setSingleShot(true);
    connect(m_resizeDebounceTimer, &QTimer::timeout, this, [this]() {
        int savedAnchorRow = m_anchorRow;
        int savedAnchorOffset = m_anchorOffsetInViewport;

        // preserveTextLengths=true: размеры viewport изменились, но данные модели те же —
        // кэш длин текста позволит пересчитать высоты без обращения к модели
        invalidateRowState(-1, /*preserveTextLengths=*/true);
        rebuildHeightCache();
        updateScrollbar();

        // Восстанавливаем позицию скролла по якорной строке
        if (savedAnchorRow >= 0 && model() && savedAnchorRow < model()->rowCount()) {
            int newRowTop = rowYOffset(savedAnchorRow);
            int newScrollY = qBound(0, newRowTop - savedAnchorOffset, verticalScrollBar()->maximum());
            verticalScrollBar()->setValue(newScrollY);
        }

        m_anchorRow = -1;
        m_anchorOffsetInViewport = 0;

        viewport()->update();
    });

    // НЕ вызываем updateScrollbar() в конструкторе - отложим до установки модели
}

void LogListView::setWordWrap(bool enabled) {
    if (m_wordWrapEnabled == enabled) return;

    m_wordWrapEnabled = enabled;

    m_toggledRows.clear();
    invalidateRowState(-1, /*preserveTextLengths=*/true);
    rebuildHeightCache();
    updateScrollbar();
    viewport()->update();

    // Высоты строк изменились — прокручиваем к выделенному элементу,
    // чтобы он не уплыл за пределы видимой области.
    QTimer::singleShot(0, this, [this]() {
        const QModelIndex cur = currentIndex();
        if (cur.isValid())
            scrollTo(cur, QAbstractItemView::EnsureVisible);
    });
}

void LogListView::setModel(QAbstractItemModel *model) {
    QListView::setModel(model);
    invalidateRowState();
    if (model) {
        // Полная инвалидация при сбросе модели (фильтрация, setEntries и т.п.)
        // Запоминаем выделение ДО сброса модели.
        // ВАЖНО: используем m_selection.anchorRow, а не selectionModel()->currentIndex() —
        // QItemSelectionModel подключён к modelAboutToBeReset раньше нас и к моменту
        // вызова нашего слота уже очищает currentIndex. anchorRow не зависит от selectionModel.
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
            m_pendingSelection.clear();
            if (m_selection.anchorRow < 0) return;
            auto* logModel = qobject_cast<LogModel*>(this->model());
            if (!logModel) return;
            const auto& entries = logModel->filteredEntries();
            if (m_selection.anchorRow < entries.size()) {
                m_pendingSelection.logicalEntryId = entries[m_selection.anchorRow]->logicalEntryId;
                m_pendingSelection.sourceFile = entries[m_selection.anchorRow]->sourceFile.get();
            }
        });

        connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            m_toggledRows.clear();
            // Поисковая подсветка привязана к индексу строки — после reset
            // индексы невалидны.
            m_searchMatchRow = -1;
            m_searchHighlighter.setPatterns({});
            invalidateRowState();
            rebuildHeightCache();

            // Восстанавливаем выделение по logicalEntryId и sourceFile.
            int restoreRow = -1;
            if (m_pendingSelection.isValid()) {
                auto* logModel = qobject_cast<LogModel*>(this->model());
                if (logModel) {
                    const auto& entries = logModel->filteredEntries();
                    for (int i = 0; i < entries.size(); ++i) {
                        if (entries[i]->logicalEntryId == m_pendingSelection.logicalEntryId &&
                            entries[i]->sourceFile.get() == m_pendingSelection.sourceFile) {
                            restoreRow = i;
                            break;
                        }
                    }
                }
                m_pendingSelection.clear();
            }

            // Обновляем скроллбар ДО вызова setCurrentIndex, чтобы первоначальные границы были корректны
            updateScrollbar();

            // Если нашли нашу строку, мы должны её выделить и прокрутить к ней.
            // ВАЖНО: Базовый QListView может откладывать вызовы updateGeometries() или внутренние пересчеты.
            // Поэтому прокрутку к элементу обязательно нужно делать через QTimer::singleShot,
            // чтобы наш код выполнился ПОСЛЕ того, как Qt закончит все свои внутренние обновления.
            if (restoreRow >= 0 && selectionModel() && this->model()) {
                selectionModel()->setCurrentIndex(
                    this->model()->index(restoreRow, 0),
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_selection.anchorRow = m_selection.activeRow = restoreRow;

                QTimer::singleShot(0, this, [this, restoreRow]() {
                    if (!this->model() || restoreRow >= this->model()->rowCount()) return;
                    
                    // Еще раз обновляем скроллбар на случай, если Qt что-то сбросило
                    updateScrollbar();

                    // Вычисляем точную позицию новой строки в пикселях
                    int rowY = rowYOffset(restoreRow);
                    int rowH = getRowHeight(restoreRow);
                    int vpH  = viewport()->height();
                    
                    // Центрируем строку на экране
                    int target = qBound(0, rowY - (vpH - rowH) / 2, verticalScrollBar()->maximum());
                    verticalScrollBar()->setValue(target);
                    viewport()->update();
                });
            } else {
                verticalScrollBar()->setValue(0);
                viewport()->update();
            }
        });

        // Инвалидация при вставке/удалении строк
        auto onRowsChanged = [this]() {
            invalidateRowState();
            rebuildHeightCache();
            updateScrollbar();
            viewport()->update();
        };
        connect(model, &QAbstractItemModel::rowsInserted, this, onRowsChanged);
        connect(model, &QAbstractItemModel::rowsRemoved,  this, onRowsChanged);

        // Инвалидация кэша строк при изменении данных (например, смена видимых полей /
        // применение нового ConversionPattern). Без этого getRowState() возвращает
        // устаревший текст из кэша даже после того, как модель уже прислала dataChanged.
        connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>&) {
                const int total = this->model() ? this->model()->rowCount() : 0;
                const int count = bottomRight.row() - topLeft.row() + 1;
                const bool isBulk = (count >= total / 4 || count > 500);
                if (isBulk) {
                    // Большой диапазон — инвалидируем всё за один раз
                    invalidateRowState(-1, /*preserveTextLengths=*/false);
                    rebuildHeightCache();
                } else {
                    for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
                        invalidateRowState(row, /*preserveTextLengths=*/false);
                    m_heightsDirty = true;
                }
                viewport()->update();
                // После массового изменения высот строк прокручиваем к выделенному
                // элементу — универсально работает при смене видимых полей, паттерна
                // и любых других модификаторов, меняющих компоновку.
                if (isBulk) {
                    QTimer::singleShot(0, this, [this]() {
                        const QModelIndex cur = currentIndex();
                        if (cur.isValid())
                            scrollTo(cur, QAbstractItemView::EnsureVisible);
                    });
                }
            });

        // Откладываем до получения реальных размеров viewport
        QTimer::singleShot(0, this, [this]() {
            rebuildHeightCache();
            updateScrollbar();
            viewport()->update();
        });
    }
}

LogListView::~LogListView() {}

// Обновление кэша метрик шрифта (вызывать при смене шрифта)
void LogListView::updateFontMetricsCache() {
    QFontMetrics fm(font());
    QFontMetricsF fmF(font());
    m_charWidth = fmF.horizontalAdvance(QLatin1Char('M')); // дробная точность — избегаем накопленной ошибки округления
    m_lineHeight = fm.height();
    
    // Инвалидируем кэши при смене шрифта
    invalidateRowState(-1, /*preserveTextLengths=*/true);
}

// ---------------------------------------------------------------------------
// changeEvent — реагирует на смену шрифта (например, при смене настроек).
// updateFontMetricsCache() вызывается всегда; деferred-пересчёт высот —
// только когда таймер уже создан (в конструкторе таймер создаётся после
// setFont, поэтому при первичной установке шрифта пересчёт не нужен —
// модели ещё нет).
void LogListView::changeEvent(QEvent* event)
{
    QListView::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateFontMetricsCache();
        if (m_heightUpdateTimer) {  // null during constructor — timer not yet created
            invalidateRowState(-1, /*preserveTextLengths=*/true);
            m_heightUpdateTimer->start(0);
            viewport()->update();
        }
    }
}

// Фиксированная высота строки для QListView — предотвращает O(N) вычисления внутри базового класса
int LogListView::sizeHintForRow(int) const {
    return singleRowHeight();
}

// ============================================================================
// RowState — кэширование и вычисление состояния строки
// ============================================================================

void LogListView::invalidateRowState(int row, bool preserveTextLengths) {
    if (row == -1) {
        m_rowStateCache.clear();
        m_rowPixmapCache.clear();
        m_cachedViewportWidth = -1;
        m_heightsDirty = true;
        if (!preserveTextLengths) {
            m_rowTextLengths.clear();
        }
    } else {
        m_rowStateCache.remove(row);
        m_rowPixmapCache.remove(row);
        m_heightsDirty = true;  // prefix sums stale after any single-row change
        if (!preserveTextLengths) {
            m_rowTextLengths.remove(row);
        }
    }
}

RowState LogListView::getRowState(int row) const {
    int vpWidth = viewport()->width();

    if (m_cachedViewportWidth == vpWidth && m_rowStateCache.contains(row)) {
        return m_rowStateCache[row];
    }

    RowState state = computeRowState(row);

    if (m_cachedViewportWidth != vpWidth) {
        m_rowStateCache.clear();
        m_rowPixmapCache.clear();
        m_cachedViewportWidth = vpWidth;
    }
    m_rowStateCache[row] = state;
    return state;
}

// O(1) при заполненном кэше высот, оиначе fallback на полный RowState
int LogListView::getRowHeight(int row) const {
    if (m_uniformHeights) {
        return singleRowHeight();  // O(1) — однострочный режим
    }
    if (!m_heightsDirty && row >= 0 && row < m_rowHeights.size() && m_rowHeights[row] > 0) {
        return m_rowHeights[row];
    }
    return getRowState(row).height;
}

// ============================================================================
// Geometry helpers — единственная точка правды о геометрии столбцов viewport
// ============================================================================

// Суммарная ширина всех левых желобков. Чтобы добавить второй желобок —
// достаточно изменить это значение и добавить отрисовку в paintGutter().
int LogListView::textAreaLeft() const {
    return kLeftMarginWidth;
}

// Высота одной строки в пикселях (единственный источник — используется вместо повсеместного m_lineHeight + 2)
int LogListView::singleRowHeight() const {
    return m_lineHeight + kLineVerticalPadding;
}

// Явное пересечение границы систем координат: viewport-X → text-local-X.
// Все функции hit-test/mouse используют только это преобразование.
int LogListView::viewportToTextX(int viewportX) const {
    return viewportX - textAreaLeft();
}

// Единственный источник экранного прямоугольника текстовой области строки.
// badgesWidth — суммарная ширина плашек справа (из state.badgesWidth).
QRect LogListView::textAreaScreenRect(int y, int height, int badgesWidth) const {
    const int x = textAreaLeft();
    return QRect(x, y, qMax(0, viewport()->width() - x - badgesWidth - kTextBadgeGap), height);
}

// Отрисовка всего содержимого желобка для строки row.
// gutterRect — прямоугольник желобка в координатах viewport (x=0 .. textAreaLeft()).
// Добавить второй маркер или номер строки — только здесь.
void LogListView::paintGutter(QPainter& painter, int row, QRect gutterRect) {
    painter.save();
    const bool isNew = model()->data(model()->index(row, 0), LogModel::IsNewRole).toBool();
    painter.setPen(isNew ? AppTheme::instance().gutterNewEntry : AppTheme::instance().gutterMarker);
    // Маркер начала записи "›", выровненный по первой строке элемента
    QRect markerRect(gutterRect.left(), gutterRect.top(),
                     gutterRect.width() - 1, singleRowHeight());
    painter.drawText(markerRect, Qt::AlignVCenter | Qt::AlignHCenter,
                     QStringLiteral("\u203A"));
    painter.restore();
}

RowState LogListView::computeRowState(int row) const {
    RowState state;
    state.row = row;
    state.viewportWidth = viewport()->width();
    // Текстовая область начинается после всех левых желобков
    const int effectiveVpWidth = state.viewportWidth - textAreaLeft();
    state.text = model()->data(model()->index(row, 0)).toString();
    m_rowTextLengths[row] = state.text.length();  // кэшируем длину — пережнвает resize
    const bool expandedRow = isRowMultiLine(row);
    const bool forceCollapseBadge = !m_wordWrapEnabled && m_toggledRows.contains(row);
    
    // Шаг 1: Определяем, нужен ли вообще бейдж сворачивания.
    // Для auto-wrap в глобальном режиме WordWrap короткие строки не должны получать "-".
    // Для строк, раскрытых вручную из режима +N, бейдж сворачивания сохраняем всегда.
    QFontMetrics fm(font());
    int fileBadgeWidth = 0;
    QVariant fileBadgeVar = model()->data(model()->index(row, 0), LogModel::FileBadgeRole);
    if (fileBadgeVar.isValid()) {
        fileBadgeWidth = fm.horizontalAdvance(fileBadgeVar.toMap()["text"].toString()) + kBadgeHPadding + kBadgeGap;
    }
    const int collapseBadgeWidth = fm.horizontalAdvance(QStringLiteral("-")) + kBadgeHPadding + kBadgeGap;
    const int hiddenToggleBadgeWidth = fm.horizontalAdvance("+9999") + kBadgeHPadding + kBadgeGap; // максимальная оценка для HiddenToggle
    const int baseAvailableTextWidth = effectiveVpWidth - fileBadgeWidth - kTextBadgeGap;
    const int fullTextWidth = calculateWidth(QStringView(state.text));

    state.showCollapseBadge = expandedRow && (forceCollapseBadge ||
        (baseAvailableTextWidth > 0 && fullTextWidth > baseAvailableTextWidth));

    int estimatedBadgesWidth = fileBadgeWidth;
    if (state.showCollapseBadge) {
        estimatedBadgesWidth += collapseBadgeWidth;
    } else if (!expandedRow) {
        estimatedBadgesWidth += hiddenToggleBadgeWidth;
    }

    int availableTextWidth = effectiveVpWidth - estimatedBadgesWidth - kTextBadgeGap;
    state.multiLine = expandedRow && availableTextWidth > 0 && fullTextWidth > availableTextWidth;
    
    // Шаг 2: Вычисляем высоту с учётом реальной доступной ширины
    state.height = computeRowHeight(state.text, state.multiLine, availableTextWidth);
    
    // Шаг 3: Собираем плашки с точным расчётом скрытых символов
    QList<BadgeSpec> badgeSpecs = collectBadgeSpecs(row, state.text, 
        availableTextWidth, state.multiLine, state.showCollapseBadge, state.hiddenChars);
    state.badges = layoutBadges(badgeSpecs, state.height);
    
    // Шаг 4: Вычисляем реальную ширину плашек
    state.badgesWidth = 0;
    for (const auto& b : state.badges) {
        state.badgesWidth += b.rect.width() + 4;
    }
    
    // Шаг 5: Область для текста (с зазором перед плашками)
    state.textRect = QRect(0, 0, effectiveVpWidth - state.badgesWidth - kTextBadgeGap, state.height);
    
    // Шаг 6: Вычисляем видимые символы для однострочного режима
    if (!state.multiLine) {
        state.visibleChars = state.text.length() - state.hiddenChars;
    } else {
        state.visibleChars = state.text.length(); // все символы видимы
    }
    
    // Шаг 7: Фрагменты для многострочного режима
    if (state.multiLine) {
        state.fragments = splitTextIntoLines(state.text, state.textRect);
        // Пересчитываем высоту по фактическому количеству фрагментов
        int actualHeight = qMax(1, state.fragments.size()) * fontMetrics().height() + 2;
        if (actualHeight != state.height) {
            state.height = actualHeight;
            // Пересчитываем позиции плашек с новой высотой
            state.badges = layoutBadges(badgeSpecs, state.height);
        }
    }
    
    return state;
}

int LogListView::computeRowHeight(const QString& text, bool multiLine, int availableWidth) const {
    if (!multiLine) {
        return singleRowHeight();
    }
    
    // Для моноширинного шрифта — простая арифметика. Считаем по визуальным
    // КОЛОНКАМ (с раскрытием табуляции), чтобы оценка высоты совпадала с реальным
    // переносом в splitTextIntoLines.
    int lineWidth = (availableWidth > 0) ? availableWidth - 2 * kTextPaddingX : viewport()->width() - textAreaLeft() - 2 * kTextPaddingX;
    if (lineWidth <= 0) lineWidth = 100;  // fallback

    int colsPerLine = qMax(1, (int)(lineWidth / m_charWidth));
    int totalCols = m_tabExpander.columns(QStringView(text));
    int lineCount = (totalCols + colsPerLine - 1) / colsPerLine;
    if (lineCount < 1) lineCount = 1;

    return lineCount * m_lineHeight + kLineVerticalPadding;
}

// ============================================================================
// Кэш высот строк — O(log N) поиск по Y-позиции
// ============================================================================

void LogListView::rebuildHeightCache() {
    // Внимание: Этот метод вызывается не только при инициализации, но и после
    // ресайза окна. Он пересчитывает высоты строк (m_rowHeights) так, чтобы 
    // скроллбар всегда точно соответствовал суммарной длине многострочных логов.
    if (!model() || viewport()->width() <= 0) {
        m_rowHeights.clear();
        m_rowPrefixY.clear();
        m_totalHeight = 0;
        m_uniformHeights = false;
        return;
    }

    const int rows = model()->rowCount();
    if (rows == 0) {
        m_rowHeights.clear();
        m_rowPrefixY.clear();
        m_totalHeight = 0;
        m_uniformHeights = false;
        m_heightsDirty = false;
        return;
    }

    const int singleRowH = singleRowHeight();

    // БЫСТРЫЙ ПУТЬ: При выключенном WordWrap (и если нет локально развернутых строк)
    // общая высота - это просто (количество строк * высоту 1 строки). 
    // Выполняется за O(1), без выделения памяти и прохождения циклов.
    if (!m_wordWrapEnabled && m_toggledRows.isEmpty()) {
        m_rowHeights.clear();
        m_rowPrefixY.clear();
        m_totalHeight = singleRowH * rows;
        m_uniformHeights = true;
        m_heightsDirty = false;
        return;
    }

    m_uniformHeights = false;
    m_rowHeights.resize(rows);

    const int vpWidth = viewport()->width();
    const int toggleDashW = QFontMetrics(font()).horizontalAdvance(QChar('-')) + kBadgeHPadding + kBadgeGap;
    const bool vpCacheValid = (m_cachedViewportWidth == vpWidth);

    // Забираем кэш длины строк напрямик (минуя тормозящий model()->data()),
    // это позволяет за доли секунды предсказать количество строк-переносов!
    auto* logModel = qobject_cast<LogModel*>(this->model());
    if (logModel && m_rowTextLengths.isEmpty()) {
        const auto& entries = logModel->filteredEntries();
        for (int i = 0; i < qMin(rows, (int)entries.size()); ++i) {
            m_rowTextLengths[i] = entries[i]->message.length();
        }
    }

    // Вычисляем сколько символов МОНОШИРИННОГО шрифта помещается в одну строку на экране
    int lineWidth = vpWidth - textAreaLeft() - toggleDashW - kTextBadgeGap - 2 * kTextPaddingX;
    if (lineWidth <= 0) lineWidth = 100;
    const int charsPerLine = qMax(1, (int)(lineWidth / m_charWidth));

    for (int i = 0; i < rows; ++i) {
        if (!isRowMultiLine(i)) {
            // Строка свернута — высота = 1 базовая строка
            m_rowHeights[i] = singleRowH;
        } else if (vpCacheValid && m_rowStateCache.contains(i)) {
            // Для этой строки уже был отрендерен пиксельный кэш
            m_rowHeights[i] = m_rowStateCache[i].height;
        } else if (m_rowTextLengths.contains(i)) {
            // Быстрый математический расчет высоты при переносе (длина / вместимость ширины)
            const int lineCount = qMax(1, (m_rowTextLengths[i] + charsPerLine - 1) / charsPerLine);
            m_rowHeights[i] = lineCount * m_lineHeight + kLineVerticalPadding;
        } else {
            // Заглушка, если кэш оборвался (практически не вызывается)
            m_rowHeights[i] = singleRowH * 3;
        }
    }

    // Восстанавливаем кэш смещений для бинарного поиска строк O(log N)
    rebuildPrefixSums();
    m_heightsDirty = false;
}

void LogListView::rebuildPrefixSums() {
    const int rows = m_rowHeights.size();
    if (m_rowPrefixY.size() != rows + 1)
        m_rowPrefixY.resize(rows + 1);
    m_rowPrefixY[0] = 0;
    for (int i = 0; i < rows; ++i)
        m_rowPrefixY[i + 1] = m_rowPrefixY[i] + m_rowHeights[i];
    m_totalHeight = (rows > 0) ? m_rowPrefixY[rows] : 0;
}

// O(1): Y-позиция начала строки row
int LogListView::rowYOffset(int row) const {
    if (m_uniformHeights) {
        return row * singleRowHeight();  // O(1) — однострочный режим
    }
    if (!m_heightsDirty && row >= 0 && row < m_rowPrefixY.size()) {
        return m_rowPrefixY[row];
    }
    // Fallback: линейная сумма
    int y = 0;
    int limit = qBound(0, row, model() ? model()->rowCount() : 0);
    for (int i = 0; i < limit; ++i) {
        y += getRowHeight(i);
    }
    return y;
}

// O(log N): индекс строки по Y-позиции в контенте
int LogListView::rowAtY(int contentY) const {
    const int rows = model() ? model()->rowCount() : 0;
    if (rows == 0) return 0;

    if (m_uniformHeights) {
        const int h = singleRowHeight();
        return qBound(0, contentY / h, rows - 1);  // O(1) — однострочный режим
    }

    if (!m_heightsDirty && (int)m_rowPrefixY.size() == rows + 1) {
        // upper_bound даёт итератор на первый элемент > contentY
        auto it = std::upper_bound(m_rowPrefixY.constBegin(), m_rowPrefixY.constEnd(), contentY);
        if (it == m_rowPrefixY.constBegin()) return 0;
        --it;
        int row = (int)(it - m_rowPrefixY.constBegin());
        return qBound(0, row, rows - 1);
    }

    // Fallback: линейный поиск
    int y = 0;
    for (int i = 0; i < rows; ++i) {
        int h = getRowHeight(i);
        if (contentY < y + h) return i;
        y += h;
    }
    return rows - 1;
}

int LogListView::targetScrollValueForRow(int row, ScrollHint hint) const {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return verticalScrollBar()->value();
    }

    const int rowTop = rowYOffset(row);
    const int rowHeight = getRowHeight(row);
    const int viewportHeight = viewport()->height();
    const int currentScroll = verticalScrollBar()->value();

    int target = currentScroll;
    switch (hint) {
        case PositionAtTop:
            target = rowTop;
            break;
        case PositionAtBottom:
            target = rowTop + rowHeight - viewportHeight;
            break;
        case PositionAtCenter:
            target = rowTop - qMax(0, (viewportHeight - rowHeight) / 2);
            break;
        case EnsureVisible:
        default:
            if (rowTop < currentScroll) {
                target = rowTop;
            } else if (rowTop + rowHeight > currentScroll + viewportHeight) {
                target = rowTop + rowHeight - viewportHeight;
            }
            break;
    }

    return qBound(0, target, verticalScrollBar()->maximum());
}

int LogListView::targetScrollValueForRowOffset(int row, int viewportOffset) const {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return verticalScrollBar()->value();
    }

    return qBound(0, rowYOffset(row) - viewportOffset, verticalScrollBar()->maximum());
}

bool LogListView::captureVisibleRowOffset(int row, int& viewportOffset) const {
    viewportOffset = 0;
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return false;
    }

    const int scrollY = verticalScrollBar()->value();
    const int rowTop = rowYOffset(row);
    const int rowBottom = rowTop + getRowHeight(row);
    const int viewportBottom = scrollY + viewport()->height();

    if (rowBottom <= scrollY || rowTop >= viewportBottom) {
        return false;
    }

    viewportOffset = rowTop - scrollY;
    return true;
}

void LogListView::scrollTo(const QModelIndex& index, ScrollHint hint) {
    if (!index.isValid() || !model() || index.model() != model()) {
        return;
    }

    const int row = index.row();
    if (row < 0 || row >= model()->rowCount()) {
        return;
    }

    const int target = targetScrollValueForRow(row, hint);
    if (target != verticalScrollBar()->value()) {
        verticalScrollBar()->setValue(target);
    } else {
        viewport()->update();
    }
}

// ============================================================================
// Плашки (badges)
// ============================================================================

QList<BadgeSpec> LogListView::collectBadgeSpecs(int row, const QString& text, 
    int availableWidth, bool multiLine, bool showCollapseBadge, int& hiddenCount) const {
    
    QList<BadgeSpec> specs;
    hiddenCount = 0;
    const AppTheme& theme = AppTheme::instance();

    // Информационная плашка файла.
    // Модель возвращает QVariant() если файл один — показывать нечего.
    // Там же хранится цвет, привязанный к файлу.
    QVariant badgeVar = model()->data(model()->index(row, 0), LogModel::FileBadgeRole);
    if (badgeVar.isValid()) {
        QVariantMap map = badgeVar.toMap();
        BadgeSpec b;
        b.type = BadgeType::Info;
        b.text  = map["text"].toString();
        b.bg    = map["color"].value<QColor>();
        b.fg    = theme.badgeFg;
        specs.append(b);
    }

    // Плашка скрытых символов для однострочного режима
    if (showCollapseBadge) {
        BadgeSpec b;
        b.type = BadgeType::HiddenToggle;
        b.text = QStringLiteral("-");
        b.bg = theme.badgeBg;
        b.fg = theme.badgeFg;
        specs.prepend(b);
    } else if (!multiLine) {
        // Ширина и точка обрезки считаются с учётом раскрытия табуляции.
        int textWidth = calculateWidth(QStringView(text));

        if (textWidth > availableWidth && availableWidth > 0) {
            int visibleChars = getCharIndexAt(QStringView(text), availableWidth);
            visibleChars = qMin(visibleChars, static_cast<int>(text.length()));
            hiddenCount = text.length() - visibleChars;
            
            if (hiddenCount > 0) {
                BadgeSpec b;
                b.type = BadgeType::HiddenToggle;
                b.text = QStringLiteral("+") + QString::number(hiddenCount);
                b.bg = theme.badgeBg;
                b.fg = theme.badgeFg;
                specs.prepend(b);
            }
        }
    }

    return specs;
}

QList<BadgeLayout> LogListView::layoutBadges(const QList<BadgeSpec>& specs, int rowHeight) const {
    QList<BadgeLayout> layouts;
    if (specs.isEmpty()) return layouts;

    QFontMetrics fm(font());
    int vpWidth = viewport()->width();
    int x = vpWidth - kBadgeGap;
    
    for (int i = specs.size() - 1; i >= 0; --i) {
        const auto& spec = specs[i];
        int w = fm.horizontalAdvance(spec.text) + kBadgeHPadding;
        int h = fm.height() - 2;
        QRect r(x - w + 1, rowHeight / 2 - h / 2, w, h);
        x = r.left() - kBadgeGap;
        layouts.prepend({spec, r});
    }
    return layouts;
}

// ============================================================================
// Хелперы для позиционирования
// ============================================================================

bool LogListView::getRowAtContentY(int contentY, int& row, int& rowTop) const {
    row = -1;
    rowTop = 0;
    const int rows = model() ? model()->rowCount() : 0;
    if (rows == 0 || contentY < 0) return false;

    int r = rowAtY(contentY);
    if (r < 0 || r >= rows) return false;

    int yOff = rowYOffset(r);
    // Verify contentY is within this row (guards against contentY past end of content)
    if (contentY >= yOff + getRowHeight(r)) return false;

    row    = r;
    rowTop = yOff;
    return true;
}

int LogListView::getFragmentIndexForPosition(int pos, const QList<TextFragment>& fragments) const {
    if (fragments.isEmpty()) return -1;

    for (int i = 0; i < fragments.size(); ++i) {
        const auto& fragment = fragments[i];
        if (pos >= fragment.startPos && pos <= fragment.startPos + fragment.length) {
            return i;
        }
    }
    // Если позиция за пределами, выбираем ближайший край
    if (pos < fragments.first().startPos) return 0;
    return fragments.size() - 1;
}

QList<TextFragment> LogListView::splitTextIntoLines(const QString& text, const QRect& rect) const {
    // Разбиение по визуальным КОЛОНКАМ: табуляция занимает несколько колонок,
    // поэтому перенос считается по фактической ширине, а не по числу символов.
    // Каждый фрагмент меряется от своего начала (колонка 0) — так отрисовка,
    // hit-test и выделение фрагмента остаются согласованными.
    QList<TextFragment> fragments;

    int lineWidth = rect.width() - 2 * kTextPaddingX;
    if (lineWidth <= 0) lineWidth = 100;

    const int colsPerLine = qMax(1, (int)(lineWidth / m_charWidth));
    const QStringView view(text);
    int y = rect.top() + 1;

    // Перенос по словам: правило границ инкапсулировано в LineWrapper, здесь —
    // только геометрия фрагментов. Каждый фрагмент меряется от своего начала
    // (колонка 0), что согласуется с отрисовкой, hit-test и выделением.
    m_lineWrapper.wrap(view, colsPerLine, [&](int start, int length) {
        TextFragment fragment;
        fragment.startPos = start;
        fragment.length = length;
        fragment.text = view.sliced(start, length);
        fragment.rect = QRect(rect.left() + kTextPaddingX, y, lineWidth, m_lineHeight);
        fragments.append(fragment);
        y += m_lineHeight;
    });
    
    // Минимум одна строка даже для пустого текста
    if (fragments.isEmpty()) {
        TextFragment fragment;
        fragment.startPos = 0;
        fragment.length = 0;
        fragment.text = QStringView();
        fragment.rect = QRect(rect.left() + kTextPaddingX, rect.top() + 1, lineWidth, m_lineHeight);
        fragments.append(fragment);
    }
    
    return fragments;
}

// Универсальная функция для определения позиции текста по координатам мыши
// Использует кэшированные фрагменты из RowState
int LogListView::getTextPositionFromMouse(const QPoint& mousePos, const RowState& state) const {
    if (state.multiLine) {
        const auto& fragments = state.fragments;
        if (fragments.isEmpty()) return 0;

        const auto& firstFrag = fragments.first();
        const auto& lastFrag = fragments.last();
        QRect firstRect = firstFrag.rect;
        QRect lastRect = lastFrag.rect;

        if (mousePos.y() < firstRect.top()) {
            return firstFrag.startPos;
        }
        if (mousePos.y() > lastRect.bottom()) {
            return lastFrag.startPos + lastFrag.length;
        }

        for (const auto& fragment : fragments) {
            QRect fragRect = fragment.rect;
            if (mousePos.y() >= fragRect.top() && mousePos.y() <= fragRect.bottom()) {
                // Переходим из viewport-X в text-local-X через viewportToTextX()
                int x = viewportToTextX(mousePos.x()) - fragRect.left();
                int closest = getCharIndexAt(fragment.text, x);
                return fragment.startPos + closest;
            }
        }

        return state.text.length();
    } else {
        // Однострочный режим — ограничиваем выделение видимыми символами
        // Переходим из viewport-X в text-local-X, затем вычитаем 4px внутренний отступ текста
        int x = viewportToTextX(mousePos.x()) - kTextPaddingX;
        int pos = getCharIndexAt(QStringView(state.text), x);
        // Ограничиваем позицию количеством видимых символов
        return qMin(pos, state.visibleChars);
    }
}

void LogListView::setTextHighlightPatterns(const QVector<HighlightPattern>& patterns)
{
    m_textHighlighter.setPatterns(patterns);
    // Заливки совпадений запечены в пиксмапы строк — сбрасываем кэш растров,
    // кэш геометрии (RowState) не зависит от подсветки и остаётся валидным.
    m_rowPixmapCache.clear();
    viewport()->update();
}

void LogListView::showSearchMatch(int row, const QString& term, bool caseSensitive)
{
    if (row < 0 || term.isEmpty() || !model() || row >= model()->rowCount()) {
        clearSearchMatch();
        return;
    }

    // Снимаем подсветку с предыдущей найденной строки.
    if (m_searchMatchRow >= 0 && m_searchMatchRow != row)
        m_rowPixmapCache.remove(m_searchMatchRow);

    m_searchMatchRow = row;
    HighlightPattern pattern;
    pattern.text            = term;
    pattern.color           = AppTheme::instance().searchMatch;
    pattern.caseSensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    m_searchHighlighter.setPatterns({pattern});

    // Свёрнутую строку раскрываем в многострочный режим, чтобы были видны
    // все вхождения (включая попавшие в скрытую часть). toggleRowMultiLine
    // сам пересчитает высоты и сохранит позицию скролла.
    if (!isRowMultiLine(row))
        toggleRowMultiLine(row);

    m_rowPixmapCache.remove(row);
    viewport()->update();
}

void LogListView::clearSearchMatch()
{
    if (m_searchMatchRow < 0)
        return;
    m_rowPixmapCache.remove(m_searchMatchRow);
    m_searchMatchRow = -1;
    m_searchHighlighter.setPatterns({});
    viewport()->update();
}

// Фоновые заливки совпадений. Геометрия повторяет drawSelectionHighlight:
// однострочный режим клипуется по visibleChars, многострочный идёт по фрагментам.
void LogListView::drawMatchHighlightSpans(QPainter& painter, const QRect& rect, const QString& text,
                                          const RowState& state, const QVector<HighlightSpan>& spans) const
{
    for (const auto& span : spans) {
        int spanBegin = span.start;
        int spanFinish = span.start + span.length;

        if (!state.multiLine) {
            spanBegin  = qMin(spanBegin,  state.visibleChars);
            spanFinish = qMin(spanFinish, state.visibleChars);
            if (spanBegin >= spanFinish)
                continue;
            const QStringView textView(text);
            const int x1 = rect.left() + kTextPaddingX + textWidthUntil(textView, spanBegin);
            const int x2 = rect.left() + kTextPaddingX + textWidthUntil(textView, spanFinish);
            painter.fillRect(x1, rect.top() + 1, x2 - x1, rect.height() - 2, span.color);
            continue;
        }

        for (const auto& fragment : state.fragments) {
            const int fragSelStart = qMax(spanBegin,  fragment.startPos) - fragment.startPos;
            const int fragSelEnd   = qMin(spanFinish, fragment.startPos + fragment.length) - fragment.startPos;
            if (fragSelEnd <= fragSelStart)
                continue;
            const int x1 = rect.left() + fragment.rect.left() + textWidthUntil(fragment.text, fragSelStart);
            const int x2 = rect.left() + fragment.rect.left() + textWidthUntil(fragment.text, fragSelEnd);
            const int y  = rect.top()  + fragment.rect.top();
            painter.fillRect(x1, y, x2 - x1, fragment.rect.height(), span.color);
        }
    }
}

// Основная отрисовка строки лога с подсветкой чисел и строк
// Использует кэшированные фрагменты из RowState
void LogListView::drawLogLine(QPainter& painter, const QRect& rect, const QString& text, const RowState& state) {
    // Заливки совпадений — до текста, чтобы существующая раскраска
    // SyntaxHighlighter рисовалась поверх и не менялась.
    if (!m_textHighlighter.isEmpty()) {
        const QVector<HighlightSpan> spans = m_textHighlighter.computeSpans(text);
        if (!spans.isEmpty())
            drawMatchHighlightSpans(painter, rect, text, state, spans);
    }

    // Подсветка результата поиска — только в найденной строке.
    if (state.row == m_searchMatchRow && !m_searchHighlighter.isEmpty()) {
        const QVector<HighlightSpan> spans = m_searchHighlighter.computeSpans(text);
        if (!spans.isEmpty())
            drawMatchHighlightSpans(painter, rect, text, state, spans);
    }

    QList<HighlightToken> tokens = SyntaxHighlighter::tokenize(text);

    if (state.multiLine) {
        for (const auto& fragment : state.fragments) {
            // Корректируем X: фрагменты хранятся в локальных координатах пикселя (0-based),
            // rect.left() добавляет смещение до текстовой области (kLeftMarginWidth или 0 в пикселе)
            int x = rect.left() + fragment.rect.left();
            int baseY = rect.top() + fragment.rect.top() + fontMetrics().height() - fontMetrics().descent();
            drawTextWithHighlights(painter, x, baseY, fragment.text, fragment.startPos, tokens);
        }
    } else {
        // Однострочный режим — рисуем только видимые символы
        int x = rect.left() + kTextPaddingX;
        int baseY = rect.bottom() - fontMetrics().descent() - 1;
        QStringView visibleText = QStringView(text).left(state.visibleChars);
        drawTextWithHighlights(painter, x, baseY, visibleText, 0, tokens);
    }
}

// Подсветка выделенного текста внутри строки
// Использует кэшированные фрагменты из RowState
void LogListView::drawSelectionHighlight(QPainter& painter, const QRect& rect, const QString& text, int selStart, int selEnd, const RowState& state) {
    int selBegin = std::min(selStart, selEnd);
    int selFinish = std::max(selStart, selEnd);

    if (!state.multiLine) {
        // Ограничиваем выделение видимыми символами
        selBegin = qMin(selBegin, state.visibleChars);
        selFinish = qMin(selFinish, state.visibleChars);
        if (selBegin == selFinish) return;
        
        QStringView textView(text);
        int x1 = textWidthUntil(textView, selBegin) + kTextPaddingX;
        int x2 = textWidthUntil(textView, selFinish) + kTextPaddingX;
        QRect selRect(rect.left() + x1, rect.top() + 1, x2 - x1, rect.height() - 2);
        painter.fillRect(selRect, AppTheme::instance().selectionFill);
        return;
    }

    const auto& fragments = state.fragments;
    if (fragments.isEmpty()) return;

    int startFrag = 0;
    int endFrag = fragments.size() - 1;
    for (int i = 0; i < fragments.size(); ++i) {
        const auto& f = fragments[i];
        if (selBegin >= f.startPos && selBegin <= f.startPos + f.length) {
            startFrag = i;
        }
        if (selFinish >= f.startPos && selFinish <= f.startPos + f.length) {
            endFrag = i;
            break;
        }
    }

    for (int i = startFrag; i <= endFrag; ++i) {
        const auto& fragment = fragments[i];

        int fragSelStart = (i == startFrag) ? selBegin - fragment.startPos : 0;
        int fragSelEnd = (i == endFrag) ? selFinish - fragment.startPos : fragment.length;

        fragSelStart = std::max(0, fragSelStart);
        fragSelEnd = std::min(fragment.length, fragSelEnd);

        if (fragSelEnd > fragSelStart) {
            // rect.left() = kLeftMarginWidth (смещение текстовой области от края viewport),
            // fragment.rect.left() = 4 (отступ текста внутри пикселя).
            int x1 = rect.left() + textWidthUntil(fragment.text, fragSelStart) + fragment.rect.left();
            int x2 = rect.left() + textWidthUntil(fragment.text, fragSelEnd) + fragment.rect.left();
            int fragY = rect.top() + fragment.rect.top();
            QRect selRect(x1, fragY, x2 - x1, fragment.rect.height());
            painter.fillRect(selRect, AppTheme::instance().selectionFill);
        }
    }
}

// Подсветка найденной парной скобки (рисуется поверх кэшированного растра строки)
void LogListView::drawBracketHighlight(QPainter& painter, const QRect& textRect, const RowState& state) const
{
    const int pos = m_bracketMatch.pos;
    const QColor& color = AppTheme::instance().bracketMatch;

    if (!state.multiLine) {
        if (pos >= state.visibleChars) return;
        const QStringView text(state.text);
        const int x1 = textRect.left() + kTextPaddingX + textWidthUntil(text, pos);
        const int x2 = textRect.left() + kTextPaddingX + textWidthUntil(text, pos + 1);
        painter.fillRect(x1, textRect.top() + 1, x2 - x1, textRect.height() - 2, color);
    } else {
        for (const auto& fragment : state.fragments) {
            if (pos < fragment.startPos || pos >= fragment.startPos + fragment.length)
                continue;
            const int relPos = pos - fragment.startPos;
            const int x1 = textRect.left() + fragment.rect.left() + textWidthUntil(fragment.text, relPos);
            const int x2 = textRect.left() + fragment.rect.left() + textWidthUntil(fragment.text, relPos + 1);
            const int y  = textRect.top()  + fragment.rect.top();
            painter.fillRect(x1, y, x2 - x1, fragment.rect.height(), color);
            break;
        }
    }
}

// Пересчитывает m_bracketMatch по текущему выделению.
// Если выделена открывающая скобка — ищет парную закрывающую вперёд.
// Если выделена закрывающая скобка — ищет парную открывающую назад.
void LogListView::updateBracketMatch()
{
    m_bracketMatch.clear();

    // Нас интересует ровно один символ, выделенный на одной строке
    if (!m_selection.isValid() || m_selection.isEmpty() || m_selection.isMultiRow())
        return;
    if (std::abs(m_selection.anchorPos - m_selection.activePos) != 1)
        return;
    if (!model()) return;

    const int selRow = m_selection.anchorRow;
    const int selPos = m_selection.firstPos();
    const QString rowText = model()->data(model()->index(selRow, 0)).toString();
    if (selPos >= rowText.length()) return;

    const QChar selChar = rowText[selPos];

    // Определяем пару и направление поиска
    QChar openChar, closeChar;
    bool searchForward;
    switch (selChar.unicode()) {
        case '(': openChar = '('; closeChar = ')'; searchForward = true;  break;
        case '{': openChar = '{'; closeChar = '}'; searchForward = true;  break;
        case '<': openChar = '<'; closeChar = '>'; searchForward = true;  break;
        case '[': openChar = '['; closeChar = ']'; searchForward = true;  break;
        case ')': openChar = '('; closeChar = ')'; searchForward = false; break;
        case '}': openChar = '{'; closeChar = '}'; searchForward = false; break;
        case '>': openChar = '<'; closeChar = '>'; searchForward = false; break;
        case ']': openChar = '['; closeChar = ']'; searchForward = false; break;
        default: return;
    }

    constexpr int kMaxSearchRows = 2000;
    int depth = 1;

    if (searchForward) {
        // Поиск вперёд: найти парную закрывающую скобку
        for (int r = selRow; r <= selRow + kMaxSearchRows; ++r) {
            if (r >= model()->rowCount()) break;
            const QString text = (r == selRow) ? rowText
                                               : model()->data(model()->index(r, 0)).toString();
            const int startPos = (r == selRow) ? selPos + 1 : 0;
            for (int p = startPos; p < text.length(); ++p) {
                const QChar c = text[p];
                if      (c == openChar)  { ++depth; }
                else if (c == closeChar) {
                    if (--depth == 0) { m_bracketMatch.row = r; m_bracketMatch.pos = p; return; }
                }
            }
        }
    } else {
        // Поиск назад: найти парную открывающую скобку
        for (int r = selRow; r >= selRow - kMaxSearchRows; --r) {
            if (r < 0) break;
            const QString text = (r == selRow) ? rowText
                                               : model()->data(model()->index(r, 0)).toString();
            const int endPos = (r == selRow) ? selPos - 1 : text.length() - 1;
            for (int p = endPos; p >= 0; --p) {
                const QChar c = text[p];
                if      (c == closeChar) { ++depth; }
                else if (c == openChar)  {
                    if (--depth == 0) { m_bracketMatch.row = r; m_bracketMatch.pos = p; return; }
                }
            }
        }
    }
}

// Кэширование растров строк для ускорения отрисовки
QPixmap LogListView::getRowPixmap(int row, const RowState& state) {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return QPixmap();
    }
    
    // Проверяем кэш (pixmap кэш синхронизирован с RowState кэшем)
    if (m_rowPixmapCache.contains(row)) {
        return m_rowPixmapCache[row];
    }
    
    // Ограничиваем размер кэша — при переполнении сбрасываем целиком
    if (m_rowPixmapCache.size() >= MAX_CACHED_ROWS) {
        m_rowPixmapCache.clear();
    }
    
    // Создаем новый растр с высоким качеством
    int width = state.textRect.width();
    int height = state.height;
    qreal devicePixelRatio = devicePixelRatioF();
    
    QPixmap pixmap(width * devicePixelRatio, height * devicePixelRatio);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);
    
    QPainter painter(&pixmap);
    painter.setFont(font());
    
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    QRect rect(0, 0, width, height);
    drawLogLine(painter, rect, state.text, state);
    
    m_rowPixmapCache[row] = pixmap;
    
    return pixmap;
}

void LogListView::toggleRowMultiLine(int row) {
    if (row < 0) return;

    int preservedViewportOffset = 0;
    const bool keepRowAnchor = captureVisibleRowOffset(row, preservedViewportOffset);

    if (m_toggledRows.contains(row)) {
        m_toggledRows.remove(row);
    } else {
        m_toggledRows.insert(row);
    }

    invalidateRowState(row, /*preserveTextLengths=*/true);
    rebuildHeightCache();
    updateScrollbar();

    if (keepRowAnchor) {
        verticalScrollBar()->setValue(targetScrollValueForRowOffset(row, preservedViewportOffset));
    } else if (model() && row < model()->rowCount()) {
        scrollTo(model()->index(row, 0), EnsureVisible);
    }

    viewport()->update();
}

bool LogListView::hitTestBadge(const QList<BadgeLayout>& layouts, const QPoint& pos, int& badgeIndex) const {
    for (int i = 0; i < layouts.size(); ++i) {
        if (layouts[i].rect.contains(pos)) {
            badgeIndex = i;
            return true;
        }
    }
    return false;
}

// Основная отрисовка виджета
void LogListView::paintEvent(QPaintEvent *event) {
    // НЕ вызываем QListView::paintEvent(event) - полностью отключаем стандартную отрисовку
    
    QPainter painter(viewport());
    
    // Включаем сглаживание для качественной отрисовки
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    int rows = model() ? model()->rowCount() : 0;
    if (rows == 0) return;

    int scrollY = verticalScrollBar()->value();
    QRect visibleRect = event->rect();
    
    // O(log N) поиск первой видимой строки
    int firstVisibleRow = rowAtY(scrollY);
    int currentY = rowYOffset(firstVisibleRow) - scrollY;
    bool anyHeightChanged = false;
    for (int row = firstVisibleRow; row < rows; ++row) {
        // Получаем кэшированное состояние строки
        RowState state = getRowState(row);

        // Ленивое уточнение высот: если при первой отрисовке реальная высота
        // отличается от ранее сохранённой оценки — обновляем и перестраиваем prefix-суммы
        if (!m_uniformHeights && !m_heightsDirty && row < m_rowHeights.size()) {
            if (m_rowHeights[row] != state.height) {
                m_rowHeights[row] = state.height;
                anyHeightChanged = true;
            }
        }

        int rowHeight = state.height;
        int vpWidth = viewport()->width();
        QRect rect(0, currentY, vpWidth, rowHeight);

        if (currentY > visibleRect.bottom()) {
            break;
        }

        // Отрисовываем только если строка пересекается с областью обновления
        if (rect.intersects(visibleRect)) {
            QModelIndex idx = model()->index(row, 0);

            QRect textRect = textAreaScreenRect(currentY, rowHeight, state.badgesWidth);

            // Фон row-маркера (недеструктивная подсветка всей строки).
            // Рисуется первым: выделение строки имеет визуальный приоритет.
            const QVariant markerColor = model()->data(idx, LogModel::RowMarkerColorRole);
            if (markerColor.isValid()) {
                painter.fillRect(rect, markerColor.value<QColor>());
            }

            // Фон выделенной строки (на всю ширину, включая левый желобок)
            if (selectionModel() && selectionModel()->isSelected(idx)) {
                painter.fillRect(rect, AppTheme::instance().selectionRowBg);
            }

            // Левый желобок: маркер начала записи и любые будущие индикаторы
            paintGutter(painter, row, QRect(0, currentY, textAreaLeft(), rowHeight));

            // Кэшированный растр ИЛИ прямая отрисовка — клипаем к textRect, чтобы текст не залезал на плашки
            QPixmap rowPixmap = getRowPixmap(row, state);
            if (!rowPixmap.isNull()) {
                painter.save();
                painter.setClipRect(textRect);
                painter.drawPixmap(textRect.topLeft(), rowPixmap);
                painter.restore();
            } else {
                // Fallback на обычную отрисовку
                painter.save();
                painter.setClipRect(textRect);
                drawLogLine(painter, textRect, state.text, state);
                painter.restore();
            }
            
            // Если есть выделение текста - рисуем его поверх
            if (m_selection.containsRow(row) && !m_selection.isEmpty()) {
                int selStart = 0, selEnd = 0;
                m_selection.rangeForRow(row, state.text.length(), selStart, selEnd);
                if (selStart != selEnd)
                    drawSelectionHighlight(painter, textRect, state.text, selStart, selEnd, state);
            }

            // Подсветка найденной парной скобки
            if (m_bracketMatch.isValid() && m_bracketMatch.row == row)
                drawBracketHighlight(painter, textRect, state);

            // Рисуем плашки поверх (корректируем Y координаты относительно currentY)
            for (const auto& bl : state.badges) {
                QRect badgeRect = bl.rect;
                badgeRect.moveTop(badgeRect.top() + currentY);
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(bl.spec.bg);
                painter.drawRect(badgeRect);
                painter.setPen(bl.spec.fg);
                painter.drawText(badgeRect.adjusted(kBadgeHPadding/2, 0, -kBadgeHPadding/2, 0), Qt::AlignVCenter | Qt::AlignLeft, bl.spec.text);
                painter.restore();
            }
        }

        currentY += rowHeight;
    }

    // Если реальные высоты отличались от оценок — перестроим prefix-суммы через 50 мс
    if (anyHeightChanged && !m_heightUpdateTimer->isActive()) {
        m_heightUpdateTimer->start(50);
    }
}

void LogListView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    
    if (m_anchorRow < 0 && model() && model()->rowCount() > 0) {
        int scrollY = verticalScrollBar()->value();
        int rowTop = 0;
        if (getRowAtContentY(scrollY, m_anchorRow, rowTop)) {
            m_anchorOffsetInViewport = rowTop - scrollY;
        }
    }

    // Debounce: отложенный пересчёт после окончания resize
    m_resizeDebounceTimer->start(150);
}

void LogListView::enterEvent(QEnterEvent *event) {
    // Полностью игнорируем события входа мыши
}

void LogListView::leaveEvent(QEvent *event) {
    // Полностью игнорируем события выхода мыши
}

int LogListView::estimateTotalHeightForDirtyCache(int rows) const {
    const int singleRowH = singleRowHeight();
    if (rows <= 0) {
        return 0;
    }

    // В однострочном режиме точная формула O(1).
    if (!m_wordWrapEnabled && m_toggledRows.isEmpty()) {
        return singleRowH * rows;
    }

    // Для word-wrap нужен консервативный estimate, иначе scrollbar может исчезнуть
    // до первого точного пересчёта высот после reset/filter.
    const int estimatedWrappedRowH = singleRowH * 3;
    return estimatedWrappedRowH * rows;
}

void LogListView::updateScrollbar() {
    if (!model()) return;

    const int rows = model()->rowCount();
    if (rows == 0) {
        verticalScrollBar()->setRange(0, 0);
        return;
    }

    int totalHeight;
    if (!m_heightsDirty) {
        // Точное (или оценочное из кэша) значение — никакого обращения к модели
        totalHeight = m_totalHeight;
    } else {
        // Кэш ещё не построен — быстрая консервативная оценка без model()->data().
        // Для wrapped-строк нельзя использовать однострочную формулу, она занижает диапазон.
        totalHeight = estimateTotalHeightForDirtyCache(rows);
    }

    int maxScroll = std::max(0, totalHeight - viewport()->height());

    // Явно диктуем политику отображения скроллбара, чтобы разорвать
    // бесконечный цикл resizeEvent (ScrollBar Oscillation), который возникает,
    // когда базовый QListView думает, что скроллбар не нужен и прячет его, а мы — показываем.
    if (maxScroll > 0) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    } else {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    verticalScrollBar()->setRange(0, maxScroll);
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(singleRowHeight());
}

// Обработка нажатия мыши для выделения текста
void LogListView::mousePressEvent(QMouseEvent *event) {
    // Правая кнопка — не сбрасываем выделение, просто игнорируем (контекстное меню обработается через contextMenuEvent)
    if (event->button() == Qt::RightButton) {
        return;
    }
    
    // Средняя кнопка и прочие — передаем базовому классу
    if (event->button() != Qt::LeftButton) {
        QListView::mousePressEvent(event);
        return;
    }
    
    // === Лямбды-хелперы для структурирования логики ===

    // Получение информации о строке под курсором
    auto getRowHitInfoFromMouse = [this](const QPoint& viewportPos) -> RowHitInfo {
        RowHitInfo info;
        int scrollY = verticalScrollBar()->value();
        int contentY = viewportPos.y() + scrollY;

        if (getRowAtContentY(contentY, info.row, info.rowTop)) {
            info.rowHeight = getRowHeight(info.row);
            info.localPos = QPoint(viewportPos.x(), contentY - info.rowTop);
            info.valid = true;
        }
        return info;
    };

    // Обработка клика по плашке — возвращает true если клик обработан
    // localPos - координаты относительно начала строки (Y от 0 до rowHeight)
    auto handleBadgeClick = [this](int row, const QPoint& localPos) -> bool {
        const auto& state = getRowState(row);
        
        int badgeIndex = -1;
        if (hitTestBadge(state.badges, localPos, badgeIndex)) {
            const auto& spec = state.badges[badgeIndex].spec;
            if (spec.type == BadgeType::HiddenToggle) {
                toggleRowMultiLine(row);
            }
            emit badgeClicked(row, spec.type, spec.text);
            viewport()->update();
            return true;
        }
        return false;
    };

    // Начало выделения текста: устанавливаем якорь ДО вызова setCurrentIndex,
    // чтобы currentChanged не сбросил только что созданное выделение.
    auto startTextSelection = [this](int row, const QPoint& localPos) {
        const RowState& state = getRowState(row);
        const int pos = getTextPositionFromMouse(localPos, state);

        m_selection.start(row, pos);
        m_bracketMatch.clear();
        if (selectionModel()) {
            selectionModel()->setCurrentIndex(model()->index(row, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        viewport()->update();
    };

    // === Основная логика mousePressEvent ===

    // 1. Игнорируем если уже идёт выделение
    if (m_selection.isDragging) return;

    // 2. Определяем строку под курсором
    RowHitInfo hitInfo = getRowHitInfoFromMouse(event->pos());
    if (!hitInfo.valid) {
        QListView::mousePressEvent(event);
        return;
    }

    // 3. Проверяем клик по плашке (передаём локальные координаты)
    if (handleBadgeClick(hitInfo.row, hitInfo.localPos)) {
        return;
    }

    // 4. Начинаем выделение текста
    startTextSelection(hitInfo.row, hitInfo.localPos);
}

// Обновляет только изменившийся диапазон строк в viewport.
void LogListView::invalidateSelectionRange(int topRow, int bottomRow) {
    const int scrollY = verticalScrollBar()->value();
    const int topY    = rowYOffset(topRow) - scrollY;
    const int botY    = rowYOffset(bottomRow) + getRowHeight(bottomRow) - scrollY;
    viewport()->update(QRect(0, topY, viewport()->width(), botY - topY));
}

// Обработка движения мыши — расширяет выделение на несколько строк.
void LogListView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_selection.isDragging) return;

    const int scrollY  = verticalScrollBar()->value();
    const int contentY = event->pos().y() + scrollY;
    const int rowCount = model() ? model()->rowCount() : 0;
    if (rowCount == 0) return;

    // Определяем строку под курсором с зажимом на границах контента
    int newRow = -1, rowTop = 0;
    if (contentY < 0) {
        newRow = 0;
        rowTop = 0;
    } else if (!getRowAtContentY(contentY, newRow, rowTop)) {
        newRow = rowCount - 1;
        rowTop = rowYOffset(newRow);
    }

    const RowState& state   = getRowState(newRow);
    const QPoint    localPos(event->pos().x(), contentY - rowTop);
    const int       newPos  = getTextPositionFromMouse(localPos, state);

    if (newRow == m_selection.activeRow && newPos == m_selection.activePos) return;

    // Сохраняем старые границы для точечной перерисовки
    const int oldTopRow    = m_selection.topRow();
    const int oldBottomRow = m_selection.bottomRow();

    m_selection.moveTo(newRow, newPos);

    // Перерисовываем только строки, затронутые изменением
    const int dirtyTop    = std::min(oldTopRow,    m_selection.topRow());
    const int dirtyBottom = std::max(oldBottomRow, m_selection.bottomRow());
    invalidateSelectionRange(dirtyTop, dirtyBottom);
}

// Завершение выделения при отпускании кнопки мыши
void LogListView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_selection.isDragging) {
        m_selection.finish();
        updateBracketMatch();
        viewport()->update();
    }
    QListView::mouseReleaseEvent(event);
}


// Двойной клик мыши — умное выделение токена под курсором
void LogListView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    int scrollY  = verticalScrollBar()->value();
    int contentY = event->pos().y() + scrollY;
    int row = -1, rowTop = 0;
    if (!getRowAtContentY(contentY, row, rowTop)) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    const RowState& state    = getRowState(row);
    const QPoint    localPos = QPoint(event->pos().x(), contentY - rowTop);
    const int       clickPos = getTextPositionFromMouse(localPos, state);

    const auto [wordStart, wordEnd] = TextToken::findDoubleClickToken(state.text, clickPos);
    if (wordStart == wordEnd) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    m_selection.start(row, wordStart);
    m_selection.moveTo(row, wordEnd);
    m_selection.finish();
    updateBracketMatch();

    if (selectionModel()) {
        selectionModel()->setCurrentIndex(model()->index(row, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    viewport()->update();
}

// Обработка нажатия клавиш (копирование выделенного текста)
void LogListView::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy) && !m_selection.isEmpty()) {
        QStringList lines;
        for (int row = m_selection.topRow(); row <= m_selection.bottomRow(); ++row) {
            const QString rowText = model()->data(model()->index(row, 0)).toString();
            int selStart = 0, selEnd = 0;
            m_selection.rangeForRow(row, rowText.length(), selStart, selEnd);
            lines.append(rowText.mid(selStart, selEnd - selStart));
        }
        QApplication::clipboard()->setText(lines.join('\n'));
    } else {
        QListView::keyPressEvent(event);
    }
}

void LogListView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QListView::currentChanged(current, previous);
    // Во время drag-выделения anchorRow задан вручную — не сбрасывать.
    // При обычной навигации (клавиши, внешний setCurrentIndex) — сбрасываем текстовое выделение.
    if (!m_selection.isDragging && current.isValid() && current.row() != m_selection.anchorRow) {
        m_selection.clear();
        m_selection.anchorRow = current.row();
        m_selection.activeRow = current.row();
        updateBracketMatch();
        viewport()->update();
    }
}

void LogListView::updateGeometries() {
    if (m_inUpdateGeometries) return;
    m_inUpdateGeometries = true;

    int savedScrollY = verticalScrollBar()->value();
    QListView::updateGeometries();
    updateScrollbar();
    verticalScrollBar()->setValue(qBound(0, savedScrollY, verticalScrollBar()->maximum()));

    m_inUpdateGeometries = false;
}

// Предотвращаем исчезновение текста при наведении курсора
bool LogListView::viewportEvent(QEvent *event)
{
    // Блокируем все события hover
    switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverLeave:
        case QEvent::HoverMove:
        case QEvent::ToolTip:
            return true;
        default:
            break;
    }
    
    return QListView::viewportEvent(event);
}

// --- Private Helper Functions ---

void LogListView::drawTextWithHighlights(QPainter& painter, int x, int y, const QStringView& text, int fragmentStartPos, const QList<HighlightToken>& tokens) const {
    QColor defaultColor = palette().color(QPalette::Text);

    // Позиция начала и конца фрагмента в оригинальной строке
    int fragmentStart = fragmentStartPos;
    int fragmentEnd = fragmentStart + text.length();
    int last = 0;
    // baseX фиксирован — каждый сегмент вычисляет X от базы (в КОЛОНКАХ через
    // ColumnCursor), а не накапливает смещение. Это предотвращает накопление
    // ошибки округления при дробном m_charWidth и корректно учитывает табуляцию.
    const int baseX = x;

    // Курсор раскрытия табуляции: индексы сегментов запрашиваются монотонно,
    // поэтому весь проход — O(длина фрагмента). Сегмент рисуется expand()'ом
    // (табы → пробелы до табстопа), чтобы глифы попали ровно в колонки сетки.
    ColumnCursor cursor(text, m_tabExpander);

    // Проходим по всем токенам подсветки
    for (const auto& token : tokens) {
        // Проверяем, пересекается ли токен с текущим фрагментом
        if (token.end <= fragmentStart || token.start >= fragmentEnd) {
            continue; // Токен не пересекается с фрагментом
        }

        // Вычисляем пересечение токена с фрагментом
        int tokenStartInFragment = qMax(0, token.start - fragmentStart);
        int tokenEndInFragment = qMin(static_cast<int>(text.length()), token.end - fragmentStart);

        // Отрисовываем текст до токена
        if (tokenStartInFragment > last) {
            const int startCol = cursor.columnAt(last);
            QStringView before = text.sliced(last, tokenStartInFragment - last);
            painter.setPen(defaultColor);
            painter.drawText(baseX + qRound(startCol * m_charWidth), y, m_tabExpander.expand(before, startCol));
        }

        // Отрисовываем токен
        if (tokenEndInFragment > tokenStartInFragment) {
            const int startCol = cursor.columnAt(tokenStartInFragment);
            const int endCol   = cursor.columnAt(tokenEndInFragment);
            const int tx = baseX + qRound(startCol * m_charWidth);
            const int tw = qRound((endCol - startCol) * m_charWidth);
            // Заливка фона (поиск с подсветкой и пр.)
            if (token.bgColor.isValid()) {
                const QFontMetrics fm = painter.fontMetrics();
                painter.fillRect(tx, y - fm.ascent(), tw, fm.height(), token.bgColor);
            }
            QStringView tokenView = text.sliced(tokenStartInFragment, tokenEndInFragment - tokenStartInFragment);
            painter.setPen(token.color.isValid() ? token.color : defaultColor);
            painter.drawText(tx, y, m_tabExpander.expand(tokenView, startCol));
        }

        last = tokenEndInFragment;
    }

    // Отрисовываем остаток текста
    if (last < text.length()) {
        const int startCol = cursor.columnAt(last);
        QStringView rest = text.sliced(last);
        painter.setPen(defaultColor);
        painter.drawText(baseX + qRound(startCol * m_charWidth), y, m_tabExpander.expand(rest, startCol));
    }
}


// Пиксель → индекс символа. Перевод X в визуальную колонку (с учётом табуляции)
// и поиск ближайшей границы символа делегирован TabExpander — единственному
// источнику правила табстопа.
int LogListView::getCharIndexAt(const QStringView& text, int x) const {
    if (x <= 0) return 0;
    if (m_charWidth <= 0.0) return 0;
    return m_tabExpander.indexAtColumn(text, x / m_charWidth);
}

// Полная ширина текста в пикселях с учётом раскрытия табуляции.
int LogListView::calculateWidth(const QStringView& text) const {
    return qRound(m_tabExpander.columns(text) * m_charWidth);
}

// Ширина первых count символов в пикселях с учётом раскрытия табуляции.
int LogListView::textWidthUntil(const QStringView& text, int count) const {
    return qRound(m_tabExpander.columnAt(text, count) * m_charWidth);
}
