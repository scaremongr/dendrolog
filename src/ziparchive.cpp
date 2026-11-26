#include "ziparchive.h"

#include "textencoding.h"

#include <QFile>
#include <QIODevice>
#include <QStringConverter>

#include <miniz.h>

#include <vector>

namespace {

constexpr qint64 kChunkBytes = 256 * 1024;

size_t readCallback(void* opaque, mz_uint64 offset, void* buffer, size_t n)
{
    auto* file = static_cast<QFile*>(opaque);
    if (!file->seek(qint64(offset)))
        return 0;
    const qint64 got = file->read(static_cast<char*>(buffer), qint64(n));
    return got < 0 ? 0 : size_t(got);
}

QString decodeName(const QByteArray& raw, bool utf8Flag)
{
    if (utf8Flag)
        return QString::fromUtf8(raw);
    // Не помечено как UTF-8: корректный UTF-8 (многие архиваторы флаг не
    // ставят) — UTF-8, иначе OEM-кодировка русской Windows. Правило без
    // локали: имя члена сохраняется и по нему член ищется снова.
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = utf8.decode(raw);
    if (!utf8.hasError())
        return text;
    // Имя — не строка лога: без снятия ANSI-последовательностей (F-3).
    return TextDecoder(TextEncoding::Ibm866).decodeExact(raw.constData(), raw.size());
}

bool isUnixSymlink(const mz_zip_archive_file_stat& st)
{
    // Атрибуты Unix — в старших 16 битах, если архив сделан на Unix (3) или
    // macOS (19); S_IFLNK = 0120000.
    const int host = st.m_version_made_by >> 8;
    if (host != 3 && host != 19)
        return false;
    const quint32 mode = st.m_external_attr >> 16;
    return (mode & 0170000u) == 0120000u;
}

} // namespace

struct ZipArchive::Impl {
    QFile file;
    mz_zip_archive zip{};
    bool initialised = false;
};

ZipArchive::ZipArchive(const QString& path)
    : m_path(path), m(std::make_unique<Impl>())
{
}

ZipArchive::~ZipArchive()
{
    close();
}

void ZipArchive::close()
{
    if (m->initialised) {
        mz_zip_reader_end(&m->zip);
        m->initialised = false;
    }
    m->file.close();
}

void ZipArchive::fail(Error error, const QString& detail)
{
    m_error = error;
    m_errorString = detail.isEmpty() ? errorText(error) : errorText(error) + QStringLiteral(": ") + detail;
}

bool ZipArchive::hasSignature(const QByteArray& head)
{
    return head.size() >= 4 && head[0] == 'P' && head[1] == 'K'
        && ((head[2] == '\x03' && head[3] == '\x04') || (head[2] == '\x05' && head[3] == '\x06'));
}

bool ZipArchive::fileHasSignature(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return hasSignature(file.read(4));
}

bool ZipArchive::open()
{
    close();
    m_members.clear();
    m_error = Error::None;
    m_errorString.clear();
    m->file.setFileName(m_path);
    if (!m->file.open(QIODevice::ReadOnly)) {
        fail(Error::ReadError, m->file.errorString());
        return false;
    }
    if (!hasSignature(m->file.peek(4))) {
        fail(Error::NotZip);
        return false;
    }
    mz_zip_zero_struct(&m->zip);
    m->zip.m_pRead = &readCallback;
    m->zip.m_pIO_opaque = &m->file;
    if (!mz_zip_reader_init(&m->zip, mz_uint64(m->file.size()), 0)) {
        const mz_zip_error e = mz_zip_get_last_error(&m->zip);
        fail(e == MZ_ZIP_NOT_AN_ARCHIVE ? Error::NotZip : Error::Corrupt,
             QString::fromLatin1(mz_zip_get_error_string(e)));
        return false;
    }
    m->initialised = true;

    const mz_uint count = mz_zip_reader_get_num_files(&m->zip);
    m_members.reserve(int(count));
    std::vector<char> name(65536 + 1);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m->zip, i, &st)) {
            fail(Error::Corrupt, QString::fromLatin1(mz_zip_get_error_string(mz_zip_get_last_error(&m->zip))));
            return false;
        }
        // m_filename в stat усечён до 512 байтов — имя целиком отдельно.
        const mz_uint length = mz_zip_reader_get_filename(&m->zip, i, name.data(), mz_uint(name.size()));
        ZipMember member;
        member.index = int(i);
        member.name = decodeName(QByteArray(name.data(), length > 0 ? int(length) - 1 : 0),
                                 (st.m_bit_flag & 0x0800) != 0);
        member.size = qint64(st.m_uncomp_size);
        member.compressedSize = qint64(st.m_comp_size);
        member.directory = st.m_is_directory != 0;
        member.symlink = isUnixSymlink(st);
        member.encrypted = st.m_is_encrypted != 0;
        member.supported = st.m_is_supported != 0;
        m_members.append(member);
    }
    return true;
}

int ZipArchive::find(const QString& name) const
{
    for (const ZipMember& member : m_members)
        if (member.name == name)
            return member.index;
    return -1;
}

