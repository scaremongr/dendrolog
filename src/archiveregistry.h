#ifndef ARCHIVEREGISTRY_H
#define ARCHIVEREGISTRY_H

#include <QHash>
#include <QString>

#include <memory>
#include <optional>

class SpoolItem;

// ============================================================================
// ArchiveRegistry — реестр «временный каталог → архив» (F-2, шаг 4).
//
// zip или tar (F-2, шаг 5), открытый в Directory Scanner, извлекается во временный каталог
// (SourceUnpacker::unpackArchive), и сканер показывает этот каталог как обычную
// папку. Файл оттуда, открытый во вкладке (двойной щелчок, перетаскивание,
// любой путь addLogFile), по реестру становится ЧЛЕНОМ архива:
// LogSource{Archive, путь .zip, имя в архиве} — в недавние, память файла,
// закладки и сессии идёт архив и имя члена, а не временный путь, который
// после выхода исчезнет. Вкладка извлекает член сама (unpackArchiveMember) —
// тем же путём, каким восстанавливает его из сессии.
//
// Запись держит каталог (SpoolItem): пока архив показан в сканере, его файлы
// на месте; снятие записи (возврат к прежнему корню, другой скан) отпускает
// каталог — он удаляется в фоне, когда отпущены все ссылки.
//
// Один на процесс (вкладки разных окон узнают каталоги любого сканера),
// только GUI-поток.
// ============================================================================
class ArchiveRegistry {
public:
    struct Member {
        QString archivePath; // архив на диске
        QString member;      // имя в архиве (как записано)
    };

    static ArchiveRegistry& instance();

    // Каталог directory — извлечение archivePath; members — путь относительно
    // каталога ('/') → имя в архиве. Повторная регистрация каталога заменяет.
    void add(const QString& directory, const QString& archivePath,
             const QHash<QString, QString>& members, std::shared_ptr<SpoolItem> item);
    void remove(const QString& directory);

    // Файл path — из извлечённого архива? Только зарегистрированные члены:
    // посторонний файл в каталоге (его там быть не должно) — nullopt.
    std::optional<Member> locate(const QString& path) const;
    // Каталог извлечения или файл в нём (в недавние не идёт).
    bool isInsideExtraction(const QString& path) const;
    // Архив, извлечённый в directory (пусто — каталог не зарегистрирован).
    QString archiveFor(const QString& directory) const;

private:
    struct Entry {
        QString archivePath;
        QHash<QString, QString> members;
        std::shared_ptr<SpoolItem> item;
    };
    // Каталог (очищенный абсолютный путь) и относительный путь в нём.
    const Entry* entryFor(const QString& path, QString* relative) const;

    QHash<QString, Entry> m_entries;
};

#endif // ARCHIVEREGISTRY_H
