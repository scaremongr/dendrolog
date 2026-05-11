#include "LogListView.h"
#include "logmodel.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QClipboard>
#include <QApplication>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextLayout>
#include <QFontDatabase>
#include <QTimer>
#include <algorithm>
#include <limits>

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
    
    // Устанавливаем моноширинный шрифт (обязателен для оптимизации)
    // Выбираем шрифт с приоритетом: Cascadia Mono → Consolas → системный моноширинный
    // Оба хорошо читаются, имеют нормальный вес и плотный кернинг
    static const QStringList candidates = {
        QStringLiteral("Cascadia Mono"),
        QStringLiteral("Cascadia Code"),
        QStringLiteral("Consolas"),
    };
    const QStringList available = QFontDatabase::families();
    QFont monoFont;
    bool found = false;
    for (const QString& name : candidates) {
        if (available.contains(name, Qt::CaseInsensitive)) {
            monoFont = QFont(name);
            found = true;
            break;
        }
    }
    if (!found) {
        monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    monoFont.setPointSize(10);
    monoFont.setWeight(QFont::Medium);  // чуть жирнее стандартного — лучше читаемость
    setFont(monoFont);
    updateFontMetricsCache();
    
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
}

void LogListView::setModel(QAbstractItemModel *model) {
    QListView::setModel(model);
    invalidateRowState();
    if (model) {
        // Полная инвалидация при сбросе модели (фильтрация, setEntries и т.п.)
        // Запоминаем выделение ДО сброса модели.
        // ВАЖНО: используем m_selRow, а не selectionModel()->currentIndex() —
        // QItemSelectionModel подключён к modelAboutToBeReset раньше нас и к моменту
        // вызова нашего слота уже очищает currentIndex. m_selRow не зависит от selectionModel.
        connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
            m_pendingSelectionId = -1;
            if (m_selRow < 0) return;
            auto* logModel = qobject_cast<LogModel*>(this->model());
            if (!logModel) return;
            const auto& entries = logModel->filteredEntries();
            if (m_selRow < entries.size())
                m_pendingSelectionId = entries[m_selRow]->logicalEntryId;
        });

        connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            m_toggledRows.clear();
            invalidateRowState();
            rebuildHeightCache();

            // Восстанавливаем выделение по logicalEntryId.
            int restoreRow = -1;
            if (m_pendingSelectionId >= 0) {
                auto* logModel = qobject_cast<LogModel*>(this->model());
                if (logModel) {
                    const auto& entries = logModel->filteredEntries();
                    for (int i = 0; i < entries.size(); ++i) {
                        if (entries[i]->logicalEntryId == m_pendingSelectionId) {
                            restoreRow = i;
                            break;
                        }
                    }
                }
                m_pendingSelectionId = -1;
            }

            if (restoreRow >= 0 && selectionModel() && this->model()) {
                // setCurrentIndex эмитит currentChanged → QAbstractItemView::scrollTo() →
                // перезаписывает scrollbar. Поэтому updateScrollbar и прокрутку делаем
                // отложенно — они сработают после всех Qt-внутренних синхронных операций.
                selectionModel()->setCurrentIndex(
                    this->model()->index(restoreRow, 0),
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_selRow = restoreRow;
            }

            // Откладываем updateScrollbar чтобы перекрыть QListView::scrollTo,
            // который вызывается синхронно из currentChanged выше.
            QTimer::singleShot(0, this, [this, restoreRow]() {
                updateScrollbar();

                if (restoreRow >= 0 && this->model() && restoreRow < this->model()->rowCount()) {
                    int rowY = rowYOffset(restoreRow);
                    int rowH = getRowHeight(restoreRow);
                    int vpH  = viewport()->height();
                    int curScrollY = verticalScrollBar()->value();
                    if (rowY < curScrollY || rowY + rowH > curScrollY + vpH) {
                        int target = qBound(0, rowY - (vpH - rowH) / 2, verticalScrollBar()->maximum());
                        verticalScrollBar()->setValue(target);
                    }
                } else {
                    verticalScrollBar()->setValue(0);
                }

                viewport()->update();
            });
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

// Фиксированная высота строки для QListView — предотвращает O(N) вычисления внутри базового класса
int LogListView::sizeHintForRow(int) const {
    return m_lineHeight + 2;
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
        return m_lineHeight + 2;  // O(1) — однострочный режим
    }
    if (!m_heightsDirty && row >= 0 && row < m_rowHeights.size() && m_rowHeights[row] > 0) {
        return m_rowHeights[row];
    }
    return getRowState(row).height;
}

