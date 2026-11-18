#ifndef RECORDID_H
#define RECORDID_H

#include "logfile.h"

#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <limits>
#include <optional>

// ============================================================================
// Идентификатор строки лога (R-1, docs/design/R-1.md).
//
// RecordKey — ключ ФИЗИЧЕСКОЙ строки в загруженном документе, только в памяти:
// файл, номер строки (с 1, как LogEntry::originalLineNumber()), id логической
// записи и её метка времени. Вместе это полный ключ сортировки
// LogEntry::operator<, поэтому строка находится двоичным поиском на обоих
// бэкендах (LogStore::keyAt / rowForKey / nearestRowForKey), без памяти на
// строку.
//
// Ловушки:
//   • ключ действителен, пока строка лежит в документе с теми же id записи и
//     меткой: после полной перезагрузки файла или смены схемы с другими
//     границами записей rowForKey честно отвечает -1, а не находит строку по
//     одному номеру. Заново ключ получают из сохранённого идентификатора
//     (RecordLocator, шаг 3 R-1);
//   • файл держится сильной ссылкой: закрытый файл не оставляет висячего
//     указателя, а новый LogFile не займёт его адрес. Сравнение файлов — по
//     указателю, как у LogModel::EntryKey; путь участвует только в порядке
//     строк слитой вкладки (зеркало LogEntry::operator<);
//   • метка — момент в мс epoch; «метки нет» — kNoTimestamp, а не -1: метка
//     до 1970 года — законный отрицательный момент.
// ============================================================================
struct RecordKey {
    static constexpr qint64 kNoTimestamp = std::numeric_limits<qint64>::min();

    LogFilePtr file;
    qint64 line = 0;                // с 1
    int logicalEntryId = -1;
    qint64 timestampMs = kNoTimestamp;

    bool isValid() const { return file && line > 0; }
    bool hasTimestamp() const { return timestampMs != kNoTimestamp; }

    friend bool operator==(const RecordKey& a, const RecordKey& b)
    {
        return a.file == b.file && a.line == b.line
            && a.logicalEntryId == b.logicalEntryId && a.timestampMs == b.timestampMs;
    }
    friend bool operator!=(const RecordKey& a, const RecordKey& b) { return !(a == b); }
};

// ============================================================================
// Сохраняемые идентификаторы (R-1, шаг 2): SourceIdentity — тот ли это файл и
// цел ли прочитанный префикс, RecordId — строка в проверенном источнике. Оба
// пишутся на диск (JSON с версией формата и именем алгоритма) и разрешаются в
// RecordKey заново после перезапуска (RecordLocator, шаг 3).
//
// Отпечатки — SHA-1, первые 8 байт (64 бита), в JSON — 16 шестнадцатеричных
// цифр. Не qHash: тот не обязан совпадать между запусками и версиями Qt.
//
//   • область файла (начало и 4 КБ перед сохранённым концом) — по сырым
//     байтам (инвариант 5);
//   • строка — по её ASCII-СКЕЛЕТУ: символы < U+0080 как есть, каждая серия
//     символов ≥ U+0080 — один байт-заполнитель 0x80. Байтовый вариант
//     (ofLineBytes) сворачивает так же серии байтов ≥ 0x80. Все кодировки,
//     которые читает TextDecoder (UTF-8 и однобайтные), ASCII-совместимы и
//     тотальны: ASCII-байт — тот же символ, серия байтов ≥ 0x80 — серия
//     символов ≥ U+0080 (битый UTF-8 — U+FFFD, неопределённый байт
//     однобайтной — C1). Поэтому отпечаток одной строки файла одинаков на
//     обоих бэкендах, в любой кодировке, которой файл прочитан, и по сырым
//     байтам, без декодирования. Цена — строки, отличающиеся только не-ASCII
//     текстом при той же раскладке серий, неразличимы: первая линия защиты —
//     номер строки и идентичность источника, отпечаток — вторая.
//     UTF-16/32 — только по декодированному тексту (ofLineText).
//
// Ловушки:
//   • отпечаток строки не считается в горячем пути загрузки: только по запросу
//     для конкретной строки (создание закладки, проверка разрешённого ключа);
//   • текст строки — без EOL: '\r' перед '\n' и одиночный '\r' в конце
//     оборванного хвоста оба бэкенда снимают одинаково;
//   • недописанная последняя строка (без '\n') отпечатка не имеет
//     (hasText == false): её текст ещё растёт;
//   • путь в SourceIdentity — что открыл пользователь, абсолютный; сравнить
//     содержимое без пути — sameContent / checkAt (переименованный файл,
//     вторичный поиск памяти файла в R-4).
// ============================================================================
namespace RecordFingerprint {

// Имена алгоритмов в JSON (ключ "hash").
inline constexpr char kRegionAlgorithm[] = "sha1-64";
inline constexpr char kLineAlgorithm[]   = "sha1-64-ascii";

// Сырые байты области файла.
quint64 ofBytes(QByteArrayView bytes);
// Текст строки без EOL (как его отдают LogModel::messageAt и LogEntry).
quint64 ofLineText(QStringView text);
// Сырые байты строки без EOL в ASCII-совместимой кодировке (UTF-8,
// однобайтные): равно ofLineText(текст строки в любой такой кодировке).
quint64 ofLineBytes(QByteArrayView bytes);

QString toHex(quint64 fingerprint);
// Ровно 16 шестнадцатеричных цифр; иначе nullopt.
std::optional<quint64> fromHex(QStringView hex);

} // namespace RecordFingerprint

