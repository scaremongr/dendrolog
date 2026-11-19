// ============================================================================
// goto_location — переход к строке файла и ко времени (Ctrl+G):
//   • разбор ввода: номер строки либо время в нескольких формах;
//   • номер строки → видимая строка на обоих бэкендах, одно- и многофайловая
//     вкладка, с фильтром и без: скрытая строка даёт ближайшую видимую
//     (следующую за её местом в порядке вкладки, иначе последнюю), номер за
//     концом файла — последнюю строку файла. Эталон — перебор;
//   • время → первая видимая строка не раньше него, позже конца — последняя
//     строка с меткой;
//   • переход во view — выбор пользователя: строка выделена и переживает
//     фильтрацию (инвариант 10);
//   • диалог: подпись и кнопка Go следуют вводу, список файлов есть только у
//     вкладки с несколькими файлами и задаёт файл номера строки.
// ============================================================================

#include "LogListView.h"
#include "gotodialog.h"
#include "gotolocation.h"
#include "statisticspanel.h"
#include "testdocuments.h"

#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimeZone>
#include <cstdio>
#include <limits>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static QByteArray line(int second, const char* level, const QByteArray& text)
{
    return QByteArray("2026-03-05 10:") + QByteArray::number(second / 60).rightJustified(2, '0')
         + ':' + QByteArray::number(second % 60).rightJustified(2, '0') + ".000 [w] "
         + level + " - " + text + '\n';
}

// 2000 записей с растущим временем; у каждой седьмой — две строки продолжения,
// так что номер строки файла и номер записи расходятся.
static QByteArray fixture(int firstSecond, const QByteArray& tag)
{
    QByteArray out;
    for (int i = 0; i < 2000; ++i) {
        out += line(firstSecond + i, i % 5 == 0 ? "ERROR" : "INFO", tag + ' ' + QByteArray::number(i));
        if (i % 7 == 3)
            out += "    at frame one\n    at frame two\n";
    }
    return out;
}

struct LineKey {
    const LogFile* file = nullptr;
    qint64 line = 0;
    bool operator==(const LineKey& o) const { return file == o.file && line == o.line; }
};
static size_t qHash(const LineKey& k, size_t seed = 0)
{
    return qHashMulti(seed, quintptr(k.file), k.line);
}

static QVector<LineKey> visibleKeys(const LogModel& model)
{
    QVector<LineKey> keys;
    keys.reserve(model.rowCount());
    for (int row = 0; row < model.rowCount(); ++row) {
        const auto entry = model.entryAt(row);
        keys.append({entry->sourceFile().get(), entry->originalLineNumber()});
    }
    return keys;
}

static GoToTarget lineTarget(qint64 n)
{
    GoToTarget t;
    t.kind = GoToTarget::Kind::Line;
    t.line = n;
    return t;
}

// Сверка перехода по номеру строки с перебором. fullOrder — строки вкладки без
// фильтров: видимый список — их подпоследовательность.
static void checkLines(const QString& name, const LogModel& model,
                       const QVector<LineKey>& fullOrder)
{
    QHash<LineKey, int> position;
    QHash<const LogFile*, qint64> lastLine;
    for (int i = 0; i < fullOrder.size(); ++i) {
        position.insert(fullOrder[i], i);
        qint64& last = lastLine[fullOrder[i].file];
        last = qMax(last, fullOrder[i].line);
    }
    const QVector<LineKey> visible = visibleKeys(model);
    CHECK(!visible.isEmpty(), name + ": fixture leaves visible rows");

    int mismatches = 0;
    for (auto it = lastLine.constBegin(); it != lastLine.constEnd(); ++it) {
        const LogFile* file = it.key();
        const qint64 last = it.value();
        QVector<qint64> wanted = {1, 2, 3, 4, 5, 6, 7, 17, 18, 19, last / 2, last - 1, last,
                                  last + 1, last + 100000};
        for (qint64 n = 20; n < last; n += 97)
            wanted.append(n);
        for (const qint64 n : wanted) {
            const LineKey key{file, qMin(n, last)};
            const int place = position.value(key, -1);
            int expected = -1;
            for (int row = 0; row < visible.size(); ++row) {
                if (position.value(visible[row]) >= place) {
                    expected = row;
                    break;
                }
            }
            if (expected < 0)
                expected = int(visible.size()) - 1;
            const bool expectedExact = visible[expected] == LineKey{file, n};

            const GoToResult got = GoToLocation::resolve(model, lineTarget(n), file);
            if (place < 0 || got.row != expected || got.exact != expectedExact
                || got.line != visible[expected].line) {
                if (++mismatches <= 5)
                    std::fprintf(stderr, "  %s line %lld: row %d exact %d line %lld, expected row %d exact %d line %lld\n",
                                 qPrintable(name), (long long)n, got.row, int(got.exact),
                                 (long long)got.line, expected, int(expectedExact),
                                 (long long)visible[expected].line);
            }
        }
    }
    CHECK(mismatches == 0, name + ": line number resolves as brute force does");
}

