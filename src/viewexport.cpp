#include "viewexport.h"
#include "logmodel.h"
#include "logfile.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QFutureWatcher>
#include <QHash>
#include <QTimeZone>
#include <QtConcurrent/QtConcurrentRun>
#include <filesystem>

namespace {

std::filesystem::path nativePath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

ViewExport::Result identityError(const std::error_code& ec)
{
    // System messages come in the local 8-bit encoding, not UTF-8: MSVC
    // formats them in the ANSI code page (cp1251 on a Russian Windows).
    const std::string message = ec.message();
    return {ViewExport::Error::IdentityCheck,
            QString::fromLocal8Bit(message.data(), qsizetype(message.size()))};
}

ViewExport::Result checkDestination(const QString& destination,
                                   const QStringList& sourcePaths)
{
    const QFileInfo targetInfo(destination);
    const QString absoluteTarget = targetInfo.absoluteFilePath();
    // Resolve using Qt first: QSaveFile also follows Windows .lnk shortcuts,
    // which std::filesystem alone treats as ordinary, unrelated files.
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    const auto target = nativePath(canonicalTarget.isEmpty() ? absoluteTarget : canonicalTarget);
    std::error_code ec;
    const bool targetExists = std::filesystem::exists(target, ec);
    if (ec)
        return identityError(ec);

    for (const QString& sourcePath : sourcePaths) {
        const QFileInfo sourceInfo(sourcePath);
        const QString absoluteSource = sourceInfo.absoluteFilePath();
        const QString canonicalSource = sourceInfo.canonicalFilePath();
        // Also protect a temporarily missing source (e.g. during rotation).
        if (absoluteSource == absoluteTarget
            || (!canonicalTarget.isEmpty() && canonicalSource == canonicalTarget))
            return {ViewExport::Error::SourceFile, sourcePath};
        if (!targetExists)
            continue;

        const auto source = nativePath(canonicalSource.isEmpty() ? absoluteSource : canonicalSource);
        const bool sourceExists = std::filesystem::exists(source, ec);
        if (ec)
            return identityError(ec);
        if (!sourceExists)
            continue;

        // Filesystem identity handles hard links, symlinks and case aliases,
        // without assuming that every Windows directory is case-insensitive.
        const bool same = std::filesystem::equivalent(target, source, ec);
        if (ec)
            return identityError(ec);
        if (same)
            return {ViewExport::Error::SourceFile, sourcePath};
    }
    return {};
}

} // namespace

QStringList ViewExport::protectedPaths(const QList<std::shared_ptr<LogFile>>& files)
{
    QStringList paths;
    for (const auto& lf : files) {
        if (!lf)
            continue;
        paths.append(lf->filePath);
        if (lf->source().kind == LogSource::Kind::Archive && !lf->source().path.isEmpty())
            paths.append(lf->source().path);
    }
    return paths;
}

ViewExport::Format ViewExport::formatForFileName(const QString& fileName, Format fallback)
{
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (suffix == QLatin1String("csv"))
        return Format::Csv;
    // `.json` — нет: JSON Lines не является документом JSON.
    if (suffix == QLatin1String("jsonl") || suffix == QLatin1String("ndjson"))
        return Format::JsonLines;
    if (suffix == QLatin1String("log") || suffix == QLatin1String("txt"))
        return Format::Text;
    return fallback;
}

QString ViewExport::defaultSuffix(Format format)
{
    switch (format) {
    case Format::Csv: return QStringLiteral("csv");
    case Format::JsonLines: return QStringLiteral("jsonl");
    case Format::Text: break;
    }
    return QStringLiteral("log");
}

ViewExport::Columns ViewExport::columnsFor(const QStringList& schemaFields, bool fieldsShown,
                                         const QVector<int>& visibleIndexes, bool multiFile)
{
    Columns c;
    c.time = true;
    c.file = multiFile;
    c.headers << QStringLiteral("_time");
    if (multiFile)
        c.headers << QStringLiteral("_file");
    QVector<int> chosen;
    if (fieldsShown) {
        for (const int i : visibleIndexes)
            if (i >= 0 && i < schemaFields.size() && !chosen.contains(i))
                chosen << i;
    } else {
        for (int i = 0; i < schemaFields.size(); ++i)
            chosen << i;
    }
    const auto isMessage = [&](int i) {
        return schemaFields.at(i).compare(QLatin1String("Message"), Qt::CaseInsensitive) == 0;
    };
    c.messageColumn = -1;
    for (int k = 0; k < chosen.size(); ++k)
        if (isMessage(chosen.at(k))) {
            c.messageColumn = k;
            break;
        }
    if (c.messageColumn < 0) {
        int schemaMessage = -1;
        for (int i = 0; i < schemaFields.size(); ++i)
            if (isMessage(i)) {
                schemaMessage = i;
                break;
            }
        chosen << schemaMessage;   // -1 — синтетическая колонка Message
        c.messageColumn = int(chosen.size()) - 1;
    }
    c.fieldIndexes = chosen;
    // Имена колонок уникальны: колонка схемы `_time` (ключ JSON-лога) или
    // повтор имени получают «~2», «~3» — иначе в JSON два одинаковых ключа.
    for (const int i : chosen) {
        const QString name = i >= 0 ? schemaFields.at(i) : QStringLiteral("Message");
        QString unique = name;
        for (int n = 2; c.headers.contains(unique); ++n)
            unique = name + QLatin1Char('~') + QString::number(n);
        c.headers << unique;
    }
    return c;
}