// Строка в источнике: номер физической строки (с 1, как RecordKey::line) и
// отпечаток её текста.
struct RecordId {
    static constexpr int kFormatVersion = 1;

    qint64  line = 0;
    quint64 text = 0;      // RecordFingerprint::ofLineText
    bool    hasText = false; // false — строка была недописанной (без '\n')

    bool isValid() const { return line > 0; }

    // complete == false — последняя строка без '\n': без отпечатка.
    static RecordId forLine(qint64 line, QStringView lineText, bool complete = true);
    // Совпадает ли текст строки. Без отпечатка (недописанная строка) — да:
    // проверены номер строки и источник.
    bool matchesText(QStringView lineText) const;
    bool matchesBytes(QByteArrayView lineBytes) const;

    // {"version":1,"hash":"sha1-64-ascii","line":185,"text":"a1b2…"};
    // у недописанной строки вместо "text" — "partial":true (без явной метки
    // пропавший отпечаток — битые данные, а не недописанная строка).
    QJsonObject toJson() const;
    // Неизвестная версия или алгоритм, битые и недостающие данные — nullopt и
    // причина в error; лишние ключи не мешают.
    static std::optional<RecordId> fromJson(const QJsonObject& json, QString* error = nullptr);

    friend bool operator==(const RecordId& a, const RecordId& b)
    {
        return a.line == b.line && a.hasText == b.hasText && (!a.hasText || a.text == b.text);
    }
    friend bool operator!=(const RecordId& a, const RecordId& b) { return !(a == b); }
};

// Идентичность источника: путь, размер на момент сохранения (конец
// прочитанного префикса), отпечаток начала файла (min(4 КБ, размер) байтов) и
// 4 КБ перед сохранённым концом. Снятие и проверка — два чтения по 4 КБ, без
// прохода по файлу.
struct SourceIdentity {
    static constexpr int    kFormatVersion = 1;
    static constexpr qint64 kRegionBytes = 4096;

    enum class State {
        Valid,     // файл есть, не короче сохранённого, оба отпечатка совпали
        Changed,   // короче сохранённого или не совпал отпечаток
        Missing,   // файла по пути нет (или он не читается)
        Ephemeral, // спул stdin: не сохраняется, ключи живут до конца процесса
    };

    QString path;          // абсолютный (fromJson относительный не примет)
    qint64  size = -1;
    quint64 head = 0;
    quint64 tail = 0;
    bool    ephemeral = false;

    bool isValid() const { return ephemeral ? !path.isEmpty() : (!path.isEmpty() && size >= 0); }

    // Снять идентичность файла по его первым size байтам (size < 0 — текущий
    // размер файла). nullopt — файла нет, он не читается или короче size.
    // Спул stdin (StdinSpooler::isSpoolFilePath) — Ephemeral без чтения.
    static std::optional<SourceIdentity> capture(const QString& filePath, qint64 size = -1);

    // Проверка по сохранённому пути.
    State check() const { return checkAt(path); }
    // Проверка содержимого по ДРУГОМУ пути (переименованный, перенесённый
    // файл): те же правила, путь не сравнивается.
    State checkAt(const QString& filePath) const;
    // Те же байты префикса, путь не учитывается (две снятые идентичности).
    bool sameContent(const SourceIdentity& other) const
    {
        return !ephemeral && !other.ephemeral && isValid() && other.isValid()
            && size == other.size && head == other.head && tail == other.tail;
    }

    // {"version":1,"hash":"sha1-64","path":…,"size":…,"head":…,"tail":…};
    // у Ephemeral — пустой объект: спул на диск не пишется.
    QJsonObject toJson() const;
    static std::optional<SourceIdentity> fromJson(const QJsonObject& json, QString* error = nullptr);

    friend bool operator==(const SourceIdentity& a, const SourceIdentity& b)
    {
        // Регистр пути — как у файловой системы платформы.
#ifdef Q_OS_WIN
        constexpr Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
        return a.path.compare(b.path, cs) == 0 && a.ephemeral == b.ephemeral
            && (a.ephemeral || a.sameContent(b));
    }
    friend bool operator!=(const SourceIdentity& a, const SourceIdentity& b) { return !(a == b); }
};

#endif // RECORDID_H