RowState LogListView::computeRowState(int row) const {
    RowState state;
    state.row = row;
    state.viewportWidth = viewport()->width();
    state.text = model()->data(model()->index(row, 0)).toString();
    m_rowTextLengths[row] = state.text.length();  // кэшируем длину — пережнвает resize
    state.multiLine = isRowMultiLine(row);
    
    // Шаг 1: Оцениваем ширину плашек для расчёта доступной ширины текста.
    // FileBadge: берём реальное имя файла (разное для каждой строки!) — это ключевое
    // для корректного вычисления visibleChars. HiddenToggle оцениваем по максимуму.
    QFontMetrics fm(font());
    int estimatedBadgesWidth = 0;
    QVariant fileBadgeVar = model()->data(model()->index(row, 0), LogModel::FileBadgeRole);
    if (fileBadgeVar.isValid()) {
        estimatedBadgesWidth += fm.horizontalAdvance(fileBadgeVar.toMap()["text"].toString()) + 10 + 4;
    }
    int toggleBadgeWidth = fm.horizontalAdvance("+9999") + 10 + 4; // максимальная оценка для HiddenToggle
    estimatedBadgesWidth += toggleBadgeWidth;
    
    int textBadgeGap = 8;
    int availableTextWidth = state.viewportWidth - estimatedBadgesWidth - textBadgeGap;
    
    // Шаг 2: Вычисляем высоту с учётом реальной доступной ширины
    state.height = computeRowHeight(state.text, state.multiLine, availableTextWidth);
    
    // Шаг 3: Собираем плашки с точным расчётом скрытых символов
    QList<BadgeSpec> badgeSpecs = collectBadgeSpecs(row, state.text, 
        availableTextWidth, state.multiLine, state.hiddenChars);
    state.badges = layoutBadges(badgeSpecs, state.height);
    
    // Шаг 4: Вычисляем реальную ширину плашек
    state.badgesWidth = 0;
    for (const auto& b : state.badges) {
        state.badgesWidth += b.rect.width() + 4;
    }
    
    // Шаг 5: Область для текста (с зазором перед плашками)
    state.textRect = QRect(0, 0, state.viewportWidth - state.badgesWidth - textBadgeGap, state.height);
    
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
        return m_lineHeight + 2;
    }
    
    // Для моноширинного шрифта — простая арифметика O(1)
    int lineWidth = (availableWidth > 0) ? availableWidth - 8 : viewport()->width() - 8;
    if (lineWidth <= 0) lineWidth = 100;  // fallback
    
    int charsPerLine = qMax(1, (int)(lineWidth / m_charWidth));
    int lineCount = (text.length() + charsPerLine - 1) / charsPerLine;
    if (lineCount < 1) lineCount = 1;
    
    return lineCount * m_lineHeight + 2;
}

// ============================================================================
// Кэш высот строк — O(log N) поиск по Y-позиции
// ============================================================================

