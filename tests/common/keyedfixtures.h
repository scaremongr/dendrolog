#ifndef KEYEDFIXTURES_H
#define KEYEDFIXTURES_H

// ============================================================================
// keyedfixtures.h — фикстуры ключевой схемы (F-4, PR 2): JSON Lines и logfmt
// с заранее заданными моментами и уровнями каждой строки. Эталон — значения,
// записанные здесь, а не второй разборщик: общую ошибку классификатора
// сравнение бэкендов не увидит.
//
// Запись i — момент base + i секунд + своя доля секунды (метки неубывают,
// поэтому одно-файловая индексная вкладка сравнима с резидентной строго).
// Формы идут по кругу: ISO с Z и UTF-8 кириллицей, эпоха в секундах с
// дробью и pino-уровень 30, эпоха в мс и уровень 50 с \u-кириллицей,
// эпоха в нс строкой и уровень по пути log.level (logfmt — дословный ключ
// log.level) с экранированным «\\», эпоха в мкс и многострочная запись
// (исключение, стек, строка-объект без ключей ролей, пустая строка),
// обычная строка без ключей (эвристика: ведущая метка), нечитаемые ключи
// (откат на эвристику по всей строке), зона +02:00, syslog-уровень 3 и
// вложенный объект, за которым идёт pretty-printed JSON (строки с отступом
// — продолжения).
// ============================================================================

#include "keyedline.h"
#include "logentry.h"
#include "logpattern.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QTime>
#include <QTimeZone>
#include <QVector>

