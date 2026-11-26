#ifndef LINEFORMAT_H
#define LINEFORMAT_H

#include "keyedline.h"

#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <atomic>

class QIODevice;

// ============================================================================
// Формат строк файла (F-17, шаг 7; docs/design/F-17.md, раздел 1).
//
// Чтение не знает о схеме полей: то, что раньше приходило в загрузку из
// схемы (граница записи «по схеме», у ключевой — момент и уровень из
// ключей), — свойство ФАЙЛА, которое определяется само, как кодировка
// (F-1), и так же хранится на файл:
//   • Plain — обычный текст. Граница записи — правило F-12 (метка и слово
//     уровня либо ведущая метка), плюс levelStartsRecord: в файле почти нет
//     меток, а строки начинаются со слова уровня («WARNING:root:…»,
//     «[INFO] …») — запись начинает и такая строка;
//   • Json / Logfmt — строки JSON-объекты или пары key=value: момент и
//     уровень из ключей ролей (keys — то, что было ключевой схемой F-4),
//     строка, распознанная форматом, начинает запись.
//
// Решение — LineFormatDetector::decide по первым строкам файла, его зовут
// LogParser и LogIndexer В НАЧАЛЕ прохода, до первой классифицированной
// строки (LineFormatDetector::advance), и сообщают сигналом раньше первой
// порции. Поэтому бэкенды согласны по построению. Пока непустых строк
// меньше kMinLines (растущий лог, stdin), формат «не решён» и строки
// читаются как Plain; первая дозапись, после которой их стало достаточно,
// решает, и если решение читает строки иначе, чем Plain, вкладка
// перечитывает файл один раз (он по определению мал).
//
// Выбор пользователя (Origin::User; индикатор в строке статуса) главнее и
// автоматикой не меняется; сохраняется в состоянии вкладки, памяти файла и
// сессии. Решение автоматики не сохраняется — оно выводится заново.
//
// keys.extraColumns — ключи, встреченные больше чем в половине строк начала
// файла: колонки полей JSON-файла (KeyedExtractor) и список дока. Чтение их
// не видит (readingSignature их не включает).
//
// Ловушка MSVC: LineFormat — struct везде.
// ============================================================================

struct LineFormat {
    enum class Kind { Plain, Json, Logfmt };
    enum class Origin { Undecided, Auto, User };

    Kind kind = Kind::Plain;
    // Plain: запись начинает ещё и строка с ведущим словом уровня (с позиции
    // 0 или 1 за «[»).
    bool levelStartsRecord = false;
    // Json / Logfmt: ключи ролей, единица эпохи, шкала уровней, колонки.
    // keys.format держится равным kind.
    KeyedDefinition keys;
    Origin origin = Origin::Undecided;

    static LineFormat plain(Origin origin = Origin::Auto);
    static LineFormat structured(Kind kind, KeyedDefinition keys = {},
                                 Origin origin = Origin::Auto);

    bool isStructured() const noexcept { return kind != Kind::Plain; }
    bool isDecided() const noexcept { return origin != Origin::Undecided; }
    bool isUser() const noexcept { return origin == Origin::User; }
    // Читает строки так же, как Plain без дополнения (и «не решено»).
    bool readsLikePlain() const noexcept { return kind == Kind::Plain && !levelStartsRecord; }
    // Всё, что формат решает при ЧТЕНИИ (момент, уровень, граница записи):
    // вид, дополнение, ключи ролей, единица и шкала. Без колонок и Origin.
    // Одинаковая подпись — перечитывать не нужно.
    QString readingSignature() const;

    // «Plain text», «JSON Lines», «logfmt» — подпись индикатора.
    QString displayName() const;
    static QString kindName(Kind kind);   // «plain», «json», «logfmt» — JSON

    // Определение колонок полей структурированного формата для раскладки
    // вкладки (FieldLayout::schemaFor узнаёт его по префиксу): KeyedExtractor
    // по keys. У Plain — пусто.
    QString fieldDefinition() const;
    static bool isFieldDefinition(const QString& definition);
    static bool keysFromFieldDefinition(const QString& definition, KeyedDefinition* keys);

    // Сохранение — только выбор пользователя (TabSource, память файла,
    // сессия). fromJson: false — битые данные.
    QJsonObject toJson() const;
    static bool fromJson(const QJsonObject& json, LineFormat* out);

    bool operator==(const LineFormat& o) const
    {
        return kind == o.kind && levelStartsRecord == o.levelStartsRecord && origin == o.origin
            && (kind == Kind::Plain || keys == o.keys);
    }
    bool operator!=(const LineFormat& o) const { return !(*this == o); }
};

Q_DECLARE_METATYPE(LineFormat)

// Разбор строк формата на проход загрузчика: Lookup ключей ролей строится
// один раз (LineClassifier::classifyLine).
struct LineFormatReader {
    LineFormat format;
    KeyedLine::Lookup lookup;   ///< Только у Json / Logfmt.

    LineFormatReader() = default;
    explicit LineFormatReader(const LineFormat& f);
};

namespace LineFormatDetector {

// Меньше kMinLines непустых строк во всём файле — «не решено».
constexpr int kMinLines = 20;
// Сколько непустых строк начала смотрит решение и сколько байт читается.
constexpr int kSampleLines = 200;
constexpr qint64 kSampleBytes = 256 * 1024;

// Решение по первым строкам файла. endOfFile — строки кончились концом
// файла (а не пределом выборки): тогда при < kMinLines непустых — «не
// решено» (kind Plain, origin Undecided). Иначе Auto:
//   • JSON-объект или logfmt у большинства непустых строк (PatternHeuristics::
//     detectLineFormat) — Json / Logfmt, ключи — suggestKeyedDefinition;
//   • иначе Plain; levelStartsRecord — меток меньше 10 % непустых строк, а
//     ведущим словом уровня начинается не меньше половины.
LineFormat decide(const QStringList& lines, bool endOfFile);

// Первые строки файла для decide: BOM (UTF-8 пропускается, UTF-16/32 —
// декодируются), gzip по сигнатуре — распакованные, коды ANSI сняты,
// байты — как UTF-8 (решение смотрит только на ASCII-структуру; ключи с
// не-ASCII именами — в UTF-8). endOfFile — см. decide.
QStringList sampleFile(const QString& path, bool* endOfFile,
                       const std::atomic_bool* abort = nullptr);
QStringList sampleDevice(QIODevice& device, bool* endOfFile,
                         const std::atomic_bool* abort = nullptr);
LineFormat detectInFile(const QString& path, const std::atomic_bool* abort = nullptr);

// Одно место правила для LogParser и LogIndexer в начале прохода: выбор
// пользователя и решённый формат не трогаются; «не решено» — решается по
// файлу (detectInFile). true — format изменился (сообщить сигналом до
// первой порции).
bool advance(LineFormat& format, const QString& path, const std::atomic_bool* abort = nullptr);

} // namespace LineFormatDetector

#endif // LINEFORMAT_H
