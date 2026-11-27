#include "schemasamplemodel.h"

#include "schemacandidates.h"

#include <QFont>
#include <QHash>
#include <QPalette>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

SchemaSampleModel::SchemaSampleModel(QObject* parent)
    : QAbstractTableModel(parent)
    , m_watcher(new QFutureWatcher<Output>(this))
{
    connect(m_watcher, &QFutureWatcher<Output>::finished, this, &SchemaSampleModel::onFinished);
}

SchemaSampleModel::~SchemaSampleModel()
{
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    m_watcher->waitForFinished();
}

void SchemaSampleModel::setLines(const QVector<SchemaFit::Sample>& lines)
{
    m_lines = lines;
    restart();
}

void SchemaSampleModel::setExtractor(FieldExtractorPtr extractor)
{
    m_extractor = std::move(extractor);
    restart();
}

void SchemaSampleModel::setMissLines(const QVector<SchemaFit::Miss>& misses)
{
    m_missLines = misses;
    restart();
}

bool SchemaSampleModel::isComputing() const
{
    // Не m_watcher->isRunning(): тот гаснет с концом воркера, когда finished
    // ещё в очереди GUI-потока, — таблица в этот миг прежняя (пустая), а тест
    // и окно считали бы расчёт законченным.
    return m_running || m_pending;
}

void SchemaSampleModel::restart()
{
    ++m_generation;
    if (m_cancel)
        m_cancel->store(true, std::memory_order_relaxed);
    if (m_running) {
        // Прежний расчёт отменён; новый — по его концу (один в полёте).
        m_pending = true;
        return;
    }
    m_pending = false;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    m_cancel = cancel;
    const int generation = m_generation;
    const QVector<SchemaFit::Sample> lines = m_lines;
    const QVector<SchemaFit::Miss> missLines = m_missLines;
    const FieldExtractorPtr extractor = m_extractor;
    m_running = true;
    m_watcher->setFuture(QtConcurrent::run([generation, lines, missLines, extractor, cancel]() {
        Output out;
        out.generation = generation;
        if (!extractor || !extractor->isValid()) {
            // Схемы нет (или шаблон с ошибкой): строки показываются как есть.
            for (const SchemaFit::Sample& sample : lines) {
                if (sample.text.trimmed().isEmpty())
                    continue;
                LineView view;
                view.line = sample.line;
                view.text = sample.text;
                out.views.append(view);
            }
            return out;
        }
        out.columns = extractor->fields();
        const int columns = int(out.columns.size());
        SchemaFit::Budget budget;
        budget.cancel = cancel.get();
        const SchemaFit::Corpus corpus = SchemaFit::corpus(lines);
        out.fit = SchemaFit::evaluate(*extractor, corpus, budget);
        QVector<QHash<QString, int>> counts(columns);
        LogEntryFields fields;
        for (int i = 0; i < lines.size(); ++i) {
            if (cancel->load(std::memory_order_relaxed))
                return out;
            const QString& text = lines[i].text;
            if (text.trimmed().isEmpty())
                continue;
            const FieldExtractor::Outcome outcome = extractor->extract(text, &fields);
            LineView view;
            view.line = lines[i].line;
            view.text = text;
            if (outcome == FieldExtractor::Outcome::Rejected) {
                view.state = corpus.candidate.value(i) ? LineView::State::Missed : LineView::State::Other;
                out.views.append(view);
                if (missLines.isEmpty() && corpus.candidate.value(i) && out.misses.size() < kMaxMisses) {
                    Miss miss;
                    miss.line = lines[i].line;
                    miss.text = text;
                    miss.reason = SchemaCandidates::reasonText(extractor->explain(text), true);
                    out.misses.append(miss);
                }
                continue;
            }
            Row row;
            row.line = lines[i].line;
            row.text = text;
            row.partial = outcome == FieldExtractor::Outcome::Partial;
            row.values.reserve(columns);
            row.starts.reserve(columns);
            row.sizes.reserve(columns);
            for (int c = 0; c < columns; ++c) {
                const QString v = c < fields.size() ? fields.value(c, text) : QString();
                row.values.append(v);
                row.starts.append(fields.has(c) ? fields.spans[c].start : -1);
                row.sizes.append(fields.has(c) ? fields.spans[c].size() : 0);
                if (!v.isEmpty())
                    ++counts[c][v];
            }
            view.state = row.partial ? LineView::State::Partial : LineView::State::Accepted;
            view.row = int(out.rows.size());
            out.views.append(view);
            out.rows.append(row);
            if (row.partial && missLines.isEmpty() && out.misses.size() < kMaxMisses) {
                Miss miss;
                miss.line = row.line;
                miss.text = text;
                miss.partial = true;
                miss.reason = SchemaCandidates::reasonText(extractor->explain(text), true);
                out.misses.append(miss);
            }
        }
        for (const SchemaFit::Miss& m : missLines) {
            if (cancel->load(std::memory_order_relaxed))
                return out;
            Miss miss;
            miss.line = m.line;
            miss.text = m.text;
            miss.partial = m.partial;
            miss.reason = SchemaCandidates::reasonText(extractor->explain(m.text), true);
            out.misses.append(miss);
        }
        out.top.resize(columns);
        out.distinct.resize(columns);
        for (int c = 0; c < columns; ++c) {
            QVector<QPair<QString, int>> all;
            all.reserve(counts[c].size());
            for (auto it = counts[c].cbegin(); it != counts[c].cend(); ++it)
                all.append({it.key(), it.value()});
            std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            out.distinct[c] = int(all.size());
            all.resize(qMin<qsizetype>(all.size(), kTopValues));
            out.top[c] = all;
        }
        return out;
    }));
}

