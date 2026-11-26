// Каталог схем (SchemaStore), перевод каталога в шаблоны строки и редактор
// схем (F-17, шаг 9; метаданные — Q-17).
//
//   1. Описание и неизвестные ключи верхнего уровня проходят load → sync без
//      правок (файл не переписывается), с правкой шаблона, с переименованием;
//      схема без описания не получает пустой ключ "description".
//   2. Правка и переименование пишут в тот же файл; файл с именем, выведенным
//      из прежнего названия схемы, переезжает под новое название.
//   3. Импорт и экспорт переносят метаданные; импортированный файл не
//      дублируется следующей синхронизацией; файл версии 3 импортируется
//      шаблоном.
//   4. Перевод каталога версии 3 (migrateToTemplates): файл — версией 4 под
//      тем же именем, с тем же описанием и неизвестными ключами, оригинал — в
//      converted-from-v3/ байт в байт; непереводимый не тронут; повторный
//      запуск ничего не делает.
//   5. Встроенные схемы по манифесту: у пользователя прежнего выпуска
//      (блочные, tests/builtin_schemas/v3) нетронутые — шаблонами, изменённая
//      пользователем — не тронута установкой и переведена каталогом;
//      пересохранение без правок и правка только описания не делают
//      встроенную «изменённой пользователем», правка шаблона и
//      переименование — делают.
//   6. Окно SchemaEditorWindow: схема каталога, описание, отмена и повтор
//      (QUndoStack, Ctrl+Z), ошибка шаблона, переименование поля — вопрос о
//      правилах фильтров и переименование там, Save в тот же файл (метаданные
//      целы), Save As, New ▾ (From Template, Import Grok), закрытие с
//      правками спрашивает; на вкладке — таблица образцов (число разных
//      значений в заголовке, частые — в подсказке), «Did not match» с
//      причинами, покрытие файла = SchemaFit::evaluate на тех же строках
//      (одна мера), Apply to Tab, Find a Schema панелью.
//   7. Ключевая схема прежних версий в каталоге — шаблон «{Message}», ключевая
//      часть — метаданные.
//
// Каталоги в исходниках — define DENDROLOG_PATTERNS_SOURCE_DIR (поставляемые
// шаблоны) и DENDROLOG_V3_PATTERNS_DIR (блочные прежнего выпуска).

#include "appsettings.h"
#include "builtinschemas.h"
#include "fieldlayout.h"
#include "fieldschemacontroller.h"
#include "linetemplate.h"
#include "logmodel.h"
#include "logviewwidget.h"
#include "LogListView.h"
#include "schemacandidateswindow.h"
#include "schemaconverter.h"
#include "schemaeditorwindow.h"
#include "schemafit.h"
#include "schemasamplemodel.h"
#include "schemastore.h"
#include "v3schema.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QKeyEvent>
#include <QSettings>
#include <QUndoStack>
#include <QAction>
#include <QToolButton>

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

const QString kBuiltinDir = QStringLiteral(DENDROLOG_PATTERNS_SOURCE_DIR);
const QString kV3Dir = QStringLiteral(DENDROLOG_V3_PATTERNS_DIR);

QDir patterns() { return QDir(SchemaStore::patternsDir()); }

void clearPatterns()
{
    QDir dir = patterns();
    const QStringList files = dir.entryList(QDir::Files | QDir::Hidden);
    for (const QString& f : files)
        dir.remove(f);
    QDir(dir.filePath(QString::fromLatin1(SchemaStore::kConvertedDirName))).removeRecursively();
}

bool waitFor(const std::function<bool()>& pred, int ms = 30000)
{
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QByteArray readBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}

QJsonObject readJson(const QString& path)
{
    return QJsonDocument::fromJson(readBytes(path)).object();
}

QStringList schemaFiles()
{
    return patterns().entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
}

const SchemaStore::SchemaEntry* findByName(const SchemaStore::SchemaList& list, const QString& name)
{
    for (const auto& e : list)
        if (e.name == name)
            return &e;
    return nullptr;
}