QString ViewExport::csvRecord(const QStringList& values)
{
    QString out;
    for (int i = 0; i < values.size(); ++i) {
        if (i)
            out += QLatin1Char(',');
        const QString& v = values.at(i);
        bool quote = false;
        for (const QChar ch : v)
            if (ch == QLatin1Char(',') || ch == QLatin1Char('"') || ch == QLatin1Char('\r')
                || ch == QLatin1Char('\n')) {
                quote = true;
                break;
            }
        // Ведущий/хвостовой пробел Excel и многие парсеры срезают — тоже в кавычки.
        if (!quote && !v.isEmpty() && (v.front().isSpace() || v.back().isSpace()))
            quote = true;
        if (!quote) {
            out += v;
            continue;
        }
        out += QLatin1Char('"');
        for (const QChar ch : v) {
            if (ch == QLatin1Char('"'))
                out += QLatin1Char('"');
            out += ch;
        }
        out += QLatin1Char('"');
    }
    return out;
}

void ViewExport::appendJsonString(QString& out, QStringView text)
{
    static const char hex[] = "0123456789abcdef";
    out += QLatin1Char('"');
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar ch = text[i];
        const char16_t u = ch.unicode();
        switch (u) {
        case u'"': out += QLatin1String("\\\""); continue;
        case u'\\': out += QLatin1String("\\\\"); continue;
        case u'\n': out += QLatin1String("\\n"); continue;
        case u'\r': out += QLatin1String("\\r"); continue;
        case u'\t': out += QLatin1String("\\t"); continue;
        default: break;
        }
        if (ch.isHighSurrogate() && i + 1 < n && text[i + 1].isLowSurrogate()) {
            out += ch;
            out += text[++i];
            continue;
        }
        // Управляющие символы и одиночные суррогаты — \uXXXX: в UTF-8
        // одиночный суррогат записался бы как U+FFFD.
        if (u < 0x20 || ch.isSurrogate()) {
            out += QLatin1String("\\u");
            out += QLatin1Char(hex[(u >> 12) & 0xF]);
            out += QLatin1Char(hex[(u >> 8) & 0xF]);
            out += QLatin1Char(hex[(u >> 4) & 0xF]);
            out += QLatin1Char(hex[u & 0xF]);
            continue;
        }
        out += ch;
    }
    out += QLatin1Char('"');
}

QString ViewExport::jsonRecord(const QStringList& headers, const QStringList& values,
                               const QVector<bool>& nulls)
{
    QString out;
    out += QLatin1Char('{');
    for (int i = 0; i < headers.size(); ++i) {
        if (i)
            out += QLatin1Char(',');
        appendJsonString(out, headers.at(i));
        out += QLatin1Char(':');
        if (i < nulls.size() && nulls.at(i))
            out += QLatin1String("null");
        else
            appendJsonString(out, i < values.size() ? QStringView(values.at(i)) : QStringView());
    }
    out += QLatin1Char('}');
    return out;
}

ViewExport::WriteOptions ViewExport::WriteOptions::forFormat(Format format)
{
    WriteOptions o;
    switch (format) {
    case Format::Text:
        break;
    case Format::Csv:
        o.textMode = false;
        o.utf8Bom = true;
        o.terminator = QStringLiteral("\r\n");
        break;
    case Format::JsonLines:
        o.textMode = false;
        break;
    }
    return o;
}

ViewExport::Result ViewExport::writeRows(QIODevice& output, const RowSource& rows,
                                       const std::atomic_bool* cancel,
                                       const WriteOptions& options)
{
    QTextStream stream(&output);
    stream.setEncoding(QStringConverter::Utf8);
    stream.setGenerateByteOrderMark(options.utf8Bom);
    Result result;
    qint64 written = 0;
    rows([&](QStringView text) {
        if (cancel && (++written & 0xFF) == 0 && cancel->load(std::memory_order_relaxed)) {
            result = {Error::Cancelled, QString()};
            return false;
        }
        stream << text << options.terminator;
        if (stream.status() != QTextStream::Ok) {
            result = {Error::Write, output.errorString()};
            return false;
        }
        return true;
    });
    if (!result.ok())
        return result;
    if (cancel && cancel->load(std::memory_order_relaxed))
        return {Error::Cancelled, QString()};
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return {Error::Write, output.errorString()};
    return {};
}

