#ifndef SOURCEUNPACKER_H
#define SOURCEUNPACKER_H

#include "gzipdevice.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>

// ============================================================================
// SourceUnpacker — распаковка сжатого источника во временный файл (F-2).
//
// Один объект — одно задание за раз, как LogParser/LogIndexer на файл:
// приватный пул из одного потока, atomic-флаг отмены, деструктор отменяет и
// ЖДЁТ воркер. Воркер получает только пути и копию настроек (снимок на
// момент старта); живых структур GUI-потока не видит. Прогресс и результат
// приходят queued-сигналами на поток объекта.
//
// Место на диске (docs/design/F-2.md, «Место на диске»):
//   • ДО начала: нужно не меньше max(размер сжатого, ISIZE из хвоста) плюс
//     резерв (по умолчанию 512 МБ). ISIZE — длина по модулю 4 ГБ, поэтому это
//     только нижняя оценка;
//   • ПО ХОДУ: каждые checkInterval (64 МБ) записанного свободное место
//     сравнивается с резервом; меньше — остановка с NoSpace. Это и защита от
//     gzip-«бомбы» (поток нулей, сжатый в тысячу раз, с ложной или
//     переполненной ISIZE): она останавливается, не заполнив диск дальше
//     резерва (с точностью до одного интервала). Отдельного предела на размер
//     или отношение сжатия нет — решение «Место на диске» документа.
// Функция «сколько свободно» подменяемая (тесты — без заполнения диска).
//
// Отмена и любая ошибка удаляют частичный файл: результата либо нет, либо он
// целиком проверен (CRC и длина каждого потока gzip).
//
// zip (шаг 4, ziparchive.h):
//   • unpackZip — набор файлов во временный КАТАЛОГ (сканер): только члены,
//     которые принял фильтр (имена логов из настроек и их .gz); имена
//     обезврежены ArchiveNames::safeRelativePath, ссылки и члены с паролем
//     пропускаются и считаются. Место ДО начала — сумма размеров отобранных
//     членов из оглавления (они точные) плюс резерв, по ходу — как у gzip.
//     Ошибка или отмена удаляют каталог целиком (воркер, до итога; ждать его
//     для этого не нужно). Повтор имени члена — извлекается первый;
//   • unpackZipMember — один член в ФАЙЛ (вкладка: открыть из сканера,
//     восстановить закладки, память файла, сессию по имени члена). Член,
//     который сам gzip (`app.log.gz` в архиве), распаковывается дальше в тот
//     же файл — вкладка получает текст.
// Прогресс у zip — распакованные байты из суммы размеров оглавления.
// ============================================================================
class SourceUnpacker : public QObject {
    Q_OBJECT

public:
    enum class Error {
        None,
        Cancelled,
        SourceError,  // источник не gzip / повреждён / не читается — см. gzipError
        NoSpace,      // не хватает места (до начала или по ходу)
        WriteFailed,  // не создать или не записать целевой файл (в том числе цель — сам источник)
    };

    struct Result {
        quint64 job = 0;          // номер задания из unpackGzip
        Error error = Error::None;
        GzipDevice::Error gzipError = GzipDevice::Error::None;
        QString message;          // текст для пользователя (английский, как UI)
        QString sourcePath;
        QString targetPath;
        qint64 bytesWritten = 0;  // распакованных байт (при ошибке — сколько успели)
        qint64 requiredBytes = 0; // NoSpace: сколько требовалось (с резервом)
        qint64 availableBytes = 0;// NoSpace: сколько было свободно
        // unpackZip: что извлечено (имя в архиве → путь относительно каталога)
        // и что пропущено.
        struct Extracted {
            QString member;       // имя в архиве (как записано, '/')
            QString relativePath; // путь в каталоге извлечения
            qint64 size = 0;
        };
        QVector<Extracted> extracted;
        int skippedUnsafe = 0;    // опасное имя (.., абсолютный путь, диск) или ссылка
        int skippedEncrypted = 0; // с паролем или неизвестным методом сжатия
        bool ok() const { return error == Error::None; }
    };

    // Фильтр членов для unpackZip: имя в архиве → извлекать ли. Зовётся в
    // воркере: только копии данных, без живых объектов GUI-потока.
    using MemberFilter = std::function<bool(const QString& memberName)>;

    // Сколько байт свободно на томе каталога dir (-1 — неизвестно: проверка
    // пропускается). Зовётся в воркере.
    using FreeSpaceFunction = std::function<qint64(const QString& dir)>;

    static constexpr qint64 kDefaultReserveBytes = 512LL * 1024 * 1024;
    static constexpr qint64 kDefaultCheckIntervalBytes = 64LL * 1024 * 1024;

    explicit SourceUnpacker(QObject* parent = nullptr);
    ~SourceUnpacker() override;

    // Настройки берутся снимком при старте задания.
    void setFreeSpaceFunction(FreeSpaceFunction fn);
    void setReserveBytes(qint64 bytes) { m_reserveBytes = bytes; }
    void setCheckIntervalBytes(qint64 bytes) { m_checkIntervalBytes = bytes; }
    qint64 reserveBytes() const { return m_reserveBytes; }

    // Распаковать gzip sourcePath в targetPath (каталог должен существовать;
    // файл перезаписывается). Возвращает номер задания (> 0) — он же придёт в
    // Result::job; 0 — воркер ещё занят прежним заданием, ничего не запущено.
    // Итог — сигнал finished.
    quint64 unpackGzip(const QString& sourcePath, const QString& targetPath);
    // zip целиком (отобранные члены) в каталог targetDir (создаётся; при
    // ошибке и отмене удаляется). Номер задания — как у unpackGzip.
    quint64 unpackZip(const QString& zipPath, const QString& targetDir, MemberFilter accept);
    // Один член zip в файл targetPath; член-gzip — распакованным.
    quint64 unpackZipMember(const QString& zipPath, const QString& member, const QString& targetPath);

    // Отменить идущее задание (finished придёт с Cancelled, файла не будет).
    // Прерывание срабатывает на ближайшем блоке входа (GzipDevice::setAbortFlag).
    void cancel();
    // Воркер занят. Становится false, как только воркер закончил, — раньше,
    // чем finished дойдёт через очередь событий: после cancel() +
    // waitForFinished() новое задание запускается сразу, а запоздавший
    // Result прежнего отличается номером job.
    bool isRunning() const { return m_busy.load(); }
    // Дождаться воркера (тесты; деструктор; перезапуск после cancel()).
    void waitForFinished();

    static qint64 systemFreeSpace(const QString& dir);
    static QString errorText(Error error);

signals:
    // Прочитано сжатых байт из total (размер источника) — не чаще ~раза в
    // 4 МБ сжатого.
    void progress(qint64 compressedDone, qint64 compressedTotal);
    void finished(const SourceUnpacker::Result& result);

    // Внутренний: из воркера на поток объекта (queued).
    void workerFinished(const SourceUnpacker::Result& result, QPrivateSignal);

private:
    void onWorkerFinished(const SourceUnpacker::Result& result);
    struct Job;
    quint64 start(const std::function<void(Job&)>& body, const QString& sourcePath,
                  const QString& targetPath);

    QThreadPool m_pool;
    std::atomic_bool m_cancel{false};
    std::atomic_bool m_busy{false};
    quint64 m_lastJob = 0;
    FreeSpaceFunction m_freeSpace;
    qint64 m_reserveBytes = kDefaultReserveBytes;
    qint64 m_checkIntervalBytes = kDefaultCheckIntervalBytes;
};

Q_DECLARE_METATYPE(SourceUnpacker::Result)

#endif // SOURCEUNPACKER_H
