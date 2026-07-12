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
class LineClassifier {
public:
    LineClassifier();

    // Распознать таймстамп в начале строки. true — ts заполнен.
    bool detectTimestamp(const QString& line, QDateTime& ts) const;
    // Как detectTimestamp, но сразу мс epoch (локальная зона) БЕЗ построения
    // QDateTime — горячий путь индексатора. Конверсия локаль→epoch кэшируется
    // по минуте: на Windows каждая конверсия стоит микросекунды, и именно она
    // (а не поиск формы) доминировала в стоимости индексации.
    bool detectTimestampMs(const QString& line, qint64& msecs) const;
    // Распознать уровень логирования (словом). true — level заполнен.
    bool detectLogLevel(const QString& line, LogLevel& level) const;

    // Правило первичной строки логической записи. Строка первична, если её
    // распознала схема ИЛИ у неё есть и таймстамп, и уровень; остальные
    // строки — continuation текущей записи.
    static bool isPrimaryLine(bool schemaMatched, bool hasTimestamp, bool hasLevel)
    {
        return schemaMatched || (hasTimestamp && hasLevel);
    }

private:
    QStringList m_timeFormats;
};

#endif // LINECLASSIFIER_H
