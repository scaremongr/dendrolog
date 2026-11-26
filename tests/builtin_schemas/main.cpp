// Встроенные схемы полей (resources/patterns/*.json) и их установка.
//
//   1. Каждая встроенная схема на образцах настоящих строк своего формата
//      извлекает поля с ожидаемыми значениями, а строки-продолжения
//      (стек-трейсы, traceback) не принимает за начало записи.
//   2. Файл из тех же образцов, загруженный обоими путями хранилища
//      (LogParser и LogIndexer), даёт одинаковые границы записей и те же поля.
//   3. Установка BuiltinSchemas::install: пустой каталог, повторный запуск,
//      исправленная встроенная схема в новой версии, изменённая и удалённая
//      пользователем схема, чужой файл того же имени, новая схема в новой
//      версии, потерянный манифест.
//   4. Каждый файл схемы перечислен в resources/resources.qrc.
//   5. Метки времени форматов распознаёт LineClassifier (F-10), а схемы,
//      исправленные ради этого после Q-5 (q5/ — их прежние версии),
//      доезжают до пользователя, который их не трогал.
//
// Каталог схем в исходниках передаётся define DENDROLOG_PATTERNS_SOURCE_DIR.

#include "builtinschemas.h"
#include "lineclassifier.h"
#include "logpattern.h"
#include "patternheuristics.h"
#include "builtinsamples.h"
#include "testdocuments.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const QString& what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "[FAIL] %s\n", qPrintable(what));
        std::fflush(stderr);
    }
}

const QString kSourceDir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);

// Образцы строк форматов — общий корпус, tests/common/builtinsamples.h.
using namespace builtinsamples;

QByteArray readAll(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QString schemaOf(const QString& file, QString* name = nullptr)
{
    QString canonical;
    BuiltinSchemas::parseSchemaFile(readAll(QDir(kSourceDir).filePath(file)),
                                    QString(), name, &canonical, true);
    return canonical;
}

QString describeFields(const LogPattern& pattern, const QString& line, const LogEntryFields& f)
{
    QStringList parts;
    const QStringList names = pattern.fieldNames();
    for (int i = 0; i < names.size(); ++i)
        parts << QStringLiteral("%1=«%2»").arg(names[i], f.value(i, line));
    return parts.join(QStringLiteral(", "));
}

// Поля строки против ожидаемых; пустой результат — совпало.
QString compareFields(const LogPattern& pattern, const QString& line,
                      const LogEntryFields& f, const Fields& expected)
{
    const QStringList names = pattern.fieldNames();
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        if (!names.contains(it.key()))
            return QStringLiteral("в схеме нет поля %1").arg(it.key());
    }
    for (int i = 0; i < names.size(); ++i) {
        const QString got = f.value(i, line);
        const QString want = expected.value(names[i]);
        if (got != want)
            return QStringLiteral("поле %1: «%2», ожидалось «%3»; все поля: %4")
                .arg(names[i], got, want, describeFields(pattern, line, f));
    }
    return QString();
}

// ---------------------------------------------------------------------------
// 1. Схемы на образцах
// ---------------------------------------------------------------------------

