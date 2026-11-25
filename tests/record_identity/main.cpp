// ============================================================================
// record_identity — сохраняемые идентификаторы R-1 (шаг 2, docs/design/R-1.md):
//
//   • SourceIdentity: снятие и проверка (Valid / Changed / Missing /
//     Ephemeral) на дозаписи, усечении, переписывании той же длины,
//     переименовании, файле короче 4 КБ, пустом, из одного BOM, с CRLF, спуле
//     stdin; сравнение содержимого без пути;
//   • отпечаток строки: одинаков на обоих бэкендах для каждой строки фикстур
//     (UTF-8, CRLF, Windows-1251, IBM866, KOI8-R, Windows-1252, битые байты,
//     пустые строки, строка длиннее 64 КБ, слитая вкладка, UTF-16), совпадает
//     с отпечатком сырых байтов строки, не меняется от кодировки, которой
//     прочитан файл (setFileEncoding на индексном, выбор пользователя на
//     резидентном), различает строки с разным ASCII-текстом;
//   • якорь дочитывания FileChangeDetector (Q-19): таблица случаев —
//     не изменился, дописан, усечён, переписано начало той же длины и с
//     ростом, переписан хвост, правка в середине, файл короче 4 КБ, пустой,
//     исчез; согласованность с SourceIdentity на тех же случаях (Valid ⇔ не
//     Replaced, те же отпечатки) и цена проверки (открытия и чтения);
//   • JSON: круговой проход, отказ на чужой версии, алгоритме и битых данных;
//   • сохранённые пути (R-8, SavedPath): разбор одинаков на любой ОС — путь
//     Windows с диском, UNC, «\\?\», POSIX абсолютны; нормализация,
//     сравнение, показ; SourceIdentity с путём другой ОС читается и не
//     совпадает с локальным файлом «<текущий каталог>/C:/…».
// ============================================================================

#include "filechangedetector.h"
#include "recordid.h"
#include "recordlocator.h"
#include "stdinspooler.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QStringDecoder>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cstdio>

using namespace testdocs;

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

using State = SourceIdentity::State;

static const char* stateName(State s)
{
    switch (s) {
    case State::Valid:     return "Valid";
    case State::Changed:   return "Changed";
    case State::Missing:   return "Missing";
    case State::Ephemeral: return "Ephemeral";
    }
    return "?";
}

static void expectState(const SourceIdentity& id, State expected, const QString& what,
                        const QString& path = QString())
{
    const State got = path.isEmpty() ? id.check() : id.checkAt(path);
    CHECK(got == expected, QStringLiteral("%1: ожидалось %2, получено %3")
                               .arg(what, QLatin1String(stateName(expected)),
                                    QLatin1String(stateName(got))));
}