bool ZipArchive::extract(const ZipMember& member, QIODevice& out, const std::atomic_bool* abort,
                         const std::function<bool(qint64)>& onChunk)
{
    m_error = Error::None;
    m_errorString.clear();
    if (!m->initialised) {
        fail(Error::ReadError, QStringLiteral("archive is not open"));
        return false;
    }
    if (member.encrypted) {
        fail(Error::Unsupported, QStringLiteral("%1 is encrypted (password-protected archives are not supported)")
                                     .arg(member.name));
        return false;
    }
    if (!member.supported || member.directory) {
        fail(Error::Unsupported, member.name);
        return false;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&m->zip, mz_uint(member.index), &st)) {
        fail(Error::Corrupt, member.name);
        return false;
    }
    mz_zip_reader_extract_iter_state* it = mz_zip_reader_extract_iter_new(&m->zip, mz_uint(member.index), 0);
    if (!it) {
        fail(Error::Corrupt, QStringLiteral("%1: %2").arg(member.name,
             QString::fromLatin1(mz_zip_get_error_string(mz_zip_get_last_error(&m->zip)))));
        return false;
    }
    QByteArray buffer(int(kChunkBytes), Qt::Uninitialized);
    qint64 written = 0;
    mz_ulong crc = MZ_CRC32_INIT;
    bool ok = true;
    for (;;) {
        if (abort && abort->load(std::memory_order_relaxed)) {
            fail(Error::Aborted);
            ok = false;
            break;
        }
        const size_t n = mz_zip_reader_extract_iter_read(it, buffer.data(), size_t(buffer.size()));
        if (it->status < 0) {
            fail(Error::Corrupt, QStringLiteral("%1: %2").arg(member.name,
                 QString::fromLatin1(mz_zip_get_error_string(mz_zip_get_last_error(&m->zip)))));
            ok = false;
            break;
        }
        if (n == 0)
            break;
        crc = mz_crc32(crc, reinterpret_cast<const unsigned char*>(buffer.constData()), n);
        if (out.write(buffer.constData(), qint64(n)) != qint64(n)) {
            fail(Error::WriteError, out.errorString());
            ok = false;
            break;
        }
        written += qint64(n);
        if (written > qint64(st.m_uncomp_size)) {
            fail(Error::Corrupt, QStringLiteral("%1: data longer than recorded").arg(member.name));
            ok = false;
            break;
        }
        if (onChunk && !onChunk(written)) {
            m_error = Error::Stopped;
            ok = false;
            break;
        }
    }
    mz_zip_reader_extract_iter_free(it);
    if (!ok)
        return false;
    if (written != qint64(st.m_uncomp_size)) {
        fail(Error::Corrupt, QStringLiteral("%1: data shorter than recorded").arg(member.name));
        return false;
    }
    if (quint32(crc) != st.m_crc32) {
        fail(Error::Corrupt, QStringLiteral("%1: CRC mismatch").arg(member.name));
        return false;
    }
    return true;
}

QString ZipArchive::errorText(Error error)
{
    switch (error) {
    case Error::None: return QString();
    case Error::NotZip: return QStringLiteral("Not a zip archive");
    case Error::ReadError: return QStringLiteral("Cannot read the archive");
    case Error::Corrupt: return QStringLiteral("The archive is damaged");
    case Error::Unsupported: return QStringLiteral("Unsupported archive entry");
    case Error::Aborted: return QStringLiteral("Extraction cancelled");
    case Error::WriteError: return QStringLiteral("Cannot write the extracted file");
    case Error::Stopped: return QStringLiteral("Extraction stopped");
    }
    return QString();
}

// ---------------------------------------------------------------------------

namespace ArchiveNames {

static bool isReservedWindowsName(const QString& component)
{
    // Имя до первой точки, без учёта регистра: «con», «CON.log», «Com1.txt».
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    static const QStringList kFixed = { QStringLiteral("CON"), QStringLiteral("PRN"),
                                        QStringLiteral("AUX"), QStringLiteral("NUL") };
    if (kFixed.contains(base))
        return true;
    if (base.size() == 4 && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))
        && base[3] >= u'1' && base[3] <= u'9')
        return true;
    return false;
}

QString safeRelativePath(const QString& memberName)
{
    QString name = memberName;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')) || name.endsWith(QLatin1Char('/')))
        return QString(); // абсолютный, UNC или каталог
    if (name.size() >= 2 && name[1] == u':' && name[0].isLetter())
        return QString(); // буква диска (в том числе «C:x» — относительный к диску)
    QStringList out;
    const QStringList parts = name.split(QLatin1Char('/'));
    for (QString part : parts) {
        if (part.isEmpty() || part == QLatin1String("."))
            continue;
        if (part == QLatin1String(".."))
            return QString();
        for (QChar& c : part) {
            const char16_t u = c.unicode();
            if (u < 0x20 || u == 0x7F || u == u'<' || u == u'>' || u == u':' || u == u'"' || u == u'|'
                || u == u'?' || u == u'*')
                c = u'_';
        }
        // Windows молча отрезает хвостовые точки и пробелы: «a.log.» и
        // «a.log» — один файл. Режем сами — два члена не попадут в одно
        // имя незаметно (повторы распаковщик разводит суффиксом).
        while (!part.isEmpty() && (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' '))))
            part.chop(1);
        if (part.isEmpty())
            part = QStringLiteral("_");
        if (isReservedWindowsName(part))
            part.prepend(QLatin1Char('_'));
        out << part;
    }
    return out.join(QLatin1Char('/'));
}

} // namespace ArchiveNames
