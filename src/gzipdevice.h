#ifndef GZIPDEVICE_H
#define GZIPDEVICE_H

#include <QByteArray>
#include <QIODevice>
#include <QString>
#include <atomic>
#include <memory>

// ============================================================================
// GzipDevice — последовательный QIODevice, отдающий распакованные байты gzip
// (RFC 1952) потоком, без записи на диск и без файла целиком в памяти (F-2).
//
// Источник — путь (устройство своё) или чужой открытый QIODevice. Декодер —
// inflate из miniz (third_party/miniz) за этим интерфейсом: заголовок gzip,
// CRC-32, длина и склейка потоков — свой код, miniz разбирает только deflate.
//
// Что поддерживается:
//   • один и несколько склеенных потоков (`cat a.gz b.gz`, pigz, bgzip):
//     распакованное — подряд, как у `gzip -d`;
//   • все поля заголовка: FEXTRA, FNAME (originalName()), FCOMMENT, FHCRC
//     (проверяется);
//   • хвост каждого потока: CRC-32 и ISIZE (длина по модулю 2^32);
//   • нули после последнего потока (выравнивание блоков ленты) — как у
//     `gzip -d`, без ошибки.
// Всё остальное — своя ошибка: не gzip (включая пустой файл), неизвестный
// метод сжатия или флаги, обрыв, повреждённые данные, неверная CRC или
// длина, мусор после потока, ошибка чтения источника.
//
// При ошибке read() возвращает -1 (уже отданные байты остаются отданными);
// error() и errorString() говорят почему. Байты до ошибки НЕ проверены CRC:
// распаковщик, которому важна целостность, дочитывает до конца и смотрит
// error() (SourceUnpacker удаляет результат при любой ошибке).
//
// Потокобезопасность: обычный QIODevice — один поток на объект; создавать
// можно в воркере.
// ============================================================================
class GzipDevice : public QIODevice {
    Q_OBJECT

public:
    enum class Error {
        None,
        NotGzip,          // нет сигнатуры 1F 8B (в том числе пустой файл)
        BadHeader,        // метод не deflate, зарезервированные флаги, CRC заголовка
        Truncated,        // источник кончился посреди потока
        CorruptData,      // deflate-данные повреждены
        CrcMismatch,      // CRC-32 распакованного не совпала с хвостом
        SizeMismatch,     // ISIZE не совпала с длиной распакованного
        TrailingGarbage,  // после потока не gzip и не нули
        ReadError,        // источник не открылся или не читается
        Aborted,          // поднят флаг прерывания (setAbortFlag)
    };

    explicit GzipDevice(const QString& path, QObject* parent = nullptr);
    // source не принадлежит устройству и должен жить дольше него. Открытый
    // читается с текущей позиции и остаётся открытым после close(); закрытый
    // устройство открывает при open() и закрывает при close().
    explicit GzipDevice(QIODevice* source, QObject* parent = nullptr);
    ~GzipDevice() override;

    bool open(OpenMode mode) override;   // только ReadOnly
    void close() override;
    bool isSequential() const override { return true; }
    bool atEnd() const override;

    Error error() const { return m_error; }

    // Флаг прерывания (не принадлежит устройству, живёт дольше него). Поднят —
    // read() кончается ошибкой Aborted на ближайшем блоке источника (256 КБ
    // сжатого), даже если вход не даёт вывода. Так отмена распаковки не ждёт
    // конца файла из одних нулей или пустых потоков.
    void setAbortFlag(const std::atomic_bool* flag) { m_abort = flag; }

    // Сколько СЖАТЫХ байт прочитано из источника — для прогресса (источник
    // читается блоками, поэтому значение опережает распакованное на блок).
    qint64 compressedBytesRead() const { return m_compressedRead; }
    // Сколько распаковано всего (по всем потокам).
    qint64 uncompressedBytes() const { return m_totalOut; }
    // Имя файла из заголовка первого потока (FNAME, Latin-1), если есть.
    QString originalName() const { return m_originalName; }
    // Сколько потоков начато (склеенный файл — больше одного).
    int memberCount() const { return m_members; }

    // Первые байты — сигнатура gzip (1F 8B)? Имя файла не важно.
    static bool hasSignature(const QByteArray& head);
    static bool fileHasSignature(const QString& path);
    // Поле ISIZE в последних 4 байтах файла (длина последнего потока по
    // модулю 4 ГБ) — нижняя оценка размера распакованного для проверки места.
    // -1 — файл короче минимального gzip или не читается.
    static qint64 trailerSize(const QString& path);

    static QString errorText(Error error);

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    enum class State { Header, Body, Trailer, BetweenMembers, Done, Failed };

    int getByte();                 // байт источника или -1 (конец/ошибка чтения)
    bool fillInput();              // дочитать буфер источника; false — конец
    bool parseHeader(bool first);  // first: сигнатура ещё не прочитана
    bool readTrailer();
    bool afterMember();            // следующий поток, нули или мусор
    void fail(Error error);

    struct Inflater;

    std::unique_ptr<QIODevice> m_ownedSource;
    QIODevice* m_source = nullptr;
    bool m_openedSource = false;   // источник открыт нами в open()
    const std::atomic_bool* m_abort = nullptr;
    bool m_aborted = false;
    std::unique_ptr<Inflater> m_inflater;

    QByteArray m_in;               // буфер сжатых байт
    int m_inPos = 0;
    int m_inLen = 0;
    bool m_sourceEof = false;
    bool m_readFailed = false;

    State m_state = State::Header;
    Error m_error = Error::None;
    quint32 m_crc = 0;             // CRC-32 распакованного текущего потока
    quint32 m_memberOut = 0;       // его длина по модулю 2^32
    qint64 m_totalOut = 0;
    qint64 m_compressedRead = 0;
    int m_members = 0;
    QString m_originalName;
};

#endif // GZIPDEVICE_H