static void checkTimes(const QString& name, const LogModel& model)
{
    const int n = model.rowCount();
    int lastStamped = -1;
    for (int row = 0; row < n; ++row)
        if (model.visibleTimestampAt(row).isValid())
            lastStamped = row;
    const QDateTime base(QDate(2026, 3, 5), QTime(10, 0, 0));
    int mismatches = 0;
    for (const int offsetMs : {-5000, 0, 500, 1000, 61000, 61001, 1000 * 1000, 2500 * 1000,
                               5000 * 1000}) {
        GoToTarget target;
        target.kind = GoToTarget::Kind::Time;
        target.time = base.addMSecs(offsetMs);
        int expected = lastStamped;
        for (int row = 0; row < n; ++row) {
            const QDateTime ts = model.visibleTimestampAt(row);
            if (ts.isValid() && ts >= target.time) {
                expected = row;
                break;
            }
        }
        const GoToResult got = GoToLocation::resolve(model, target, nullptr);
        const bool expectedExact =
            expected >= 0 && model.visibleTimestampAt(expected) == target.time;
        if (got.row != expected || got.exact != expectedExact
            || (expected >= 0 && got.time != model.visibleTimestampAt(expected))) {
            if (++mismatches <= 5)
                std::fprintf(stderr, "  %s +%d ms: row %d exact %d, expected row %d exact %d\n",
                             qPrintable(name), offsetMs, got.row, int(got.exact), expected,
                             int(expectedExact));
        }
    }
    CHECK(mismatches == 0, name + ": time resolves to the first visible row at or after it");
}

static void checkDocument(const QString& name, testdocs::Document& doc)
{
    LogModel& model = *doc.model;
    CHECK(doc.errors.isEmpty(), name + ": document loads");
    const QVector<LineKey> fullOrder = visibleKeys(model);

    checkLines(name + " unfiltered", model, fullOrder);
    checkTimes(name + " unfiltered", model);

    model.setLogLevelFilter({LogLevel::Error});
    CHECK(testdocs::settle(model), name + ": level filter settles");
    CHECK(model.rowCount() > 0 && model.rowCount() < fullOrder.size(), name + ": filter hides rows");
    checkLines(name + " errors only", model, fullOrder);
    checkTimes(name + " errors only", model);

    // Файл не из этой вкладки и пустой видимый набор — перехода нет.
    LogFile stranger(QStringLiteral("/nowhere/stranger.log"));
    CHECK(GoToLocation::resolve(model, lineTarget(5), &stranger).row == -1,
          name + ": a file of another tab resolves to nothing");
    model.setLogLevelFilter({LogLevel::Fatal});
    CHECK(testdocs::settle(model) && model.rowCount() == 0, name + ": everything hidden");
    CHECK(GoToLocation::resolve(model, lineTarget(5), fullOrder.first().file).row == -1,
          name + ": nothing visible resolves to nothing");

    model.setLogLevelFilter({});
    CHECK(testdocs::settle(model) && model.rowCount() == fullOrder.size(), name + ": filter reset");
}