void LogListView::rebuildHeightCache() {
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

    const int singleRowH = m_lineHeight + 2;

    // БЫСТРЫЙ ПУТЬ: все строки однострочные — O(1), никакого обращения к модели
    if (!m_wordWrapEnabled && m_toggledRows.isEmpty()) {
        m_rowHeights.clear();   // не нужны — rowYOffset/rowAtY используют формулу
        m_rowPrefixY.clear();
        m_totalHeight = singleRowH * rows;
        m_uniformHeights = true;
        m_heightsDirty = false;
        return;
    }

    // МНОГОСТРОЧНЫЙ ПУТЬ: никакого прямого обращения к model()->data() в цикле.
    // Источники данных (в порядке приоритета):
    //   1. Кэшированное состояние строки (точная высота)
    //   2. Кэшированная длина текста m_rowTextLengths (точная высота, нет обращений к модели)
    //   3. Средняя высота уже известных строк (оценка; уточняется лениво при отрисовке)
    m_uniformHeights = false;
    m_rowHeights.resize(rows);

    const int textBadgeGap = 8;
    const int vpWidth = viewport()->width();
    const int toggleDashW = QFontMetrics(font()).horizontalAdvance(QChar('-')) + 10 + 4;
    const bool vpCacheValid = (m_cachedViewportWidth == vpWidth);

    // Средняя высота из уже посчитанных многострочных строк (для оценки остальных)
    int sumH = 0, countH = 0;
    for (const auto& st : std::as_const(m_rowStateCache)) {
        if (st.multiLine) { sumH += st.height; ++countH; }
    }
    const int estimatedMLH = (countH > 0) ? sumH / countH : singleRowH * 3;

    for (int i = 0; i < rows; ++i) {
        if (!isRowMultiLine(i)) {
            m_rowHeights[i] = singleRowH;
        } else if (vpCacheValid && m_rowStateCache.contains(i)) {
            // Точная высота из кэша состояний (viewport не менялся)
            m_rowHeights[i] = m_rowStateCache[i].height;
        } else if (m_rowTextLengths.contains(i)) {
            // Вычисляем из кэшированной длины текста — без обращения к модели
            int lineWidth = vpWidth - toggleDashW - textBadgeGap - 8;
            if (lineWidth <= 0) lineWidth = 100;
            const int charsPerLine = qMax(1, (int)(lineWidth / m_charWidth));
            const int lineCount = qMax(1, (m_rowTextLengths[i] + charsPerLine - 1) / charsPerLine);
            m_rowHeights[i] = lineCount * m_lineHeight + 2;
        } else {
            // Нет данных — оценка, уточнится при первой отрисовке строки
            m_rowHeights[i] = estimatedMLH;
        }
    }

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
        return row * (m_lineHeight + 2);  // O(1) — однострочный режим
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
        const int h = m_lineHeight + 2;
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

// ============================================================================
// Плашки (badges)
// ============================================================================

QList<BadgeSpec> LogListView::collectBadgeSpecs(int row, const QString& text, 
    int availableWidth, bool multiLine, int& hiddenCount) const {
    
    QList<BadgeSpec> specs;
    hiddenCount = 0;

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
        b.fg    = Qt::white;
        specs.append(b);
    }

    // Плашка скрытых символов для однострочного режима
    if (!multiLine) {
        // Для моноширинного шрифта — точный расчёт O(1)
        int textWidth = qRound(text.length() * m_charWidth);
        
        if (textWidth > availableWidth && availableWidth > 0) {
            int visibleChars = qMax(0, (int)(availableWidth / m_charWidth));
            visibleChars = qMin(visibleChars, static_cast<int>(text.length()));
            hiddenCount = text.length() - visibleChars;
            
            if (hiddenCount > 0) {
                BadgeSpec b;
                b.type = BadgeType::HiddenToggle;
                b.text = QStringLiteral("+") + QString::number(hiddenCount);
                b.bg = QColor(0, 120, 215);
                b.fg = QColor(Qt::white);
                specs.prepend(b);
            }
        }
    } else {
        // Многострочный — показываем кнопку "-"
        BadgeSpec b;
        b.type = BadgeType::HiddenToggle;
        b.text = QStringLiteral("-");
        b.bg = QColor(0, 120, 215);
        b.fg = QColor(Qt::white);
        specs.prepend(b);
    }

    return specs;
}

QList<BadgeLayout> LogListView::layoutBadges(const QList<BadgeSpec>& specs, int rowHeight) const {
    QList<BadgeLayout> layouts;
    if (specs.isEmpty()) return layouts;

    QFontMetrics fm(font());
    int gap = 4;
    int vpWidth = viewport()->width();
    int x = vpWidth - gap;
    
    for (int i = specs.size() - 1; i >= 0; --i) {
        const auto& spec = specs[i];
        int w = fm.horizontalAdvance(spec.text) + 10;
        int h = fm.height() - 2;
        QRect r(x - w + 1, rowHeight / 2 - h / 2, w, h);
        x = r.left() - gap;
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
    // Для моноширинного шрифта — простое разбиение по количеству символов
    QList<TextFragment> fragments;
    
    int lineWidth = rect.width() - 8;
    if (lineWidth <= 0) lineWidth = 100;
    
    int charsPerLine = qMax(1, (int)(lineWidth / m_charWidth));
    int pos = 0;
    int y = rect.top() + 1;
    int textLen = text.length();
    
    while (pos < textLen) {
        int lineLen = qMin(charsPerLine, textLen - pos);
        
        TextFragment fragment;
        fragment.startPos = pos;
        fragment.length = lineLen;
        fragment.text = QStringView(text).sliced(pos, lineLen);
        fragment.rect = QRect(rect.left() + 4, y, lineWidth, m_lineHeight);
        fragments.append(fragment);
        
        pos += lineLen;
        y += m_lineHeight;
    }
    
    // Минимум одна строка даже для пустого текста
    if (fragments.isEmpty()) {
        TextFragment fragment;
        fragment.startPos = 0;
        fragment.length = 0;
        fragment.text = QStringView();
        fragment.rect = QRect(rect.left() + 4, rect.top() + 1, lineWidth, m_lineHeight);
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
                int x = mousePos.x() - fragRect.left();
                int closest = getCharIndexAt(fragment.text, x);
                return fragment.startPos + closest;
            }
        }

        return state.text.length();
    } else {
        // Однострочный режим — ограничиваем выделение видимыми символами
        int x = mousePos.x() - 4;
        int pos = getCharIndexAt(QStringView(state.text), x);
        // Ограничиваем позицию количеством видимых символов
        return qMin(pos, state.visibleChars);
    }
}

