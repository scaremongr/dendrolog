#ifndef RECORDID_H
#define RECORDID_H

#include "logfile.h"

#include <QtGlobal>
#include <limits>

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

#endif // RECORDID_H
