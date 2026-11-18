#include "bookmarkpanelwidget.h"
#include "apptheme.h"
#include "logviewwidget.h"

#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>
#include <QVBoxLayout>

// ===========================================================================
// BookmarkCard
// ===========================================================================

BookmarkCard::BookmarkCard(const Bookmark& bookmark, QWidget* parent)
    : CardFrame(parent)
    , m_id(bookmark.id)
{
    setProperty("bookmarkId", m_id);
    QVBoxLayout* rows = rowsLayout();

    // ---- Строка 1: где закладка (переход), статус, снять ---------------- //
    auto* headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(3);
    rows->addLayout(headerRow);

    m_goButton = makeToolButton(QString(), tr("Go to this line"));
    m_goButton->setObjectName(QStringLiteral("bookmarkGo"));
    headerRow->addWidget(m_goButton);
    connect(m_goButton, &QToolButton::clicked, this, [this]() { emit goRequested(m_id); });

    m_statusLabel = new MutedLabel(QString(), this);
    m_statusLabel->setObjectName(QStringLiteral("bookmarkStatus"));
    headerRow->addWidget(m_statusLabel);
    headerRow->addStretch(1);

    m_removeButton = makeToolButton(QStringLiteral("✕"), tr("Remove this bookmark"));
    m_removeButton->setObjectName(QStringLiteral("bookmarkRemove"));
    headerRow->addWidget(m_removeButton);
    connect(m_removeButton, &QToolButton::clicked, this, [this]() { emit removeRequested(m_id); });

    // ---- Строка 2: фрагмент строки --------------------------------------- //
    m_previewLabel = new MutedLabel(QString(), this);
    m_previewLabel->setObjectName(QStringLiteral("bookmarkPreview"));
    m_previewLabel->setTextFormat(Qt::PlainText);
    // Фрагмент длинный — без переноса, иначе карточка распухнет; ширину
    // не диктует (полный текст — в подсказке).
    m_previewLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    rows->addWidget(m_previewLabel);

    // ---- Строка 3: заметка ------------------------------------------------ //
    m_noteEdit = new QLineEdit(this);
    m_noteEdit->setObjectName(QStringLiteral("bookmarkNote"));
    m_noteEdit->setPlaceholderText(tr("Note…"));
    // Текст — до кнопки очистки: иначе она появляется с анимацией
    // прозрачности, и свежая карточка выглядит иначе, чем устоявшаяся.
    m_noteEdit->setText(bookmark.note);
    m_noteEdit->setClearButtonEnabled(true);
    rows->addWidget(m_noteEdit);
    connect(m_noteEdit, &QLineEdit::editingFinished, this,
            [this]() { emit noteEdited(m_id, m_noteEdit->text()); });

    setBookmark(bookmark);
    applyAccent();
}

void BookmarkCard::setBookmark(const Bookmark& bookmark)
{
    m_goButton->setText(QStringLiteral("%1 : %2")
                            .arg(QFileInfo(bookmark.filePath).fileName())
                            .arg(bookmark.line));
    m_goButton->setToolTip(tr("Go to line %1 of %2").arg(bookmark.line).arg(bookmark.filePath));
    // Первая строка фрагмента, табы — пробелами: подпись однострочная.
    QString preview = bookmark.preview;
    preview.replace(QLatin1Char('\t'), QLatin1Char(' '));
    m_previewLabel->setText(preview);
    m_previewLabel->setToolTip(bookmark.preview);
    // Не затираем заметку, которую пользователь сейчас набирает.
    if (!m_noteEdit->hasFocus() && m_noteEdit->text() != bookmark.note)
        m_noteEdit->setText(bookmark.note);
}

QString BookmarkCard::noteText() const
{
    return m_noteEdit->text();
}

void BookmarkCard::setStatus(BookmarkStore::Status status)
{
    m_status = status;
    switch (status) {
    case BookmarkStore::Status::Visible:
        m_statusLabel->setText(QString());
        break;
    case BookmarkStore::Status::Hidden:
        m_statusLabel->setText(tr("hidden by filter"));
        break;
    case BookmarkStore::Status::Pending:
        m_statusLabel->setText(tr("line not loaded"));
        break;
    case BookmarkStore::Status::Missing:
        m_statusLabel->setText(tr("line not found"));
        break;
    }
    m_goButton->setEnabled(status == BookmarkStore::Status::Visible
                           || status == BookmarkStore::Status::Hidden);
    applyAccent();
}

