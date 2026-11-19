#ifndef LOGFILE_H
#define LOGFILE_H

#include <QString>
#include <QDateTime>
#include <memory>
#include <QMetaType>
#include <QFileInfo>

class SpoolItem;

// ============================================================================
// LogSource — что открыл ПОЛЬЗОВАТЕЛЬ (F-2). У обычного лога источник и
// данные совпадают; у сжатого файла источник — архив на диске (`app.log.gz`,
// `bundle.zip` + имя внутри), а данные — распакованная временная копия; у
// потока (stdin, вывод команды) — подпись, файла-источника нет.
// ============================================================================
struct LogSource {
    enum class Kind { File, Archive, Stream };
    Kind    kind = Kind::File;
    QString path;    // лог или архив на диске; пусто у потока
    QString member;  // имя внутри zip/tar; пусто у .gz
    QString label;   // подпись потока: «stdin», командная строка (F-7)
};

// ============================================================================
// LogFile — файл документа.
//
// ДВА ПУТИ (F-2), и путать их нельзя:
//   • filePath  — откуда ЧИТАЮТ: парсер, индексатор, кэш текста, воркеры,
//                 FileChangeDetector. У сжатого источника это временная копия
//                 в каталоге спула (SpoolDir), у stdin — файл спула;
//   • source()  — что открыл пользователь. Его путь сохраняется на диск
//                 (недавние, память файла, сессии, закладки).
// Правило: пользователю показывать displayPath(), на диск сохранять source(),
// читать по filePath. Временный путь в интерфейсе — ошибка.
//
// Порядок строк с равным временем — по orderKey() (LogEntry::operator<,
// IndexedLogStore::lessRef, ключ строки резидентного хранилища): у обычного
// файла он равен filePath, так что слитая вкладка из обычных файлов ведёт
// себя как до F-2; у сжатого — путь архива и имя внутри, а не случайное имя
// временной копии, поэтому порядок не зависит от того, куда распаковали.
// Внутри вкладки orderKey обязан быть единственным у каждого LogFile: при
// равных ключах бэкенды разрешают ничью по-разному (резидентный — по id
// записи, индексный — по номеру файла). Отсюда отсев дубликатов вкладки — по
// источнику, а не по временному filePath: тот же архив дважды во вкладку не
// попадает.
// ============================================================================
struct LogFile {
    QString filePath;        // Путь, по которому ЧИТАЮТ (см. выше)
    QString displayName;     // Имя для отображения (обычно имя файла)
    QDateTime lastModified;  // Время последней модификации
    qint64 fileSize = 0;     // Размер файла
    int totalEntries = 0;     // Total number of entries in the file
    QDateTime firstEntryTimestamp;
    QDateTime lastEntryTimestamp;
    int warnCount = 0;         // Number of warning entries in the file
    int errorCount = 0;        // Number of error entries in the file
    int fatalCount = 0;        // Number of fatal entries in the file
    bool statsAvailable = false; // Flag to indicate if stats have been computed

    // Временная копия, которой владеет этот файл (распакованный архив); пусто
    // у обычного лога. Копия удаляется, когда исчезает последний LogFile,
    // который на неё ссылается (Search Results делит записи с вкладкой).
    std::shared_ptr<SpoolItem> spool;

    // Конструктор для удобства создания: источник — сам файл.
    LogFile(const QString& path);

    // Возвращает короткое имя файла для отображения
    QString shortName() const { return displayName; }

    // Что открыл пользователь. По умолчанию — {File, filePath}. Задаётся до
    // того, как файл отдан парсеру или хранилищу: source() и orderKey()
    // читаются без синхронизации, в том числе снапшотами в воркерах.
    const LogSource& source() const { return m_source; }
    void setSource(const LogSource& source);

    // Что показывать пользователю: `C:\logs\app.log.gz`,
    // `bundle.zip › logs/app.log`, подпись потока; у обычного файла — filePath.
    QString displayPath() const;

    // Ключ порядка при равном времени (см. блок выше). Без выделений памяти
    // на сравнение: у обычного файла и потока — ссылка на filePath.
    const QString& orderKey() const
    {
        return m_source.kind == LogSource::Kind::Archive ? m_archiveOrderKey : filePath;
    }

private:
    LogSource m_source;
    QString m_archiveOrderKey; // path [+ '\x01' + member], считается в setSource
};

// Используем shared_ptr для автоматического управления памятью
using LogFilePtr = std::shared_ptr<LogFile>;

// Регистрируем тип для использования в Qt
Q_DECLARE_METATYPE(LogFilePtr)

#endif // LOGFILE_H
