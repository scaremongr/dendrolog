#include "sourceunpacker.h"

#include "tararchive.h"
#include "ziparchive.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QFileInfo>
#include <QStorageInfo>
#include <QtConcurrent>

namespace {

constexpr qint64 kChunkBytes = 1024 * 1024;
constexpr qint64 kProgressStepBytes = 4 * 1024 * 1024;

QString mib(qint64 bytes)
{
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}

} // namespace

SourceUnpacker::SourceUnpacker(QObject* parent)
    : QObject(parent)
    , m_freeSpace(&SourceUnpacker::systemFreeSpace)
{
    qRegisterMetaType<SourceUnpacker::Result>();
    m_pool.setMaxThreadCount(1);
    connect(this, &SourceUnpacker::workerFinished, this, &SourceUnpacker::onWorkerFinished,
            Qt::QueuedConnection);
}

SourceUnpacker::~SourceUnpacker()
{
    m_cancel.store(true);
    m_pool.waitForDone();
}

void SourceUnpacker::setFreeSpaceFunction(FreeSpaceFunction fn)
{
    m_freeSpace = fn ? std::move(fn) : FreeSpaceFunction(&SourceUnpacker::systemFreeSpace);
}

qint64 SourceUnpacker::systemFreeSpace(const QString& dir)
{
    const QStorageInfo info(dir);
    if (!info.isValid() || !info.isReady())
        return -1;
    return info.bytesAvailable();
}

void SourceUnpacker::cancel()
{
    m_cancel.store(true);
}

void SourceUnpacker::waitForFinished()
{
    m_pool.waitForDone();
}

void SourceUnpacker::onWorkerFinished(const Result& result)
{
    emit finished(result);
}

// Цель — сам источник (тот же путь или ссылка на него): открытие на запись
// обнулило бы источник, а уборка после ошибки — удалила.
static bool targetIsSource(const QString& sourcePath, const QString& targetPath)
{
    const QString srcCanonical = QFileInfo(sourcePath).canonicalFilePath();
    return !srcCanonical.isEmpty() && QFileInfo(targetPath).canonicalFilePath() == srcCanonical;
}

// Отбор членов набора (zip и tar): имя члена → путь в каталоге извлечения.
// Пропуски считаются в Result; правило одно на оба формата.
struct MemberPlanner {
    enum class Kind { File, Link, Unsupported };
    QSet<QString> used;   // занятые пути (без учёта регистра: Windows)
    QSet<QString> dirs;   // каталоги этих путей
    QSet<QString> names;  // имена членов: член ищется по имени (unpackArchiveMember)

    // Пусто — член пропущен (и посчитан в r).
    QString place(const QString& name, Kind kind, SourceUnpacker::Result& r)
    {
        // Два члена с одним именем (дописанный архив): вкладка, память и
        // закладки знают член по имени, и второй открылся бы первым —
        // извлекается только первый, второй считается пропущенным.
        if (names.contains(name)) {
            ++r.skippedUnsafe;
            return QString();
        }
        names.insert(name);
        if (kind == Kind::Link) {
            ++r.skippedUnsafe;
            return QString();
        }
        QString relative = ArchiveNames::safeRelativePath(name);
        if (relative.isEmpty()) {
            ++r.skippedUnsafe;
            return QString();
        }
        if (kind == Kind::Unsupported) {
            ++r.skippedEncrypted;
            return QString();
        }
        // Повтор того же пути (другой регистр, хвостовая точка, имя,
        // занятое каталогом) — суффиксом «~N» перед расширением.
        const auto taken = [&](const QString& p) {
            return used.contains(p.toLower()) || dirs.contains(p.toLower());
        };
        for (qsizetype at = relative.indexOf(QLatin1Char('/')); at >= 0;
             at = relative.indexOf(QLatin1Char('/'), at + 1)) {
            if (used.contains(relative.left(at).toLower())) {
                ++r.skippedUnsafe; // каталог совпал с именем уже извлекаемого файла
                return QString();
            }
        }
        if (taken(relative)) {
            const QFileInfo fi(relative);
            const QString dir = relative.contains(QLatin1Char('/'))
                ? relative.left(relative.lastIndexOf(QLatin1Char('/')) + 1) : QString();
            const QString base = fi.completeBaseName();
            const QString suffix = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
            for (int n = 2;; ++n) {
                const QString candidate = dir + base + QStringLiteral("~%1").arg(n) + suffix;
                if (!taken(candidate)) {
                    relative = candidate;
                    break;
                }
            }
        }
        used.insert(relative.toLower());
        for (qsizetype at = relative.indexOf(QLatin1Char('/')); at >= 0;
             at = relative.indexOf(QLatin1Char('/'), at + 1))
            dirs.insert(relative.left(at).toLower());
        return relative;
    }
};

