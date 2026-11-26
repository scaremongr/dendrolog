#ifndef TESTDOCUMENTS_H
#define TESTDOCUMENTS_H

// ============================================================================
// testdocuments.h — общие для тестов фикстуры и загрузка документа в LogModel
// обоими путями хранилища, ровно так, как это делает LogViewWidget:
//   • резидентный — LogParser → сортировка батча → LogModel::mergeEntries;
//   • индексный   — LogIndexer → IndexedLogStore::attachFile/appendIndexedRows.
//
// Header-only: подключается из тестов, которые сами компилируют нужные
// исходники src/. Ошибки загрузки не печатаются здесь, а копятся в
// Document::errors — каждый тест сообщает о них своим способом.
// ============================================================================

#include "indexedlogstore.h"
#include "lineindex.h"
#include "logentry.h"
#include "logfile.h"
#include "logindexer.h"
#include "logmodel.h"
#include "logparser.h"
#include "logpattern.h"
#include "recordlocator.h"
#include "textencoding.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QRandomGenerator>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace testdocs {

// ---------------------------------------------------------------------------
// Схема полей и генерация фикстур
// ---------------------------------------------------------------------------

inline PatternBlock patternBlock(PatternBlock::MatchKind kind, const QString& name,
                                 const QString& lead = QString(),
                                 const QString& closing = QString(),
                                 const QString& sep = QString())
{
    PatternBlock b;
    b.matchKind   = kind;
    b.name        = name;
    b.leadingText = lead;
    b.closingText = closing;
    b.separator   = sep;
    return b;
}

// Схема, под которую генерируются фикстуры:
//   2026-03-05 10:00:00.000 [worker-1] INFO - текст
// Поля: 0 Timestamp, 1 Thread, 2 Level, 3 Message.
inline QString buildSchema()
{
    PatternDefinition def;
    def.blocks = {
        patternBlock(PatternBlock::MatchKind::Timestamp, QStringLiteral("Timestamp")),
        patternBlock(PatternBlock::MatchKind::TextUntilSeparator, QStringLiteral("Thread"),
                     QStringLiteral("["), QStringLiteral("]")),
        patternBlock(PatternBlock::MatchKind::Level, QStringLiteral("Level"), QString(),
                     QString(), QStringLiteral("-")),
        patternBlock(PatternBlock::MatchKind::Remainder, QStringLiteral("Message")),
    };
    return LogPattern::serializeDefinition(def);
}

