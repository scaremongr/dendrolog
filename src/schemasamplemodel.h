#ifndef SCHEMASAMPLEMODEL_H
#define SCHEMASAMPLEMODEL_H

#include "fieldextractor.h"
#include "schemafit.h"

#include <QAbstractTableModel>
#include <QFutureWatcher>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <memory>

// ============================================================================
// SchemaSampleModel — таблица образцов редактора схем (F-17, шаг 9; раздел 8
// F-17.md): ячейки — ровно то, что увидит вкладка (FieldExtractor::extract
// правимой схемы на строках-образцах), в заголовке колонки — число разных
// значений («Level · 6»), в подсказке заголовка — самые частые со счётчиками
// («Order Service 412, Payment Gateway 230 …»): по ним видно, верно ли стоит
// граница поля. Строки, которые схема не приняла, — в misses() с причиной
// словами (SchemaCandidates::reasonText); промахи файла вкладки (SchemaFitJob)
// приходят setMissLines и объясняются тем же извлекателем.
//
// Весь разбор — в пуле (QtConcurrent::run + QFutureWatcher, поколение и флаг
// отмены): схема с /регексом/ — Background и на GUI-потоке не вызывается
// (инвариант 12), тяжёлый регекс не замораживает окно (бюджет SchemaFit).
// isComputing — тест ждёт по нему, а не по времени.
// ============================================================================
class SchemaSampleModel : public QAbstractTableModel {
    Q_OBJECT
public:
    static constexpr int kTopValues = 6;      ///< частых значений в подсказке
    static constexpr int kMaxMisses = 50;

    struct Miss {
        qint64 line = 0;        ///< строка файла (0 — образец без номера)
        QString text;
        QString reason;         ///< словами
        bool partial = false;
    };

    explicit SchemaSampleModel(QObject* parent = nullptr);
    ~SchemaSampleModel() override;

    /// Строки-образцы (по порядку файла). Пересчёт.
    void setLines(const QVector<SchemaFit::Sample>& lines);
    const QVector<SchemaFit::Sample>& lines() const { return m_lines; }
    /// Схема (nullptr или невалидная — таблица пуста). Пересчёт.
    void setExtractor(FieldExtractorPtr extractor);
    FieldExtractorPtr extractor() const { return m_extractor; }
    /// Строки файла, которые не подошли (SchemaFitJob), — объясняются тем же
    /// извлекателем; пусто — промахи образцов.
    void setMissLines(const QVector<SchemaFit::Miss>& misses);

    bool isComputing() const;
    /// Мера на образцах (последний законченный расчёт).
    const SchemaFit::Result& fit() const { return m_fit; }
    const QStringList& columns() const { return m_columns; }
    /// Принятых строк в таблице (по образцам).
    int acceptedRows() const { return int(m_rows.size()); }
    QString cell(int row, int column) const;
    /// Число разных значений колонки (по принятым строкам образцов).
    int distinctCount(int column) const;
    /// Частые значения колонки: «значение — сколько», лучшие первыми.
    QVector<QPair<QString, int>> topValues(int column) const;
    const QVector<Miss>& misses() const { return m_misses; }

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

signals:
    /// Расчёт закончен, таблица и промахи — новые.
    void computed();

private:
    struct Row {
        qint64 line = 0;
        QString text;
        QStringList values;
        bool partial = false;
    };
    struct Output {
        int generation = 0;
        QStringList columns;
        QVector<Row> rows;
        QVector<QVector<QPair<QString, int>>> top;   ///< по колонке
        QVector<int> distinct;
        QVector<Miss> misses;
        SchemaFit::Result fit;
    };
    void restart();
    void onFinished();

    QVector<SchemaFit::Sample> m_lines;
    QVector<SchemaFit::Miss> m_missLines;
    FieldExtractorPtr m_extractor;

    QStringList m_columns;
    QVector<Row> m_rows;
    QVector<QVector<QPair<QString, int>>> m_top;
    QVector<int> m_distinct;
    QVector<Miss> m_misses;
    SchemaFit::Result m_fit;

    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QFutureWatcher<Output>* m_watcher = nullptr;
    bool m_pending = false;   ///< расчёт запрошен, пока шёл прежний
};

#endif // SCHEMASAMPLEMODEL_H