// Задание в воркере: снимок настроек, итог и общие проверки (место, отмена,
// прогресс) для gzip и zip.
struct SourceUnpacker::Job {
    SourceUnpacker* self = nullptr;
    Result r;
    FreeSpaceFunction freeSpace;
    qint64 reserve = 0;
    qint64 interval = 0;
    QString spaceDir;        // где проверяется место
    qint64 sinceCheck = 0;
    qint64 lastProgress = 0;
    bool quietProgress = false; // вложенная стадия (член-gzip): шкала не идёт назад

    bool cancelled() const { return self->m_cancel.load(std::memory_order_relaxed); }
    void setCancelled()
    {
        r.error = Error::Cancelled;
        r.message = errorText(Error::Cancelled);
    }
    void sourceFailed(GzipDevice::Error e, const QString& text)
    {
        r.error = Error::SourceError;
        r.gzipError = e;
        r.message = text;
    }
    void zipFailed(const ZipArchive& zip)
    {
        r.error = zip.error() == ZipArchive::Error::Aborted      ? Error::Cancelled
                : zip.error() == ZipArchive::Error::WriteError ? Error::WriteFailed
                                                               : Error::SourceError;
        r.message = r.error == Error::Cancelled ? errorText(Error::Cancelled) : zip.errorString();
    }
    void tarFailed(const TarArchive& tar)
    {
        if (r.error != Error::None)
            return; // место или запись — уже названы (Stopped)
        r.error = tar.error() == TarArchive::Error::Aborted      ? Error::Cancelled
                : tar.error() == TarArchive::Error::WriteError ? Error::WriteFailed
                                                               : Error::SourceError;
        r.message = r.error == Error::Cancelled ? errorText(Error::Cancelled) : tar.errorString();
    }
    // Место ДО начала: estimate распакованного + резерв.
    bool checkSpaceBefore(qint64 estimate, const QString& verb)
    {
        const qint64 freeBefore = freeSpace(spaceDir);
        if (freeBefore >= 0 && freeBefore < estimate + reserve) {
            r.error = Error::NoSpace;
            r.requiredBytes = estimate + reserve;
            r.availableBytes = freeBefore;
            r.message = QStringLiteral("Not enough disk space to %1: at least %2 needed "
                                       "(including %3 reserve), %4 free")
                            .arg(verb, mib(r.requiredBytes), mib(reserve), mib(freeBefore));
            return false;
        }
        return true;
    }
    // После каждого записанного блока: место раз в interval.
    bool wrote(qint64 n, QFileDevice& out)
    {
        r.bytesWritten += n;
        sinceCheck += n;
        if (sinceCheck < interval)
            return true;
        sinceCheck = 0;
        if (!out.flush()) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot write %1: %2").arg(out.fileName(), out.errorString());
            return false;
        }
        const qint64 freeNow = freeSpace(spaceDir);
        if (freeNow >= 0 && freeNow < reserve) {
            r.error = Error::NoSpace;
            r.requiredBytes = reserve;
            r.availableBytes = freeNow;
            r.message = QStringLiteral("Unpacking stopped: free disk space fell below the "
                                       "%1 reserve after %2 written")
                            .arg(mib(reserve), mib(r.bytesWritten));
            return false;
        }
        return true;
    }
    void progress(qint64 done, qint64 total, bool force = false)
    {
        if (quietProgress)
            return;
        if (force || done - lastProgress >= kProgressStepBytes) {
            lastProgress = done;
            emit self->progress(done, total);
        }
    }

    // gzip sourcePath → targetPath (оба пути заданы); частичный файл при
    // ошибке не удаляет — это делает вызывающий.
    void gunzip(const QString& sourcePath, const QString& targetPath)
    {
        const QFileInfo src(sourcePath);
        const qint64 compressedTotal = src.size();
        const qint64 estimate = qMax(compressedTotal, GzipDevice::trailerSize(sourcePath));
        if (!checkSpaceBefore(estimate, QStringLiteral("unpack")))
            return;
        GzipDevice gz(sourcePath);
        gz.setAbortFlag(&self->m_cancel); // отмена — и посреди входа без вывода
        if (!gz.open(QIODevice::ReadOnly)) {
            sourceFailed(gz.error(), gz.errorString());
            return;
        }
        QFile out(targetPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create %1: %2").arg(targetPath, out.errorString());
            return;
        }
        QByteArray buffer(int(kChunkBytes), Qt::Uninitialized);
        lastProgress = 0;
        progress(0, compressedTotal, true);
        for (;;) {
            if (cancelled()) {
                setCancelled();
                break;
            }
            const qint64 n = gz.read(buffer.data(), buffer.size());
            if (n < 0) {
                if (gz.error() == GzipDevice::Error::Aborted)
                    setCancelled();
                else
                    sourceFailed(gz.error(), gz.errorString());
                break;
            }
            if (n == 0) {
                if (gz.error() != GzipDevice::Error::None)
                    sourceFailed(gz.error(), gz.errorString());
                break; // конец: все потоки проверены CRC и длиной
            }
            if (out.write(buffer.constData(), n) != n) {
                r.error = Error::WriteFailed;
                r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
                break;
            }
            if (!wrote(n, out))
                break;
            progress(gz.compressedBytesRead(), compressedTotal);
        }
        if (r.error == Error::None && !out.flush()) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
        }
        out.close();
        gz.close();
        if (r.error == Error::None)
            progress(compressedTotal, compressedTotal, true);
    }

    // Член zip → targetPath (открывается здесь). Частичный файл не удаляет.
    void extractMember(ZipArchive& zip, const ZipMember& member, const QString& targetPath,
                       qint64 doneBefore, qint64 total)
    {
        QFile out(targetPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create %1: %2").arg(targetPath, out.errorString());
            return;
        }
        qint64 previous = 0;
        const bool ok = zip.extract(member, out, &self->m_cancel, [&](qint64 written) {
            const qint64 n = written - previous;
            previous = written;
            if (!wrote(n, out))
                return false;
            progress(doneBefore + written, total);
            return true;
        });
        if (!ok && r.error == Error::None)
            zipFailed(zip);
        if (r.error == Error::None && !out.flush()) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
        }
        out.close();
    }

    // Текущий член tar → targetPath (открывается здесь). Прогресс — по
    // прочитанному из архива (у .tar.gz — сжатых байт). Частичный файл не
    // удаляет.
    void extractTarMember(TarArchive& tar, const QString& targetPath)
    {
        QFile out(targetPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create %1: %2").arg(targetPath, out.errorString());
            return;
        }
        qint64 previous = 0;
        const bool ok = tar.extract(out, [&](qint64 written) {
            const qint64 n = written - previous;
            previous = written;
            if (!wrote(n, out))
                return false;
            progress(tar.bytesRead(), tar.archiveSize());
            return true;
        });
        if (!ok)
            tarFailed(tar);
        if (r.error == Error::None && !out.flush()) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot write %1: %2").arg(targetPath, out.errorString());
        }
        out.close();
    }

    // Извлечённый член — сам gzip (`app.log.gz` в архиве): распаковать
    // дальше в тот же файл; промежуточный — рядом, удаляется в любом случае.
    // Итог задания: при ошибке файла нет, иначе прогресс дошёл до конца.
    void finishMemberFile(const QString& targetPath, qint64 progressTotal)
    {
        if (r.error == Error::None && GzipDevice::fileHasSignature(targetPath)) {
            const QString packed = targetPath + QStringLiteral(".gz-part");
            QFile::remove(packed);
            if (!QFile::rename(targetPath, packed)) {
                r.error = Error::WriteFailed;
                r.message = QStringLiteral("Cannot write %1").arg(targetPath);
            } else {
                // Шкала прогресса уже дошла до конца извлечения — вторая
                // стадия её не откатывает (запись и место считаются заново).
                r.bytesWritten = 0;
                sinceCheck = 0;
                quietProgress = true;
                gunzip(packed, targetPath);
                quietProgress = false;
                QFile::remove(packed);
            }
        }
        if (r.error != Error::None)
            QFile::remove(targetPath);
        else
            progress(progressTotal, progressTotal, true);
    }

    void notArchive(const QString& path)
    {
        r.error = Error::SourceError;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            r.message = QStringLiteral("Cannot read the archive: %1").arg(file.errorString());
            return;
        }
        r.message = QStringLiteral("%1 is not a zip or tar archive")
                        .arg(QDir::toNativeSeparators(path));
    }

    // zip целиком (отобранные члены) в каталог targetDir.
    void zipSet(const QString& zipPath, const QString& targetDir, const MemberFilter& accept)
    {
        spaceDir = targetDir;
        const auto cleanup = [this, &targetDir]() {
            if (r.error != Error::None) {
                QDir(targetDir).removeRecursively();
                r.extracted.clear();
            }
        };
        ZipArchive zip(zipPath);
        if (!zip.open()) {
            zipFailed(zip);
            cleanup();
            return;
        }
        // Отбор: каталоги — сами появятся; ссылки и опасные имена —
        // пропущены и посчитаны; пароль и неизвестный метод — тоже.
        struct Planned { const ZipMember* member; QString relative; };
        QVector<Planned> planned;
        MemberPlanner planner;
        qint64 total = 0;
        for (const ZipMember& m : zip.members()) {
            if (m.directory)
                continue;
            if (accept && !accept(m.name))
                continue;
            const QString relative = planner.place(m.name,
                m.symlink ? MemberPlanner::Kind::Link
                          : (m.encrypted || !m.supported) ? MemberPlanner::Kind::Unsupported
                                                          : MemberPlanner::Kind::File,
                r);
            if (relative.isEmpty())
                continue;
            planned.append({ &m, relative });
            total += m.size;
        }

        if (!QDir().mkpath(targetDir)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create %1").arg(targetDir);
            cleanup();
            return;
        }
        if (!checkSpaceBefore(total, QStringLiteral("extract the archive"))) {
            cleanup();
            return;
        }
        const QString root = QDir::cleanPath(QFileInfo(targetDir).absoluteFilePath());
        progress(0, total, true);
        qint64 done = 0;
        for (const Planned& p : std::as_const(planned)) {
            if (cancelled()) {
                setCancelled();
                break;
            }
            // Вторая линия: путь после очистки обязан остаться в каталоге.
            const QString target = QDir::cleanPath(root + QLatin1Char('/') + p.relative);
            if (!target.startsWith(root + QLatin1Char('/'))) {
                ++r.skippedUnsafe;
                continue;
            }
            if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
                r.error = Error::WriteFailed;
                r.message = QStringLiteral("Cannot create a folder for %1").arg(p.member->name);
                break;
            }
            extractMember(zip, *p.member, target, done, total);
            if (r.error != Error::None)
                break;
            done += p.member->size;
            r.extracted.append({ p.member->name, p.relative, p.member->size });
        }
        if (r.error == Error::None)
            progress(total, total, true);
        cleanup();
    }

    // Один член zip в файл targetPath.
    void zipMember(const QString& zipPath, const QString& member, const QString& targetPath)
    {
        spaceDir = QFileInfo(targetPath).absolutePath();
        ZipArchive zip(zipPath);
        if (!zip.open()) {
            zipFailed(zip);
            return;
        }
        const int index = zip.find(member);
        if (index < 0) {
            r.error = Error::SourceError;
            r.message = QStringLiteral("%1 is not in the archive").arg(member);
            return;
        }
        const ZipMember& m = zip.members().at(index);
        if (m.directory || m.symlink) {
            r.error = Error::SourceError;
            r.message = QStringLiteral("%1 is not a file").arg(member);
            return;
        }
        if (targetIsSource(zipPath, targetPath)) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot extract %1 onto itself").arg(zipPath);
            return;
        }
        if (!checkSpaceBefore(m.size, QStringLiteral("extract the file"))) {
            return;
        }
        progress(0, m.size, true);
        extractMember(zip, m, targetPath, 0, m.size);
        zip.close();
        finishMemberFile(targetPath, m.size);
    }

    void tarSet(const QString& tarPath, const QString& targetDir, const MemberFilter& accept);
    void tarMember(const QString& tarPath, const QString& member, const QString& targetPath);
};

