#ifndef ZIPARCHIVE_H
#define ZIPARCHIVE_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QIODevice;

// ============================================================================
// ZipArchive — чтение zip (F-2, шаг 4): оглавление и извлечение члена
// потоком, без загрузки в память.
//
// Inflate и разбор оглавления (в том числе zip64) — miniz
// (`third_party/miniz`), чтение файла — свой QFile через функцию чтения
// miniz: путь с не-ASCII символами на Windows открывается так же, как любой
// другой (fopen miniz взял бы ANSI-путь). Целостность члена проверяется
// здесь же: длина из оглавления и CRC-32 — по выданным байтам, а не по
// статусу итератора miniz (у хранимых без сжатия членов он CRC не сверяет).
// Член, разворачивающийся больше заявленной длины, — ошибка: miniz сам
// останавливает inflate за пределом (zip-«бомба» с ложным размером).
//
// Имена: флаг UTF-8 (бит 11) или корректный UTF-8 — UTF-8; иначе — IBM866
// (OEM-кодировка русской Windows, «Сжатая ZIP-папка» пишет имена в ней).
// Правило не зависит от локали: имя члена сохраняется (закладки, память
// файла, сессии) и по нему же член извлекается снова.
//
// Объект — для одного потока (воркер распаковщика); GUI-поток его не видит.
// ============================================================================
struct ZipMember {
    int     index = -1;        // номер в оглавлении
    QString name;              // имя в архиве: разделители '/', как записано
    qint64  size = 0;          // распакованный размер из оглавления
    qint64  compressedSize = 0;
    bool    directory = false;
    bool    symlink = false;   // символическая ссылка (Unix-атрибуты) — не извлекается
    bool    encrypted = false; // пароль — не поддерживается
    bool    supported = true;  // метод сжатия известен miniz (stored, deflate)
};

class ZipArchive {
public:
    enum class Error {
        None,
        NotZip,      // нет сигнатуры или оглавления
        ReadError,   // файл не открыть / не прочитать
        Corrupt,     // повреждён: оглавление, данные, CRC, длина
        Unsupported, // метод сжатия или шифрование
        Aborted,     // флаг прерывания
        WriteError,  // приёмник не принял байты
        Stopped,     // onChunk вернул false (место, решение вызывающего)
    };

    explicit ZipArchive(const QString& path);
    ~ZipArchive();
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    // Открыть и прочитать оглавление.
    bool open();
    void close();

    Error error() const { return m_error; }
    QString errorString() const { return m_errorString; }

    const QVector<ZipMember>& members() const { return m_members; }
    // Член по имени (точное совпадение, как записано). -1 — нет.
    int find(const QString& name) const;

    // Извлечь член в out (открыт на запись вызывающим). abort — флаг
    // прерывания, проверяется на каждом блоке; onChunk(всего записано) — после
    // каждого блока; false останавливает извлечение (Error::Stopped).
    bool extract(const ZipMember& member, QIODevice& out, const std::atomic_bool* abort = nullptr,
                 const std::function<bool(qint64 written)>& onChunk = {});

    // Сигнатура zip: локальный заголовок «PK\3\4» или пустой архив «PK\5\6».
    static bool hasSignature(const QByteArray& head);
    static bool fileHasSignature(const QString& path);

    static QString errorText(Error error);

private:
    struct Impl;
    void fail(Error error, const QString& detail = QString());

    QString m_path;
    std::unique_ptr<Impl> m;
    QVector<ZipMember> m_members;
    Error m_error = Error::None;
    QString m_errorString;
};

// ============================================================================
// ArchiveNames — имя члена архива → безопасный относительный путь на диске
// (F-2, «Архивы с набором файлов»). Отбрасываются (пустой результат):
// абсолютные пути (`/etc/x`, `\\server\x`), буквы дисков (`C:`), любой
// компонент `..`, пустое имя и каталоги. Заменяются: символы, недопустимые
// в именах Windows (`<>:"|?*`, управляющие), — на `_`; хвостовые точки и
// пробелы компонента убираются; зарезервированные имена Windows (CON, PRN,
// AUX, NUL, COM1–9, LPT1–9, с любым расширением) получают префикс `_`.
// Обратная косая — разделитель (архивы из Windows). Компоненты `.` и
// пустые выбрасываются. Символические ссылки отсекает распаковщик по
// атрибутам члена.
// ============================================================================
namespace ArchiveNames {
QString safeRelativePath(const QString& memberName);
}

#endif // ZIPARCHIVE_H