// Основная отрисовка строки лога с подсветкой чисел и строк
// Использует кэшированные фрагменты из RowState
void LogListView::drawLogLine(QPainter& painter, const QRect& rect, const QString& text, const RowState& state) {
    QList<HighlightToken> tokens = findHighlightTokens(text);
    
    if (state.multiLine) {
        for (const auto& fragment : state.fragments) {
            // Корректируем Y координаты относительно rect
            int x = fragment.rect.left();
            int baseY = rect.top() + fragment.rect.top() + fontMetrics().height() - fontMetrics().descent();
            drawTextWithHighlights(painter, x, baseY, fragment.text, fragment.startPos, tokens);
        }
    } else {
        // Однострочный режим — рисуем только видимые символы
        int x = rect.left() + 4;
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
        int x1 = textWidthUntil(textView, selBegin) + 4;
        int x2 = textWidthUntil(textView, selFinish) + 4;
        QRect selRect(rect.left() + x1, rect.top() + 1, x2 - x1, rect.height() - 2);
        painter.fillRect(selRect, QColor(0, 120, 215, 120));
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
            // Корректируем Y координаты относительно rect
            int x1 = textWidthUntil(fragment.text, fragSelStart) + fragment.rect.left();
            int x2 = textWidthUntil(fragment.text, fragSelEnd) + fragment.rect.left();
            int fragY = rect.top() + fragment.rect.top();
            QRect selRect(x1, fragY, x2 - x1, fragment.rect.height());
            painter.fillRect(selRect, QColor(0, 120, 215, 120));
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

    if (m_toggledRows.contains(row)) {
        m_toggledRows.remove(row);
    } else {
        m_toggledRows.insert(row);
    }

    invalidateRowState(row, /*preserveTextLengths=*/true);
    rebuildHeightCache();
    updateScrollbar();
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

            // Область текста (относительно экрана)
            QRect textRect = rect.adjusted(0, 0, -state.badgesWidth - 8, 0);

            // Фон выделенной строки
            if (selectionModel() && selectionModel()->isSelected(idx)) {
                painter.fillRect(rect, QColor(0, 120, 215, 60));
            }

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
            if (row == m_selRow && m_selStart >= 0 && m_selEnd >= 0 && m_selStart != m_selEnd) {
                drawSelectionHighlight(painter, textRect, state.text, m_selStart, m_selEnd, state);
            }

            // Рисуем плашки поверх (корректируем Y координаты относительно currentY)
            for (const auto& bl : state.badges) {
                QRect badgeRect = bl.rect;
                badgeRect.moveTop(badgeRect.top() + currentY);
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(bl.spec.bg);
                painter.drawRect(badgeRect);
                painter.setPen(bl.spec.fg);
                painter.drawText(badgeRect.adjusted(5, 0, -5, 0), Qt::AlignVCenter | Qt::AlignLeft, bl.spec.text);
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
        // Кэш ещё не построен — быстрая оценка без model()->data()
        totalHeight = (m_lineHeight + 2) * rows;
    }

    verticalScrollBar()->setRange(0, std::max(0, totalHeight - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(m_lineHeight + 2);
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

    // Начало выделения текста
    auto startTextSelection = [this](int row, const QPoint& localPos) {
        const auto& state = getRowState(row);

        m_selRow = row;
        m_selStart = getTextPositionFromMouse(localPos, state);
        m_selEnd = m_selStart;

        // Обновляем фрагменты для многострочного режима
        if (state.multiLine) {
            int fragIndex = getFragmentIndexForPosition(m_selStart, state.fragments);
            m_selStartFragment = fragIndex;
            m_selEndFragment = fragIndex;
        } else {
            m_selStartFragment = -1;
            m_selEndFragment = -1;
        }

        m_selecting = true;
        if (selectionModel()) {
            selectionModel()->select(model()->index(row, 0),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        }
        viewport()->update();
    };

    // === Основная логика mousePressEvent ===

    // 1. Игнорируем если уже идёт выделение
    if (m_selecting) return;

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

// Обработка движения мыши для выделения текста (только в рамках одной строки)
void LogListView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_selecting || m_selRow < 0) {
        // Полностью игнорируем обычное движение мыши
        return;
    }

    int oldSelEnd = m_selEnd;
    
    // Быстрый путь: используем кэшированные данные без полного getRowState
    int scrollY = verticalScrollBar()->value();
    int contentY = event->pos().y() + scrollY;

    // Находим верхнюю координату текущей строки
    int rowTop = 0;
    int row = -1;
    if (!getRowAtContentY(contentY, row, rowTop) || row != m_selRow) {
        // Курсор вышел за пределы строки — O(1) по кэшу
        rowTop = rowYOffset(m_selRow);
    }

    // Получаем состояние строки (из кэша, т.к. уже был вызван в mousePressEvent)
    RowState state = getRowState(m_selRow);
    
    QPoint localPos(event->pos().x(), contentY - rowTop);
    m_selEnd = getTextPositionFromMouse(localPos, state);

    // Обновляем фрагмент для многострочного режима
    if (state.multiLine) {
        m_selEndFragment = getFragmentIndexForPosition(m_selEnd, state.fragments);
    }

    // Обновляем только если выделение действительно изменилось
    if (m_selEnd != oldSelEnd) {
        int rowY = rowTop - scrollY;
        viewport()->update(QRect(0, rowY, viewport()->width(), state.height));
    }
}

// Сброс состояния выделения при отпускании кнопки мыши
void LogListView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_selecting) {
        m_selecting = false;
        // Полная перерисовка с подсветкой синтаксиса после завершения выделения
        viewport()->update();
    }
    QListView::mouseReleaseEvent(event);
}

// Двойной клик мыши — выделение слова под курсором
void LogListView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    int scrollY = verticalScrollBar()->value();
    int contentY = event->pos().y() + scrollY;
    int row = -1;
    int rowTop = 0;

    if (!getRowAtContentY(contentY, row, rowTop)) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    m_selRow = row;
    const auto& state = getRowState(row);
    const QString& text = state.text;

    QPoint localPos(event->pos().x(), contentY - rowTop);
    int clickPos = getTextPositionFromMouse(localPos, state);

    // Находим границы слова
    int wordStart = clickPos;
    int wordEnd = clickPos;

    while (wordStart > 0 && !text[wordStart - 1].isSpace() && text[wordStart - 1] != '\t') {
        wordStart--;
    }
    while (wordEnd < text.length() && !text[wordEnd].isSpace() && text[wordEnd] != '\t') {
        wordEnd++;
    }

    // Если клик по пробелу — выберем ближайшее слово справа
    if (wordStart == wordEnd) {
        while (wordEnd < text.length() && (text[wordEnd].isSpace() || text[wordEnd] == '\t')) {
            wordEnd++;
        }
        wordStart = wordEnd;
        while (wordEnd < text.length() && !text[wordEnd].isSpace() && text[wordEnd] != '\t') {
            wordEnd++;
        }
    }

    m_selStart = wordStart;
    m_selEnd = wordEnd;

    // Обновляем фрагменты для многострочного режима
    if (state.multiLine) {
        m_selStartFragment = getFragmentIndexForPosition(m_selStart, state.fragments);
        m_selEndFragment = getFragmentIndexForPosition(m_selEnd, state.fragments);
    } else {
        m_selStartFragment = -1;
        m_selEndFragment = -1;
    }

    m_selecting = false; // Двойной клик завершает выделение сразу
    
    if (selectionModel()) {
        selectionModel()->select(model()->index(row, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    viewport()->update();
    // НЕ вызываем QListView::mouseDoubleClickEvent - мы полностью обработали событие
}

// Обработка нажатия клавиш (копирование выделенного текста)
void LogListView::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::Copy) && m_selRow >= 0 && m_selStart != m_selEnd) {
        int start = std::min(m_selStart, m_selEnd);
        int len = std::abs(m_selEnd - m_selStart);
        QString text = model()->data(model()->index(m_selRow, 0)).toString().mid(start, len);
        QApplication::clipboard()->setText(text);
    } else {
        QListView::keyPressEvent(event);
    }
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

QList<HighlightToken> LogListView::findHighlightTokens(const QString& text) const {
    QList<HighlightToken> tokens;

    // static: регулярное выражение компилируется один раз
    static const QRegularExpression re(R"((['"])(?:(?!\1|\\).|\\.)*\1|(?<![\w])(0x[0-9a-fA-F]+|\d+(?:[^\w\s]\d+)*))");

    QRegularExpressionMatchIterator it = re.globalMatch(text);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        HighlightToken token;
        token.start = match.capturedStart();
        token.end = match.capturedEnd();
        
        QString matchedText = match.captured();
        if (matchedText.startsWith('\'') || matchedText.startsWith('"')) {
            token.color = QColor(200, 180, 0); // Желтый для строк
        } else {
            token.color = Qt::darkGreen; // Зеленый для чисел
        }
        
        tokens.append(token);
    }
    
    return tokens;
}