// tar (шаг 5): набор в каталог. Оглавления у tar нет: у `.tar` отбор и
// размеры берутся предварительным проходом по заголовкам (данные
// перескакиваются seek), место до начала — сумма отобранных, как у zip;
// у `.tar.gz` проход стоил бы второй распаковки — оценка до начала —
// размер архива на диске, остальное ловит проверка по ходу.
void SourceUnpacker::Job::tarSet(const QString& tarPath, const QString& targetDir, const MemberFilter& accept)
{
    spaceDir = targetDir;
    const auto cleanup = [this, &targetDir]() {
        if (r.error != Error::None) {
            QDir(targetDir).removeRecursively();
            r.extracted.clear();
        }
    };
    const auto kindOf = [](const TarMember& m) {
        return m.isFile() ? MemberPlanner::Kind::File
             : m.type == TarMember::Type::Unsupported ? MemberPlanner::Kind::Unsupported
                                                      : MemberPlanner::Kind::Link;
    };
    const auto wanted = [&accept](const TarMember& m) {
        return m.type != TarMember::Type::Directory && (!accept || accept(m.name));
    };

    TarArchive tar(tarPath);
    if (!tar.open(&self->m_cancel)) {
        tarFailed(tar);
        cleanup();
        return;
    }
    qint64 estimate = tar.archiveSize();
    if (!tar.isCompressed()) {
        TarArchive scan(tarPath);
        MemberPlanner dry;
        Result dryResult;
        TarMember m;
        estimate = 0;
        if (scan.open(&self->m_cancel)) {
            while (scan.next(m)) {
                if (wanted(m) && !dry.place(m.name, kindOf(m), dryResult).isEmpty())
                    estimate += m.size;
            }
        }
        if (scan.error() != TarArchive::Error::None) {
            tarFailed(scan);
            cleanup();
            return;
        }
    }
    if (!QDir().mkpath(targetDir)) {
        r.error = Error::WriteFailed;
        r.message = QStringLiteral("Cannot create %1").arg(targetDir);
        cleanup();
        return;
    }
    if (!checkSpaceBefore(estimate, QStringLiteral("extract the archive"))) {
        cleanup();
        return;
    }
    const QString root = QDir::cleanPath(QFileInfo(targetDir).absoluteFilePath());
    MemberPlanner planner;
    progress(0, tar.archiveSize(), true);
    TarMember m;
    while (r.error == Error::None && tar.next(m)) {
        if (!wanted(m))
            continue;
        const QString relative = planner.place(m.name, kindOf(m), r);
        if (relative.isEmpty())
            continue;
        // Вторая линия: путь после очистки обязан остаться в каталоге.
        const QString target = QDir::cleanPath(root + QLatin1Char('/') + relative);
        if (!target.startsWith(root + QLatin1Char('/'))) {
            ++r.skippedUnsafe;
            continue;
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
            r.error = Error::WriteFailed;
            r.message = QStringLiteral("Cannot create a folder for %1").arg(m.name);
            break;
        }
        extractTarMember(tar, target);
        if (r.error != Error::None) {
            if (r.error == Error::SourceError)
                r.message = QStringLiteral("%1: %2").arg(m.name, r.message);
            break;
        }
        r.extracted.append({ m.name, relative, m.size });
    }
    if (r.error == Error::None && tar.error() != TarArchive::Error::None)
        tarFailed(tar);
    if (r.error == Error::None && !tar.finish([&] { progress(tar.bytesRead(), tar.archiveSize()); }))
        tarFailed(tar);
    if (r.error == Error::None)
        progress(tar.archiveSize(), tar.archiveSize(), true);
    cleanup();
}

