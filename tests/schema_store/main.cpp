// Каталог схем (SchemaStore) и редактор схем: метаданные схемы переживают
// пересохранение (Q-17).
//
//   1. Описание и неизвестные ключи верхнего уровня проходят load → sync без
//      правок (файл не переписывается), с правкой блоков, с переименованием;
//      схема без описания не получает пустой ключ "description".
//   2. Правка и переименование пишут в тот же файл; файл с именем, выведенным
//      из прежнего названия схемы, переезжает под новое название.
//   3. Импорт и экспорт переносят метаданные; импортированный файл не
//      дублируется следующей синхронизацией.
//   4. Манифест встроенных схем: пересохранение без правок и правка только
//      описания не делают встроенную схему «изменённой пользователем» —
//      исправленная версия её заменяет; правка блоков и переименование в
//      редакторе — делают (файл остаётся, не считается удалённым).
//   5. Тот же путь через ConversionPatternDialog: поле Description, Apply,
//      Revert, импорт Grok, переименование, resultPatterns → sync → loadAll.
//
// Каталог встроенных схем в исходниках — define DENDROLOG_PATTERNS_SOURCE_DIR.

#include "appsettings.h"
#include "builtinschemas.h"
#include "conversionpatterndialog.h"
#include "patternblockcard.h"
#include "logpattern.h"
#include "schemastore.h"

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

QDir patterns() { return QDir(SchemaStore::patternsDir()); }

