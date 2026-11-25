#ifndef SPOOLDIR_H
#define SPOOLDIR_H

#include <QString>
#include <atomic>
#include <memory>

class QLockFile;

// ============================================================================
// SpoolDir / SpoolItem — временные файлы распаковки (F-2; потом и спул F-7).
//
// Раскладка корня (корень — параметр: ядро не знает про AppSettings; в
// программе — defaultRoot(), в тестах — QTemporaryDir):
//
//     <root>/<pid>-<случайное>/        каталог экземпляра программы
//     <root>/<pid>-<случайное>.lock    его QLockFile
//
// Блокировка лежит РЯДОМ с каталогом, а не в нём: экземпляр сначала берёт
// блокировку и только потом создаёт каталог, поэтому уборщик чужого процесса
// никогда не видит «каталог без живой блокировки» у живого владельца. Будь
// блокировка внутри, между mkdir и lock каталог выглядел бы осиротевшим.
//
// Время жизни:
//   • SpoolDir живёт, пока на него ссылается хоть один SpoolItem (у каждого
//     элемента shared_ptr на каталог) или владелец; последний удаляет каталог
//     экземпляра целиком и отпускает блокировку — штатное завершение;
//   • SpoolItem — один временный файл или каталог внутри; удаляет его, когда
//     исчезает последняя ссылка (LogFile::spool). Удаление — в глобальном пуле,
//     не на вызывающем потоке: распакованная копия бывает на десятки ГБ;
//   • после падения каталог остаётся. removeOrphans() (при старте, в фоне)
//     удаляет каталоги, чью блокировку удалось взять: владелец мёртв. Живой
//     чужой каталог не трогается — его блокировку держит процесс.
//
// Windows: файл, открытый читателем, не удаляется — он останется до уборки
// при следующем старте (каталог экземпляра тогда тоже не удалится, а
// блокировка уже отпущена — removeOrphans его заберёт).
// ============================================================================
class SpoolDir : public std::enable_shared_from_this<SpoolDir> {
public:
    // Каталог экземпляра в root (root создаётся при необходимости). nullptr —
    // не удалось создать каталог или взять блокировку; errorString — почему.
    static std::shared_ptr<SpoolDir> create(const QString& root, QString* errorString = nullptr);
    // Общий на процесс каталог экземпляра для root: вкладки и сканер делят
    // один каталог и одну блокировку, пока кто-то его держит (слабая ссылка);
    // последний владелец его удаляет. Только GUI-поток.
    static std::shared_ptr<SpoolDir> shared(const QString& root, QString* errorString = nullptr);

    // <CacheLocation>/spool. Не QDir::temp(): /tmp бывает в оперативной памяти.
    static QString defaultRoot();

    // Удалить осиротевшие каталоги экземпляров в root (блокировка взялась —
    // владелец мёртв; блокировки нет вовсе — тоже сирота). Живые не трогает.
    // Синхронно, ходит по диску: звать не на GUI-потоке. Возвращает число
    // удалённых каталогов.
    static int removeOrphans(const QString& root);

    ~SpoolDir();
    SpoolDir(const SpoolDir&) = delete;
    SpoolDir& operator=(const SpoolDir&) = delete;

    QString root() const { return m_root; }
    QString path() const { return m_path; }

    // Зарезервировать уникальное имя для файла (каталога) внутри экземпляра.
    // Сам файл не создаётся: его пишет распаковщик. nameHint — имя для
    // узнаваемости (`app.log`), недопустимые символы заменяются.
    std::shared_ptr<class SpoolItem> createFile(const QString& nameHint);
    std::shared_ptr<class SpoolItem> createDirectory(const QString& nameHint);

private:
    SpoolDir() = default;
    std::shared_ptr<SpoolItem> createItem(const QString& nameHint, bool directory);

    QString m_root;
    QString m_path;
    std::unique_ptr<QLockFile> m_lock;
    std::atomic<int> m_counter{0};
};

class SpoolItem {
public:
    ~SpoolItem();
    SpoolItem(const SpoolItem&) = delete;
    SpoolItem& operator=(const SpoolItem&) = delete;

    QString path() const { return m_path; }
    bool isDirectory() const { return m_directory; }

private:
    friend class SpoolDir;
    SpoolItem(std::shared_ptr<SpoolDir> dir, QString path, bool directory)
        : m_dir(std::move(dir)), m_path(std::move(path)), m_directory(directory) {}

    std::shared_ptr<SpoolDir> m_dir;
    QString m_path;
    bool m_directory = false;
};

#endif // SPOOLDIR_H
