#include "LogListView.h"
#include "shortcount.h"
#include "logmodel.h"
#include "apptheme.h"
#include "appsettings.h"
#include "highlightpalette.h"
#include "lineclassifier.h"
#include "newlinesindicator.h"
#include "scrollbarmarks.h"
#include "scrollbarmarksoverlay.h"
#include "shortcutmanager.h"
#include <QHelpEvent>
#include <QToolTip>
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

// Знак развёрнутой строки на переключателе переноса (U-23).
static constexpr QChar kCollapseGlyph(0x2212);

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

    // Режим метки файла (U-23) — общая настройка: меняет её окно (View →
    // File Labels), все view, включая выдачу Search, следуют сами.
    m_fileLabelMode = AppSettings::instance().fileLabelMode();
    m_fileColumnChars = AppSettings::instance().fileColumnChars();
    m_lineNumbersVisible = AppSettings::instance().lineNumbers();
    // Движение мыши без кнопок — только курсор над границей колонки файла.
    viewport()->setMouseTracking(true);
    connect(&AppSettings::instance(), &AppSettings::settingsChanged, this, [this]() {
        setFileLabelMode(AppSettings::instance().fileLabelMode());
        if (!m_columnDrag)
            setFileColumnChars(AppSettings::instance().fileColumnChars());
        setLineNumbersVisible(AppSettings::instance().lineNumbers());
    });
    
    // Отметки на полосе прокрутки (R-6): расчёт по модели и оверлей поверх
    // скроллбара; сам скроллбар не подменяется.
    m_scrollbarMarks = new ScrollbarMarks(this);
    m_scrollbarMarksOverlay = new ScrollbarMarksOverlay(this, m_scrollbarMarks);

    // Подключаем сигнал скроллбара для плавной прокрутки
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() {
        // Прокрутка в окне дебаунса resize (переход, восстановление после
        // фильтра, колесо) новее якоря, снятого в resizeEvent: якорь —
        // то, что на экране сейчас, иначе отложенный пересчёт вернул бы вид
        // к позиции на момент resize.
        if (m_resizeDebounceTimer && m_resizeDebounceTimer->isActive())
            captureResizeAnchor();
        viewport()->update();
        // Строки, попавшие на экран, просмотрены; у низа плашка уходит.
        markNewLinesSeen();
    });

    // Плашка «↓ N new lines» — дочерний виджет самого view, а не вьюпорта:
    // прокрутка вьюпорта её не двигает. Место — в placeNewLinesIndicator.
    m_newLinesIndicator = new NewLinesIndicator(this);
    connect(m_newLinesIndicator, &QAbstractButton::clicked, this, &LogListView::goToNewLines);
    // Пользователь сам крутит ползунок (drag, стрелки, клик по жёлобу) —
    // позиция после перехода больше не удерживается (см. goToRow).
    connect(verticalScrollBar(), &QScrollBar::actionTriggered, this, [this]() {
        m_holdAfterGoTo = false;
        noteUserAction();
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
        // Вид у самого низа там и остаётся: уточнённые высоты последних
        // строк растягивают контент, и без этого конец снова уехал бы за край.
        QScrollBar* bar = verticalScrollBar();
        const bool atBottom = bar->maximum() > 0 && bar->value() == bar->maximum();
        if (m_heightsDirty)
            rebuildHeightCache();   // включает rebuildPrefixSums()
        else
            rebuildPrefixSums();
        updateScrollbar();
        if (atBottom)
            bar->setValue(bar->maximum());
        // Resize, перенос строк, зум шрифта: на экран могли попасть новые
        // строки (или низ) без смены значения скроллбара.
        markNewLinesSeen();
        placeNewLinesIndicator();
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

        // Восстанавливаем позицию скролла по якорной строке. rebuildHeightCache
        // даёт ОЦЕНКИ высот (перенос строк) — над якорем они уточняются, иначе
        // накопленная ошибка оценки (высокая строка в несколько экранов)
        // уводит якорь далеко от прежнего места.
        if (savedAnchorRow >= 0 && model() && savedAnchorRow < model()->rowCount()) {
            refineHeightsAbove(savedAnchorRow);
            updateScrollbar();
            verticalScrollBar()->setValue(
                contentYToScrollValue(rowYOffset(savedAnchorRow) - savedAnchorOffset));
        }

        m_anchorRow = -1;
        m_anchorOffsetInViewport = 0;

        // Окно стало выше — новые строки (или низ) могли оказаться на экране.
        markNewLinesSeen();
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
    clearNewLines();
    m_persistentSelection.clear();
    m_resetViewportAnchor.clear();
    m_resetViewportOffset = 0;
    m_holdRestoredOffset = false;
    m_awaitedAnchor = {};
    {
        auto* logModel = qobject_cast<LogModel*>(model);
        m_indexedBackendModel = logModel && logModel->isIndexedBackend();
        m_scrollbarMarks->setModel(logModel);
        m_lineNumberHighWater = 0;
        m_fileColumnWidth = computeFileColumnWidth();
        rebuildFileLabels();
        m_lineNumberWidth = computeLineNumberWidth();
        if (logModel) {
            m_modelConnections << connect(logModel, &LogModel::fileSetChanged, this, [this]() {
                m_lineNumberHighWater = 0;   // другой набор файлов — ширина заново
                scheduleLeftColumns();
            });
            m_modelConnections << connect(logModel, &LogModel::fileLinesChanged,
                                          this, &LogListView::onFileLinesChanged);
        }
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
            // На случай, если reset оставит модель пустой (конверсия, полная
            // перезагрузка): ключ выбранной строки, иначе верхней. Якорь,
            // который уже ждёт своих строк, частичными данными не заменяется.
            if (m_awaitedAnchor.armed)
                return;
            m_awaitedAnchor = {};
            const int selected = userSelectedRow();
            const int keyRow = selected >= 0 ? selected : row;
            m_awaitedAnchor.key = logModel->keyAt(keyRow);
            m_awaitedAnchor.selected = selected >= 0;
            int offset = 0;
            if (!captureVisibleRowOffset(keyRow, offset))
                m_awaitedAnchor.centered = true;
            m_awaitedAnchor.offset = offset;
            m_awaitedAnchor.userActions = m_userActions;
        });

        m_modelConnections << connect(model, &QAbstractItemModel::modelReset, this, [this]() {
            m_toggledRows.clear();
            m_pendingChromeClick = {};
            // Набор строк другой (фильтр, перезагрузка, конверсия бэкенда):
            // прежний счёт новых строк к нему не относится.
            clearNewLines();
            // Якорь resize — номер строки, после reset он указывает на чужую
            // строку. Позицию ниже восстанавливает сам reset; якорь снимается
            // заново, когда она установлена.
            m_anchorRow = -1;
            m_anchorOffsetInViewport = 0;
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
            // Строку поставил setViewAnchor (восстановленное состояние
            // вкладки), а пользователь вид с тех пор не трогал — она остаётся
            // на своём месте экрана, а не уходит в центр.
            const bool keepRestoredOffset = m_holdRestoredOffset && selectedRow >= 0;
            const int restoredOffset = m_restoredOffset;
            // Строк нет вовсе (конверсия, полная перезагрузка) — якорь ждёт
            // своих строк из вставок (placeAwaitedAnchor).
            if (logModel && logModel->rowCount() == 0 && m_awaitedAnchor.isValid()) {
                // Выбор без удерживаемого места встаёт по центру, как после
                // обычного reset.
                if (!m_awaitedAnchor.armed && m_awaitedAnchor.selected && !m_holdRestoredOffset)
                    m_awaitedAnchor.centered = true;
                m_awaitedAnchor.armed = true;
            } else if (!m_awaitedAnchor.armed) {
                m_awaitedAnchor = {};
            }

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
                const quint64 epoch = m_positionEpoch;
                QTimer::singleShot(0, this, [this, centerRow, viewportRow, viewportOffset,
                                             keepRestoredOffset, restoredOffset, epoch]() {
                    if (!this->model()) return;
                    // Пользователь успел сам выбрать строку или прокрутить
                    // вид после reset, либо позицию поставили явно
                    // (setViewAnchor, ожидающий якорь) — это новее.
                    if (m_positionEpoch != epoch) return;
                    const int rowCount = this->model()->rowCount();

                    // Еще раз обновляем скроллбар на случай, если Qt что-то сбросило
                    updateScrollbar();

                    int target = -1;
                    if (keepRestoredOffset && centerRow >= 0 && centerRow < rowCount) {
                        if (refineHeightsAbove(centerRow))
                            updateScrollbar();
                        target = targetScrollValueForRowOffset(centerRow, restoredOffset);
                    } else if (centerRow >= 0 && centerRow < rowCount) {
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
                    // Значение могло не измениться, а строки под ним — другие.
                    if (m_resizeDebounceTimer->isActive())
                        captureResizeAnchor();
                    viewport()->update();
                });
            } else {
                verticalScrollBar()->setValue(0);
                viewport()->update();
            }
            // Якорь ждёт строку с прошлого пустого reset, а этот пришёл уже
            // со строками (фильтр по частичным данным, батч слитой вкладки):
            // строка могла прийти с ним — после восстановления выше.
            if (m_awaitedAnchor.armed && this->model() && this->model()->rowCount() > 0)
                QTimer::singleShot(0, this, [this]() {
                    if (m_awaitedAnchor.armed)
                        placeAwaitedAnchor();
                });
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
                    if (m_awaitedAnchor.armed)
                        placeAwaitedAnchor();
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
                if (!roles.isEmpty() && !roles.contains(Qt::DisplayRole)) {
                    viewport()->update();
                    return;
                }

                const int total = this->model() ? this->model()->rowCount() : 0;
                const int count = bottomRight.row() - topLeft.row() + 1;
                const bool isBulk = (count >= total / 4 || count > 500);
                // Якорь вьюпорта снимаем ДО пересчёта высот: строки на местах,
                // меняются только их высоты, поэтому положение восстанавливается
                // по той же строке с тем же смещением.
                int anchorRow = -1;
                qint64 anchorOffset = 0;
                if (isBulk && total > 0) {
                    const qint64 scrollY = scrollContentY();
                    qint64 rowTop = 0;
                    if (getRowAtContentY(scrollY, anchorRow, rowTop))
                        anchorOffset = rowTop - scrollY;
                }
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
                // После массового изменения высот строк возвращаем положение:
                // к ВЫБРАННОЙ пользователем строке (смена видимых полей,
                // паттерна и прочих модификаторов компоновки), а если выбора
                // нет — удерживаем вьюпорт на прежней верхней строке. Проверка
                // выделения здесь обязательна: текущей строку мог назначить
                // Qt при получении фокуса, не выделяя её, и прокрутка к ней
                // утаскивала бы вьюпорт к началу файла (см. focusInEvent).
                if (isBulk) {
                    QTimer::singleShot(0, this, [this, anchorRow, anchorOffset]() {
                        if (!this->model())
                            return;
                        if (userSelectedRow() >= 0) {
                            scrollTo(currentIndex(), QAbstractItemView::EnsureVisible);
                        } else if (anchorRow >= 0 && anchorRow < this->model()->rowCount()) {
                            verticalScrollBar()->setValue(
                                contentYToScrollValue(rowYOffset(anchorRow) - anchorOffset));
                            viewport()->update();
                        }
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
    // Нажатие на переключатель — по номеру строки; строки сдвинулись до
    // отпускания — щелчок не засчитывается (иначе развернулась бы чужая).
    if (m_pendingChromeClick.row >= first)
        m_pendingChromeClick = {};

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
    if (m_newLinesCount > 0 && m_firstNewLineRow >= first)
        m_firstNewLineRow += count;
}

// Симметрично shiftRowKeyedState: после удаления count строк начиная с first
// состояние, привязанное к номерам строк, сдвигается вверх; указывавшее
// в удалённый диапазон — сбрасывается.
void LogListView::removeRowKeyedState(int first, int count)
{
    if (count <= 0)
        return;
    if (m_pendingChromeClick.row >= first)
        m_pendingChromeClick = {};
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

    // Новые строки: удалённые из непросмотренного диапазона уходят из счёта
    // (резидентный путь удаляет предварительную последнюю строку перед тем,
    // как перечитать её с дозаписью — иначе она посчиталась бы дважды).
    if (m_newLinesCount > 0) {
        if (lastRemoved < m_firstNewLineRow) {
            m_firstNewLineRow -= count;
        } else {
            const int removedNew = lastRemoved - qMax(first, m_firstNewLineRow) + 1;
            setNewLines(m_newLinesCount - removedNew, qMin(m_firstNewLineRow, first));
        }
    }
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
    // Исключение — вид, поставленный у низа ПЕРЕХОДОМ к строке (goToRow в
    // последние полэкрана): пользователь смотрит на строку, а не на хвост.
    const bool followScrollAfterAppend =
        m_followTail || (!m_holdAfterGoTo && isScrolledToBottom());

    if (!isAppend) {
        // Вставка в середину (слияние файлов по времени): полная инвалидация.
        invalidateRowState();
        rebuildHeightCache();
        updateScrollbar();
        if (followScrollAfterAppend)
            scrollToBottomFollow();
        countNewRows(first, last, followScrollAfterAppend);
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
    countNewRows(first, last, followScrollAfterAppend);
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
    m_collapseBadgeWidth = fm.horizontalAdvance(QString(kCollapseGlyph)) + kBadgeHPadding + kBadgeGap;
    // Резерв «+N» — по самой широкой из возможных подписей ShortCount: в
    // пропорциональном шрифте буквы K/M/G шире цифр.
    int widestCount = 0;
    for (const char* label : {"+9999", "+999K", "+9.9M", "+999M", "+9.9G"})
        widestCount = qMax(widestCount, fm.horizontalAdvance(QLatin1String(label)));
    m_hiddenToggleBadgeWidth = widestCount + kBadgeHPadding + kBadgeGap;

    // Инвалидируем кэши при смене шрифта
    invalidateRowState(-1, /*preserveTextLengths=*/true);
}

// ---------------------------------------------------------------------------
// changeEvent — реагирует на смену шрифта (например, при смене настроек)
// и палитры (смена темы).
// updateFontMetricsCache() вызывается всегда; деferred-пересчёт высот —
// только когда таймер уже создан (в конструкторе таймер создаётся после
// setFont, поэтому при первичной установке шрифта пересчёт не нужен —
// модели ещё нет).
void LogListView::changeEvent(QEvent* event)
{
    QListView::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateFontMetricsCache();  // сам инвалидирует кэши строк
        if (m_resizeDebounceTimer)
            updateFileColumn();     // ширина меток — от шрифта
        if (m_heightUpdateTimer) {  // null during constructor — timer not yet created
            m_heightUpdateTimer->start(0);
            viewport()->update();
        }
    } else if (event->type() == QEvent::PaletteChange) {
        // Смена темы: цвет текста по умолчанию берётся из палитры и уже
        // запечён в pixmap'ах строк. Высоты от палитры не зависят.
        m_rowStateCache.clear();
        clearRowPixmaps();
        viewport()->update();
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
    return fileColumnLeft() + m_fileColumnWidth;
}

// ---------------------------------------------------------------------------
// Колонка метки файла (U-23, часть 2): между жёлобом и текстом, одной ширины
// на вид — текст всех строк слитой вкладки на одной вертикали.
// ---------------------------------------------------------------------------
void LogListView::setFileLabelMode(FileLabelMode mode)
{
    if (m_fileLabelMode == mode)
        return;
    m_fileLabelMode = mode;
    updateFileColumn();
}

int LogListView::computeFileColumnWidth() const
{
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel || m_fileLabelMode == FileLabelMode::Hidden
        || logModel->labeledFiles().size() < 2)
        return 0;
    if (m_fileLabelMode == FileLabelMode::ColorOnly)
        return kFileStripeWidth + kFileStripeGap;
    return kFileStripeWidth + kFileLabelGap + fileLabelTextWidth(effectiveFileColumnChars())
         + kBadgeHPadding + kFileStripeGap;
}

int LogListView::fileLabelTextWidth(int chars) const
{
    return int(std::ceil(chars * m_charWidth));
}

int LogListView::effectiveFileColumnChars() const
{
    auto* logModel = qobject_cast<LogModel*>(model());
    int chars = m_fileColumnChars;
    if (chars <= 0) {
        // Авто: имена до kDefaultMaxChars видны целиком.
        chars = kMinFileColumnChars;
        if (logModel)
            for (const LogModel::LabeledFile& f : logModel->labeledFiles())
                chars = qMax(chars, int(f.file->shortName().size()));
        chars = qMin(chars, FileLabels::kDefaultMaxChars);
    }
    // Колонка не шире половины вьюпорта: тексту должно остаться место.
    const int most = qMax(kMinFileColumnChars, int(viewport()->width() / 2 / qMax<qreal>(1.0, m_charWidth)));
    return qBound(kMinFileColumnChars, chars, most);
}

void LogListView::setFileColumnChars(int chars)
{
    chars = qMax(0, chars);
    if (m_fileColumnChars == chars)
        return;
    m_fileColumnChars = chars;
    applyLeftColumns();
}

void LogListView::rebuildFileLabels()
{
    m_fileShortLabels.clear();
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel || m_fileLabelMode != FileLabelMode::Name || logModel->labeledFiles().size() < 2)
        return;
    QStringList names;
    for (const LogModel::LabeledFile& f : logModel->labeledFiles())
        names << f.file->shortName();
    const QStringList labels = FileLabels::build(names, effectiveFileColumnChars());
    for (int i = 0; i < names.size(); ++i)
        m_fileShortLabels.insert(logModel->labeledFiles()[i].file.get(), labels.value(i));
}

QRect LogListView::fileColumnHandleRect() const
{
    if (m_fileLabelMode != FileLabelMode::Name || m_fileColumnWidth <= 0)
        return QRect();
    return QRect(textAreaLeft() - kFileStripeGap, 0, kFileStripeGap + 2, viewport()->height());
}

void LogListView::updateFileColumn()
{
    applyLeftColumns();
}

void LogListView::scheduleLeftColumns()
{
    // Отложенно: сигналы набора файлов и числа строк приходят изнутри
    // rowsInserted / modelReset модели — раньше, чем view обработал вставку;
    // якорь вида, снятый в этот момент, считался бы по устаревшей геометрии.
    // Смена настроек и шрифта применяется сразу (updateFileColumn).
    if (!m_leftColumnsTimer) {
        m_leftColumnsTimer = new QTimer(this);
        m_leftColumnsTimer->setSingleShot(true);
        m_leftColumnsTimer->setInterval(0);
        connect(m_leftColumnsTimer, &QTimer::timeout, this, &LogListView::applyLeftColumns);
    }
    m_leftColumnsTimer->start();
}

void LogListView::applyLeftColumns()
{
    const int width = computeFileColumnWidth();
    const int numbers = computeLineNumberWidth();
    rebuildFileLabels();   // O(числа файлов): метки — по ширине колонки
    if (width == m_fileColumnWidth && numbers == m_lineNumberWidth) {
        viewport()->update();   // метки могли смениться при той же ширине
        return;
    }
    // Ширина текста сменилась: состояния и высоты строк заново, вид держит
    // якорь — выбранную строку, если она на экране (перенос по новой ширине
    // меняет высоты строк над ней, и она главнее якоря отложенного resize),
    // иначе якорь отложенного resize или первая видимая строка. Пересчёт — сразу, как у
    // setWordWrap: отложенный ждал бы, пока скроллбар, урезанный по новой
    // высоте контента, сдвинет вид, — и снял бы якорь заново по уже
    // неверной геометрии.
    int anchorRow = -1;
    int anchorOffset = 0;
    const int selected = userSelectedRow();
    if (selected >= 0 && captureVisibleRowOffset(selected, anchorOffset)) {
        anchorRow = selected;
        anchorOffset = qMax(0, anchorOffset);
    } else {
        if (m_anchorRow < 0)
            captureResizeAnchor();
        anchorRow = m_anchorRow;
        anchorOffset = m_anchorOffsetInViewport;
    }
    m_resizeDebounceTimer->stop();
    m_anchorRow = -1;
    m_anchorOffsetInViewport = 0;
    m_fileColumnWidth = width;
    m_lineNumberWidth = numbers;
    ++m_leftColumnRelayouts;
    invalidateRowState(-1, /*preserveTextLengths=*/true);
    rebuildHeightCache();
    updateScrollbar();
    if (anchorRow >= 0 && model() && anchorRow < model()->rowCount()) {
        refineHeightsAbove(anchorRow);
        updateScrollbar();
        verticalScrollBar()->setValue(targetScrollValueForRowOffset(anchorRow, anchorOffset));
    }
    markNewLinesSeen();
    viewport()->update();
}

// ---------------------------------------------------------------------------
// Колонка номеров строк (U-23, часть 3): сразу за жёлобом, левее метки
// файла. Номер — физическая строка файла (RecordKey::line), на первой линии
// строки с прилипанием к верху экрана; ширина — по разрядности наибольшего
// числа строк файла вкладки, не меньше четырёх цифр.
// ---------------------------------------------------------------------------
void LogListView::setLineNumbersVisible(bool visible)
{
    if (m_lineNumbersVisible == visible)
        return;
    m_lineNumbersVisible = visible;
    updateFileColumn();
}

int LogListView::lineNumberDigits() const
{
    auto* logModel = qobject_cast<LogModel*>(model());
    // Наибольший номер строки (у выдачи Search строк мало, а номера — из
    // глубины файла); не уже прежней, пока набор файлов тот же: полная
    // перезагрузка сбрасывает строки файла, и колонка не должна дёргаться.
    qint64 most = m_lineNumberHighWater;
    if (logModel)
        for (const LogModel::LabeledFile& f : logModel->labeledFiles())
            most = qMax(most, f.maxLine);
    m_lineNumberHighWater = most;
    int digits = 1;
    for (qint64 n = most; n >= 10; n /= 10)
        ++digits;
    return qMax(4, digits);
}

int LogListView::computeLineNumberWidth() const
{
    if (!m_lineNumbersVisible || !model())
        return 0;
    return int(std::ceil(lineNumberDigits() * m_charWidth)) + 2 * kLineNumberPadX;
}

void LogListView::onFileLinesChanged()
{
    // На событие батча — одно сравнение разрядности (инвариант 3); пересчёт
    // раскладки — только при её смене.
    if (m_lineNumbersVisible && computeLineNumberWidth() != m_lineNumberWidth)
        scheduleLeftColumns();
}

QString LogListView::lineNumberText(int row) const
{
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel || row < 0 || row >= logModel->rowCount())
        return QString();
    const RecordKey key = logModel->keyAt(row);
    return key.isValid() ? QString::number(key.line) : QString();
}

QRect LogListView::lineNumberScreenRect(qint64 rowTop, int rowHeight) const
{
    const int top = stickyTop(rowTop, rowHeight, singleRowHeight());
    return QRect(kLeftMarginWidth, top, m_lineNumberWidth, singleRowHeight());
}

QRect LogListView::lineNumberRect(int row) const
{
    if (m_lineNumberWidth <= 0 || !model() || row < 0 || row >= model()->rowCount())
        return QRect();
    return lineNumberScreenRect(rowYOffset(row) - scrollContentY(), getRowHeight(row));
}

void LogListView::paintLineNumber(QPainter& painter, int row, int rowTop, int rowHeight, bool selected)
{
    if (m_lineNumberWidth <= 0)
        return;
    const QString text = lineNumberText(row);
    if (text.isEmpty())
        return;
    const QRect r = lineNumberScreenRect(rowTop, rowHeight).adjusted(0, 0, -kLineNumberPadX, 0);
    painter.save();
    painter.setPen(selected ? palette().color(QPalette::Text) : AppTheme::lineNumberFg(palette()));
    painter.setClipRect(QRect(kLeftMarginWidth, r.top(), m_lineNumberWidth, r.height()));
    painter.drawText(r, Qt::AlignVCenter | Qt::AlignRight, text);
    painter.restore();
}

const LogModel::LabeledFile* LogListView::rowFile(int row) const
{
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel || m_fileColumnWidth <= 0)
        return nullptr;
    const auto ptr = logModel->data(logModel->index(row, 0), LogModel::SourceFileRole).value<quintptr>();
    return logModel->labeledFile(reinterpret_cast<const LogFile*>(ptr));
}

QStringList LogListView::fileLabelLinesFor(const LogModel::LabeledFile& file, int rowHeight) const
{
    // У строки в несколько линий имя пишется целиком — на стольких линиях,
    // сколько у строки есть; не помещается и так — короткая метка.
    const int chars = effectiveFileColumnChars();
    const QString name = file.file->shortName();
    const int rowLines = qMax(1, (rowHeight - kLineVerticalPadding) / qMax(1, m_lineHeight));
    if (rowLines >= 2 && name.size() > chars) {
        const QStringList lines = FileLabels::wrap(name, chars, rowLines);
        if (!lines.isEmpty())
            return lines;
    }
    return { m_fileShortLabels.value(file.file.get(), name) };
}

QRect LogListView::fileLabelScreenRect(const QStringList& lines, qint64 rowTop, int rowHeight) const
{
    const int x = fileColumnLeft() + kFileStripeWidth + kFileLabelGap;
    const int most = m_fileColumnWidth - kFileStripeWidth - kFileLabelGap - kFileStripeGap;
    if (lines.size() > 1) {
        // Блок линий с шагом строк текста; прилипает к верху целиком.
        const int h = int(lines.size()) * m_lineHeight;
        const int slot = h + kLineVerticalPadding;
        return QRect(x, stickyTop(rowTop, rowHeight, slot) + kLineVerticalPadding / 2, most, h);
    }
    const int h = m_lineHeight - 2;
    const int w = qMin(fileLabelTextWidth(int(lines.value(0).size())) + kBadgeHPadding, most);
    const int top = stickyTop(rowTop, rowHeight, singleRowHeight()) + (singleRowHeight() - h) / 2;
    return QRect(x, top, w, h);
}

QStringList LogListView::fileLabelLines(int row) const
{
    if (m_fileLabelMode != FileLabelMode::Name || !model() || row < 0 || row >= model()->rowCount())
        return {};
    const LogModel::LabeledFile* file = rowFile(row);
    return file ? fileLabelLinesFor(*file, getRowHeight(row)) : QStringList();
}

QRect LogListView::fileLabelRect(int row) const
{
    const QStringList lines = fileLabelLines(row);
    if (lines.isEmpty())
        return QRect();
    return fileLabelScreenRect(lines, rowYOffset(row) - scrollContentY(), getRowHeight(row));
}

void LogListView::paintFileColumn(QPainter& painter, int row, int rowTop, int rowHeight)
{
    const LogModel::LabeledFile* file = rowFile(row);
    if (!file)
        return;
    const QColor color = AppTheme::fileColor(file->colorOrdinal, viewport()->palette());
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    // Полоса — на всю высоту строки: цвет файла виден в любой её части.
    painter.fillRect(QRect(fileColumnLeft(), rowTop, kFileStripeWidth, rowHeight), color);
    if (m_fileLabelMode == FileLabelMode::Name) {
        const QStringList lines = fileLabelLinesFor(*file, rowHeight);
        const QRect r = fileLabelScreenRect(lines, rowTop, rowHeight);
        painter.fillRect(r, color);
        painter.setPen(AppTheme::instance().badgeFg);
        painter.setClipRect(r);
        const int lineH = r.height() / int(lines.size());
        for (int i = 0; i < lines.size(); ++i)
            painter.drawText(QRect(r.left() + kBadgeHPadding / 2, r.top() + i * lineH,
                                   r.width() - kBadgeHPadding / 2, lineH),
                             Qt::AlignVCenter | Qt::AlignLeft, lines[i]);
    }
    painter.restore();
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
    const QModelIndex index = model()->index(row, 0);
    const bool isNew = model()->data(index, LogModel::IsNewRole).toBool();
    // Маркер начала записи "›", выровненный по первой строке элемента
    QRect markerRect(gutterRect.left(), gutterRect.top(),
                     gutterRect.width() - 1, singleRowHeight());
    // Закладка (R-2): лента-флажок цвета закладок на всю высоту первой
    // строки, «›» поверх неё — светлым. Роль отвечает за O(1).
    const bool bookmarked = model()->data(index, LogModel::BookmarkRole).toBool();
    if (bookmarked) {
        const QRectF r = QRectF(markerRect).adjusted(1, 1, 0, -1);
        const qreal notch = qMin<qreal>(4.0, r.height() / 3.0);
        QPolygonF ribbon;
        ribbon << r.topLeft() << r.topRight() << r.bottomRight()
               << QPointF(r.center().x(), r.bottom() - notch) << r.bottomLeft();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(AppTheme::instance().gutterBookmark);
        painter.drawPolygon(ribbon);
        // «›» — контрастным к ленте цветом: её цвет пользователь задаёт сам.
        const QColor ribbonColor = AppTheme::instance().gutterBookmark;
        painter.setPen(isNew ? AppTheme::instance().gutterNewEntry
                             : (ribbonColor.lightnessF() > 0.6 ? QColor(Qt::black)
                                                               : QColor(Qt::white)));
    } else {
        painter.setPen(isNew ? AppTheme::instance().gutterNewEntry : AppTheme::instance().gutterMarker);
    }
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
    // Метка файла — колонкой слева (textAreaLeft), справа только переключатель.
    const int fileBadgeWidth = 0;
    const int baseAvailableTextWidth = effectiveVpWidth - fileBadgeWidth - kTextBadgeGap;
    const int fullTextWidth = calculateWidth(QStringView(state.text));

    state.showCollapseBadge = expandedRow && (forceCollapseBadge ||
        (baseAvailableTextWidth > 0 && fullTextWidth > baseAvailableTextWidth));

    // Столбец переключателя переноса — постоянной ширины: у свёрнутой строки
    // резерв «+9999» (сокращённое «+N» в него помещается всегда, число
    // скрытых символов считается по той же ширине, что и обрезка текста), у
    // развёрнутой — «−». Короткая строка столбца не получает.
    const int toggleColumn = state.showCollapseBadge ? m_collapseBadgeWidth
                           : (!expandedRow ? m_hiddenToggleBadgeWidth : 0);
    const int availableTextWidth = effectiveVpWidth - fileBadgeWidth - toggleColumn - kTextBadgeGap;
    state.multiLine = expandedRow && availableTextWidth > 0 && fullTextWidth > availableTextWidth;

    // Шаг 2: Вычисляем высоту с учётом реальной доступной ширины
    state.height = computeRowHeight(state.text, state.multiLine, availableTextWidth);

    // Шаг 3: Собираем плашки с точным расчётом скрытых символов
    QList<BadgeSpec> badgeSpecs = collectBadgeSpecs(row, state.text,
        availableTextWidth, state.multiLine, state.showCollapseBadge, state.hiddenChars);
    state.badges = layoutBadges(badgeSpecs);
    for (int i = 0; i < state.badges.size(); ++i)
        if (state.badges[i].spec.type == BadgeType::HiddenToggle)
            state.toggleIndex = i;

    // Шаг 4: Ширина, зарезервированная справа: метка файла и столбец
    // переключателя целиком (а не по ширине самой плашки) — текст обрывается
    // ровно там, где кончаются посчитанные видимые символы.
    state.badgesWidth = fileBadgeWidth + (state.toggleIndex >= 0 ? toggleColumn : 0);

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
        if (actualHeight != state.height)
            state.height = actualHeight;  // Y плашек от высоты строки не зависит
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

bool LogListView::refineHeightsAbove(int row) {
    if (m_uniformHeights || m_heightsDirty || !model()) return false;
    if (row < 0 || row >= m_rowHeights.size()) return false;

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
    return changed;
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

// Отметки R-6: строка → значение скроллбара. Линейную сумму rowYOffset
// (высоты ещё не построены) здесь звать нельзя — отметок десятки тысяч.
qreal LogListView::markScrollValue(int row) const {
    qint64 y;
    if (m_uniformHeights) {
        y = qint64(row) * singleRowHeight();
    } else if (!m_heightsDirty && row >= 0 && row < m_rowPrefixY.size()) {
        y = m_rowPrefixY[row];
    } else {
        const int rows = model() ? model()->rowCount() : 0;
        if (rows <= 0)
            return 0;
        y = qint64(qreal(estimateTotalHeightForDirtyCache(rows)) * row / rows);
    }
    return m_scrollScale > 1.0 ? qreal(y) / m_scrollScale : qreal(y);
}

qint64 LogListView::markGeometryStamp() const {
    // Всё, от чего зависит markScrollValue при тех же строках.
    quint64 stamp = m_uniformHeights ? 1 : (m_heightsDirty ? 2 : 3);
    stamp = stamp * 1000003u + quint64(m_totalHeight);
    stamp = stamp * 1000003u + quint64(m_prefixDirtyFrom);
    stamp = stamp * 1000003u + quint64(singleRowHeight());
    return qint64(stamp);
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

    // С переносом строк высоты до первой отрисовки — оценки. Отрисовка идёт
    // от первой видимой строки вниз по РЕАЛЬНЫМ высотам, поэтому прокрутку
    // считаем по точным высотам самой строки и тех, что окажутся на экране над
    // ней: иначе после отрисовки строка уехала бы за край вьюпорта (Down,
    // PageDown, End), а до последней не хватило бы диапазона скроллбара.
    if (refineHeightsAbove(row))
        updateScrollbar();

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
    // Плашка скрытых символов для однострочного режима
    // Переключатель переноса — приглушённый (AppTheme::wrapToggle*): яркой
    // остаётся только информация (метка файла).
    const QPalette& pal = viewport()->palette();
    if (showCollapseBadge) {
        BadgeSpec b;
        b.type = BadgeType::HiddenToggle;
        b.text = QString(kCollapseGlyph);
        b.bg = AppTheme::wrapToggleBg(pal);
        b.fg = AppTheme::wrapToggleFg(pal);
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
                // До 9 999 — точно, дальше «12K», «3.4M»: всегда в резерве
                // «+9999»; точное число — в подсказке.
                b.text = QStringLiteral("+") + ShortCount::format(hiddenCount, 10000, false);
                b.bg = AppTheme::wrapToggleBg(pal);
                b.fg = AppTheme::wrapToggleFg(pal);
                specs.prepend(b);
            }
        }
    }

    return specs;
}

QList<BadgeLayout> LogListView::layoutBadges(const QList<BadgeSpec>& specs) const {
    QList<BadgeLayout> layouts;
    if (specs.isEmpty()) return layouts;

    QFontMetrics fm(font());
    int vpWidth = viewport()->width();
    int x = vpWidth - kBadgeGap;
    // Y — внутри слота первой визуальной линии (высота singleRowHeight):
    // на экране слот прилипает к верху вьюпорта (badgeScreenRect).
    const int h = fm.height() - 2;
    const int top = (singleRowHeight() - h) / 2;

    for (int i = specs.size() - 1; i >= 0; --i) {
        const auto& spec = specs[i];
        int w = fm.horizontalAdvance(spec.text) + kBadgeHPadding;
        QRect r(x - w + 1, top, w, h);
        x = r.left() - kBadgeGap;
        layouts.prepend({spec, r});
    }
    return layouts;
}

int LogListView::stickyTop(qint64 rowTop, int rowHeight, int itemHeight)
{
    // clamp(0, rowTop, rowTop + rowHeight − itemHeight): элемент у верха
    // строки, пока он на экране; строка ушла верхом за экран — элемент у
    // верха вьюпорта; дальше он уходит вместе с низом строки. Строка ниже
    // элемента — у её верха.
    const qint64 lo = rowTop;
    const qint64 hi = rowTop + rowHeight - itemHeight;
    qint64 y = 0;
    if (hi < lo || y < lo)
        y = lo;
    else if (y > hi)
        y = hi;
    constexpr qint64 kLimit = std::numeric_limits<int>::max() / 2;
    return int(std::clamp<qint64>(y, -kLimit, kLimit));
}

QRect LogListView::badgeScreenRect(const BadgeLayout& badge, qint64 rowTop, int rowHeight) const
{
    QRect r = badge.rect;
    r.moveTop(stickyTop(rowTop, rowHeight, singleRowHeight()) + badge.rect.top());
    return r;
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

void LogListView::showSearchMatch(int row, const TextQuery& query)
{
    if (row < 0 || query.isEmpty() || !model() || row >= model()->rowCount()) {
        clearSearchMatch();
        return;
    }

    // Снимаем подсветку с предыдущей найденной строки.
    if (m_searchMatchRow >= 0 && m_searchMatchRow != row)
        removeRowPixmap(m_searchMatchRow);

    m_searchMatchRow = row;
    // Тот же запрос, что нашёл строку (слово целиком и регекс — регексом):
    // залито ровно найденное.
    m_searchHighlighter.setPatterns({query.highlightPattern(AppTheme::instance().searchMatch)});

    // Свёрнутую строку раскрываем в многострочный режим, чтобы были видны
    // все вхождения (включая попавшие в скрытую часть). toggleRowMultiLine
    // сам пересчитает высоты и сохранит позицию скролла.
    if (!isRowMultiLine(row))
        toggleRowMultiLine(row);

    removeRowPixmap(row);
    viewport()->update();
}

void LogListView::goToRow(int row, NavKind kind)
{
    if (!model() || row < 0 || row >= model()->rowCount())
        return;
    noteUserAction();
    const QModelIndex index = model()->index(row, 0);
    recordNavJump(-1, row, kind);
    // Follow-tail вернул бы вид к хвосту на следующей же дописанной порции:
    // программная прокрутка его сама не снимает (только ползунок и колесо).
    if (m_followTail && row < model()->rowCount() - 1)
        setFollowTail(false);
    selectAsUserChoice(row);
    scrollTo(index, QAbstractItemView::PositionAtCenter);
    // Строку из последних полэкрана отцентрировать нельзя — вид встаёт у
    // самого низа, и «прилипание» к хвосту увело бы его на первой же
    // дозаписи. Переход к самой последней строке — это переход к хвосту.
    m_holdAfterGoTo = row < model()->rowCount() - 1;
}

void LogListView::recordNavJump(int fromRow, int row, NavKind kind)
{
    // История переходов: откуда ушли — выбор пользователя (инвариант 10), а
    // без него — первая строка экрана. Пишется ДО перехода.
    if (kind == NavKind::History)
        return;
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel)
        return;
    const int rows = model()->rowCount();
    if (fromRow < 0 || fromRow >= rows)
        fromRow = userSelectedRow();
    if (fromRow < 0 && rows > 0)
        fromRow = qBound(0, rowAtY(scrollContentY()), rows - 1);
    logModel->navHistory().recordJump(fromRow, row, kind);
}

void LogListView::goToRowAt(int row, int offset, NavKind kind, int fromRow)
{
    if (!model() || row < 0 || row >= model()->rowCount())
        return;
    noteUserAction();
    recordNavJump(fromRow, row, kind);
    // Выбор пользователя, место на экране и снятие follow-tail — как у
    // восстановленного якоря.
    setViewAnchor({ row, offset, true });
}

void LogListView::selectAsUserChoice(int row)
{
    // setCurrentIndex выделяет строку (ClearAndSelect) и через currentChanged
    // обновляет m_persistentSelection. Но если строка уже текущая (её мог
    // назначить Qt по фокусу, без выделения и без якоря), currentChanged не
    // придёт — якорь ставим сами.
    setCurrentIndex(model()->index(row, 0));
    if (auto* logModel = qobject_cast<LogModel*>(model())) {
        const LogModel::EntryKey key = logModel->keyForRow(row);
        if (key.logicalEntryId >= 0) {
            m_persistentSelection.logicalEntryId = key.logicalEntryId;
            m_persistentSelection.sourceFile = key.sourceFile;
        }
    }
}

bool LogListView::placeAwaitedAnchor()
{
    auto* logModel = qobject_cast<LogModel*>(model());
    // Пользователь уже смотрит сам или вид идёт за хвостом — ждать нечего.
    if (!logModel || m_userActions != m_awaitedAnchor.userActions || m_followTail) {
        m_awaitedAnchor = {};
        return true;
    }
    const int row = logModel->rowForKey(m_awaitedAnchor.key);
    if (row < 0)
        return false; // строка ещё не пришла (или скрыта фильтром — settleAwaitedAnchor)
    const AwaitedAnchor anchor = std::exchange(m_awaitedAnchor, {});
    ++m_positionEpoch;
    if (anchor.selected) {
        selectAsUserChoice(row);
        m_selection.anchorRow = m_selection.activeRow = row;
    }
    if (refineHeightsAbove(row))
        updateScrollbar();
    const int offset = anchor.centered // как после reset — по центру
        ? qMax(0, (viewport()->height() - getRowHeight(row)) / 2) : anchor.offset;
    verticalScrollBar()->setValue(targetScrollValueForRowOffset(row, offset));
    viewport()->update();
    return true;
}

void LogListView::settleAwaitedAnchor()
{
    // Пустая модель — данных ещё нет (конверсия только началась): ждём.
    if (!m_awaitedAnchor.armed || !model() || model()->rowCount() == 0 || placeAwaitedAnchor())
        return;
    auto* logModel = qobject_cast<LogModel*>(model());
    const AwaitedAnchor anchor = std::exchange(m_awaitedAnchor, {});
    const int row = logModel ? logModel->nearestRowForKey(anchor.key) : -1;
    if (row < 0)
        return;
    ++m_positionEpoch;
    if (refineHeightsAbove(row))
        updateScrollbar();
    verticalScrollBar()->setValue(targetScrollValueForRow(row, QAbstractItemView::PositionAtCenter));
    viewport()->update();
}

void LogListView::noteUserAction()
{
    ++m_userActions;
    ++m_positionEpoch;
    m_holdRestoredOffset = false;
}

LogListView::ViewAnchor LogListView::viewAnchor() const
{
    ViewAnchor anchor;
    const int rows = model() ? model()->rowCount() : 0;
    if (rows <= 0)
        return anchor;
    const qint64 scrollY = scrollContentY();
    const int selected = userSelectedRow();
    if (selected >= 0) {
        anchor.row = selected;
        anchor.selected = true;
        // Выбранная строка вне экрана: смещение — «по центру», как её
        // поставил бы goToRow (разность координат могла бы не влезть в int).
        if (!captureVisibleRowOffset(selected, anchor.offset))
            anchor.offset = qMax(0, (viewport()->height() - getRowHeight(selected)) / 2);
        return anchor;
    }
    anchor.row = qBound(0, rowAtY(scrollY), rows - 1);
    anchor.offset = int(rowYOffset(anchor.row) - scrollY);
    return anchor;
}

void LogListView::setViewAnchor(const ViewAnchor& anchor)
{
    if (!model() || anchor.row < 0 || anchor.row >= model()->rowCount())
        return;
    const int rows = model()->rowCount();
    m_awaitedAnchor = {};
    ++m_positionEpoch;
    if (m_followTail && anchor.row < rows - 1)
        setFollowTail(false);
    if (anchor.selected)
        selectAsUserChoice(anchor.row);
    // Как scrollTo: точные высоты строки и тех, что окажутся над ней.
    if (refineHeightsAbove(anchor.row))
        updateScrollbar();
    // Выбранная строка — на экране: место, снятое в окне выше нынешнего,
    // прижимается к низу (вьюпорт ещё не разложен — как есть).
    int offset = anchor.offset;
    const int rowH = getRowHeight(anchor.row);
    if (anchor.selected && viewport()->height() > rowH)
        offset = qBound(0, offset, viewport()->height() - rowH);
    const int target = targetScrollValueForRowOffset(anchor.row, offset);
    if (target != verticalScrollBar()->value())
        verticalScrollBar()->setValue(target);
    viewport()->update();
    m_holdAfterGoTo = anchor.row < rows - 1;
    m_holdRestoredOffset = anchor.selected;
    m_restoredOffset = offset;
}

int LogListView::userSelectedRow() const
{
    const QModelIndex current = currentIndex();
    if (!current.isValid() || !selectionModel()
        || !selectionModel()->isRowSelected(current.row(), QModelIndex()))
        return -1;
    return current.row();
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

// Заливки совпадений + перерисовка попавшего в них текста контрастным цветом.
// Идёт ПОСЛЕ основной отрисовки: заливка затирает уже нарисованные буквы, а
// повторный проход рисует их поверх цветом, читаемым на этой заливке (пен из
// HighlightPalette::textOn). Так подсветка видна при любой теме и при любом
// цвете, который выбрал пользователь, — раскраска SyntaxHighlighter внутри
// совпадения уступает место контрасту (как в выделении).
//
// Геометрия повторяет drawSelectionHighlight: однострочный режим клипуется по
// visibleChars, многострочный идёт по фрагментам. Координаты текста в точности
// повторяют drawLogLine, а токены передаются те же — сегменты ложатся пиксель
// в пиксель поверх исходных.
void LogListView::drawMatchTextOverlay(QPainter& painter, const QRect& rect, const QString& text,
                                       const RowState& state, const QVector<HighlightSpan>& spans,
                                       const QList<HighlightToken>& tokens) const
{
    const QRectF clip = painter.hasClipping() ? painter.clipBoundingRect() : QRectF();
    const QFontMetrics fm = fontMetrics(); // тот же источник координат, что и в drawLogLine

    for (const auto& span : spans) {
        if (!span.color.isValid())
            continue;
        const QColor pen = HighlightPalette::textOn(span.color);
        int spanBegin  = span.start;
        int spanFinish = span.start + span.length;

        if (!state.multiLine) {
            spanBegin  = qMin(spanBegin,  state.visibleChars);
            spanFinish = qMin(spanFinish, state.visibleChars);
            if (spanBegin >= spanFinish)
                continue;
            const QStringView textView(text);
            const int x1 = rect.left() + kTextPaddingX + textWidthUntil(textView, spanBegin);
            const int x2 = rect.left() + kTextPaddingX + textWidthUntil(textView, spanFinish);
            const QRect fillRect(x1, rect.top() + 1, x2 - x1, rect.height() - 2);
            painter.fillRect(fillRect, span.color);

            painter.save();
            painter.setClipRect(fillRect, Qt::IntersectClip);
            drawTextWithHighlights(painter, rect.left() + kTextPaddingX,
                                   rect.bottom() - fm.descent() - 1,
                                   textView.left(state.visibleChars), 0, tokens, pen);
            painter.restore();
            continue;
        }

        for (const auto& fragment : state.fragments) {
            const int fragTop = rect.top() + fragment.rect.top();
            if (!clip.isNull()
                && (fragTop + fragment.rect.height() < clip.top() || fragTop > clip.bottom()))
                continue;
            const int fragSelStart = qMax(spanBegin,  fragment.startPos) - fragment.startPos;
            const int fragSelEnd   = qMin(spanFinish, fragment.startPos + fragment.length) - fragment.startPos;
            if (fragSelEnd <= fragSelStart)
                continue;
            const int x1 = rect.left() + fragment.rect.left() + textWidthUntil(fragment.text, fragSelStart);
            const int x2 = rect.left() + fragment.rect.left() + textWidthUntil(fragment.text, fragSelEnd);
            const QRect fillRect(x1, fragTop, x2 - x1, fragment.rect.height());
            painter.fillRect(fillRect, span.color);

            painter.save();
            painter.setClipRect(fillRect, Qt::IntersectClip);
            drawTextWithHighlights(painter, rect.left() + fragment.rect.left(),
                                   fragTop + fm.height() - fm.descent(),
                                   fragment.text, fragment.startPos, tokens, pen);
            painter.restore();
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

    // Совпадения фильтров/поиска — ПОСЛЕ текста: заливка кладётся поверх уже
    // нарисованных букв, и они тут же перерисовываются контрастным к ней цветом.
    if (!matchSpans.isEmpty())
        drawMatchTextOverlay(painter, rect, text, state, matchSpans, tokens);

    // Подсветка результата поиска (тулбарные Next/Prev) — только в найденной
    // строке. Не кэшируем: ровно одна строка, и подсветка живёт только пока
    // активен поиск. Рисуется последней — приоритет над подсветкой фильтров.
    if (state.row == m_searchMatchRow && !m_searchHighlighter.isEmpty()) {
        const QVector<HighlightSpan> spans = m_searchHighlighter.computeSpans(text);
        if (!spans.isEmpty())
            drawMatchTextOverlay(painter, rect, text, state, spans, tokens);
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

    const bool wasExpanded = isRowMultiLine(row);
    int preservedViewportOffset = 0;
    const bool keepRowAnchor = captureVisibleRowOffset(row, preservedViewportOffset);
    // Сворачивание строки, верх которой выше экрана (читали середину
    // гигантской строки): с прежним смещением свёрнутая строка оказалась бы
    // над экраном — она встаёт у верха вьюпорта. Верх был на экране — строка
    // остаётся на своём месте; разворачивание — тоже на месте.
    if (keepRowAnchor && wasExpanded && preservedViewportOffset < 0)
        preservedViewportOffset = 0;

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
            paintGutter(painter, row, QRect(0, currentY, kLeftMarginWidth, rowHeight));
            paintLineNumber(painter, row, currentY, rowHeight,
                            selectionModel() && selectionModel()->isSelected(idx));
            paintFileColumn(painter, row, currentY, rowHeight);

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

            // Служебные элементы справа (U-23). Рейка развёрнутой строки — на
            // всю её высоту в столбце «−»: видно, где строка кончается, и
            // свернуть её можно из любой точки. Плашки — на первой линии,
            // прилипая к верху экрана (stickyTop), пока строка его пересекает.
            if (state.toggleIndex >= 0 && state.showCollapseBadge) {
                const QRect toggle = state.badges[state.toggleIndex].rect;
                const int x = toggle.left() + toggle.width() / 2;
                const int top = qMax(currentY + 2, visibleRect.top());
                const int bottom = qMin(currentY + rowHeight - 3, visibleRect.bottom());
                if (bottom > top) {
                    painter.save();
                    painter.setRenderHint(QPainter::Antialiasing, false);
                    painter.setPen(QPen(AppTheme::wrapRail(palette()), 1));
                    painter.drawLine(x, top, x, bottom);
                    painter.restore();
                }
            }
            for (const auto& bl : state.badges) {
                const QRect badgeRect = badgeScreenRect(bl, currentY, rowHeight);
                // Цвета переключателя — из палитры в момент отрисовки, а не из
                // кэша состояния строки: порядок PaletteChange у view и его
                // вьюпорта зависит от платформы, и кэш мог успеть заполниться
                // цветами прежней темы.
                const bool toggle = bl.spec.type == BadgeType::HiddenToggle;
                painter.save();
                painter.setPen(Qt::NoPen);
                painter.setBrush(toggle ? AppTheme::wrapToggleBg(palette()) : bl.spec.bg);
                painter.drawRect(badgeRect);
                painter.setPen(toggle ? AppTheme::wrapToggleFg(palette()) : bl.spec.fg);
                painter.drawText(badgeRect.adjusted(kBadgeHPadding/2, 0, -kBadgeHPadding/2, 0), Qt::AlignVCenter | Qt::AlignLeft, bl.spec.text);
                painter.restore();
            }
        }

        currentY += rowHeight;
    }

    // Граница колонки метки файла — её тянут мышью (fileColumnHandleRect).
    if (const QRect handle = fileColumnHandleRect(); !handle.isEmpty())
        painter.fillRect(QRect(textAreaLeft() - 2, visibleRect.top(), 1, visibleRect.height()),
                         AppTheme::wrapRail(palette()));

    // Если реальные высоты отличались от оценок — перестроим prefix-суммы через 50 мс
    if (anyHeightChanged && !m_heightUpdateTimer->isActive()) {
        m_heightUpdateTimer->start(50);
    }
}

void LogListView::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    placeNewLinesIndicator();
    // Предел ширины колонки файла — половина вьюпорта.
    if (m_fileColumnWidth > 0 && computeFileColumnWidth() != m_fileColumnWidth)
        scheduleLeftColumns();
    
    if (m_anchorRow < 0)
        captureResizeAnchor();

    // Debounce: отложенный пересчёт после окончания resize
    m_resizeDebounceTimer->start(150);
}

// Якорь отложенного пересчёта после resize: первая видимая строка и её
// смещение — по текущей (ещё не пересчитанной) геометрии.
void LogListView::captureResizeAnchor() {
    m_anchorRow = -1;
    m_anchorOffsetInViewport = 0;
    if (!model() || model()->rowCount() <= 0)
        return;
    const qint64 scrollY = scrollContentY();
    qint64 rowTop = 0;
    if (getRowAtContentY(scrollY, m_anchorRow, rowTop))
        m_anchorOffsetInViewport = int(rowTop - scrollY);
    else
        m_anchorRow = -1;
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
    noteUserAction();
    // Отпускание прежнего нажатия на переключатель могло уйти другому
    // виджету (всплывающее окно, смена окна) — новое нажатие его отменяет.
    m_pendingChromeClick = {};
    // Боковые кнопки мыши — история переходов (R-7); выделение не трогаем.
    if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        emit historyNavigationRequested(event->button() == Qt::ForwardButton);
        event->accept();
        return;
    }
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

    // 0. Граница колонки метки файла: протяжка меняет её ширину.
    if (fileColumnHandleRect().contains(event->pos())) {
        m_columnDrag = true;
        event->accept();
        return;
    }

    // 1. Игнорируем если уже идёт выделение
    if (m_selection.isDragging) return;

    // 2. Определяем строку под курсором
    RowHitInfo hitInfo = getRowHitInfoFromMouse(event->pos());
    if (!hitInfo.valid) {
        QListView::mousePressEvent(event);
        return;
    }

    // 3. Служебный элемент строки (U-23): переключатель и рейка действуют по
    // отпусканию без протяжки (mouseReleaseEvent); метка файла — без
    // действия. Выделение текста здесь не начинается.
    const RowChromeHit chrome = rowChromeAt(event->pos());
    if ((chrome.kind == RowChrome::LineNumber || chrome.kind == RowChrome::FileLabel)
        && event->pos().x() < textAreaLeft()) {
        // Номер и метка файла не выделяются: щелчок выбирает строку.
        m_selection.clear();
        selectAsUserChoice(chrome.row);
        viewport()->update();
        event->accept();
        return;
    }
    if (chrome.kind != RowChrome::None) {
        m_pendingChromeClick = {chrome.row, chrome.kind, event->pos()};
        event->accept();
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
    if (m_columnDrag) {
        // Ширина — в символах, от левого края метки до курсора.
        const int labelLeft = fileColumnLeft() + kFileStripeWidth + kFileLabelGap + kBadgeHPadding;
        const int most = qMax(kMinFileColumnChars, int(viewport()->width() / 2 / qMax<qreal>(1.0, m_charWidth)));
        const int chars = qBound(kMinFileColumnChars,
                                 qRound((event->pos().x() - labelLeft) / qMax<qreal>(1.0, m_charWidth)), most);
        setFileColumnChars(chars);
        event->accept();
        return;
    }
    if (event->buttons() == Qt::NoButton) {
        // Движение без кнопок (mouse tracking): курсор над границей колонки.
        const bool over = fileColumnHandleRect().contains(event->pos());
        if (over != m_columnCursor) {
            m_columnCursor = over;
            if (over)
                viewport()->setCursor(Qt::SplitHCursor);
            else
                viewport()->unsetCursor();
        }
        return;
    }
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
    if (m_columnDrag) {
        // Ширина — общая настройка: её берут все виды (и следующий запуск).
        m_columnDrag = false;
        AppSettings::instance().setFileColumnChars(m_fileColumnChars);
        event->accept();
        return;
    }
    if (m_pendingChromeClick.row >= 0) {
        const PendingChromeClick pending = m_pendingChromeClick;
        m_pendingChromeClick = {};
        if (event->button() == Qt::LeftButton
            && (event->pos() - pending.pressPos).manhattanLength() < QApplication::startDragDistance()) {
            const RowChromeHit hit = rowChromeAt(event->pos());
            if (hit.row == pending.row
                && (hit.kind == RowChrome::WrapToggle || hit.kind == RowChrome::WrapRail))
                activateRowChrome(hit.row, hit.kind);
        }
        event->accept();
        return;
    }
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
    // Быстрый второй щелчок боковой кнопкой приходит двойным щелчком.
    if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        emit historyNavigationRequested(event->button() == Qt::ForwardButton);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QListView::mouseDoubleClickEvent(event);
        return;
    }
    // Двойной щелчок по границе колонки файла — ширина «авто».
    if (fileColumnHandleRect().contains(event->pos())) {
        m_columnDrag = false;
        setFileColumnChars(0);
        AppSettings::instance().setFileColumnChars(0);
        event->accept();
        return;
    }
    // Двойной щелчок по переключателю или рейке — второй щелчок по ним (по
    // отпусканию), а не выделение слова у края строки.
    if (const RowChromeHit chrome = rowChromeAt(event->pos()); chrome.kind != RowChrome::None) {
        m_pendingChromeClick = {chrome.row, chrome.kind, event->pos()};
        event->accept();
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

// ---------------------------------------------------------------------------
// Служебные элементы строки (U-23): hit-test, подсказки, действие.
// ---------------------------------------------------------------------------
LogListView::RowChromeHit LogListView::rowChromeAt(const QPoint& pos) const
{
    RowChromeHit hit;
    if (!model() || !viewport()->rect().contains(pos))
        return hit;
    const qint64 scrollY = scrollContentY();
    int row = -1;
    qint64 rowTopContent = 0;
    if (!getRowAtContentY(pos.y() + scrollY, row, rowTopContent))
        return hit;
    const RowState state = getRowState(row);
    const qint64 rowTop = rowTopContent - scrollY;
    // Колонка номеров строк: номер на первой линии, без действия по щелчку
    // (кроме выбора строки).
    if (m_lineNumberWidth > 0 && pos.x() >= kLeftMarginWidth && pos.x() < fileColumnLeft()) {
        hit.kind = RowChrome::LineNumber;
        hit.row = row;
        hit.rect = lineNumberScreenRect(rowTop, state.height);
        return hit;
    }
    // Колонка метки файла: полоса и метка — путь в подсказке; щелчка нет.
    if (pos.x() >= fileColumnLeft() && pos.x() < textAreaLeft()) {
        if (const LogModel::LabeledFile* file = rowFile(row)) {
            hit.kind = RowChrome::FileLabel;
            hit.row = row;
            hit.rect = QRect(fileColumnLeft(), int(qMax<qint64>(0, rowTop)), m_fileColumnWidth,
                             int(qMin<qint64>(viewport()->height(), rowTop + state.height)
                                 - qMax<qint64>(0, rowTop)));
            hit.toolTip = file->file->displayPath();
        }
        return hit;
    }
    for (int i = 0; i < state.badges.size(); ++i) {
        const QRect r = badgeScreenRect(state.badges[i], rowTop, state.height);
        if (!r.contains(pos))
            continue;
        hit.row = row;
        hit.rect = r;
        if (i == state.toggleIndex) {
            hit.kind = RowChrome::WrapToggle;
            hit.toolTip = wrapToggleToolTip(state);
        } else {
            hit.kind = RowChrome::FileLabel;
            hit.toolTip = state.badges[i].spec.text;
        }
        return hit;
    }
    // Рейка: столбец «−» на всю высоту развёрнутой строки.
    if (state.toggleIndex >= 0 && state.showCollapseBadge) {
        const QRect toggle = state.badges[state.toggleIndex].rect;
        if (pos.x() >= toggle.left() && pos.x() <= toggle.right()) {
            const qint64 top = qMax<qint64>(0, rowTop);
            const qint64 bottom = qMin<qint64>(viewport()->height(), rowTop + state.height);
            hit.kind = RowChrome::WrapRail;
            hit.row = row;
            hit.rect = QRect(toggle.left(), int(top), toggle.width(), int(qMax<qint64>(0, bottom - top)));
            hit.toolTip = wrapToggleToolTip(state);
        }
    }
    return hit;
}

QString LogListView::wrapToggleToolTip(const RowState& state) const
{
    const QKeySequence key = ShortcutManager::instance().sequence(QStringLiteral("toggleLineWrap"));
    const QString orKey = key.isEmpty() ? QString()
        : tr(" or %1").arg(key.toString(QKeySequence::NativeText));
    if (state.showCollapseBadge)
        return tr("Click%1 to collapse this line").arg(orKey);
    QLocale c = QLocale::c();
    c.setNumberOptions(QLocale::DefaultNumberOptions);
    const QString hidden = c.toString(state.hiddenChars);
    if (hugeRowMode())
        return tr("%1 characters hidden \u2014 click%2 to show the full line in Entry Details "
                  "(lines are not wrapped in this tab)").arg(hidden, orKey);
    return tr("%1 characters hidden \u2014 click%2 to wrap this line").arg(hidden, orKey);
}

void LogListView::activateRowChrome(int row, RowChrome kind)
{
    if (!model() || row < 0 || row >= model()->rowCount())
        return;
    if (kind != RowChrome::WrapToggle && kind != RowChrome::WrapRail)
        return;
    if (hugeRowMode()) {
        // Переноса нет: полный текст строки — в Entry Details. Строка
        // становится выбором пользователя, высоты не меняются.
        selectAsUserChoice(row);
        viewport()->update();
        emit fullTextRequested(row);
        return;
    }
    toggleRowMultiLine(row);
}

bool LogListView::toggleCurrentLineWrap()
{
    // Строка, ВЫБРАННАЯ пользователем (инвариант 10): текущую без выделения
    // назначает Qt по фокусу, и развернуть её с прокруткой к ней нельзя.
    const int row = userSelectedRow();
    if (row < 0 || !rowHasWrapToggle(row))
        return false;
    noteUserAction();
    activateRowChrome(row, RowChrome::WrapToggle);
    return true;
}

LogListView::RowWrapInfo LogListView::rowWrapInfo(int row) const
{
    RowWrapInfo info;
    if (!model() || row < 0 || row >= model()->rowCount())
        return info;
    const RowState state = getRowState(row);
    const qint64 rowTop = rowYOffset(row) - scrollContentY();
    info.length = int(state.text.length());
    info.visibleChars = state.visibleChars;
    info.hiddenChars = state.multiLine ? 0 : state.hiddenChars;
    info.expanded = state.showCollapseBadge;
    info.textRect = textAreaScreenRect(int(std::clamp<qint64>(rowTop, -(qint64(1) << 30), qint64(1) << 30)),
                                       state.height, state.badgesWidth);
    if (state.toggleIndex >= 0) {
        const BadgeLayout& toggle = state.badges[state.toggleIndex];
        info.hasToggle = true;
        info.toggleText = toggle.spec.text;
        info.toggleRect = badgeScreenRect(toggle, rowTop, state.height);
        info.toggleReserve = state.showCollapseBadge ? m_collapseBadgeWidth : m_hiddenToggleBadgeWidth;
    }
    return info;
}

// True если у строки есть плашка-переключатель раскрытия (она длиннее видимой
// области либо уже развёрнута на несколько визуальных строк).
bool LogListView::rowHasWrapToggle(int row) const {
    if (row < 0 || !model() || row >= model()->rowCount())
        return false;
    return getRowState(row).toggleIndex >= 0;
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
    if (enabled)
        m_holdAfterGoTo = false;
    emit followTailChanged(enabled);
    if (enabled) {
        scrollToBottomFollow();
        clearNewLines(); // хвост на экране — новое видно и так
    }
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

// ---------------------------------------------------------------------------
// Новые строки ниже экрана: «↓ N new lines».
// ---------------------------------------------------------------------------

int LogListView::lastVisibleRow() const
{
    const int rows = model() ? model()->rowCount() : 0;
    if (rows <= 0)
        return -1;
    const qint64 bottomY = scrollContentY() + qMax(1, viewport()->height()) - 1;
    if (bottomY >= rowYOffset(rows - 1))
        return rows - 1;
    return qBound(0, rowAtY(bottomY), rows - 1);
}

// O(1) на батч: диапазон вставки и одна проверка IsNewRole. Все строки одной
// вставки — строки одного батча модели: дозапись помечена «новой» целиком
// (стор ставит пометку ДО вставки), начальная загрузка и слияние файлов —
// не помечены вовсе.
void LogListView::countNewRows(int first, int last, bool viewFollowed)
{
    if (viewFollowed || m_followTail) {
        markNewLinesSeen(); // вид у хвоста — новое на экране
        return;
    }
    auto* logModel = qobject_cast<LogModel*>(model());
    if (!logModel || first < 0 || last < first
        || !logModel->data(logModel->index(first, 0), LogModel::IsNewRole).toBool())
        return;
    // Строки, вставшие на экран или выше него, видны сразу или уже позади.
    const int from = qMax(first, lastVisibleRow() + 1);
    if (from > last)
        return;
    const int firstRow = m_newLinesCount > 0 ? qMin(m_firstNewLineRow, from) : from;
    setNewLines(m_newLinesCount + (last - from + 1), firstRow);
}

void LogListView::markNewLinesSeen()
{
    if (m_newLinesCount <= 0 || !model())
        return;
    const int rows = model()->rowCount();
    if (m_followTail || isScrolledToBottom() || m_firstNewLineRow >= rows) {
        clearNewLines();
        return;
    }
    const int lastRow = lastVisibleRow();
    if (lastRow >= rows - 1) {
        clearNewLines();
        return;
    }
    if (lastRow < m_firstNewLineRow)
        return;
    setNewLines(m_newLinesCount - (lastRow - m_firstNewLineRow + 1), lastRow + 1);
}

void LogListView::clearNewLines()
{
    setNewLines(0, -1);
}

void LogListView::setNewLines(qint64 count, int firstRow)
{
    if (count <= 0) {
        count = 0;
        firstRow = -1;
    }
    m_newLinesCount = count;
    m_firstNewLineRow = firstRow;
    if (!m_newLinesIndicator)
        return;
    const QSize before = m_newLinesIndicator->size();
    m_newLinesIndicator->setCount(count);
    if (count > 0 && (m_newLinesIndicator->isHidden() || m_newLinesIndicator->size() != before)) {
        placeNewLinesIndicator();
        m_newLinesIndicator->raise();
    }
    m_newLinesIndicator->setVisible(count > 0);
}

void LogListView::placeNewLinesIndicator()
{
    if (!m_newLinesIndicator)
        return;
    const QSize size = m_newLinesIndicator->sizeHint();
    const QRect area = viewport()->geometry();
    const int gap = qMax(4, size.height() / 3);
    m_newLinesIndicator->setGeometry(area.left() + (area.width() - size.width()) / 2,
                                     area.bottom() + 1 - size.height() - gap,
                                     size.width(), size.height());
}

void LogListView::goToNewLines()
{
    const int row = firstNewLineRow();
    if (row < 0 || !model() || row >= model()->rowCount())
        return;
    goToRow(row, NavKind::NewLines);
    markNewLinesSeen();
}

bool LogListView::hasPendingLayout() const
{
    return m_resizeDebounceTimer->isActive() || m_heightUpdateTimer->isActive()
        || (m_leftColumnsTimer && m_leftColumnsTimer->isActive());
}

void LogListView::wheelEvent(QWheelEvent *event)
{
    // Ctrl+колесо — размер шрифта, как в браузерах и редакторах (а не
    // постраничная прокрутка базового QAbstractScrollArea).
    if (event->modifiers() & Qt::ControlModifier) {
        m_zoomWheelAccum += event->angleDelta().y();
        const int steps = m_zoomWheelAccum / QWheelEvent::DefaultDeltasPerStep;
        if (steps != 0) {
            m_zoomWheelAccum -= steps * QWheelEvent::DefaultDeltasPerStep;
            emit fontZoomRequested(steps);
        }
        event->accept();
        return;
    }

    // Пользователь листает вверх — он хочет читать историю, а не хвост.
    if (m_followTail && event->angleDelta().y() > 0)
        setFollowTail(false);
    m_holdAfterGoTo = false;   // пользователь сам выбирает позицию
    noteUserAction();
    QListView::wheelEvent(event);
}

void LogListView::keyPressEvent(QKeyEvent *event) {
    // Действие с видом — не модификатор сам по себе (начало Shift+F3,
    // Alt+Tab) и не копирование.
    switch (event->key()) {
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta:
    case Qt::Key_AltGr: case Qt::Key_CapsLock: case Qt::Key_NumLock: case Qt::Key_ScrollLock:
        break;
    default:
        if (!event->matches(QKeySequence::Copy))
            noteUserAction();
        break;
    }
    // Навигация вниз — пользователь сам идёт к хвосту: позиция после
    // перехода (goToRow) больше не удерживается.
    switch (event->key()) {
    case Qt::Key_Down:
    case Qt::Key_PageDown:
    case Qt::Key_End:
        m_holdAfterGoTo = false;
        break;
    default:
        break;
    }

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
        // Перенос текущей строки — команда окна toggleLineWrap (по умолчанию
        // Пробел, переназначается; toggleCurrentLineWrap). Сюда Пробел
        // доходит, только если команде дана другая клавиша: событие гасится,
        // чтобы обработка QListView не двигала выделение.
        return;
    }

    QListView::keyPressEvent(event);
}

// Разбор строки таймстампа в QDateTime по набору распространённых форматов.
// Дробная часть секунд отбрасывается (гранулярность фильтра — секунды).
// year — опорная дата года для меток без года (syslog): та же, что у файла
// строки, иначе граница получила бы год по сегодняшней дате, а строки — по
// mtime файла.
static QDateTime parseTimestampText(const QString& raw,
                                    const LineClassifier::YearReference& year) {
    // Сначала тем же классификатором, что и при загрузке (ISO с дробью и
    // зоной, фолбэк-формы, syslog, access): граница фильтра — тот же
    // момент, что у строки.
    qint64 ms = 0;
    if (LineClassifier().detectTimestampMs(raw.trimmed(), ms, year))
        return QDateTime::fromMSecsSinceEpoch(ms - (ms % 1000 + 1000) % 1000);

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

// Контекстное меню строки: копирование выделения и строки, действия над
// выделенным текстом (найти, отфильтровать, подсветить), переключение word
// wrap для строки под курсором и действия в зависимости от типа выделенного
// блока (открыть ссылку / файл). Граница фильтра по времени — выделенный
// таймстамп или метка строки под курсором.
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
    }
    if (haveRow && model()) {
        // Вся строка под курсором — именно отображаемое содержимое, с учётом
        // выбранных Log Fields.
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

    // Короткий однострочный фрагмент — готовый запрос: найти его, оставить
    // или скрыть строки с ним, окрасить такие строки.
    constexpr int kMaxQueryLength = 200;
    if (!sel.isEmpty() && sel.size() <= kMaxQueryLength) {
        const QString shown = fontMetrics().elidedText(sel, Qt::ElideMiddle,
                                                       fontMetrics().averageCharWidth() * 32);
        const auto addTextAction = [&](const QString& text, TextAction action) {
            QAction* a = menu.addAction(text.arg(shown));
            connect(a, &QAction::triggered, this,
                    [this, action, sel]() { emit textActionRequested(action, sel); });
        };
        menu.addSeparator();
        addTextAction(tr("Find “%1”"), TextAction::Find);
        addTextAction(tr("Find All “%1”"), TextAction::FindAll);
        addTextAction(tr("Show Only Lines with “%1”"), TextAction::FilterInclude);
        addTextAction(tr("Hide Lines with “%1”"), TextAction::FilterExclude);
        addTextAction(tr("Highlight Lines with “%1”"), TextAction::Highlight);
    }

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
    }

    // Граница фильтра по времени: выделенный таймстамп, а без него — метка
    // строки под курсором (выделять время ради фильтра не нужно).
    QDateTime boundTime;
    if (selType == TextToken::TokenType::Timestamp) {
        LineClassifier::YearReference year;
        if (haveRow) {
            if (auto* logModel = qobject_cast<LogModel*>(model())) {
                const std::shared_ptr<LogEntry> entry = logModel->entryAt(row);
                if (entry && entry->sourceFile())
                    year = LineClassifier::yearReferenceForFile(entry->sourceFile()->filePath);
            }
        }
        boundTime = parseTimestampText(sel, year);
    }
    if (!boundTime.isValid() && haveRow) {
        if (auto* logModel = qobject_cast<LogModel*>(model()))
            boundTime = logModel->visibleTimestampAt(row);
    }
    if (boundTime.isValid()) {
        const QString shown = boundTime.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
        menu.addSeparator();
        QAction* asStart = menu.addAction(tr("Use as Time Filter Start (%1)").arg(shown));
        connect(asStart, &QAction::triggered, this,
                [this, boundTime]() { emit timeFilterBoundRequested(boundTime, true); });
        QAction* asEnd = menu.addAction(tr("Use as Time Filter End (%1)").arg(shown));
        connect(asEnd, &QAction::triggered, this,
                [this, boundTime]() { emit timeFilterBoundRequested(boundTime, false); });
    }

    // Закладка на строке под курсором (R-2): поставить / снять и заметка.
    if (haveRow && model() && m_bookmarkActions) {
        const bool bookmarked =
            model()->data(model()->index(row, 0), LogModel::BookmarkRole).toBool();
        menu.addSeparator();
        QAction* toggle = menu.addAction(bookmarked ? tr("Remove Bookmark") : tr("Add Bookmark"));
        connect(toggle, &QAction::triggered, this,
                [this, row]() { emit bookmarkActionRequested(row, BookmarkAction::Toggle); });
        QAction* note = menu.addAction(bookmarked ? tr("Edit Bookmark Note…")
                                                  : tr("Add Bookmark with Note…"));
        connect(note, &QAction::triggered, this,
                [this, row]() { emit bookmarkActionRequested(row, BookmarkAction::EditNote); });
    }

    // Полный лог (R-3): показать строку под курсором без фильтров вкладки
    // либо вернуться к отфильтрованному виду.
    if (m_fullLogAction && model()) {
        auto* logModel = qobject_cast<LogModel*>(model());
        if (m_fullLogShown) {
            menu.addSeparator();
            QAction* back = menu.addAction(tr("Return to Filtered View"));
            connect(back, &QAction::triggered, this, [this]() { emit fullLogToggleRequested(-1); });
        } else if (haveRow && logModel && logModel->hasFilters()) {
            menu.addSeparator();
            QAction* show = menu.addAction(tr("Show in Full Log"));
            // Пока меню открыто, строки могли сдвинуться (дозапись слитой
            // вкладки, конец фонового фильтра): строка — по ключу записи.
            const RecordKey key = logModel->keyAt(row);
            connect(show, &QAction::triggered, this, [this, key]() {
                auto* m = qobject_cast<LogModel*>(model());
                const int now = m && key.isValid() ? m->rowForKey(key) : -1;
                if (now >= 0)
                    emit fullLogToggleRequested(now);
            });
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

void LogListView::focusInEvent(QFocusEvent* event) {
    // Окно снова стало активным, фокус вернулся из панели, пришёл Tab — если
    // текущей строки нет, QAbstractItemView::focusInEvent назначает её сам
    // (moveCursor(MoveNext) с NoUpdate: строка становится текущей, но НЕ
    // выделяется). Пользователь при этом ничего не выбирал, поэтому якорь
    // выделения обновлять нельзя: иначе следующая же смена фильтра
    // «восстановит» запись у начала файла и утащит туда вьюпорт вместе с
    // выделением. Именно так терялась позиция после фильтрации, когда
    // выбранную запись перед этим скрывал фильтр.
    m_inFocusIn = true;
    QListView::focusInEvent(event);
    m_inFocusIn = false;
}

void LogListView::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QListView::currentChanged(current, previous);

    // Текущую строку назначил не пользователь, а Qt при получении фокуса:
    // ни якорь выделения, ни текстовое выделение не трогаем.
    if (m_inFocusIn)
        return;

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
    if (!cur.isValid()) {
        // Текущей строки нет — движение начинается с первой (для MoveEnd с
        // последней), как в базовом QListView. Прежняя реализация считала
        // текущей строку 0 и на первое же движение вниз уходила на строку 1,
        // пропуская первую.
        return model()->index(cursorAction == MoveEnd ? rows - 1 : 0, 0);
    }
    const int curRow = qBound(0, cur.row(), rows - 1);
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
            return true;
        case QEvent::ToolTip: {
            // Подсказка — только над служебным элементом строки (U-23); в
            // остальной области событие гасится, как прежде.
            auto* help = static_cast<QHelpEvent*>(event);
            const RowChromeHit hit = rowChromeAt(help->pos());
            if (hit.kind != RowChrome::None && !hit.toolTip.isEmpty())
                QToolTip::showText(help->globalPos(), hit.toolTip, viewport(), hit.rect);
            else
                QToolTip::hideText();
            return true;
        }
        case QEvent::Resize:
            // Вьюпорт меняет размер и без resize самого view (появился или
            // исчез горизонтальный скроллбар) — плашка держится у его низа.
            placeNewLinesIndicator();
            break;
        default:
            break;
    }
    
    return QListView::viewportEvent(event);
}

// --- Private Helper Functions ---

void LogListView::drawTextWithHighlights(QPainter& painter, int x, int y, const QStringView& text, int fragmentStartPos, const QList<HighlightToken>& tokens, const QColor& overrideColor) const {
    // overrideColor задан — рисуем весь фрагмент одним контрастным пеном
    // (перерисовка поверх заливки совпадения); фоны токенов при этом не нужны.
    const bool override = overrideColor.isValid();
    QColor defaultColor = override ? overrideColor : palette().color(QPalette::Text);

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
            if (!override && token.bgColor.isValid()) {
                const QFontMetrics fm = painter.fontMetrics();
                painter.fillRect(tx, y - fm.ascent(), tw, fm.height(), token.bgColor);
            }
            QStringView tokenView = text.sliced(tokenStartInFragment, tokenEndInFragment - tokenStartInFragment);
            painter.setPen(!override && token.color.isValid() ? token.color : defaultColor);
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