void testSchemaFiles()
{
    const QStringList files =
        QDir(kSourceDir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    QStringList covered;
    for (const SchemaCase& sc : cases())
        covered << sc.file;
    covered.sort();
    check(files == covered,
          QStringLiteral("каждая встроенная схема покрыта образцами: файлы %1, в тесте %2")
              .arg(files.join(QLatin1Char(' ')), covered.join(QLatin1Char(' '))));

    QStringList names;
    for (const QString& file : files) {
        const QJsonObject obj = QJsonDocument::fromJson(readAll(QDir(kSourceDir).filePath(file))).object();
        check(!obj.value(QStringLiteral("description")).toString().trimmed().isEmpty(),
              QStringLiteral("%1: есть описание").arg(file));
        QString name;
        const QString canonical = schemaOf(file, &name);
        check(!canonical.isEmpty(), QStringLiteral("%1: строгая проверка схемы").arg(file));
        check(!name.isEmpty() && !names.contains(name),
              QStringLiteral("%1: имя «%2» задано и уникально").arg(file, name));
        names << name;
    }
}

void testSamples(const SchemaCase& sc)
{
    QString name;
    const QString schema = schemaOf(sc.file, &name);
    check(name == sc.name, QStringLiteral("%1: имя «%2», ожидалось «%3»").arg(sc.file, name, sc.name));
    const LogPattern pattern(schema);
    check(pattern.isValid(), QStringLiteral("%1: схема компилируется").arg(sc.file));
    if (!pattern.isValid())
        return;

    int records = 0;
    for (const Sample& s : sc.samples) {
        const LogEntryFields f = pattern.extractFields(s.line);
        if (s.continuation) {
            check(f.isEmpty(), QStringLiteral("%1: продолжение не принято за запись: %2 → %3")
                                   .arg(sc.file, s.line, describeFields(pattern, s.line, f)));
            continue;
        }
        ++records;
        const QString diff = compareFields(pattern, s.line, f, s.fields);
        check(diff.isEmpty(), QStringLiteral("%1: %2\n       %3").arg(sc.file, s.line, diff));
    }
    check(records >= 5, QStringLiteral("%1: не меньше 5 образцов записей (%2)").arg(sc.file).arg(records));
}

// ---------------------------------------------------------------------------
// 2. Оба бэкенда: границы записей и поля
// ---------------------------------------------------------------------------

bool expectsTime(const SchemaCase& sc, const Sample& s)
{
    // Время без даты (Serilog console, Logback HH:mm:ss.SSS) — метка с F-11:
    // дату даёт состояние разбора файла.
    Q_UNUSED(s);
    return sc.file != QStringLiteral("python-basicconfig.json");   // времени нет вовсе
}

bool isKeyedCase(const SchemaCase& sc)
{
    return LogPattern(schemaOf(sc.file)).isKeyed();
}

// Ключевая схема (F-4): момент и «распознана» — из ключей
// (LineClassifier::classifyKeyed), заданный момент каждой записи; строки
// продолжений схема не распознаёт. Формат строки по образцу
// (PatternHeuristics::detectLineFormat) — формат схемы, у регексных
// схем — «блоки».
void testKeyedClassification(const SchemaCase& sc)
{
    const LogPattern pattern(schemaOf(sc.file));
    const bool keyed = pattern.isKeyed();
    PatternHeuristics::LineFormat expected = PatternHeuristics::LineFormat::Blocks;
    if (keyed)
        expected = pattern.keyed()->format == KeyedDefinition::Format::Logfmt
            ? PatternHeuristics::LineFormat::Logfmt : PatternHeuristics::LineFormat::Json;
    const LineClassifier classifier;
    for (const Sample& s : sc.samples) {
        if (s.continuation)
            continue;
        check(PatternHeuristics::detectLineFormat(s.line) == expected,
              QStringLiteral("%1: формат строки по образцу: %2").arg(sc.file, s.line));
        if (!keyed)
            continue;
        const KeyedClassification k = classifier.classifyKeyed(pattern, s.line);
        check(k.matched && k.hasTs && k.tsMs == s.ms,
              QStringLiteral("%1: момент из ключа %2, ожидалось %3: %4")
                  .arg(sc.file).arg(k.tsMs).arg(s.ms).arg(s.line));
        check(k.hasLevel, QStringLiteral("%1: уровень из ключа: %2").arg(sc.file, s.line));
    }
    if (!keyed)
        return;
    for (const Sample& s : sc.samples)
        if (s.continuation)
            check(!classifier.classifyKeyed(pattern, s.line).matched,
                  QStringLiteral("%1: продолжение не распознано: %2").arg(sc.file, s.line));
    check(pattern.fieldNames().mid(0, 3) == QStringList({"Timestamp", "Level", "Message"}),
          QStringLiteral("%1: колонки ролей").arg(sc.file));
}

void testClassifierTime(const SchemaCase& sc)
{
    if (isKeyedCase(sc))
        return;   // время — из ключа: testKeyedClassification
    const LineClassifier classifier;
    for (const Sample& s : sc.samples) {
        if (s.continuation)
            continue;
        qint64 ms = 0;
        // Контекст разбора файла (F-11): без него время без даты не метка.
        ClockDates clock;
        const bool has = classifier.detectTimestampMs(s.line, ms, {}, nullptr, &clock);
        const bool expectedTime = expectsTime(sc, s);
        check(has == expectedTime, QStringLiteral("%1: метка времени %2 распознаётся классификатором: %3")
                                       .arg(sc.file, expectedTime ? QString() : QStringLiteral("НЕ"), s.line));
    }
}

struct RowInfo {
    int     recordId = -1;
    qint64  ms = kNoTimestampMs; // момент записи (logentry.h)
    QString fieldsText;   // поля строки, как их отдаёт хранилище
};

// Строки документа по номеру строки файла (порядок вкладки у бэкендов может
// различаться: резидентный сортирует по времени).
QMap<int, RowInfo> rowsByLine(testdocs::Document& doc, const LogPattern& pattern)
{
    QMap<int, RowInfo> rows;
    const int n = doc.model->rowCount();
    for (int r = 0; r < n; ++r) {
        const std::shared_ptr<LogEntry> e = doc.model->entryAt(r);
        if (!e)
            continue;
        RowInfo info;
        info.recordId = e->logicalEntryId();
        info.ms = e->timestamp().isValid() ? e->timestamp().toMSecsSinceEpoch() : kNoTimestampMs;
        info.fieldsText = e->fields().isEmpty()
            ? QString()
            : describeFields(pattern, e->message(), e->fields());
        rows.insert(e->originalLineNumber(), info);
    }
    return rows;
}

void testBackends(const SchemaCase& sc, const QDir& tmp)
{
    const QString schema = schemaOf(sc.file);
    const LogPattern pattern(schema);

    QByteArray bytes;
    for (const Sample& s : sc.samples)
        bytes += s.line.toUtf8() + '\n';
    const QString path = testdocs::writeFile(tmp, sc.file + QStringLiteral(".log"), bytes);
    check(!path.isEmpty(), QStringLiteral("%1: файл образцов записан").arg(sc.file));

    testdocs::Document resident = testdocs::buildResident({path}, schema);
    testdocs::Document indexed = testdocs::buildIndexed({path}, schema);
    check(resident.errors.isEmpty() && indexed.errors.isEmpty(),
          QStringLiteral("%1: загрузка без ошибок: %2")
              .arg(sc.file, (resident.errors + indexed.errors).join(QStringLiteral("; "))));

    const QMap<int, RowInfo> res = rowsByLine(resident, pattern);
    const QMap<int, RowInfo> idx = rowsByLine(indexed, pattern);
    check(res.size() == sc.samples.size() && idx.size() == sc.samples.size(),
          QStringLiteral("%1: строк %2 / %3, ожидалось %4")
              .arg(sc.file).arg(res.size()).arg(idx.size()).arg(sc.samples.size()));
    if (res.size() != sc.samples.size() || idx.size() != sc.samples.size())
        return;

    const QList<int> lines = res.keys();
    check(lines == idx.keys(), QStringLiteral("%1: одни и те же номера строк").arg(sc.file));
    for (int i = 0; i < lines.size(); ++i) {
        const Sample& s = sc.samples[i];
        const RowInfo& r = res[lines[i]];
        const RowInfo& x = idx[lines[i]];
        const bool resStarts = i == 0 || r.recordId != res[lines[i - 1]].recordId;
        const bool idxStarts = i == 0 || x.recordId != idx[lines[i - 1]].recordId;
        check(resStarts == !s.continuation,
              QStringLiteral("%1: резидентный, начало записи на строке %2: %3")
                  .arg(sc.file).arg(i + 1).arg(s.line));
        check(idxStarts == !s.continuation,
              QStringLiteral("%1: индексный, начало записи на строке %2: %3")
                  .arg(sc.file).arg(i + 1).arg(s.line));
        check(r.ms == x.ms,
              QStringLiteral("%1: момент строки %2 у бэкендов: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.ms).arg(x.ms));
        if (!s.continuation && s.ms != kNoTimestampMs)
            check(r.ms == s.ms && x.ms == s.ms,
                  QStringLiteral("%1: момент строки %2: %3 | %4, ожидалось %5")
                      .arg(sc.file).arg(i + 1).arg(r.ms).arg(x.ms).arg(s.ms));
        if (!s.continuation)
            check(hasTimestampMs(r.ms) == expectsTime(sc, s),
                  QStringLiteral("%1: у записи строки %2 %3момент: %4")
                      .arg(sc.file).arg(i + 1).arg(expectsTime(sc, s) ? QString() : QStringLiteral("нет "), s.line));
        check(r.fieldsText == x.fieldsText,
              QStringLiteral("%1: поля строки %2 у бэкендов: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.fieldsText, x.fieldsText));
        if (!s.continuation) {
            const LogEntryFields f = pattern.extractFields(s.line);
            check(r.fieldsText == describeFields(pattern, s.line, f),
                  QStringLiteral("%1: поля строки %2 в хранилище как у схемы").arg(sc.file).arg(i + 1));
        }
    }
}

// F-12. Без включённого извлечения полей (по умолчанию) границы записей
// решает классификатор: у форматов, где у каждой записи есть метка с датой,
// они обязаны совпасть с границами по схеме, а момент — быть у каждой записи.
// Прежде access-лог и syslog без слов уровня становились одной записью.
// Пустая схема = схема не участвует, как при выключенном Filter blocks.
void testBackendsWithoutExtraction(const SchemaCase& sc, const QDir& tmp)
{
    if (isKeyedCase(sc))
        return;   // эпоху числом и уровень из ключа без схемы не видно (F-4)
    for (const Sample& s : sc.samples)
        if (!s.continuation && !expectsTime(sc, s))
            return;   // запись без метки отличает только схема (basicConfig)

    QByteArray bytes;
    for (const Sample& s : sc.samples)
        bytes += s.line.toUtf8() + '\n';
    const QString path = testdocs::writeFile(tmp, sc.file + QStringLiteral(".noschema.log"), bytes);
    testdocs::Document resident = testdocs::buildResident({path}, QString());
    testdocs::Document indexed = testdocs::buildIndexed({path}, QString());
    check(resident.errors.isEmpty() && indexed.errors.isEmpty(),
          QStringLiteral("%1 без схемы: загрузка без ошибок").arg(sc.file));
    const LogPattern none;
    const QMap<int, RowInfo> res = rowsByLine(resident, none);
    const QMap<int, RowInfo> idx = rowsByLine(indexed, none);
    if (res.size() != sc.samples.size() || idx.size() != sc.samples.size()) {
        check(false, QStringLiteral("%1 без схемы: число строк").arg(sc.file));
        return;
    }
    const QList<int> lines = res.keys();
    for (int i = 0; i < lines.size(); ++i) {
        const Sample& s = sc.samples[i];
        const RowInfo& r = res[lines[i]];
        const RowInfo& x = idx[lines[i]];
        const bool resStarts = i == 0 || r.recordId != res[lines[i - 1]].recordId;
        const bool idxStarts = i == 0 || x.recordId != idx[lines[i - 1]].recordId;
        check(resStarts == !s.continuation && idxStarts == !s.continuation,
              QStringLiteral("%1 без схемы: начало записи на строке %2 (резидентный %3, индексный %4): %5")
                  .arg(sc.file).arg(i + 1).arg(resStarts).arg(idxStarts).arg(s.line));
        check(r.ms == x.ms && (s.continuation || hasTimestampMs(r.ms)),
              QStringLiteral("%1 без схемы: момент строки %2: %3 | %4")
                  .arg(sc.file).arg(i + 1).arg(r.ms).arg(x.ms));
    }
}

// Момент записи распознаёт LineClassifier (F-10): ISO, nginx error
// (dddd/dd/dd), access (dd/Mmm/dddd:…), syslog без года; время без даты
// (Serilog console, Logback HH:mm:ss.SSS) — с F-11, по состоянию разбора
// файла. Python basicConfig времени не пишет вовсе.
// ---------------------------------------------------------------------------
// 3. Установка
// ---------------------------------------------------------------------------

QStringList jsonFiles(const QString& dir)
{
    return QDir(dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
}

// Копия встроенных схем — «версия программы», которую тест меняет.
QString makeSource(const QDir& tmp, const QString& name)
{
    const QString dir = tmp.filePath(name);
    QDir().mkpath(dir);
    for (const QString& f : jsonFiles(kSourceDir))
        QFile::copy(QDir(kSourceDir).filePath(f), QDir(dir).filePath(f));
    return dir;
}

// Меняет в файле схемы значение по JSON-ключу верхнего уровня.
bool patchSchema(const QString& path, const QString& key, const QJsonValue& value)
{
    QJsonObject obj = QJsonDocument::fromJson(readAll(path)).object();
    obj.insert(key, value);
    return writeAll(path, QJsonDocument(obj).toJson());
}

// Добавляет в начало схемы файла линейный префикс — изменение самой схемы.
bool changeSchema(const QString& path, const QString& prefix)
{
    return patchSchema(path, QStringLiteral("linePrefix"), prefix);
}

void testInstall()
{
    QTemporaryDir tmpDir;
    const QDir tmp(tmpDir.path());
    const QStringList all = jsonFiles(kSourceDir);
    check(all.size() >= 8, QStringLiteral("встроенных схем не меньше 8 (%1)").arg(all.size()));

    // Пустой каталог (его ещё нет): ставится всё.
    const QString v1 = makeSource(tmp, QStringLiteral("v1"));
    const QString target = tmp.filePath(QStringLiteral("config/patterns"));
    BuiltinSchemas::InstallReport r = BuiltinSchemas::install(v1, target);
    check(r.errors.isEmpty(), QStringLiteral("установка без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
    check(r.installed == all, QStringLiteral("в пустой каталог ставятся все: %1").arg(r.installed.join(QLatin1Char(' '))));
    check(jsonFiles(target) == all, QStringLiteral("в каталоге все встроенные схемы"));
    check(QFile::exists(QDir(target).filePath(QString::fromLatin1(BuiltinSchemas::kManifestFileName))),
          QStringLiteral("манифест записан"));
    for (const QString& f : all)
        check(readAll(QDir(target).filePath(f)) == readAll(QDir(v1).filePath(f)),
              QStringLiteral("%1 скопирован байт в байт").arg(f));

    // Повторный запуск — ничего не меняется.
    r = BuiltinSchemas::install(v1, target);
    check(!r.changed() && r.declined.isEmpty() && r.errors.isEmpty(),
          QStringLiteral("повторный запуск ничего не делает"));

    // Пользователь изменил одну схему, удалил другую, переформатировал третью.
    const QString edited = QStringLiteral("logback.json");
    const QString removed = QStringLiteral("serilog-console.json");
    const QString reformatted = QStringLiteral("nginx-error.json");
    const QString untouched = QStringLiteral("python-logging.json");
    check(changeSchema(QDir(target).filePath(edited), QStringLiteral("APP ")), QStringLiteral("правка схемы"));
    const QByteArray editedBytes = readAll(QDir(target).filePath(edited));
    check(QFile::remove(QDir(target).filePath(removed)), QStringLiteral("удаление схемы"));
    {
        const QString p = QDir(target).filePath(reformatted);
        const QJsonDocument doc = QJsonDocument::fromJson(readAll(p));
        check(writeAll(p, doc.toJson(QJsonDocument::Compact)), QStringLiteral("переформатирование схемы"));
    }

    // Тот же выпуск: изменённая остаётся, удалённая не воскресает.
    r = BuiltinSchemas::install(v1, target);
    check(!r.changed() && r.errors.isEmpty(), QStringLiteral("правки пользователя не трогаются тем же выпуском"));
    check(readAll(QDir(target).filePath(edited)) == editedBytes, QStringLiteral("изменённая схема цела"));
    check(!QFile::exists(QDir(target).filePath(removed)), QStringLiteral("удалённая схема не воскресла"));

    // Новый выпуск: исправлены все четыре схемы, плюс одна новая.
    const QString v2 = makeSource(tmp, QStringLiteral("v2"));
    for (const QString& f : {edited, removed, reformatted, untouched})
        check(changeSchema(QDir(v2).filePath(f), QStringLiteral("> ")), QStringLiteral("исправление %1 в v2").arg(f));
    const QString added = QStringLiteral("zz-new-format.json");
    QFile::copy(QDir(kSourceDir).filePath(untouched), QDir(v2).filePath(added));
    check(patchSchema(QDir(v2).filePath(added), QStringLiteral("name"), QStringLiteral("New format")),
          QStringLiteral("новая схема в v2"));

    const QString oldUntouched = schemaOf(untouched);
    r = BuiltinSchemas::install(v2, target);
    check(r.errors.isEmpty(), QStringLiteral("обновление без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
    check(r.installed == QStringList{added}, QStringLiteral("новая схема выпуска ставится: %1").arg(r.installed.join(QLatin1Char(' '))));
    check(r.updated == QStringList({reformatted, untouched}),
          QStringLiteral("обновляются только нетронутые (и переформатированная): %1").arg(r.updated.join(QLatin1Char(' '))));
    check(readAll(QDir(target).filePath(untouched)) == readAll(QDir(v2).filePath(untouched)),
          QStringLiteral("нетронутая схема заменена исправленной"));
    check(readAll(QDir(target).filePath(edited)) == editedBytes, QStringLiteral("изменённая пользователем не перезаписана"));
    check(!QFile::exists(QDir(target).filePath(removed)), QStringLiteral("удалённая не воскресла в новом выпуске"));
    bool replacedOk = false;
    for (const auto& pr : r.replaced)
        replacedOk |= pr.first == oldUntouched && pr.second != oldUntouched && !pr.second.isEmpty();
    check(r.replaced.size() == 2 && replacedOk, QStringLiteral("замены сообщены парами старая → новая"));

    r = BuiltinSchemas::install(v2, target);
    check(!r.changed(), QStringLiteral("после обновления повторный запуск ничего не делает"));

    // Каталог, где у пользователя уже есть свои схемы: файл того же имени и
    // схема с тем же отображаемым именем в файле с другим именем.
    const QString target2 = tmp.filePath(QStringLiteral("config2/patterns"));
    QDir().mkpath(target2);
    const QByteArray ownBytes = "{\"name\": \"My logback\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}";
    check(writeAll(QDir(target2).filePath(QStringLiteral("logback.json")), ownBytes), QStringLiteral("своя схема logback.json"));
    QString pyName;
    schemaOf(untouched, &pyName);
    const QByteArray sameNameBytes = QStringLiteral("{\"name\": \"%1\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}")
                                         .arg(pyName).toUtf8();
    check(writeAll(QDir(target2).filePath(QStringLiteral("mine.json")), sameNameBytes), QStringLiteral("своя схема с тем же именем"));
    // Ровно встроенная (например, импортированная раньше вручную).
    QFile::copy(QDir(v1).filePath(reformatted), QDir(target2).filePath(reformatted));

    r = BuiltinSchemas::install(v1, target2);
    check(r.errors.isEmpty(), QStringLiteral("установка рядом со своими без ошибок"));
    check(r.declined == QStringList({edited, untouched}),
          QStringLiteral("свои файл и имя отклоняют встроенную: %1").arg(r.declined.join(QLatin1Char(' '))));
    check(!r.installed.contains(reformatted), QStringLiteral("совпадающий файл не переписывается"));
    check(r.installed.size() == all.size() - 3, QStringLiteral("остальные ставятся"));
    check(readAll(QDir(target2).filePath(QStringLiteral("logback.json"))) == ownBytes, QStringLiteral("свой logback.json цел"));
    check(!QFile::exists(QDir(target2).filePath(untouched)), QStringLiteral("дубль имени не создан"));

    // Следующий выпуск: отклонённые так и остаются, совпадавшая обновляется.
    r = BuiltinSchemas::install(v2, target2);
    check(readAll(QDir(target2).filePath(QStringLiteral("logback.json"))) == ownBytes,
          QStringLiteral("свой logback.json цел и после обновления"));
    check(!QFile::exists(QDir(target2).filePath(untouched)), QStringLiteral("отклонённая не появилась в новом выпуске"));
    check(r.updated.contains(reformatted), QStringLiteral("принятая совпадающая схема обновляется"));

    // Потерян манифест: изменённая пользователем не перезаписывается,
    // удалённая возвращается.
    QFile::remove(QDir(target).filePath(QString::fromLatin1(BuiltinSchemas::kManifestFileName)));
    r = BuiltinSchemas::install(v2, target);
    check(readAll(QDir(target).filePath(edited)) == editedBytes,
          QStringLiteral("без манифеста изменённая схема не перезаписана"));
    check(r.installed.contains(removed), QStringLiteral("без манифеста удалённая возвращается"));

    // Каталог назначения нельзя создать — ошибка, без исключений и падений.
    const QString blocker = tmp.filePath(QStringLiteral("file-not-dir"));
    writeAll(blocker, "x");
    r = BuiltinSchemas::install(v1, blocker + QStringLiteral("/patterns"));
    check(!r.errors.isEmpty() && !r.changed(), QStringLiteral("недоступный каталог — ошибка в отчёте"));
}

// ---------------------------------------------------------------------------
// 4. Ресурсы
// ---------------------------------------------------------------------------

// Автоопределение схемы (редактор) узнаёт метки новых видов как блок
// Timestamp: syslog в начале строки, access-лог в скобках, nginx error.
void testAutoDetectTimestamp()
{
    const struct { const char* line; int block; } cases[] = {
        { "Nov 18 13:55:36 web01 sshd[2231]: Accepted publickey for deploy", 0 },
        { "Nov  8 03:04:05 web01 kernel: usb 1-1: new device", 0 },
        { "127.0.0.1 - frank [10/Oct/2026:13:55:36 -0700] \"GET /a HTTP/1.0\" 200 2326", -1 },
        { "2026/11/18 13:55:36 [error] 1234#1234: *99 open() failed", 0 },
    };
    for (const auto& c : cases) {
        const QString line = QString::fromUtf8(c.line);
        const PatternDefinition def = PatternHeuristics::suggestSchema(line);
        int found = -1;
        for (int b = 0; b < def.blocks.size(); ++b) {
            if (def.blocks[b].matchKind == PatternBlock::MatchKind::Timestamp) {
                found = b;
                break;
            }
        }
        check(c.block >= 0 ? found == c.block : found > 0,
              QStringLiteral("автоопределение: блок Timestamp (%1): %2").arg(found).arg(line));
    }
    // Блок Timestamp берёт всю метку access-лога вместе с зоной.
    const QRegularExpression re(QRegularExpression::anchoredPattern(PatternHeuristics::timestampRegex()));
    for (const char* t : { "10/Oct/2026:13:55:36 -0700", "18/Nov/2026:13:55:36", "Nov 18 13:55:36",
                           "Nov  8 03:04:05", "2026/11/18 13:55:36", "13:55:36" })
        check(re.match(QString::fromLatin1(t)).hasMatch(),
              QStringLiteral("timestampRegex принимает «%1»").arg(QString::fromLatin1(t)));
    for (const char* t : { "Foo 18 13:55:36", "18/Foo/2026:13:55:36", "Nov 18 13:55",
                           "Nov  18 13:55:36" })
        check(!re.match(QString::fromLatin1(t)).hasMatch(),
              QStringLiteral("timestampRegex не принимает «%1»").arg(QString::fromLatin1(t)));
}

// Схемы, исправленные после Q-5 ради блока Timestamp (F-10): у
// пользователя, который поставил их прежние версии и не трогал, они
// заменяются новыми; изменённая пользователем прежняя версия остаётся.
void testUpgradeFromQ5()
{
    const QString q5Dir = QStringLiteral(DENDROLOG_Q5_PATTERNS_DIR);
    const QStringList fixed = jsonFiles(q5Dir);
    check(fixed == QStringList({QStringLiteral("access-combined.json"),
                                QStringLiteral("syslog-rfc3164.json")}),
          QStringLiteral("прежние версии исправленных схем: %1").arg(fixed.join(QLatin1Char(' '))));
    for (const QString& f : fixed)
        check(readAll(QDir(q5Dir).filePath(f)) != readAll(QDir(kSourceDir).filePath(f)),
              QStringLiteral("%1 в этом выпуске изменена").arg(f));

    QTemporaryDir tmpDir;
    const QDir tmp(tmpDir.path());
    const QString v0 = makeSource(tmp, QStringLiteral("q5"));
    for (const QString& f : fixed) {
        QFile::remove(QDir(v0).filePath(f));
        QFile::copy(QDir(q5Dir).filePath(f), QDir(v0).filePath(f));
    }
    for (const bool userEdited : {false, true}) {
        const QString target = tmp.filePath(userEdited ? QStringLiteral("edited/patterns")
                                                       : QStringLiteral("untouched/patterns"));
        BuiltinSchemas::InstallReport r = BuiltinSchemas::install(v0, target);
        check(r.errors.isEmpty() && r.installed.size() == jsonFiles(kSourceDir).size(),
              QStringLiteral("прежний выпуск установлен"));
        const QString edited = QDir(target).filePath(fixed.first());
        if (userEdited)
            check(changeSchema(edited, QStringLiteral("APP ")), QStringLiteral("правка прежней схемы"));
        const QByteArray editedBytes = readAll(edited);

        r = BuiltinSchemas::install(kSourceDir, target);
        check(r.errors.isEmpty(), QStringLiteral("обновление без ошибок: %1").arg(r.errors.join(QStringLiteral("; "))));
        check(r.installed.isEmpty(), QStringLiteral("новых схем нет"));
        const QStringList expectedUpdated = userEdited ? fixed.mid(1) : fixed;
        check(r.updated == expectedUpdated,
              QStringLiteral("обновлены %1, ожидалось %2")
                  .arg(r.updated.join(QLatin1Char(' ')), expectedUpdated.join(QLatin1Char(' '))));
        for (const QString& f : expectedUpdated) {
            check(readAll(QDir(target).filePath(f)) == readAll(QDir(kSourceDir).filePath(f)),
                  QStringLiteral("%1 заменена исправленной").arg(f));
            QString name;
            const QString canonical = schemaOf(f, &name);
            check(LogPattern(canonical).isValid()
                      && canonical.contains(QStringLiteral("timestamp")),
                  QStringLiteral("%1: поле времени — блок Timestamp").arg(f));
        }
        if (userEdited)
            check(readAll(edited) == editedBytes, QStringLiteral("изменённая пользователем прежняя схема цела"));
    }
}

// Встроенные ключевые схемы (F-4, PR 4) доезжают до пользователя прежнего
// выпуска: ставятся рядом с его схемами, своя схема с тем же файлом или
// именем их отклоняет; пересохранение нетронутой не делает её «изменённой».
void testKeyedArrival()
{
    const QStringList keyedFiles = {QStringLiteral("json-lines.json"), QStringLiteral("logfmt.json")};
    for (const QString& f : keyedFiles) {
        const QJsonObject obj = QJsonDocument::fromJson(readAll(QDir(kSourceDir).filePath(f))).object();
        check(obj.value(QStringLiteral("version")).toInt() == 3 && obj.contains(QStringLiteral("keyed")),
              QStringLiteral("%1: формат версии 3 с ключевой частью").arg(f));
        const QJsonArray blocks = obj.value(QStringLiteral("blocks")).toArray();
        check(blocks.size() == 1
                  && blocks[0].toObject().value(QStringLiteral("name")).toString() == QLatin1String("Message"),
              QStringLiteral("%1: запасной блок Message для версий без F-4").arg(f));
        check(LogPattern(schemaOf(f)).isKeyed(), QStringLiteral("%1: ключевая схема").arg(f));
    }

    QTemporaryDir tmpDir;
    const QDir tmp(tmpDir.path());
    // Прежний выпуск — без ключевых схем.
    const QString old = makeSource(tmp, QStringLiteral("v06"));
    for (const QString& f : keyedFiles)
        QFile::remove(QDir(old).filePath(f));

    const QString target = tmp.filePath(QStringLiteral("arrive/patterns"));
    BuiltinSchemas::InstallReport r = BuiltinSchemas::install(old, target);
    check(r.errors.isEmpty() && !r.installed.contains(keyedFiles[0]), QStringLiteral("прежний выпуск поставлен"));
    r = BuiltinSchemas::install(kSourceDir, target);
    check(r.errors.isEmpty() && r.installed == keyedFiles && r.updated.isEmpty(),
          QStringLiteral("доезжают обе ключевые схемы: %1").arg(r.installed.join(QLatin1Char(' '))));
    for (const QString& f : keyedFiles)
        check(readAll(QDir(target).filePath(f)) == readAll(QDir(kSourceDir).filePath(f)),
              QStringLiteral("%1 скопирован байт в байт").arg(f));
    r = BuiltinSchemas::install(kSourceDir, target);
    check(!r.changed() && r.declined.isEmpty(), QStringLiteral("ключевые: повторный запуск ничего не делает"));

    // Пересохранение каноническим определением (так пишет каталог схем) —
    // та же схема: следующий выпуск её обновит.
    {
        const QString p = QDir(target).filePath(keyedFiles[0]);
        QJsonObject obj = QJsonDocument::fromJson(readAll(p)).object();
        const QJsonObject canonical = QJsonDocument::fromJson(schemaOf(keyedFiles[0]).toUtf8()).object();
        for (auto it = canonical.begin(); it != canonical.end(); ++it)
            obj.insert(it.key(), it.value());
        check(writeAll(p, QJsonDocument(obj).toJson(QJsonDocument::Compact)), QStringLiteral("пересохранение"));
        const QString v2 = makeSource(tmp, QStringLiteral("v2keyed"));
        QJsonObject fixedKeyed = obj.value(QStringLiteral("keyed")).toObject();
        fixedKeyed.insert(QStringLiteral("extraColumns"), QJsonArray({QStringLiteral("logger")}));
        check(patchSchema(QDir(v2).filePath(keyedFiles[0]), QStringLiteral("keyed"), fixedKeyed),
              QStringLiteral("исправление ключевой части в новом выпуске"));
        r = BuiltinSchemas::install(v2, target);
        check(r.updated == QStringList{keyedFiles[0]},
              QStringLiteral("пересохранённая нетронутая обновляется: %1").arg(r.updated.join(QLatin1Char(' '))));
    }

    // У пользователя уже есть свой logfmt.json и своя схема «JSON Lines».
    const QString target2 = tmp.filePath(QStringLiteral("own/patterns"));
    QDir().mkpath(target2);
    const QByteArray ownLogfmt = "{\"name\": \"My logfmt\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}";
    const QByteArray ownJson = "{\"name\": \"JSON Lines\", \"blocks\": [{\"name\": \"Message\", \"kind\": \"remainder\"}]}";
    check(writeAll(QDir(target2).filePath(QStringLiteral("logfmt.json")), ownLogfmt), QStringLiteral("свой logfmt.json"));
    check(writeAll(QDir(target2).filePath(QStringLiteral("my-json.json")), ownJson), QStringLiteral("своя «JSON Lines»"));
    r = BuiltinSchemas::install(old, target2);
    r = BuiltinSchemas::install(kSourceDir, target2);
    check(r.declined == keyedFiles,
          QStringLiteral("свои файл и имя отклоняют ключевые: %1").arg(r.declined.join(QLatin1Char(' '))));
    check(readAll(QDir(target2).filePath(QStringLiteral("logfmt.json"))) == ownLogfmt, QStringLiteral("свой logfmt.json цел"));
    check(!QFile::exists(QDir(target2).filePath(QStringLiteral("json-lines.json"))), QStringLiteral("дубль имени не создан"));
}

void testQrc()
{
    const QString qrc = QString::fromUtf8(readAll(QDir(kSourceDir).filePath(QStringLiteral("../resources.qrc"))));
    check(!qrc.isEmpty(), QStringLiteral("resources.qrc читается"));
    for (const QString& f : jsonFiles(kSourceDir))
        check(qrc.contains(QStringLiteral("<file alias=\"%1\">patterns/%1</file>").arg(f)),
              QStringLiteral("%1 есть в resources.qrc под префиксом /patterns").arg(f));
    check(qrc.contains(QStringLiteral("<qresource prefix=\"/patterns\">")), QStringLiteral("префикс /patterns"));
}

// Что предлагает автоопределение на первой записи — для сравнения в отчёте
// (не проверка: встроенная схема должна быть не хуже, это видно глазами).
void printAutodetect(const SchemaCase& sc)
{
    if (qEnvironmentVariableIsEmpty("DENDRO_SHOW_AUTODETECT"))
        return;
    const LogPattern p(LogPattern::serializeDefinition(PatternHeuristics::suggestSchema(sc.samples.first().line)));
    std::fprintf(stderr, "%s: автоопределение: %s\n", qPrintable(sc.file),
                 qPrintable(describeFields(p, sc.samples.first().line, p.extractFields(sc.samples.first().line))));
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testSchemaFiles();
    QTemporaryDir tmpDir;
    for (const SchemaCase& sc : cases()) {
        printAutodetect(sc);
        testSamples(sc);
        testBackends(sc, QDir(tmpDir.path()));
        testBackendsWithoutExtraction(sc, QDir(tmpDir.path()));
        testClassifierTime(sc);
        testKeyedClassification(sc);
    }
    testInstall();
    testUpgradeFromQ5();
    testKeyedArrival();
    testAutoDetectTimestamp();
    testQrc();

    std::fprintf(stderr, "builtin_schemas: %d проверок, %d провалов\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
