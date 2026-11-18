// ============================================================================
// encoding_ui — интерфейс кодировок (F-1, шаг 4), без окна:
//   • EncodingIndicator: подпись для каждого источника решения («не решено»,
//     авто, авто предварительно, BOM, вручную), UTF-16 — без меню; у слитой
//     вкладки подпись следует за файлом текущей строки;
//   • меню → LogViewWidget::setFileEncoding; «Detect automatically» после
//     ручного выбора возвращает решение автоматики — в том числе когда файл
//     с самого начала читался выбранной кодировкой (--encoding);
//   • настройка запасной кодировки: хранение в ini, действие на «не
//     решённый» и предварительно решённый файл открытой вкладки, окончательные
//     решения не трогаются;
//   • --encoding: начальный выбор пользователя для файла (оба бэкенда), для
//     растущего файла (stdin) и для файла, уже открытого во вкладке;
//   • объединение вкладок переносит состояние кодировки;
//   • UTF-16 во вкладке с несколькими файлами — понятное сообщение.
// Ожидания — по состоянию (загрузка закончилась, модель в покое), не по
// таймерам; пиксели не сравниваются.
// ============================================================================

#include "appsettings.h"
#include "encodingindicator.h"
#include "logviewwidget.h"
#include "openrouting.h"
#include "testdocuments.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>

using namespace testdocs;

static int failures = 0;
static int checks = 0;
static QString g_context;
#define CHECK(condition, label) do { \
    ++checks; \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL [%s]: %s (line %d)\n", \
        qPrintable(g_context), qPrintable(QString(label)), __LINE__); } \
} while (0)