void BookmarkCard::applyAccent()
{
    // Живая закладка — цвет ленты в жёлобе; не показанная — приглушённая.
    setAccentColor(m_status == BookmarkStore::Status::Visible
                       ? AppTheme::instance().gutterBookmark
                       : mutedBorderColor(palette()));
}

void BookmarkCard::changeEvent(QEvent* event)
{
    CardFrame::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyAccent();
}

// ===========================================================================
// BookmarkPanelWidget
// ===========================================================================

BookmarkPanelWidget::BookmarkPanelWidget(QWidget* parent)
    : QWidget(parent)
{
    // Поля и промежутки — от CompactStyle, как у остальных панелей.
    auto* rootLayout = new QVBoxLayout(this);

    auto* headerCard = new CardFrame(this);
    headerCard->setAccentColor(palette().color(QPalette::Mid)); // нейтральный акцент
    auto* controls = new QHBoxLayout();
    m_countLabel = new MutedLabel(QString(), headerCard);
    m_countLabel->setObjectName(QStringLiteral("bookmarkCount"));
    controls->addWidget(m_countLabel);
    controls->addStretch(1);
    m_clearButton = headerCard->makeToolButton(tr("✕ Clear All"),
        tr("Remove every bookmark of the CURRENT tab"));
    m_clearButton->setObjectName(QStringLiteral("bookmarkClearAll"));
    connect(m_clearButton, &QToolButton::clicked, this, [this]() {
        if (m_logView)
            m_logView->bookmarks().clear();
    });
    controls->addWidget(m_clearButton);
    headerCard->rowsLayout()->addLayout(controls);
    rootLayout->addWidget(headerCard);

    m_cardsArea = new CardListArea(this);
    m_cardsArea->setEmptyText(tr("No bookmarks in this tab. A bookmark marks a line so you "
                                 "can come back to it and leave a note.\n\n"
                                 "Press Ctrl+F2 on a selected line or choose “Add Bookmark” "
                                 "in its context menu; F2 and Shift+F2 jump between "
                                 "bookmarks."));
    m_cardsLayout = m_cardsArea->cardsLayout();
    rootLayout->addWidget(m_cardsArea, /*stretch=*/1);

    m_statusTimer.setSingleShot(true);
    m_statusTimer.setInterval(50);
    connect(&m_statusTimer, &QTimer::timeout, this, &BookmarkPanelWidget::refreshStatuses);

    rebuild();
}

void BookmarkPanelWidget::disconnectSource()
{
    for (const auto& c : m_sourceConnections)
        disconnect(c);
    m_sourceConnections.clear();
}

void BookmarkPanelWidget::dropCards()
{
    BookmarkStore* store = m_logView ? &m_logView->bookmarks() : nullptr;
    for (BookmarkCard* card : std::as_const(m_cards)) {
        // Заметка, которую набирали без Enter, принадлежит уходящей вкладке;
        // потом карточка молчит — уход фокуса при скрытии прислал бы
        // editingFinished уже в контексте новой вкладки (id там другие).
        if (store) {
            if (const Bookmark* b = store->find(card->bookmarkId()); b && b->note != card->noteText())
                store->setNote(card->bookmarkId(), card->noteText());
        }
        card->disconnect(this);
        card->blockSignals(true);
        m_cardsLayout->removeWidget(card);
        card->hide();
        card->deleteLater();
    }
    m_cards.clear();
}

