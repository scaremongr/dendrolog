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
#include "logpattern.h"
#include "schemastore.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
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

    clearPatterns();
    QFile::remove(AppSettings::iniFilePath());
    if (g_failures) {
        std::fprintf(stderr, "schema_store: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("schema_store: OK (%d checks)\n", g_checks);
    return 0;
}