static void checkParsing()
{
    using Kind = GoToTarget::Kind;
    const QDate day(2026, 3, 5);
    const QDateTime reference(day, QTime(12, 0));
    const auto parse = [&reference](const char* text) {
        return GoToLocation::parse(QString::fromUtf8(text), reference);
    };
    const auto at = [&day](int h, int m, int s, int ms) {
        return QDateTime(day, QTime(h, m, s, ms));
    };

    CHECK(parse("1234").kind == Kind::Line && parse("1234").line == 1234, "digits are a line number");
    CHECK(parse("  42 ").kind == Kind::Line && parse("  42 ").line == 42, "spaces around are ignored");
    CHECK(parse("0").kind == Kind::Line && parse("0").line == 1, "line 0 means the first line");
    CHECK(parse("1 234").kind == Kind::Line && parse("1 234").line == 1234, "digit grouping by spaces");
    CHECK(parse("99999999999999999999").kind == Kind::Invalid, "overflowing number is invalid");

    CHECK(parse("10:02").kind == Kind::Time && parse("10:02").time == at(10, 2, 0, 0), "HH:mm");
    CHECK(parse("9:02").time == at(9, 2, 0, 0), "H:mm");
    CHECK(parse("10:02:03").time == at(10, 2, 3, 0), "HH:mm:ss");
    CHECK(parse("10:02:03.5").time == at(10, 2, 3, 500), "fraction is cut to milliseconds");
    CHECK(parse("10:02:03,250").time == at(10, 2, 3, 250), "comma fraction");
    CHECK(parse("2026-03-06 10:02").time == QDateTime(QDate(2026, 3, 6), QTime(10, 2)),
          "date and HH:mm");
    CHECK(parse("2026-03-06T10:02:03.123").time == QDateTime(QDate(2026, 3, 6), QTime(10, 2, 3, 123)),
          "ISO date and time");
    CHECK(parse("2026-03-06").time == QDateTime(QDate(2026, 3, 6), QTime(0, 0)), "date only is midnight");
    CHECK(parse("2026-03-06 10:02:03Z").time
              == QDateTime(QDate(2026, 3, 6), QTime(10, 2, 3), QTimeZone::UTC),
          "explicit zone sets the exact moment");
    CHECK(parse("10:02:03+03:00").time
              == QDateTime(day, QTime(7, 2, 3), QTimeZone::UTC),
          "zone on a time without a date");

    const QDateTime sevenUtc(day, QTime(7, 2, 3), QTimeZone::UTC);
    for (const char* zoned : {"10:02:03+0300", "10:02:03 +03", "10:02:03 +03:00", "07:02:03 utc",
                              "07:02:03z", "07:02:03 GMT", "05:32:03-01:30"})
        CHECK(parse(zoned).time == sevenUtc, QStringLiteral("zone form '%1'").arg(QLatin1String(zoned)));

    for (const char* bad : {"", "   ", "abc", "12:", "25:00", "10:61", "10:02:99", "2026-13-01",
                            "2026-03-06 xx", "-5", "12a", "10:02 junk", "10:02+15:00",
                            "10:02+03:70", "2026-03-06 Z"})
        CHECK(parse(bad).kind == Kind::Invalid, QStringLiteral("'%1' is invalid").arg(QLatin1String(bad)));

    // Время без даты требует опорного момента.
    CHECK(GoToLocation::parse(QStringLiteral("10:02"), QDateTime()).kind == Kind::Invalid,
          "time needs a reference moment");
    CHECK(GoToLocation::parse(QStringLiteral("2026-03-06 10:02"), QDateTime()).kind == Kind::Time,
          "full date does not need one");

    // Время с зоной берёт день опорного момента в ЭТОЙ зоне, а не локальный:
    // 2026-03-04 22:30Z — это уже 5 марта в +03:00 и ещё 4-е по UTC.
    const QDateTime lateUtc(QDate(2026, 3, 4), QTime(22, 30), QTimeZone::UTC);
    CHECK(GoToLocation::parse(QStringLiteral("22:30Z"), lateUtc).time == lateUtc,
          "zoned time takes the reference day in its own zone (UTC)");
    CHECK(GoToLocation::parse(QStringLiteral("01:30+03:00"), lateUtc).time == lateUtc,
          "zoned time takes the reference day in its own zone (+03:00)");
}