inline QDateTime fixtureBaseTime()
{
    return QDateTime::fromString(QStringLiteral("2026-03-05 10:00:00.000"),
                                 QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
}

// Пустая строка при ошибке — тест проверяет и сообщает сам.
// Путь спула потока (stdin, вывод команды — F-7) внутри dir: так его называет
// SpoolDir::createStreamFile (`<pid>-<16 hex>/<n>-stream-<имя>.log`), и
// SpoolDir::isStreamFile узнаёт его без настоящего каталога экземпляра.
// Каталог создаётся, файл — нет.
inline QString streamSpoolPath(const QDir& dir, const QString& name = QStringLiteral("stdin"))
{
    const QString instance = QStringLiteral("%1-%2").arg(QCoreApplication::applicationPid())
                                 .arg(quint64(0xF7), 16, 16, QLatin1Char('0'));
    dir.mkpath(instance);
    return QDir(dir.filePath(instance)).absoluteFilePath(QStringLiteral("1-stream-%1.log").arg(name));
}

inline QString writeFile(const QDir& dir, const QString& name, const QByteArray& bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(bytes);
    return path;
}

// Детерминированный синтетический лог. Намеренно содержит всё, что ломает
// наивные реализации: многострочные записи, пустые строки, не-ASCII,
// дубликаты меток, а по желанию — преамбулу свободного текста и нарушения
// порядка таймстампов.
// monotonic — метки только возрастают: порядок строк в файле совпадает с
// порядком по времени, и оба бэкенда обязаны показывать строку в строку одно
// и то же даже для одно-файловой вкладки (где индексный не сортирует).
inline QByteArray makeLogBytes(int records, quint32 seed, const QByteArray& eol,
                               const QDateTime& base, bool withPreamble,
                               bool trailingNewline, bool monotonic)
{
    QRandomGenerator rng(seed);
    QByteArray out;

    const char* threads[] = { "worker-1", "worker-2", "io-pool", "main" };
    const char* levels[]  = { "TRACE", "DEBUG", "INFO", "INFO", "WARN", "ERROR", "FATAL" };
    const char* messages[] = {
        "Connection established to %1",
        "Timeout waiting for response from node %1",
        "Disk usage at %1 percent",
        "Кэш прогрет, записей: %1",
        "Retrying request #%1 after transient failure",
        "Checkpoint %1 written",
        "Disk timeout on volume %1",
        "Пользователь %1 вышел из системы",
    };

    auto appendLine = [&](const QString& line) {
        out += line.toUtf8();
        out += eol;
    };

    if (withPreamble) {
        appendLine(QStringLiteral("### DendroLog synthetic fixture"));
        appendLine(QStringLiteral("generated deterministically, seed=%1").arg(seed));
        appendLine(QString());
    }

    qint64 tick = 0;
    for (int i = 0; i < records; ++i) {
        // Шаг времени с разбросом: часть записей получает одинаковый штамп,
        // часть — уходит назад, чтобы слияние/сортировка были нетривиальны.
        const int roll = int(rng.bounded(100));
        if (roll < 10)
            ;                                   // тот же штамп, что у предыдущей
        else if (!monotonic && roll < 15)
            tick -= 400;                        // шаг назад
        else
            tick += 500 + rng.bounded(2000);

        const QDateTime ts = base.addMSecs(tick);
        const QString line =
            QStringLiteral("%1 [%2] %3 - %4")
                .arg(ts.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                     QString::fromLatin1(threads[rng.bounded(4)]),
                     QString::fromLatin1(levels[rng.bounded(7)]),
                     QString::fromLatin1(messages[rng.bounded(8)])
                         .arg(rng.bounded(1000)));
        appendLine(line);

        // Continuation-строки: стек-трейс без таймстампа.
        if (rng.bounded(100) < 18) {
            const int frames = 1 + int(rng.bounded(3));
            for (int f = 0; f < frames; ++f)
                appendLine(QStringLiteral("    at com.example.Module%1.call(Module%1.java:%2)")
                               .arg(f).arg(100 + rng.bounded(900)));
        }
        // Пустая строка как часть записи.
        if (rng.bounded(100) < 6)
            appendLine(QString());
    }

    if (!trailingNewline && out.endsWith(eol))
        out.chop(eol.size());
    return out;
}

// ---------------------------------------------------------------------------
// Кодировки (F-1): фикстуры в однобайтных кодировках
// ---------------------------------------------------------------------------

// Текст → байты кодировки. Однобайтные — по обратной таблице TextDecoder
// (сама таблица сверена с эталоном в tests/textencoding); символ, которого в
// кодировке нет, — '?' (типографских «» — № нет в IBM866 и KOI8-R: в
// фикстурах их не использовать). UTF-8 — toUtf8.
inline QByteArray encodeText(const QString& text, TextEncoding encoding)
{
    if (!TextEncodings::isSingleByte(encoding))
        return text.toUtf8();
    const TextDecoder decoder(encoding);
    QHash<char16_t, char> reverse;
    for (int b = 0x80; b <= 0xFF; ++b) {
        const char byte = char(b);
        const QString ch = decoder.decode(&byte, 1);
        if (ch.size() == 1)
            reverse.insert(ch.at(0).unicode(), byte);
    }
    QByteArray out;
    out.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() < 0x80)
            out += char(c.unicode());
        else
            out += reverse.value(c.unicode(), '?');
    }
    return out;
}

// Лог с кириллицей: ASCII-преамбула из asciiRecords записей (решать нечего),
// затем записи с русским текстом разного регистра, многострочная запись и
// ещё ASCII. Метки возрастают — одно-файловый порядок совпадает с временным.
// secondBase — сдвиг меток (слитая вкладка из двух таких файлов).
inline QString cyrillicLogText(int asciiRecords, int secondBase = 0)
{
    static const char16_t* const kMessages[] = {
        u"Ошибка соединения с базой данных: таймаут",
        u"Пользователь Иванов вошёл в систему",
        u"ПРЕДУПРЕЖДЕНИЕ: диск заполнен на 95%",
        u"Запись в журнал: щёлкнул (Сохранить) - успешно",
        u"повторная попытка 3 после сбоя",
        u"Сервер ответил: ошибка 500, повтор",
    };
    static const char* const kLevels[] = { "ERROR", "INFO", "WARN", "INFO", "DEBUG", "ERROR" };
    const QDateTime base = fixtureBaseTime();
    QString out;
    int second = secondBase;
    const auto stamp = [&]() {
        return base.addSecs(second++).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    };
    for (int i = 0; i < asciiRecords; ++i)
        out += QStringLiteral("%1 [main] INFO - ascii preamble line %2\n").arg(stamp()).arg(i);
    for (int round = 0; round < 3; ++round) {
        for (int m = 0; m < 6; ++m) {
            out += QStringLiteral("%1 [worker-%2] %3 - %4\n")
                       .arg(stamp())
                       .arg(m % 3)
                       .arg(QLatin1String(kLevels[m]))
                       .arg(QString::fromUtf16(kMessages[m]));
            if (m == 0)
                out += QStringLiteral("    в модуле Хранилище.Подключение (строка %1)\n").arg(100 + round);
        }
        out += QStringLiteral("%1 [main] INFO - plain ascii between rounds %2\n").arg(stamp()).arg(round);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Прокачка событий
// ---------------------------------------------------------------------------

inline void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    } while (t.elapsed() < ms);
}

