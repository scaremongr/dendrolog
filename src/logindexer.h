#ifndef LOGINDEXER_H
#define LOGINDEXER_H

#include "lineclassifier.h"
#include "lineindex.h"
#include "logfile.h"
#include "lineformat.h"
#include "textencoding.h"
#include <QObject>
#include <QThreadPool>
#include <atomic>
#include <memory>

// ============================================================================
// LogIndexer — асинхронное построение LineIndex по файлу лога. Аналог
// LogParser для индексного бэкенда: та же модель жизненного цикла (приватный
// пул, atomic-abort, деструктор ждёт воркеров), тот же LineClassifier и та же
// семантика первичных/continuation-строк, но текст строк НЕ материализуется.
//
// Критические инварианты:
//   • файл открывается БЕЗ QIODevice::Text — все смещения сырые байтовые
//     (совместимы с якорями FileChangeDetector); QTextStream не используется
//     (он не даёт байтовых позиций);
//   • разрез по '\n', '\r' перед ним учитывается в eolBytes и исключается
//     из текста; хвост файла без '\n' индексируется «предварительной»
//     строкой (eolBytes = 0, а с одиночным '\r' на конце —
//     LineIndex::kEolProvisionalCr: '\r' не текст) и переиндексируется при
//     дозаписи; признак — LineIndex::lastLineProvisional(), а не eolBytes;
//   • UTF-8 BOM пропускается (строки начинаются после него); UTF-16/32 BOM →
//     needsResidentFallback (эти кодировки умеет только резидентный путь);
//   • декодирование по строке декодером файла (F-1, TextDecoder). Пока файл
//     «не решён» (FileEncoding::Undecided — встречались только ASCII-байты),
//     каждый прочитанный буфер проверяется на байт ≥ 0x80; найдя его,
//     индексатор решает по окну EncodingDetector::decide (окно — от этого
//     байта на kWindowBytes вперёд, недостающее дочитывается вторым
//     дескриптором), шлёт encodingDecided ДО публикации следующей порции и
//     продолжает новым декодером. Строки до решения — ASCII, их текст от
//     решения не меняется, поэтому переиндексация не нужна. После решения
//     проверка не выполняется. Решения нет (nullopt: последовательность
//     оборвана концом файла) — файл остаётся «не решённым», решит дозапись;
//   • индекс от декодера не зависит (классификатор смотрит только на ASCII —
//     tests/textencoding, testClassifierIsAsciiOnly); декодер влияет только
//     на текст строки (ключи JSON / logfmt формата строк — тоже ASCII);
//   • формат строк файла (F-17, шаг 7, lineformat.h) решается в начале
//     прохода (LineFormatDetector::advance) и сообщается lineFormatDecided
//     раньше первой порции; схему полей индексатор не знает.
//
// Дозапись (startIndexingFrom) наследует контекст логической записи из
// самого индекса: continuation-строки после границы дозаписи присоединяются
// к последней записи (резидентный инкрементальный парсер контекст терял —
// здесь поведение строго лучше).
// ============================================================================
class LogIndexer : public QObject
{
    Q_OBJECT

public:
    explicit LogIndexer(QObject* parent = nullptr);
    ~LogIndexer() override;

    // Запасная однобайтная кодировка автоопределения (F-1): её выбирает
    // decide, если окно не похоже на UTF-8. Умолчание — по локали системы;
    // настройка пользователя — шаг 4 F-1.
    void setFallbackEncoding(TextEncoding encoding) { m_fallbackEncoding = encoding; }
    TextEncoding fallbackEncoding() const { return m_fallbackEncoding; }

public slots:
    // Полная индексация с нуля; index должен быть свежесозданным (пустым).
    // encoding — кодировка файла на старте (обычно «не решена»; выбор
    // пользователя переживает перечитывание переписанного файла).
    // lineFormat — формат строк файла на этот проход (F-17, шаг 7): «не
    // решён» — решается по началу файла до первой строки (сигнал
    // lineFormatDecided раньше первой порции). Схемы полей индексатор не знает.
    void startIndexing(const LogFilePtr& logFile,
                       const std::shared_ptr<LineIndex>& index,
                       FileEncoding encoding = FileEncoding(),
                       LineFormat lineFormat = {});
    // Дозапись хвоста с index->endOffset(). reindexProvisionalTail — сначала
    // переиндексировать последнюю строку, если она была без '\n'
    // (truncateFrom + чтение с её начала); store обязан согласованно убрать
    // её строку из модели до применения новых батчей. encoding — текущая
    // кодировка файла у стора (IndexedLogStore::fileEncoding): «не решено»
    // переживает дозапись, решённая читается своим декодером.
    void startIndexingFrom(const LogFilePtr& logFile,
                           const std::shared_ptr<LineIndex>& index,
                           bool reindexProvisionalTail,
                           FileEncoding encoding = FileEncoding(),
                           LineFormat lineFormat = {});

signals:
    void indexingStarted(const LogFilePtr& logFile);
    // Опубликована очередная порция: строки [firstLine, firstLine + count).
    void indexBatchReady(const LogFilePtr& logFile, qint64 firstLine, qint64 count);
    void indexingProgress(int progressPercentage, const LogFilePtr& logFile);
    void indexingFinished(qint64 newLines, const LogFilePtr& logFile);
    void indexingFailed(const LogFilePtr& logFile);
    // Файл не в UTF-8 (BOM UTF-16/32) — открыть через резидентный путь.
    void needsResidentFallback(const LogFilePtr& logFile, const QString& reason);
    // Кодировка файла решена (BOM UTF-8 или окно у первого байта ≥ 0x80).
    // Приходит раньше порции, в которой есть первая не-ASCII строка; строки
    // прежних порций — ASCII, их текст не меняется (IndexedLogStore::
    // adoptDetectedEncoding, без reset).
    void encodingDecided(const LogFilePtr& logFile, const FileEncoding& encoding);
    // Формат строк решён в начале прохода (как у LogParser::lineFormatDecided).
    void lineFormatDecided(const LogFilePtr& logFile, const LineFormat& format, bool afterLines);

private:
    void doIndex(const LogFilePtr& logFile, std::shared_ptr<LineIndex> index,
                 LineFormat lineFormat,
                 bool initial, bool reindexProvisionalTail,
                 FileEncoding encoding, TextEncoding fallback);

    const LineClassifier m_classifier;
    TextEncoding m_fallbackEncoding = TextEncodings::systemFallback();

    // Задачи индексации живут в приватном пуле; деструктор ждёт их завершения,
    // чтобы воркер не пережил объект (паттерн LogParser).
    QThreadPool      m_pool;
    std::atomic_bool m_abort{false};
};

#endif // LOGINDEXER_H