void clearPatterns()
{
    QDir dir = patterns();
    const QStringList files = dir.entryList(QDir::Files | QDir::Hidden);
    for (const QString& f : files)
        dir.remove(f);
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

// Файл схемы, написанный «руками»: описание, ключ, которого приложение не
// знает (объект), и ключ-строка.
QByteArray handWrittenSchema(const QString& name, const QString& description)
{
    PatternDefinition def;
    PatternBlock ts;
    ts.name = QStringLiteral("Time");
    ts.matchKind = PatternBlock::MatchKind::Timestamp;
    def.blocks.append(ts);
    PatternBlock msg;
    msg.name = QStringLiteral("Message");
    msg.matchKind = PatternBlock::MatchKind::Remainder;
    def.blocks.append(msg);
    QJsonObject obj = QJsonDocument::fromJson(LogPattern::serializeDefinition(def).toUtf8()).object();
    obj.insert(QStringLiteral("name"), name);
    if (!description.isNull())
        obj.insert(QStringLiteral("description"), description);
    obj.insert(QStringLiteral("x-detect"), QJsonObject{{QStringLiteral("sample"), QStringLiteral("2026-01-01 boot")}});
    obj.insert(QStringLiteral("author"), QStringLiteral("ops team"));
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

// Каноническая схема с изменённым именем первого блока — «правка блоков».
QString editedSchema(const QString& canonical, const QString& firstBlockName)
{
    PatternDefinition def;
    LogPattern::deserializeDefinition(canonical, &def, true, false);
    if (!def.blocks.isEmpty())
        def.blocks.first().name = firstBlockName;
    return LogPattern::serializeDefinition(def);
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
    check(!list[0].meta.contains(QStringLiteral("blocks")) && !list[0].meta.contains(QStringLiteral("name")),
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
    check(obj.value(QStringLiteral("blocks")).toArray().first().toObject()
              .value(QStringLiteral("name")).toString() == QLatin1String("When"), "edit is written");

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
    check(keys == (QStringList{QStringLiteral("blocks"), QStringLiteral("linePrefix"), QStringLiteral("name"),
                               QStringLiteral("type"), QStringLiteral("version")}),
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
    QJsonArray blocks = obj.value(QStringLiteral("blocks")).toArray();
    QJsonObject thread = blocks[1].toObject();
    thread.insert(QStringLiteral("name"), QStringLiteral("ThreadV2"));
    blocks[1] = thread;
    obj.insert(QStringLiteral("blocks"), blocks);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    writeBytes(path, QJsonDocument(obj).toJson(QJsonDocument::Indented));
    return dir;
}

QString threadBlockName(const QString& path)
{
    return readJson(path).value(QStringLiteral("blocks")).toArray()[1].toObject()
        .value(QStringLiteral("name")).toString();
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

// Ответить на QInputDialog, который откроет следующее действие.
void answerNextInputDialog(const QString& text)
{
    QTimer::singleShot(0, [text] {
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (auto* dlg = qobject_cast<QInputDialog*>(w); dlg && dlg->isVisible()) {
                dlg->setTextValue(text);
                dlg->accept();
                return;
            }
        }
        check(false, "Grok input dialog was shown");
    });
}

int rowOf(QTableWidget* table, const QString& name)
{
    for (int r = 0; r < table->rowCount(); ++r)
        if (table->item(r, 0) && table->item(r, 0)->text() == name)
            return r;
    return -1;
}

void testDialog()
{
    clearPatterns();
    writeBytes(patterns().filePath(QStringLiteral("curated-name.json")),
               handWrittenSchema(QStringLiteral("Orders"), QStringLiteral("Orders service log")));
    writeBytes(patterns().filePath(QStringLiteral("other.json")),
               handWrittenSchema(QStringLiteral("Other"), QString()));
    BuiltinSchemas::install(kBuiltinDir, SchemaStore::patternsDir());
    const QByteArray otherBefore = readBytes(patterns().filePath(QStringLiteral("other.json")));
    const QByteArray logbackBefore = readBytes(patterns().filePath(QStringLiteral("logback.json")));

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    {
        ConversionPatternDialog dlg(list);
        auto* table = child<QTableWidget>(&dlg, "schemaTable");
        auto* name = child<QLineEdit>(&dlg, "nameEdit");
        auto* description = child<QLineEdit>(&dlg, "descriptionEdit");
        auto* apply = child<QPushButton>(&dlg, "applyBtn");
        auto* revert = child<QPushButton>(&dlg, "revertBtn");
        auto* grok = child<QPushButton>(&dlg, "grokBtn");
        if (!table || !name || !description || !apply || !revert || !grok)
            return;

        // Открыть, посмотреть, закрыть — ничего не меняется (проверка ниже).
        const int orders = rowOf(table, QStringLiteral("Orders"));
        check(orders >= 0, "dialog lists Orders");
        table->setCurrentCell(orders, 0);
        check(description->text() == QLatin1String("Orders service log"), "editor shows the description");
        check(table->item(orders, 0)->toolTip() == QLatin1String("Orders service log"),
              "schema list: description is the tooltip");

        // Импорт Grok заменяет блоки, описание остаётся.
        answerNextInputDialog(QStringLiteral("%{TIMESTAMP_ISO8601:time} %{LOGLEVEL:level} %{GREEDYDATA:msg}"));
        grok->click();
        check(description->text() == QLatin1String("Orders service log"), "Grok import keeps the description field");
        apply->click();

        // Правка описания + Revert — откат к применённому.
        typeInto(description, QStringLiteral("draft"));
        check(revert->isEnabled(), "description edit makes the editor dirty");
        revert->click();
        check(description->text() == QLatin1String("Orders service log"), "Revert restores the description");

        // Переименование встроенной схемы и правка её описания.
        const int logback = rowOf(table, QStringLiteral("Logback / Log4j"));
        check(logback >= 0, "dialog lists Logback");
        table->setCurrentCell(logback, 0);
        typeInto(description, QStringLiteral("Our Java services"));
        apply->click();
        check(table->item(logback, 0)->toolTip() == QLatin1String("Our Java services"), "tooltip follows the edit");

        // Новая схема с описанием.
        auto* add = child<QPushButton>(&dlg, "addSchemaBtn");
        if (add) {
            add->click();
            typeInto(name, QStringLiteral("Fresh"));
            typeInto(description, QStringLiteral("Brand new"));
            apply->click();
        }
        list = dlg.resultPatterns();
    }
    check(SchemaStore::sync(list), "sync dialog result");

    const QJsonObject orders = readJson(patterns().filePath(QStringLiteral("curated-name.json")));
    check(hasMeta(orders, QStringLiteral("Orders service log")),
          "dialog + Grok: description and unknown keys survive in the same file");
    check(orders.value(QStringLiteral("blocks")).toArray().size() == 3
              && orders.value(QStringLiteral("blocks")).toArray()[2].toObject()
                     .value(QStringLiteral("name")).toString() == QLatin1String("msg"),
          "dialog + Grok: the new blocks are saved");
    check(readBytes(patterns().filePath(QStringLiteral("other.json"))) == otherBefore,
          "untouched schema without description: file not rewritten");
    check(readJson(patterns().filePath(QStringLiteral("logback.json")))
              .value(QStringLiteral("description")).toString() == QLatin1String("Our Java services"),
          "dialog: built-in description edit saved in place");
    check(readBytes(patterns().filePath(QStringLiteral("logback.json"))) != logbackBefore, "logback rewritten");
    const QJsonObject fresh = readJson(patterns().filePath(QStringLiteral("Fresh.json")));
    check(fresh.value(QStringLiteral("description")).toString() == QLatin1String("Brand new"),
          "dialog: new schema saved with its description");

    // Untouched built-ins: not rewritten by the dialog round trip.
    const QStringList shipped = QDir(kBuiltinDir).entryList({QStringLiteral("*.json")}, QDir::Files);
    for (const QString& f : shipped) {
        if (f == QLatin1String("logback.json"))
            continue;
        check(readBytes(patterns().filePath(f)) == readBytes(QDir(kBuiltinDir).filePath(f)),
              "dialog round trip leaves built-in file untouched: " + f);
    }

    // Описание только у logback изменено — встроенная схема всё ещё «нетронутая».
    const BuiltinSchemas::InstallReport again = BuiltinSchemas::install(kBuiltinDir, SchemaStore::patternsDir());
    check(!again.changed() && again.declined.isEmpty() && again.errors.isEmpty(),
          "after the dialog: nothing to install, nothing declined");
}


// ---------------------------------------------------------------- F-4

// Ключевая схема (F-4, PR 1): «keyed» — часть формата, а не метаданные;
// файл пересохраняется без потерь (хранилище и редактор), запасные блоки —
// «Message» для версий без F-4; чужая ключевая часть не читается, но файл
// переживает правку каталога (Q-23).
QByteArray keyedSchemaFile(const QString& name)
{
    PatternDefinition def;
    KeyedDefinition keyed;
    keyed.format = KeyedDefinition::Format::Logfmt;
    keyed.extraColumns = {QStringLiteral("logger"), QStringLiteral("trace_id")};
    keyed.numericLevels = KeyedDefinition::NumericLevels::Python;
    def.keyed = keyed;
    QJsonObject obj = QJsonDocument::fromJson(LogPattern::serializeDefinition(def).toUtf8()).object();
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
    check(QJsonDocument::fromJson(original).object().value(QStringLiteral("version")).toInt() == 3,
          "keyed: format version stays 3");

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    check(list.size() == 1, "keyed: schema loaded");
    if (list.size() != 1)
        return;
    check(!list[0].meta.contains(QStringLiteral("keyed")), "keyed: a format key, not metadata");
    LogPattern pattern(list[0].schema);
    check(pattern.isKeyed() && pattern.keyed()->format == KeyedDefinition::Format::Logfmt
              && pattern.fieldNames() == QStringList({"Timestamp", "Level", "Message", "logger", "trace_id"}),
          "keyed: canonical schema carries the keyed part");

    check(SchemaStore::sync(list), "keyed: sync untouched");
    check(readBytes(path) == original, "keyed: untouched file not rewritten");

    list[0].name = QStringLiteral("App logfmt v2");
    check(SchemaStore::sync(list), "keyed: sync renamed");
    QJsonObject obj = readJson(path);
    check(obj.value(QStringLiteral("keyed")).toObject() == keyedJson, "keyed: rename keeps the keyed part");
    check(hasMeta(obj, QStringLiteral("App logfmt")), "keyed: rename keeps metadata");
    const QJsonArray blocks = obj.value(QStringLiteral("blocks")).toArray();
    check(blocks.size() == 1 && blocks[0].toObject().value(QStringLiteral("name")).toString() == QLatin1String("Message")
              && blocks[0].toObject().value(QStringLiteral("kind")).toString() == QLatin1String("remainder"),
          "keyed: fallback blocks for versions without F-4 are a single Message");

    // Редактор схем (ключевого режима в нём ещё нет — шаг 4): правка
    // описания и Apply ключевую часть не теряют.
    list = SchemaStore::loadAll();
    {
        ConversionPatternDialog dlg(list);
        auto* table = child<QTableWidget>(&dlg, "schemaTable");
        auto* description = child<QLineEdit>(&dlg, "descriptionEdit");
        auto* apply = child<QPushButton>(&dlg, "applyBtn");
        if (!table || !description || !apply)
            return;
        const int row = rowOf(table, QStringLiteral("App logfmt v2"));
        check(row >= 0, "keyed: dialog lists the schema");
        table->setCurrentCell(row, 0);
        // Ключевая схема открывается в ключевом режиме редактора (F-4,
        // PR 4): формат, роли, колонки, единица и шкала — из файла;
        // цепочка блоков скрыта.
        auto* format = child<QComboBox>(&dlg, "formatCombo");
        auto* blocksScroll = child<QScrollArea>(&dlg, "blocksScroll");
        auto* keyedCard = child<QWidget>(&dlg, "keyedCard");
        auto* columns = child<QLineEdit>(&dlg, "columnsEdit");
        auto* levels = child<QComboBox>(&dlg, "numericLevelsCombo");
        if (!format || !blocksScroll || !keyedCard || !columns || !levels)
            return;
        check(format->currentData().toString() == QLatin1String("logfmt"), "keyed: editor shows logfmt");
        check(!blocksScroll->isVisibleTo(&dlg) && keyedCard->isVisibleTo(&dlg), "keyed: keyed card instead of blocks");
        check(columns->text() == QLatin1String("logger, trace_id"), "keyed: columns in the editor");
        check(levels->currentData().toString() == QLatin1String("python"), "keyed: numeric levels in the editor");
        typeInto(description, QStringLiteral("Edited in dialog"));
        apply->click();
        list = dlg.resultPatterns();
    }
    check(SchemaStore::sync(list), "keyed: sync dialog result");
    obj = readJson(path);
    check(obj.value(QStringLiteral("keyed")).toObject() == keyedJson, "keyed: dialog Apply keeps the keyed part");
    check(obj.value(QStringLiteral("description")).toString() == QLatin1String("Edited in dialog"),
          "keyed: dialog edit saved");

    // Ключевая часть, которой эта версия не знает (будущий формат), — схема
    // не читается, файл переживает правку каталога байт в байт.
    QJsonObject future = QJsonDocument::fromJson(keyedSchemaFile(QStringLiteral("Future"))).object();
    future.insert(QStringLiteral("keyed"), QJsonObject{{QStringLiteral("format"), QStringLiteral("gelf")}});
    const QString futurePath = patterns().filePath(QStringLiteral("future.json"));
    const QByteArray futureBytes = QJsonDocument(future).toJson();
    check(writeBytes(futurePath, futureBytes), "keyed: write future schema");
    list = SchemaStore::loadAll();
    check(!findByName(list, QStringLiteral("Future")), "keyed: unknown keyed format is not loaded");
    list.removeIf([](const SchemaStore::SchemaEntry& e) { return e.name == QLatin1String("App logfmt v2"); });
    check(SchemaStore::sync(list), "keyed: sync after removing a schema");
    check(readBytes(futurePath) == futureBytes, "keyed: unparsed future schema survives sync");
}
// Ключевой режим редактора (F-4, PR 4): формат Blocks / JSON / logfmt,
// ключи ролей, колонки, единица эпохи и шкала уровней; Apply / Revert;
// переключение формата туда и обратно не теряет блоки; «Add keys from
// sample»; Auto-detect на образце JSON предлагает ключевую схему; живой
// предпросмотр распознаёт строки образца.
void selectData(QComboBox* combo, const char* data)
{
    const int i = combo->findData(QString::fromLatin1(data));
    check(i >= 0, QStringLiteral("combo %1 has %2").arg(combo->objectName(), QString::fromLatin1(data)));
    combo->setCurrentIndex(i);
}

bool waitPreview(QLabel* status, const QString& wanted)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (status->text().contains(wanted))
            return true;
    }
    return false;
}

QJsonArray blocksOf(const QJsonObject& obj) { return obj.value(QStringLiteral("blocks")).toArray(); }

void testKeyedEditor()
{
    clearPatterns();
    const QString plainPath = patterns().filePath(QStringLiteral("plain.json"));
    check(writeBytes(plainPath, handWrittenSchema(QStringLiteral("Plain"), QStringLiteral("Regex"))),
          "editor: write regex schema");
    const QJsonArray plainBlocks = blocksOf(readJson(plainPath));

    SchemaStore::SchemaList list = SchemaStore::loadAll();
    {
        ConversionPatternDialog dlg(list);
        auto* table = child<QTableWidget>(&dlg, "schemaTable");
        auto* name = child<QLineEdit>(&dlg, "nameEdit");
        auto* apply = child<QPushButton>(&dlg, "applyBtn");
        auto* revert = child<QPushButton>(&dlg, "revertBtn");
        auto* add = child<QPushButton>(&dlg, "addSchemaBtn");
        auto* autoDetect = child<QPushButton>(&dlg, "autoDetectBtn");
        auto* format = child<QComboBox>(&dlg, "formatCombo");
        auto* blocksScroll = child<QScrollArea>(&dlg, "blocksScroll");
        auto* addBlock = child<QPushButton>(&dlg, "addBlockBtn");
        auto* keyedCard = child<QWidget>(&dlg, "keyedCard");
        auto* timeKeys = child<QLineEdit>(&dlg, "timeKeysEdit");
        auto* levelKeys = child<QLineEdit>(&dlg, "levelKeysEdit");
        auto* messageKeys = child<QLineEdit>(&dlg, "messageKeysEdit");
        auto* columns = child<QLineEdit>(&dlg, "columnsEdit");
        auto* epoch = child<QComboBox>(&dlg, "epochUnitCombo");
        auto* levels = child<QComboBox>(&dlg, "numericLevelsCombo");
        auto* addKeys = child<QPushButton>(&dlg, "addSampleKeysBtn");
        auto* sample = child<QPlainTextEdit>(&dlg, "sampleEdit");
        auto* status = child<QLabel>(&dlg, "previewStatusLabel");
        auto* validation = child<QLabel>(&dlg, "validationLabel");
        if (!table || !name || !apply || !revert || !add || !autoDetect || !format || !blocksScroll
                || !addBlock || !keyedCard || !timeKeys || !levelKeys || !messageKeys || !columns
                || !epoch || !levels || !addKeys || !sample || !status || !validation)
            return;

        // Регексная схема: режим Blocks; JSON и обратно — те же блоки.
        table->setCurrentCell(rowOf(table, QStringLiteral("Plain")), 0);
        check(format->currentData().toString() == QLatin1String("blocks"), "editor: regex schema in Blocks mode");
        check(blocksScroll->isVisibleTo(&dlg) && !keyedCard->isVisibleTo(&dlg), "editor: blocks shown, keyed card hidden");
        const int cards = dlg.findChildren<PatternBlockCard*>().size();
        selectData(format, "json");
        check(!blocksScroll->isVisibleTo(&dlg) && keyedCard->isVisibleTo(&dlg) && !addBlock->isVisibleTo(&dlg),
              "editor: JSON mode shows the keyed card instead of the blocks");
        check(timeKeys->text() == KeyedDefinition::defaultTimeKeys().join(QStringLiteral(", "))
                  && levelKeys->text() == KeyedDefinition::defaultLevelKeys().join(QStringLiteral(", "))
                  && messageKeys->text() == KeyedDefinition::defaultMessageKeys().join(QStringLiteral(", ")),
              "editor: default role keys prefilled");
        check(apply->isEnabled(), "editor: format switch makes the editor dirty");
        selectData(format, "blocks");
        QCoreApplication::processEvents();
        check(dlg.findChildren<PatternBlockCard*>().size() == cards, "editor: back to Blocks keeps the cards");
        apply->click();
        list = dlg.resultPatterns();
        check(SchemaStore::sync(list), "editor: sync after the round trip");
        check(blocksOf(readJson(plainPath)) == plainBlocks && !readJson(plainPath).contains(QStringLiteral("keyed")),
              "editor: Blocks → JSON → Blocks loses no block");

        // Новая ключевая схема: формат, ключи, колонки, единица и шкала.
        add->click();
        typeInto(name, QStringLiteral("App JSON"));
        selectData(format, "json");
        typeInto(timeKeys, QStringLiteral("when, ts"));
        typeInto(columns, QStringLiteral("service, trace_id"));
        selectData(epoch, "ms");
        selectData(levels, "python");
        check(validation->text().contains(QStringLiteral("valid")), "editor: keyed schema is valid");
        sample->setPlainText(QStringLiteral(
            "{\"when\":1763474136456,\"level\":40,\"msg\":\"a\",\"service\":\"api\"}\n"
            "{\"ts\":\"2026-11-18T10:00:00Z\",\"level\":\"info\",\"msg\":\"b\",\"trace_id\":\"t1\",\"user\":\"bob\"}\n"
            "not json at all"));
        check(waitPreview(status, QStringLiteral("2 matched")), "editor: live preview recognises the JSON lines: " + status->text());
        check(status->text().contains(QStringLiteral("1 not recognized")), "editor: preview counts the plain line");
        apply->click();

        // Revert возвращает применённое.
        typeInto(columns, QStringLiteral("x"));
        revert->click();
        check(columns->text() == QLatin1String("service, trace_id"), "editor: Revert restores the columns");
        check(format->currentData().toString() == QLatin1String("json"), "editor: Revert keeps JSON mode");

        // Колонка с именем роли — ошибка проверки.
        typeInto(columns, QStringLiteral("level"));
        check(validation->text().contains(QStringLiteral("twice")), "editor: column clashing with a role is invalid: " + validation->text());
        revert->click();

        // «Add keys from sample» — ключи образцов, которых нет в колонках.
        addKeys->click();
        check(columns->text() == QLatin1String("service, trace_id, user"),
              "editor: keys from the sample appended: " + columns->text());
        apply->click();

        // Auto-detect на образце logfmt — ключевая схема logfmt.
        add->click();
        typeInto(name, QStringLiteral("Detected"));
        sample->setPlainText(QStringLiteral(
            "ts=2026-11-18T10:00:00Z level=info msg=\"Starting\" caller=main.go:12\n"
            "ts=2026-11-18T10:00:01Z level=error msg=\"boom\" caller=db.go:7"));
        autoDetect->click();
        check(format->currentData().toString() == QLatin1String("logfmt"), "editor: Auto-detect proposes logfmt");
        check(columns->text() == QLatin1String("caller"), "editor: Auto-detect fills columns: " + columns->text());
        apply->click();

        // Строка JSON в меньшинстве — регексное предложение, не ключевое.
        add->click();
        typeInto(name, QStringLiteral("Mostly plain"));
        sample->setPlainText(QStringLiteral(
            "{\"level\":\"info\",\"msg\":\"x\"}\n"
            "2026-11-18 10:00:00 INFO first\n"
            "2026-11-18 10:00:01 WARN second"));
        autoDetect->click();
        check(format->currentData().toString() == QLatin1String("blocks"),
              "editor: a JSON line in the minority gives a block schema");
        apply->click();

        list = dlg.resultPatterns();
    }
    check(SchemaStore::sync(list), "editor: sync keyed schemas");
    const QJsonObject app = readJson(patterns().filePath(QStringLiteral("App JSON.json")));
    const QJsonObject keyed = app.value(QStringLiteral("keyed")).toObject();
    check(app.value(QStringLiteral("version")).toInt() == 3, "editor: keyed schema saved as version 3");
    check(keyed.value(QStringLiteral("format")).toString() == QLatin1String("json")
              && keyed.value(QStringLiteral("timeKeys")).toArray() == QJsonArray({"when", "ts"})
              && keyed.value(QStringLiteral("extraColumns")).toArray() == QJsonArray({"service", "trace_id", "user"})
              && keyed.value(QStringLiteral("epochUnit")).toString() == QLatin1String("ms")
              && keyed.value(QStringLiteral("numericLevels")).toString() == QLatin1String("python")
              && keyed.value(QStringLiteral("levelKeys")).toArray()
                     == QJsonArray::fromStringList(KeyedDefinition::defaultLevelKeys()),
          "editor: keyed part saved: " + QString::fromUtf8(QJsonDocument(keyed).toJson(QJsonDocument::Compact)));
    check(blocksOf(app).size() == 1, "editor: fallback Message block saved");
    const SchemaStore::SchemaList loaded = SchemaStore::loadAll();
    const SchemaStore::SchemaEntry* detected = findByName(loaded, QStringLiteral("Detected"));
    check(detected && LogPattern(detected->schema).isKeyed()
              && LogPattern(detected->schema).keyed()->format == KeyedDefinition::Format::Logfmt,
          "editor: auto-detected logfmt schema saved");
    // Файл, сохранённый редактором, ничем не отличается от загруженного:
    // следующая правка каталога его не переписывает.
    const QByteArray before = readBytes(patterns().filePath(QStringLiteral("App JSON.json")));
    check(SchemaStore::sync(SchemaStore::loadAll()), "editor: sync again");
    check(readBytes(patterns().filePath(QStringLiteral("App JSON.json"))) == before, "editor: saved keyed file is stable");

    // Ключ, который через запятую не записать («a,b», « pad »), переживает
    // Apply правки описания: нетронутое поле отдаёт список как был.
    {
        PatternDefinition def;
        KeyedDefinition k;
        k.extraColumns = {QStringLiteral("a,b"), QStringLiteral(" pad ")};
        def.keyed = k;
        QJsonObject obj = QJsonDocument::fromJson(LogPattern::serializeDefinition(def).toUtf8()).object();
        obj.insert(QStringLiteral("name"), QStringLiteral("Commas"));
        writeBytes(patterns().filePath(QStringLiteral("commas.json")), QJsonDocument(obj).toJson());
    }
    list = SchemaStore::loadAll();
    {
        ConversionPatternDialog dlg(list);
        auto* table = child<QTableWidget>(&dlg, "schemaTable");
        auto* description = child<QLineEdit>(&dlg, "descriptionEdit");
        auto* apply = child<QPushButton>(&dlg, "applyBtn");
        if (!table || !description || !apply)
            return;
        table->setCurrentCell(rowOf(table, QStringLiteral("Commas")), 0);
        typeInto(description, QStringLiteral("touched"));
        apply->click();
        list = dlg.resultPatterns();
    }
    check(SchemaStore::sync(list), "editor: sync commas");
    check(readJson(patterns().filePath(QStringLiteral("commas.json"))).value(QStringLiteral("keyed")).toObject()
                  .value(QStringLiteral("extraColumns")).toArray() == QJsonArray({"a,b", " pad "}),
          "editor: keys with a comma or padding survive Apply");
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

    testDialog();
    testKeyedSchema();
    testKeyedEditor();

    clearPatterns();
    QFile::remove(AppSettings::iniFilePath());
    if (g_failures) {
        std::fprintf(stderr, "schema_store: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("schema_store: OK (%d checks)\n", g_checks);
    return 0;
}