void SchemaSampleModel::onFinished()
{
    m_running = false;
    const Output out = m_watcher->result();
    if (m_pending) {
        restart();
        return;
    }
    if (out.generation != m_generation)
        return;
    beginResetModel();
    m_columns = out.columns;
    m_rows = out.rows;
    m_top = out.top;
    m_distinct = out.distinct;
    m_misses = out.misses;
    m_views = out.views;
    m_fit = out.fit;
    endResetModel();
    emit computed();
}

QString SchemaSampleModel::cell(int row, int column) const
{
    if (row < 0 || row >= m_rows.size())
        return QString();
    return m_rows[row].values.value(column);
}

QString SchemaSampleModel::rowText(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row].text : QString();
}

qint64 SchemaSampleModel::rowLine(int row) const
{
    return row >= 0 && row < m_rows.size() ? m_rows[row].line : 0;
}

int SchemaSampleModel::cellStart(int row, int column) const
{
    if (row < 0 || row >= m_rows.size())
        return -1;
    return m_rows[row].starts.value(column, -1);
}

QString SchemaSampleModel::rawCell(int row, int column) const
{
    const int start = cellStart(row, column);
    if (start < 0)
        return QString();
    return m_rows[row].text.mid(start, m_rows[row].sizes.value(column));
}

Qt::ItemFlags SchemaSampleModel::flags(const QModelIndex& index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    // «Редактор» ячейки — только для выделения текста (Make Field, F-17,
    // шаг 10): поле только для чтения, модель не меняется.
    if (index.isValid() && cellStart(index.row(), index.column()) >= 0)
        f |= Qt::ItemIsEditable;
    return f;
}

int SchemaSampleModel::distinctCount(int column) const
{
    return m_distinct.value(column);
}

QVector<QPair<QString, int>> SchemaSampleModel::topValues(int column) const
{
    return m_top.value(column);
}

int SchemaSampleModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

int SchemaSampleModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : int(m_columns.size());
}

QVariant SchemaSampleModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row& row = m_rows[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        return row.values.value(index.column());
    case Qt::EditRole:
        return rawCell(index.row(), index.column());
    case Qt::ToolTipRole:
        return row.line > 0 ? tr("Line %1: %2").arg(row.line).arg(row.text) : row.text;
    case Qt::FontRole:
        if (row.partial) {
            QFont f;
            f.setItalic(true);
            return f;
        }
        return {};
    case Qt::ForegroundRole:
        // Строка, принятая с остановкой: поля слева сошлись, остаток — в
        // хвостовом поле; приглушена (роль палитры — следует за темой).
        if (row.partial)
            return QPalette().brush(QPalette::PlaceholderText);
        return {};
    default:
        return {};
    }
}

QVariant SchemaSampleModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return QAbstractTableModel::headerData(section, orientation, role);
    if (section < 0 || section >= m_columns.size())
        return {};
    if (role == Qt::DisplayRole)
        return tr("%1 · %2").arg(m_columns[section]).arg(m_distinct.value(section));
    if (role == Qt::ToolTipRole) {
        const auto top = m_top.value(section);
        if (top.isEmpty())
            return tr("%1: no values in the sample lines").arg(m_columns[section]);
        QStringList parts;
        for (const auto& [value, count] : top)
            parts << tr("%1 — %2").arg(value.left(60)).arg(count);
        QString text = tr("%1: %2 different values in the sample lines. Most frequent:\n%3")
                           .arg(m_columns[section])
                           .arg(m_distinct.value(section))
                           .arg(parts.join(QLatin1Char('\n')));
        if (m_distinct.value(section) > top.size())
            text += QStringLiteral("\n…");
        return text;
    }
    return {};
}
