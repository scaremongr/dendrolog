#ifndef TIMEDELTA_H
#define TIMEDELTA_H

#include "logentry.h"
#include <QString>

class LogModel;

// ============================================================================
// Дельта времени (F-8, docs/design/F-8.md) без виджетов: колонка Δ в
// LogListView и строки Entry Details считают одним кодом.
//
//   • «Предыдущая» строки — ближайшая ПОКАЗАННАЯ выше строка с меткой из
//     ДРУГОЙ записи (продолжения наследуют метку своей записи). Поиск — по
//     метаданным (LogModel::visibleMetaAt, текст не читается), назад не
//     дальше kMaxBackScan строк.
//   • «Нулевая» — LogModel::timeZero(): дельта от её момента.
//   • В колонке дельта стоит только у первой показанной строки записи
//     (firstOfRecord); у «нулевой» — всегда (isZero).
//
// Моменты — мс эпохи (годы 0001–9999): разность в qint64 не переполняется.
// ============================================================================
namespace TimeDelta {

// Предел прохода назад в поисках предыдущей строки с меткой: дальше —
// «предыдущей нет». Цена худшего случая (длинная серия строк без меток) —
// единицы мс на индексной вкладке.
constexpr int kMaxBackScan = 200000;

// «+0 ms», «+123 ms», «+12.345 s», «+12 m 03 s», «+5 h 07 m», «+12 d 05 h»,
// «+1234 d»; отрицательная — с «−» (U+2212). Усечение к нулю. Не длиннее
// kMaxChars символов.
QString format(qint64 deltaMs);
constexpr int kMaxChars = 11;
// Для Entry Details: format и, от минуты, точное значение в мс.
QString formatDetailed(qint64 deltaMs);

// Предыдущая строка row (см. выше) или -1. limit — предел прохода назад;
// *exhausted (если задан) — поиск упёрся в предел, а не в начало.
int previousTimedRow(const LogModel& model, int row, int limit = kMaxBackScan,
                     bool* exhausted = nullptr);

struct RowDelta {
    bool valid = false;          // строка есть
    bool hasTimestamp = false;   // у строки есть метка (ms)
    qint64 ms = kNoTimestampMs;
    bool firstOfRecord = false;  // первая показанная строка своей записи
    bool isZero = false;         // строка — «нулевая»
    bool hasPrevious = false;
    int previousRow = -1;
    qint64 fromPrevious = 0;     // ms - момент предыдущей
    bool hasZero = false;        // «нулевая» задана и у строки есть метка
    qint64 fromZero = 0;         // ms - момент «нулевой»
};
// Нужна ли колонке предыдущая строка row: у строки метка, это первая
// показанная строка записи и (без ignoreZero) «нулевой» нет. O(1).
bool needsPrevious(const LogModel& model, int row, bool ignoreZero = false);
// previousRow — уже найденная предыдущая строка (-1 — нет), -2 — найти.
RowDelta forRow(const LogModel& model, int row, int previousRow = -2);

// Текст колонки Δ: «нулевая» задана — от неё («= 0» у самой «нулевой»),
// иначе от предыдущей; пусто — у строки продолжения, без метки, без опоры.
QString columnText(const RowDelta& delta);

} // namespace TimeDelta

#endif // TIMEDELTA_H