static QString recordText(int i, const QString& message)
{
    return QStringLiteral("2026-03-05 10:%1:%2.000 [w] %3 - %4\n")
        .arg(i / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(i % 60, 2, 10, QLatin1Char('0'))
        .arg(i % 7 == 3 ? QStringLiteral("ERROR") : QStringLiteral("INFO"), message);
}

// Кириллица в каждой строке — решение окончательное с первого окна.
static QString denseText(int records, int from = 0)
{
    QString text;
    for (int i = from; i < from + records; ++i)
        text += recordText(i, QStringLiteral("Запись номер %1: ошибка диска").arg(i));
    return text;
}

static QString asciiText(int records, int from = 0)
{
    QString text;
    for (int i = from; i < from + records; ++i)
        text += recordText(i, QStringLiteral("plain ascii line %1").arg(i));
    return text;
}

static bool settled(LogViewWidget& tab)
{
    return waitFor([&] { return !tab.isLoading(); }, 60000) && settle(*tab.model());
}

static std::unique_ptr<LogViewWidget> openTab(const QString& schema, const QStringList& paths,
                                              const FileEncoding& initial = {})
{
    auto tab = std::make_unique<LogViewWidget>();
    tab->resize(900, 400);
    tab->setParserPattern(schema);
    tab->setFallbackEncoding(kTestFallbackEncoding);
    for (const QString& path : paths)
        tab->addLogFile(path, initial);
    CHECK(settled(*tab), QStringLiteral("загрузка"));
    return tab;
}

static int rowOfFile(const LogModel& model, const QString& path, int from = 0)
{
    for (int row = from; row < model.rowCount(); ++row) {
        const RecordKey key = model.keyAt(row);
        if (key.file && key.file->filePath == path)
            return row;
    }
    return -1;
}

static QAction* menuAction(const EncodingIndicator& indicator, const QString& objectName)
{
    return indicator.menu() ? indicator.menu()->findChild<QAction*>(objectName) : nullptr;
}

// Текст строки row, если байты файла прочитать кодировкой encoding.
static QString decodedLine(const QString& text, TextEncoding fileEncoding, TextEncoding readAs,
                           int line)
{
    const QByteArray bytes = encodeText(text, fileEncoding);
    const QList<QByteArray> lines = bytes.split('\n');
    return TextDecoder(readAs).decode(lines.value(line));
}

// ---------------------------------------------------------------------------

static void checkLabels()
{
    g_context = QStringLiteral("подписи");
    FileEncoding e;
    CHECK(EncodingIndicator::label(e, TextEncoding::Utf8) == QStringLiteral("ASCII (auto)"),
          EncodingIndicator::label(e, TextEncoding::Utf8));
    e.decoder = TextDecoder(TextEncoding::Windows1251);
    e.source = FileEncoding::Source::Auto;
    CHECK(EncodingIndicator::label(e, TextEncoding::Windows1251) == QStringLiteral("Windows-1251 (auto)"),
          EncodingIndicator::label(e, TextEncoding::Windows1251));
    e.provisional = true;
    CHECK(EncodingIndicator::label(e, TextEncoding::Windows1251)
              == QStringLiteral("Windows-1251 (auto, provisional)"),
          EncodingIndicator::label(e, TextEncoding::Windows1251));
    const FileEncoding bom{ TextDecoder(TextEncoding::Utf8), FileEncoding::Source::Bom };
    CHECK(EncodingIndicator::label(bom, TextEncoding::Utf8) == QStringLiteral("UTF-8 (BOM)"),
          EncodingIndicator::label(bom, TextEncoding::Utf8));
    const FileEncoding user = e.withUserChoice(TextDecoder(TextEncoding::Koi8R));
    CHECK(EncodingIndicator::label(user, TextEncoding::Koi8R) == QStringLiteral("KOI8-R (manual)"),
          EncodingIndicator::label(user, TextEncoding::Koi8R));
    CHECK(EncodingIndicator::label(FileEncoding(), TextEncoding::Utf16LE) == QStringLiteral("UTF-16LE (BOM)"),
          EncodingIndicator::label(FileEncoding(), TextEncoding::Utf16LE));
}

// Подпись живой вкладки для каждого источника; меню и «авто».
static void checkIndicator(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / индикатор");
    const QString ascii = writeFile(dir, name + "-ascii.log", asciiText(50).toUtf8());
    const QString dense = writeFile(dir, name + "-dense.log",
                                    encodeText(denseText(200), TextEncoding::Windows1251));
    QString sparse = asciiText(40);
    sparse += recordText(40, QStringLiteral("Пользователь вышел из системы"));
    sparse += asciiText(40, 41);
    const QString provisional = writeFile(dir, name + "-word.log",
                                          encodeText(sparse, TextEncoding::Windows1251));
    const QString bomFile = writeFile(dir, name + "-bom.log",
                                      QByteArray("\xEF\xBB\xBF") + denseText(20).toUtf8());

    EncodingIndicator indicator;
    CHECK(indicator.isHidden(), QStringLiteral("без вкладки скрыт"));

    const auto expectLabel = [&](const QString& path, const QString& label) {
        auto tab = openTab(schema, { path });
        indicator.setTab(tab.get());
        CHECK(!indicator.isHidden() && indicator.text() == label
                  && indicator.file() == tab->loadedFiles().value(0),
              QStringLiteral("%1: «%2», а показано «%3»")
                  .arg(QFileInfo(path).fileName(), label, indicator.text()));
        CHECK(indicator.menu() && menuAction(indicator, QStringLiteral("encodingAutoAction")),
              QStringLiteral("%1: есть меню").arg(QFileInfo(path).fileName()));
        indicator.setTab(nullptr);
        return tab;
    };
    expectLabel(ascii, QStringLiteral("ASCII (auto)"));
    expectLabel(provisional, QStringLiteral("Windows-1251 (auto, provisional)"));
    expectLabel(bomFile, QStringLiteral("UTF-8 (BOM)"));
    CHECK(indicator.isHidden(), QStringLiteral("вкладку убрали — скрыт"));

    auto tab = openTab(schema, { dense });
    indicator.setTab(tab.get());
    const LogFilePtr file = tab->loadedFiles().value(0);
    LogModel& model = *tab->model();
    CHECK(indicator.text() == QStringLiteral("Windows-1251 (auto)"), indicator.text());
    tab->view()->goToRow(100);
    const QString original = model.messageAt(100);

    // Меню → выбор пользователя: подпись, состояние вкладки, текст.
    QAction* koi = menuAction(indicator, QStringLiteral("encodingAction_KOI8-R"));
    CHECK(koi, QStringLiteral("пункт KOI8-R"));
    if (koi)
        koi->trigger();
    CHECK(settled(*tab), QStringLiteral("смена на KOI8-R"));
    CHECK(tab->fileEncoding(file).source == FileEncoding::Source::User
              && tab->fileEncoding(file).decoder.encoding() == TextEncoding::Koi8R,
          QStringLiteral("меню → setFileEncoding"));
    CHECK(waitFor([&] { return indicator.text() == QStringLiteral("KOI8-R (manual)"); }, 5000),
          QStringLiteral("подпись после выбора: ") + indicator.text());
    CHECK(koi && koi->isChecked(), QStringLiteral("выбранный пункт отмечен"));
    CHECK(model.messageAt(100) == decodedLine(denseText(200), TextEncoding::Windows1251,
                                              TextEncoding::Koi8R, 100),
          QStringLiteral("текст — чтение KOI8-R: ") + model.messageAt(100));
    CHECK(tab->view()->currentIndex().row() == 100, QStringLiteral("текущая строка на месте"));

    // «Определить автоматически» — решение автоматики, текст прежний.
    QAction* autoAction = menuAction(indicator, QStringLiteral("encodingAutoAction"));
    if (autoAction)
        autoAction->trigger();
    CHECK(settled(*tab), QStringLiteral("«авто»"));
    CHECK(tab->fileEncoding(file).source == FileEncoding::Source::Auto
              && tab->fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("«авто» вернуло решение автоматики"));
    CHECK(waitFor([&] { return indicator.text() == QStringLiteral("Windows-1251 (auto)"); }, 5000),
          QStringLiteral("подпись после «авто»: ") + indicator.text());
    CHECK(model.messageAt(100) == original, QStringLiteral("текст после «авто» — исходный"));
    CHECK(autoAction && autoAction->isChecked(), QStringLiteral("«авто» отмечено"));
    indicator.setTab(nullptr);

    // «Не решён» — выбор доступен: ASCII-файл, выбран Windows-1252.
    auto asciiTab = openTab(schema, { ascii });
    indicator.setTab(asciiTab.get());
    QAction* w1252 = menuAction(indicator, QStringLiteral("encodingAction_Windows-1252"));
    CHECK(w1252 && w1252->isEnabled(), QStringLiteral("выбор доступен, пока не решено"));
    if (w1252)
        w1252->trigger();
    CHECK(settled(*asciiTab), QStringLiteral("выбор на ASCII-файле"));
    CHECK(waitFor([&] { return indicator.text() == QStringLiteral("Windows-1252 (manual)"); }, 5000),
          indicator.text());
    indicator.setTab(nullptr);
}

// UTF-16: подпись без меню.
static void checkUtf16(const QDir& dir, const QString& schema)
{
    g_context = QStringLiteral("UTF-16");
    QByteArray bytes("\xFF\xFE", 2);
    const QString text = denseText(30);
    bytes.append(reinterpret_cast<const char*>(text.utf16()), text.size() * 2);
    const QString path = writeFile(dir, "utf16.log", bytes);
    auto tab = openTab(schema, { path });
    EncodingIndicator indicator;
    indicator.setTab(tab.get());
    CHECK(indicator.text() == QStringLiteral("UTF-16LE (BOM)"), indicator.text());
    CHECK(!indicator.menu(), QStringLiteral("у UTF-16 нет меню"));
    // Выбор у такого файла ничего не меняет.
    const LogFilePtr file = tab->loadedFiles().value(0);
    tab->setFileEncoding(file, TextEncoding::Windows1251);
    CHECK(tab->fileStreamEncoding(file) == TextEncoding::Utf16LE
              && tab->fileEncoding(file).source != FileEncoding::Source::User,
          QStringLiteral("выбор для UTF-16 игнорируется"));

    // UTF-16 в индексной вкладке с другим файлом — понятное сообщение.
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(0);
    const QString other = writeFile(dir, "utf16-companion.log", asciiText(20).toUtf8());
    LogViewWidget merged;
    merged.setParserPattern(schema);
    LogFilePtr failed;
    QObject::connect(&merged, &LogViewWidget::fileParsingFailed, &merged,
                     [&](const LogFilePtr& f) { failed = f; });
    merged.addLogFile(other);
    merged.addLogFile(path);
    CHECK(waitFor([&] { return failed != nullptr; }, 30000), QStringLiteral("UTF-16 не загружен"));
    const QString message = merged.loadFailureMessage(failed);
    CHECK(message.contains(QStringLiteral("UTF-16")) && message.contains(QStringLiteral("utf16.log"))
              && message.contains(QStringLiteral("own tab")),
          QStringLiteral("понятное сообщение: ") + message);
    CHECK(merged.loadFailureMessage(merged.loadedFiles().value(0)).startsWith(QStringLiteral("Failed to parse")),
          QStringLiteral("у прочих — общее сообщение"));
    CHECK(merged.fileStreamEncoding(failed) == TextEncoding::Utf16LE,
          QStringLiteral("незагруженный UTF-16 — кодировка по BOM, а не «ASCII»"));
    merged.setFileEncoding(failed, TextEncoding::Koi8R);
    CHECK(merged.fileEncoding(failed).source != FileEncoding::Source::User,
          QStringLiteral("выбор для незагруженного UTF-16 игнорируется"));
    settings.setIndexedThresholdMB(512);
}

// Слитая вкладка: подпись — файла текущей строки.
static void checkMergedTab(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / слитая вкладка");
    const QString a = writeFile(dir, name + "-merged-a.log", denseText(60).toUtf8());
    const QString b = writeFile(dir, name + "-merged-b.log",
                                encodeText(denseText(60, 30), TextEncoding::Windows1251));
    auto tab = openTab(schema, { a, b });
    EncodingIndicator indicator;
    indicator.setTab(tab.get());
    LogModel& model = *tab->model();
    const int rowA = rowOfFile(model, a, 40);
    const int rowB = rowOfFile(model, b, 40);
    CHECK(rowA >= 0 && rowB >= 0, QStringLiteral("строки обоих файлов"));
    tab->view()->goToRow(rowA);
    CHECK(indicator.text() == QStringLiteral("UTF-8 (auto)") && indicator.file()
              && indicator.file()->filePath == a,
          QStringLiteral("строка файла A: ") + indicator.text());
    tab->view()->goToRow(rowB);
    CHECK(indicator.text() == QStringLiteral("Windows-1251 (auto)") && indicator.file()
              && indicator.file()->filePath == b,
          QStringLiteral("строка файла B: ") + indicator.text());
    // Выбор в меню — для файла текущей строки, второй не трогается.
    if (QAction* koi = menuAction(indicator, QStringLiteral("encodingAction_KOI8-R")))
        koi->trigger();
    CHECK(settled(*tab), QStringLiteral("смена у файла B"));
    const LogFilePtr fileA = tab->loadedFiles().value(0);
    const LogFilePtr fileB = tab->loadedFiles().value(1);
    CHECK(tab->fileEncoding(fileB).source == FileEncoding::Source::User
              && tab->fileEncoding(fileA).source == FileEncoding::Source::Auto,
          QStringLiteral("выбор — только для файла текущей строки"));
    tab->view()->goToRow(rowOfFile(model, a, 40));
    CHECK(indicator.text() == QStringLiteral("UTF-8 (auto)"), indicator.text());
}

// --encoding с самого начала, затем «авто»; уже открытый файл; растущий файл.
static void checkCommandLineEncoding(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / --encoding");
    const QString text = denseText(200);
    const QString path = writeFile(dir, name + "-cli.log", encodeText(text, TextEncoding::Windows1251));
    const OpenRouting::CommandLine cl = OpenRouting::parseCommandLine({ "--encoding", "koi8r", path });
    auto tab = openTab(schema, cl.paths, OpenRouting::initialEncoding(cl));
    const LogFilePtr file = tab->loadedFiles().value(0);
    LogModel& model = *tab->model();
    CHECK(tab->fileEncoding(file).source == FileEncoding::Source::User
              && tab->fileEncoding(file).decoder.encoding() == TextEncoding::Koi8R,
          QStringLiteral("файл открыт выбранной кодировкой"));
    CHECK(model.rowCount() == 200
              && model.messageAt(50) == decodedLine(text, TextEncoding::Windows1251, TextEncoding::Koi8R, 50),
          QStringLiteral("текст — KOI8-R с первой строки: ") + model.messageAt(50));
    // Автоматика решала рядом, хотя файл читался выбранной кодировкой.
    CHECK(tab->fileEncoding(file).automatic().decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("решение автоматики под выбором"));
    tab->restoreAutomaticEncoding(file);
    CHECK(settled(*tab), QStringLiteral("«авто»"));
    CHECK(tab->fileEncoding(file).source == FileEncoding::Source::Auto
              && tab->fileEncoding(file).decoder.encoding() == TextEncoding::Windows1251
              && model.messageAt(50) == decodedLine(text, TextEncoding::Windows1251,
                                                    TextEncoding::Windows1251, 50),
          QStringLiteral("«авто» после --encoding — Windows-1251: ") + model.messageAt(50));

    // Тот же файл ещё раз с --encoding (второй экземпляр) — выбор для
    // открытого файла, без дубля строк.
    tab->addLogFile(path, FileEncoding().withUserChoice(TextDecoder(TextEncoding::Ibm866)));
    CHECK(settled(*tab), QStringLiteral("повторное открытие"));
    CHECK(tab->fileCount() == 1 && model.rowCount() == 200
              && tab->fileEncoding(file).decoder.encoding() == TextEncoding::Ibm866,
          QStringLiteral("уже открытый файл получил выбор"));

    // Растущий файл (stdin): сначала ASCII, кириллица — дозаписью; читается
    // выбранной кодировкой, а не автоматикой.
    const QString spool = writeFile(dir, name + "-stdin.log", asciiText(20).toUtf8());
    const OpenRouting::CommandLine stdinCl = OpenRouting::parseCommandLine({ "--encoding=cp866", "-" });
    CHECK(stdinCl.readStdin && stdinCl.encoding == TextEncoding::Ibm866, QStringLiteral("разбор «-»"));
    auto live = openTab(schema, { spool }, OpenRouting::initialEncoding(stdinCl));
    const QString tail = denseText(30, 20);
    CHECK(appendToFile(spool, encodeText(tail, TextEncoding::Windows1251)), QStringLiteral("дозапись"));
    live->reloadChangedFiles(true);
    CHECK(settled(*live), QStringLiteral("дозапись прочитана"));
    const LogFilePtr spoolFile = live->loadedFiles().value(0);
    CHECK(live->model()->rowCount() == 50
              && live->model()->messageAt(45) == decodedLine(tail, TextEncoding::Windows1251,
                                                             TextEncoding::Ibm866, 25),
          QStringLiteral("хвост — IBM866: ") + live->model()->messageAt(45));
    CHECK(live->fileEncoding(spoolFile).source == FileEncoding::Source::User
              && live->fileEncoding(spoolFile).automatic().decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("выбор стоит, автоматика решила рядом"));
}

// Запасная кодировка: ini и действие на открытую вкладку.
static void checkFallbackSetting(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / запасная кодировка");
    AppSettings& settings = AppSettings::instance();
    settings.setFallbackEncodingChoice(TextEncoding::Windows1252);

    QString sparse = asciiText(40);
    sparse += recordText(40, QStringLiteral("Пользователь вышел из системы"));
    sparse += asciiText(40, 41);
    const QString provisionalPath = writeFile(dir, name + "-fb-word.log",
                                              encodeText(sparse, TextEncoding::Windows1251));
    const QString densePath = writeFile(dir, name + "-fb-dense.log",
                                        encodeText(denseText(200), TextEncoding::Windows1251));
    const QString asciiPath = writeFile(dir, name + "-fb-ascii.log", asciiText(30).toUtf8());

    // Вкладки берут запасную из настройки (тестовую не ставим).
    const auto open = [&](const QString& path) {
        auto tab = std::make_unique<LogViewWidget>();
        tab->setParserPattern(schema);
        tab->addLogFile(path);
        CHECK(settled(*tab), QStringLiteral("загрузка ") + path);
        return tab;
    };
    auto provisionalTab = open(provisionalPath);
    auto denseTab = open(densePath);
    auto asciiTab = open(asciiPath);
    CHECK(provisionalTab->fallbackEncoding() == TextEncoding::Windows1252,
          QStringLiteral("вкладка взяла запасную из настройки"));
    const LogFilePtr pf = provisionalTab->loadedFiles().value(0);
    const LogFilePtr df = denseTab->loadedFiles().value(0);
    const LogFilePtr af = asciiTab->loadedFiles().value(0);
    CHECK(provisionalTab->fileEncoding(pf).decoder.encoding() == TextEncoding::Windows1252
              && provisionalTab->fileEncoding(pf).provisional,
          QStringLiteral("одно слово при запасной Windows-1252 — предварительно Windows-1252"));
    const TextEncoding denseBefore = denseTab->fileEncoding(df).decoder.encoding();
    CHECK(!denseTab->fileEncoding(df).provisional, QStringLiteral("плотный файл решён окончательно"));

    settings.setFallbackEncodingChoice(TextEncoding::Windows1251);
    CHECK(settled(*provisionalTab) && settled(*denseTab) && settled(*asciiTab),
          QStringLiteral("смена запасной применилась"));
    CHECK(provisionalTab->fallbackEncoding() == TextEncoding::Windows1251,
          QStringLiteral("открытая вкладка получила новую запасную"));
    CHECK(provisionalTab->fileEncoding(pf).decoder.encoding() == TextEncoding::Windows1251
              && provisionalTab->fileEncoding(pf).provisional,
          QStringLiteral("предварительное решение пересчитано: Windows-1251"));
    const int wordRow = 40;
    CHECK(provisionalTab->model()->messageAt(wordRow).endsWith(QStringLiteral("Пользователь вышел из системы")),
          QStringLiteral("слово перечитано: ") + provisionalTab->model()->messageAt(wordRow));
    CHECK(denseTab->fileEncoding(df).decoder.encoding() == denseBefore,
          QStringLiteral("окончательное решение не тронуто"));
    // «Не решённый» решит дозапись — уже с новой запасной.
    CHECK(appendToFile(asciiPath, encodeText(recordText(30, QStringLiteral("Пользователь вышел из системы")),
                                             TextEncoding::Windows1251)),
          QStringLiteral("дозапись слова"));
    asciiTab->reloadChangedFiles(true);
    CHECK(settled(*asciiTab), QStringLiteral("дозапись прочитана"));
    CHECK(asciiTab->fileEncoding(af).decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("«не решённый» решён с новой запасной"));

    // Смена запасной посреди загрузки: проход в полёте решает с прежней,
    // пересмотр — по её окончании; итог тот же, что без гонки.
    {
        settings.setFallbackEncodingChoice(TextEncoding::Windows1252);
        auto loading = std::make_unique<LogViewWidget>();
        loading->setParserPattern(schema);
        loading->addLogFile(provisionalPath);
        settings.setFallbackEncodingChoice(TextEncoding::Windows1251);
        CHECK(settled(*loading), QStringLiteral("загрузка при смене запасной"));
        const LogFilePtr lf = loading->loadedFiles().value(0);
        CHECK(loading->fileEncoding(lf).decoder.encoding() == TextEncoding::Windows1251
                  && loading->model()->messageAt(wordRow).endsWith(QStringLiteral("Пользователь вышел из системы")),
              QStringLiteral("смена посреди загрузки: Windows-1251, текст читается"));
    }

    // ini: словом в [App]; неизвестное — по локали системы.
    settings.save();
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("App/fallbackEncoding")).toString() == QStringLiteral("Windows-1251"),
              QStringLiteral("в ini: ") + ini.value(QStringLiteral("App/fallbackEncoding")).toString());
        ini.setValue(QStringLiteral("App/fallbackEncoding"), QStringLiteral("cp866"));
    }
    settings.load();
    CHECK(settings.fallbackEncodingChoice() == TextEncoding::Ibm866, QStringLiteral("синоним из ini"));
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("App/fallbackEncoding"), QStringLiteral("UTF-8"));
    }
    settings.load();
    CHECK(!settings.fallbackEncodingChoice()
              && settings.fallbackEncoding() == TextEncodings::systemFallback(),
          QStringLiteral("не однобайтная в ini — по локали системы"));
    settings.setFallbackEncodingChoice(std::nullopt);
    settings.save();
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("App/fallbackEncoding")).toString() == QStringLiteral("system"),
              QStringLiteral("умолчание в ini — system"));
    }
}