// Один член tar в файл: проход от начала до первого члена с этим именем
// (как у набора: повтор имени — извлекается первый).
void SourceUnpacker::Job::tarMember(const QString& tarPath, const QString& member, const QString& targetPath)
{
    spaceDir = QFileInfo(targetPath).absolutePath();
    TarArchive tar(tarPath);
    if (!tar.open(&self->m_cancel)) {
        tarFailed(tar);
        return;
    }
    // Прогресс — по прочитанному из архива на всех стадиях: поиск члена
    // проходом, извлечение, дочитывание потока (у .tar.gz — сжатые байты).
    const qint64 total = tar.archiveSize();
    progress(0, total, true);
    TarMember m;
    bool found = false;
    while (tar.next(m)) {
        if (m.name == member) {
            found = true;
            break;
        }
        progress(tar.bytesRead(), total);
    }
    if (!found) {
        if (tar.error() != TarArchive::Error::None) {
            tarFailed(tar);
        } else {
            r.error = Error::SourceError;
            r.message = QStringLiteral("%1 is not in the archive").arg(member);
        }
        return;
    }
    if (!m.isFile()) {
        r.error = Error::SourceError;
        r.message = QStringLiteral("%1 is not a file").arg(member);
        return;
    }
    if (targetIsSource(tarPath, targetPath)) {
        r.error = Error::WriteFailed;
        r.message = QStringLiteral("Cannot extract %1 onto itself").arg(tarPath);
        return;
    }
    if (!checkSpaceBefore(m.size, QStringLiteral("extract the file")))
        return;
    extractTarMember(tar, targetPath);
    // У .tar.gz — дочитать поток до конца: CRC gzip проверен, иначе
    // повреждение после члена осталось бы незамеченным.
    if (r.error == Error::None && !tar.finish([&] { progress(tar.bytesRead(), total); }))
        tarFailed(tar);
    tar.close();
    finishMemberFile(targetPath, total);
}

