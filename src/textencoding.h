#ifndef TEXTENCODING_H
#define TEXTENCODING_H

#include <QByteArrayView>
#include <QString>
#include <QMetaType>
#include <QStringConverter>

#include <optional>
#include <type_traits>

// ============================================================================
// Кодировки файлов (F-1, docs/design/F-1.md): свой декодер с таблицами и
// автоопределение по байтам. Модуль ядра без состояния и без Qt Widgets.
//
//   • TextEncoding — что умеем показывать. Читаются этим модулем UTF-8 и
//     однобайтные ASCII-совместимые кодировки (Windows-1251, IBM866, KOI8-R,
//     Windows-1252); UTF-16/32 — значения только для показа (их читает
//     QTextStream / QStringDecoder резидентного пути, вручную не выбираются).
//   • TextDecoder — тривиально копируемое значение (указатель на таблицу из
//     128 char16_t либо «UTF-8»). Переносится в воркер копией, как схема полей;
//     общего состояния у копий нет, поэтому декодировать можно из любого потока.
//   • EncodingDetector — чистые функции: где первый байт ≥ 0x80 и какую
//     кодировку выбрать по окну байтов.
//
// Ловушки:
//   • Все кодировки объёма ASCII-совместимы: байты 0x00–0x7F — те же символы,
//     0x0A/0x0D — только перевод строки. На этом стоят разрез по '\n' в
//     сырых байтах (инвариант 5) и независимость индекса от кодировки.
//   • Байты, которых нет в таблице стандарта (0x98 в Windows-1251; 0x81, 0x8D,
//     0x8F, 0x90, 0x9D в Windows-1252), декодируются в управляющие символы
//     C1 с тем же номером (U+0098, …), как в индексах WHATWG и в
//     MultiByteToWideChar: декодер тотален, байт не теряется и не становится
//     U+FFFD.
//   • decode() для UTF-8 — ровно QString::fromUtf8, как до модуля: текст
//     UTF-8-файлов не меняется ни на символ.
// ============================================================================

enum class TextEncoding : quint8 {
    Utf8,
    Windows1251,
    Ibm866,
    Koi8R,
    Windows1252,
    // Только для показа: такие файлы читает резидентный путь по BOM.
    Utf16LE,
    Utf16BE,
    Utf32LE,
    Utf32BE,
};

namespace TextEncodings {

// Однобайтная табличная кодировка (Windows-1251, IBM866, KOI8-R, Windows-1252).
bool isSingleByte(TextEncoding encoding);
// Кириллическая однобайтная: между ними выбирает EncodingDetector::decide.
bool isCyrillic(TextEncoding encoding);
// Читается TextDecoder: UTF-8 или однобайтная.
bool isDecodable(TextEncoding encoding);

// Имя для интерфейса и командной строки: "UTF-8", "Windows-1251", "IBM866",
// "KOI8-R", "Windows-1252", "UTF-16LE", …
QString displayName(TextEncoding encoding);
// Обратно, без учёта регистра; понимает и распространённые синонимы
// (cp1251, cp866, koi8r, latin1 не входит — это не Windows-1252). Только
// декодируемые кодировки: UTF-16/32 вручную не выбираются.
std::optional<TextEncoding> fromName(QStringView name);

// Мост к QStringConverter для резидентного пути и сканера (Q-8, Q-15 держат
// кодировку хвоста как QStringConverter::Encoding). Однобайтных там нет —
// nullopt; обобщённые Utf16/Utf32 (порядок машины) сводятся к LE/BE.
std::optional<TextEncoding> fromQt(QStringConverter::Encoding encoding);
std::optional<QStringConverter::Encoding> toQt(TextEncoding encoding);

// BOM в начале данных: кодировка и длина BOM в байтах (3 у UTF-8, 2 у UTF-16,
// 4 у UTF-32; UTF-32LE проверяется раньше UTF-16LE — их BOM начинаются
// одинаково). nullopt — BOM нет.
struct Bom {
    TextEncoding encoding;
    int length;
};
std::optional<Bom> detectBom(QByteArrayView head);

// Запасная кодировка для файлов не в UTF-8 по локали системы: кириллическая
// письменность — Windows-1251, иначе Windows-1252. Пока нет настройки
// (шаг 4 F-1), это и есть умолчание индексатора.
TextEncoding systemFallback();

} // namespace TextEncodings

class TextDecoder
{
public:
    // По умолчанию — UTF-8.
    constexpr TextDecoder() = default;
    // Для недекодируемой кодировки (UTF-16/32) — UTF-8 с Q_ASSERT в отладке:
    // такие файлы этим декодером читаться не должны.
    explicit TextDecoder(TextEncoding encoding);