void BookmarkPanelWidget::setLogView(LogViewWidget* logView)
{
    if (m_logView == logView)
        return;
    disconnectSource();
    dropCards();
    m_logView = logView;
    if (logView) {
        BookmarkStore* store = &logView->bookmarks();
        LogModel* model = logView->model();
        m_sourceConnections << connect(store, &BookmarkStore::changed,
                                       this, &BookmarkPanelWidget::rebuild);
        // Видна / скрыта меняется с фильтром и данными: пересчёт с дебаунсом.
        m_sourceConnections << connect(model, &QAbstractItemModel::modelReset, this,
                                       &BookmarkPanelWidget::scheduleStatusRefresh);
        m_sourceConnections << connect(model, &QAbstractItemModel::layoutChanged, this,
                                       &BookmarkPanelWidget::scheduleStatusRefresh);
        m_sourceConnections << connect(model, &QAbstractItemModel::rowsInserted, this,
                                       &BookmarkPanelWidget::scheduleStatusRefresh);
        m_sourceConnections << connect(model, &QAbstractItemModel::rowsRemoved, this,
                                       &BookmarkPanelWidget::scheduleStatusRefresh);
        m_sourceConnections << connect(model, &LogModel::modelFiltered, this,
                                       &BookmarkPanelWidget::scheduleStatusRefresh);
        // Вкладку закрыли — её закладок больше нет. К моменту destroyed
        // QPointer уже обнулён, поэтому не через setLogView (его проверка
        // «та же вкладка» вернула бы управление сразу), а напрямую.
        m_sourceConnections << connect(logView, &QObject::destroyed, this, [this]() {
            disconnectSource();
            m_logView = nullptr;
            dropCards();
            rebuild();
        });
    }
    rebuild();
}

void BookmarkPanelWidget::rebuild()
{
    const QVector<Bookmark> list = m_logView ? m_logView->bookmarks().bookmarks()
                                             : QVector<Bookmark>();
    // Карточки переиспользуются: правка заметки сама присылает changed(), и
    // пересоздание уничтожило бы поле, из которого пришло editingFinished.
    QHash<int, BookmarkCard*> keep;
    for (const Bookmark& b : list) {
        BookmarkCard* card = m_cards.take(b.id);
        if (!card) {
            card = new BookmarkCard(b, this);
            connect(card, &BookmarkCard::goRequested, this, &BookmarkPanelWidget::goTo);
            connect(card, &BookmarkCard::removeRequested, this, [this](int id) {
                if (m_logView)
                    m_logView->bookmarks().remove(id);
            });
            connect(card, &BookmarkCard::noteEdited, this, [this](int id, const QString& note) {
                if (m_logView)
                    m_logView->bookmarks().setNote(id, note);
            });
        } else {
            card->setBookmark(b);
        }
        keep.insert(b.id, card);
    }
    for (BookmarkCard* gone : std::as_const(m_cards)) {
        m_cardsLayout->removeWidget(gone);
        gone->hide();
        gone->deleteLater();
    }
    m_cards = keep;
    // Порядок раскладки — порядок закладок (файл, строка).
    for (int i = 0; i < list.size(); ++i) {
        BookmarkCard* card = m_cards.value(list[i].id);
        if (m_cardsLayout->indexOf(card) != i) {
            m_cardsLayout->removeWidget(card);
            m_cardsLayout->insertWidget(i, card);
        }
        card->show();
    }
    m_cardsArea->cardsChanged();

    m_countLabel->setText(list.isEmpty() ? tr("No bookmarks")
                                         : tr("%n bookmark(s)", nullptr, int(list.size())));
    m_clearButton->setEnabled(!list.isEmpty());
    refreshStatuses();
}

void BookmarkPanelWidget::scheduleStatusRefresh()
{
    if (!m_cards.isEmpty() && !m_statusTimer.isActive())
        m_statusTimer.start();
}

void BookmarkPanelWidget::refreshStatuses()
{
    m_statusTimer.stop();
    if (!m_logView)
        return;
    const BookmarkStore& store = m_logView->bookmarks();
    for (auto it = m_cards.constBegin(); it != m_cards.constEnd(); ++it)
        it.value()->setStatus(store.status(it.key()));
}

void BookmarkPanelWidget::goTo(int id)
{
    if (!m_logView)
        return;
    if (m_logView->goToBookmarkId(id))
        return;
    emit statusMessage(m_logView->bookmarks().status(id) == BookmarkStore::Status::Missing
                           ? tr("The bookmarked line was not found in the file.")
                           : tr("The bookmarked line is not shown."));
}
