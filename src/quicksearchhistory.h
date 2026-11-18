#ifndef QUICKSEARCHHISTORY_H
#define QUICKSEARCHHISTORY_H

#include <QString>
#include <QStringList>

// ============================================================================
// QuickSearchHistory — последние запросы быстрого поиска (поле в тулбаре).
//
// Чистые данные без виджетов и без настроек: хранит их AppSettings
// (quickSearchHistory), показывает QuickSearchField.
//
// Правила:
//   • свежий запрос — первым; повтор уже известного запроса поднимает его
//     наверх, а не добавляет второй раз (сравнение точное, с регистром:
//     «Error» и «error» — разные запросы);
//   • не больше kMaxEntries запросов, самые старые вытесняются;
//   • пустой запрос и запрос длиннее kMaxEntryLength символов не
//     запоминаются (вставленный в поле мегабайт не должен жить в ini);
//   • запоминается только ТЕКСТ, без флагов «Aa» / слово / регекс: флаги —
//     состояние поля, одно на все запросы, и тот же текст под разными
//     флагами не плодит почти одинаковые строки списка.
// ============================================================================
class QuickSearchHistory {
public:
    static constexpr int kMaxEntries = 20;
    static constexpr int kMaxEntryLength = 500;

    QuickSearchHistory() = default;
    // Список из настроек: приводится к правилам выше (дубли, длина, предел).
    explicit QuickSearchHistory(const QStringList& entries);

    const QStringList& entries() const { return m_entries; }
    bool isEmpty() const { return m_entries.isEmpty(); }

    // Запомнить запрос. true — список изменился.
    bool add(const QString& text);
    void clear() { m_entries.clear(); }

private:
    static bool acceptable(const QString& text);
    QStringList m_entries;
};

#endif // QUICKSEARCHHISTORY_H
