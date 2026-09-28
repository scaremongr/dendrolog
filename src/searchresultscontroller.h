#ifndef SEARCHRESULTSCONTROLLER_H
#define SEARCHRESULTSCONTROLLER_H

#include "filterruleset.h"

#include <QObject>
#include <QPointer>
#include <QVector>

class LogModel;
class QTimer;

// ============================================================================
// SearchResultsController — недеструктивный поиск (панель Search Results) без
// виджетов: запрос, модель результатов и её жизнь вместе с активной вкладкой.
//
// Выдача — отдельная LogModel поверх ВСЕХ видимых строк источника
// (LogModel::searchVisible). Дальше контроллер держит её в согласии с ним:
//   • новые видимые строки источника (дозапись хвоста, инкрементальный
//     фильтр) — appendSearchRows: запросом проверяются только они, выдача
//     растёт вставками без reset. Поиск по растущему гигантскому логу не
//     начинается заново на каждой порции — иначе он мог не завершиться вовсе;
//   • изменился текст видимой строки (дописан хвост без '\n') —
//     refreshSearchRow;
//   • всё прочее (reset источника, удаление строк, смена его фильтров) —
//     полный поиск заново, с дебаунсом;
//   • смена отображаемых полей источника зеркалится без поиска.
// Пока контроллер «не живой» (панель скрыта или режим не Search), полный
// поиск откладывается до setLive(true); инкрементальные обновления дёшевы и
// идут, пока выдача не отстала от источника.
//
// Навигация по выдаче и подсветка — дело окна: контроллер про view не знает.
// ============================================================================
class SearchResultsController : public QObject {
    Q_OBJECT

public:
    explicit SearchResultsController(QObject* parent = nullptr);
    ~SearchResultsController() override;

    LogModel* model() const { return m_results; }
    LogModel* source() const { return m_source; }
    // Есть применённый запрос (пустой или испорченный запрос — не активен).
    bool isActive() const { return m_active; }
    const FilterRuleSet& rules() const { return m_rules; }
    QString statusText() const { return m_status; }

    // Модель активной вкладки; nullptr — вкладок нет. Смена источника
    // сбрасывает выдачу и запрос.
    void setSource(LogModel* source);
    // Новый запрос по видимым строкам источника — сразу, без дебаунса.
    // Правила уже привязаны к полям схемы.
    void search(const FilterRuleSet& rules);
    void clear();
    void setLive(bool live);

    // Пауза перед полным поиском заново: изменения источника часто идут
    // пачкой (reset, затем modelFiltered и новые строки).
    static constexpr int kRefreshDelayMs = 200;

signals:
    void statusChanged(const QString& text);

private:
    void disconnectSource();
    void requestFullSearch();
    void runFullSearch();
    void setStatus(const QString& text);
    void updateCountStatus();
    void mirrorFieldDisplay();
    void onSourceRowsInserted(int first, int last);
    void onSourceDataChanged(int first, int last, const QList<int>& roles);

    LogModel* m_results = nullptr;
    QPointer<LogModel> m_source;
    QVector<QMetaObject::Connection> m_sourceConnections;
    FilterRuleSet m_rules;
    bool m_active = false;
    bool m_live = true;
    // Выдача отстала от источника и ждёт полного поиска: инкрементальные
    // обновления до него бессмысленны.
    bool m_stale = false;
    QTimer* m_refreshTimer = nullptr;
    QString m_status;
};

#endif // SEARCHRESULTSCONTROLLER_H