static bool rewrite(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

static QByteArray filler(int bytes, char seed)
{
    QByteArray out;
    int n = 0;
    while (out.size() < bytes)
        out += "2026-03-05 10:00:00.000 [w] INFO - line " + QByteArray::number(n++) + ' ' + seed + '\n';
    out.truncate(bytes - 1);
    out += '\n';
    return out;
}

// ---------------------------------------------------------------------------
// SourceIdentity
// ---------------------------------------------------------------------------

static void testSourceIdentity(const QDir& dir)
{
    // Большой файл: начало и конец — разные области.
    const QByteArray big = filler(20000, 'a');
    const QString path = writeFile(dir, QStringLiteral("big.log"), big);
    const std::optional<SourceIdentity> id = SourceIdentity::capture(path);
    CHECK(id && id->isValid() && id->size == big.size() && !id->ephemeral, "снятие большого файла");
    if (!id)
        return;
    CHECK(id->head != id->tail, "у файла длиннее 4 КБ начало и конец — разные отпечатки");
    expectState(*id, State::Valid, "тот же файл");

    // Дозапись (вырос между снятием и проверкой).
    CHECK(appendToFile(path, "2026-03-05 10:00:09.000 [w] INFO - appended\n"), "дозапись");
    expectState(*id, State::Valid, "дозапись");
    // Хвост без '\n' — тоже дозапись.
    CHECK(appendToFile(path, "partial"), "дозапись без перевода строки");
    expectState(*id, State::Valid, "дозапись без перевода строки");

    // Правка в середине, мимо обеих областей: источник Valid — правку ловит
    // отпечаток строки (вторая линия защиты).
    {
        QByteArray mid = big;
        mid[10000] = mid[10000] == 'X' ? 'Y' : 'X';
        CHECK(rewrite(path, mid), "правка в середине");
        expectState(*id, State::Valid, "правка в середине, вне областей отпечатка");
    }
    // Переписан той же длиной: другое начало.
    {
        QByteArray other = big;
        other[5] = other[5] == 'X' ? 'Y' : 'X';
        CHECK(rewrite(path, other), "переписан той же длиной (начало)");
        expectState(*id, State::Changed, "переписан той же длиной, другое начало");
    }
    // Переписан той же длиной: другой конец сохранённого префикса.
    {
        QByteArray other = big;
        other[big.size() - 10] = 'Z';
        CHECK(rewrite(path, other), "переписан той же длиной (конец)");
        expectState(*id, State::Changed, "переписан той же длиной, другой конец");
    }
    // Переписан той же длиной и тем же содержимым — тот же источник.
    CHECK(rewrite(path, big), "переписан тем же содержимым");
    expectState(*id, State::Valid, "переписан тем же содержимым");
    // Переписан длиннее с другим содержимым — Changed, а не дозапись.
    CHECK(rewrite(path, filler(30000, 'b')), "переписан длиннее");
    expectState(*id, State::Changed, "переписан длиннее другим содержимым");
    // Усечение.
    CHECK(rewrite(path, big.left(big.size() - 1)), "усечение");
    expectState(*id, State::Changed, "усечён на один байт");
    CHECK(rewrite(path, QByteArray()), "усечение до нуля");
    expectState(*id, State::Changed, "усечён до нуля");

    // Удалён — Missing; каталог по этому пути — тоже Missing.
    QFile::remove(path);
    expectState(*id, State::Missing, "файл удалён");
    QDir().mkpath(path);
    expectState(*id, State::Missing, "по пути каталог");
    QDir().rmdir(path);

    // Переименование: путь другой, содержимое то же.
    const QString before = writeFile(dir, QStringLiteral("rotate.log"), big);
    const std::optional<SourceIdentity> rotated = SourceIdentity::capture(before);
    CHECK(rotated.has_value(), "снятие до ротации");
    const QString after = dir.filePath(QStringLiteral("rotate.log.1"));
    CHECK(QFile::rename(before, after), "переименование");
    if (rotated) {
        expectState(*rotated, State::Missing, "по старому пути после переименования");
        expectState(*rotated, State::Valid, "по новому пути (checkAt)", after);
        const std::optional<SourceIdentity> moved = SourceIdentity::capture(after);
        CHECK(moved && moved->sameContent(*rotated) && moved->path != rotated->path
                  && *moved != *rotated,
              "переименованный файл: то же содержимое, другой путь");
        // Новый файл с прежним именем — другое содержимое.
        writeFile(dir, QStringLiteral("rotate.log"), filler(5000, 'c'));
        expectState(*rotated, State::Changed, "новый файл с прежним именем");
        const std::optional<SourceIdentity> fresh = SourceIdentity::capture(before);
        CHECK(fresh && !fresh->sameContent(*rotated), "новый файл — другое содержимое");
        // Переименованный и дописанный: Valid по новому пути, но не sameContent.
        CHECK(appendToFile(after, "grown\n"), "дозапись переименованного");
        expectState(*rotated, State::Valid, "переименованный и дописанный", after);
    }

    // Снятие по префиксу: файл длиннее сохранённого размера.
    {
        const QString p = writeFile(dir, QStringLiteral("prefix.log"), big);
        const std::optional<SourceIdentity> prefix = SourceIdentity::capture(p, 8000);
        CHECK(prefix && prefix->size == 8000, "снятие по префиксу");
        const std::optional<SourceIdentity> whole = SourceIdentity::capture(p);
        if (prefix && whole) {
            CHECK(prefix->head == whole->head && prefix->tail != whole->tail,
                  "префикс: то же начало, свой конец");
            QByteArray edited = big;
            edited[9000] = 'Q'; // за сохранённым концом
            CHECK(rewrite(p, edited), "правка за префиксом");
            expectState(*prefix, State::Valid, "правка за сохранённым концом не задевает префикс");
            edited[7990] = 'Q';
            CHECK(rewrite(p, edited), "правка в префиксе");
            expectState(*prefix, State::Changed, "правка в последних 4 КБ префикса");
        }
        CHECK(!SourceIdentity::capture(p, big.size() + 1).has_value(),
              "снятие с размером больше файла — отказ");
        CHECK(!SourceIdentity::capture(dir.filePath(QStringLiteral("nope.log"))).has_value(),
              "снятие несуществующего файла — отказ");
    }
}

static void testSmallFiles(const QDir& dir)
{
    // Короче 4 КБ: начало и конец — одна область.
    const QByteArray small = "2026-03-05 10:00:00.000 [w] INFO - small\r\nsecond\r\n";
    const QString p = writeFile(dir, QStringLiteral("small.log"), small);
    const std::optional<SourceIdentity> id = SourceIdentity::capture(p);
    CHECK(id && id->size == small.size() && id->head == id->tail, "файл короче 4 КБ");
    if (id) {
        expectState(*id, State::Valid, "короткий файл");
        // Дорос за 4 КБ: начало считается по сохранённому размеру, а не 4 КБ.
        CHECK(appendToFile(p, filler(9000, 'd')), "короткий файл дорос");
        expectState(*id, State::Valid, "короткий файл дорос за 4 КБ");
        QByteArray changed = small;
        changed[3] = '7';
        CHECK(rewrite(p, changed), "короткий файл переписан");
        expectState(*id, State::Changed, "короткий файл переписан той же длиной");
        // CRLF → LF той же длины не бывает; CRLF → LF короче — Changed.
        QByteArray lf = small;
        lf.replace("\r\n", "\n");
        CHECK(rewrite(p, lf), "короткий файл в LF");
        expectState(*id, State::Changed, "CRLF переписан в LF");
    }

    // Пустой файл: Valid, пока есть; дописанный — тоже (префикс пуст).
    const QString empty = writeFile(dir, QStringLiteral("empty.log"), QByteArray());
    const std::optional<SourceIdentity> e = SourceIdentity::capture(empty);
    CHECK(e && e->size == 0, "пустой файл");
    if (e) {
        expectState(*e, State::Valid, "пустой файл");
        CHECK(appendToFile(empty, "line\n"), "пустой файл дописан");
        expectState(*e, State::Valid, "пустой файл дописан");
        QFile::remove(empty);
        expectState(*e, State::Missing, "пустой файл удалён");
    }

    // Файл из одного BOM и с BOM + CRLF.
    const QString bom = writeFile(dir, QStringLiteral("bom.log"), "\xEF\xBB\xBF");
    const std::optional<SourceIdentity> b = SourceIdentity::capture(bom);
    CHECK(b && b->size == 3, "файл из одного BOM");
    if (b) {
        expectState(*b, State::Valid, "файл из одного BOM");
        CHECK(appendToFile(bom, "2026-03-05 10:00:00.000 [w] INFO - x\r\n"), "BOM дописан");
        expectState(*b, State::Valid, "BOM-файл дописан");
        CHECK(rewrite(bom, "2026-03-05 10:00:00.000 [w] INFO - x\r\n"), "BOM снят");
        expectState(*b, State::Changed, "файл без BOM вместо файла с BOM");
        CHECK(rewrite(bom, "\xFF\xFE"), "BOM UTF-16");
        expectState(*b, State::Changed, "BOM UTF-16 вместо BOM UTF-8");
    }
}

// ---------------------------------------------------------------------------
// FileChangeDetector (Q-19): якорь дочитывания вкладки
// ---------------------------------------------------------------------------

using Change = FileChangeDetector::Change;

static const char* changeName(Change c)
{
    switch (c) {
    case Change::Unchanged: return "Unchanged";
    case Change::Appended:  return "Appended";
    case Change::Replaced:  return "Replaced";
    }
    return "?";
}

// mtime файла — явно: случаи таблицы не должны зависеть от того, успели ли
// часы файловой системы сдвинуться между записями.
static bool setMtime(const QString& path, qint64 ms)
{
    QFile f(path);
    return f.open(QIODevice::ReadWrite)
        && f.setFileTime(QDateTime::fromMSecsSinceEpoch(ms), QFileDevice::FileModificationTime);
}

static void testFileChangeDetector(const QDir& dir)
{
    const QByteArray big = filler(20000, 'a');     // начало и конец — разные области
    const QByteArray small = filler(1500, 's');    // короче 4 КБ: одна область

    struct Case {
        const char* name;
        QByteArray before;
        // Что стало с файлом; пустой optional — файл удалён.
        std::optional<QByteArray> after;
        Change expected;
    };
    const auto edited = [](QByteArray bytes, int at) {
        bytes[at] = bytes[at] == 'X' ? 'Y' : 'X';
        return bytes;
    };
    const QByteArray more = "2026-03-05 10:00:09.000 [w] INFO - appended\n";
    const QVector<Case> cases = {
        { "не изменился",                         big,   big,                          Change::Unchanged },
        { "дописан",                              big,   big + more,                   Change::Appended },
        { "дописан хвост без перевода строки",    big,   big + "partial",              Change::Appended },
        { "усечён на байт",                       big,   big.left(big.size() - 1),     Change::Replaced },
        { "усечён до нуля",                       big,   QByteArray(),                 Change::Replaced },
        { "переписано начало той же длины",       big,   edited(big, 5),               Change::Replaced },
        { "переписано начало, файл вырос",        big,   edited(big, 5) + more,        Change::Replaced },
        { "переписано начало за первым КБ",       big,   edited(big, 4000),            Change::Replaced },
        { "переписан хвост той же длины",         big,   edited(big, big.size() - 10), Change::Replaced },
        { "переписан хвост, файл вырос",          big,   edited(big, big.size() - 10) + more, Change::Replaced },
        { "переписан другим содержимым длиннее",  big,   filler(30000, 'b'),           Change::Replaced },
        // Предел (заголовок filechangedetector.h): правку только в середине
        // при тех же длине, начале и хвосте не видит ни якорь, ни SourceIdentity.
        { "правка в середине (не ловится)",       big,   edited(big, 10000),           Change::Unchanged },
        { "короче 4 КБ: не изменился",            small, small,                        Change::Unchanged },
        { "короче 4 КБ: дописан",                 small, small + more,                 Change::Appended },
        { "короче 4 КБ: дорос за 4 КБ",           small, small + filler(9000, 'd'),    Change::Appended },
        { "короче 4 КБ: переписано начало",       small, edited(small, 3),             Change::Replaced },
        { "короче 4 КБ: переписано начало с ростом", small, edited(small, 3) + more,   Change::Replaced },
        { "пустой: остался пустым",               QByteArray(), QByteArray(),          Change::Unchanged },
        { "пустой: дописан",                      QByteArray(), more,                  Change::Appended },
        { "исчез",                                big,   std::nullopt,                 Change::Replaced },
        { "пустой исчез",                         QByteArray(), std::nullopt,          Change::Replaced },
    };

    int n = 0;
    for (const Case& c : cases) {
        const QString what = QStringLiteral("детектор: %1").arg(QString::fromUtf8(c.name));
        const QString path = writeFile(dir, QStringLiteral("detector-%1.log").arg(n++), c.before);
        const FileChangeDetector::Anchor anchor = FileChangeDetector::capture(path, c.before.size());
        const std::optional<SourceIdentity> id = SourceIdentity::capture(path);
        CHECK(anchor.valid && id, what + ": снятие");
        if (!anchor.valid || !id)
            continue;
        // Те же области и тот же хеш, что у SourceIdentity.
        CHECK(anchor.head == id->head && anchor.tail == id->tail,
              what + ": отпечатки якоря и SourceIdentity совпадают");

        if (c.after) {
            CHECK(rewrite(path, *c.after), what + ": запись");
            // Другая mtime, как у любой настоящей записи.
            CHECK(setMtime(path, anchor.stampMtimeMs + 5000), what + ": mtime");
        } else {
            CHECK(QFile::remove(path), what + ": удаление");
        }
        const Change got = FileChangeDetector::classify(path, anchor);
        CHECK(got == c.expected, QStringLiteral("%1: ожидалось %2, получено %3")
                                     .arg(what, QLatin1String(changeName(c.expected)),
                                          QLatin1String(changeName(got))));
        // Согласованность: вкладка дочитывает (не Replaced) ⇔ закладки и
        // память файла считают источник тем же (Valid).
        const State state = id->check();
        CHECK((got != Change::Replaced) == (state == State::Valid),
              QStringLiteral("%1: детектор %2, SourceIdentity %3 — расходятся")
                  .arg(what, QLatin1String(changeName(got)), QLatin1String(stateName(state))));
    }

    // Якорь на префиксе (дочитывание остановилось раньше конца файла): правка
    // начала и правка за концом прочитанного.
    {
        const QString path = writeFile(dir, QStringLiteral("detector-prefix.log"), big);
        const auto anchor = FileChangeDetector::capture(path, 8000);
        CHECK(anchor.valid && anchor.consumedBytes == 8000 && anchor.stampSize == big.size(),
              "детектор: якорь на префиксе");
        CHECK(setMtime(path, anchor.stampMtimeMs + 1000), "детектор: префикс, mtime");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Appended,
              "детектор: за прочитанным есть байты — дозапись");
        CHECK(rewrite(path, edited(big, 9000)) && setMtime(path, anchor.stampMtimeMs + 2000),
              "детектор: правка за префиксом");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Appended,
              "детектор: правка за концом прочитанного — всё ещё дозапись");
        CHECK(rewrite(path, edited(big, 100)) && setMtime(path, anchor.stampMtimeMs + 3000),
              "детектор: правка начала");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Replaced,
              "детектор: префикс, переписано начало — Replaced");
        CHECK(FileChangeDetector::classify(path, FileChangeDetector::capture(path, big.size() + 1))
                  == Change::Replaced,
              "детектор: якорь длиннее файла недействителен — Replaced");
        CHECK(!FileChangeDetector::capture(dir.filePath(QStringLiteral("nope.log")), 0).valid,
              "детектор: якорь несуществующего файла недействителен");
    }

    // Цена проверки (check — как у вкладки): чтение начала — только когда
    // файл трогали или его mtime слишком свежая, чтобы за него ручаться.
    const qint64 longAgo = QDateTime::currentMSecsSinceEpoch() - 600000;
    {
        const QString path = writeFile(dir, QStringLiteral("detector-cost.log"), big);
        CHECK(setMtime(path, longAgo), "детектор, цена: старая mtime");
        auto anchor = FileChangeDetector::capture(path, big.size());
        CHECK(!anchor.stampRacy, "детектор, цена: старая mtime — не «racy»");
        const auto cost = [&](const char* what, Change expected, quint64 opens, quint64 reads) {
            const auto before = FileChangeDetector::ioCounts();
            const Change got = FileChangeDetector::check(path, anchor);
            const auto after = FileChangeDetector::ioCounts();
            CHECK(got == expected && after.opens - before.opens == opens
                      && after.reads - before.reads == reads,
                  QStringLiteral("детектор, цена: %1 — %2, открытий %3 (ждали %4), чтений %5 (ждали %6)")
                      .arg(QLatin1String(what), QLatin1String(changeName(got)))
                      .arg(after.opens - before.opens).arg(opens)
                      .arg(after.reads - before.reads).arg(reads));
        };
        // Не тронут: как до Q-19 — одно открытие, одно чтение (хвост).
        cost("не изменился", Change::Unchanged, 1, 1);
        cost("не изменился, второй тик", Change::Unchanged, 1, 1);
        // touch: mtime другой — начало читается один раз, перезагрузки нет;
        // проверенная подпись становится новой, следующий тик — снова одно чтение.
        CHECK(setMtime(path, longAgo + 1000), "детектор, цена: touch");
        cost("touch", Change::Unchanged, 1, 2);
        cost("тик после touch", Change::Unchanged, 1, 1);
        // Дописан: то же открытие, второе чтение — начало.
        CHECK(appendToFile(path, more), "детектор, цена: дозапись");
        cost("дописан", Change::Appended, 1, 2);
        const auto before = FileChangeDetector::ioCounts();
        FileChangeDetector::capture(path, big.size() + more.size());
        const auto after = FileChangeDetector::ioCounts();
        CHECK(after.opens - before.opens == 1 && after.reads - before.reads == 2,
              "детектор, цена: снятие якоря — одно открытие, два чтения");
        // Файл короче 4 КБ: начало и хвост — одно чтение даже при дозаписи.
        const QString tiny = writeFile(dir, QStringLiteral("detector-cost-small.log"), small);
        const auto smallAnchor = FileChangeDetector::capture(tiny, small.size());
        // Свежая mtime («racy»): начало читается на каждой проверке, пока
        // подпись не состарится; состарилась — одно чтение.
        {
            const QString fresh = writeFile(dir, QStringLiteral("detector-cost-racy.log"), big);
            auto racy = FileChangeDetector::capture(fresh, big.size());
            CHECK(racy.stampRacy, "детектор, цена: свежая mtime — «racy»");
            for (int tick = 0; tick < 2; ++tick) {
                const auto b = FileChangeDetector::ioCounts();
                CHECK(FileChangeDetector::check(fresh, racy) == Change::Unchanged,
                      "детектор, цена: racy, не изменился");
                const auto a = FileChangeDetector::ioCounts();
                CHECK(a.reads - b.reads == 2, "детектор, цена: racy — начало читается каждый тик");
            }
            CHECK(setMtime(fresh, longAgo), "детектор, цена: mtime состарилась");
            CHECK(FileChangeDetector::check(fresh, racy) == Change::Unchanged && !racy.stampRacy,
                  "детектор, цена: проверка состарившейся подписи снимает «racy»");
            const auto b = FileChangeDetector::ioCounts();
            CHECK(FileChangeDetector::check(fresh, racy) == Change::Unchanged,
                  "детектор, цена: после «racy» не изменился");
            const auto a = FileChangeDetector::ioCounts();
            CHECK(a.reads - b.reads == 1, "детектор, цена: после «racy» — одно чтение");
        }
        CHECK(appendToFile(tiny, more), "детектор, цена: дозапись короткого");
        const auto b2 = FileChangeDetector::ioCounts();
        CHECK(FileChangeDetector::classify(tiny, smallAnchor) == Change::Appended,
              "детектор, цена: короткий дописан");
        const auto a2 = FileChangeDetector::ioCounts();
        CHECK(a2.opens - b2.opens == 1 && a2.reads - b2.reads == 1,
              "детектор, цена: короткий файл — одно чтение");
    }

    // Предел, закреплённый явно (заголовок filechangedetector.h): начало
    // переписано той же длиной, а размер и давняя mtime — как при снятии (копия
    // с сохранением времени). Якорь начало не перечитывает — Unchanged, хотя
    // SourceIdentity видит Changed. Стоит mtime сдвинуться — Replaced.
    {
        const QString path = writeFile(dir, QStringLiteral("detector-same-stamp.log"), big);
        CHECK(setMtime(path, longAgo), "детектор: давняя mtime");
        const auto anchor = FileChangeDetector::capture(path, big.size());
        CHECK(rewrite(path, edited(big, 5)) && setMtime(path, anchor.stampMtimeMs),
              "детектор: перезапись с прежней mtime");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Unchanged,
              "детектор: перезапись начала при тех же размере и mtime не видна (предел)");
        CHECK(setMtime(path, anchor.stampMtimeMs + 1), "детектор: mtime сдвинута");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Replaced,
              "детектор: после сдвига mtime перезапись начала видна");
    }
    // Та же перезапись при СВЕЖЕЙ mtime (запись в тот же тик часов, что и
    // снятие якоря): подпись «racy», начало читается — Replaced.
    {
        const QString path = writeFile(dir, QStringLiteral("detector-same-tick.log"), big);
        const auto anchor = FileChangeDetector::capture(path, big.size());
        CHECK(rewrite(path, edited(big, 5)) && setMtime(path, anchor.stampMtimeMs),
              "детектор: перезапись в тот же тик");
        CHECK(FileChangeDetector::classify(path, anchor) == Change::Replaced,
              "детектор: перезапись начала в тот же тик часов видна (racy)");
    }
}

