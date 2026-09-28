// ============================================================================
// logview_lifecycle — жизненный цикл вкладки (LogViewWidget) на живых файлах:
//   • дозапись многострочной записи по частям, в том числе строки, дописанной
//     в два приёма (без '\n' между ними), — на обоих бэкендах;
//   • файл переписан заново (ротация) — полная перезагрузка;
//   • растущий файл пересёк порог индексного бэкенда — вкладка на лету
//     становится индексной, строки те же;
//   • вкладку закрыли посреди загрузки — без падения.
// ============================================================================

#include "appsettings.h"
#include "logviewwidget.h"
#include "testdocuments.h"

#include <QApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static QByteArray record(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); }, 60000)
        && testdocs::settle(*tab.model());
}

static QStringList rows(const LogModel& model)
{
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row)
        out << model.messageAt(row);
    return out;
}

// Дозапись: новая запись, её продолжение, затем строка в два приёма.
static void checkAppendInParts(const QDir& dir, const QString& name, const QString& schema)
{
    const QString path = testdocs::writeFile(dir, name + ".log",
        record(0, "INFO", "first") + record(1, "INFO", "second"));
    LogViewWidget tab;
    tab.setParserPattern(schema);
    tab.setExtractionEnabled(true);
    tab.addLogFile(path);
    CHECK(settled(tab), name + ": initial load");
    LogModel& model = *tab.model();
    CHECK(model.rowCount() == 2, name + ": two lines loaded");

    const auto append = [&](const QByteArray& bytes) {
        CHECK(testdocs::appendToFile(path, bytes), name + ": append");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), name + ": reload settles");
    };

    append(record(2, "ERROR", "failure"));
    CHECK(model.rowCount() == 3 && model.messageAt(2).endsWith("failure"),
          name + ": appended record shown");
    append("    at continuation one\n");
    CHECK(model.rowCount() == 4 && model.messageAt(3) == "    at continuation one",
          name + ": continuation shown");
    CHECK(model.keyForRow(3).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + ": continuation belongs to the record it continues");

    // Строка дописана в два приёма: до перевода строки её видно частично,
    // после — целиком, одной строкой, а не двумя.
    append("    at contin");
    append("uation two\n");
    const QStringList text = rows(model);
    CHECK(model.rowCount() == 5, name + ": a line written in two parts is one row: "
                                     + text.join(" | "));
    CHECK(!text.isEmpty() && text.last() == "    at continuation two",
          name + ": the completed line has its full text: " + (text.isEmpty() ? QString() : text.last()));
    CHECK(model.rowCount() == 5
              && model.keyForRow(4).logicalEntryId == model.keyForRow(2).logicalEntryId,
          name + ": the completed continuation stays in its record");

    append(record(3, "INFO", "after"));
    CHECK(model.rowCount() == 6 && model.messageAt(5).endsWith("after"),
          name + ": next record after the completed line");
    CHECK(model.keyForRow(5).logicalEntryId != model.keyForRow(2).logicalEntryId,
          name + ": next record starts a new logical record");

    // Пустая строка в дозаписи — такая же строка, как при первой загрузке.
    append(QByteArray("\n") + record(4, "INFO", "after blank"));
    CHECK(model.rowCount() == 8 && model.messageAt(6).isEmpty()
              && model.messageAt(7).endsWith("after blank"),
          name + ": a blank appended line is kept: " + rows(model).join(" | "));

    // Ротация: файл переписан заново и стал короче — полная перезагрузка.
    QFile rewrite(path);
    CHECK(rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate), name + ": rewrite");
    rewrite.write(record(10, "WARN", "rotated"));
    rewrite.close();
    tab.reloadChangedFiles(/*force=*/true);
    CHECK(settled(tab), name + ": rewrite settles");
    CHECK(model.rowCount() == 1 && model.messageAt(0).endsWith("rotated"),
          name + ": rewritten file is reloaded from scratch: " + rows(model).join(" | "));
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();
    AppSettings& settings = AppSettings::instance();

    // Резидентная вкладка (порог по умолчанию — сотни МБ) и индексная (порог 0).
    checkAppendInParts(dir, QStringLiteral("resident"), schema);
    settings.setIndexedThresholdMB(0);
    checkAppendInParts(dir, QStringLiteral("indexed"), schema);

    // Растущий файл пересекает порог: вкладка становится индексной на лету.
    {
        settings.setIndexedThresholdMB(1);
        QByteArray start;
        for (int i = 0; i < 2000; ++i)
            start += record(i / 50, "INFO", "line " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "grows.log", start);
        LogViewWidget tab;
        tab.setParserPattern(schema);
        tab.setExtractionEnabled(true);
        tab.addLogFile(path);
        CHECK(settled(tab), "threshold: initial load");
        CHECK(!tab.model()->isIndexedBackend(), "threshold: small file starts resident");
        QByteArray more;
        for (int i = 2000; i < 30000; ++i)
            more += record(i / 50, "INFO", "line " + QByteArray::number(i));
        CHECK(testdocs::appendToFile(path, more), "threshold: append past the threshold");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold: conversion settles");
        CHECK(tab.model()->isIndexedBackend(), "threshold: tab converted to indexed");
        CHECK(tab.model()->rowCount() == 30000, "threshold: every line present after conversion");
        CHECK(tab.model()->messageAt(29999).endsWith("line 29999")
                  && tab.model()->messageAt(0).endsWith("line 0"),
              "threshold: first and last lines intact");
        CHECK(testdocs::appendToFile(path, record(500, "INFO", "tail after conversion")),
              "threshold: append after conversion");
        tab.reloadChangedFiles(/*force=*/true);
        CHECK(settled(tab), "threshold: tail after conversion settles");
        CHECK(tab.model()->rowCount() == 30001
                  && tab.model()->messageAt(30000).endsWith("tail after conversion"),
              "threshold: the indexed tab keeps following the tail");
    }

    // Вкладку закрыли посреди загрузки: воркеры останавливаются, падения нет.
    {
        settings.setIndexedThresholdMB(512);
        QByteArray big;
        big.reserve(300000 * 60);
        for (int i = 0; i < 300000; ++i)
            big += record(i / 1000, "INFO", "row " + QByteArray::number(i));
        const QString path = testdocs::writeFile(dir, "closing.log", big);
        for (const int threshold : {512, 0}) {
            settings.setIndexedThresholdMB(threshold);
            auto tab = std::make_unique<LogViewWidget>();
            tab->setParserPattern(schema);
            tab->addLogFile(path);
            testdocs::pump(20);
            const bool wasLoading = tab->isLoading();
            tab.reset();
            testdocs::pump(200);
            CHECK(true, "closing a loading tab does not crash");
            if (!wasLoading)
                std::fprintf(stdout, "note: load finished before closing (threshold %d)\n", threshold);
        }
    }

    std::fprintf(stdout, "LogViewWidget lifecycle: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
