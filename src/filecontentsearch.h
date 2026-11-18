#ifndef FILECONTENTSEARCH_H
#define FILECONTENTSEARCH_H

#include "textencoding.h"

#include <QRegularExpression>
#include <QString>
#include <QStringView>

#include <atomic>
#include <functional>

// ============================================================================
// FileContentSearch — «есть ли в файле строка, подходящая под запрос»: фильтр
// сканера каталога по содержимому (DirectoryScanner), без виджетов.
//
// Кодировка — то же правило, что у вкладки (F-1, шаг 5):
//   • BOM → декодер по BOM (UTF-8 — TextDecoder, UTF-16/32 — QStringDecoder);
//   • иначе EncodingDetector::advance по каждому блоку чтения (окно 64 КБ от
//     первого байта ≥ 0x80, предварительное решение и его пересмотр) и
//     TextDecoder решённой кодировки. Запасная кодировка приходит ЗНАЧЕНИЕМ
//     (AppSettings — не ядро).
// Результат — тот, что дал бы файл, сразу прочитанный ИТОГОВОЙ кодировкой.
// Пока решение предварительное, находка в строке с байтами ≥ 0x80 —
// предварительная: такие строки (их меньше kFinalHighBytes — в каждой хотя бы
// один такой байт) запоминаются сырыми байтами и при пересмотре проверяются
// заново новым декодером. Строки из одного ASCII одинаковы в любой кодировке —
// находка в них окончательна сразу. Файл решение не читает дважды: окно — как
// у индексатора, вторым дескриптором только то, что за блоком.
//
// Блоки режутся по байту '\n' (все кодировки объёма ASCII-совместимы), поэтому
// многобайтный символ на границе блока не разрывается, а совпадение не может
// пересечь границу: запрос из поля ввода перевода строки не содержит. Строка
// длиннее kMaxLineBytes (бинарный файл без '\n') проверяется кусками; у
// подстроки куски перекрываются на длину запроса.
//
// Отмена — флаг между блоками; прогресс — сырые байты, прочитанные впервые.
// Функции без состояния: каждый вызов — свой файл, свой поток.
// ============================================================================
class ContentQuery
{
public:
    ContentQuery() = default;
    ContentQuery(const QString& text, bool isRegex, Qt::CaseSensitivity cs);

    bool isRegex() const { return m_isRegex; }
    const QString& text() const { return m_text; }
    // Пустой запрос или невалидный регекс — ни один файл не подходит.
    bool isUsable() const;

    // text — одна или несколько ЦЕЛЫХ строк, разделённых '\n' (последняя —
    // без него). Подстрока ищется во всём куске (перевода строки в ней нет);
    // регекс — по строкам, без завершающего '\r'.
    bool matchesLines(QStringView text) const;

private:
    QString             m_text;
    bool                m_isRegex = false;
    Qt::CaseSensitivity m_cs = Qt::CaseInsensitive;
    QRegularExpression  m_re;
};

namespace FileContentSearch {

// Размер блока чтения. Достаточно велик, чтобы окупить системные вызовы,
// и мал, чтобы прогресс шёл плавно и параллельные воркеры не копили память.
constexpr qint64 kReadBlockBytes = 256 * 1024;
// «Строка» длиннее этого проверяется кусками, чтобы файл без '\n' не растил
// буфер без предела.
constexpr qsizetype kMaxLineBytes = 4 * 1024 * 1024;
// Предел памяти под предварительные не-ASCII строки. Строк меньше
// kFinalHighBytes, поэтому он достигается только строками в мегабайты
// (бинарный мусор); тогда пересмотр перечитывает файл с начала.
constexpr qsizetype kRetainedLimitBytes = 16 * 1024 * 1024;

struct Options {
    TextEncoding fallback = TextEncodings::systemFallback();
    qint64 blockBytes = kReadBlockBytes;          // меньше — только для тестов
    qsizetype maxLineBytes = kMaxLineBytes;       // меньше — только для тестов
    // Сколько байтов предварительных не-ASCII строк держать для пересмотра;
    // больше — пересмотр перечитывает файл с начала. Меньше — только тесты.
    qsizetype retainedLimitBytes = kRetainedLimitBytes;
    const std::atomic<bool>* cancelled = nullptr; // nullptr — без отмены
    std::atomic<qint64>* bytesDone = nullptr;     // nullptr — без прогресса
    // Только для тестов: зовётся в потоке поиска после чтения каждого блока
    // (end — смещение конца блока) — детерминированная отмена посреди файла.
    std::function<void(qint64 end)> afterBlock;
};

struct Result {
    bool matched = false;
    // Ответ зависел от запасной кодировки: файл решён автоматикой в
    // однобайтную кодировку. Смена запасной перепроверяет только такие файлы.
    bool dependsOnFallback = false;
    bool cancelled = false;
    bool readable = true;
    // Байтов файла, учтённых в bytesDone (каждый — один раз): вызывающий
    // доливает прогресс до размера файла, не глядя на общий счётчик.
    qint64 bytesRead = 0;
    // С чем закончили (для тестов и диагностики): кодировка, которой читался
    // файл, и состояние автоматики (у файла с BOM UTF-16/32 — пустое).
    TextEncoding usedEncoding = TextEncoding::Utf8;
    FileEncoding encoding;
};

Result search(const QString& path, const ContentQuery& query, const Options& options = {});

} // namespace FileContentSearch

#endif // FILECONTENTSEARCH_H