namespace keyedfx {

// Ключевая схема формата с колонкой Thread (её знают конфигурации фильтров
// tests/common/filterconfigs.h): колонки Timestamp, Level, Message, Thread.
inline QString schema(KeyedDefinition::Format format)
{
    PatternDefinition def;
    KeyedDefinition k;
    k.format = format;
    k.extraColumns = {QStringLiteral("Thread")};
    def.keyed = k;
    return LogPattern::serializeDefinition(def);
}

struct Record {
    qint64 ms = kNoTimestampMs;
    LogLevel level = LogLevel::Unknown;
    QString message;   ///< Ожидаемое значение колонки Message (пусто — не проверяется).
};

struct Fixture {
    QByteArray bytes;
    QVector<int> lineRecord;   ///< Номер записи каждой физической строки (с 0).
    QVector<Record> records;
};

inline qint64 baseMs()
{
    return QDateTime(QDate(2026, 3, 5), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();
}

// «2026-03-05T10:MM:SS.fffZ» для момента base + seconds + frac мс (UTC);
// hourShift — запись в зоне +hh:00 (часы сдвинуты, момент тот же).
inline QByteArray iso(int seconds, int frac, int hourShift = 0)
{
    const QDateTime t = QDateTime::fromMSecsSinceEpoch(baseMs() + seconds * 1000LL + frac,
                                                       QTimeZone::UTC)
                            .addSecs(hourShift * 3600);
    QByteArray s = t.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz")).toLatin1();
    if (hourShift == 0)
        return s + 'Z';
    return s + (hourShift > 0 ? '+' : '-') + QByteArray::number(qAbs(hourShift)).rightJustified(2, '0')
         + ":00";
}

inline QByteArray num(qint64 v) { return QByteArray::number(v); }

// records — число записей; формы по кругу (8 штук).
inline Fixture make(KeyedDefinition::Format format, int records)
{
    const bool json = format == KeyedDefinition::Format::JsonObject;
    Fixture fx;
    const auto line = [&](const QByteArray& text) {
        fx.bytes += text;
        fx.bytes += '\n';
        fx.lineRecord.append(int(fx.records.size()) - 1);
    };
    const auto record = [&](qint64 ms, LogLevel level, const QString& message,
                            const QByteArray& text) {
        fx.records.append(Record{ms, level, message});
        line(text);
    };
    for (int i = 0; i < records; ++i) {
        const qint64 second = baseMs() + i * 1000LL;
        const QByteArray n = num(i);
        switch (i % 8) {
        case 0:   // ISO с Z, слово уровня, UTF-8 кириллица
            record(second + 123, LogLevel::Info, QStringLiteral("Запрос %1 ok").arg(i),
                   json ? "{\"ts\":\"" + iso(i, 123) + "\",\"level\":\"info\",\"Thread\":\"worker-1\","
                          "\"msg\":\"Запрос " + n + " ok\"}"
                        : "ts=" + iso(i, 123) + " level=info Thread=worker-1 msg=\"Запрос " + n
                              + " ok\"");
            break;
        case 1:   // эпоха в секундах с дробью, pino 30, экранированные кавычки
            record(second + 250, LogLevel::Info, QStringLiteral("Timeout on \"Disk\" %1").arg(i),
                   json ? "{\"time\":" + num(second / 1000) + ".250,\"level\":30,\"Thread\":\"worker-2\","
                          "\"msg\":\"Timeout on \\\"Disk\\\" " + n + "\"}"
                        : "time=" + num(second / 1000) + ".250 level=30 Thread=worker-2 msg=\"Timeout on "
                              "\\\"Disk\\\" " + n + "\"");
            break;
        case 2:   // эпоха в мс, pino 50, кириллица \u-последовательностями
            record(second, LogLevel::Error, QStringLiteral("Кэш miss %1").arg(i),
                   json ? "{\"level\":50,\"ts\":" + num(second) + ",\"msg\":\"\\u041a\\u044d\\u0448 miss "
                              + n + "\",\"Thread\":\"worker-3\"}"
                        : "level=50 ts=" + num(second) + " msg=\"Кэш miss " + n + "\" Thread=worker-3");
            break;
        case 3:   // эпоха в нс строкой, уровень по пути (logfmt — дословный ключ), «\\»
            record(second + 7, LogLevel::Warn, QStringLiteral("path C:\\temp\\x %1").arg(i),
                   json ? "{\"@timestamp\":\"" + num((second + 7) * 1000000LL + 999) + "\","
                          "\"log\":{\"level\":\"warn\",\"origin\":{\"file\":\"a.go\"}},"
                          "\"message\":\"path C:\\\\temp\\\\x " + n + "\",\"Thread\":\"worker-1\"}"
                        : "@timestamp=" + num((second + 7) * 1000000LL + 999) + " log.level=warn "
                          "message=\"path C:\\\\temp\\\\x " + n + "\" Thread=worker-1");
            break;
        case 4:   // эпоха в мкс, многострочная запись
            record(second + 456, LogLevel::Error, QStringLiteral("multi\nline %1").arg(i),
                   json ? "{\"ts\":" + num((second + 456) * 1000LL + 789) + ",\"severity\":\"ERROR\","
                          "\"msg\":\"multi\\nline " + n + "\"}"
                        : "ts=" + num((second + 456) * 1000LL + 789) + " severity=ERROR msg=\"multi\\nline "
                              + n + "\"");
            line("java.lang.IllegalStateException: boom " + n);
            line("\tat com.example.Foo.run(Foo.java:" + n + ")");
            line(json ? QByteArray("{\"foo\":1,\"bar\":\"no role keys\"}")
                      : QByteArray("foo=1 bar=\"no role keys\""));
            line(QByteArray());
            break;
        case 5:   // строка без ключей — эвристика (ведущая метка)
            record(second, LogLevel::Warn, QString(),
                   iso(i, 0) + " WARN plain Retrying " + n);
            break;
        case 6:   // ключи есть, значения не читаются — эвристика по всей строке
            record(second + 600, LogLevel::Error, QStringLiteral("retry after error at %1")
                                                     .arg(QString::fromLatin1(iso(i, 600))),
                   json ? "{\"ts\":\"bogus\",\"level\":8,\"msg\":\"retry after error at "
                              + iso(i, 600) + "\",\"Thread\":\"worker-2\"}"
                        : "ts=bogus level=8 msg=\"retry after error at " + iso(i, 600)
                              + "\" Thread=worker-2");
            break;
        case 7:   // зона +02:00, syslog 3, вложенное; за ним pretty JSON / мусор
            record(second + 900, LogLevel::Error, QStringLiteral("zoned %1").arg(i),
                   json ? "{\"t\":\"" + iso(i, 900, 2) + "\",\"lvl\":3,\"msg\":\"zoned " + n
                              + "\",\"Thread\":\"worker-3\",\"ctx\":{\"a\":[1,2,{\"b\":\"}\"}]}}"
                        : "t=" + iso(i, 900, 2) + " lvl=3 msg=\"zoned " + n + "\" Thread=worker-3");
            if (json) {
                line("{");
                line("  \"ts\": \"" + iso(0, 0) + "\",");
                line("  \"level\": \"fatal\"");
                line("}");
            } else {
                line("  caused by: x=1 level=fatal");
                line("Caused by: x=1 level=fatal");
            }
            break;
        }
    }
    return fx;
}

} // namespace keyedfx

#endif // KEYEDFIXTURES_H