quint64 SourceUnpacker::start(const std::function<void(Job&)>& body, const QString& sourcePath,
                              const QString& targetPath)
{
    if (m_busy.load())
        return 0;
    m_busy.store(true);
    m_cancel.store(false);
    const quint64 job = ++m_lastJob;

    // Снимок настроек: воркер не читает члены объекта, кроме флага отмены.
    const FreeSpaceFunction freeSpace = m_freeSpace;
    const qint64 reserve = qMax<qint64>(0, m_reserveBytes);
    const qint64 interval = qMax<qint64>(kChunkBytes, m_checkIntervalBytes);

    // QtConcurrent::run в свой пул, как у LogParser/LogIndexer: передачу
    // данных в воркер видит и слой TSan (tests/sanitizers/tsan_qt_sync.cpp),
    // голый QThreadPool::start(лямбда) — нет.
    (void)QtConcurrent::run(&m_pool, [this, job, body, sourcePath, targetPath, freeSpace, reserve, interval]() {
        Job j;
        j.self = this;
        j.r.job = job;
        j.r.sourcePath = sourcePath;
        j.r.targetPath = targetPath;
        j.freeSpace = freeSpace;
        j.reserve = reserve;
        j.interval = interval;
        body(j);
        // Занятость снимается ДО отправки итога: к приходу finished
        // isRunning() уже false, а следующее задание можно ставить, не
        // дожидаясь очереди событий (пул на один поток запустит его, когда
        // эта задача вернётся; итоги различаются номером job).
        m_busy.store(false);
        emit workerFinished(j.r, QPrivateSignal());
    });
    return job;
}