// Файл схемы, написанный «руками»: шаблон строки (версия 4), описание, ключ,
// которого приложение не знает (объект), и ключ-строка.
QByteArray handWrittenSchema(const QString& name, const QString& description,
                             const QString& templateText = QStringLiteral("{Time:time} {Message}"))
{
    QJsonObject obj{{"version", 4}, {"type", "line-template"}, {"template", templateText}};
    obj.insert(QStringLiteral("name"), name);
    if (!description.isNull())
        obj.insert(QStringLiteral("description"), description);
    obj.insert(QStringLiteral("x-detect"), QJsonObject{{QStringLiteral("sample"), QStringLiteral("2026-01-01 boot")}});
    obj.insert(QStringLiteral("author"), QStringLiteral("ops team"));
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

// То же прежнего формата (версия 3, блоки) — как файл каталога прежнего выпуска.
QByteArray handWrittenV3(const QString& name, const QString& description)
{
    QJsonObject obj = v3schema::object({{QStringLiteral("timestamp"), QStringLiteral("Time")},
                                        {QStringLiteral("remainder"), QStringLiteral("Message")}});
    obj.insert(QStringLiteral("name"), name);
    if (!description.isNull())
        obj.insert(QStringLiteral("description"), description);
    obj.insert(QStringLiteral("x-detect"), QJsonObject{{QStringLiteral("sample"), QStringLiteral("2026-01-01 boot")}});
    obj.insert(QStringLiteral("author"), QStringLiteral("ops team"));
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

QString templateOf(const QString& definition)
{
    QString text;
    LineTemplateSchema::templateOf(definition, &text);
    return text;
}

QStringList columnsOf(const QString& definition)
{
    const LineTemplate t(templateOf(definition));
    return t.isValid() ? t.columns() : QStringList();
}

// Определение с переименованным первым полем — «правка шаблона».
QString editedSchema(const QString& canonical, const QString& firstFieldName)
{
    QString text = templateOf(canonical);
    const int open = int(text.indexOf(QLatin1Char('{')));
    int end = open + 1;
    while (end < text.size() && text[end] != QLatin1Char(':') && text[end] != QLatin1Char('}'))
        ++end;
    if (open >= 0)
        text.replace(open + 1, end - open - 1, firstFieldName);
    return LineTemplateSchema::definition(text);
}

bool hasMeta(const QJsonObject& obj, const QString& description)
{
    return obj.value(QStringLiteral("description")).toString() == description
        && obj.value(QStringLiteral("author")).toString() == QLatin1String("ops team")
        && obj.value(QStringLiteral("x-detect")).toObject().value(QStringLiteral("sample")).toString()
               == QLatin1String("2026-01-01 boot");
}

// ---------------------------------------------------------------- 1, 2

void testStoreRoundTrip()
{
    clearPatterns();
    const QString path = patterns().filePath(QStringLiteral("curated-name.json"));
    const QByteArray original = handWrittenSchema(QStringLiteral("Orders"), QStringLiteral("Orders service log"));
    check(writeBytes(path, original), "write hand-written schema");

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    check(list.size() == 1, "one schema loaded");
    if (list.size() != 1)
        return;
    check(list[0].fileName == QLatin1String("curated-name.json"), "entry remembers its file");
    check(list[0].description() == QLatin1String("Orders service log"), "description loaded");
    check(list[0].meta.contains(QStringLiteral("x-detect")) && list[0].meta.contains(QStringLiteral("author")),
          "unknown keys loaded as metadata");
    check(!list[0].meta.contains(QStringLiteral("template")) && !list[0].meta.contains(QStringLiteral("name"))
              && !list[0].meta.contains(QStringLiteral("version")),
          "format keys are not metadata");

    // Пересохранение без правок: файл не переписывается вовсе.
    check(SchemaStore::sync(list), "sync untouched");
    check(readBytes(path) == original, "untouched schema: file bytes unchanged");

    // Правка блоков: тот же файл, метаданные на месте.
    list[0].schema = editedSchema(list[0].schema, QStringLiteral("When"));
    check(SchemaStore::sync(list), "sync edited");
    check(schemaFiles() == QStringList{QStringLiteral("curated-name.json")}, "edit keeps the curated file");
    QJsonObject obj = readJson(path);
    check(hasMeta(obj, QStringLiteral("Orders service log")), "edit keeps description and unknown keys");
    check(obj.value(QStringLiteral("template")).toString().startsWith(QLatin1String("{When:")), "edit is written");

    // Переименование: курируемое имя файла остаётся.
    list = SchemaStore::loadAll();
    list[0].name = QStringLiteral("Orders v2");
    check(SchemaStore::sync(list), "sync renamed");
    check(schemaFiles() == QStringList{QStringLiteral("curated-name.json")}, "rename keeps the curated file");
    obj = readJson(path);
    check(obj.value(QStringLiteral("name")).toString() == QLatin1String("Orders v2"), "rename is written");
    check(hasMeta(obj, QStringLiteral("Orders service log")), "rename keeps metadata");

    // Правка описания.
    list = SchemaStore::loadAll();
    list[0].setDescription(QStringLiteral("  Orders, v2  "));
    check(SchemaStore::sync(list), "sync description");
    obj = readJson(path);
    check(hasMeta(obj, QStringLiteral("Orders, v2")), "description edit is written (trimmed), other keys kept");

    // Описание стёрто — ключа нет, а не пустая строка.
    list = SchemaStore::loadAll();
    list[0].setDescription(QString());
    check(SchemaStore::sync(list), "sync cleared description");
    obj = readJson(path);
    check(!obj.contains(QStringLiteral("description")), "cleared description removes the key");
    check(obj.contains(QStringLiteral("author")), "clearing description keeps other keys");
}

void testNoDescription()
{
    clearPatterns();
    // Схема, созданная в редакторе: без файла и без метаданных.
    SchemaStore::SchemaEntry entry;
    entry.name = QStringLiteral("Plain");
    entry.schema = editedSchema(
        SchemaStore::parseEntry(handWrittenSchema(QStringLiteral("x"), QString()), QString(), &entry)
            ? entry.schema : QString(), QStringLiteral("T"));
    entry.name = QStringLiteral("Plain");
    entry.meta = QJsonObject();
    check(SchemaStore::sync({entry}), "sync new schema");
    check(schemaFiles() == QStringList{QStringLiteral("Plain.json")}, "new schema gets a file named after it");
    const QJsonObject obj = readJson(patterns().filePath(QStringLiteral("Plain.json")));
    check(!obj.contains(QStringLiteral("description")), "no description key for a schema without one");
    const QStringList keys = obj.keys();
    check(keys == (QStringList{QStringLiteral("name"), QStringLiteral("template"), QStringLiteral("type"),
                               QStringLiteral("version")}),
          QStringLiteral("only format keys and name: %1").arg(keys.join(QLatin1Char(','))));

    // Файл с выведенным из названия именем едет за переименованием.
    SchemaStore::SchemaList list = SchemaStore::loadAll();
    check(list.size() == 1 && list[0].fileName == QLatin1String("Plain.json"), "loaded with file");
    list[0].name = QStringLiteral("Renamed");
    list[0].setDescription(QStringLiteral("now with a note"));
    check(SchemaStore::sync(list), "sync rename of derived file");
    check(schemaFiles() == QStringList{QStringLiteral("Renamed.json")}, "derived file name follows the rename");
    check(readJson(patterns().filePath(QStringLiteral("Renamed.json")))
              .value(QStringLiteral("description")).toString() == QLatin1String("now with a note"),
          "description written to the moved file");

    // Удалённая из списка схема удаляет свой файл.
    check(SchemaStore::sync({}), "sync empty");
    check(schemaFiles().isEmpty(), "removed schema removes its file");
}

// ---------------------------------------------------------------- 2а

// Файл, который каталог не разобрал (схема формата новее, файл, испорченный
// правкой руками), — не наш: sync его не удаляет и не занимает его имя.
// Раньше любая правка каталога (OK в редакторе схем, открытие сессии с новой
// схемой) молча стирала такой файл.
void testUnparsedFilesSurvive()
{
    clearPatterns();
    const QByteArray broken = "{ this is not a schema";
    QJsonObject futureObj = QJsonDocument::fromJson(handWrittenSchema(QStringLiteral("Future"), QString())).object();
    futureObj.insert(QStringLiteral("version"), 99);
    const QByteArray future = QJsonDocument(futureObj).toJson(QJsonDocument::Indented);
    const QString brokenPath = patterns().filePath(QStringLiteral("Broken.json"));
    const QString futurePath = patterns().filePath(QStringLiteral("future.json"));
    check(writeBytes(brokenPath, broken), "write a broken file");
    check(writeBytes(futurePath, future), "write a file of a newer format");
    check(SchemaStore::loadAll().isEmpty(), "unparsed files are not schemas");

    // Новая схема, из названия которой выводится имя занятого файла.
    SchemaStore::SchemaEntry entry;
    check(SchemaStore::parseEntry(handWrittenSchema(QStringLiteral("Broken"), QString()), QString(), &entry),
          "parse the new schema");
    entry.name = QStringLiteral("Broken");
    entry.fileName.clear();
    check(SchemaStore::sync({entry}), "sync with unparsed files in the folder");
    check(readBytes(brokenPath) == broken, "a broken file is neither deleted nor overwritten");
    check(readBytes(futurePath) == future, "a file of a newer format is neither deleted nor overwritten");
    SchemaStore::SchemaList list = SchemaStore::loadAll();
    check(list.size() == 1 && list[0].name == QLatin1String("Broken")
              && list[0].fileName.compare(QLatin1String("Broken.json"), Qt::CaseInsensitive) != 0,
          QStringLiteral("the new schema gets a file of its own: %1")
              .arg(list.isEmpty() ? QStringLiteral("<none>") : list[0].fileName));

    // Правка и удаление схемы трогают только её файл.
    if (!list.isEmpty()) {
        list[0].setDescription(QStringLiteral("edited"));
        check(SchemaStore::sync(list), "sync an edit");
    }
    check(readBytes(brokenPath) == broken && readBytes(futurePath) == future, "an edit leaves unparsed files alone");
    check(SchemaStore::sync({}), "sync empty with unparsed files");
    check(schemaFiles() == (QStringList{QStringLiteral("Broken.json"), QStringLiteral("future.json")}),
          QStringLiteral("a removed schema removes only its own file: %1").arg(schemaFiles().join(QLatin1Char(','))));
    check(readBytes(brokenPath) == broken && readBytes(futurePath) == future, "unparsed files are byte-identical");
}

// ---------------------------------------------------------------- 3

void testImportExport(const QDir& tmp)
{
    clearPatterns();
    const QString src = tmp.filePath(QStringLiteral("shared-schema.json"));
    check(writeBytes(src, handWrittenSchema(QStringLiteral("Shared"), QStringLiteral("From a colleague"))),
          "write import source");
    QString error;
    const SchemaStore::SchemaEntry imported = SchemaStore::importFile(src, &error);
    check(!imported.isEmpty(), "import succeeds: " + error);
    check(imported.description() == QLatin1String("From a colleague"), "import: description in entry");
    check(imported.fileName == QLatin1String("Shared.json"), "import: entry knows its file");
    const QJsonObject inFolder = readJson(patterns().filePath(QStringLiteral("Shared.json")));
    check(hasMeta(inFolder, QStringLiteral("From a colleague")), "import: metadata in the copied file");

    // Редактор показывает импортированную строку сразу и потом синхронизирует.
    SchemaStore::SchemaList list{imported};
    const QByteArray before = readBytes(patterns().filePath(QStringLiteral("Shared.json")));
    check(SchemaStore::sync(list), "sync after import");
    check(schemaFiles() == QStringList{QStringLiteral("Shared.json")}, "import is not duplicated by sync");
    check(readBytes(patterns().filePath(QStringLiteral("Shared.json"))) == before, "import not rewritten");

    const QString out = tmp.filePath(QStringLiteral("exported.json"));
    check(SchemaStore::exportToFile(imported, out, &error), "export: " + error);
    const QJsonObject exported = readJson(out);
    check(hasMeta(exported, QStringLiteral("From a colleague")), "export carries metadata");
    check(exported.value(QStringLiteral("name")).toString() == QLatin1String("Shared"), "export carries name");

    // Файл прежнего формата (версия 3) импортируется шаблоном строки.
    const QString old = tmp.filePath(QStringLiteral("old-schema.json"));
    check(writeBytes(old, handWrittenV3(QStringLiteral("Old shared"), QStringLiteral("Version 3"))), "write v3 source");
    const SchemaStore::SchemaEntry importedOld = SchemaStore::importFile(old, &error);
    check(!importedOld.isEmpty() && FieldLayout::isTemplateDefinition(importedOld.schema)
              && columnsOf(importedOld.schema) == (QStringList{QStringLiteral("Time"), QStringLiteral("Message")}),
          "import of a version 3 file: a line template with the same columns");
    const QJsonObject oldInFolder = readJson(patterns().filePath(importedOld.fileName));
    check(oldInFolder.value(QStringLiteral("version")).toInt() == 4 && hasMeta(oldInFolder, QStringLiteral("Version 3")),
          "import of a version 3 file: written as version 4 with its metadata");
}

// ---------------------------------------------------------------- 4

void testMigration()
{
    clearPatterns();
    const QDir dir = patterns();
    const QByteArray orders = handWrittenV3(QStringLiteral("Orders"), QStringLiteral("Orders service log"));
    QJsonObject badObj = v3schema::object({{QStringLiteral("integer"), QStringLiteral("_")},
                                           {QStringLiteral("remainder"), QStringLiteral("Message")}});
    badObj.insert(QStringLiteral("name"), QStringLiteral("Bad"));
    const QByteArray bad = QJsonDocument(badObj).toJson();
    const QByteArray v4 = handWrittenSchema(QStringLiteral("Already v4"), QStringLiteral("new"));
    check(writeBytes(dir.filePath(QStringLiteral("orders.json")), orders)
              && writeBytes(dir.filePath(QStringLiteral("bad.json")), bad)
              && writeBytes(dir.filePath(QStringLiteral("v4.json")), v4),
          "migration: write the catalogue");

    QElapsedTimer t;
    t.start();
    SchemaStore::MigrationReport r = SchemaStore::migrateToTemplates(dir.path());
    const qint64 firstNs = t.nsecsElapsed();
    check(r.converted == QStringList{QStringLiteral("orders.json")},
          QStringLiteral("migration: the version 3 file is converted (%1)").arg(r.converted.join(',')));
    check(r.failed.size() == 1 && r.failed.first().startsWith(QLatin1String("bad.json")),
          QStringLiteral("migration: an untranslatable file is reported: %1").arg(r.failed.join("; ")));
    const QDir kept(dir.filePath(QString::fromLatin1(SchemaStore::kConvertedDirName)));
    check(readBytes(kept.filePath(QStringLiteral("orders.json"))) == orders,
          "migration: the original is kept byte for byte in converted-from-v3/");
    const QJsonObject converted = readJson(dir.filePath(QStringLiteral("orders.json")));
    check(converted.value(QStringLiteral("version")).toInt() == 4
              && converted.value(QStringLiteral("type")).toString() == QLatin1String("line-template")
              && converted.value(QStringLiteral("name")).toString() == QLatin1String("Orders")
              && !converted.contains(QStringLiteral("blocks")) && !converted.contains(QStringLiteral("linePrefix")),
          "migration: the file is version 4 under the same name");
    check(hasMeta(converted, QStringLiteral("Orders service log")),
          "migration: description and unknown keys are kept");
    check(LineTemplate(converted.value(QStringLiteral("template")).toString()).columns()
              == (QStringList{QStringLiteral("Time"), QStringLiteral("Message")}),
          "migration: the same columns");
    check(readBytes(dir.filePath(QStringLiteral("bad.json"))) == bad && !kept.exists(QStringLiteral("bad.json")),
          "migration: an untranslatable file is left as it was");
    check(readBytes(dir.filePath(QStringLiteral("v4.json"))) == v4, "migration: a version 4 file is not touched");

    // Повторный запуск ничего не делает.
    const QByteArray after = readBytes(dir.filePath(QStringLiteral("orders.json")));
    t.restart();
    r = SchemaStore::migrateToTemplates(dir.path());
    const qint64 againNs = t.nsecsElapsed();
    check(r.converted.isEmpty(), "migration: a second run converts nothing");
    check(readBytes(dir.filePath(QStringLiteral("orders.json"))) == after
              && kept.entryList(QDir::Files).size() == 1,
          "migration: a second run changes no file");
    t.restart();
    const SchemaStore::SchemaList list = SchemaStore::loadAll();
    const qint64 loadNs = t.nsecsElapsed();
    const SchemaStore::SchemaEntry* entry = findByName(list, QStringLiteral("Orders"));
    check(entry && FieldLayout::isTemplateDefinition(entry->schema) && !findByName(list, QStringLiteral("Bad")),
          "migration: the catalogue reads the converted schema; the untranslatable one is not a schema");
    std::printf("migration: first run %.2f ms, second %.2f ms, loadAll %.2f ms (3 files)\n",
                firstNs / 1e6, againNs / 1e6, loadNs / 1e6);

    // Новый файл версии 3 с тем же именем (брошен в каталог руками) —
    // переведён, оригинал рядом с прежним под другим именем.
    check(writeBytes(dir.filePath(QStringLiteral("orders.json")), handWrittenV3(QStringLiteral("Orders"), QString())),
          "migration: drop a version 3 file again");
    r = SchemaStore::migrateToTemplates(dir.path());
    check(r.converted == QStringList{QStringLiteral("orders.json")} && kept.exists(QStringLiteral("orders_2.json"))
              && readBytes(kept.filePath(QStringLiteral("orders.json"))) == orders,
          "migration: an earlier original is never overwritten");
}

// Встроенные схемы у пользователя прежнего выпуска: установка шаблонов по
// манифесту, затем перевод каталога (порядок FieldSchemaController::load).
void testBuiltinsFromV3()
{
    clearPatterns();
    const QString dir = SchemaStore::patternsDir();
    BuiltinSchemas::InstallReport r = BuiltinSchemas::install(kV3Dir, dir);
    check(r.installed.size() == 10, "v3 release: ten block built-ins installed");
    // Пользователь изменил Spring Boot и описание Logback.
    {
        QJsonObject spring = readJson(patterns().filePath(QStringLiteral("spring-boot.json")));
        spring.insert(QStringLiteral("linePrefix"), QStringLiteral("APP "));
        writeBytes(patterns().filePath(QStringLiteral("spring-boot.json")), QJsonDocument(spring).toJson());
        QJsonObject logback = readJson(patterns().filePath(QStringLiteral("logback.json")));
        logback.insert(QStringLiteral("description"), QStringLiteral("My note"));
        writeBytes(patterns().filePath(QStringLiteral("logback.json")), QJsonDocument(logback).toJson());
    }
    const QByteArray springEdited = readBytes(patterns().filePath(QStringLiteral("spring-boot.json")));
    r = BuiltinSchemas::install(kBuiltinDir, dir);
    const SchemaStore::MigrationReport m = SchemaStore::migrateToTemplates(dir);
    check(r.errors.isEmpty() && m.failed.isEmpty(), "update: no errors");
    check(!r.updated.contains(QStringLiteral("spring-boot.json")) && r.updated.contains(QStringLiteral("logback.json")),
          "update: the edited built-in is not replaced; the one with only a new description is");
    check(readBytes(patterns().filePath(QStringLiteral("logback.json")))
              == readBytes(QDir(kBuiltinDir).filePath(QStringLiteral("logback.json"))),
          "update: an untouched built-in is the shipped template");
    check(m.converted == QStringList{QStringLiteral("spring-boot.json")},
          QStringLiteral("update: the edited built-in is converted by the catalogue (%1)").arg(m.converted.join(',')));
    const QDir kept(patterns().filePath(QString::fromLatin1(SchemaStore::kConvertedDirName)));
    check(readBytes(kept.filePath(QStringLiteral("spring-boot.json"))) == springEdited,
          "update: the user's block version is kept in converted-from-v3/");
    const QJsonObject spring = readJson(patterns().filePath(QStringLiteral("spring-boot.json")));
    check(spring.value(QStringLiteral("version")).toInt() == 4
              && spring.value(QStringLiteral("template")).toString().startsWith(QLatin1String("APP "))
              && spring.value(QStringLiteral("name")).toString() == QLatin1String("Spring Boot"),
          QStringLiteral("update: the user's edit survives the conversion: %1")
              .arg(spring.value(QStringLiteral("template")).toString()));
    check(QFile::exists(patterns().filePath(QStringLiteral("syslog.json")))
              && !QFile::exists(patterns().filePath(QStringLiteral("syslog-iso.json")))
              && !QFile::exists(patterns().filePath(QStringLiteral("syslog-rfc3164.json"))),
          "update: the two syslog schemas became one template");
    // Копия встроенной под своим именем и файлом (манифест её не знает):
    // каталог и сохранённые состояния переводят её одинаково — поставляемым
    // шаблоном (одно правило SchemaConverter::translate), сессия находит пункт.
    {
        const QByteArray v3Logback = readBytes(QDir(kV3Dir).filePath(QStringLiteral("logback.json")));
        QJsonObject mine = QJsonDocument::fromJson(v3Logback).object();
        mine.insert(QStringLiteral("name"), QStringLiteral("My Logback"));
        writeBytes(patterns().filePath(QStringLiteral("my-logback.json")), QJsonDocument(mine).toJson());
        const SchemaStore::SchemaList list = SchemaStore::loadAll();
        const SchemaStore::SchemaEntry* e = findByName(list, QStringLiteral("My Logback"));
        check(e && e->schema == SchemaConverter::upgradeDefinition(QString::fromUtf8(v3Logback)),
              "update: a renamed copy of a built-in reads as the same definition as a saved state");
        SchemaStore::migrateToTemplates(dir);
        check(readJson(patterns().filePath(QStringLiteral("my-logback.json"))).value(QStringLiteral("template")).toString()
                  == readJson(QDir(kBuiltinDir).filePath(QStringLiteral("logback.json"))).value(QStringLiteral("template")).toString(),
              "update: the renamed copy is converted to the shipped template");
        QFile::remove(patterns().filePath(QStringLiteral("my-logback.json")));
    }
    // Следующий запуск — ничего.
    r = BuiltinSchemas::install(kBuiltinDir, dir);
    const SchemaStore::MigrationReport again = SchemaStore::migrateToTemplates(dir);
    check(!r.changed() && again.converted.isEmpty(), "update: the next start does nothing");
}


// ---------------------------------------------------------------- 4

// Копия встроенных схем, в которой logback.json «исправлен» в новой версии:
// другое описание и другое имя блока Thread.
QString makeFixedBuiltins(const QDir& tmp)
{
    const QString dir = tmp.filePath(QStringLiteral("builtins-v2"));
    QDir().mkpath(dir);
    const QStringList files = QDir(kBuiltinDir).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& f : files) {
        QFile::remove(QDir(dir).filePath(f));
        QFile::copy(QDir(kBuiltinDir).filePath(f), QDir(dir).filePath(f));
    }
    const QString path = QDir(dir).filePath(QStringLiteral("logback.json"));
    QJsonObject obj = readJson(path);
    obj.insert(QStringLiteral("description"), QStringLiteral("Fixed in v2"));
    obj.insert(QStringLiteral("template"),
               obj.value(QStringLiteral("template")).toString().replace(QStringLiteral("{Thread}"),
                                                                        QStringLiteral("{ThreadV2}")));
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    writeBytes(path, QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return dir;
}

QString threadBlockName(const QString& path)
{
    return LineTemplate(readJson(path).value(QStringLiteral("template")).toString()).columns().value(1);
}

enum class UserAction { Nothing, ResaveUntouched, EditDescription, EditBlocks, Rename };

void testBuiltinManifest(const QDir& tmp, const QString& fixedDir, UserAction action, const char* label)
{
    clearPatterns();
    const QString what = QString::fromLatin1(label);
    BuiltinSchemas::InstallReport first = BuiltinSchemas::install(kBuiltinDir, SchemaStore::patternsDir());
    check(first.installed.contains(QStringLiteral("logback.json")), what + ": logback installed");
    const QString path = patterns().filePath(QStringLiteral("logback.json"));
    const QString shippedDescription = readJson(path).value(QStringLiteral("description")).toString();
    check(!shippedDescription.isEmpty(), what + ": built-in has a description");

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    const SchemaStore::SchemaEntry* lb = findByName(list, QStringLiteral("Logback / Log4j"));
    check(lb && lb->fileName == QLatin1String("logback.json")
              && lb->description() == shippedDescription, what + ": loaded with description and file");
    if (!lb)
        return;
    SchemaStore::SchemaEntry& entry = list[int(lb - list.constData())];
    switch (action) {
    case UserAction::Nothing:
        break;
    case UserAction::ResaveUntouched:
        check(SchemaStore::sync(list), what + ": sync");
        break;
    case UserAction::EditDescription:
        entry.setDescription(QStringLiteral("My own note"));
        check(SchemaStore::sync(list), what + ": sync");
        break;
    case UserAction::EditBlocks:
        entry.schema = editedSchema(entry.schema, QStringLiteral("When"));
        check(SchemaStore::sync(list), what + ": sync");
        break;
    case UserAction::Rename:
        entry.name = QStringLiteral("Our Logback");
        check(SchemaStore::sync(list), what + ": sync");
        break;
    }
    check(QFile::exists(path), what + ": logback.json still in place after save");
    check(readJson(path).value(QStringLiteral("description")).toString()
              == (action == UserAction::EditDescription ? QStringLiteral("My own note") : shippedDescription),
          what + ": description after save");

    // Следующая версия приносит исправленную схему.
    const BuiltinSchemas::InstallReport second = BuiltinSchemas::install(fixedDir, SchemaStore::patternsDir());
    const bool userModified = action == UserAction::EditBlocks || action == UserAction::Rename;
    check(second.updated.contains(QStringLiteral("logback.json")) == !userModified,
          what + (userModified ? ": user-modified copy is not replaced" : ": untouched copy is replaced"));
    check(second.installed.isEmpty() && second.declined.isEmpty() && second.errors.isEmpty(),
          what + ": nothing else happens");
    if (userModified) {
        check(threadBlockName(path) == QLatin1String("Thread"), what + ": user's blocks kept");
        const QJsonObject obj = readJson(path);
        if (action == UserAction::Rename)
            check(obj.value(QStringLiteral("name")).toString() == QLatin1String("Our Logback"),
                  what + ": user's name kept");
        check(obj.value(QStringLiteral("description")).toString() == shippedDescription,
              what + ": user's copy keeps its description");
    } else {
        check(threadBlockName(path) == QLatin1String("ThreadV2"), what + ": fixed blocks arrived");
        check(readJson(path).value(QStringLiteral("description")).toString() == QLatin1String("Fixed in v2"),
              what + ": fixed description arrived");
    }
    // Ещё один запуск той же версии ничего не меняет.
    const BuiltinSchemas::InstallReport third = BuiltinSchemas::install(fixedDir, SchemaStore::patternsDir());
    check(!third.changed() && third.declined.isEmpty(), what + ": stable on the next run");
    Q_UNUSED(tmp);
}

// ---------------------------------------------------------------- 5

template <typename T>
T* child(QWidget* parent, const char* name)
{
    T* w = parent->findChild<T*>(QString::fromLatin1(name));
    check(w != nullptr, QStringLiteral("widget %1 exists").arg(QString::fromLatin1(name)));
    return w;
}

void typeInto(QLineEdit* edit, const QString& text)
{
    edit->setText(text);
    Q_EMIT edit->textEdited(text);
}

// Ответы окна редактора (Asker): по очереди; вопросы запоминаются.
struct Answers {
    QList<SchemaEditorWindow::Answer> queue;
    QStringList questions;
    SchemaEditorWindow::Asker asker()
    {
        return [this](const QString& q, bool) {
            questions << q;
            return queue.isEmpty() ? SchemaEditorWindow::Answer::Cancel : queue.takeFirst();
        };
    }
};

bool sameFit(const SchemaFit::Result& a, const SchemaFit::Result& b)
{
    return a.valid == b.valid && a.examined == b.examined && a.candidates == b.candidates && a.full == b.full
        && a.partial == b.partial && a.missed == b.missed && a.loose == b.loose && a.filled == b.filled
        && a.distinct == b.distinct && a.misses.size() == b.misses.size();
}

void loadController(FieldSchemaController& c)
{
    QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
    ini.beginGroup(QStringLiteral("View"));
    c.load(ini);
}

void testEditor(const QDir& tmp)
{
    using Answer = SchemaEditorWindow::Answer;
    clearPatterns();
    const QString ordersPath = patterns().filePath(QStringLiteral("curated-name.json"));
    writeBytes(ordersPath, handWrittenSchema(QStringLiteral("Orders"), QStringLiteral("Orders service log")));
    writeBytes(patterns().filePath(QStringLiteral("other.json")), handWrittenSchema(QStringLiteral("Other"), QString()));
    const QByteArray otherBefore = readBytes(patterns().filePath(QStringLiteral("other.json")));

    FieldSchemaController c;
    loadController(c);
    SchemaEditorWindow w(&c);
    Answers answers;
    w.setAsker(answers.asker());
    QStringList renamed;
    int uses = 3;
    QStringList renameSchemas;
    w.setFieldUses({[&uses](const QString& field, const QString&) { return field == QLatin1String("Time") ? uses : 0; },
                    [&renamed, &renameSchemas](const QString& from, const QString& to, const QString& schema) {
                        renamed << from + QLatin1Char('>') + to;
                        renameSchemas << schema;
                    }});
    SchemaFit::Budget noLimit;
    noLimit.maxMs = 0;
    w.setFitBudget(noLimit);
    w.show();

    for (const char* name : {"schemaList", "nameEdit", "descriptionEdit", "templateEdit", "sampleTable", "missesTable",
                             "fileFitLabel", "applyToTabBtn", "saveBtn", "saveAsBtn", "closeBtn", "newSchemaBtn",
                             "findSchemaBtn"})
        check(w.findChild<QWidget*>(QString::fromLatin1(name)) != nullptr,
              QStringLiteral("editor: %1 exists").arg(QString::fromLatin1(name)));
    for (const char* name : {"newEmptyAction", "newFromSamplesAction", "newFromTemplateAction", "importGrokAction"})
        check(w.findChild<QAction*>(QString::fromLatin1(name)) != nullptr,
              QStringLiteral("editor: New ▾ — %1").arg(QString::fromLatin1(name)));
    check(w.isVisible() && !w.isModal(), "editor: a non-modal window");

    // Схема каталога.
    check(w.openSchema(QStringLiteral("Orders")) && w.savedName() == QLatin1String("Orders")
              && w.description() == QLatin1String("Orders service log")
              && w.templateText() == QLatin1String("{Time:time} {Message}") && !w.isModified(),
          "editor: opens a schema of the catalogue");

    // Описание: правка, отмена, повтор (QUndoStack), Ctrl+Z в поле.
    w.setDescription(QStringLiteral("draft"));
    check(w.isModified(), "editor: a description edit is a change");
    w.undoStack()->undo();
    check(w.description() == QLatin1String("Orders service log") && !w.isModified(), "editor: undo");
    w.undoStack()->redo();
    check(w.description() == QLatin1String("draft"), "editor: redo");
    if (auto* desc = w.findChild<QLineEdit*>(QStringLiteral("descriptionEdit"))) {
        QKeyEvent ctrlZ(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QApplication::sendEvent(desc, &ctrlZ);
        check(w.description() == QLatin1String("Orders service log") && desc->text() == w.description(),
              "editor: Ctrl+Z in a field undoes through the window's stack");
    }

    // Ошибка шаблона — на месте, словами; сохранить нельзя.
    w.setTemplateText(QStringLiteral("{A:text} {B}"));
    auto* save = w.findChild<QPushButton*>(QStringLiteral("saveBtn"));
    auto* error = w.findChild<QLabel*>(QStringLiteral("templateError"));
    check(!w.templateError().isEmpty() && w.definition().isEmpty() && save && !save->isEnabled()
              && error && error->isVisible() && error->text().contains(QLatin1String("A")),
          QStringLiteral("editor: a template error is shown in words: %1").arg(error ? error->text() : QString()));
    w.undoStack()->undo();
    check(w.templateText() == QLatin1String("{Time:time} {Message}") && w.templateError().isEmpty(),
          "editor: undo of the template");

    // Переименование поля: вопрос о правилах фильтров с этим именем, Yes —
    // переименование там; Save — в тот же файл, метаданные целы.
    w.setTemplateText(QStringLiteral("{When:time} {Message}"));
    answers.queue = {Answer::Yes};
    check(w.save(), "editor: Save");
    check(!answers.questions.isEmpty() && answers.questions.constLast().contains(QLatin1String("Time"))
              && answers.questions.constLast().contains(QLatin1String("When"))
              && answers.questions.constLast().contains(QLatin1String("3")),
          QStringLiteral("editor: renaming a field asks about the rules that use it: %1")
              .arg(answers.questions.value(answers.questions.size() - 1)));
    check(renamed == QStringList{QStringLiteral("Time>When")}
              && renameSchemas == QStringList{LineTemplateSchema::definition(QStringLiteral("{Time:time} {Message}"))},
          "editor: the rules of tabs with the edited schema are renamed too");
    const QJsonObject orders = readJson(ordersPath);
    check(orders.value(QStringLiteral("template")).toString() == QLatin1String("{When:time} {Message}")
              && hasMeta(orders, QStringLiteral("Orders service log")) && !w.isModified(),
          "editor: Save writes the same file and keeps the metadata");
    check(readBytes(patterns().filePath(QStringLiteral("other.json"))) == otherBefore,
          "editor: other schemas are not rewritten");
    // No — поле переименовано только в схеме.
    renamed.clear();
    uses = 0;
    w.setTemplateText(QStringLiteral("{Time:time} {Message}"));
    check(w.save() && renamed.isEmpty(), "editor: a field nothing uses is renamed without a question");

    // Save As — новая схема рядом.
    check(w.saveAs(QStringLiteral("Orders copy")) && w.savedName() == QLatin1String("Orders copy")
              && QFile::exists(patterns().filePath(QStringLiteral("Orders copy.json"))),
          "editor: Save As makes a new schema");
    check(!w.saveAs(QStringLiteral("Orders")), "editor: Save As refuses a taken name");

    // New ▾ → From Template, Import Grok.
    check(w.newFromTemplate(QStringLiteral("{Ts:time} {Level:level} {Message}"), QStringLiteral("Tpl"))
              && w.savedName().isEmpty() && w.isModified(),
          "editor: New from template — an unsaved new schema");
    check(w.save() && QFile::exists(patterns().filePath(QStringLiteral("Tpl.json"))) && w.savedName() == QLatin1String("Tpl"),
          "editor: Save of a new schema writes its file");
    QString grokError;
    check(w.importGrok(QStringLiteral("%{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} %{GREEDYDATA:msg}"), &grokError)
              && columnsOf(w.definition()) == (QStringList{QStringLiteral("time"), QStringLiteral("level"), QStringLiteral("msg")}),
          QStringLiteral("editor: Import Grok makes a line template: %1 %2").arg(w.templateText(), grokError));
    check(!w.importGrok(QStringLiteral("no tokens"), &grokError) && !grokError.isEmpty(),
          "editor: Grok without %{…} is refused in words");

    // Закрытие с правками спрашивает: Cancel — окно остаётся; Discard — закрыто.
    answers.queue = {Answer::Cancel};
    w.close();
    check(w.isVisible() && w.isModified(), "editor: close with changes asks; Cancel keeps the window");
    answers.queue = {Answer::Discard};
    w.close();
    check(!w.isVisible(), "editor: Discard closes");
    w.show();

    // Переход к другой схеме с правками тоже спрашивает.
    w.openSchema(QStringLiteral("Orders"));
    w.setDescription(QStringLiteral("unsaved"));
    answers.queue = {Answer::Cancel};
    check(!w.openSchema(QStringLiteral("Other")) && w.savedName() == QLatin1String("Orders"),
          "editor: switching schemas with changes asks; Cancel stays");
    answers.queue = {Answer::Discard};
    check(w.openSchema(QStringLiteral("Other")) && w.savedName() == QLatin1String("Other"), "editor: Discard switches");

    // Вкладка: образцы, промахи, покрытие файла (одна мера), Apply to Tab.
    QByteArray bytes;
    const char* levels[] = {"INFO", "WARN", "ERROR"};
    for (int i = 0; i < 300; ++i) {
        bytes += QStringLiteral("2026-03-05 10:%1:%2.000 [w%3] %4 - message %5\n")
                     .arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0'))
                     .arg(i % 4).arg(QLatin1String(levels[i % 3])).arg(i).toUtf8();
        if (i % 50 == 7)
            bytes += QStringLiteral("2026-03-05 10:%1:%2.500 restart marker\n")
                         .arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0')).toUtf8();
    }
    const QString logPath = tmp.filePath(QStringLiteral("editor.log"));
    writeBytes(logPath, bytes);
    LogViewWidget tab;
    c.configureView(&tab);
    c.setCurrentView(&tab);
    tab.addLogFile(logPath);
    check(waitFor([&] { return !tab.isLoading() && tab.model()->rowCount() > 300 && !tab.model()->isFiltering(); }),
          "editor: the tab loads");
    w.setView(&tab);
    const QString tpl = QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} {?- ?}{Message}");
    w.newFromTemplate(tpl, QStringLiteral("Fixture"));
    check(waitFor([&] { return !w.isComputing() && w.fileFitReady(); }), "editor: the file check finishes");
    SchemaSampleModel* samples = w.sampleModel();
    const TemplateExtractor x(tpl);
    const SchemaFit::Result expected =
        SchemaFit::evaluate(x, SchemaFitJob::sampleLines(tab.model()->scanSnapshot(false), nullptr), noLimit);
    check(sameFit(w.fileFit(), expected) && expected.valid && expected.partial > 0,
          QStringLiteral("editor: file coverage = SchemaFit::evaluate on the same lines (partial %1/%2)")
              .arg(w.fileFit().partial).arg(expected.partial));
    auto* fileLabel = w.findChild<QLabel*>(QStringLiteral("fileFitLabel"));
    check(fileLabel && fileLabel->text().contains(SchemaFit::summary(expected)),
          QStringLiteral("editor: the coverage line: %1").arg(fileLabel ? fileLabel->text() : QString()));
    const int levelColumn = int(samples->columns().indexOf(QStringLiteral("Level")));
    check(samples->acceptedRows() > 0 && levelColumn == 2 && samples->distinctCount(levelColumn) == 3
              && samples->headerData(levelColumn, Qt::Horizontal).toString() == QString::fromUtf8("Level · 3")
              && samples->headerData(levelColumn, Qt::Horizontal, Qt::ToolTipRole).toString().contains(QLatin1String("INFO")),
          QStringLiteral("editor: the column header shows the number of values, the tooltip the frequent ones: %1 | %2 | %3")
              .arg(samples->headerData(levelColumn, Qt::Horizontal).toString(),
                   samples->headerData(levelColumn, Qt::Horizontal, Qt::ToolTipRole).toString(),
                   samples->columns().join(',')));
    check(samples->cell(0, levelColumn) == tab.model()->fieldsOf(*tab.model()->entryAt(0)).value(levelColumn,
                                                                         tab.model()->messageAt(0))
              || samples->cell(0, levelColumn) == QLatin1String("INFO"),
          "editor: the cells are the fields of the tab's lines");
    bool reasons = !samples->misses().isEmpty();
    for (const auto& miss : samples->misses())
        reasons = reasons && miss.line > 0 && miss.text.contains(QLatin1String("restart")) && !miss.reason.isEmpty();
    check(reasons, QStringLiteral("editor: Did not match — lines of the file with a reason in words (%1)")
                       .arg(samples->misses().isEmpty() ? QString() : samples->misses().first().reason));
    check(w.applyToTab() && tab.parserPattern() == w.definition(), "editor: Apply to Tab sets the schema of the tab");
    check(waitFor([&] { return !tab.isLoading(); }) && tab.model()->rowCount() > 300, "editor: Apply does not reload");

    // Find a Schema — панелью: вариант открывается в редакторе.
    auto* find = w.findChild<QPushButton*>(QStringLiteral("findSchemaBtn"));
    if (find)
        find->click();
    auto* panel = w.findChild<SchemaCandidatesWindow*>();
    check(panel && panel->isVisible() && !panel->isWindow(), "editor: Find a Schema is a panel of the window");
    answers.queue = {Answer::Discard};
    if (panel)
        emit panel->editRequested(QStringLiteral("{Ts:time} {Message}"), QStringLiteral("Inferred"));
    check(w.templateText() == QLatin1String("{Ts:time} {Message}") && w.savedName().isEmpty(),
          "editor: a variant of Find a Schema opens as a new schema");
    answers.queue = {Answer::Discard};
    w.close();
    w.setView(nullptr);
}

// ---------------------------------------------------------------- 7

// Ключевая схема прежних версий (F-4) в каталоге: JSON и logfmt — формат
// строк файла (F-17, шаг 7), схема — шаблон её запасного блока «{Message}»
// (F-17, шаг 9); ключевая часть — метаданные: правка каталога и редактор её
// не теряют.
QByteArray keyedSchemaFile(const QString& name)
{
    QJsonObject obj{{"version", 3}, {"type", "dynamic-block-schema"}, {"linePrefix", ""},
                    {"blocks", QJsonArray{QJsonObject{{"name", "Message"}, {"kind", "remainder"},
                                                      {"leadingText", ""}, {"closingText", ""},
                                                      {"separator", ""}, {"customRegex", ""}}}}};
    obj.insert(QStringLiteral("keyed"),
               QJsonObject{{"format", "logfmt"}, {"extraColumns", QJsonArray{"logger", "trace_id"}},
                           {"numericLevels", "python"}});
    obj.insert(QStringLiteral("name"), name);
    obj.insert(QStringLiteral("description"), QStringLiteral("App logfmt"));
    obj.insert(QStringLiteral("author"), QStringLiteral("ops team"));
    obj.insert(QStringLiteral("x-detect"), QJsonObject{{QStringLiteral("sample"), QStringLiteral("2026-01-01 boot")}});
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

void testKeyedSchema()
{
    clearPatterns();
    const QString path = patterns().filePath(QStringLiteral("app-logfmt.json"));
    const QByteArray original = keyedSchemaFile(QStringLiteral("App logfmt"));
    check(writeBytes(path, original), "keyed: write schema file");
    const QJsonObject keyedJson = QJsonDocument::fromJson(original).object().value(QStringLiteral("keyed")).toObject();

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    check(list.size() == 1, "keyed: schema loaded");
    if (list.size() != 1)
        return;
    check(list[0].meta.value(QStringLiteral("keyed")).toObject() == keyedJson, "keyed: kept as metadata");
    check(columnsOf(list[0].schema) == QStringList({"Message"}), "keyed: the schema is its fallback {Message}");

    check(SchemaStore::sync(list), "keyed: sync untouched");
    check(readBytes(path) == original, "keyed: untouched file not rewritten");

    list[0].name = QStringLiteral("App logfmt v2");
    check(SchemaStore::sync(list), "keyed: sync renamed");
    QJsonObject obj = readJson(path);
    check(obj.value(QStringLiteral("keyed")).toObject() == keyedJson, "keyed: rename keeps the keyed part");
    check(hasMeta(obj, QStringLiteral("App logfmt")), "keyed: rename keeps metadata");

    // Перевод каталога: ключевая часть — метаданные файла версии 4.
    SchemaStore::migrateToTemplates(SchemaStore::patternsDir());
    obj = readJson(path);
    check(obj.value(QStringLiteral("version")).toInt() == 4 && obj.value(QStringLiteral("keyed")).toObject() == keyedJson,
          "keyed: conversion keeps the keyed part");

    // Редактор: режима Format нет; правка описания ключевую часть не теряет.
    FieldSchemaController c;
    loadController(c);
    {
        SchemaEditorWindow w(&c);
        check(!w.findChild<QWidget*>(QStringLiteral("formatCombo")) && !w.findChild<QWidget*>(QStringLiteral("keyedCard")),
              "editor: no Format mode (JSON and logfmt are line formats)");
        check(w.openSchema(QStringLiteral("App logfmt v2")), "keyed: the editor opens the schema");
        w.setDescription(QStringLiteral("Edited in the editor"));
        check(w.save(), "keyed: Save");
    }
    obj = readJson(path);
    check(obj.value(QStringLiteral("keyed")).toObject() == keyedJson, "keyed: Save keeps the keyed part");
    check(obj.value(QStringLiteral("description")).toString() == QLatin1String("Edited in the editor"),
          "keyed: the edit is saved");
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("DendroLogTest"));
    QApplication::setApplicationName(QStringLiteral("schema_store"));
    QFile::remove(AppSettings::iniFilePath());

    QTemporaryDir temporary;
    if (!temporary.isValid() || SchemaStore::patternsDir().isEmpty()) {
        std::fprintf(stderr, "schema_store: no temporary or patterns folder\n");
        return 1;
    }
    const QDir tmp(temporary.path());

    testStoreRoundTrip();
    testNoDescription();
    testUnparsedFilesSurvive();
    testImportExport(tmp);

    const QString fixed = makeFixedBuiltins(tmp);
    testBuiltinManifest(tmp, fixed, UserAction::Nothing, "builtin untouched");
    testBuiltinManifest(tmp, fixed, UserAction::ResaveUntouched, "builtin re-saved");
    testBuiltinManifest(tmp, fixed, UserAction::EditDescription, "builtin description edited");
    testBuiltinManifest(tmp, fixed, UserAction::EditBlocks, "builtin blocks edited");
    testBuiltinManifest(tmp, fixed, UserAction::Rename, "builtin renamed");

    testMigration();
    testBuiltinsFromV3();
    testEditor(tmp);
    testKeyedSchema();

    clearPatterns();
    QFile::remove(AppSettings::iniFilePath());
    if (g_failures) {
        std::fprintf(stderr, "schema_store: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("schema_store: OK (%d checks)\n", g_checks);
    return 0;
}
