#ifndef KEYEDLINE_H
#define KEYEDLINE_H

#include <QString>
#include <QStringList>
#include <QStringView>
#include <QVarLengthArray>
#include <QVector>

// ============================================================================
// Ключи JSON Lines и logfmt (F-4, docs/design/F-4.md; с F-17, шаг 7 — формат
// строк файла, lineformat.h, а не схема): строка лога — JSON-объект или пары
// logfmt, поля берутся по ИМЕНАМ ключей, а не по позициям.
//
// KeyedDefinition — ключи ролей, колонки, единица эпохи, шкала уровней
// формата (LineFormat::keys); загрузчик классифицирует строку по ним
// (LineClassifier::classifyLine), поля — KeyedExtractor. Колонки: Timestamp,
// Level, Message, затем extraColumns по именам ключей.
//
// KeyedLine — разбор строки без аллокаций и без регекса: PairReader
// отдаёт пары верхнего уровня (диапазон ключа, диапазон значения, вид
// значения, «экранировано»), KeyedLookup ищет колонки. Вложенный объект
// или массив пропускается счётом скобок с учётом строк, без рекурсии;
// ключ роли ищется сначала дословно («log.level»), затем как путь во
// вложенные объекты («log» → «level»). Повторный ключ — берётся первый,
// значение null — поля нет.
//
// «Распознана» (строка начинает запись — LineClassifier::isPrimaryLine):
//   • JSON: «{» в позиции 0 и хотя бы одна полная пара с ключом роли
//     (время, уровень, сообщение). Разбор терпимый: оборванный объект
//     (хвост растущего файла, строка, обрезанная писателем) отдаёт пары до
//     обрыва;
//   • logfmt: с позиции 0 идёт пара key=value, и среди пар есть ключ роли.
//     «Caused by: x=1» не распознаётся — первый токен не пара.
// Отступ в начале строки — не распознана (тело многострочного JSON и текст
// исключения остаются продолжениями, F-12). Правило смотрит только на
// ASCII-структуру — от кодировки файла не зависит.
//
// Значение в кавычках с обратной косой — в экранированной форме, как в
// файле; читателю его раскодирует unescape (JSON: \" \\ \/ \b \f \n \r \t
// \uXXXX с суррогатными парами; logfmt пишет подмножество того же).
// ============================================================================

struct KeyedDefinition {
    enum class Format { JsonObject, Logfmt };
    // Единица эпохи числом. Auto — по величине |целой части|: < 1e11 —
    // секунды (дробь — миллисекунды), < 1e14 — мс, < 1e17 — мкс, иначе нс.
    enum class EpochUnit { Auto, Seconds, Millis, Micros, Nanos };
    // Числовой уровень. Auto: 0–7 — syslog, от 10 — pino / Bunyan по
    // десяткам; Python — явной настройкой (10 Debug … 50 Critical).
    enum class NumericLevels { Auto, Syslog, Python };

    Format        format = Format::JsonObject;
    QStringList   timeKeys    = defaultTimeKeys();
    QStringList   levelKeys   = defaultLevelKeys();
    QStringList   messageKeys = defaultMessageKeys();
    QStringList   extraColumns;          ///< Ключи, ставшие колонками, в порядке колонок.
    EpochUnit     epochUnit = EpochUnit::Auto;
    NumericLevels numericLevels = NumericLevels::Auto;

    static QStringList defaultTimeKeys();
    static QStringList defaultLevelKeys();
    static QStringList defaultMessageKeys();

    // Колонки ролей — те же имена, что у встроенных регексных схем: правило
    // фильтра «Level contains ERROR» и выбор колонок переносятся между ними.
    static constexpr int kTimestampColumn = 0;
    static constexpr int kLevelColumn = 1;
    static constexpr int kMessageColumn = 2;
    static constexpr int kRoleColumns = 3;
    QStringList columnNames() const;

    bool operator==(const KeyedDefinition& o) const
    {
        return format == o.format && timeKeys == o.timeKeys && levelKeys == o.levelKeys
            && messageKeys == o.messageKeys && extraColumns == o.extraColumns
            && epochUnit == o.epochUnit && numericLevels == o.numericLevels;
    }
    bool operator!=(const KeyedDefinition& o) const { return !(*this == o); }

    // Имена в файле схемы («json», «logfmt», «auto», «ms», …).
    static QString formatName(Format f);
    static bool formatFromName(const QString& name, Format* f);
    static QString epochUnitName(EpochUnit u);
    static bool epochUnitFromName(const QString& name, EpochUnit* u);
    static QString numericLevelsName(NumericLevels n);
    static bool numericLevelsFromName(const QString& name, NumericLevels* n);
};