static void testEphemeral(const QDir& dir)
{
    const QString spool = StdinSpooler::spoolFilePathFor(QCoreApplication::applicationPid());
    CHECK(rewrite(spool, "streamed line\n"), "спул stdin создан");
    const std::optional<SourceIdentity> id = SourceIdentity::capture(spool);
    CHECK(id && id->ephemeral && id->isValid(), "спул stdin — Ephemeral");
    if (id) {
        expectState(*id, State::Ephemeral, "спул stdin");
        CHECK(id->toJson().isEmpty(), "спул stdin на диск не пишется");
        CHECK(!id->sameContent(*id), "Ephemeral не сравнивается по содержимому");
        CHECK(!SourceIdentity().sameContent(SourceIdentity()),
              "пустые (неснятые) идентичности не совпадают по содержимому");
    }
    QFile::remove(spool);
    if (id)
        expectState(*id, State::Ephemeral, "спул удалён — всё равно Ephemeral");
    // Файл с именем спула вне временного каталога — обычный источник
    // (StdinSpooler::isSpoolFilePath смотрит и на каталог).
    const QString lookalike = writeFile(dir, QStringLiteral("DendroLog-stdin-1.log"), "x\n");
    const std::optional<SourceIdentity> l = SourceIdentity::capture(lookalike);
    CHECK(l && !l->ephemeral, "файл с именем спула вне временного каталога — обычный");
    if (l)
        expectState(*l, State::Valid, "файл с именем спула вне временного каталога");
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

static QJsonObject parse(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

static void testJson(const QDir& dir)
{
    const QString path = writeFile(dir, QStringLiteral("json.log"), filler(6000, 'j'));
    const std::optional<SourceIdentity> id = SourceIdentity::capture(path);
    CHECK(id.has_value(), "снятие для JSON");
    if (id) {
        const QJsonObject json = id->toJson();
        CHECK(json.value("version").toInt() == 1 && json.value("hash").toString() == "sha1-64",
              "SourceIdentity: версия и алгоритм в JSON");
        CHECK(json.value("head").toString().size() == 16, "отпечаток — 16 цифр");
        // Через текст — как на диске.
        const QJsonObject back = QJsonDocument::fromJson(QJsonDocument(json).toJson()).object();
        const std::optional<SourceIdentity> read = SourceIdentity::fromJson(back);
        CHECK(read && *read == *id, "SourceIdentity: круговой проход");
        if (read)
            expectState(*read, State::Valid, "прочитанная идентичность проверяется");

        QJsonObject extra = json;
        extra.insert("future", 42);
        CHECK(SourceIdentity::fromJson(extra).has_value(), "лишние ключи не мешают");

        const auto refused = [&](QJsonObject bad, const QString& what) {
            QString error;
            CHECK(!SourceIdentity::fromJson(bad, &error).has_value() && !error.isEmpty(),
                  "SourceIdentity: отказ — " + what);
        };
        QJsonObject bad = json; bad.insert("version", 2); refused(bad, "версия 2");
        bad = json; bad.insert("version", 0); refused(bad, "версия 0");
        bad = json; bad.insert("version", 1.5); refused(bad, "дробная версия");
        bad = json; bad.insert("version", "1"); refused(bad, "версия строкой");
        bad = json; bad.remove("version"); refused(bad, "нет версии");
        bad = json; bad.insert("hash", "md5"); refused(bad, "чужой алгоритм");
        bad = json; bad.remove("hash"); refused(bad, "нет алгоритма");
        bad = json; bad.insert("hash", "sha1-64-ascii"); refused(bad, "алгоритм строки вместо области");
        bad = json; bad.remove("path"); refused(bad, "нет пути");
        bad = json; bad.insert("path", ""); refused(bad, "пустой путь");
        bad = json; bad.insert("path", 5); refused(bad, "путь числом");
        bad = json; bad.insert("path", "logs/app.log"); refused(bad, "относительный путь");
        bad = json; bad.insert("size", -1); refused(bad, "отрицательный размер");
        bad = json; bad.insert("size", 10.5); refused(bad, "дробный размер");
        bad = json; bad.insert("size", "6000"); refused(bad, "размер строкой");
        bad = json; bad.remove("size"); refused(bad, "нет размера");
        bad = json; bad.insert("head", "123"); refused(bad, "короткий отпечаток");
        bad = json; bad.insert("head", "zz00000000000000"); refused(bad, "не hex");
        bad = json; bad.insert("tail", QJsonValue()); refused(bad, "null вместо отпечатка");
        bad = json; bad.remove("tail"); refused(bad, "нет отпечатка конца");
        refused(QJsonObject(), "пустой объект");
        refused(parse("[1,2]"), "не объект");

        // F-2: сжатый источник — необязательные поля, версия та же.
        SourceIdentity gz = *id;
        gz.path = dir.filePath(QStringLiteral("json.log.gz"));
        gz.compressed = true;
        const QJsonObject gzJson = gz.toJson();
        CHECK(gzJson.value("version").toInt() == 1 && gzJson.value("compressed").toBool()
                  && !gzJson.contains("member"), "сжатый: compressed в JSON, версия 1");
        CHECK(!json.contains("compressed") && !json.contains("member"), "обычный: полей сжатого нет");
        const auto gzRead = SourceIdentity::fromJson(gzJson);
        CHECK(gzRead && *gzRead == gz && gzRead->compressed, "сжатый: круговой проход");
        CHECK(gz != *id, "сжатый и обычный с тем же содержимым — разные источники");
        SourceIdentity member = gz;
        member.member = QStringLiteral("logs/app.log");
        const auto memberRead = SourceIdentity::fromJson(member.toJson());
        CHECK(memberRead && memberRead->member == member.member && *memberRead == member,
              "член архива: круговой проход");
        CHECK(RecordLocator::sourceKey(member) == RecordLocator::pathKey(gz.path) + QChar(0x01) + member.member,
              "ключ источника с членом");
        bad = json; bad.insert("member", "x"); refused(bad, "member без compressed");
        bad = gzJson; bad.insert("compressed", "yes"); refused(bad, "compressed строкой");
        bad = gzJson; bad.insert("member", 3); refused(bad, "member числом");
        // Проверка: архива нет — Missing; есть, но не gzip — Missing; gzip —
        // Valid (содержимое сверяется по копии); содержимое — по копии.
        expectState(gz, State::Missing, "сжатый: архива нет");
        writeFile(dir, QStringLiteral("json.log.gz"), QByteArray("not a gzip"));
        expectState(gz, State::Missing, "сжатый: не gzip");
        writeFile(dir, QStringLiteral("json.log.gz"), QByteArray("\x1F\x8B\x08\x00rest", 8));
        expectState(gz, State::Valid, "сжатый: архив gzip — можно открыть");
        CHECK(gz.checkContentAt(path) == State::Valid, "сжатый: содержимое — по копии");
        CHECK(gz.checkContentAt(gz.path) == State::Changed, "сжатый: байты архива — не содержимое");
        // С отпечатками архива: тот же архив — Valid, другой gzip — Changed.
        const auto archive = SourceIdentity::capture(gz.path);
        CHECK(archive.has_value(), "сжатый: снятие архива");
        if (archive) {
            gz.archiveSize = archive->size;
            gz.archiveHead = archive->head;
            gz.archiveTail = archive->tail;
        }
        expectState(gz, State::Valid, "сжатый: тот же архив");
        const auto withArchive = SourceIdentity::fromJson(gz.toJson());
        CHECK(withArchive && withArchive->archiveSize == gz.archiveSize && withArchive->archiveHead == gz.archiveHead
                  && withArchive->archiveTail == gz.archiveTail, "сжатый: отпечатки архива — круговой проход");
        writeFile(dir, QStringLiteral("json.log.gz"), QByteArray("\x1F\x8B\x08\x00other", 9));
        expectState(gz, State::Changed, "сжатый: другой gzip по тому же пути");
        bad = gz.toJson(); bad.remove("compressed"); refused(bad, "archive без compressed");
        bad = gz.toJson(); bad.insert("archive", 5); refused(bad, "archive не объект");
        // Архив, из которого ещё ничего не прочитано (снимок посреди первой
        // распаковки, ревью F-2): пустой префикс — любая копия Valid, архив —
        // по отпечаткам; круговой проход через JSON.
        {
            LogFile copy(path);
            LogSource src;
            src.kind = LogSource::Kind::Archive;
            src.path = gz.path;
            copy.setSource(src);
            const auto unread = SourceIdentity::captureUnread(copy);
            CHECK(unread && unread->isValid() && unread->isUnreadArchive() && unread->archiveSize == 9,
                  "непрочитанный архив: снятие");
            if (unread) {
                CHECK(unread->checkContentAt(path) == State::Valid, "непрочитанный архив: любая копия — Valid");
                expectState(*unread, State::Valid, "непрочитанный архив: тот же архив");
                const auto back = SourceIdentity::fromJson(unread->toJson());
                CHECK(back && *back == *unread && back->isUnreadArchive(), "непрочитанный архив: круговой проход");
            }
            CHECK(!SourceIdentity::captureUnread(LogFile(path)), "непрочитанный: обычный файл — нет");
            CHECK(!id->isUnreadArchive() && !gz.isUnreadArchive(), "прочитанный — не «непрочитанный»");
        }
    }

    const RecordId rid = RecordId::forLine(185, u"2026-07-16 09:20:31.554 [db-pool] ERROR первый таймаут");
    CHECK(rid.isValid() && rid.hasText, "RecordId для завершённой строки");
    const QJsonObject json = rid.toJson();
    CHECK(json.value("version").toInt() == 1 && json.value("hash").toString() == "sha1-64-ascii"
              && json.value("line").toInteger() == 185 && json.value("text").toString().size() == 16,
          "RecordId: поля JSON");
    const std::optional<RecordId> back =
        RecordId::fromJson(QJsonDocument::fromJson(QJsonDocument(json).toJson()).object());
    CHECK(back && *back == rid && back->text == rid.text, "RecordId: круговой проход");

    const RecordId partial = RecordId::forLine(7, u"недописанная стро", /*complete=*/false);
    CHECK(!partial.hasText && !partial.toJson().contains("text")
              && partial.toJson().value("partial").toBool(),
          "недописанная строка без отпечатка, с явной меткой");
    const std::optional<RecordId> partialBack = RecordId::fromJson(partial.toJson());
    CHECK(partialBack && *partialBack == partial && !partialBack->hasText,
          "RecordId без отпечатка: круговой проход");
    CHECK(partial.matchesText(u"недописанная строка целиком"),
          "без отпечатка совпадает любой текст (проверены номер и источник)");

    const auto refused = [&](QJsonObject bad, const QString& what) {
        QString error;
        CHECK(!RecordId::fromJson(bad, &error).has_value() && !error.isEmpty(),
              "RecordId: отказ — " + what);
    };
    QJsonObject bad = json; bad.insert("version", 2); refused(bad, "версия 2");
    bad = json; bad.remove("version"); refused(bad, "нет версии");
    bad = json; bad.insert("hash", "sha1-64"); refused(bad, "алгоритм области вместо строки");
    bad = json; bad.insert("line", 0); refused(bad, "строка 0");
    bad = json; bad.insert("line", -3); refused(bad, "отрицательная строка");
    bad = json; bad.insert("line", 2.5); refused(bad, "дробная строка");
    bad = json; bad.insert("line", "185"); refused(bad, "строка строкой");
    bad = json; bad.remove("line"); refused(bad, "нет строки");
    bad = json; bad.insert("text", "abc"); refused(bad, "короткий отпечаток");
    bad = json; bad.insert("text", 12); refused(bad, "отпечаток числом");
    bad = json; bad.insert("text", QJsonValue()); refused(bad, "null вместо отпечатка");
    bad = json; bad.remove("text"); refused(bad, "нет отпечатка и нет метки недописанной строки");
    bad = json; bad.insert("partial", false); bad.remove("text"); refused(bad, "partial:false без отпечатка");
    bad = json; bad.insert("partial", true); refused(bad, "partial вместе с отпечатком");
    bad = json; bad.insert("partial", "yes"); refused(bad, "partial не булево");
    refused(parse("{\"version\":1,\"hash\":\"sha1-64-ascii\",\"line\":1e300}"), "огромная строка");

    // Hex: регистр не важен, длина — ровно 16.
    CHECK(RecordFingerprint::fromHex(u"00000000000000FF") == quint64(0xFF), "hex в верхнем регистре");
    CHECK(RecordFingerprint::toHex(0xFF) == "00000000000000ff", "hex с ведущими нулями");
    CHECK(!RecordFingerprint::fromHex(u"0x000000000000ff").has_value(), "префикс 0x — отказ");
}

// ---------------------------------------------------------------------------
// Отпечаток строки
// ---------------------------------------------------------------------------

// Строки документа по (файл, номер строки) → текст.
using LineMap = QMap<QPair<QString, qint64>, QString>;

static LineMap linesOf(const LogModel& model)
{
    LineMap out;
    for (int row = 0; row < model.rowCount(); ++row) {
        const RecordKey key = model.keyAt(row);
        out.insert({ key.file ? key.file->filePath : QString(), key.line }, model.messageAt(row));
    }
    return out;
}

// Сырые строки файла по правилу нумерации (expectedPhysicalLines): без BOM,
// '\r' перед '\n' и в конце хвоста снят.
static QList<QByteArray> rawLines(QByteArray bytes)
{
    if (bytes.startsWith("\xEF\xBB\xBF"))
        bytes.remove(0, 3);
    QList<QByteArray> parts = bytes.split('\n');
    if (!parts.isEmpty() && parts.last().isEmpty())
        parts.removeLast();
    for (QByteArray& part : parts)
        if (part.endsWith('\r'))
            part.chop(1);
    return parts;
}

// Отпечатки строк по номеру; каждая строка документа — ofLineText.
static QMap<QPair<QString, qint64>, quint64> fingerprints(const LineMap& lines)
{
    QMap<QPair<QString, qint64>, quint64> out;
    for (auto it = lines.constBegin(); it != lines.constEnd(); ++it)
        out.insert(it.key(), RecordFingerprint::ofLineText(it.value()));
    return out;
}

struct Fixture {
    QString name;
    QStringList paths;
    QList<QByteArray> bytes;   // содержимое файлов (для сверки с сырыми байтами)
    bool asciiCompatible = true; // false — UTF-16: только резидентный путь
};

static void checkBackendsAgree(const Fixture& fx, const QString& schema,
                               QMap<QPair<QString, qint64>, quint64>* reference = nullptr)
{
    Document res = buildResident(fx.paths, schema);
    for (const QString& e : res.errors)
        CHECK(false, fx.name + ": " + e);
    const LineMap resLines = linesOf(*res.model);
    const auto resPrints = fingerprints(resLines);

    // Эталон нумерации — разбиение байтов: сколько строк в каждом файле.
    int expectedLines = 0;
    for (const QByteArray& b : fx.bytes)
        expectedLines += fx.asciiCompatible ? rawLines(b).size() : 0;
    if (fx.asciiCompatible)
        CHECK(resLines.size() == expectedLines,
              QStringLiteral("%1: резидентный — %2 строк, в файле %3")
                  .arg(fx.name).arg(resLines.size()).arg(expectedLines));

    if (fx.asciiCompatible) {
        Document idx = buildIndexed(fx.paths, schema);
        for (const QString& e : idx.errors)
            CHECK(false, fx.name + ": " + e);
        const LineMap idxLines = linesOf(*idx.model);
        const auto idxPrints = fingerprints(idxLines);
        CHECK(idxLines.size() == resLines.size(),
              QStringLiteral("%1: число строк бэкендов %2 / %3")
                  .arg(fx.name).arg(resLines.size()).arg(idxLines.size()));
        int mismatches = 0;
        for (auto it = resPrints.constBegin(); it != resPrints.constEnd(); ++it) {
            if (idxPrints.value(it.key()) != it.value() && mismatches++ < 5)
                CHECK(false, QStringLiteral("%1: строка %2 — отпечатки бэкендов разные:\n"
                                            "    резидентный: %3\n    индексный:   %4")
                                 .arg(fx.name).arg(it.key().second)
                                 .arg(resLines.value(it.key()), idxLines.value(it.key())));
        }
        CHECK(mismatches == 0, QStringLiteral("%1: строк с разными отпечатками: %2").arg(fx.name).arg(mismatches));

        // Тот же отпечаток по сырым байтам строки, без декодирования.
        int byteMismatches = 0;
        for (int f = 0; f < fx.paths.size(); ++f) {
            const QList<QByteArray> raw = rawLines(fx.bytes[f]);
            for (int i = 0; i < raw.size(); ++i) {
                const QPair<QString, qint64> k{ fx.paths[f], i + 1 };
                if ((!resPrints.contains(k) || RecordFingerprint::ofLineBytes(raw[i]) != resPrints.value(k))
                    && byteMismatches++ < 5)
                    CHECK(false, QStringLiteral("%1: строка %2 — отпечаток байтов не равен отпечатку текста")
                                     .arg(fx.name).arg(i + 1));
            }
        }
        CHECK(byteMismatches == 0, QStringLiteral("%1: строк, где байты ≠ текст: %2")
                                       .arg(fx.name).arg(byteMismatches));
    }
    if (reference) {
        // Отпечатки, не зависящие от кодировки и пути: сравнение по номеру
        // строки и порядку файла.
        QList<quint64> mine = resPrints.values();
        QList<quint64> ref = reference->values();
        CHECK(mine == ref, fx.name + ": отпечатки не совпали с эталонной фикстурой");
    }
}

// Смена кодировки: те же строки, прочитанные любой декодируемой кодировкой,
// дают те же отпечатки на обоих бэкендах.
static void checkEncodingIndependence(const QString& name, const QString& path, const QString& schema)
{
    const QList<TextEncoding> encodings = { TextEncoding::Utf8, TextEncoding::Windows1251,
                                            TextEncoding::Ibm866, TextEncoding::Koi8R,
                                            TextEncoding::Windows1252 };
    Document base = buildIndexed({ path }, schema);
    const auto basePrints = fingerprints(linesOf(*base.model));
    const LineMap baseText = linesOf(*base.model);
    CHECK(!basePrints.isEmpty(), name + ": эталон пуст");

    IndexedLogStore* store = base.model->indexedOrNull();
    int textChanged = 0;
    for (const TextEncoding enc : encodings) {
        const QString encName = TextEncodings::displayName(enc);
        // Индексный: смена кодировки без переиндексации.
        store->setFileEncoding(base.files.value(0), TextDecoder(enc));
        CHECK(settle(*base.model), name + ": индексный успокоился после смены кодировки");
        const LineMap idxText = linesOf(*base.model);
        if (idxText != baseText)
            ++textChanged;
        CHECK(fingerprints(idxText) == basePrints,
              QStringLiteral("%1: индексный в %2 — отпечатки изменились").arg(name, encName));

        // Резидентный: выбор пользователя до разбора.
        Document res;
        res.model = std::make_unique<LogModel>();
        res.encodings[path] = FileEncoding{}.withUserChoice(TextDecoder(enc));
        addResidentFiles(res, { path }, schema);
        CHECK(settle(*res.model), name + ": резидентный успокоился");
        const LineMap resText = linesOf(*res.model);
        CHECK(fingerprints(resText) == basePrints,
              QStringLiteral("%1: резидентный в %2 — отпечатки изменились").arg(name, encName));
        CHECK(resText == idxText,
              QStringLiteral("%1: в %2 текст бэкендов разный").arg(name, encName));
    }
    CHECK(textChanged > 0, name + ": смена кодировки не изменила текст — проверка пустая");
}

static void testLineFingerprints(const QDir& dir, const QString& schema)
{
    // Различение и известные совпадения.
    using RecordFingerprint::ofLineText;
    using RecordFingerprint::ofLineBytes;
    CHECK(ofLineText(u"ERROR - disk a") != ofLineText(u"ERROR - disk b"), "разный ASCII — разные отпечатки");
    CHECK(ofLineText(u"ERROR - disk") != ofLineText(u"ERROR - disk "), "лишний пробел различается");
    CHECK(ofLineText(u"") != ofLineText(u" "), "пустая строка ≠ пробел");
    CHECK(ofLineText(u"a\tb") != ofLineText(u"a b"), "таб ≠ пробел");
    CHECK(ofLineText(u"x\ry") != ofLineText(u"xy"), "'\\r' внутри строки — часть текста");
    CHECK(ofLineText(u"Ошибка 1") == ofLineText(u"Привет 1"),
          "известное совпадение: только не-ASCII текст различается");
    CHECK(ofLineText(u"Ошибка 1") != ofLineText(u"Ошибка 2"), "не-ASCII строки с разным ASCII различаются");
    CHECK(ofLineText(u"Ош ибка") != ofLineText(u"Ошибка"), "серии не-ASCII считаются по отдельности");
    CHECK(ofLineText(u"a\u0080b") == ofLineBytes("a\x80" "b"), "заполнитель не путается с ASCII");
    CHECK(ofLineText(u"a\u0080b") != ofLineText(u"ab"), "серия не-ASCII ≠ ничего");
    // Длинная строка: буфер скелета переливается несколько раз.
    QString longText;
    for (int i = 0; i < 20000; ++i)
        longText += (i % 7 == 0) ? QStringLiteral("ж") : QStringLiteral("x");
    CHECK(ofLineText(longText) == ofLineBytes(longText.toUtf8()), "длинная строка: текст = байты UTF-8");
    QString longOther = longText;
    longOther[19999] = u'y';
    CHECK(ofLineText(longText) != ofLineText(longOther), "длинная строка: различие в конце");

    // Битый UTF-8: декодер не съедает ASCII за битой последовательностью.
    const QByteArray broken[] = {
        "\xC3" "A", "\xE2\x82(", "\xC1\x81", "\x80", "\xFF\xFE", "ok \xD0\x9F\xD1\x80 \xD0",
        "\xF0\x9F\x98" "x", "\xED\xA0\x80" "z", "\xF4\x90\x80\x80" "q", "\xE0\x80\xAF/",
    };
    for (const QByteArray& b : broken) {
        CHECK(ofLineText(QString::fromUtf8(b)) == ofLineBytes(b),
              "битый UTF-8: текст ≠ байты для " + QString::fromLatin1(b.toHex()));
        for (const TextEncoding enc : { TextEncoding::Windows1251, TextEncoding::Ibm866,
                                        TextEncoding::Koi8R, TextEncoding::Windows1252 })
            CHECK(ofLineText(TextDecoder(enc).decode(b)) == ofLineBytes(b),
                  QStringLiteral("%1: текст ≠ байты для %2").arg(TextEncodings::displayName(enc),
                                                                  QString::fromLatin1(b.toHex())));
    }
    // Все байты подряд в каждой однобайтной кодировке.
    QByteArray all;
    for (int c = 1; c < 256; ++c)
        if (c != '\n')
            all += char(c);
    for (const TextEncoding enc : { TextEncoding::Utf8, TextEncoding::Windows1251, TextEncoding::Ibm866,
                                    TextEncoding::Koi8R, TextEncoding::Windows1252 })
        CHECK(ofLineText(TextDecoder(enc).decode(all)) == ofLineBytes(all),
              TextEncodings::displayName(enc) + ": все байты — текст ≠ байты");

    // --- Фикстуры на обоих бэкендах -------------------------------------------
    const QDateTime base = fixtureBaseTime();
    const QByteArray utf8Log = makeLogBytes(300, 7, "\n", base, true, true, true);
    const QByteArray crlfLog = makeLogBytes(300, 8, "\r\n", base, true, false, true);
    const QString cyr = cyrillicLogText(10);
    const QString cyrB = cyrillicLogText(0, 7);

    QByteArray brokenLog;
    int second = 0;
    const auto stamp = [&]() {
        return base.addSecs(second++).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")).toUtf8();
    };
    for (const QByteArray& b : broken)
        brokenLog += stamp() + " [w] ERROR - broken " + b + " tail\n";
    brokenLog += "\n\n" + stamp() + " [w] INFO - after blanks \xD0\x9F\xD1\x80\xD0\xB8\n";
    brokenLog += "    \xC3\n";
    brokenLog += stamp() + " [w] INFO - cut tail \xE2\x82\r";

    QByteArray longLog = stamp() + " [w] INFO - before\n" + stamp() + " [w] WARN - long ";
    for (int i = 0; i < 12000; ++i)
        longLog += (i % 5 == 0) ? QByteArray("\xD0\xB6") : QByteArray("abcdefg");
    longLog += "\xC3\n" + stamp() + " [w] INFO - after\n";
    CHECK(longLog.size() > 65536, "строка длиннее 64 КБ");

    const QByteArray latin = encodeText(QStringLiteral(
        "2026-03-05 10:00:00.000 [w] INFO - café Müller € naïve\n"
        "2026-03-05 10:00:01.000 [w] WARN - Ångström ß œ\n"
        "    plain continuation\n"), TextEncoding::Windows1252);

    struct Source { QString name; QByteArray bytes; };
    const QList<Source> sources = {
        { "utf8.log", utf8Log },
        { "crlf.log", crlfLog },
        { "cp1251.log", encodeText(cyr, TextEncoding::Windows1251) },
        { "cp866.log", encodeText(cyr, TextEncoding::Ibm866) },
        { "koi8r.log", encodeText(cyr, TextEncoding::Koi8R) },
        { "cp1252.log", latin },
        { "broken.log", brokenLog },
        { "long.log", longLog },
        { "cyr-utf8.log", encodeText(cyr, TextEncoding::Utf8) },
    };
    QHash<QString, QString> pathOf;
    for (const Source& s : sources) {
        pathOf[s.name] = writeFile(dir, s.name, s.bytes);
        checkBackendsAgree({ s.name, { pathOf[s.name] }, { s.bytes } }, schema);
    }

    // Те же строки в разных кодировках — те же отпечатки по номеру строки.
    Document cyrRef = buildResident({ pathOf["cyr-utf8.log"] }, schema);
    auto cyrPrints = fingerprints(linesOf(*cyrRef.model));
    for (const char* n : { "cp1251.log", "cp866.log", "koi8r.log" }) {
        Document d = buildIndexed({ pathOf[n] }, schema);
        CHECK(fingerprints(linesOf(*d.model)).values() == cyrPrints.values(),
              QStringLiteral("%1: отпечатки не равны отпечаткам того же текста в UTF-8").arg(n));
    }

    // UTF-16LE с BOM — только резидентный: отпечаток от декодированного текста
    // равен отпечатку того же текста в UTF-8.
    {
        QStringEncoder toUtf16(QStringEncoder::Utf16LE, QStringEncoder::Flag::WriteBom);
        const QByteArray utf16 = toUtf16.encode(cyr);
        const QString p = writeFile(dir, "utf16.log", utf16);
        checkBackendsAgree({ "utf16.log", { p }, { utf16 }, false }, schema, &cyrPrints);
    }

    // Слитая вкладка: UTF-8 и Windows-1251 вперемешку по времени.
    {
        const QByteArray b = encodeText(cyrB, TextEncoding::Windows1251);
        const QString pb = writeFile(dir, "merged-b.log", b);
        checkBackendsAgree({ "слитая вкладка", { pathOf["cyr-utf8.log"], pb },
                             { encodeText(cyr, TextEncoding::Utf8), b } }, schema);
    }

    // Отпечаток не зависит от кодировки, которой прочитан файл.
    for (const char* n : { "cp1251.log", "cyr-utf8.log", "broken.log", "cp1252.log", "long.log" })
        checkEncodingIndependence(QString::fromLatin1(n), pathOf[n], schema);
}

// ---------------------------------------------------------------------------
// RecordLocator (шаг 3): сохранённые идентификаторы → ключи на обоих бэкендах
// ---------------------------------------------------------------------------

using Refusal = RecordLocator::Refusal;

// Сохранённая строка: что R-4 запишет на диск, плюс эталон для сверки.
struct SavedLine {
    SourceIdentity source;
    RecordId id;
    QString path;
    qint64 line = 0;
    QString text;
};

// Ответы локатора по токенам.
struct Outcomes {
    QHash<int, RecordKey> resolved;
    QHash<int, Refusal> refused;
    QHash<int, RecordId> completed;
    QHash<int, qint64> linesShownAtResolve; // строк в модели в момент ответа
    int answers() const { return int(resolved.size() + refused.size()); }
};

static void listen(LogModel& model, Outcomes& out, QObject* context)
{
    RecordLocator& loc = model.recordLocator();
    QObject::connect(&loc, &RecordLocator::resolved, context,
                     [&out, &model](int token, const RecordKey& key) {
                         out.resolved.insert(token, key);
                         out.linesShownAtResolve.insert(token, model.totalLineCount());
                     });
    QObject::connect(&loc, &RecordLocator::refused, context,
                     [&out](int token, Refusal reason) { out.refused.insert(token, reason); });
    QObject::connect(&loc, &RecordLocator::completed, context,
                     [&out](int token, const RecordId& id) { out.completed.insert(token, id); });
}

static bool waitIdle(LogModel& model)
{
    return waitFor([&model] { return !model.recordLocator().isBusy(); }, 60000);
}

// Сохранить строки документа rows: идентичность источника по концу
// прочитанного и идентификатор строки — через JSON, как на диск.
static QVector<SavedLine> saveRows(const LogModel& model, const QVector<int>& rows)
{
    QVector<SavedLine> out;
    RecordLocator& loc = model.recordLocator();
    for (const int row : rows) {
        const RecordKey key = model.keyAt(row);
        if (!key.isValid())
            continue;
        const auto source = loc.sourceFor(key.file);
        const RecordId id = loc.idFor(key, model.messageAt(row));
        if (!source)
            continue;
        const QJsonDocument doc(QJsonObject{ { "source", source->toJson() }, { "record", id.toJson() } });
        const QJsonObject back = QJsonDocument::fromJson(doc.toJson()).object();
        const auto s = SourceIdentity::fromJson(back.value("source").toObject());
        const auto r = RecordId::fromJson(back.value("record").toObject());
        CHECK(s && r && *s == *source && *r == id, "JSON сохранённой строки — круговой проход");
        if (!s || !r)
            continue;
        out.append({ *s, *r, key.file->filePath, key.line, model.messageAt(row) });
    }
    return out;
}

// Документ: модель → before (фильтр, запросы) → загрузка обоими путями.
static Document loadDocument(bool resident, const QStringList& paths, const QString& schema,
                             const std::function<void(Document&)>& before = {})
{
    Document doc;
    doc.model = std::make_unique<LogModel>();
    if (before)
        before(doc);
    if (resident)
        addResidentFiles(doc, paths, schema);
    else
        addIndexedFiles(doc, paths, schema, IndexedLoad::Concurrent);
    if (!settle(*doc.model) || !waitIdle(*doc.model))
        doc.errors << QStringLiteral("документ не пришёл в покой");
    return doc;
}

static const char* backendName(bool resident) { return resident ? "резидентный" : "индексный"; }

static QVector<int> spreadRows(const LogModel& model, int count)
{
    QVector<int> rows;
    const int n = model.rowCount();
    for (int i = 0; i < count && n > 0; ++i)
        rows.append(int(qint64(n - 1) * i / qMax(1, count - 1)));
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

static void testLocatorRoundTrip(const QDir& dir, const QString& schema)
{
    const QString a = writeFile(dir, "loc-a.log",
                                makeLogBytes(9000, 101u, "\n", fixtureBaseTime(), true, true, false));
    const QString b = writeFile(dir, "loc-b.log",
                                makeLogBytes(4000, 202u, "\r\n", fixtureBaseTime().addSecs(30), false, true, true));
    const QList<QStringList> sets = { { a }, { a, b } };
    for (const QStringList& paths : sets) {
        for (const bool saveResident : { true, false }) {
            Document saved = loadDocument(saveResident, paths, schema);
            CHECK(saved.errors.isEmpty(), saved.errors.join("; "));
            const QVector<SavedLine> lines = saveRows(*saved.model, spreadRows(*saved.model, 120));
            CHECK(lines.size() > 100, QStringLiteral("сохранено строк: %1").arg(lines.size()));
            const qint64 total = saved.model->totalLineCount();
            saved = Document(); // вкладку закрыли

            for (const bool restoreResident : { true, false }) {
                for (const bool filtered : { false, true }) {
                    const QString what = QStringLiteral("%1 файл(ов), сохранено %2, найдено %3%4")
                                             .arg(paths.size())
                                             .arg(QString::fromUtf8(backendName(saveResident)),
                                                  QString::fromUtf8(backendName(restoreResident)),
                                                  filtered ? QStringLiteral(", с фильтром") : QString());
                    QObject context;
                    Outcomes out;
                    QHash<int, SavedLine> byToken;
                    Document doc = loadDocument(restoreResident, paths, schema, [&](Document& d) {
                        listen(*d.model, out, &context);
                        // Фильтр — у бэкенда, в который придут строки.
                        if (!restoreResident)
                            d.model->convertToIndexedBackend()->setFieldPattern(schema, true);
                        if (filtered)
                            d.model->setLogLevelFilter({ LogLevel::Error, LogLevel::Fatal });
                        for (const SavedLine& s : lines) {
                            const int token = d.model->recordLocator().request(s.source, s.id);
                            byToken.insert(token, s);
                        }
                        CHECK(out.answers() == 0, what + ": ответа внутри request нет");
                    });
                    CHECK(doc.errors.isEmpty(), what + ": " + doc.errors.join("; "));
                    LogModel& model = *doc.model;
                    CHECK(out.refused.isEmpty(), QStringLiteral("%1: отказов %2").arg(what).arg(out.refused.size()));
                    CHECK(out.resolved.size() == lines.size(),
                          QStringLiteral("%1: найдено %2 из %3").arg(what).arg(out.resolved.size()).arg(lines.size()));
                    CHECK(model.recordLocator().pendingCount() == 0, what + ": ожидающих не осталось");
                    CHECK(model.recordLocator().stats().examined == lines.size(),
                          QStringLiteral("%1: тронуто ожидающих %2").arg(what).arg(model.recordLocator().stats().examined));
                    // По мере загрузки: первые строки нашлись раньше, чем
                    // показаны все (слитая вкладка; резидентный — порциями).
                    if (restoreResident) {
                        qint64 earliest = total;
                        for (const qint64 shown : std::as_const(out.linesShownAtResolve))
                            earliest = qMin(earliest, shown);
                        CHECK(earliest < total, QStringLiteral("%1: найдено по мере загрузки (%2 из %3 строк)")
                                                    .arg(what).arg(earliest).arg(total));
                    }
                    int hidden = 0;
                    for (auto it = out.resolved.cbegin(); it != out.resolved.cend(); ++it) {
                        const SavedLine& s = byToken[it.key()];
                        const RecordKey& key = it.value();
                        CHECK(key.isValid() && key.file->filePath == s.path && key.line == s.line,
                              QStringLiteral("%1: строка %2 найдена на месте").arg(what).arg(s.line));
                        if (model.rowForKey(key) < 0) {
                            ++hidden;
                            CHECK(filtered && model.nearestRowForKey(key) >= 0,
                                  QStringLiteral("%1: скрытая строка %2 — в данных").arg(what).arg(s.line));
                        }
                    }
                    if (filtered)
                        CHECK(hidden > 0 && hidden < out.resolved.size(),
                              QStringLiteral("%1: фильтр скрыл часть (%2)").arg(what).arg(hidden));
                    model.setLogLevelFilter({});
                    settle(model);
                    for (auto it = out.resolved.cbegin(); it != out.resolved.cend(); ++it) {
                        const int row = model.rowForKey(it.value());
                        CHECK(row >= 0 && model.messageAt(row) == byToken[it.key()].text,
                              QStringLiteral("%1: ключ ведёт к той же строке (%2)").arg(what).arg(byToken[it.key()].line));
                    }
                }
            }
        }
    }
}

static void testLocatorRefusals(const QDir& dir, const QString& schema)
{
    for (const bool resident : { true, false }) {
        const QString tag = QString::fromUtf8(backendName(resident));
        const QByteArray bytes = makeLogBytes(800, 7u, "\n", fixtureBaseTime(), false, true, true);
        const QString path = writeFile(dir, QStringLiteral("refuse-%1.log").arg(resident), bytes);
        QVector<SavedLine> lines;
        {
            Document d = loadDocument(resident, { path }, schema);
            lines = saveRows(*d.model, spreadRows(*d.model, 40));
        }
        CHECK(lines.size() >= 30, tag + ": сохранены строки");
        // Начала строк файла (номер строки → смещение).
        QVector<qint64> lineStart = { 0 };
        for (qint64 i = 0; i < bytes.size(); ++i)
            if (bytes[i] == '\n')
                lineStart.append(i + 1);
        // Строка из середины файла, которую правим (дальше 4 КБ от краёв).
        int middle = -1;
        for (int i = 0; i < lines.size(); ++i) {
            const qint64 at = lineStart.value(lines[i].line - 1, -1);
            if (at > 8192 && at < bytes.size() - 8192 && lines[i].text.contains(QRegularExpression("[a-z]"))) {
                middle = i;
                break;
            }
        }
        CHECK(middle >= 0, tag + ": есть строка в середине файла");

        // LineAbsent: номер за концом файла.
        {
            SavedLine beyond = lines.constLast();
            beyond.id.line += 100000;
            QObject context;
            Outcomes out;
            int token = 0;
            Document d = loadDocument(resident, { path }, schema, [&](Document& doc) {
                listen(*doc.model, out, &context);
                token = doc.model->recordLocator().request(beyond.source, beyond.id);
            });
            CHECK(out.refused.value(token, Refusal::SourceChanged) == Refusal::LineAbsent
                      && out.refused.size() == 1 && out.resolved.isEmpty(),
                  tag + ": строки нет — LineAbsent");
        }

        // TextMismatch: правка одной строки в середине, начало и конец целы.
        if (middle >= 0) {
            QByteArray edited = bytes;
            const qint64 at = lineStart[lines[middle].line - 1];
            const qint64 letter = at + lines[middle].text.indexOf(QRegularExpression("[a-z]"));
            edited[letter] = edited[letter] == 'q' ? 'z' : 'q';
            const QString editedPath = writeFile(dir, QStringLiteral("refuse-mid-%1.log").arg(resident), edited);
            QObject context;
            Outcomes out;
            QHash<int, int> index;
            Document d = loadDocument(resident, { editedPath }, schema, [&](Document& doc) {
                listen(*doc.model, out, &context);
                for (int i = 0; i < lines.size(); ++i) {
                    SourceIdentity s = lines[i].source;
                    s.path = editedPath; // тот же файл, перенесённый
                    index.insert(doc.model->recordLocator().request(s, lines[i].id), i);
                }
            });
            CHECK(lines[middle].source.checkAt(editedPath) == State::Valid, tag + ": правка в середине — источник Valid");
            for (auto it = index.cbegin(); it != index.cend(); ++it) {
                if (it.value() == middle)
                    CHECK(out.refused.value(it.key(), Refusal::LineAbsent) == Refusal::TextMismatch,
                          tag + ": изменённая строка — TextMismatch");
                else
                    CHECK(out.resolved.contains(it.key()),
                          QStringLiteral("%1: строка %2 найдена").arg(tag).arg(lines[it.value()].line));
            }
        }

        // SourceChanged: переписан той же длиной (первый байт другой).
        {
            QByteArray rewritten = bytes;
            rewritten[0] = rewritten[0] == '1' ? '2' : '1';
            const QString changed = writeFile(dir, QStringLiteral("refuse-changed-%1.log").arg(resident), rewritten);
            QObject context;
            Outcomes out;
            Document d = loadDocument(resident, { changed }, schema, [&](Document& doc) {
                listen(*doc.model, out, &context);
                for (const SavedLine& s : lines) {
                    SourceIdentity src = s.source;
                    src.path = changed;
                    doc.model->recordLocator().request(src, s.id);
                }
            });
            CHECK(out.resolved.isEmpty() && out.refused.size() == lines.size(),
                  QStringLiteral("%1: переписанный — ни одного resolved (%2)").arg(tag).arg(out.resolved.size()));
            bool allChanged = true;
            for (const Refusal r : std::as_const(out.refused))
                allChanged = allChanged && r == Refusal::SourceChanged;
            CHECK(allChanged, tag + ": все — SourceChanged");
            CHECK(d.model->recordLocator().stats().sourceChecks == 1, tag + ": источник проверен один раз");
        }

        // SourceMissing: файл исчез после загрузки (поздний запрос), пустая
        // идентичность.
        {
            const QString gone = writeFile(dir, QStringLiteral("refuse-gone-%1.log").arg(resident), bytes);
            Document d = loadDocument(resident, { gone }, schema);
            QObject context;
            Outcomes out;
            listen(*d.model, out, &context);
            CHECK(QFile::remove(gone), tag + ": файл удалён");
            SourceIdentity src = lines[0].source;
            src.path = gone;
            const int t1 = d.model->recordLocator().request(src, lines[0].id);
            const int t2 = d.model->recordLocator().request(SourceIdentity(), lines[1].id);
            CHECK(out.answers() == 0, tag + ": ответ не внутри request");
            waitFor([&] { return out.answers() == 2; }, 5000);
            CHECK(out.refused.value(t1, Refusal::LineAbsent) == Refusal::SourceMissing,
                  tag + ": файла нет — SourceMissing");
            CHECK(out.refused.value(t2, Refusal::LineAbsent) == Refusal::SourceMissing,
                  tag + ": пустая идентичность — SourceMissing");
        }

        // Поздний запрос: файл уже прочитан — строки находятся отдельным проходом.
        {
            Document d = loadDocument(resident, { path }, schema);
            QObject context;
            Outcomes out;
            listen(*d.model, out, &context);
            for (const SavedLine& s : lines)
                d.model->recordLocator().request(s.source, s.id);
            waitFor([&] { return out.answers() == lines.size(); }, 10000);
            CHECK(out.resolved.size() == lines.size(),
                  QStringLiteral("%1: поздние запросы найдены (%2)").arg(tag).arg(out.resolved.size()));
            CHECK(d.model->recordLocator().stats().latePasses >= 1, tag + ": поздний проход");
        }
    }
}

static void testLocatorPartialLine(const QDir& dir, const QString& schema)
{
    // Индексный путь: недописанная строка (без '\n') — идентификатор без
    // отпечатка; дописанная — completed с отпечатком полной строки.
    QByteArray bytes = makeLogBytes(300, 9u, "\n", fixtureBaseTime(), false, true, true);
    bytes += "2026-07-16 10:00:00.000 [main] INFO partial tail";
    const QString path = writeFile(dir, "partial.log", bytes);
    SavedLine saved;
    {
        Document d = loadDocument(false, { path }, schema);
        LogModel& m = *d.model;
        const int last = m.rowCount() - 1;
        const QVector<SavedLine> s = saveRows(m, { last });
        CHECK(s.size() == 1 && !s[0].id.hasText, "индексный: последняя строка без '\\n' — без отпечатка");
        CHECK(s.size() == 1 && s[0].id.toJson().value("partial").toBool(), "JSON: partial:true");
        if (s.size() == 1)
            saved = s[0];
    }
    QObject context;
    Outcomes out;
    int token = 0;
    Document d = loadDocument(false, { path }, schema, [&](Document& doc) {
        listen(*doc.model, out, &context);
        token = doc.model->recordLocator().request(saved.source, saved.id);
    });
    CHECK(out.resolved.value(token).line == saved.line, "недописанная строка найдена по номеру");
    CHECK(out.completed.isEmpty(), "пока не дописана — completed нет");
    CHECK(appendIndexedTail(d, 0, " and the rest\nnext line\n", schema), "дозапись");
    waitIdle(*d.model);
    const RecordId expected = RecordId::forLine(saved.line, saved.text + " and the rest");
    CHECK(out.completed.contains(token) && out.completed[token] == expected,
          "дописана — completed с отпечатком полной строки");
    // Идентичность по концу прочитанного до дозаписи по-прежнему Valid.
    CHECK(saved.source.check() == State::Valid, "дозапись: источник Valid");
}

static void testLocatorEncodings(const QDir& dir, const QString& schema)
{
    const QByteArray cp1251 = encodeText(cyrillicLogText(40), TextEncoding::Windows1251);
    const QString path = writeFile(dir, "loc-cp1251.log", cp1251);
    for (const bool saveResident : { true, false }) {
        QVector<SavedLine> lines;
        {
            Document d = loadDocument(saveResident, { path }, schema);
            lines = saveRows(*d.model, spreadRows(*d.model, 60));
        }
        for (const bool restoreResident : { true, false }) {
            const QString what = QStringLiteral("cp1251 → KOI8-R, %1 → %2")
                                     .arg(QString::fromUtf8(backendName(saveResident)),
                                          QString::fromUtf8(backendName(restoreResident)));
            QObject context;
            Outcomes out;
            Document d = loadDocument(restoreResident, { path }, schema, [&](Document& doc) {
                listen(*doc.model, out, &context);
                // Между сохранением и восстановлением пользователь выбрал
                // другую кодировку (резидентный — до загрузки).
                doc.encodings[path] = FileEncoding().withUserChoice(TextDecoder(TextEncoding::Koi8R));
                for (const SavedLine& s : lines)
                    doc.model->recordLocator().request(s.source, s.id);
            });
            if (!restoreResident) {
                setDocumentEncoding(d, d.files[0], FileEncoding().withUserChoice(TextDecoder(TextEncoding::Koi8R)), schema);
                settle(*d.model);
            }
            CHECK(out.resolved.size() == lines.size() && out.refused.isEmpty(),
                  QStringLiteral("%1: найдено %2 из %3").arg(what).arg(out.resolved.size()).arg(lines.size()));
            // Текст строки теперь другой (KOI8-R), а ключ ведёт к той же строке.
            int differs = 0;
            for (const RecordKey& key : std::as_const(out.resolved)) {
                const int row = d.model->rowForKey(key);
                CHECK(row >= 0, what + ": ключ виден");
                for (const SavedLine& s : lines)
                    if (s.line == key.line && row >= 0 && d.model->messageAt(row) != s.text)
                        ++differs;
            }
            CHECK(differs > 0, what + ": текст строк сменился вместе с кодировкой");
        }
    }
}

// Цена порции (инвариант 3): K ожидающих, поток порций разного размера —
// работа не зависит от размера порции.
static void testLocatorBatchCost(const QDir& dir, const QString& schema)
{
    const QString path = writeFile(dir, "cost.log",
                                   makeLogBytes(30000, 33u, "\n", fixtureBaseTime(), false, true, true));
    for (const bool resident : { true, false }) {
        Document d = loadDocument(resident, { path }, schema);
        LogModel& model = *d.model;
        const QVector<SavedLine> lines = saveRows(model, spreadRows(model, 1000));
        const LogFilePtr file = d.files[0];
        const qint64 total = model.totalLineCount();
        // Все строки резидентного документа в порядке файла — для порций.
        QVector<std::shared_ptr<LogEntry>> fileOrder;
        if (resident) {
            fileOrder = model.residentEntriesForFieldMutation();
            std::sort(fileOrder.begin(), fileOrder.end(), [](const auto& x, const auto& y) {
                return x->originalLineNumber() < y->originalLineNumber();
            });
        }
        qint64 examinedPerChunk[2] = { 0, 0 };
        const int chunks[2] = { 50, 20000 };
        for (int c = 0; c < 2; ++c) {
            RecordLocator& loc = model.recordLocator();
            QObject context;
            Outcomes out;
            listen(model, out, &context);
            model.invalidateFileRows(file->filePath); // строки «сброшены»
            for (const SavedLine& s : lines)
                loc.reacquire(file, s.id);
            const qint64 before = loc.stats().examined;
            for (qint64 first = 0; first < total; first += chunks[c]) {
                const qint64 count = qMin<qint64>(chunks[c], total - first);
                if (resident)
                    loc.residentBatch(fileOrder.mid(first, count));
                else
                    loc.indexedRange(file, first, count);
            }
            loc.fileLoaded(file);
            waitIdle(model);
            examinedPerChunk[c] = loc.stats().examined - before;
            CHECK(out.resolved.size() == lines.size(),
                  QStringLiteral("%1, порции по %2: найдено %3 из %4").arg(QString::fromUtf8(backendName(resident)))
                      .arg(chunks[c]).arg(out.resolved.size()).arg(lines.size()));
        }
        CHECK(examinedPerChunk[0] == lines.size() && examinedPerChunk[1] == lines.size(),
              QStringLiteral("%1: тронуто ожидающих %2 (порции по 50) и %3 (по 20000) при K = %4")
                  .arg(QString::fromUtf8(backendName(resident))).arg(examinedPerChunk[0])
                  .arg(examinedPerChunk[1]).arg(lines.size()));
    }
}

// ---------------------------------------------------------------------------
// Замер: --bench <file> [K] [--resident] — разрешение K сохранённых
// идентификаторов (по умолчанию 1000) при загрузке файла против той же
// загрузки без них: время и самое долгое событие GUI-потока.
// ---------------------------------------------------------------------------

class TimingApp : public QCoreApplication {
public:
    using QCoreApplication::QCoreApplication;
    bool timing = false;
    qint64 maxNs = 0;
    bool notify(QObject* receiver, QEvent* event) override
    {
        if (!timing)
            return QCoreApplication::notify(receiver, event);
        QElapsedTimer t;
        t.start();
        const bool ok = QCoreApplication::notify(receiver, event);
        maxNs = qMax(maxNs, t.nsecsElapsed());
        return ok;
    }
};

static int runBench(TimingApp& app, const QString& path, int k, bool resident)
{
    const QString schema = buildSchema();
    const auto load = [&](const std::function<void(Document&)>& before, qint64* resolvedAtMs,
                          int* resolvedCount) {
        app.maxNs = 0;
        app.timing = true;
        QElapsedTimer t;
        t.start();
        Document doc;
        doc.model = std::make_unique<LogModel>();
        QObject context;
        int resolved = 0;
        QObject::connect(&doc.model->recordLocator(), &RecordLocator::resolved, &context,
                         [&](int, const RecordKey&) {
                             ++resolved;
                             if (resolvedAtMs)
                                 *resolvedAtMs = t.elapsed();
                         });
        if (before)
            before(doc);
        if (resident)
            addResidentFiles(doc, { path }, schema);
        else
            addIndexedFiles(doc, { path }, schema);
        const qint64 loadedMs = t.elapsed();
        waitIdle(*doc.model);
        app.timing = false;
        if (resolvedCount)
            *resolvedCount = resolved;
        std::fprintf(stdout, "  загрузка %lld мс, до конца проверки %lld мс, самое долгое событие %.1f мс, строк %lld\n",
                     qint64(loadedMs), qint64(t.elapsed()), app.maxNs / 1e6,
                     qint64(doc.model->totalLineCount()));
        return doc;
    };
    std::fprintf(stdout, "%s, %s бэкенд\n", qPrintable(path), resident ? "резидентный" : "индексный");
    std::fprintf(stdout, "без идентификаторов:\n");
    Document base = load({}, nullptr, nullptr);
    const QVector<SavedLine> lines = saveRows(*base.model, spreadRows(*base.model, k));
    base = Document();
    std::fprintf(stdout, "с %d сохранёнными идентификаторами:\n", int(lines.size()));
    qint64 lastResolvedMs = 0;
    int resolved = 0;
    QElapsedTimer requestTimer;
    qint64 requestNs = 0;
    Document doc = load([&](Document& d) {
        requestTimer.start();
        for (const SavedLine& s : lines)
            d.model->recordLocator().request(s.source, s.id);
        requestNs = requestTimer.nsecsElapsed();
    }, &lastResolvedMs, &resolved);
    const RecordLocator::Stats& st = doc.model->recordLocator().stats();
    std::fprintf(stdout, "  постановка запросов %.2f мс; найдено %d из %d, последнее — на %lld мс; "
                         "порций %lld, тронуто ожидающих %lld, сверено байтами %lld, проверок источника %lld\n",
                 requestNs / 1e6, resolved, int(lines.size()), lastResolvedMs, st.feeds, st.examined,
                 st.byteChecks, st.sourceChecks);
    return resolved == lines.size() ? 0 : 1;
}

// R-8: путь в сохранённом файле — по правилу SavedPath, одинаково на любой
// ОС. Разбор не смотрит на платформу; от неё зависят только isNative /
// isForeign и регистр POSIX-путей в same.
static void testSavedPaths(const QDir& dir)
{
    using SavedPath::Style;
    struct Case { const char16_t* path; Style style; const char16_t* normalized; };
    const Case cases[] = {
        { u"C:/logs/app.log",                   Style::WindowsDrive, u"C:/logs/app.log" },
        { u"C:\\logs\\app.log",                 Style::WindowsDrive, u"C:/logs/app.log" },
        { u"c:\\Logs/mixed\\app.log",           Style::WindowsDrive, u"c:/Logs/mixed/app.log" },
        { u"D:\\",                              Style::WindowsDrive, u"D:/" },
        { u"//server/share/app.log",            Style::WindowsUnc,   u"//server/share/app.log" },
        { u"\\\\server\\share\\логи и данные\\app.log", Style::WindowsUnc, u"//server/share/логи и данные/app.log" },
        { u"\\\\?\\C:\\very\\long\\app.log",    Style::WindowsDrive, u"C:/very/long/app.log" },
        { u"\\\\?\\UNC\\server\\share\\app.log", Style::WindowsUnc,  u"//server/share/app.log" },
        { u"\\\\?\\unc\\server\\share\\a.log",  Style::WindowsUnc,   u"//server/share/a.log" },
        { u"/var/log/app.log",                  Style::Posix,        u"/var/log/app.log" },
        { u"/home/anna/back\\slash.log",        Style::Posix,        u"/home/anna/back\\slash.log" },
        { u"/",                                 Style::Posix,        u"/" },
        { u"/home/anna/dir/",                   Style::Posix,        u"/home/anna/dir/" },
        { u"logs/app.log",                      Style::None,         u"logs/app.log" },
        { u"../logs/app.log",                   Style::None,         u"../logs/app.log" },
        { u"C:logs\\app.log",                   Style::None,         u"C:logs\\app.log" },
        { u"\\logs\\app.log",                   Style::None,         u"\\logs\\app.log" },
        { u"\\\\?\\",                           Style::None,         u"\\\\?\\" },
        { u"\\\\?\\UNC\\",                      Style::None,         u"\\\\?\\UNC\\" },
        { u"1:/x.log",                          Style::None,         u"1:/x.log" },
        { u"",                                  Style::None,         u"" },
    };
    for (const Case& c : cases) {
        const QString path = QString::fromUtf16(c.path);
        CHECK(SavedPath::style(path) == c.style,
              QStringLiteral("стиль пути «%1»: %2").arg(path).arg(int(SavedPath::style(path))));
        CHECK(SavedPath::isAbsolute(path) == (c.style != Style::None),
              QStringLiteral("абсолютный «%1»").arg(path));
        CHECK(SavedPath::normalized(path) == QString::fromUtf16(c.normalized),
              QStringLiteral("нормализация «%1» → «%2»").arg(path, SavedPath::normalized(path)));
#ifdef Q_OS_WIN
        const bool foreign = c.style == Style::Posix;
#else
        const bool foreign = SavedPath::isWindows(c.style);
#endif
        CHECK(SavedPath::isForeign(path) == foreign, QStringLiteral("чужой ОС «%1»").arg(path));
    }
    CHECK(SavedPath::same(QStringLiteral("C:/Logs/App.log"), QStringLiteral("c:/logs/app.log"))
              && SavedPath::same(QStringLiteral("//Server/Share/a.log"), QStringLiteral("//server/share/A.LOG"))
              && SavedPath::same(QStringLiteral("C:/Логи/a.log"), QStringLiteral("c:/логи/a.log")),
          "путь Windows сравнивается без учёта регистра на любой ОС");
    CHECK(!SavedPath::same(QStringLiteral("C:/logs/a.log"), QStringLiteral("C:/logs/b.log"))
              && !SavedPath::same(QStringLiteral("/logs/a.log"), QStringLiteral("C:/logs/a.log")),
          "разные пути различаются");
#ifdef Q_OS_WIN
    CHECK(SavedPath::same(QStringLiteral("/var/Log/a.log"), QStringLiteral("/var/log/a.log")),
          "POSIX на Windows — без учёта регистра, как файловая система");
#else
    CHECK(!SavedPath::same(QStringLiteral("/var/Log/a.log"), QStringLiteral("/var/log/a.log")),
          "POSIX — с учётом регистра");
#endif
    CHECK(SavedPath::toDisplay(QStringLiteral("C:/logs/app.log")) == QStringLiteral("C:\\logs\\app.log")
              && SavedPath::toDisplay(QStringLiteral("//server/share/a.log")) == QStringLiteral("\\\\server\\share\\a.log")
              && SavedPath::toDisplay(QStringLiteral("/var/log/a.log")) == QStringLiteral("/var/log/a.log"),
          "показ пути");
    // Локальный абсолютный путь (QFileInfo::absoluteFilePath) — родной.
    const QString local = QFileInfo(dir.filePath(QStringLiteral("x.log"))).absoluteFilePath();
    CHECK(SavedPath::isAbsolute(local) && !SavedPath::isForeign(local),
          QStringLiteral("локальный путь «%1» — родной").arg(local));

    // SourceIdentity из файла с другой ОС: читается, путь нормализован,
    // круговой проход держит его как есть.
    const QString realPath = writeFile(dir, QStringLiteral("saved paths.log"), filler(5000, 's'));
    const std::optional<SourceIdentity> real = SourceIdentity::capture(realPath);
    CHECK(real.has_value(), "снятие для сохранённых путей");
    if (!real)
        return;
    for (const QString& path : { QStringLiteral("C:\\logs\\app.log"), QStringLiteral("//127.0.0.1/share/app.log"),
                                 QStringLiteral("\\\\?\\C:\\logs\\app.log"), QStringLiteral("/var/log/app.log") }) {
        QJsonObject json = real->toJson();
        json.insert("path", path);
        QString error;
        const std::optional<SourceIdentity> read = SourceIdentity::fromJson(json, &error);
        CHECK(read.has_value(), QStringLiteral("путь «%1» читается на любой ОС: %2").arg(path, error));
        if (!read)
            continue;
        CHECK(read->path == SavedPath::normalized(path), QStringLiteral("путь «%1» нормализован").arg(path));
        const std::optional<SourceIdentity> again = SourceIdentity::fromJson(read->toJson());
        CHECK(again && *again == *read, QStringLiteral("круговой проход «%1»").arg(path));
        if (SavedPath::isForeign(path))
            expectState(*read, State::Missing, QStringLiteral("путь другой ОС «%1» — Missing").arg(path));
        else
            expectState(*read, State::Missing, QStringLiteral("файла «%1» нет").arg(path));
        // Содержимое по родному пути проверяется как обычно.
        CHECK(read->checkAt(realPath) == State::Valid, QStringLiteral("по родному пути — Valid (%1)").arg(path));
    }

#ifndef Q_OS_WIN
    // Ловушка: «C:/logs/app.log» на Linux для QFileInfo — относительный путь.
    // Файл с тем же содержимым по «<текущий каталог>/C:/logs/app.log» не
    // должен за него сойти.
    const QString cwd = QDir::currentPath();
    QDir().mkpath(dir.filePath(QStringLiteral("C:/logs")));
    QFile::copy(realPath, dir.filePath(QStringLiteral("C:/logs/app.log")));
    QDir::setCurrent(dir.path());
    CHECK(QFileInfo(QStringLiteral("C:/logs/app.log")).isFile(), "ловушка: относительный файл есть");
    SourceIdentity trap = *real;
    trap.path = QStringLiteral("C:/logs/app.log");
    CHECK(trap.check() == State::Missing, "путь Windows на Linux не читается как относительный");
    QDir::setCurrent(cwd);
#else
    // Ловушка: «/var/log/app.log» на Windows для QFileInfo — путь на текущем
    // диске.
    const QString cwd = QDir::currentPath();
    const QString drive = QDir(dir).absolutePath().left(2); // «C:»
    QDir::setCurrent(dir.path());
    const QString trapDir = QStringLiteral("/dendrolog-r8-trap-%1").arg(QCoreApplication::applicationPid());
    SourceIdentity trap = *real;
    trap.path = trapDir + QStringLiteral("/app.log");
    // Корень диска может быть закрыт на запись — тогда ловушку не поставить.
    if (QDir().mkpath(drive + trapDir) && QFile::copy(realPath, drive + trapDir + QStringLiteral("/app.log"))) {
        CHECK(QFileInfo(trap.path).isFile(), "ловушка: файл на текущем диске есть");
        CHECK(trap.check() == State::Missing, "путь POSIX на Windows не читается с текущего диска");
    } else {
        std::fprintf(stdout, "record_identity: корень %s закрыт на запись, ловушка POSIX-пути пропущена\n",
                     qPrintable(drive));
    }
    QFile::remove(drive + trapDir + QStringLiteral("/app.log"));
    QDir().rmdir(drive + trapDir);
    QDir::setCurrent(cwd);
#endif
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    TimingApp app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() >= 3 && args[1] == QLatin1String("--bench")) {
        const int k = args.size() >= 4 && args[3].toInt() > 0 ? args[3].toInt() : 1000;
        return runBench(app, args[2], k, args.contains(QLatin1String("--resident")));
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL: temp dir\n");
        return 1;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();

    testSourceIdentity(dir);
    testSmallFiles(dir);
    testFileChangeDetector(dir);
    testEphemeral(dir);
    testJson(dir);
    testSavedPaths(dir);
    testLineFingerprints(dir, schema);
    testLocatorRoundTrip(dir, schema);
    testLocatorRefusals(dir, schema);
    testLocatorPartialLine(dir, schema);
    testLocatorEncodings(dir, schema);
    testLocatorBatchCost(dir, schema);

    if (failures) {
        std::fprintf(stderr, "record_identity: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stdout, "record_identity: OK\n");
    return 0;
}
