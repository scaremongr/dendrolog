#include "secretstore.h"

#include "appsettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#  include <QSaveFile>
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dpapi.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace {

QString& sessionKeyStorage()
{
    static QString key;
    return key;
}

#ifdef Q_OS_WIN
const char kEntropy[] = "DendroLog AI key v1";

QByteArray protect(const QByteArray& plain, bool encrypt, bool* ok)
{
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()));
    in.cbData = DWORD(plain.size());
    DATA_BLOB entropy;
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    entropy.cbData = DWORD(sizeof(kEntropy) - 1);
    DATA_BLOB out{};
    const BOOL done = encrypt
        ? CryptProtectData(&in, L"DendroLog", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)
        : CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    *ok = done != FALSE;
    if (!done)
        return QByteArray();
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
#endif

} // namespace

SecretStore::SecretStore(const QString& dir, Kind kind)
    : m_dir(dir)
    , m_kind(dir.isEmpty() ? Kind::Unavailable : kind)
{
    if (m_kind == Kind::SystemProtected)
        m_path = QDir(dir).filePath(QStringLiteral("ai-key.dpapi"));
    else if (m_kind == Kind::PrivateFile)
        m_path = QDir(dir).filePath(QStringLiteral("ai-key"));
#ifndef Q_OS_WIN
    if (m_kind == Kind::SystemProtected)
        m_kind = Kind::Unavailable;   // DPAPI есть только на Windows
#endif
}

SecretStore::Kind SecretStore::platformKind()
{
#ifdef Q_OS_WIN
    return Kind::SystemProtected;
#else
    return Kind::PrivateFile;
#endif
}

SecretStore SecretStore::forApplication()
{
#ifdef Q_OS_WIN
    return SecretStore(AppSettings::configDir(), Kind::SystemProtected);
#else
    // Portable (каталог настроек — каталог exe) и подменённый тестом каталог:
    // ключ не сохраняется.
    const QString config = QDir(AppSettings::configDir()).absolutePath();
    const QString exeDir = QDir(QCoreApplication::applicationDirPath()).absolutePath();
    if (config == exeDir || AppSettings::isConfigDirOverridden())
        return SecretStore(QString(), Kind::Unavailable);
    return SecretStore(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), Kind::PrivateFile);
#endif
}

QString SecretStore::description() const
{
    switch (m_kind) {
    case Kind::SystemProtected:
        return QCoreApplication::translate("SecretStore",
                                           "Saved encrypted for your Windows account (DPAPI) in %1.")
            .arg(QDir::toNativeSeparators(m_path));
    case Kind::PrivateFile:
        return QCoreApplication::translate("SecretStore",
                                           "Saved in %1, readable only by your user account. The file is not "
                                           "encrypted: anyone with access to your account can read it.")
            .arg(QDir::toNativeSeparators(m_path));
    case Kind::Unavailable:
        break;
    }
    return QCoreApplication::translate("SecretStore",
                                       "In portable mode the key is not saved: set the ANTHROPIC_API_KEY "
                                       "environment variable, or enter the key for this session only.");
}

bool SecretStore::hasKey() const
{
    return isAvailable() && QFileInfo::exists(m_path);
}

bool SecretStore::readKey(QString* key, QString* error) const
{
    if (key)
        key->clear();
    if (!hasKey())
        return false;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QCoreApplication::translate("SecretStore", "The saved key could not be read.");
        return false;
    }
    QByteArray bytes = file.readAll();
#ifdef Q_OS_WIN
    bool ok = false;
    const QByteArray plain = protect(bytes, false, &ok);
    if (!ok) {
        if (error)
            *error = QCoreApplication::translate("SecretStore",
                                                 "The saved key could not be decrypted (another Windows user or "
                                                 "machine?). Enter it again.");
        return false;
    }
    bytes = plain;
#endif
    if (key)
        *key = QString::fromUtf8(bytes).trimmed();
    return !QString::fromUtf8(bytes).trimmed().isEmpty();
}

bool SecretStore::writeKey(const QString& key, QString* error)
{
    const auto fail = [error](const QString& text) {
        if (error)
            *error = text;
        return false;
    };
    if (!isAvailable())
        return fail(description());
    const QByteArray plain = key.trimmed().toUtf8();
    if (plain.isEmpty())
        return removeKey(error);
#ifdef Q_OS_WIN
    if (!QDir().mkpath(m_dir))
        return fail(QCoreApplication::translate("SecretStore", "The folder for the key could not be created."));
    bool ok = false;
    const QByteArray sealed = protect(plain, true, &ok);
    if (!ok)
        return fail(QCoreApplication::translate("SecretStore", "Windows could not encrypt the key."));
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly) || file.write(sealed) != sealed.size() || !file.commit())
        return fail(QCoreApplication::translate("SecretStore", "The key could not be saved."));
    return true;
#else
    // Каталог 0700, файл создаётся сразу с правами 0600 (не «записать, потом
    // сузить права»), затем переименовывается на место.
    if (!QDir().mkpath(m_dir))
        return fail(QCoreApplication::translate("SecretStore", "The folder for the key could not be created."));
    ::chmod(QFile::encodeName(m_dir).constData(), 0700);
    const QByteArray target = QFile::encodeName(m_path);
    const QByteArray temp = target + ".tmp";
    ::unlink(temp.constData());
    const int fd = ::open(temp.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return fail(QCoreApplication::translate("SecretStore", "The key could not be saved."));
    qsizetype written = 0;
    while (written < plain.size()) {
        const ssize_t n = ::write(fd, plain.constData() + written, size_t(plain.size() - written));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        written += n;
    }
    const bool synced = ::fsync(fd) == 0;
    ::close(fd);
    if (written != plain.size() || !synced || ::rename(temp.constData(), target.constData()) != 0) {
        ::unlink(temp.constData());
        return fail(QCoreApplication::translate("SecretStore", "The key could not be saved."));
    }
    return true;
#endif
}

bool SecretStore::removeKey(QString* error)
{
    if (!isAvailable() || !QFileInfo::exists(m_path))
        return true;
    if (!QFile::remove(m_path)) {
        if (error)
            *error = QCoreApplication::translate("SecretStore", "The saved key could not be removed.");
        return false;
    }
    return true;
}

QString SecretStore::environmentKey()
{
    return qEnvironmentVariable("ANTHROPIC_API_KEY").trimmed();
}

void SecretStore::setSessionKey(const QString& key)
{
    sessionKeyStorage() = key.trimmed();
}

QString SecretStore::sessionKey()
{
    return sessionKeyStorage();
}

QString SecretStore::effectiveKey(const SecretStore& store, Source* source)
{
    const auto give = [source](Source s, const QString& key) {
        if (source)
            *source = s;
        return key;
    };
    const QString env = environmentKey();
    if (!env.isEmpty())
        return give(Source::Environment, env);
    QString stored;
    if (store.readKey(&stored) && !stored.isEmpty())
        return give(Source::Stored, stored);
    const QString session = sessionKey();
    if (!session.isEmpty())
        return give(Source::Session, session);
    return give(Source::None, QString());
}