namespace KeyedLine {

enum class ValueKind : quint8 {
    String,   ///< В кавычках; диапазон — без кавычек.
    Number,   ///< Голый токен: JSON — начинается с цифры или «-»; logfmt — весь токен вида [-]цифры[.цифры].
    Literal,  ///< true / false и прочие голые токены (logfmt: любое голое значение не-число, «2026-11-18T…» тоже).
    Null,     ///< null — поля нет.
    Object,   ///< {…} целиком, текст как есть.
    Array,    ///< […] целиком, текст как есть.
};

struct Pair {
    int keyStart = 0, keyLength = 0;
    int valueStart = 0, valueLength = 0;
    bool keyEscaped = false;
    bool valueEscaped = false;   ///< Строка в кавычках с обратной косой внутри.
    ValueKind kind = ValueKind::Literal;
};

// Пары верхнего уровня строки по порядку. Без аллокаций: диапазоны — в
// тексте, переданном в конструктор (вложенный объект читается тем же
// классом по диапазону своего значения).
class PairReader {
public:
    PairReader(QStringView text, KeyedDefinition::Format format);
    // Строка имеет форму формата: JSON — «{» в позиции 0, logfmt — первый
    // токен пара key=value. Без формы next() ничего не отдаёт.
    bool structured() const { return m_structured; }
    // Следующая ПОЛНАЯ пара; false — пар больше нет (конец, обрыв, мусор).
    bool next(Pair* pair);

private:
    bool nextJson(Pair* pair);
    bool nextLogfmt(Pair* pair);

    QStringView m_text;
    qsizetype m_pos = 0;
    qsizetype m_firstKeyEnd = -1;   ///< logfmt: конец первого ключа, найденный конструктором.
    KeyedDefinition::Format m_format;
    bool m_structured = false;
    bool m_done = false;
};

// Раскодировать значение (или ключ) с обратной косой. keepControls — для
// однострочного текста view: \n, \r, \t и прочие управляющие символы
// остаются escape-последовательностью, как в файле. Неизвестная
// последовательность и битая \u остаются как есть.
QString unescape(QStringView escaped, bool keepControls = false);

// Длина того, что вернул бы unescape, без построения строки.
qsizetype unescapedSize(QStringView escaped, bool keepControls = false);

// Значение пары как текст: строка — раскодированная, объект и массив — их
// текст, число и литерал — токен.
QString valueText(QStringView text, const Pair& pair);

// Все пары верхнего уровня строки как текст (раздел Keys Entry Details):
// ключ — раскодированный, значение — valueText. Строка не той формы — пусто.
QVector<std::pair<QString, QString>> keyValuePairs(QStringView line, KeyedDefinition::Format format);

// Найденная колонка: значение в тексте строки (абсолютное смещение —
// вложенные значения тоже).
struct Hit {
    int start = -1, length = 0;
    int rank = 0;   ///< Чем меньше, тем главнее: кандидат * 2 + (путь ? 1 : 0).
    bool escaped = false;
    ValueKind kind = ValueKind::Literal;
    bool isValid() const { return start >= 0; }
    QStringView view(QStringView line) const { return isValid() ? line.mid(start, length) : QStringView(); }
};

using Hits = QVarLengthArray<Hit, 8>;

// Колонки ключей, разобранные один раз (LineFormatReader, KeyedExtractor):
// у каждой — ключи-кандидаты по приоритету, ключ с точкой — ещё и путь.
class Lookup {
public:
    Lookup() = default;
    explicit Lookup(const KeyedDefinition& definition);

    KeyedDefinition::Format format() const { return m_format; }
    int columnCount() const { return m_columnCount; }

    // Пары строки → найденные колонки (hits размером columnCount()).
    // true — строка распознана (см. блок в начале файла).
    //
    // neededColumns — сколько первых колонок нужно вызывающему (0 и -1 — все).
    // Разбор строки кончается, когда у каждой из них найден первый
    // кандидат: классификации на загрузке (LineClassifier::classifyLine без
    // полей — индексатор) хватает Timestamp и Level, и ключи после них не
    // читаются. Ответ «распознана» от этого не меняется (найденная колонка
    // роли — уже «да»), колонки за neededColumns в hits — неполные.
    bool scan(QStringView line, Hits* hits, int neededColumns = -1) const;

private:
    // Кандидат колонки: ключ дословно либо путь (key — первый сегмент).
    // Лежат корзинами по длине ключа: на пару строки — сравнения только с
    // кандидатами той же длины (обычно ни одного или один).
    struct Entry {
        QString key;
        QStringList path;   ///< Путь целиком; пусто — дословный ключ.
        int column = 0;
        int rank = 0;       ///< Номер кандидата в списке * 2 + (путь ? 1 : 0).
    };
    static constexpr int kMaxBucketedKey = 48;   ///< Длиннее — в последней корзине.
    // Первые символы ключей корзины: ASCII — битами, остальные — одним флагом.
    struct FirstChars {
        quint64 bits[2] = {0, 0};
        bool nonAscii = false;
    };
    // settled — счётчик колонок, получивших первого кандидата (rank 0).
    // Считаются только колонки < neededColumns.
    void consider(QStringView line, QStringView key, const Pair& pair, Hits* hits,
                  int neededColumns, int* settled) const;

    KeyedDefinition::Format m_format = KeyedDefinition::Format::JsonObject;
    int m_columnCount = 0;
    QVector<QVector<Entry>> m_byLength;
    QVector<FirstChars> m_firstChars;   ///< По корзинам m_byLength.
};

} // namespace KeyedLine

#endif // KEYEDLINE_H