// Крутит цикл событий, пока предикат не станет истинным; false — по таймауту.
inline bool waitFor(const std::function<bool()>& ready, int timeoutMs = 60000)
{
    QElapsedTimer t;
    t.start();
    while (!ready()) {
        if (t.elapsed() >= timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// Ожидание тишины модели: фильтрация индексного бэкенда всегда асинхронна (у
// резидентного — от 100k записей) и может идти несколькими инкрементальными
// джобами, поэтому ждём, пока модель не закончит фильтровать, перестанут
// приходить modelFiltered / filterProgress И перестанет меняться rowCount(). Заодно отрабатывают
// отложенные таймеры view (singleShot(0) восстановления прокрутки).
// false — модель не успокоилась за timeoutMs.
inline bool settle(LogModel& model, int quietMs = 80, int timeoutMs = 60000)
{
    QElapsedTimer quiet;
    QElapsedTimer total;
    quiet.start();
    total.start();

    bool progressPending = false;
    auto c1 = QObject::connect(&model, &LogModel::modelFiltered, &model,
                               [&](int) { quiet.restart(); });
    auto c2 = QObject::connect(&model, &LogModel::filterProgress, &model,
                               [&](int p) {
                                   progressPending = (p < 100);
                                   quiet.restart();
                               });

    bool calm = false;
    int lastRows = model.rowCount();
    while (total.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        const int rows = model.rowCount();
        if (rows != lastRows) {
            lastRows = rows;
            quiet.restart();
        }
        // isFiltering(): тишина сама по себе не значит, что джоб закончился —
        // на медленной машине (санитайзеры) фоновый фильтр молчит дольше
        // quietMs между стартом и первым сообщением о прогрессе.
        if (!progressPending && !model.isFiltering() && quiet.elapsed() >= quietMs) {
            calm = true;
            break;
        }
    }

    QObject::disconnect(c1);
    QObject::disconnect(c2);
    return calm;
}

// Покой по СОСТОЯНИЮ, без окна тишины (P-6): модель не фильтрует, всё
// отложенное доставлено (queued-вызовы и QTimer::singleShot(0) — это posted
// events, как и UpdateRequest отрисовки, на которой view уточняет высоты), и
// busy() — отложенная работа, которую видно только снаружи (таймеры view:
// LogListView::hasPendingLayout) — молчит. Покой засчитывается на втором
// подряд спокойном проходе: обработка событий первого могла поставить новые.
// Таймеры не ждутся сверх того, что видно в busy(): тест, которому нужен
// ещё чей-то таймер, добавляет его в предикат. false — не успокоилось.
inline bool waitIdle(const std::function<bool()>& busy, int timeoutMs = 60000)
{
    QElapsedTimer total;
    total.start();
    int calmPasses = 0;
    while (total.elapsed() < timeoutMs) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (busy && busy()) {
            calmPasses = 0;
            continue;
        }
        if (++calmPasses >= 2)
            return true;
    }
    return false;
}

inline bool settleIdle(LogModel& model, const std::function<bool()>& busy = {},
                       int timeoutMs = 60000)
{
    return waitIdle([&] { return model.isFiltering() || (busy && busy()); }, timeoutMs);
}

// ---------------------------------------------------------------------------
// Документ: модель + её файлы
// ---------------------------------------------------------------------------

// Запасная кодировка автоопределения в тестах (F-1): как у машины с
// кириллической локалью — тогда decide выбирает между Windows-1251, IBM866 и
// KOI8-R. UTF-8-фикстуры от неё не зависят.
constexpr TextEncoding kTestFallbackEncoding = TextEncoding::Windows1251;

struct Document {
    std::unique_ptr<LogModel> model;
    QVector<LogFilePtr>       files;
    QStringList               errors;   // пусто — загрузка прошла чисто
    // Резидентный путь: кодировка файла (ключ — путь), как её держит вкладка
    // (LogViewWidget::m_fileEncodings) — для дозаписи. У индексного — в сторе.
    QHash<QString, FileEncoding> encodings;
    // Колонки вкладки (F-5): схема каждого файла дописывает свои колонки,
    // поля записей — в этих индексах (FieldLayout::schemaFor).
    FieldLayout layout;
    // Формат строк файла (F-17, шаг 7), как его держит вкладка
    // (LogViewWidget::m_fileFormats / IndexedLogStore::lineFormat).
    QHash<QString, LineFormat> formats;
};

// Схема файла документа (F-5, PR 2) — как у вкладки: файл читается схемой,
// поставленной ему в модели (LogModel::setFileSchema); первая схема файла
// дописывает недостающие колонки в раскладку документа. Пустая строка —
// без схемы (извлечение выключено).
inline FileSchemaPtr fileSchema(Document& doc, const QString& path, const QString& schema)
{
    // Файл JSON / logfmt (F-17, шаг 7): поля — ключи его формата строк,
    // главнее схемы (как LogViewWidget::applySchemaToModel).
    const QString format = doc.formats.value(path).fieldDefinition();
    if (!format.isEmpty()) {
        FileSchemaPtr known = doc.model->fileSchemas().byPath.value(path);
        if (known && known->definition == format)
            return known;
        FileSchemaPtr made = doc.layout.schemaFor(format);
        doc.model->setFileSchema(path, made);
        return made;
    }
    if (FileSchemaPtr known = doc.model->fileSchemas().byPath.value(path))
        if (known->definition == schema)
            return known;
    if (schema.isEmpty()) {
        doc.model->setFileSchema(path, nullptr);
        return nullptr;
    }
    FileSchemaPtr made = doc.layout.schemaFor(schema);
    doc.model->setFileSchema(path, made);
    return made;
}

// Решение загрузчика о формате строк → документ, как LogViewWidget::
// handleLineFormatDecided: колонки JSON-файла — ключи формата. true —
// решение пришло в дозаписи и читает строки иначе, чем Plain (вкладка
// перечитала бы файл).
inline bool adoptLineFormat(Document& doc, const LogFilePtr& lf, const LineFormat& format,
                            bool afterLines)
{
    const LineFormat before = doc.formats.value(lf->filePath);
    if (before.isUser())
        return false;
    doc.formats[lf->filePath] = format;
    if (IndexedLogStore* store = doc.model->indexedOrNull())
        store->setLineFormat(lf, format);
    if (format.isStructured())
        fileSchema(doc, lf->filePath, QString());
    return afterLines && format.readingSignature() != before.readingSignature();
}

// Решение парсера → состояние документа, как LogViewWidget::
// handleEncodingDecided (FileEncoding::adopt): выбор пользователя хранит
// автоматику рядом; true —
// пересмотр предварительного решения (шаг 3а): записи, отданные прежде,
// декодированы прежней кодировкой, файл надо перечитать.
inline bool adoptResidentEncoding(Document& doc, const LogFilePtr& lf, const FileEncoding& encoding)
{
    return doc.encodings[lf->filePath].adopt(encoding);
}

// Полный разбор файла с состоянием кодировки документа. Пересмотр посреди
// разбора — файл перечитывается целиком новым состоянием (как
// LogViewWidget::startReencode), пока решение не перестанет меняться.
// revisions — сколько перечитываний понадобилось.
inline QVector<std::shared_ptr<LogEntry>> parseResidentWhole(Document& doc, const LogFilePtr& logFile,
                                                              const QString& schema,
                                                              int* revisions = nullptr)
{
    QVector<std::shared_ptr<LogEntry>> entries;
    for (int pass = 0; pass < 16; ++pass) {
        LogParser parser;
        parser.setFallbackEncoding(kTestFallbackEncoding);
        entries.clear();
        bool done = false;
        bool revised = false;
        QObject relay;
        QObject::connect(&parser, &LogParser::encodingDecided, &relay,
                         [&](const LogFilePtr& lf, const FileEncoding& encoding) {
                             revised = adoptResidentEncoding(doc, lf, encoding) || revised;
                         });
        QObject::connect(&parser, &LogParser::lineFormatDecided, &relay,
                         [&](const LogFilePtr& lf, const LineFormat& format, bool afterLines) {
                             adoptLineFormat(doc, lf, format, afterLines);
                         });
        QObject::connect(&parser, &LogParser::entriesParsed, &relay,
                         [&](const QVector<std::shared_ptr<LogEntry>>& batch,
                             const LogFilePtr&) { entries += batch; });
        QObject::connect(&parser, &LogParser::parsingFinished, &relay,
                         [&](int, const LogFilePtr&) { done = true; });
        QObject::connect(&parser, &LogParser::parsingFailed, &relay,
                         [&](const LogFilePtr&) { done = true; });
        fileSchema(doc, logFile->filePath, schema);
        parser.startParsing(logFile, doc.encodings.value(logFile->filePath),
                            doc.formats.value(logFile->filePath));
        if (!waitFor([&done] { return done; })) {
            doc.errors << QStringLiteral("резидентный парсинг %1 не завершился").arg(logFile->filePath);
            break;
        }
        if (!revised)
            break;
        if (revisions)
            ++*revisions;
    }
    return entries;
}

// Резидентный путь: LogParser → mergeEntries, как в LogViewWidget. Файлы
// добавляются в УЖЕ существующую модель — так же, как дописывание файла во
// вкладку (строки второго файла встают в середину по времени).
inline void addResidentFiles(Document& doc, const QStringList& paths, const QString& schema)
{
    for (const QString& path : paths) {
        auto logFile = std::make_shared<LogFile>(path);
        doc.files.append(logFile);
        QVector<std::shared_ptr<LogEntry>> entries = parseResidentWhole(doc, logFile, schema);
        // Порядок подачи как в production: батчи по 5000 сортируются и
        // сливаются.
        // Локатору (R-1) — порция в порядке файла, после слияния, и конец
        // прохода: как LogViewWidget::handleEntriesParsed / handleParsingFinished.
        for (qsizetype from = 0; from < entries.size(); from += 5000) {
            const QVector<std::shared_ptr<LogEntry>> fileOrder = entries.mid(from, 5000);
            QVector<std::shared_ptr<LogEntry>> batch = fileOrder;
            std::sort(batch.begin(), batch.end(), logEntryPtrLess);
            doc.model->mergeEntries(batch);
            doc.model->recordLocator().residentBatch(fileOrder);
        }
        doc.model->recordLocator().fileLoaded(logFile);
    }
}

inline Document buildResident(const QStringList& paths, const QString& schema)
{
    Document doc;
    doc.model = std::make_unique<LogModel>();
    addResidentFiles(doc, paths, schema);
    if (!settle(*doc.model))
        doc.errors << QStringLiteral("резидентная модель не пришла в покой после загрузки");
    return doc;
}

// Смена состояния кодировки файла документа — как LogViewWidget::
// setFileEncoding / restoreAutomaticEncoding (F-1, шаг 4): индексный —
// IndexedLogStore::setFileEncodingState (без переиндексации, reset);
// резидентный — файл перечитывается в сторону и его записи подменяются под
// одним reset (LogModel::replaceEntriesForFile), как startReencode.
inline void setDocumentEncoding(Document& doc, const LogFilePtr& lf, const FileEncoding& encoding,
                                const QString& schema)
{
    if (IndexedLogStore* store = doc.model->indexedOrNull()) {
        store->setFileEncodingState(lf, encoding);
        return;
    }
    doc.encodings[lf->filePath] = encoding;
    QVector<std::shared_ptr<LogEntry>> entries = parseResidentWhole(doc, lf, schema);
    std::sort(entries.begin(), entries.end(), logEntryPtrLess);
    doc.model->replaceEntriesForFile(lf->filePath, entries);
}

// Как файлы попадают в индексную вкладку. Порядок строк у стора собирается
// по-разному, поэтому оба сценария проверяются отдельно:
//   • Sequential — файл полностью проиндексирован, затем добавлен следующий
//     («дописал файл в открытую вкладку»): второй файл подключается к уже
//     показанным строкам первого (materializeAllRefs);
//   • Concurrent — все файлы подключены ДО первого батча и индексируются
//     параллельно («открыл несколько файлов разом», объединение вкладок,
//     конверсия вкладки в индексную): батчи разных файлов приходят вперемешку.
enum class IndexedLoad { Sequential, Concurrent };

inline QString describe(IndexedLoad mode)
{
    return mode == IndexedLoad::Sequential ? QStringLiteral("по очереди")
                                           : QStringLiteral("разом");
}

// Индексатор в тестах: запасная кодировка — как у вкладки, плюс решение о
// кодировке → стор (LogViewWidget::ensureIndexer), формат строк → документ.
inline void configureIndexer(LogIndexer& indexer, IndexedLogStore* store, QObject* context,
                             Document* doc = nullptr)
{
    indexer.setFallbackEncoding(kTestFallbackEncoding);
    if (doc) {
        QObject::connect(&indexer, &LogIndexer::lineFormatDecided, context,
                         [doc](const LogFilePtr& lf, const LineFormat& format, bool afterLines) {
                             if (adoptLineFormat(*doc, lf, format, afterLines))
                                 doc->errors << QStringLiteral("формат строк решён в дозаписи "
                                                               "индексного документа: %1")
                                                    .arg(lf->filePath);
                         });
    }
    QObject::connect(&indexer, &LogIndexer::encodingDecided, context,
                     [store](const LogFilePtr& lf, const FileEncoding& encoding) {
                         store->adoptDetectedEncoding(lf, encoding);
                     });
}

// Индексный путь: LogIndexer → attachFile/appendIndexedRows, как в
// LogViewWidget::startIndexedLoad + handleIndexBatchReady. Сигналы индексатора
// приходят из воркера очередью — порядок «батчи, затем finished» сохраняется.
// Модель документа переводится на индексный бэкенд, если ещё не переведена.
// schemas (F-5) — своя схема каждому файлу (по порядку paths) вместо schema:
// слитая вкладка из файлов с разными схемами.
inline void addIndexedFiles(Document& doc, const QStringList& paths, const QString& schema,
                            IndexedLoad mode = IndexedLoad::Sequential,
                            const QStringList& schemas = {})
{
    IndexedLogStore* store = doc.model->indexedOrNull();
    if (!store)
        store = doc.model->convertToIndexedBackend();

    struct Job {
        LogFilePtr                  logFile;
        std::shared_ptr<LineIndex>  index;
        std::unique_ptr<LogIndexer> indexer;
        bool                        done = false;
        bool                        fallback = false;
    };
    std::vector<std::unique_ptr<Job>> jobs;
    QObject relay;

    const auto prepare = [&](const QString& path) -> Job& {
        auto job = std::make_unique<Job>();
        job->logFile = std::make_shared<LogFile>(path);
        job->index = std::make_shared<LineIndex>();
        job->indexer = std::make_unique<LogIndexer>();
        configureIndexer(*job->indexer, store, &relay, &doc);
        doc.files.append(job->logFile);
        // Схема файла — у модели до подключения: стор берёт её при attach.
        const int at = int(paths.indexOf(path));
        fileSchema(doc, path, schemas.isEmpty() ? schema : schemas.value(at));
        store->attachFile(job->logFile, job->index);
        if (doc.formats.contains(path))
            store->setLineFormat(job->logFile, doc.formats.value(path));

        Job* raw = job.get();
        LogModel* model = doc.model.get();
        QObject::connect(raw->indexer.get(), &LogIndexer::indexBatchReady, &relay,
                         [store, model](const LogFilePtr& lf, qint64 first, qint64 count) {
                             store->appendIndexedRows(lf, first, count, false);
                             model->recordLocator().indexedRange(lf, first, count);
                         });
        QObject::connect(raw->indexer.get(), &LogIndexer::indexingFinished, &relay,
                         [raw, model](qint64, const LogFilePtr& lf) {
                             model->recordLocator().fileLoaded(lf, raw->index->endOffset());
                             raw->done = true;
                         });
        QObject::connect(raw->indexer.get(), &LogIndexer::indexingFailed, &relay,
                         [raw](const LogFilePtr&) { raw->done = true; });
        QObject::connect(raw->indexer.get(), &LogIndexer::needsResidentFallback, &relay,
                         [raw](const LogFilePtr&, const QString&) {
                             raw->fallback = true;
                             raw->done = true;
                         });
        jobs.push_back(std::move(job));
        return *raw;
    };

    if (mode == IndexedLoad::Sequential) {
        for (const QString& path : paths) {
            Job& job = prepare(path);
            job.indexer->startIndexing(job.logFile, job.index, FileEncoding(),
                                       doc.formats.value(job.logFile->filePath));
            waitFor([&job] { return job.done; });
        }
    } else {
        for (const QString& path : paths)
            prepare(path);
        for (auto& job : jobs)
            job->indexer->startIndexing(job->logFile, job->index, FileEncoding(),
                                        doc.formats.value(job->logFile->filePath));
        waitFor([&jobs] {
            for (const auto& job : jobs)
                if (!job->done)
                    return false;
            return true;
        });
    }

    for (const auto& job : jobs) {
        if (!job->done)
            doc.errors << QStringLiteral("индексация %1 не завершилась").arg(job->logFile->filePath);
        if (job->fallback)
            doc.errors << QStringLiteral("неожиданный откат в резидентный путь: %1")
                              .arg(job->logFile->filePath);
    }
}

inline Document buildIndexed(const QStringList& paths, const QString& schema,
                             IndexedLoad mode = IndexedLoad::Sequential)
{
    Document doc;
    doc.model = std::make_unique<LogModel>();
    addIndexedFiles(doc, paths, schema, mode);
    if (!settle(*doc.model))
        doc.errors << QStringLiteral("индексная модель не пришла в покой после загрузки");
    return doc;
}

// ---------------------------------------------------------------------------
// Дозапись хвоста — как авто-обновление вкладки (LogViewWidget::
// reloadChangedFiles): байты дописываются в файл и дочитываются только они.
// Строка без '\n' в конце остаётся «предварительной» и при следующей дозаписи
// переиндексируется. false — дописать или дочитать не удалось.
// ---------------------------------------------------------------------------

inline bool appendToFile(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Append) && f.write(bytes) == bytes.size();
}

// markNew = false — дописанные строки не помечаются «новыми» (сравнение с
// вкладкой, загрузившей те же байты разом).
inline bool appendIndexedTail(Document& doc, int fileIndex, const QByteArray& bytes,
                              const QString& schema, bool markNew = true)
{
    IndexedLogStore* store = doc.model->indexedOrNull();
    const LogFilePtr logFile = doc.files.value(fileIndex);
    if (!store || !logFile || !appendToFile(logFile->filePath, bytes))
        return false;
    const std::shared_ptr<LineIndex> index = store->indexForFile(logFile->filePath);
    if (!index)
        return false;

    LogIndexer indexer;
    bool done = false;
    bool ok = true;
    QObject relay;
    configureIndexer(indexer, store, &relay, &doc);
    LogModel* model = doc.model.get();
    QObject::connect(&indexer, &LogIndexer::indexBatchReady, &relay,
                     [store, model, markNew](const LogFilePtr& lf, qint64 first, qint64 count) {
                         store->appendIndexedRows(lf, first, count, markNew);
                         model->recordLocator().indexedRange(lf, first, count);
                     });
    QObject::connect(&indexer, &LogIndexer::indexingFinished, &relay,
                     [&done, model, index](qint64, const LogFilePtr& lf) {
                         model->recordLocator().fileLoaded(lf, index->endOffset());
                         done = true;
                     });
    QObject::connect(&indexer, &LogIndexer::indexingFailed, &relay,
                     [&](const LogFilePtr&) { ok = false; done = true; });
    fileSchema(doc, logFile->filePath, schema);
    indexer.startIndexingFrom(logFile, index, index->lastLineProvisional(),
                              store->fileEncoding(logFile), store->lineFormat(logFile));
    return waitFor([&done] { return done; }) && ok;
}

// Резидентный путь дочитывает только завершённые строки: хвост без '\n'
// передавать сюда не нужно.
inline bool appendResidentTail(Document& doc, int fileIndex, const QByteArray& bytes,
                               const QString& schema)
{
    const LogFilePtr logFile = doc.files.value(fileIndex);
    if (!logFile)
        return false;
    const qint64 oldSize = QFileInfo(logFile->filePath).size();
    if (!appendToFile(logFile->filePath, bytes))
        return false;

    // Контекст дочитывания — как ведёт его вкладка: последняя строка файла
    // (её запись продолжает строка продолжения в начале дозаписи) и
    // следующий id записи.
    LogParser::ResumeContext context;
    std::shared_ptr<LogEntry> last;
    for (const auto& entry : doc.model->residentEntries()) {
        if (!entry || entry->sourceFile() != logFile)
            continue;
        context.nextLogicalEntryId = std::max(context.nextLogicalEntryId,
                                              entry->logicalEntryId() + 1);
        if (!last || entry->originalLineNumber() > last->originalLineNumber())
            last = entry;
    }
    if (last) {
        context.currentLogicalEntryId = last->logicalEntryId();
        context.currentTimestamp = last->timestamp();
        context.currentLevel = last->level();
        context.nextLineNumber = last->originalLineNumber() + 1;
        // Первая строка записи (F-17, шаг 3) — её берёт продолжение в дозаписи.
        context.currentHead = last->recordHead() ? last->recordHead()
                                                 : std::shared_ptr<const LogEntry>(last);
    }

    LogParser parser;
    parser.setFallbackEncoding(kTestFallbackEncoding);
    QVector<QVector<std::shared_ptr<LogEntry>>> batches;
    bool done = false;
    bool revised = false;
    QObject relay;
    QObject::connect(&parser, &LogParser::encodingDecided, &relay,
                     [&](const LogFilePtr& lf, const FileEncoding& encoding) {
                         revised = adoptResidentEncoding(doc, lf, encoding) || revised;
                     });
    QObject::connect(&parser, &LogParser::lineFormatDecided, &relay,
                     [&](const LogFilePtr& lf, const LineFormat& format, bool afterLines) {
                         revised = adoptLineFormat(doc, lf, format, afterLines) || revised;
                     });
    QObject::connect(&parser, &LogParser::entriesParsed, &relay,
                     [&](const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr&) {
                         batches.append(batch);
                     });
    QObject::connect(&parser, &LogParser::parsingFinished, &relay,
                     [&done](int, const LogFilePtr&) { done = true; });
    QObject::connect(&parser, &LogParser::parsingFailed, &relay,
                     [&done](const LogFilePtr&) { done = true; });
    // Кодировка хвоста (F-1) — состояние файла: «не решено» решит хвост.
    fileSchema(doc, logFile->filePath, schema);
    parser.startParsingFrom(logFile, oldSize, context, TextEncoding::Utf8,
                            doc.encodings.value(logFile->filePath),
                            doc.formats.value(logFile->filePath));
    if (!waitFor([&done] { return done; }))
        return false;
    for (const auto& batch : batches) {
        doc.model->appendEntries(batch);
        doc.model->recordLocator().residentBatch(batch);
    }
    doc.model->recordLocator().fileLoaded(logFile);
    if (revised) {
        // Пересмотр кодировки хвостом (шаг 3а) или формат строк, решённый
        // дозаписью малого файла (F-17, шаг 7): файл перечитывается целиком и
        // его записи подменяются под одним reset — LogViewWidget::startReencode.
        QVector<std::shared_ptr<LogEntry>> entries = parseResidentWhole(doc, logFile, schema);
        std::sort(entries.begin(), entries.end(), logEntryPtrLess);
        doc.model->replaceEntriesForFile(logFile->filePath, entries);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Номера физических строк (R-1): эталон — разбиение байтов файла. Строка — то,
// что кончается '\n' ('\r' перед ним снимается), плюс хвост без '\n'; пустая
// строка — тоже строка, в том числе последняя; за завершающим '\n' строки нет.
// Сверяется в виде «номер: текст», по возрастанию номера (порядок строк
// одно-файловой вкладки у бэкендов законно разный).
// ---------------------------------------------------------------------------

inline QStringList numbered(const QStringList& lines)
{
    QStringList out;
    for (int i = 0; i < lines.size(); ++i)
        out << QStringLiteral("%1: %2").arg(i + 1).arg(lines[i]);
    return out;
}

inline QStringList expectedPhysicalLines(QByteArray bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);
    QList<QByteArray> parts = bytes.split('\n');
    if (!parts.isEmpty() && parts.last().isEmpty())
        parts.removeLast(); // за завершающим '\n' строки нет
    QStringList lines;
    for (QByteArray& part : parts) {
        if (part.endsWith('\r'))
            part.chop(1);
        lines << QString::fromUtf8(part);
    }
    return numbered(lines);
}

// Строки документа по номерам из ключа строки (LogModel::keyAt). Повтор номера
// помечается, а не теряется.
inline QStringList numberedLines(const LogModel& model)
{
    QMap<qint64, QString> byLine;
    for (int row = 0; row < model.rowCount(); ++row) {
        const RecordKey key = model.keyAt(row);
        if (byLine.contains(key.line))
            byLine[key.line] += QStringLiteral(" <ПОВТОР НОМЕРА> ") + model.messageAt(row);
        else
            byLine.insert(key.line, model.messageAt(row));
    }
    QStringList out;
    for (auto it = byLine.constBegin(); it != byLine.constEnd(); ++it)
        out << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
    return out;
}

} // namespace testdocs

#endif // TESTDOCUMENTS_H