ViewExport::Result ViewExport::writeRows(QIODevice& output, int rowCount,
                                       const RowText& rowText)
{
    return writeRows(output, [&](const RowSink& sink) {
        for (int row = 0; row < rowCount; ++row) {
            if (!sink(rowText(row)))
                return;
        }
    });
}

ViewExport::Result ViewExport::save(const QString& destination,
                                  const QStringList& sourcePaths,
                                  const RowSource& rows, const std::atomic_bool* cancel,
                                  const WriteOptions& options)
{
    Result result = checkDestination(destination, sourcePaths);
    if (!result.ok())
        return result;

    QSaveFile output(destination);
    // Never fall back to truncating the destination if a temporary file cannot
    // be created. The existing result must survive any failed export.
    output.setDirectWriteFallback(false);
    const QIODevice::OpenMode mode = options.textMode
                                         ? QIODevice::WriteOnly | QIODevice::Text
                                         : QIODevice::WriteOnly;
    if (!output.open(mode))
        return {Error::Open, output.errorString()};

    result = writeRows(output, rows, cancel, options);
    if (!result.ok()) {
        output.cancelWriting();
        return result;
    }
    // A long export can overlap external changes to the destination.
    result = checkDestination(destination, sourcePaths);
    if (!result.ok()) {
        output.cancelWriting();
        return result;
    }
    if (!output.commit())
        return {Error::Commit, output.errorString()};
    return {};
}

ViewExport::Result ViewExport::save(const QString& destination,
                                  const QStringList& sourcePaths,
                                  int rowCount, const RowText& rowText)
{
    return save(destination, sourcePaths, [&](const RowSink& sink) {
        for (int row = 0; row < rowCount; ++row) {
            if (!sink(rowText(row)))
                return;
        }
    });
}

// ---------------------------------------------------------------------------
// ViewExportJob
// ---------------------------------------------------------------------------

ViewExportJob::ViewExportJob(QObject* parent)
    : QObject(parent)
    , m_progressTimer(new QTimer(this))
{
    m_progressTimer->setInterval(200);
    connect(m_progressTimer, &QTimer::timeout, this, [this]() {
        if (m_progress)
            emit progress(m_progress->load(std::memory_order_relaxed));
    });
}

ViewExportJob::~ViewExportJob()
{
    // QSaveFile в воркере должен закончить (отменить) запись до выхода.
    cancel(/*wait=*/true);
}

namespace {

// Момент строки в ISO 8601 с мс и местным смещением; пусто — метки нет.
// Местное смещение — поиск по базе зон, поэтому оно кэшируется на 15-минутный
// интервал (переходы зон приходятся на их границы; исключение — давние LMT
// с секундами, ISO их и не пишет); строка строится по фиксированному смещению.
class IsoTime {
public:
    QString operator()(qint64 ms)
    {
        if (!hasTimestampMs(ms))
            return QString();
        const qint64 bucket = ms >= 0 ? ms / kBucketMs : -((-ms + kBucketMs - 1) / kBucketMs);
        if (!m_valid || bucket != m_bucket) {
            m_bucket = bucket;
            m_valid = true;
            m_zone = QTimeZone::fromSecondsAheadOfUtc(QDateTime::fromMSecsSinceEpoch(ms).offsetFromUtc());
        }
        return QDateTime::fromMSecsSinceEpoch(ms, m_zone).toString(Qt::ISODateWithMs);
    }

private:
    static constexpr qint64 kBucketMs = 15 * 60 * 1000;
    qint64 m_bucket = 0;
    bool m_valid = false;
    QTimeZone m_zone;
};

} // namespace