quint64 SourceUnpacker::unpackGzip(const QString& sourcePath, const QString& targetPath)
{
    return start([sourcePath, targetPath](Job& j) {
        j.spaceDir = QFileInfo(targetPath).absolutePath();
        // 1. Источник — gzip? Без сигнатуры целевой файл даже не создаётся.
        QFile probe(sourcePath);
        if (!probe.open(QIODevice::ReadOnly)) {
            j.sourceFailed(GzipDevice::Error::ReadError,
                           GzipDevice::errorText(GzipDevice::Error::ReadError) + QStringLiteral(": ")
                               + probe.errorString());
            return;
        }
        if (!GzipDevice::hasSignature(probe.read(2))) {
            j.sourceFailed(GzipDevice::Error::NotGzip, GzipDevice::errorText(GzipDevice::Error::NotGzip));
            return;
        }
        probe.close();
        if (targetIsSource(sourcePath, targetPath)) {
            j.r.error = Error::WriteFailed;
            j.r.message = QStringLiteral("Cannot unpack %1 onto itself").arg(sourcePath);
            return;
        }
        // 2–3. Место до начала, распаковка блоками; отмена и место — между блоками.
        j.gunzip(sourcePath, targetPath);
        // Частичный файл — не результат: не оставлять. Ошибка до создания
        // цели (место, источник) — цели нет и удалять нечего.
        if (j.r.error != Error::None)
            QFile::remove(targetPath);
    }, sourcePath, targetPath);
}

