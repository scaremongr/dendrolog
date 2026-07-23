#include "LogListView.h"
#include "logmodel.h"
#include "apptheme.h"
#include "appsettings.h"
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QFileInfo>
#include <QDateTime>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTextLayout>
#include <QFontDatabase>
#include <QTimer>
#include <algorithm>
#include <cmath>
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
    // changeEvent(FontChange) тоже вызовет updateFontMetricsCache(), но если
    // шрифт из настроек совпал с текущим, событие не придёт — инициализируем явно.
    updateFontMetricsCache();
    
    // Подключаем сигнал скроллбара для плавной прокрутки
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        viewport()->update();
    });

    // Перетаскивание ползунка вверх снимает follow-tail (пользователь ушёл
    // от низа). Программные setValue sliderMoved не эмитят.
    connect(verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int value) {
        if (m_followTail && value < verticalScrollBar()->maximum())
            setFollowTail(false);
    });

    // Таймер для отложенного пересчёта высот/prefix-сумм. Два режима:
    // кэш высот актуален (ленивое уточнение при отрисовке) — только prefix-суммы;
    // кэш помечен грязным (точечная инвалидация) — полный пересчёт высот.
    m_heightUpdateTimer = new QTimer(this);
    m_heightUpdateTimer->setSingleShot(true);
    connect(m_heightUpdateTimer, &QTimer::timeout, this, [this]() {
        if (m_heightsDirty)
            rebuildHeightCache();   // включает rebuildPrefixSums()
        else
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
            verticalScrollBar()->setValue(
                contentYToScrollValue(rowYOffset(savedAnchorRow) - savedAnchorOffset));
        }

        m_anchorRow = -1;
        m_anchorOffsetInViewport = 0;

        viewport()->update();
    });

    // НЕ вызываем updateScrollbar() в конструкторе - отложим до установки модели
}

void LogListView::setWordWrap(bool enabled) {
    if (m_wordWrapEnabled == enabled) return;

    // Якорь захватываем ДО пересчёта высот, пока геометрия старая:
    // выделенная строка (и её смещение от верха вьюпорта, если видима),
    // а без выделения — первая видимая строка, чтобы позиция чтения
    // не уезжала.
    const QModelIndex cur = currentIndex();
    int anchorRow = -1;
    int anchorOffset = 0;
    bool anchorIsSelection = false;
    bool selectionWasVisible = false;
    if (cur.isValid()) {
        anchorRow = cur.row();
        anchorIsSelection = true;
        selectionWasVisible = captureVisibleRowOffset(anchorRow, anchorOffset);
    } else if (model() && model()->rowCount() > 0) {
        const qint64 scrollY = scrollContentY();
        anchorRow = rowAtY(scrollY);
        anchorOffset = int(rowYOffset(anchorRow) - scrollY);
    }

    m_wordWrapEnabled = enabled;

    m_toggledRows.clear();
    invalidateRowState(-1, /*preserveTextLengths=*/true);
    rebuildHeightCache();
    updateScrollbar();

    if (anchorRow >= 0 && model() && anchorRow < model()->rowCount()) {
        // rebuildHeightCache даёт ОЦЕНКИ высот, реальная раскладка появляется
        // только при отрисовке. Без точных высот строк над якорем накопленная
        // ошибка оценки утащит выделение за границу вьюпорта (именно так оно
        // «уплывало чуть ниже» при включении WordWrap).
        refineHeightsAbove(anchorRow);
        updateScrollbar();

        int offset = anchorOffset;
        if (anchorIsSelection) {
            // Выделение остаётся в поле видимости: сохраняем прежнее смещение;
            // если строка была частично срезана сверху или не видна вовсе —
            // ставим её в верх вьюпорта.
            const int maxOffset = qMax(0, viewport()->height() - singleRowHeight());
            offset = selectionWasVisible ? qBound(0, anchorOffset, maxOffset) : 0;
        }
        verticalScrollBar()->setValue(targetScrollValueForRowOffset(anchorRow, offset));
    }

    viewport()->update();
}