    TextEncoding encoding() const { return m_encoding; }
    bool isUtf8() const { return m_table == nullptr; }

    // Байты строки (без EOL) → текст. Ветка UTF-8 — в заголовке: это горячий
    // путь индексатора и сканов, вызов на строку не должен стоить лишнего.
    QString decode(const char* data, qsizetype len) const
    {
        return m_table ? decodeSingleByte(data, len) : QString::fromUtf8(data, len);
    }
    QString decode(QByteArrayView bytes) const { return decode(bytes.data(), bytes.size()); }

    // Строка, прочитанная как Latin-1 (байт = символ U+0000–U+00FF, так её
    // отдаёт QTextStream с кодировкой Latin1), → текст однобайтной кодировки
    // на месте: символы ≥ U+0080 заменяются по таблице. Для UTF-8 ничего не
    // делает (такую строку декодирует decode(toLatin1)). Символы выше U+00FF
    // не трогает: их в Latin-1-строке нет.
    void remapLatin1(QString& text) const;

    friend bool operator==(const TextDecoder& a, const TextDecoder& b)
    {
        return a.m_encoding == b.m_encoding;
    }
    friend bool operator!=(const TextDecoder& a, const TextDecoder& b) { return !(a == b); }

private:
    QString decodeSingleByte(const char* data, qsizetype len) const;

    // Таблица байтов 0x80–0xFF; nullptr — UTF-8.
    const char16_t* m_table = nullptr;
    TextEncoding m_encoding = TextEncoding::Utf8;
};

static_assert(std::is_trivially_copyable_v<TextDecoder>,
              "TextDecoder is copied into worker snapshots");

// Кодировка файла вкладки и откуда она взялась (F-1, шаг 2). Пока source ==
// Undecided, в файле встречались только ASCII-байты: текст одинаков в любой
// кодировке, decoder — UTF-8, и индексатор проверяет каждый новый буфер на
// байт ≥ 0x80. Состояние живёт в IndexedLogStore на файл и переживает
// дозапись хвоста; переписанный файл начинает с Undecided (кроме выбора
// пользователя).
struct FileEncoding {
    enum class Source : quint8 {
        Undecided, // только ASCII — решать нечего
        Auto,      // EncodingDetector::decide по окну
        Bom,       // BOM UTF-8
        User,      // выбор пользователя; автоматика его не меняет
    };
    TextDecoder decoder;
    Source source = Source::Undecided;

    bool decided() const { return source != Source::Undecided; }
};

Q_DECLARE_METATYPE(FileEncoding)

namespace EncodingDetector {

// Окно решения: от начала первой строки с байтом ≥ 0x80 до стольких байт
// вперёд (или до конца файла).
constexpr qsizetype kWindowBytes = 64 * 1024;

// Смещение первого байта ≥ 0x80 в [data, data + len) или -1, если все ASCII.
qsizetype firstHighByte(const char* data, qsizetype len);
inline qsizetype firstHighByte(QByteArrayView bytes)
{
    return firstHighByte(bytes.data(), bytes.size());
}

// Счёт по окну. validUtf8 — полные корректные многобайтные последовательности
// UTF-8 (без overlong, суррогатов и кодов выше U+10FFFF); invalidBytes —
// байты ≥ 0x80, не вошедшие в такую последовательность. Последовательность,
// оборванная краем окна (все её байты пока корректны), не считается вовсе.
struct Utf8Counts {
    qsizetype validUtf8 = 0;
    qsizetype invalidBytes = 0;
};
Utf8Counts countUtf8(QByteArrayView window);

// Решение по окну. Валидных последовательностей больше, чем невалидных
// байтов, — UTF-8; иначе запасная кодировка fallback, а если она
// кириллическая — та из Windows-1251 / IBM866 / KOI8-R, в которой байты окна
// больше похожи на русский текст (частоты букв, регистр, доля не-букв).
// nullopt — решать нечего: в окне нет ни одного учтённого байта ≥ 0x80
// (только ASCII либо единственная последовательность оборвана краем окна).
// fallback — однобайтная кодировка; иная трактуется как Windows-1252.
std::optional<TextEncoding> decide(QByteArrayView window, TextEncoding fallback);

// Только выбор между кириллическими кодировками, без проверки UTF-8 —
// открыт для тестов и замеров точности.
TextEncoding guessCyrillic(QByteArrayView window);

} // namespace EncodingDetector

#endif // TEXTENCODING_H