void LogListView::drawTextWithHighlights(QPainter& painter, int x, int y, const QStringView& text, int fragmentStartPos, const QList<HighlightToken>& tokens) const {
    QColor defaultColor = palette().color(QPalette::Text);
    
    // Позиция начала и конца фрагмента в оригинальной строке
    int fragmentStart = fragmentStartPos;
    int fragmentEnd = fragmentStart + text.length();
    int last = 0;
    // baseX фиксирован — каждый сегмент вычисляет X от базы, а не накапливает смещение.
    // Это предотвращает накопление ошибки округления при дробном m_charWidth.
    const int baseX = x;
    
    // Проходим по всем токенам подсветки
    for (const auto& token : tokens) {
        // Проверяем, пересекается ли токен с текущим фрагментом
        if (token.end <= fragmentStart || token.start >= fragmentEnd) {
            continue; // Токен не пересекается с фрагментом
        }
        
        // Вычисляем пересечение токена с фрагментом
        int tokenStartInFragment = qMax(0, token.start - fragmentStart);
        int tokenEndInFragment = qMin(static_cast<qsizetype>(text.length()), static_cast<qsizetype>(token.end - fragmentStart));
        
        // Отрисовываем текст до токена
        if (tokenStartInFragment > last) {
            QStringView before = text.sliced(last, tokenStartInFragment - last);
            painter.setPen(defaultColor);
            painter.drawText(baseX + qRound(last * m_charWidth), y, before.toString());
        }
        
        // Отрисовываем токен
        if (tokenEndInFragment > tokenStartInFragment) {
            QStringView tokenView = text.sliced(tokenStartInFragment, tokenEndInFragment - tokenStartInFragment);
            painter.setPen(token.color);
            painter.drawText(baseX + qRound(tokenStartInFragment * m_charWidth), y, tokenView.toString());
        }
        
        last = tokenEndInFragment;
    }
    
    // Отрисовываем остаток текста
    if (last < text.length()) {
        QStringView rest = text.sliced(last);
        painter.setPen(defaultColor);
        painter.drawText(baseX + qRound(last * m_charWidth), y, rest.toString());
    }
}


// Для моноширинного шрифта — O(1) вместо QTextLayout
int LogListView::getCharIndexAt(const QStringView& text, int x) const {
    if (x <= 0) return 0;
    if (m_charWidth <= 0.0) return 0;
    
    // Округление к ближайшей границе символа (snap to nearest character boundary).
    // Без этого курсор всегда прыгает к НАЧАЛУ символа под курсором,
    // что вызывает ощущение смещения на символ.
    int index = qRound(x / m_charWidth);
    return std::clamp(index, 0, static_cast<int>(text.length()));
}

// Для моноширинного шрифта — O(1)
int LogListView::calculateWidth(const QStringView& text) const {
    return qRound(text.length() * m_charWidth);
}

// Для моноширинного шрифта — O(1)
int LogListView::textWidthUntil(const QStringView& text, int count) const {
    int n = std::clamp(count, 0, static_cast<int>(text.length()));
    return qRound(n * m_charWidth);
}
