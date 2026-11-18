#ifndef LINECLASSIFIER_H
#define LINECLASSIFIER_H

#include "logentry.h"
#include <QDateTime>
#include <QString>
#include <QStringList>

// ============================================================================
// LineClassifier — распознавание таймстампа и уровня в строке лога плюс
// правило «первичной» строки. ЕДИНЫЙ источник истины для LogParser
// (резидентная загрузка) и LogIndexer (индексная): вынесен из LogParser,
// чтобы два пути загрузки никогда не разошлись в классификации.
//
// Оба детектора зовутся на каждую строку лога, поэтому реализованы ручными
// сканерами (см. lineclassifier.cpp) — эквивалент канонических паттернов
// PatternHeuristics::isoTimestampDetectPattern()/levelDetectPattern(),
// которые продолжает использовать редактор схем; эквивалентность закреплена
// тестом в tests/lineindex_smoke.
//
// Потокобезопасность: методы const и не мутируют состояние; экземпляр можно
// разделять между последовательными задачами одного владельца (как это делал
// LogParser), но проще держать по экземпляру на воркера.
// ============================================================================

// Опорная дата для меток без года (syslog RFC 3164: «Nov 18 13:55:36»).
// Метка получает последний год, в котором её день не позже СЛЕДУЮЩЕГО
// дня после опорной даты (сутки запаса — на чужой пояс и часы писателя);
// 29 февраля отступает к ближайшему високосному году. Опорная дата —
// локальная дата mtime файла: её строит yearReferenceForFile, а читатели
// файла (LogParser, LogIndexer, сканер каталога) только передают её.
// Пустая (по умолчанию) — сегодняшняя дата.
struct TimestampYearReference {
    int year = 0, month = 0, day = 0;   // опорная дата + 1 день
    bool isValid() const { return year != 0; }
};

class LineClassifier {
public:
    LineClassifier();

    // Опорная дата для меток без года — см. TimestampYearReference выше.
    using YearReference = TimestampYearReference;
    static YearReference yearReference(const QDate& referenceDate);
    // По mtime файла; файла нет — сегодняшняя дата.
    static YearReference yearReferenceForFile(const QString& filePath);

    // Распознать таймстамп в строке. true — ts заполнен. Метка без зоны —
    // локальное время машины; с явной зоной (Z, ±hh:mm, UTC…) — точный
    // момент. Дробная часть усекается до миллисекунд. Формы и где в строке
    // они ищутся — в начале lineclassifier.cpp.
    // leading (если задан) — метка ВЕДУЩАЯ, см. isPrimaryLine.
    bool detectTimestamp(const QString& line, QDateTime& ts,
                         const YearReference& year = YearReference(),
                         bool* leading = nullptr) const;
    // Как detectTimestamp, но сразу мс epoch БЕЗ построения QDateTime —
    // горячий путь индексатора. Конверсия локаль→epoch кэшируется по минуте:
    // на Windows каждая конверсия стоит микросекунды, и именно она (а не
    // поиск формы) доминировала в стоимости индексации. Метка с зоной
    // считается арифметикой и в кэш не ходит.
    bool detectTimestampMs(const QString& line, qint64& msecs,
                           const YearReference& year = YearReference(),
                           bool* leading = nullptr) const;
    // Распознать уровень логирования (словом). true — level заполнен.
    bool detectLogLevel(const QString& line, LogLevel& level) const;

    // Правило первичной строки логической записи (F-12, docs/design/F-12.md).
    // Строка первична, если её распознала схема, ИЛИ у неё есть метка и
    // слово уровня, ИЛИ метка ВЕДУЩАЯ — открывает строку:
    //   • стоит с позиции 0 (любая форма; syslog и фолбэк-формы только так);
    //   • стоит с позиции 1 сразу за «[» («[2026-11-18 10:00:00] …»);
    //   • это форма access-лога сразу за «[» в строке без ведущего пробела
    //     («10.0.0.1 - frank [18/Nov/2026:13:55:36 +0000] "GET …"»).
    // Остальные строки — continuation текущей записи: стек-трейсы и тело
    // сообщения (JSON, SQL, XML) с меткой в середине или с отступом.
    // Правило построчное, без состояния: одно у LogParser, LogIndexer и
    // сканера каталога (analyzeFileForStats).
    static bool isPrimaryLine(bool schemaMatched, bool hasTimestamp,
                              bool leadingTimestamp, bool hasLevel)
    {
        return schemaMatched || (hasTimestamp && (leadingTimestamp || hasLevel));
    }

private:
    QStringList m_timeFormats;
};

#endif // LINECLASSIFIER_H