void LogListView::setModel(QAbstractItemModel *model) {
    // Рвём соединения с предыдущей моделью: они живут, пока живы оба объекта,
    // и при замене модели старые лямбды продолжили бы дёргать кэши view.
    for (const QMetaObject::Connection& connection : std::as_const(m_modelConnections))
        disconnect(connection);
    m_modelConnections.clear();

    QListView::setModel(model);
    m_persistentSelection.clear();
    m_resetViewportAnchor.clear();
    m_resetViewportOffset = 0;
    {
        auto* logModel = qobject_cast<LogModel*>(model);
        m_indexedBackendModel = logModel && logModel->isIndexedBackend();
    }
    invalidateRowState();
    if (model) {
        // Полная инвалидация при сбросе модели (фильтрация, setEntries и т.п.)
        // Выделенную ЗАПИСЬ здесь запоминать не нужно — она уже хранится в
        // m_persistentSelection (обновляется в currentChanged и переживает
        // любые reset). Здесь захватываем только якорь вьюпорта — первую
        // видимую строку — на случай, когда выделения нет вовсе.
        m_modelConnections << connect(model, &QAbstractItemModel::modelAboutToBeReset, this, [this]() {
            m_resetViewportAnchor.clear();
            m_resetViewportOffset = 0;
            auto* logModel = qobject_cast<LogModel*>(this->model());
            if (!logModel) return;
            const int total = logModel->rowCount();
            if (total <= 0) return;
            const qint64 scrollY = scrollContentY();
            const int row = qBound(0, rowAtY(scrollY), total - 1);
            const LogModel::EntryKey key = logModel->keyForRow(row);
            m_resetViewportAnchor.logicalEntryId = key.logicalEntryId;
            m_resetViewportAnchor.sourceFile = key.sourceFile;
            m_resetViewportOffset = int(scrollY - rowYOffset(row));
        });

        m_modelConnections << connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            m_toggledRows.clear();
            // Поисковая подсветка привязана к индексу строки — после reset
            // индексы невалидны.
            m_searchMatchRow = -1;
            m_searchHighlighter.setPatterns({});

            auto* logModel = qobject_cast<LogModel*>(this->model());
            // Смена бэкенда (конверсия вкладки) приходит только через reset —
            // освежаем ДО пересчёта кэшей: hugeRowMode() читает флаг.
            m_indexedBackendModel = logModel && logModel->isIndexedBackend();
            invalidateRowState();
            rebuildHeightCache();

            // 1) Выделенная запись видна в новом наборе строк — восстанавливаем
            //    выделение и центрируем на ней.
            int selectedRow = -1;
            if (logModel && m_persistentSelection.isValid()) {
                selectedRow = logModel->rowForEntry(m_persistentSelection.logicalEntryId,
                                                    m_persistentSelection.sourceFile);
            }

            // 2) Запись скрыта фильтром — прокручиваем к ближайшей видимой.
            //    Сам якорь НЕ сбрасываем: когда фильтр снимут, запись снова
            //    найдётся точно, выделение и позиция вернутся.
            int centerRow = selectedRow;
            if (centerRow < 0 && logModel && m_persistentSelection.isValid()) {
                centerRow = logModel->nearestVisibleRow(m_persistentSelection.logicalEntryId,
                                                        m_persistentSelection.sourceFile);
            }

            // 3) Выделения не было (или запись исчезла из данных) — держим
            //    вьюпорт на строке, которая была первой видимой до reset.
            int viewportRow = -1;
            if (centerRow < 0 && logModel && m_resetViewportAnchor.isValid()) {
                viewportRow = logModel->nearestVisibleRow(m_resetViewportAnchor.logicalEntryId,
                                                          m_resetViewportAnchor.sourceFile);
            }
            const int viewportOffset = m_resetViewportOffset;
            m_resetViewportAnchor.clear();
            m_resetViewportOffset = 0;

            // Обновляем скроллбар ДО вызова setCurrentIndex, чтобы первоначальные границы были корректны
            updateScrollbar();

            if (selectedRow >= 0 && selectionModel() && this->model()) {
                selectionModel()->setCurrentIndex(
                    this->model()->index(selectedRow, 0),
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                m_selection.anchorRow = m_selection.activeRow = selectedRow;
            } else {
                // Строки старого выделения в новом наборе нет — сбрасываем
                // текстовое выделение, иначе stale anchorRow подсветит чужую
                // строку и испортит захват при следующем reset.
                m_selection.clear();
            }

            // ВАЖНО: Базовый QListView может откладывать вызовы updateGeometries() или внутренние пересчеты.
            // Поэтому прокрутку к элементу обязательно нужно делать через QTimer::singleShot,
            // чтобы наш код выполнился ПОСЛЕ того, как Qt закончит все свои внутренние обновления.
            if (centerRow >= 0 || viewportRow >= 0) {
                QTimer::singleShot(0, this, [this, centerRow, viewportRow, viewportOffset]() {
                    if (!this->model()) return;
                    const int rowCount = this->model()->rowCount();

                    // Еще раз обновляем скроллбар на случай, если Qt что-то сбросило
                    updateScrollbar();

                    int target = -1;
                    if (centerRow >= 0 && centerRow < rowCount) {
                        // Центрируем строку на экране
                        const qint64 rowY = rowYOffset(centerRow);
                        const int rowH = getRowHeight(centerRow);
                        const int vpH  = viewport()->height();
                        target = contentYToScrollValue(rowY - (vpH - rowH) / 2);
                    } else if (viewportRow >= 0 && viewportRow < rowCount) {
                        // Восстанавливаем позицию вьюпорта с прежним смещением
                        target = contentYToScrollValue(rowYOffset(viewportRow) + viewportOffset);
                    }
                    if (target < 0) return;
                    verticalScrollBar()->setValue(target);
                    viewport()->update();
                });
            } else {
                verticalScrollBar()->setValue(0);
                viewport()->update();
            }
        });

        // При вставке строк В СЕРЕДИНУ (слияние нескольких файлов по времени)
        // номера существующих строк ниже точки вставки сдвигаются — переносим
        // привязанное к ним состояние. Должно выполняться ДО общей инвалидации.
        m_modelConnections << connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex&, int first, int last) {
                    shiftRowKeyedState(first, last - first + 1);
                });

        // Пересчёт кэшей при вставке: дозагрузка в конец (tail-follow) идёт
        // инкрементально за O(count), вставка в середину — полная инвалидация.
        m_modelConnections << connect(model, &QAbstractItemModel::rowsInserted, this,
                [this](const QModelIndex&, int first, int last) {
                    handleRowsInserted(first, last);
                });

        // При удалении строк номера строк ниже точки удаления сдвигаются
        // вверх — переносим/сбрасываем привязанное к номерам состояние
        // (симметрично shiftRowKeyedState при вставке), затем инвалидация.
        m_modelConnections << connect(model, &QAbstractItemModel::rowsRemoved, this,
                [this](const QModelIndex&, int first, int last) {
                    removeRowKeyedState(first, last - first + 1);
                    invalidateRowState();
                    rebuildHeightCache();
                    updateScrollbar();
                    viewport()->update();
                });

        // Инвалидация кэша строк при изменении данных (например, смена видимых полей /
        // применение нового ConversionPattern). Без этого getRowState() возвращает
        // устаревший текст из кэша даже после того, как модель уже прислала dataChanged.
        m_modelConnections << connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& topLeft, const QModelIndex& bottomRight, const QList<int>& roles) {
                // Роли, не влияющие на текст и раскладку (IsNewRole — маркер в
                // желобке, RowMarkerColorRole — фон), рисуются напрямую из
                // модели на каждый кадр: кэши валидны, достаточно перерисовать.
                // Пустой список ролей по конвенции Qt = «изменилось всё».
                if (!roles.isEmpty()
                    && !roles.contains(Qt::DisplayRole)
                    && !roles.contains(int(LogModel::FileBadgeRole))) {
                    viewport()->update();
                    return;
                }

                const int total = this->model() ? this->model()->rowCount() : 0;
                const int count = bottomRight.row() - topLeft.row() + 1;
                const bool isBulk = (count >= total / 4 || count > 500);
                if (isBulk) {
                    // Большой диапазон — инвалидируем всё за один раз
                    invalidateRowState(-1, /*preserveTextLengths=*/false);
                    rebuildHeightCache();
                } else {
                    // Точечная инвалидация. Кэш высот нельзя оставлять грязным:
                    // до следующего полного пересчёта rowAtY/rowYOffset падали бы
                    // в линейный fallback, который строит RowState для каждой
                    // строки от начала файла (заморозка на больших логах).
                    const bool heightsWereClean = !m_heightsDirty;
                    for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
                        invalidateRowState(row, /*preserveTextLengths=*/false);

                    if (heightsWereClean && m_uniformHeights) {
                        // Все строки одной высоты — смена текста её не меняет.
                        m_heightsDirty = false;
                    } else if (heightsWereClean && m_rowHeights.size() == total) {
                        // Обновляем высоты только изменённых строк (диапазон ≤ 500),
                        // prefix-суммы и скроллбар — отложенно.
                        for (int row = topLeft.row(); row <= bottomRight.row(); ++row) {
                            const int newHeight = getRowState(row).height;
                            if (m_rowHeights[row] != newHeight) {
                                m_rowHeights[row] = newHeight;
                                markPrefixDirtyFrom(row);
                            }
                        }
                        m_heightsDirty = false;
                        m_heightUpdateTimer->start(0);
                    } else {
                        // Кэш уже требовал пересчёта — полный отложенный rebuild.
                        m_heightUpdateTimer->start(0);
                    }
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

void LogListView::shiftRowKeyedState(int first, int count)
{
    if (count <= 0)
        return;

    // Кэши строк (RowState, высоты, длины текстов) не сдвигаем — их целиком
    // инвалидирует общий обработчик rowsInserted сразу после этого сдвига.

    if (!m_toggledRows.isEmpty()) {
        bool needShift = false;
        for (auto it = m_toggledRows.cbegin(); it != m_toggledRows.cend(); ++it) {
            if (*it >= first) { needShift = true; break; }
        }
        if (needShift) {
            QSet<int> shifted;
            shifted.reserve(m_toggledRows.size());
            for (auto it = m_toggledRows.cbegin(); it != m_toggledRows.cend(); ++it)
                shifted.insert(*it >= first ? *it + count : *it);
            m_toggledRows = std::move(shifted);
        }
    }

    if (m_searchMatchRow >= first)
        m_searchMatchRow += count;
    if (m_selection.anchorRow >= first)
        m_selection.anchorRow += count;
    if (m_selection.activeRow >= first)
        m_selection.activeRow += count;
    if (m_bracketMatch.row >= first)
        m_bracketMatch.row += count;
    if (m_anchorRow >= first)
        m_anchorRow += count;
}

// Симметрично shiftRowKeyedState: после удаления count строк начиная с first
// состояние, привязанное к номерам строк, сдвигается вверх; указывавшее
// в удалённый диапазон — сбрасывается.
void LogListView::removeRowKeyedState(int first, int count)
{
    if (count <= 0)
        return;
    const int lastRemoved = first + count - 1;

    // row < first — не задет; row в [first, lastRemoved] — удалён (-1); дальше — сдвиг.
    auto shiftRow = [first, lastRemoved, count](int row) {
        if (row < first) return row;
        if (row > lastRemoved) return row - count;
        return -1;
    };

    if (!m_toggledRows.isEmpty()) {
        QSet<int> shifted;
        shifted.reserve(m_toggledRows.size());
        for (int row : std::as_const(m_toggledRows)) {
            const int newRow = shiftRow(row);
            if (newRow >= 0)
                shifted.insert(newRow);
        }
        m_toggledRows = std::move(shifted);
    }

    if (m_searchMatchRow >= first) {
        m_searchMatchRow = shiftRow(m_searchMatchRow);
        if (m_searchMatchRow < 0)
            m_searchHighlighter.setPatterns({});
    }

    // Выделение, пересекающееся с удалённым диапазоном, сбрасываем целиком —
    // частичная перепривязка дала бы неочевидный для пользователя результат.
    if (m_selection.isValid()) {
        if (m_selection.topRow() <= lastRemoved && m_selection.bottomRow() >= first) {
            m_selection.clear();
        } else {
            m_selection.anchorRow = shiftRow(m_selection.anchorRow);
            m_selection.activeRow = shiftRow(m_selection.activeRow);
        }
    }

    if (m_bracketMatch.row >= first) {
        const int newRow = shiftRow(m_bracketMatch.row);
        if (newRow < 0)
            m_bracketMatch.clear();
        else
            m_bracketMatch.row = newRow;
    }

    if (m_anchorRow >= first)
        m_anchorRow = shiftRow(m_anchorRow);
}

// Вставка строк в модель. Частый случай — дозагрузка в конец (tail-follow):
// кэши существующих строк остаются валидными, достаточно досчитать длины и
// высоты новых строк и достроить prefix-суммы — O(count) вместо полной
// инвалидации с O(N)-пересчётом на каждый батч. Вставка в середину (слияние
// файлов по времени) — редкий путь с полной инвалидацией; сдвиг row-keyed
// состояния к этому моменту уже выполнен shiftRowKeyedState().
void LogListView::handleRowsInserted(int first, int last)
{
    if (!model())
        return;
    const int rows = model()->rowCount();
    const bool isAppend = (last == rows - 1);

    // Прокрутить к новому концу после вставки, если включён явный follow-tail
    // ЛИБО вьюпорт и так стоял на последней строке (тогда хвост «прилипает»:
    // каждое обновление файла показывает свежие записи, пока пользователь не
    // уйдёт вверх). Замер — ДО пересчёта высот и скроллбара, т.е. по старой
    // геометрии: вопрос именно в том, где пользователь был до этого батча.
    const bool followScrollAfterAppend = m_followTail || isScrolledToBottom();

    if (!isAppend) {
        // Вставка в середину (слияние файлов по времени): полная инвалидация.
        invalidateRowState();
        rebuildHeightCache();
        updateScrollbar();
        if (followScrollAfterAppend)
            scrollToBottomFollow();
        viewport()->update();
        return;
    }

    // --- Append: инкрементальное расширение кэшей ---

    // Кэш длин: досчитываем только новые строки (O(1) на строку, без model()->data()).
    // В огромном режиме плотный кэш не ведётся вовсе (см. hugeRowMode()).
    if (auto* logModel = qobject_cast<LogModel*>(model()); logModel && !hugeRowMode()) {
        if (m_rowTextLengths.size() < rows) {
            const int oldSize = m_rowTextLengths.size();
            m_rowTextLengths.resize(rows);
            std::fill(m_rowTextLengths.begin() + oldSize, m_rowTextLengths.end(), -1);
        }
        for (int i = first; i <= last; ++i)
            m_rowTextLengths[i] = logModel->displayTextLength(i);
    }

    if (hugeRowMode() || (!m_wordWrapEnabled && m_toggledRows.isEmpty())) {
        // Однострочный режим: rebuildHeightCache — это O(1) fast path
        rebuildHeightCache();
    } else if (!m_heightsDirty && m_rowHeights.size() == first
               && m_rowPrefixY.size() == first + 1) {
        // Достраиваем высоты только для новых строк; prefix-суммы досчитывает
        // частичный rebuildPrefixSums от first (или раньше, если уже был
        // взведён отложенный маркер от ленивого уточнения высот).
        m_rowHeights.resize(rows);
        const int charsPerLine = wrapCharsPerLine();
        for (int i = first; i <= last; ++i) {
            m_rowHeights[i] = isRowMultiLine(i)
                ? estimateWrappedRowHeight(m_rowTextLengths.value(i, -1), charsPerLine)
                : singleRowHeight();
        }
        m_rowPrefixY.resize(rows + 1);
        markPrefixDirtyFrom(first);
        rebuildPrefixSums();
    } else {
        // Кэш высот в неожиданном состоянии — честный полный пересчёт
        rebuildHeightCache();
    }

    updateScrollbar();
    if (followScrollAfterAppend)
        scrollToBottomFollow();
    viewport()->update();
}

// Обновление кэша метрик шрифта (вызывать при смене шрифта)
void LogListView::updateFontMetricsCache() {
    QFontMetrics fm(font());
    QFontMetricsF fmF(font());
    m_charWidth = fmF.horizontalAdvance(QLatin1Char('M')); // дробная точность — избегаем накопленной ошибки округления
    m_lineHeight = fm.height();
    // Ширины служебных бейджей зависят только от шрифта — меряем один раз,
    // а не QFontMetrics'ом на каждую строку в горячих путях.
    m_collapseBadgeWidth = fm.horizontalAdvance(QStringLiteral("-")) + kBadgeHPadding + kBadgeGap;
    m_hiddenToggleBadgeWidth = fm.horizontalAdvance(QStringLiteral("+9999")) + kBadgeHPadding + kBadgeGap;

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
        updateFontMetricsCache();  // сам инвалидирует кэши строк
        if (m_heightUpdateTimer) {  // null during constructor — timer not yet created
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
        clearRowPixmaps();
        m_cachedViewportWidth = -1;
        m_heightsDirty = true;
        if (!preserveTextLengths) {
            m_rowTextLengths.clear();
        }
    } else {
        m_rowStateCache.remove(row);
        removeRowPixmap(row);
        m_heightsDirty = true;  // prefix sums stale after any single-row change
        if (!preserveTextLengths && row < m_rowTextLengths.size()) {
            m_rowTextLengths[row] = -1;
        }
    }
}

// Записывает длину display-текста строки в плотный вектор, при необходимости
// расширяя его до размера модели (новые ячейки — «неизвестно»).
void LogListView::cacheTextLength(int row, int length) const {
    if (row < 0) return;
    if (hugeRowMode()) return; // плотный кэш длин в огромном режиме не ведётся
    if (row >= m_rowTextLengths.size()) {
        const int oldSize = m_rowTextLengths.size();
        const int rows = model() ? model()->rowCount() : 0;
        m_rowTextLengths.resize(qMax(rows, row + 1));
        std::fill(m_rowTextLengths.begin() + oldSize, m_rowTextLengths.end(), -1);
    }
    m_rowTextLengths[row] = length;
}

RowState LogListView::getRowState(int row) const {
    int vpWidth = viewport()->width();

    if (m_cachedViewportWidth == vpWidth && m_rowStateCache.contains(row)) {
        return m_rowStateCache[row];
    }

    RowState state = computeRowState(row);

    if (m_cachedViewportWidth != vpWidth) {
        m_rowStateCache.clear();
        clearRowPixmaps();
        m_cachedViewportWidth = vpWidth;
    }
    // Кэш ограничен: каждый RowState хранит копию текста строки, и без лимита
    // долгий скролл по большому файлу накопил бы в памяти копию всего файла.
    // Пересчёт дёшев (одно обращение к модели), поэтому при переполнении
    // сбрасываем целиком; лимит с запасом больше числа видимых строк.
    if (m_rowStateCache.size() >= kMaxCachedRowStates)
        m_rowStateCache.clear();
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
    cacheTextLength(row, state.text.length());  // кэшируем длину — переживает resize
    const bool expandedRow = isRowMultiLine(row);
    const bool forceCollapseBadge = !m_wordWrapEnabled && m_toggledRows.contains(row);
    
    // Шаг 1: Определяем, нужен ли вообще бейдж сворачивания.
    // Для auto-wrap в глобальном режиме WordWrap короткие строки не должны получать "-".
    // Для строк, раскрытых вручную из режима +N, бейдж сворачивания сохраняем всегда.
    int fileBadgeWidth = 0;
    QVariant fileBadgeVar = model()->data(model()->index(row, 0), LogModel::FileBadgeRole);
    if (fileBadgeVar.isValid()) {
        fileBadgeWidth = fontMetrics().horizontalAdvance(fileBadgeVar.toMap()["text"].toString()) + kBadgeHPadding + kBadgeGap;
    }
    const int collapseBadgeWidth = m_collapseBadgeWidth;
    const int hiddenToggleBadgeWidth = m_hiddenToggleBadgeWidth; // максимальная оценка для HiddenToggle
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
    // Огромный режим форсирует этот путь всегда: плотные массивы высот на
    // сотнях миллионов строк — гигабайты (см. hugeRowMode()).
    if (hugeRowMode() || (!m_wordWrapEnabled && m_toggledRows.isEmpty())) {
        m_rowHeights.clear();
        m_rowPrefixY.clear();
        if (hugeRowMode()) {
            m_rowTextLengths.clear();
            m_rowTextLengths.squeeze();
        }
        m_totalHeight = qint64(singleRowH) * rows;
        m_uniformHeights = true;
        m_heightsDirty = false;
        return;
    }

    m_uniformHeights = false;
    m_rowHeights.resize(rows);

    const bool vpCacheValid = (m_cachedViewportWidth == viewport()->width());

    // Кэш длин display-текста: недостающие значения добираем напрямую из модели
    // (O(1) на строку, минуя model()->data() и построение строки) —
    // это позволяет за доли секунды предсказать количество строк-переносов.
    auto* logModel = qobject_cast<LogModel*>(this->model());
    if (m_rowTextLengths.size() != rows) {
        const int oldSize = m_rowTextLengths.size();
        m_rowTextLengths.resize(rows);
        std::fill(m_rowTextLengths.begin() + qMin(oldSize, rows), m_rowTextLengths.end(), -1);
    }

    const int charsPerLine = wrapCharsPerLine();
    const bool anyStates = vpCacheValid && !m_rowStateCache.isEmpty();

    // Высота развернутой строки: точная раскладка из кэша состояний или
    // быстрая оценка по длине текста (длина / вместимость ширины).
    auto expandedRowHeight = [&](int i) -> int {
        if (anyStates && m_rowStateCache.contains(i))
            return m_rowStateCache[i].height;
        int len = m_rowTextLengths[i];
        if (len < 0 && logModel) {
            len = logModel->displayTextLength(i);
            m_rowTextLengths[i] = len;
        }
        return estimateWrappedRowHeight(len, charsPerLine);
    };

    if (!m_wordWrapEnabled) {
        // Wrap выключен: все строки однострочные, кроме точечно развернутых —
        // заполняем константой и правим только развернутые, не делая по
        // hash-lookup (isRowMultiLine) на каждую из миллионов строк.
        m_rowHeights.fill(singleRowH);
        for (int row : std::as_const(m_toggledRows)) {
            if (row >= 0 && row < rows)
                m_rowHeights[row] = expandedRowHeight(row);
        }
    } else {
        // Wrap включен: развернуто всё, кроме точечно свернутых
        const bool anyToggled = !m_toggledRows.isEmpty();
        for (int i = 0; i < rows; ++i) {
            const bool multi = anyToggled ? isRowMultiLine(i) : true;
            m_rowHeights[i] = multi ? expandedRowHeight(i) : singleRowH;
        }
    }

    // Все высоты пересчитаны — prefix-суммы устарели целиком.
    // Восстанавливаем кэш смещений для бинарного поиска строк O(log N).
    m_prefixDirtyFrom = 0;
    rebuildPrefixSums();
    m_heightsDirty = false;
}

// Сколько моноширинных колонок помещается в одну визуальную строку wrap-режима
// при текущей ширине viewport (с учётом желобка, бейджа "-" и отступов).
int LogListView::wrapCharsPerLine() const {
    int lineWidth = viewport()->width() - textAreaLeft() - m_collapseBadgeWidth
                    - kTextBadgeGap - 2 * kTextPaddingX;
    if (lineWidth <= 0) lineWidth = 100;
    return qMax(1, (int)(lineWidth / m_charWidth));
}

// Оценка высоты развернутой строки по длине текста; length < 0 (длина
// неизвестна) — консервативная заглушка, уточнится при отрисовке.
int LogListView::estimateWrappedRowHeight(int length, int charsPerLine) const {
    if (length < 0)
        return singleRowHeight() * 3;
    const int lineCount = qMax(1, (length + charsPerLine - 1) / charsPerLine);
    return lineCount * m_lineHeight + kLineVerticalPadding;
}

// Достраивает prefix-суммы из m_rowHeights. Частично: пересчитывается только
// суффикс от первой изменённой строки (m_prefixDirtyFrom) — при ленивом
// уточнении высот видимых строк во время прокрутки это O(N − row), а не O(N)
// на каждый экран. При несовпадении размеров — полный пересчёт.
void LogListView::rebuildPrefixSums() {
    // В uniform-режиме prefix-суммы не используются, а m_totalHeight уже
    // вычислен точной формулой в rebuildHeightCache — пересчёт по пустому
    // m_rowHeights обнулил бы его (например, отложенный таймер сработал уже
    // после выключения WordWrap) и схлопнул скроллбар.
    if (m_uniformHeights) {
        m_prefixDirtyFrom = std::numeric_limits<int>::max();
        return;
    }

    const int rows = m_rowHeights.size();
    int from = 0;
    if (m_rowPrefixY.size() == rows + 1) {
        from = qBound(0, m_prefixDirtyFrom, rows);
    } else {
        m_rowPrefixY.resize(rows + 1);
        m_rowPrefixY[0] = 0;
    }
    for (int i = from; i < rows; ++i)
        m_rowPrefixY[i + 1] = m_rowPrefixY[i] + m_rowHeights[i];
    m_totalHeight = (rows > 0) ? m_rowPrefixY[rows] : 0;
    m_prefixDirtyFrom = std::numeric_limits<int>::max();
}

void LogListView::refineHeightsAbove(int row) {
    if (m_uniformHeights || m_heightsDirty || !model()) return;
    if (row < 0 || row >= m_rowHeights.size()) return;

    // Идём от row вверх, пока точные высоты не перекроют вьюпорт (с запасом
    // в одну строку): только эти строки влияют на экранную позицию якоря —
    // отрисовка стартует с первой видимой строки и идёт вниз реальными
    // высотами, поэтому внутри этого окна расчёт и отрисовка совпадут.
    const int need = viewport()->height() + singleRowHeight();
    int acc = 0;
    bool changed = false;
    for (int r = row; r >= 0 && acc < need; --r) {
        const int h = getRowState(r).height;
        if (m_rowHeights[r] != h) {
            m_rowHeights[r] = h;
            markPrefixDirtyFrom(r);
            changed = true;
        }
        acc += h;
    }
    if (changed) rebuildPrefixSums();
}

// O(1): Y-позиция начала строки row (64-битные координаты контента)
qint64 LogListView::rowYOffset(int row) const {
    if (m_uniformHeights) {
        return qint64(row) * singleRowHeight();  // O(1) — однострочный режим
    }
    if (!m_heightsDirty && row >= 0 && row < m_rowPrefixY.size()) {
        return m_rowPrefixY[row];
    }
    // Fallback: линейная сумма
    qint64 y = 0;
    int limit = qBound(0, row, model() ? model()->rowCount() : 0);
    for (int i = 0; i < limit; ++i) {
        y += getRowHeight(i);
    }
    return y;
}

// O(log N): индекс строки по Y-позиции в контенте
int LogListView::rowAtY(qint64 contentY) const {
    const int rows = model() ? model()->rowCount() : 0;
    if (rows == 0) return 0;

    if (m_uniformHeights) {
        const int h = singleRowHeight();
        return int(qBound(qint64(0), contentY / h, qint64(rows - 1)));  // O(1) — однострочный режим
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
    qint64 y = 0;
    for (int i = 0; i < rows; ++i) {
        int h = getRowHeight(i);
        if (contentY < y + h) return i;
        y += h;
    }
    return rows - 1;
}

// ============================================================================
// Отображение скроллбар ↔ координаты контента.
// Пока контент помещается в 32-битный диапазон, масштаб равен 1 и значение
// скроллбара — это пиксель контента (прежнее поведение бит-в-бит).
// ============================================================================

qint64 LogListView::scrollContentY() const {
    const int value = verticalScrollBar()->value();
    if (m_scrollScale <= 1.0)
        return value;
    return qint64(std::llround(qreal(value) * m_scrollScale));
}

int LogListView::contentYToScrollValue(qint64 contentY) const {
    qint64 value = contentY;
    if (m_scrollScale > 1.0)
        value = qint64(std::llround(qreal(contentY) / m_scrollScale));
    return int(qBound(qint64(0), value, qint64(verticalScrollBar()->maximum())));
}

int LogListView::targetScrollValueForRow(int row, ScrollHint hint) const {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return verticalScrollBar()->value();
    }

    const qint64 rowTop = rowYOffset(row);
    const int rowHeight = getRowHeight(row);
    const int viewportHeight = viewport()->height();
    const qint64 currentScroll = scrollContentY();

    qint64 target = currentScroll;
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

    return contentYToScrollValue(target);
}

int LogListView::targetScrollValueForRowOffset(int row, int viewportOffset) const {
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return verticalScrollBar()->value();
    }

    return contentYToScrollValue(rowYOffset(row) - viewportOffset);
}

bool LogListView::captureVisibleRowOffset(int row, int& viewportOffset) const {
    viewportOffset = 0;
    if (!model() || row < 0 || row >= model()->rowCount()) {
        return false;
    }

    const qint64 scrollY = scrollContentY();
    const qint64 rowTop = rowYOffset(row);
    const qint64 rowBottom = rowTop + getRowHeight(row);
    const qint64 viewportBottom = scrollY + viewport()->height();

    if (rowBottom <= scrollY || rowTop >= viewportBottom) {
        return false;
    }

    // Строка видима — смещение ограничено высотой вьюпорта, в int помещается
    viewportOffset = int(rowTop - scrollY);
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

bool LogListView::getRowAtContentY(qint64 contentY, int& row, qint64& rowTop) const {
    row = -1;
    rowTop = 0;
    const int rows = model() ? model()->rowCount() : 0;
    if (rows == 0 || contentY < 0) return false;

    int r = rowAtY(contentY);
    if (r < 0 || r >= rows) return false;

    qint64 yOff = rowYOffset(r);
    // Verify contentY is within this row (guards against contentY past end of content)
    if (contentY >= yOff + getRowHeight(r)) return false;

    row    = r;
    rowTop = yOff;
    return true;
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
    clearRowPixmaps();
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
        removeRowPixmap(m_searchMatchRow);

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

    removeRowPixmap(row);
    viewport()->update();
}

void LogListView::clearSearchMatch()
{
    if (m_searchMatchRow < 0)
        return;
    removeRowPixmap(m_searchMatchRow);
    m_searchMatchRow = -1;
    m_searchHighlighter.setPatterns({});
    viewport()->update();
}

// Фоновые заливки совпадений. Геометрия повторяет drawSelectionHighlight:
// однострочный режим клипуется по visibleChars, многострочный идёт по фрагментам.
void LogListView::drawMatchHighlightSpans(QPainter& painter, const QRect& rect, const QString& text,
                                          const RowState& state, const QVector<HighlightSpan>& spans) const
{
    // Прямая отрисовка гигантской строки клипуется видимой областью —
    // фрагменты вне клипа пропускаем (см. drawLogLine).
    const QRectF clip = painter.hasClipping() ? painter.clipBoundingRect() : QRectF();

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
            if (!clip.isNull()) {
                const int fragTop = rect.top() + fragment.rect.top();
                if (fragTop + fragment.rect.height() < clip.top() || fragTop > clip.bottom())
                    continue;
            }
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
    // Токенизация и поиск совпадений — O(длина строки). Для обычных строк
    // результат запекается в пиксмап и повторно не нужен; гигантские строки
    // (не влезающие в пиксмап-кэш) рисуются напрямую на каждый кадр — для них
    // результаты разбора кэшируются по номеру строки.
    QList<HighlightToken> tokens;
    QVector<HighlightSpan> matchSpans;
    const bool useParseCache = !isRowPixmapCacheable(state);
    bool haveCached = false;
    if (useParseCache) {
        const auto it = m_rowParseCache.constFind(state.row);
        if (it != m_rowParseCache.constEnd()) {
            tokens     = it->tokens;
            matchSpans = it->matchSpans;
            haveCached = true;
        }
    }
    if (!haveCached) {
        tokens = SyntaxHighlighter::tokenize(text);
        if (!m_textHighlighter.isEmpty())
            matchSpans = m_textHighlighter.computeSpans(text);
        if (useParseCache) {
            if (m_rowParseCache.size() >= kMaxParseCacheRows)
                m_rowParseCache.clear();
            m_rowParseCache.insert(state.row, {tokens, matchSpans});
        }
    }

    // Заливки совпадений — до текста, чтобы существующая раскраска
    // SyntaxHighlighter рисовалась поверх и не менялась.
    if (!matchSpans.isEmpty())
        drawMatchHighlightSpans(painter, rect, text, state, matchSpans);

    // Подсветка результата поиска — только в найденной строке. Не кэшируем:
    // ровно одна строка, и подсветка живёт только пока активен поиск.
    if (state.row == m_searchMatchRow && !m_searchHighlighter.isEmpty()) {
        const QVector<HighlightSpan> spans = m_searchHighlighter.computeSpans(text);
        if (!spans.isEmpty())
            drawMatchHighlightSpans(painter, rect, text, state, spans);
    }

    if (state.multiLine) {
        // При прямой отрисовке (без пиксмапа — гигантские строки) painter обрезан
        // видимой областью: фрагменты вне неё пропускаем, иначе строка на тысячи
        // фрагментов отрисовывалась бы целиком на каждый кадр. При рендере в
        // pixmap клипа нет (clip пуст) — рисуются все фрагменты.
        const QRectF clip = painter.hasClipping() ? painter.clipBoundingRect() : QRectF();
        for (const auto& fragment : state.fragments) {
            if (!clip.isNull()) {
                const int fragTop = rect.top() + fragment.rect.top();
                if (fragTop + fragment.rect.height() < clip.top() || fragTop > clip.bottom())
                    continue;
            }
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

// Байтовый размер растра (для учёта бюджета кэша)
static qint64 pixmapByteSize(const QPixmap& p) {
    return qint64(p.width()) * p.height() * qMax(1, p.depth()) / 8;
}

// Адаптивный лимит числа записей: минимум 100, иначе три экрана однострочных
// строк — чтобы на высоких мониторах кэш не вытеснял ещё видимые строки.
int LogListView::maxCachedPixmaps() const {
    return qMax(100, 3 * (viewport()->height() / qMax(1, singleRowHeight())));
}

// Растр строки помещается в пиксмап-кэш? Гигантские (и вырожденные) строки —
// нет: их рисует напрямую fallback-ветка paintEvent на каждый кадр, с
// отсечением невидимых фрагментов и кэшем разбора (m_rowParseCache).
bool LogListView::isRowPixmapCacheable(const RowState& state) const {
    const int width = state.textRect.width();
    const int height = state.height;
    if (width <= 0 || height <= 0)
        return false;
    const qreal dpr = devicePixelRatioF();
    const qint64 estimatedBytes = qint64(width * dpr) * qint64(height * dpr) * 4;
    return estimatedBytes <= kMaxSingleRowPixmapBytes;
}

void LogListView::removeRowPixmap(int row) const {
    m_rowParseCache.remove(row);  // кэш разбора живёт и умирает вместе с растром
    auto it = m_rowPixmapCache.find(row);
    if (it == m_rowPixmapCache.end())
        return;
    m_rowPixmapCacheBytes -= pixmapByteSize(it.value());
    m_rowPixmapCache.erase(it);
}

void LogListView::clearRowPixmaps() const {
    m_rowParseCache.clear();      // кэш разбора живёт и умирает вместе с растрами
    m_rowPixmapCache.clear();
    m_rowPixmapCacheBytes = 0;
}

// Вытесняет растры строк, далёких от текущей позиции скролла. Если и после
// этого кэш переполнен (всё близко к вьюпорту) — полный сброс.
void LogListView::evictDistantPixmaps() const {
    const int anchor = rowAtY(scrollContentY());
    const int keepRadius = qMax(maxCachedPixmaps() / 2,
                                viewport()->height() / qMax(1, singleRowHeight()) + 1);
    for (auto it = m_rowPixmapCache.begin(); it != m_rowPixmapCache.end();) {
        if (std::abs(it.key() - anchor) > keepRadius) {
            m_rowPixmapCacheBytes -= pixmapByteSize(it.value());
            it = m_rowPixmapCache.erase(it);
        } else {
            ++it;
        }
    }
    if (m_rowPixmapCacheBytes > kMaxPixmapCacheBytes
        || m_rowPixmapCache.size() >= maxCachedPixmaps()) {
        clearRowPixmaps();
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

    // Гигантские (и вырожденные) развернутые строки не кэшируем: fallback-ветка
    // paintEvent нарисует их напрямую, отсекая невидимые фрагменты.
    if (!isRowPixmapCacheable(state))
        return QPixmap();

    const int width = state.textRect.width();
    const int height = state.height;
    const qreal devicePixelRatio = devicePixelRatioF();

    // Создаем новый растр с высоким качеством
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

    // Бюджет кэша: при переполнении вытесняем далёкие от вьюпорта строки
    const qint64 bytes = pixmapByteSize(pixmap);
    if (m_rowPixmapCacheBytes + bytes > kMaxPixmapCacheBytes
        || m_rowPixmapCache.size() >= maxCachedPixmaps()) {
        evictDistantPixmaps();
    }
    m_rowPixmapCache[row] = pixmap;
    m_rowPixmapCacheBytes += bytes;

    return pixmap;
}

void LogListView::toggleRowMultiLine(int row) {
    if (row < 0) return;
    // Огромный режим: высоты принудительно uniform, точечные развороты
    // потребовали бы плотного массива высот на весь файл.
    if (hugeRowMode()) return;

    int preservedViewportOffset = 0;
    const bool keepRowAnchor = captureVisibleRowOffset(row, preservedViewportOffset);

    if (m_toggledRows.contains(row)) {
        m_toggledRows.remove(row);
    } else {
        m_toggledRows.insert(row);
    }

    const bool heightsWereClean = !m_heightsDirty;
    invalidateRowState(row, /*preserveTextLengths=*/true);

    // Меняется высота ровно одной строки. Если кэш высот был актуален и режим
    // структурно не меняется (не входим в uniform-режим и не выходим из него),
    // обновляем одну высоту и суффикс prefix-сумм вместо полного O(N)-пересчёта.
    const int rows = model() ? model()->rowCount() : 0;
    const bool nowUniform = !m_wordWrapEnabled && m_toggledRows.isEmpty();
    if (heightsWereClean && !m_uniformHeights && !nowUniform
        && row < rows && m_rowHeights.size() == rows) {
        const int newHeight = getRowState(row).height;
        if (m_rowHeights[row] != newHeight) {
            m_rowHeights[row] = newHeight;
            markPrefixDirtyFrom(row);
            rebuildPrefixSums();
        }
        m_heightsDirty = false;
    } else {
        rebuildHeightCache();
    }
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

    const qint64 scrollY = scrollContentY();
    QRect visibleRect = event->rect();

    // O(log N) поиск первой строки, попадающей в ОБЛАСТЬ ОБНОВЛЕНИЯ (не только
    // вьюпорта): при частичной перерисовке (точечная инвалидация выделения)
    // не гоняем цикл по строкам выше области.
    int firstVisibleRow = rowAtY(scrollY + qMax(0, visibleRect.top()));
    int currentY = int(rowYOffset(firstVisibleRow) - scrollY);
    bool anyHeightChanged = false;
    for (int row = firstVisibleRow; row < rows; ++row) {
        // Получаем кэшированное состояние строки
        RowState state = getRowState(row);

        // Ленивое уточнение высот: если при первой отрисовке реальная высота
        // отличается от ранее сохранённой оценки — обновляем и перестраиваем prefix-суммы
        if (!m_uniformHeights && !m_heightsDirty && row < m_rowHeights.size()) {
            if (m_rowHeights[row] != state.height) {
                m_rowHeights[row] = state.height;
                markPrefixDirtyFrom(row);
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
                // Прямая отрисовка (гигантские строки не кэшируются в pixmap).
                // Клип пересекаем с видимой областью: по нему drawLogLine
                // отсекает невидимые фрагменты, не рисуя всю строку целиком.
                painter.save();
                painter.setClipRect(textRect.intersected(visibleRect));
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
        const qint64 scrollY = scrollContentY();
        qint64 rowTop = 0;
        if (getRowAtContentY(scrollY, m_anchorRow, rowTop)) {
            m_anchorOffsetInViewport = int(rowTop - scrollY);
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

qint64 LogListView::estimateTotalHeightForDirtyCache(int rows) const {
    const int singleRowH = singleRowHeight();
    if (rows <= 0) {
        return 0;
    }

    // В однострочном режиме точная формула O(1).
    if (!m_wordWrapEnabled && m_toggledRows.isEmpty()) {
        return qint64(singleRowH) * rows;
    }

    // Для word-wrap нужен консервативный estimate, иначе scrollbar может исчезнуть
    // до первого точного пересчёта высот после reset/filter.
    const qint64 estimatedWrappedRowH = qint64(singleRowH) * 3;
    return estimatedWrappedRowH * rows;
}

void LogListView::updateScrollbar() {
    if (!model()) return;

    const int rows = model()->rowCount();
    if (rows == 0) {
        m_scrollScale = 1.0;
        verticalScrollBar()->setRange(0, 0);
        return;
    }

    qint64 totalHeight;
    if (!m_heightsDirty) {
        // Точное (или оценочное из кэша) значение — никакого обращения к модели
        totalHeight = m_totalHeight;
    } else {
        // Кэш ещё не построен — быстрая консервативная оценка без model()->data().
        // Для wrapped-строк нельзя использовать однострочную формулу, она занижает диапазон.
        totalHeight = estimateTotalHeightForDirtyCache(rows);
    }

    const qint64 maxScroll = std::max<qint64>(0, totalHeight - viewport()->height());

    // Явно диктуем политику отображения скроллбара, чтобы разорвать
    // бесконечный цикл resizeEvent (ScrollBar Oscillation), который возникает,
    // когда базовый QListView думает, что скроллбар не нужен и прячет его, а мы — показываем.
    if (maxScroll > 0) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    } else {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    // Пока контент помещается в 32-битный диапазон, значение скроллбара — это
    // пиксель контента (масштаб 1, прежнее поведение). Дальше — сжатая шкала:
    // page/single step пересчитываются, чтобы прокрутка на страницу/строку
    // соответствовала тем же пикселям контента.
    if (maxScroll <= kMaxScrollRange) {
        m_scrollScale = 1.0;
        verticalScrollBar()->setRange(0, int(maxScroll));
    } else {
        m_scrollScale = qreal(maxScroll) / kMaxScrollRange;
        verticalScrollBar()->setRange(0, kMaxScrollRange);
    }
    const qreal invScale = 1.0 / m_scrollScale;
    verticalScrollBar()->setPageStep(qMax(1, int(viewport()->height() * invScale)));
    verticalScrollBar()->setSingleStep(qMax(1, int(singleRowHeight() * invScale)));
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
        const qint64 contentY = viewportPos.y() + scrollContentY();

        int row = -1;
        qint64 rowTop = 0;
        if (getRowAtContentY(contentY, row, rowTop)) {
            info.row = row;
            info.rowHeight = getRowHeight(row);
            info.localPos = QPoint(viewportPos.x(), int(contentY - rowTop));
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
    const qint64 scrollY = scrollContentY();
    const qint64 topY    = rowYOffset(topRow) - scrollY;
    const qint64 botY    = rowYOffset(bottomRow) + getRowHeight(bottomRow) - scrollY;
    // Диапазон может уходить далеко за пределы вьюпорта — обрезаем (QRect 32-битный)
    const int vpHeight = viewport()->height();
    if (botY <= 0 || topY >= vpHeight)
        return;
    const int top    = int(qMax<qint64>(0, topY));
    const int bottom = int(qMin<qint64>(vpHeight, botY));
    viewport()->update(QRect(0, top, viewport()->width(), bottom - top));
}

// Обработка движения мыши — расширяет выделение на несколько строк.
void LogListView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_selection.isDragging) return;

    const qint64 contentY = event->pos().y() + scrollContentY();
    const int rowCount = model() ? model()->rowCount() : 0;
    if (rowCount == 0) return;

    // Определяем строку под курсором с зажимом на границах контента
    int newRow = -1;
    qint64 rowTop = 0;
    if (contentY < 0) {
        newRow = 0;
        rowTop = 0;
    } else if (!getRowAtContentY(contentY, newRow, rowTop)) {
        newRow = rowCount - 1;
        rowTop = rowYOffset(newRow);
    }

    const RowState& state   = getRowState(newRow);
    const QPoint    localPos(event->pos().x(), int(contentY - rowTop));
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

    const qint64 contentY = event->pos().y() + scrollContentY();
    int row = -1;
    qint64 rowTop = 0;
    if (!getRowAtContentY(contentY, row, rowTop)) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    const RowState& state    = getRowState(row);
    const QPoint    localPos = QPoint(event->pos().x(), int(contentY - rowTop));
    const int       clickPos = getTextPositionFromMouse(localPos, state);

    const TextToken::Token tok = TextToken::findDoubleClickToken(state.text, clickPos);
    if (tok.isEmpty()) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }

    m_selection.start(row, tok.start);
    m_selection.moveTo(row, tok.end);
    m_selection.finish();
    updateBracketMatch();

    if (selectionModel()) {
        selectionModel()->setCurrentIndex(model()->index(row, 0),
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    viewport()->update();
}

// Текущий выделенный текст (строки склеены через '\n'); пусто, если нет выделения.
QString LogListView::selectedText() const {
    if (m_selection.isEmpty() || !model())
        return QString();
    QStringList lines;
    for (int row = m_selection.topRow(); row <= m_selection.bottomRow(); ++row) {
        const QString rowText = model()->data(model()->index(row, 0)).toString();
        int selStart = 0, selEnd = 0;
        m_selection.rangeForRow(row, rowText.length(), selStart, selEnd);
        lines.append(rowText.mid(selStart, selEnd - selStart));
    }
    return lines.join('\n');
}

// Копирование выделения в буфер обмена (многострочное).
void LogListView::copySelectionToClipboard() const {
    const QString text = selectedText();
    if (!text.isEmpty())
        QApplication::clipboard()->setText(text);
}

// Граница следующего токена справа от pos (для Ctrl+Shift+Right).
static int rightTokenBoundary(const QString& text, int pos) {
    const int n = text.length();
    if (pos >= n) return n;
    const TextToken::Token t = TextToken::findDoubleClickToken(text, pos);
    if (t.isEmpty() || t.end <= pos) return std::min(pos + 1, n);  // гарантируем прогресс
    return t.end;
}

// Граница предыдущего токена слева от pos (для Ctrl+Shift+Left).
static int leftTokenBoundary(const QString& text, int pos) {
    if (pos <= 0) return 0;
    const TextToken::Token t = TextToken::findDoubleClickToken(text, pos - 1);
    if (t.isEmpty() || t.start >= pos) return std::max(pos - 1, 0);
    return t.start;
}

// Двигает активный конец выделения с клавиатуры (Shift / Ctrl+Shift + стрелки).
void LogListView::extendSelectionByKeyboard(int direction, bool byToken) {
    if (!model() || model()->rowCount() == 0)
        return;

    // Нужна валидная активная позиция. Если выделения ещё нет — сеем свёрнутое
    // выделение в начале текущей строки, чтобы расширение заработало сразу.
    if (!m_selection.isValid()) {
        const QModelIndex cur = currentIndex();
        if (!cur.isValid())
            return;
        m_selection.anchorRow = m_selection.activeRow = cur.row();
        m_selection.anchorPos = m_selection.activePos = 0;
        m_selection.isDragging = false;
    }

    int     row  = m_selection.activeRow;
    int     pos  = m_selection.activePos;
    QString text = model()->data(model()->index(row, 0)).toString();
    int     len  = text.length();

    if (direction > 0) {                       // вправо
        if (pos >= len) {                      // конец строки — шаг на следующую
            if (row + 1 >= model()->rowCount())
                return;
            ++row;
            pos = 0;
        } else {
            pos = byToken ? rightTokenBoundary(text, pos) : pos + 1;
        }
    } else {                                   // влево
        if (pos <= 0) {                        // начало строки — шаг на предыдущую
            if (row <= 0)
                return;
            --row;
            text = model()->data(model()->index(row, 0)).toString();
            pos  = text.length();
        } else {
            pos = byToken ? leftTokenBoundary(text, pos) : pos - 1;
        }
    }

    const int oldTop    = m_selection.topRow();
    const int oldBottom = m_selection.bottomRow();

    m_selection.activeRow = row;
    m_selection.activePos = pos;
    m_selection.isDragging = false;

    const int dirtyTop    = std::min(oldTop,    m_selection.topRow());
    const int dirtyBottom = std::max(oldBottom, m_selection.bottomRow());
    invalidateSelectionRange(dirtyTop, dirtyBottom);

    if (row >= 0 && row < model()->rowCount())
        scrollTo(model()->index(row, 0), EnsureVisible);
}

// True если у строки есть плашка-переключатель раскрытия (она длиннее видимой
// области либо уже развёрнута на несколько визуальных строк).
bool LogListView::rowHasWrapToggle(int row) const {
    if (row < 0 || !model() || row >= model()->rowCount())
        return false;
    const RowState state = getRowState(row);
    for (const BadgeLayout& b : state.badges) {
        if (b.spec.type == BadgeType::HiddenToggle)
            return true;
    }
    return false;
}

// Обработка нажатия клавиш (копирование выделенного текста; пробел —
// раскрытие/сворачивание текущей строки, как клик по плашке справа).
// ---------------------------------------------------------------------------
// Follow-tail: автопрокрутка к концу при догрузке строк.
// ---------------------------------------------------------------------------

void LogListView::setFollowTail(bool enabled)
{
    if (m_followTail == enabled)
        return;
    m_followTail = enabled;
    emit followTailChanged(enabled);
    if (enabled)
        scrollToBottomFollow();
}

void LogListView::scrollToBottomFollow()
{
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

bool LogListView::isScrolledToBottom() const
{
    const QScrollBar* sb = verticalScrollBar();
    const int maximum = sb->maximum();
    if (maximum <= 0)
        return false; // контент влезает целиком — прокручивать было нечего
    return sb->value() >= maximum - qMax(1, sb->singleStep());
}

void LogListView::wheelEvent(QWheelEvent *event)
{
    // Пользователь листает вверх — он хочет читать историю, а не хвост.
    if (m_followTail && event->angleDelta().y() > 0)
        setFollowTail(false);
    QListView::wheelEvent(event);
}

void LogListView::keyPressEvent(QKeyEvent *event) {
    if (m_followTail) {
        switch (event->key()) {
        case Qt::Key_Up:
        case Qt::Key_PageUp:
        case Qt::Key_Home:
            setFollowTail(false); // навигация вверх снимает автопрокрутку
            break;
        default:
            break;
        }
    }

    if (event->matches(QKeySequence::Copy) && !m_selection.isEmpty()) {
        copySelectionToClipboard();
        return;
    }

    // Расширение текстового выделения с клавиатуры:
    //   Shift + ←/→        — на один символ
    //   Ctrl + Shift + ←/→ — на токен/блок
    if ((event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)
        && (event->modifiers() & Qt::ShiftModifier)
        && !(event->modifiers() & (Qt::AltModifier | Qt::MetaModifier))) {
        const bool byToken = event->modifiers() & Qt::ControlModifier;
        const int  dir     = (event->key() == Qt::Key_Right) ? +1 : -1;
        extendSelectionByKeyboard(dir, byToken);
        return;
    }

    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        // Toggle wrap only for rows that can expand/collapse; for any other row
        // do nothing (and consume the event) so the default QListView Space
        // handling does not jump the selection.
        const QModelIndex cur = currentIndex();
        if (cur.isValid() && rowHasWrapToggle(cur.row()))
            toggleRowMultiLine(cur.row());
        return;
    }

    QListView::keyPressEvent(event);
}

// Разбор строки таймстампа в QDateTime по набору распространённых форматов.
// Дробная часть секунд отбрасывается (гранулярность фильтра — секунды).
static QDateTime parseTimestampText(const QString& raw) {
    QString s = raw.trimmed();
    s.replace(QChar(','), QChar('.'));
    // Убрать дробные доли секунды в конце ("...:56.789" → "...:56").
    static const QRegularExpression fracRe(QStringLiteral("[.]\\d+$"));
    s.remove(fracRe);

    static const QStringList dateTimeFormats = {
        QStringLiteral("yyyy-MM-dd HH:mm:ss"), QStringLiteral("yyyy-MM-ddTHH:mm:ss"),
        QStringLiteral("yyyy/MM/dd HH:mm:ss"),
        QStringLiteral("dd.MM.yyyy HH:mm:ss"), QStringLiteral("dd/MM/yyyy HH:mm:ss"),
        QStringLiteral("dd-MM-yyyy HH:mm:ss"),
    };
    for (const QString& f : dateTimeFormats) {
        const QDateTime dt = QDateTime::fromString(s, f);
        if (dt.isValid())
            return dt;
    }

    // Только время — подставляем сегодняшнюю дату.
    static const QStringList timeFormats = {
        QStringLiteral("HH:mm:ss"), QStringLiteral("H:mm:ss"),
    };
    for (const QString& f : timeFormats) {
        const QTime t = QTime::fromString(s, f);
        if (t.isValid())
            return QDateTime(QDate::currentDate(), t);
    }
    return QDateTime();
}

// Контекстное меню строки: копирование выделения, переключение word wrap для
// строки под курсором и действия в зависимости от типа выделенного блока
// (открыть ссылку / файл, подставить таймстамп в фильтр по времени).
void LogListView::contextMenuEvent(QContextMenuEvent *event) {
    const QPoint vpPos    = viewport()->mapFromGlobal(event->globalPos());
    const qint64 contentY = vpPos.y() + scrollContentY();

    int row = -1;
    qint64 rowTop = 0;
    const bool haveRow = getRowAtContentY(contentY, row, rowTop);

    QMenu menu(this);

    if (!m_selection.isEmpty()) {
        QAction* copyAct = menu.addAction(tr("Copy"));
        copyAct->setShortcut(QKeySequence::Copy);
        connect(copyAct, &QAction::triggered, this, [this]() { copySelectionToClipboard(); });
    } else if (haveRow && model()) {
        // Ничего не выделено — предлагаем скопировать всю строку под курсором
        // (именно отображаемое содержимое, с учётом выбранных Log Fields).
        QAction* copyLineAct = menu.addAction(tr("Copy Whole Line"));
        const int copyRow = row;
        connect(copyLineAct, &QAction::triggered, this, [this, copyRow]() {
            if (model() && copyRow >= 0 && copyRow < model()->rowCount())
                QApplication::clipboard()->setText(
                    model()->data(model()->index(copyRow, 0), Qt::DisplayRole).toString());
        });
    }

    // Действия, зависящие от типа выделенного блока. URL/путь/таймстамп —
    // всегда однострочные токены: для многострочного выделения не собираем
    // полный текст (он может быть огромным) и не классифицируем.
    QString sel;
    if (!m_selection.isEmpty() && !m_selection.isMultiRow())
        sel = selectedText().trimmed();
    const TextToken::TokenType selType = sel.isEmpty()
        ? TextToken::TokenType::None : TextToken::classify(sel);
    if (selType == TextToken::TokenType::Url) {
        menu.addSeparator();
        QAction* a = menu.addAction(tr("Open Link"));
        connect(a, &QAction::triggered, this, [sel]() {
            QDesktopServices::openUrl(QUrl::fromUserInput(sel));
        });
    } else if (selType == TextToken::TokenType::FilePath) {
        menu.addSeparator();
        const bool exists = QFileInfo::exists(sel);
        QAction* openFile = menu.addAction(tr("Open File"));
        openFile->setEnabled(exists);
        connect(openFile, &QAction::triggered, this, [sel]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(sel));
        });
        QAction* openDir = menu.addAction(tr("Open Containing Folder"));
        openDir->setEnabled(exists);
        connect(openDir, &QAction::triggered, this, [sel]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(sel).absolutePath()));
        });
    } else if (selType == TextToken::TokenType::Timestamp) {
        const QDateTime dt = parseTimestampText(sel);
        if (dt.isValid()) {
            menu.addSeparator();
            QAction* asStart = menu.addAction(tr("Use as Time Filter Start"));
            connect(asStart, &QAction::triggered, this,
                    [this, dt]() { emit timeFilterBoundRequested(dt, true); });
            QAction* asEnd = menu.addAction(tr("Use as Time Filter End"));
            connect(asEnd, &QAction::triggered, this,
                    [this, dt]() { emit timeFilterBoundRequested(dt, false); });
        }
    }

    if (haveRow && rowHasWrapToggle(row) && !hugeRowMode()) {
        menu.addSeparator();
        QAction* wrapAct = menu.addAction(tr("Word Wrap (this line)"));
        wrapAct->setCheckable(true);
        wrapAct->setChecked(isRowMultiLine(row));
        connect(wrapAct, &QAction::triggered, this, [this, row]() { toggleRowMultiLine(row); });
    }

    menu.exec(event->globalPos());
}

void LogListView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QListView::currentChanged(current, previous);

    // Долгоживущий якорь выделения: запоминаем ЗАПИСЬ, а не номер строки.
    // Invalid current (например, очистка при reset) якорь не затирает —
    // именно он позволит восстановить выделение после смены фильтра.
    if (current.isValid()) {
        if (auto* logModel = qobject_cast<LogModel*>(model())) {
            const LogModel::EntryKey key = logModel->keyForRow(current.row());
            if (key.logicalEntryId >= 0) {
                m_persistentSelection.logicalEntryId = key.logicalEntryId;
                m_persistentSelection.sourceFile = key.sourceFile;
            }
        }
    }

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

// НЕ зовём QListView::doItemsLayout: он перестраивает внутренние
// flowPositions базовой раскладки по каждой строке — O(N) на КАЖДЫЙ батч
// rowsInserted (база планирует полную перераскладку) и на каждый reset.
// На десятках миллионов строк это сотни мс GUI-фриза за батч, при том что
// результат раскладки view нигде не использует. База QAbstractItemView
// лишь снимает флаг отложенной раскладки и обновляет геометрию/viewport.
void LogListView::doItemsLayout()
{
    QAbstractItemView::doItemsLayout();
}

// Замена базового QListView::moveCursor — единственного потребителя базовой
// раскладки (rectForIndex/flowPositions): навигация по собственной геометрии
// строк. Вызывается из QAbstractItemView::keyPressEvent; возвращённый индекс
// база делает текущим и прокручивает нашим scrollTo.
QModelIndex LogListView::moveCursor(CursorAction cursorAction,
                                    Qt::KeyboardModifiers /*modifiers*/)
{
    if (!model())
        return QModelIndex();
    const int rows = model()->rowCount();
    if (rows <= 0)
        return QModelIndex();

    const QModelIndex cur = currentIndex();
    const int curRow = cur.isValid() ? qBound(0, cur.row(), rows - 1) : 0;
    int row = curRow;

    switch (cursorAction) {
    case MoveUp:
    case MovePrevious:
        row = qMax(0, curRow - 1);
        break;
    case MoveDown:
    case MoveNext:
        row = qMin(rows - 1, curRow + 1);
        break;
    case MovePageUp:
        row = qBound(0, rowAtY(rowYOffset(curRow) - qMax(1, viewport()->height() - singleRowHeight())),
                     rows - 1);
        if (row == curRow && curRow > 0)
            row = curRow - 1; // страница из одной высокой строки — не застревать
        break;
    case MovePageDown:
        row = qBound(0, rowAtY(rowYOffset(curRow) + qMax(1, viewport()->height() - singleRowHeight())),
                     rows - 1);
        if (row == curRow && curRow < rows - 1)
            row = curRow + 1;
        break;
    case MoveHome:
        row = 0;
        break;
    case MoveEnd:
        row = rows - 1;
        break;
    case MoveLeft:
    case MoveRight:
    default:
        break; // плоский список: горизонтальных перемещений нет
    }
    return model()->index(row, 0);
}

// Экранный прямоугольник строки по собственной геометрии (координаты
// viewport). Для строк далеко за пределами вьюпорта возвращаем пустой rect —
// база использует visualRect только для точечных update/hit-test видимого.
QRect LogListView::visualRect(const QModelIndex& index) const
{
    if (!index.isValid() || !model() || index.row() < 0
        || index.row() >= model()->rowCount())
        return QRect();
    const qint64 y = rowYOffset(index.row()) - scrollContentY();
    if (qAbs(y) > (qint64(1) << 30))
        return QRect(); // за пределами разумного экранного диапазона int
    return QRect(0, int(y), viewport()->width(), getRowHeight(index.row()));
}

// Строка под точкой вьюпорта; за пределами контента — invalid.
QModelIndex LogListView::indexAt(const QPoint& point) const
{
    if (!model())
        return QModelIndex();
    int row = -1;
    qint64 rowTop = 0;
    if (!getRowAtContentY(point.y() + scrollContentY(), row, rowTop))
        return QModelIndex();
    return model()->index(row, 0);
}

// Замена базового QListView::setSelection, читающего внутреннюю раскладку
// (здесь она пуста — см. doItemsLayout). Критично для клавиатурной навигации:
// QAbstractItemView::keyPressEvent после selectionModel()->setCurrentIndex
// зовёт setSelection(rect 1×1 в центре новой текущей строки, ClearAndSelect) —
// базовая реализация не находила строку и стирала выделение. Пустая область
// (ниже контента) по-прежнему даёт пустой QItemSelection: ClearAndSelect
// снимает выделение, как и в базовом поведении.
void LogListView::setSelection(const QRect& rect,
                               QItemSelectionModel::SelectionFlags command)
{
    if (!selectionModel() || !model())
        return;
    QItemSelection selection;
    const QRect r = rect.normalized();
    const qint64 top    = qMax<qint64>(0, r.top() + scrollContentY());
    const qint64 bottom = r.bottom() + scrollContentY();
    int firstRow = -1;
    qint64 firstTop = 0;
    // getRowAtContentY отвергает точки ниже конца контента; rowAtY для нижней
    // границы клампится к последней строке. Обе — O(log N) по prefix-суммам.
    if (bottom >= 0 && getRowAtContentY(top, firstRow, firstTop)) {
        const int last = rowAtY(bottom);
        selection.select(model()->index(firstRow, 0),
                         model()->index(qMax(firstRow, last), 0));
    }
    selectionModel()->select(selection, command);
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

    // Токены отсортированы и не пересекаются (однопроходный сканер) — бинарным
    // поиском находим первый токен, пересекающийся с фрагментом, и идём до
    // конца фрагмента: O(log T + K) вместо полного перебора O(T) на фрагмент.
    auto tokenIt = std::lower_bound(tokens.constBegin(), tokens.constEnd(), fragmentStart,
        [](const HighlightToken& t, int pos) { return t.end <= pos; });
    for (; tokenIt != tokens.constEnd() && tokenIt->start < fragmentEnd; ++tokenIt) {
        const auto& token = *tokenIt;

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
