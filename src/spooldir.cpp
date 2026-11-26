#include "spooldir.h"

#include <QHash>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QtConcurrent>

namespace {

const QString kLockSuffix = QStringLiteral(".lock");

// Имя каталога экземпляра: <pid>-<16 hex>. Уборщик трогает только такие имена:
// корень, заданный по ошибке на чужой каталог, не потеряет чужих файлов.
bool isInstanceName(const QString& name)
{
    static const QRegularExpression re(QStringLiteral("^[0-9]+-[0-9a-f]{16}$"));
    return re.match(name).hasMatch();
}

QString sanitizedName(const QString& hint)
{
    QString out;
    out.reserve(qMin<qsizetype>(hint.size(), 96));
    for (const QChar c : hint) {
        if (out.size() >= 96)
            break;
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
            || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
            || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('_');
        out.append(ok ? c : QLatin1Char('_'));
    }
    // Ни «.», ни «..», ни имя из одних точек — это не имена файлов.
    while (out.startsWith(QLatin1Char('.')))
        out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("item") : out;
}

void removePath(const QString& path, bool directory)
{
    if (directory)
        QDir(path).removeRecursively();
    else
        QFile::remove(path);
}

// QLockFile без устаревания по возрасту: по умолчанию блокировка старше 30 с
// считается брошенной, даже если владелец жив, — для каталога, который живёт
// часами, это неверно. Мёртвого владельца QLockFile распознаёт по PID.
std::unique_ptr<QLockFile> makeLock(const QString& path)
{
    auto lock = std::make_unique<QLockFile>(path);
    lock->setStaleLockTime(0);
    return lock;
}

} // namespace

std::shared_ptr<SpoolDir> SpoolDir::create(const QString& root, QString* errorString)
{
    const QString cleanRoot = QDir::cleanPath(root);
    if (cleanRoot.isEmpty() || !QDir().mkpath(cleanRoot)) {
        if (errorString)
            *errorString = QStringLiteral("Cannot create directory %1").arg(QDir::toNativeSeparators(cleanRoot));
        return nullptr;
    }

    const QDir rootDir(cleanRoot);
    const qint64 pid = QCoreApplication::applicationPid();
    for (int attempt = 0; attempt < 8; ++attempt) {
        const quint64 rnd = QRandomGenerator::global()->generate64();
        const QString name = QStringLiteral("%1-%2").arg(pid).arg(rnd, 16, 16, QLatin1Char('0'));
        const QString dirPath = rootDir.filePath(name);

        // Сначала блокировка, потом каталог (см. заголовок).
        std::unique_ptr<QLockFile> lock = makeLock(dirPath + kLockSuffix);
        if (!lock->tryLock(0))
            continue;
        if (QFileInfo::exists(dirPath)) {
            // Совпадение имени с осиротевшим каталогом — взять другое.
            continue;
        }
        if (!rootDir.mkdir(name)) {
            if (errorString)
                *errorString = QStringLiteral("Cannot create directory %1").arg(QDir::toNativeSeparators(dirPath));
            return nullptr;
        }

        std::shared_ptr<SpoolDir> dir(new SpoolDir());
        dir->m_root = cleanRoot;
        dir->m_path = dirPath;
        dir->m_lock = std::move(lock);
        return dir;
    }
    if (errorString)
        *errorString = QStringLiteral("Cannot lock a spool directory in %1").arg(QDir::toNativeSeparators(cleanRoot));
    return nullptr;
}

QString SpoolDir::defaultRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/spool");
}

int SpoolDir::removeOrphans(const QString& root)
{
    const QDir rootDir(QDir::cleanPath(root));
    if (!rootDir.exists())
        return 0;

    int removed = 0;
    const QFileInfoList dirs = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& info : dirs) {
        if (!isInstanceName(info.fileName()))
            continue;
        // Блокировку держит живой процесс — tryLock не возьмёт её (в том числе
        // нашу собственную: владелец — этот же PID, и он жив). Взялась —
        // владелец мёртв или блокировки не было: каталог ничей.
        std::unique_ptr<QLockFile> lock = makeLock(info.absoluteFilePath() + kLockSuffix);
        if (!lock->tryLock(0))
            continue;
        if (QDir(info.absoluteFilePath()).removeRecursively())
            ++removed;
        lock->unlock();
    }

    // Блокировки без каталога (упали между lock и mkdir или каталог уже убран,
    // а файл блокировки остался) — убрать, если владелец мёртв.
    const QFileInfoList files = rootDir.entryInfoList({QStringLiteral("*") + kLockSuffix}, QDir::Files | QDir::Hidden);
    for (const QFileInfo& info : files) {
        const QString base = info.fileName().chopped(kLockSuffix.size());
        if (!isInstanceName(base) || rootDir.exists(base))
            continue;
        std::unique_ptr<QLockFile> lock = makeLock(info.absoluteFilePath());
        if (lock->tryLock(0))
            lock->unlock(); // unlock удаляет файл блокировки
    }
    return removed;
}