bool ViewExportJob::start(const LogModel& model, const QString& destination,
                          const QStringList& sourcePaths, ViewExport::Format format)
{
    if (isRunning())
        return false;
    // Снапшот строк и выбора колонок — на момент старта: дальше модель можно
    // менять, фильтровать и даже закрывать, экспорт пишет то, что было видно.
    const LogScanSnapshot snapshot = model.scanSnapshot(/*filteredOnly=*/true);
    const bool fieldsShown = model.fieldDisplayFilterEnabled();
    const QVector<int> visibleIndexes = model.visibleFieldIndexes();
    // Схемы файлов (F-5): строка, в схеме файла которой нет ни одной
    // выбранной колонки, — сырым текстом, как во view.
    const FileSchemaSet schemas = model.fileSchemas();
    const qint64 rows = snapshot.rowCount();
    // Таблица (F-9): колонки и имена файлов — здесь, на GUI-потоке.
    const bool table = format != ViewExport::Format::Text;
    ViewExport::Columns columns;
    QHash<const LogFile*, QString> fileNames;
    if (table) {
        const auto& files = model.labeledFiles();
        columns = ViewExport::columnsFor(model.availableFields(), fieldsShown, visibleIndexes,
                                         files.size() > 1);
        for (const auto& f : files)
            if (f.file)
                fileNames.insert(f.file.get(), QDir::toNativeSeparators(f.file->displayPath()));
    }
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    auto progressValue = std::make_shared<std::atomic<int>>(0);
    m_cancel = cancelFlag;
    m_progress = progressValue;
    m_destination = destination;
    m_format = format;
    m_rowCount = int(rows);

    m_future = QtConcurrent::run([=]() {
        const auto tick = [&](qint64& done) {
            if ((++done & 0x3FF) == 0)
                progressValue->store(int(qMin<qint64>(99, done * 100 / qMax<qint64>(1, rows))),
                                     std::memory_order_relaxed);
        };
        const ViewExport::RowSource text = [&](const ViewExport::RowSink& sink) {
            qint64 done = 0;
            snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta& meta, QStringView text) {
                tick(done);
                if (!fieldsShown)
                    return sink(text);
                // Текст строки валиден только внутри колбэка — без копии.
                const QString raw = QString::fromRawData(text.data(), text.size());
                return sink(LogModel::formatDisplayText(raw, snapshot.fieldsAt(row, raw),
                                                        true, visibleIndexes,
                                                        schemas.forFile(meta.sourceFile)));
            });
        };
        const bool csv = format == ViewExport::Format::Csv;
        const ViewExport::RowSource tableRows = [&](const ViewExport::RowSink& sink) {
            if (csv && !sink(ViewExport::csvRecord(columns.headers)))
                return;
            QStringList values;
            QVector<bool> nulls(columns.headers.size(), false);
            qint64 done = 0;
            IsoTime isoTime;
            snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta& meta, QStringView text) {
                tick(done);
                const QString raw = QString::fromRawData(text.data(), text.size());
                const LogEntryFields fields = snapshot.fieldsAt(row, raw);
                values.clear();
                values << isoTime(meta.timestampMs);
                nulls[0] = !hasTimestampMs(meta.timestampMs);
                if (columns.file)
                    values << fileNames.value(meta.sourceFile);
                for (int k = 0; k < columns.fieldIndexes.size(); ++k) {
                    const int fi = columns.fieldIndexes.at(k);
                    // values (и raw) живут до конца колбэка: запись строится в нём.
                    // Строка без полей — сырой текст в Message; так же у строки,
                    // где схема узнала часть блоков, но не Message (префиксный
                    // вариант): текст не пропадает молча.
                    if (k == columns.messageColumn && (fields.isEmpty() || (fi >= 0 && !fields.has(fi))))
                        values << raw;
                    else if (fields.isEmpty())
                        values << QString();
                    else
                        values << (fi >= 0 ? fields.value(fi, raw) : QString());
                }
                return sink(csv ? ViewExport::csvRecord(values)
                                : ViewExport::jsonRecord(columns.headers, values, nulls));
            });
        };
        return ViewExport::save(destination, sourcePaths, table ? tableRows : text,
                                cancelFlag.get(), ViewExport::WriteOptions::forFormat(format));
    });
    auto* watcher = new QFutureWatcher<ViewExport::Result>(this);
    const int generation = ++m_generation;
    connect(watcher, &QFutureWatcher<ViewExport::Result>::finished, this,
            [this, watcher, generation]() {
                watcher->deleteLater();
                if (generation != m_generation)
                    return;
                m_progressTimer->stop();
                m_cancel.reset();
                m_progress.reset();
                emit finished(watcher->result());
            });
    watcher->setFuture(m_future);
    m_progressTimer->start();
    return true;
}

void ViewExportJob::cancel(bool wait)
{
    if (m_cancel)
        m_cancel->store(true);
    if (wait)
        m_future.waitForFinished();
}

bool ViewExportJob::isRunning() const
{
    // Задание идёт до доставки finished, а не до конца воркера: маленький
    // экспорт заканчивается в пуле раньше, чем вызывающий успевает спросить,
    // и между концом воркера и сигналом второй start() иначе прошёл бы, а
    // результат первого (поколение уже другое) пропал бы без finished (Q-25).
    // m_cancel живёт ровно от start() до emit finished.
    return m_cancel != nullptr;
}
