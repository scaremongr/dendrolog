#ifndef TARARCHIVE_H
#define TARARCHIVE_H

#include <QByteArray>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

class QIODevice;

// ============================================================================
// TarArchive — чтение tar (F-2, шаг 5): `.tar`, `.tar.gz`, `.tgz`. Свой код,
// без зависимостей: заголовки по 512 байт, данные следом, всё — потоком.
//
// Форматы: ustar (имя = prefix + '/' + name), старый v7 (без magic — только
// по имени файла, см. ниже), GNU (`ustar  `: длинные имена и ссылки — члены
// 'L' / 'K', размер в base-256 для файлов больше 8 ГБ), pax (член 'x':
// `path`, `linkpath`, `size` для следующего члена; глобальный 'g' не
// влияет — его `path` бессмыслен для набора). Контрольная сумма заголовка
// проверяется (беззнаковая и знаковая, как у GNU tar).
//
// Источник — сам файл или gzip поверх него (GzipDevice): сжатый tar читается
// только последовательно, поэтому член ищется проходом от начала. После
// конца архива (нулевой блок) gzip дочитывается до конца: CRC и длина его
// потоков проверены, и результат либо есть, либо целиком проверен, как у
// `.gz`.
//
// Целостность: обрыв в заголовке, данных или их выравнивании, неверная
// контрольная сумма, мусор вместо заголовка, длинное имя больше 1 МБ —
// Corrupt. Конец файла ровно на границе заголовка без нулевых блоков
// принимается (так же поступает GNU tar). У tar нет CRC данных — проверка
// данных есть только у `.tar.gz` (CRC gzip).
//
// Имена: pax `path` — UTF-8; остальные — UTF-8, если корректны, иначе
// Latin-1 (байт = символ, обратимо; tar почти всегда с Linux). Правило без
// локали: имя члена сохраняется и по нему член ищется снова.
//
// Объект — для одного потока (воркер распаковщика).
// ============================================================================
struct TarMember {
    enum class Type {
        File,        // '0', '\0', '7'
        Directory,   // '5'
        Symlink,     // '2'
        HardLink,    // '1'
        Device,      // '3', '4'
        Fifo,        // '6'
        Unsupported, // разреженный GNU 'S', тома, прочие буквы
    };
    QString name;          // как записано ('/'); у каталога без хвостовой '/'
    qint64  size = 0;      // длина данных члена
    Type    type = Type::File;
    bool isFile() const { return type == Type::File; }
};

class TarArchive {
public:
    enum class Error {
        None,
        NotTar,      // первый заголовок — не tar (в том числе пустой файл)
        ReadError,   // файл не открыть / не прочитать
        Corrupt,     // обрыв, контрольная сумма, мусор; у .tar.gz — и ошибки gzip
        Aborted,     // флаг прерывания
        WriteError,  // приёмник не принял байты
        Stopped,     // onChunk вернул false (место, решение вызывающего)
    };

    explicit TarArchive(const QString& path);
    ~TarArchive();
    TarArchive(const TarArchive&) = delete;
    TarArchive& operator=(const TarArchive&) = delete;

    // Открыть: gzip поверх tar узнаётся по сигнатуре сам. abort — флаг
    // прерывания (проверяется на каждом блоке чтения).
    bool open(const std::atomic_bool* abort = nullptr);
    void close();

    // Следующий член (служебные 'L', 'K', 'x', 'g' применяются и не
    // выдаются). Данные прежнего, если их не прочитали, пропускаются.
    // false — конец архива (error() == None) или ошибка.
    bool next(TarMember& member);
    // Данные текущего члена в out; onChunk(всего записано) — после каждого
    // блока, false останавливает (Error::Stopped).
    bool extract(QIODevice& out, const std::function<bool(qint64 written)>& onChunk = {});
    // Дочитать источник до конца (у .tar.gz — проверка CRC и длины gzip;
    // у .tar ничего не делает). Звать после конца архива или после
    // извлечения нужного члена.
    bool finish(const std::function<void()>& onChunk = {});

    bool isCompressed() const;
    qint64 archiveSize() const;    // размер файла на диске
    qint64 bytesRead() const;      // прочитано из файла (у gzip — сжатых)

    Error error() const { return m_error; }
    QString errorString() const { return m_errorString; }

    // Блок — заголовок tar: контрольная сумма сходится и есть magic `ustar`.
    // Без magic (v7) — только при strict = false.
    static bool isHeader(const QByteArray& block, bool strict = true);
    // Файл — tar (заголовок ustar в начале) или gzip, у которого первый
    // распакованный блок — заголовок ustar (`.tar.gz` под любым именем).
    static bool fileHasSignature(const QString& path);
    // Файл — gzip с tar внутри (частный случай fileHasSignature).
    static bool fileIsCompressedTar(const QString& path);

    static QString errorText(Error error);

private:
    struct Impl;
    bool readExact(char* data, qint64 n, bool& eofAtStart);
    bool skipData();
    bool fail(Error error, const QString& detail = QString());

    QString m_path;
    std::unique_ptr<Impl> m;
    Error m_error = Error::None;
    QString m_errorString;
};

// ============================================================================
// ArchiveFormats — архив с набором файлов (zip или tar), который открывается
// в Directory Scanner, а не вкладкой (F-2, шаги 4–5). По содержимому:
// сигнатура zip, заголовок ustar, gzip с tar внутри. `.gz` с одним логом —
// не архив (вкладка). Имя файла не важно: правило по имени —
// OpenRouting::isArchiveName.
// ============================================================================
namespace ArchiveFormats {
enum class Format { None, Zip, Tar };
Format detect(const QString& path);
inline bool isMultiFileArchive(const QString& path) { return detect(path) != Format::None; }
// Файл, о котором УЖЕ известно, что он архив (открыт по имени `.tar`,
// архив члена): как detect, но tar — и без magic (v7) и пустой (одни
// нулевые блоки, сжатый или нет) — всё, что TarArchive читает без ошибки.
Format detectKnown(const QString& path);
}

#endif // TARARCHIVE_H
