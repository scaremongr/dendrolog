#ifndef LOGFILE_H
#define LOGFILE_H

#include <QString>
#include <QDateTime>
#include <memory>
#include <QMetaType>
#include <QFileInfo>

struct LogFile {
    QString filePath;        // Полный путь к файлу
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

    // Конструктор для удобства создания
    LogFile(const QString& path);
    
    // Возвращает короткое имя файла для отображения
    QString shortName() const { return displayName; }
};

// Используем shared_ptr для автоматического управления памятью
using LogFilePtr = std::shared_ptr<LogFile>;

// Регистрируем тип для использования в Qt
Q_DECLARE_METATYPE(LogFilePtr)

#endif // LOGFILE_H 