// Переход во view — выбор пользователя: строка выделена, видна и остаётся
// текущей записью после фильтрации.
static void checkView(const QString& name, testdocs::Document& doc)
{
    LogModel& model = *doc.model;
    LogListView view;
    view.resize(900, 400);
    view.setModel(&model);
    view.show();
    // Паузы после show нет намеренно (Q-12): переход и фильтр попадают в
    // окно отложенного пересчёта после resize, и он не должен их отменить.
    // Позицию проверяем, когда view закончил пересчёты, — ждём состояние.
    const auto settleView = [&name](LogListView& v) {
        CHECK(testdocs::waitFor([&v] { return !v.hasPendingLayout(); }, 10000),
              name + ": view settles");
        testdocs::pump(0);   // отложенные на один оборот цикла прокрутки
    };
    CHECK(view.hasPendingLayout(), name + ": the resize recalculation is still pending");

    const LogFile* file = model.entryAt(0)->sourceFile().get();
    // Строка 1001 файла: запись с ERROR или нет — ищем ближайшую ERROR-строку
    // перебором, чтобы после фильтра она осталась видимой.
    qint64 wanted = 0;
    for (int row = model.rowCount() / 2; row < model.rowCount(); ++row) {
        const auto entry = model.entryAt(row);
        if (entry->sourceFile().get() == file && model.visibleLevelAt(row) == LogLevel::Error
            && entry->timestamp().isValid() && !entry->isPlainText()) {
            wanted = entry->originalLineNumber();
            break;
        }
    }
    CHECK(wanted > 0, name + ": fixture has an error line");

    const GoToResult target = GoToLocation::resolve(model, lineTarget(wanted), file);
    CHECK(target.row >= 0 && target.exact, name + ": visible line resolves exactly");
    view.goToRow(target.row);
    CHECK(view.hasPendingLayout(), name + ": the jump lands before the resize recalculation");

    const auto currentLine = [&]() -> qint64 {
        const QModelIndex cur = view.currentIndex();
        if (!cur.isValid() || !view.selectionModel()->isRowSelected(cur.row(), QModelIndex()))
            return -1;
        const auto entry = model.entryAt(cur.row());
        return entry->sourceFile().get() == file ? entry->originalLineNumber() : -2;
    };
    const auto currentShown = [&]() {
        const QRect r = view.visualRect(view.currentIndex());
        return r.isValid() && view.viewport()->rect().intersects(r);
    };
    CHECK(currentLine() == wanted, name + ": jump selects the row");
    CHECK(currentShown(), name + ": jump scrolls the row into view");
    settleView(view);
    CHECK(currentShown(), name + ": the row stays in view after the resize recalculation");

    // То же с фильтром: новое окно пересчёта, фильтр применён внутри него.
    view.resize(900, 420);
    testdocs::pump(0);
    CHECK(view.hasPendingLayout(), name + ": the second resize recalculation is pending");
    model.setLogLevelFilter({LogLevel::Error});
    CHECK(testdocs::settle(model), name + ": filter settles");
    settleView(view);
    CHECK(currentLine() == wanted, name + ": selection survives the filter");
    CHECK(currentShown(), name + ": row stays in view after the filter");

    model.setLogLevelFilter({});
    CHECK(testdocs::settle(model), name + ": reset settles");
    settleView(view);
    CHECK(currentLine() == wanted, name + ": selection survives the reset");
    CHECK(currentShown(), name + ": row stays in view after the reset");

    view.goToRow(-1);
    view.goToRow(model.rowCount());
    CHECK(currentLine() == wanted, name + ": out-of-range row is ignored");

    // Follow-tail вернул бы вид к хвосту на первой же дозаписи — переход его снимает.
    view.setFollowTail(true);
    view.goToRow(target.row);
    CHECK(!view.followTail(), name + ": jump turns follow-tail off");
    view.setFollowTail(true);
    view.goToRow(model.rowCount() - 1);
    CHECK(view.followTail(), name + ": jump to the last row keeps follow-tail");
    view.setFollowTail(false);

    // Строка уже текущая, но назначена не пользователем (Qt по фокусу: без
    // выделения, якорь не тронут): currentChanged при переходе к ней не
    // придёт, и якорь обязан поставить сам переход.
    LogListView fresh;
    fresh.resize(900, 400);
    fresh.setModel(&model);
    fresh.show();
    QFocusEvent focus(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
    QApplication::sendEvent(&fresh, &focus);
    const int assigned = fresh.currentIndex().row();
    CHECK(assigned >= 0 && !fresh.selectionModel()->isRowSelected(assigned, QModelIndex()),
          name + ": focus assigns an unselected current row");
    if (assigned >= 0) {
        const auto entry = model.entryAt(assigned);
        const LogFile* assignedFile = entry->sourceFile().get();
        const qint64 assignedLine = entry->originalLineNumber();
        fresh.goToRow(assigned);
        model.setLogLevelFilter({model.visibleLevelAt(assigned)});
        CHECK(testdocs::settle(model), name + ": filter settles");
        settleView(fresh);
        const QModelIndex cur = fresh.currentIndex();
        const auto now = cur.isValid() ? model.entryAt(cur.row()) : nullptr;
        CHECK(now && now->sourceFile().get() == assignedFile
                  && now->originalLineNumber() == assignedLine
                  && fresh.selectionModel()->isRowSelected(cur.row(), QModelIndex()),
              name + ": jump to the already current row becomes the anchor");
        model.setLogLevelFilter({});
        CHECK(testdocs::settle(model), name + ": reset settles");
    }
}

static void checkDialog(const QString& name, testdocs::Document& doc)
{
    LogModel& model = *doc.model;
    const bool severalFiles = doc.files.size() > 1;
    // Текущая строка — из последнего файла: он и должен быть выбран в списке.
    const LogFile* lastFile = doc.files.last().get();
    int currentRow = -1;
    for (int row = 0; row < model.rowCount() && currentRow < 0; ++row)
        if (model.keyForRow(row).sourceFile == lastFile)
            currentRow = row;

    GoToDialog dialog(&model, doc.files.toList(), currentRow);
    auto* input = dialog.findChild<QLineEdit*>("goToInput");
    auto* combo = dialog.findChild<QComboBox*>("goToFile");
    auto* preview = dialog.findChild<QLabel*>("goToPreview");
    auto* go = dialog.findChild<QPushButton*>("goToButton");
    CHECK(input && combo && preview && go, name + ": dialog has its controls");
    if (!input || !combo || !preview || !go)
        return;
    dialog.show();
    testdocs::pump(30);

    CHECK(!go->isEnabled() && !preview->text().isEmpty(), name + ": empty input shows a hint, Go is off");
    CHECK(combo->isVisible() == severalFiles, name + ": file list only for several files");
    CHECK(combo->currentData().value<LogFilePtr>().get() == lastFile,
          name + ": file of the current row is preselected");

    input->setText("nonsense");
    CHECK(!go->isEnabled(), name + ": invalid input keeps Go off");

    input->setText("40");
    CHECK(go->isEnabled() && preview->text().contains("40"), name + ": line number enables Go");
    go->click();
    CHECK(dialog.result() == QDialog::Accepted, name + ": Go accepts");
    const GoToResult byLine = dialog.target();
    CHECK(byLine.row >= 0 && byLine.exact && byLine.line == 40 && byLine.file.get() == lastFile,
          name + ": line of the selected file");
    CHECK(dialog.targetNote().isEmpty(), name + ": exact jump has no note");

    if (severalFiles) {
        GoToDialog other(&model, doc.files.toList(), currentRow);
        auto* otherCombo = other.findChild<QComboBox*>("goToFile");
        otherCombo->setCurrentIndex(0);
        other.findChild<QLineEdit*>("goToInput")->setText("40");
        other.accept();
        CHECK(other.target().exact && other.target().file == doc.files.first()
                  && other.target().row != byLine.row,
              name + ": another file gives another row");
    }

    // Время за концом лога: переход к последней строке с меткой и пояснение.
    GoToDialog late(&model, doc.files.toList(), currentRow);
    late.findChild<QLineEdit*>("goToInput")->setText("23:59");
    late.accept();
    CHECK(late.result() == QDialog::Accepted && late.target().row >= 0 && !late.target().exact
              && !late.targetNote().isEmpty(),
          name + ": inexact jump explains itself");

    // Ввод, который ничего не находит, диалог не закрывает.
    GoToDialog invalid(&model, doc.files.toList(), currentRow);
    invalid.findChild<QLineEdit*>("goToInput")->setText("abc");
    invalid.accept();
    CHECK(invalid.result() != QDialog::Accepted && invalid.target().row < 0,
          name + ": invalid input does not accept");
}

// F-13. Переход ко времени до 1970 года: ввод разбирается в отрицательный
// момент, и строка находится на обоих бэкендах — по заданным моментам.
static void checkPre1970(const QDir& dir, const QString& schema)
{
    const auto utcMs = [](const char* text) {
        const GoToTarget t = GoToLocation::parse(QString::fromUtf8(text), QDateTime());
        return t.kind == GoToTarget::Kind::Time ? t.time.toMSecsSinceEpoch()
                                                : std::numeric_limits<qint64>::min();
    };
    CHECK(utcMs("1969-12-31T23:59:59.999Z") == -1, "pre-1970: -1 ms parses");
    CHECK(utcMs("1970-01-01 00:00:00Z") == 0, "pre-1970: the epoch parses");
    CHECK(utcMs("1970-01-01 01:00:00+02:00") == -3600000, "pre-1970: zone gives a negative moment");
    CHECK(GoToLocation::parse(QStringLiteral("1969-07-20 20:17"), QDateTime()).time
              == QDateTime(QDate(1969, 7, 20), QTime(20, 17)),
          "pre-1970: local date and time parse");

    const QString path = testdocs::writeFile(dir, "pre1970.log",
        "1969-12-31T23:59:59.000Z [main] INFO - p0\n"
        "1969-12-31T23:59:59.999Z [main] INFO - p1\n"
        "1970-01-01T00:00:00.000Z [main] ERROR - p2\n"
        "1970-01-01T00:00:00.500Z [main] INFO - p3\n");
    const struct { qint64 at; int row; bool exact; } cases[] = {
        { -1000000000000LL, 0, false }, { -1500, 0, false }, { -1000, 0, true },
        { -999, 1, false }, { -1, 1, true }, { 0, 2, true }, { 1, 3, false },
        { 500, 3, true }, { 501, 3, false },
    };
    const auto check = [&](const QString& name, testdocs::Document doc) {
        const LogModel& model = *doc.model;
        CHECK(doc.errors.isEmpty() && model.rowCount() == 4, name + ": pre-1970 document loads");
        for (const auto& c : cases) {
            GoToTarget target;
            target.kind = GoToTarget::Kind::Time;
            target.time = QDateTime::fromMSecsSinceEpoch(c.at);
            const GoToResult got = GoToLocation::resolve(model, target, nullptr);
            CHECK(got.row == c.row && got.exact == c.exact,
                  QStringLiteral("%1: time %2 ms → row %3 exact %4, expected row %5 exact %6")
                      .arg(name).arg(c.at).arg(got.row).arg(got.exact).arg(c.row).arg(c.exact));
        }
        // Опорный момент — метка строки, даже если она до 1970 года.
        CHECK(GoToLocation::referenceTime(model, 1).toMSecsSinceEpoch() == -1,
              name + ": reference moment of a -1 ms row");

        // Статистика: диапазон и счётчики меток видят отрицательные моменты.
        const DocumentStats stats = collectDocumentStats(model.scanSnapshot(false), nullptr, 0);
        CHECK(stats.tMinMs == -1000 && stats.tMaxMs == 500 && stats.recordsWithTs == 4,
              QStringLiteral("%1: statistics range %2 — %3, stamped %4")
                  .arg(name).arg(stats.tMinMs).arg(stats.tMaxMs).arg(stats.recordsWithTs));
        CHECK(stats.files.size() == 1 && stats.files[0].firstMs == -1000
                  && stats.files[0].lastMs == 500,
              name + ": statistics per file keep pre-1970 moments");
    };
    check("resident pre-1970", testdocs::buildResident({path}, schema));
    check("indexed pre-1970", testdocs::buildIndexed({path}, schema));

    // Края диапазона: годы 0001 и 9999 в одной вкладке — диапазон, корзины,
    // паузы и темп считаются без переполнения (UBSan в санитайзерной сборке);
    // строка без метки диапазон не задевает.
    const QString wide = testdocs::writeFile(dir, "wide.log",
        "0001-01-01T00:00:00.000Z [main] INFO - first\n"
        "1969-12-31T23:59:59.999Z [main] ERROR - before epoch\n"
        "9999-12-31T23:59:59.999Z [main] ERROR - last\n"
        "no stamp at all\n");
    const qint64 year1 = QDateTime(QDate(1, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    const qint64 year9999 =
        QDateTime(QDate(9999, 12, 31), QTime(23, 59, 59, 999), QTimeZone::UTC).toMSecsSinceEpoch();
    const auto checkWide = [&](const QString& name, testdocs::Document doc) {
        const LogModel& model = *doc.model;
        const DocumentStats stats = collectDocumentStats(model.scanSnapshot(false), nullptr, 0);
        CHECK(stats.tMinMs == year1 && stats.tMaxMs == year9999 && stats.recordsWithTs == 3,
              QStringLiteral("%1: statistics over years 0001-9999: %2 — %3, stamped %4")
                  .arg(name).arg(stats.tMinMs).arg(stats.tMaxMs).arg(stats.recordsWithTs));
        CHECK(stats.avgPerSec > 0 && stats.peakFromMs >= year1 && stats.peakToMs <= year9999 + 1,
              name + ": rate and peak bucket stay inside the range");
        GoToTarget target;
        target.kind = GoToTarget::Kind::Time;
        target.time = QDateTime::fromMSecsSinceEpoch(year9999);
        CHECK(GoToLocation::resolve(model, target, nullptr).row == 2,
              name + ": going to year 9999 finds its row");
    };
    checkWide("resident wide", testdocs::buildResident({wide}, schema));
    checkWide("indexed wide", testdocs::buildIndexed({wide}, schema));

    // Одно-файловая индексная вкладка держит порядок файла: преамбула без
    // метки сверху и строка 0001 года посреди 2026-го. Диапазон — от первой
    // до последней строки с меткой, далёкая строка прижимается к краю корзин
    // (без переполнения при приведении к int — UBSan).
    const QString stray = testdocs::writeFile(dir, "stray.log",
        "=== service started ===\n"
        "2026-03-05T10:00:00.000Z [main] INFO - a\n"
        "0001-01-01T00:00:00.000Z [main] ERROR - stray\n"
        "2026-03-05T11:00:00.000Z [main] INFO - b\n");
    const qint64 tenUtc = QDateTime(QDate(2026, 3, 5), QTime(10, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    {
        testdocs::Document doc = testdocs::buildIndexed({stray}, schema);
        const DocumentStats stats = collectDocumentStats(doc.model->scanSnapshot(false), nullptr, 0);
        CHECK(stats.tMinMs == tenUtc && stats.tMaxMs == tenUtc + 3600000 && stats.recordsWithTs == 3,
              QStringLiteral("indexed stray: range %1 — %2, stamped %3")
                  .arg(stats.tMinMs).arg(stats.tMaxMs).arg(stats.recordsWithTs));
    }
    {
        testdocs::Document doc = testdocs::buildResident({stray}, schema);
        const DocumentStats stats = collectDocumentStats(doc.model->scanSnapshot(false), nullptr, 0);
        CHECK(stats.tMinMs == year1 && stats.tMaxMs == tenUtc + 3600000 && stats.recordsWithTs == 3,
              QStringLiteral("resident stray: range %1 — %2, stamped %3")
                  .arg(stats.tMinMs).arg(stats.tMaxMs).arg(stats.recordsWithTs));
    }
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();

    checkParsing();
    checkPre1970(dir, schema);

    const QString first = testdocs::writeFile(dir, "first.log", fixture(0, "alpha"));
    // Второй файл начинается позже и пересекается с первым по времени.
    const QString second = testdocs::writeFile(dir, "second.log", fixture(900, "beta"));

    {
        testdocs::Document resident = testdocs::buildResident({first}, schema);
        testdocs::Document indexed = testdocs::buildIndexed({first}, schema);
        checkDocument("resident single", resident);
        checkDocument("indexed single", indexed);

        // Опорная дата для времени без даты: текущая строка, иначе начало лога.
        CHECK(GoToLocation::referenceTime(*resident.model, -1) == resident.model->visibleTimestampAt(0),
              "reference moment falls back to the first timestamp");
        CHECK(GoToLocation::referenceTime(*indexed.model, 10) == indexed.model->visibleTimestampAt(10),
              "reference moment of the current row");
        LogModel empty;
        CHECK(!GoToLocation::referenceTime(empty, -1).isValid(), "empty model has no reference moment");
        CHECK(GoToLocation::resolve(empty, lineTarget(1), nullptr).row == -1, "empty model resolves to nothing");

        checkDialog("resident dialog", resident);
        checkDialog("indexed dialog", indexed);
        checkView("resident view", resident);
        checkView("indexed view", indexed);
    }
    {
        testdocs::Document resident = testdocs::buildResident({first, second}, schema);
        testdocs::Document sequential =
            testdocs::buildIndexed({first, second}, schema, testdocs::IndexedLoad::Sequential);
        testdocs::Document concurrent =
            testdocs::buildIndexed({first, second}, schema, testdocs::IndexedLoad::Concurrent);
        checkDocument("resident merged", resident);
        checkDocument("indexed merged sequential", sequential);
        checkDocument("indexed merged concurrent", concurrent);
        checkDialog("resident merged dialog", resident);
        checkDialog("indexed merged dialog", concurrent);
        checkView("indexed merged view", sequential);
    }

    std::fprintf(stdout, "Go to location: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