quint64 SourceUnpacker::unpackZip(const QString& zipPath, const QString& targetDir, MemberFilter accept)
{
    return start([zipPath, targetDir, accept](Job& j) { j.zipSet(zipPath, targetDir, accept); },
                 zipPath, targetDir);
}

quint64 SourceUnpacker::unpackZipMember(const QString& zipPath, const QString& member,
                                        const QString& targetPath)
{
    return start([zipPath, member, targetPath](Job& j) { j.zipMember(zipPath, member, targetPath); },
                 zipPath, targetPath);
}

quint64 SourceUnpacker::unpackArchive(const QString& archivePath, const QString& targetDir, MemberFilter accept)
{
    return start([archivePath, targetDir, accept](Job& j) {
        switch (ArchiveFormats::detectKnown(archivePath)) {
        case ArchiveFormats::Format::Zip: j.zipSet(archivePath, targetDir, accept); return;
        case ArchiveFormats::Format::Tar: j.tarSet(archivePath, targetDir, accept); return;
        case ArchiveFormats::Format::None: break;
        }
        j.notArchive(archivePath);
        QDir(targetDir).removeRecursively();
    }, archivePath, targetDir);
}

quint64 SourceUnpacker::unpackArchiveMember(const QString& archivePath, const QString& member,
                                            const QString& targetPath)
{
    return start([archivePath, member, targetPath](Job& j) {
        switch (ArchiveFormats::detectKnown(archivePath)) {
        case ArchiveFormats::Format::Zip: j.zipMember(archivePath, member, targetPath); return;
        case ArchiveFormats::Format::Tar: j.tarMember(archivePath, member, targetPath); return;
        case ArchiveFormats::Format::None: break;
        }
        j.notArchive(archivePath);
    }, archivePath, targetPath);
}

QString SourceUnpacker::errorText(Error error)
{
    switch (error) {
    case Error::None: return QString();
    case Error::Cancelled: return QStringLiteral("Unpacking cancelled");
    case Error::SourceError: return QStringLiteral("Cannot unpack the file");
    case Error::NoSpace: return QStringLiteral("Not enough disk space");
    case Error::WriteFailed: return QStringLiteral("Cannot write the unpacked file");
    }
    return QString();
}