SpoolDir::~SpoolDir()
{
    // Штатное завершение: последний владелец (каталог или его последний
    // элемент) удаляет каталог экземпляра. Не удалилось (Windows: файл
    // открыт) — после unlock каталог станет сиротой и уйдёт при следующем
    // старте.
    if (!m_path.isEmpty())
        QDir(m_path).removeRecursively();
    if (m_lock)
        m_lock->unlock();
}

std::shared_ptr<SpoolItem> SpoolDir::createFile(const QString& nameHint)
{
    return createItem(nameHint, false);
}

std::shared_ptr<SpoolDir> SpoolDir::shared(const QString& root, QString* errorString)
{
    static QHash<QString, std::weak_ptr<SpoolDir>> dirs;
    if (auto dir = dirs.value(root).lock())
        return dir;
    auto dir = create(root, errorString);
    if (dir)
        dirs.insert(root, dir);
    return dir;
}

std::shared_ptr<SpoolItem> SpoolDir::createDirectory(const QString& nameHint)
{
    return createItem(nameHint, true);
}

std::shared_ptr<SpoolItem> SpoolDir::createStreamFile(const QString& nameHint)
{
    QString hint = sanitizedName(nameHint);
    if (!hint.endsWith(QLatin1String(".log"), Qt::CaseInsensitive))
        hint += QStringLiteral(".log");
    return createItem(QStringLiteral("stream-") + hint, false, true);
}

bool SpoolDir::isStreamFile(const QString& path)
{
    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString name = info.fileName();
    static const QRegularExpression streamName(QStringLiteral("^[0-9]+-stream-[^/]+$"));
    if (streamName.match(name).hasMatch()) {
        const QString parent = QFileInfo(info.path()).fileName();
        if (isInstanceName(parent))
            return true;
    }
    // Спул stdin до F-7: <temp>/DendroLog-stdin-<pid>.log.
    static const QRegularExpression legacyName(QStringLiteral("^DendroLog-stdin-[0-9]+\\.log$"));
    if (!legacyName.match(name).hasMatch())
        return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(info.absolutePath())
               .compare(QDir::cleanPath(QDir::temp().absolutePath()), cs) == 0;
}

std::shared_ptr<SpoolItem> SpoolDir::createItem(const QString& nameHint, bool directory, bool stream)
{
    const int n = m_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    QString name = sanitizedName(nameHint);
    // `<n>-stream-…` — только у файлов потока (isStreamFile): копия архива
    // `stream-api.log.gz` иначе сошла бы за спул stdin.
    if (!stream && name.startsWith(QLatin1String("stream-"), Qt::CaseInsensitive))
        name.prepend(QStringLiteral("f-"));
    const QString path = QDir(m_path).filePath(QStringLiteral("%1-%2").arg(n).arg(name));
    if (directory && !QDir().mkpath(path))
        return nullptr;
    return std::shared_ptr<SpoolItem>(new SpoolItem(shared_from_this(), path, directory));
}

SpoolItem::~SpoolItem()
{
    // Не на вызывающем потоке: последний LogFile обычно умирает на GUI-потоке,
    // а копия бывает на десятки гигабайт. Задача держит каталог экземпляра
    // (m_dir), чтобы тот не удалился раньше файла — и не наоборот.
    // При завершении приложения пул уже мог отработать waitForDone и задачу
    // не выполнить — тогда синхронно.
    QThreadPool* pool = QCoreApplication::instance() && !QCoreApplication::closingDown()
        ? QThreadPool::globalInstance()
        : nullptr;
    if (!pool) {
        removePath(m_path, m_directory);
        return;
    }
    // QtConcurrent::run, а не QThreadPool::start(лямбда): передачу видит слой
    // TSan (tests/sanitizers/tsan_qt_sync.cpp).
    (void)QtConcurrent::run(pool, [path = m_path, directory = m_directory, dir = std::move(m_dir)]() mutable {
        removePath(path, directory);
        dir.reset(); // последний элемент удаляет и каталог экземпляра — тоже здесь
    });
}
