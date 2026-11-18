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
//   • JSON: круговой проход, отказ на чужой версии, алгоритме и битых данных.
// ============================================================================

#include "recordid.h"
#include "stdinspooler.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QStringDecoder>
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

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL: temp dir\n");
        return 1;
    }
    const QDir dir(tmp.path());
    const QString schema = buildSchema();

    testSourceIdentity(dir);
    testSmallFiles(dir);
    testEphemeral(dir);
    testJson(dir);
    testLineFingerprints(dir, schema);

    if (failures) {
        std::fprintf(stderr, "record_identity: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stdout, "record_identity: OK\n");
    return 0;
}