// Объединение вкладок: состояние кодировки едет с файлом.
static void checkMergeKeepsChoice(const QDir& dir, const QString& name, const QString& schema)
{
    g_context = name + QStringLiteral(" / объединение вкладок");
    const QString text = denseText(100);
    const QString chosen = writeFile(dir, name + "-merge-chosen.log",
                                     encodeText(text, TextEncoding::Windows1251));
    const QString detected = writeFile(dir, name + "-merge-auto.log",
                                       encodeText(denseText(100, 50), TextEncoding::Ibm866));
    auto src = openTab(schema, { chosen, detected });
    src->setFileEncoding(src->loadedFiles().value(0), TextEncoding::Koi8R);
    CHECK(settled(*src), QStringLiteral("выбор в источнике"));
    const QString dstPath = writeFile(dir, name + "-merge-dst.log", asciiText(10).toUtf8());
    auto dst = openTab(schema, { dstPath });
    dst->addFilesFrom(*src);
    CHECK(settled(*dst), QStringLiteral("объединение"));
    CHECK(dst->fileCount() == 3, QStringLiteral("три файла"));
    LogFilePtr c, d;
    for (const LogFilePtr& lf : dst->loadedFiles()) {
        if (lf->filePath == chosen) c = lf;
        if (lf->filePath == detected) d = lf;
    }
    CHECK(c && dst->fileEncoding(c).source == FileEncoding::Source::User
              && dst->fileEncoding(c).decoder.encoding() == TextEncoding::Koi8R
              && dst->fileEncoding(c).automatic().decoder.encoding() == TextEncoding::Windows1251,
          QStringLiteral("ручной выбор и автоматика рядом перенесены"));
    CHECK(d && dst->fileEncoding(d).source == FileEncoding::Source::Auto
              && dst->fileEncoding(d).decoder.encoding() == TextEncoding::Ibm866,
          QStringLiteral("решение автоматики перенесено"));
    const int row = rowOfFile(*dst->model(), chosen);
    CHECK(row >= 0 && dst->model()->messageAt(row)
                          == decodedLine(text, TextEncoding::Windows1251, TextEncoding::Koi8R, 0),
          QStringLiteral("текст в приёмнике — выбранной кодировкой: ") + dst->model()->messageAt(row));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid())
        return 2;
    const QDir dir(tmp.path());
    const QString schema = buildSchema();
    AppSettings& settings = AppSettings::instance();
    settings.setFallbackEncodingChoice(std::nullopt);

    checkLabels();
    checkUtf16(dir, schema);
    for (const int threshold : { 512, 0 }) {
        settings.setIndexedThresholdMB(threshold);
        const QString name = threshold ? QStringLiteral("resident") : QStringLiteral("indexed");
        checkIndicator(dir, name, schema);
        checkMergedTab(dir, name, schema);
        checkCommandLineEncoding(dir, name, schema);
        checkFallbackSetting(dir, name, schema);
        checkMergeKeepsChoice(dir, name, schema);
    }
    settings.setIndexedThresholdMB(512);

    std::fprintf(failures ? stderr : stdout, "encoding_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
