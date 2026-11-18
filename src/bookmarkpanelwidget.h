#ifndef BOOKMARKPANELWIDGET_H
#define BOOKMARKPANELWIDGET_H

#include "bookmarkstore.h"
#include "cardframe.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class LogViewWidget;
class QLineEdit;
class QToolButton;
class QVBoxLayout;

// ============================================================================
// BookmarkCard — одна закладка в виде карточки (CardFrame).
//
//   ▌ app.log : 1234   скрыта фильтром                       ✕
//   ▌ 2026-07-16 09:20:31.554 [db-pool] ERROR …   (фрагмент строки)
//   ▌ [заметка..........................................]
//
// Полоска-акцент — цвет закладок (AppTheme::gutterBookmark); у закладки,
// чья строка сейчас не показана, — приглушённая.
// objectName дочерних элементов — для тестов: bookmarkGo, bookmarkStatus,
// bookmarkPreview, bookmarkNote, bookmarkRemove; id закладки — в свойстве
// карточки «bookmarkId».
// ============================================================================
class BookmarkCard : public CardFrame {
    Q_OBJECT
public:
    BookmarkCard(const Bookmark& bookmark, QWidget* parent = nullptr);

    int bookmarkId() const { return m_id; }
    // Текст поля заметки (может быть ещё не сохранён — пользователь печатает).
    QString noteText() const;
    void setBookmark(const Bookmark& bookmark);
    void setStatus(BookmarkStore::Status status);

signals:
    void goRequested(int id);
    void removeRequested(int id);
    void noteEdited(int id, const QString& note);

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyAccent();

    int m_id = -1;
    BookmarkStore::Status m_status = BookmarkStore::Status::Visible;
    QToolButton* m_goButton = nullptr;
    MutedLabel* m_statusLabel = nullptr;
    MutedLabel* m_previewLabel = nullptr;
    QLineEdit* m_noteEdit = nullptr;
    QToolButton* m_removeButton = nullptr;
};

// ============================================================================
// BookmarkPanelWidget — содержимое дока Bookmarks: закладки АКТИВНОЙ вкладки.
//
// Щелчок по заголовку карточки — переход (LogViewWidget::goToBookmarkId:
// скрытая фильтром — к ближайшей видимой строке; строки нет — сообщение в
// statusMessage). Заметка правится прямо в карточке (Enter или уход фокуса),
// ✕ — снять закладку, «Clear All» — снять все закладки вкладки.
//
// Статусы (видна / скрыта фильтром / не загружена / не найдена) пересчитываются
// по сигналам модели вкладки с дебаунсом, а не на каждый батч: цена — O(закладок
// · log N) за проход.
// ============================================================================
class BookmarkPanelWidget : public QWidget {
    Q_OBJECT
public:
    explicit BookmarkPanelWidget(QWidget* parent = nullptr);

    // Вкладка, чьи закладки показаны; nullptr — нет вкладок.
    void setLogView(LogViewWidget* logView);
    LogViewWidget* logView() const { return m_logView; }

signals:
    // Сообщение для строки состояния окна (переход не удался).
    void statusMessage(const QString& text);

private:
    void rebuild();
    void refreshStatuses();
    void scheduleStatusRefresh();
    void goTo(int id);
    void disconnectSource();
    // Убрать все карточки, сохранив недописанные заметки в хранилище
    // уходящей вкладки (до смены m_logView).
    void dropCards();

    QPointer<LogViewWidget> m_logView;
    QList<QMetaObject::Connection> m_sourceConnections;
    CardListArea* m_cardsArea = nullptr;
    QVBoxLayout* m_cardsLayout = nullptr;
    QHash<int, BookmarkCard*> m_cards;
    QToolButton* m_clearButton = nullptr;
    MutedLabel* m_countLabel = nullptr;
    QTimer m_statusTimer;
};

#endif // BOOKMARKPANELWIDGET_H
