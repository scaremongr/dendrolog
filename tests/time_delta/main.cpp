// ============================================================================
// time_delta — дельта времени (F-8, docs/design/F-8.md) на обоих бэкендах:
//   • TimeDelta::format — таблица (мс, с, мин, ч, сутки, знак, годы
//     0001–9999 без переполнения, ширина не больше kMaxChars);
//   • колонка Δ и TimeDelta::forRow против перебора entryAt: одна и слитая
//     вкладка, фильтр уровней и текста, многострочные записи, метки до 1970,
//     строки без меток; предел прохода назад;
//   • «нулевая»: дельты от неё, повтор снимает, строка без метки не ставится,
//     переживает фильтр (скрытая работает), дозапись, полную перезагрузку и
//     переход порога;
//   • колонка: ширина постоянная, текст сдвигается ровно на неё, выбранная
//     строка на своём месте экрана при включении и выключении, [View]
//     timeDelta;
//   • Entry Details — обе дельты.
// ============================================================================

#include "appsettings.h"
#include "entrydetailspanel.h"
#include "logviewwidget.h"
#include "LogListView.h"
#include "testdocuments.h"
#include "timedelta.h"
#include "viewidle.h"

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

static bool settled(LogViewWidget& tab)
{
    return testdocs::waitFor([&] { return !tab.isLoading(); }, 60000)
        && testdocs::settleViews(*tab.model(), [&] { return tab.isLoading(); });
}

static QByteArray line(const char* ts, const char* level, const QByteArray& text)
{
    return QByteArray(ts) + " [w] " + level + " - " + text + '\n';
}

// Строки без меток в начале, метки до 1970 и ровно эпоха, многострочная
// запись, метки не по порядку (одно-файловая индексная их не сортирует).
static QByteArray fileA()
{
    return QByteArray("preamble without a timestamp\n")
         + "another preamble line\n"
         + line("1965-06-01 12:00:00.000", "INFO", "before the epoch")
         + line("1970-01-01 00:00:00.000", "WARN", "at the epoch")
         + "    continuation one\n"
         + "    continuation two needle\n"
         + line("2026-03-05 10:00:00.000", "ERROR", "first error needle")
         + line("2026-03-05 10:00:00.250", "INFO", "quarter second later")
         + line("2026-03-05 10:01:05.000", "INFO", "a minute later needle")
         + line("2026-03-05 09:00:00.000", "WARN", "an hour earlier")
         + line("2026-03-07 15:30:00.000", "ERROR", "two days later")
         + "    trace line needle\n";
}

static QByteArray fileB()
{
    return line("2026-03-05 10:00:00.100", "INFO", "b first")
         + line("2026-03-05 10:00:30.000", "ERROR", "b error needle")
         + "    b continuation\n"
         + line("1969-12-31 23:59:59.999", "WARN", "b just before the epoch");
}

// Ожидаемое — перебором записей строк.
struct Expected {
    QString column;
    bool hasPrevious = false;
    qint64 fromPrevious = 0;
};

static QVector<Expected> expectedDeltas(const LogModel& model)
{
    QVector<Expected> out(model.rowCount());
    for (int r = 0; r < model.rowCount(); ++r) {
        const auto e = model.entryAt(r);
        const bool timed = e->timestamp().isValid();
        const auto sameRecord = [&](int other) {
            const auto o = model.entryAt(other);
            return o->sourceFile() == e->sourceFile() && o->logicalEntryId() == e->logicalEntryId();
        };
        const bool first = r == 0 || !sameRecord(r - 1);
        int prev = -1;
        for (int p = r - 1; p >= 0; --p) {
            if (model.entryAt(p)->timestamp().isValid() && !sameRecord(p)) {
                prev = p;
                break;
            }
        }
        Expected x;
        const qint64 ms = timed ? e->timestamp().toMSecsSinceEpoch() : 0;
        if (timed && prev >= 0) {
            x.hasPrevious = true;
            x.fromPrevious = ms - model.entryAt(prev)->timestamp().toMSecsSinceEpoch();
        }
        if (model.hasTimeZero()) {
            const bool zero = model.isTimeZeroRow(r);
            x.column = zero ? QStringLiteral("= 0")
                     : timed && first ? TimeDelta::format(ms - model.timeZero().ms) : QString();
        } else {
            x.column = timed && first && x.hasPrevious ? TimeDelta::format(x.fromPrevious) : QString();
        }
        out[r] = x;
    }
    return out;
}

static void checkAgainstBruteForce(LogViewWidget& tab, const QString& label)
{
    const LogModel& model = *tab.model();
    const QVector<Expected> want = expectedDeltas(model);
    int bad = 0;
    QString first;
    for (int r = 0; r < model.rowCount(); ++r) {
        const TimeDelta::RowDelta d = TimeDelta::forRow(model, r);
        const QString text = tab.view()->timeDeltaText(r);
        const bool ok = text == want[r].column && d.hasPrevious == want[r].hasPrevious
                     && (!d.hasPrevious || d.fromPrevious == want[r].fromPrevious);
        if (!ok && bad++ == 0)
            first = QStringLiteral("row %1 '%2': '%3' vs '%4'").arg(r).arg(model.messageAt(r), text, want[r].column);
    }
    CHECK(bad == 0, QStringLiteral("%1: deltas match brute force (%2 bad; %3)").arg(label).arg(bad).arg(first));
}

static int rowWithText(const LogModel& model, const QString& text)
{
    for (int r = 0; r < model.rowCount(); ++r)
        if (model.messageAt(r).contains(text))
            return r;
    return -1;
}

static void checkFormat()
{
    const struct { qint64 ms; const char* text; } table[] = {
        {0, "+0 ms"},
        {7, "+7 ms"},
        {999, "+999 ms"},
        {1000, "+1.000 s"},
        {12345, "+12.345 s"},
        {59999, "+59.999 s"},
        {60000, "+1 m 00 s"},
        {123456, "+2 m 03 s"},
        {3599999, "+59 m 59 s"},
        {3600000, "+1 h 00 m"},
        {5 * 3600000LL + 7 * 60000 + 59999, "+5 h 07 m"},
        {86400000LL, "+1 d 00 h"},
        {12 * 86400000LL + 5 * 3600000LL, "+12 d 05 h"},
        {999 * 86400000LL + 23 * 3600000LL, "+999 d 23 h"},
        {1000 * 86400000LL, "+1000 d"},
    };
    for (const auto& t : table) {
        CHECK(TimeDelta::format(t.ms) == QString::fromUtf8(t.text),
              QStringLiteral("format %1 = '%2' (got '%3')").arg(t.ms).arg(t.text, TimeDelta::format(t.ms)));
        const QString neg = QString(QChar(0x2212)) + QString::fromUtf8(t.text).mid(1);
        if (t.ms > 0)
            CHECK(TimeDelta::format(-t.ms) == neg, QStringLiteral("format -%1 = '%2' (got '%3')")
                                                         .arg(t.ms).arg(neg, TimeDelta::format(-t.ms)));
    }
    // Годы 0001–9999: разность моментов — без переполнения, ширина в пределе.
    const qint64 y1 = QDateTime(QDate(1, 1, 1), QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    const qint64 y9999 = QDateTime(QDate(9999, 12, 31), QTime(23, 59, 59, 999), QTimeZone::UTC).toMSecsSinceEpoch();
    const QString wide = TimeDelta::format(y9999 - y1);
    const QString wideNeg = TimeDelta::format(y1 - y9999);
    CHECK(wide == QStringLiteral("+3652058 d") && wideNeg == QString(QChar(0x2212)) + QStringLiteral("3652058 d"),
          "format: years 0001–9999 do not overflow: " + wide + " / " + wideNeg);
    int longest = 0;
    for (qint64 ms = 1; ms < (y9999 - y1); ms = ms * 3 + 7)
        longest = qMax(longest, int(qMax(TimeDelta::format(ms).size(), TimeDelta::format(-ms).size())));
    CHECK(longest <= TimeDelta::kMaxChars, QStringLiteral("format: no longer than %1 chars (%2)")
                                              .arg(TimeDelta::kMaxChars).arg(longest));
    CHECK(TimeDelta::format((std::numeric_limits<qint64>::min)() + 1).startsWith(QChar(0x2212)),
          "format: the extreme value keeps its sign");
    CHECK(TimeDelta::formatDetailed(123456) == QStringLiteral("+2 m 03 s (123456 ms)")
              && TimeDelta::formatDetailed(-250) == QString(QChar(0x2212)) + QStringLiteral("250 ms"),
          "formatDetailed: exact ms from a minute on");
}

static void checkTab(const QDir& dir, bool indexed)
{
    const QString label = indexed ? QStringLiteral("indexed") : QStringLiteral("resident");
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(indexed ? 0 : 512);
    settings.setTimeDelta(true);
    const QString a = testdocs::writeFile(dir, QStringLiteral("a-%1.log").arg(label), fileA());
    LogViewWidget tab;
    tab.setParserPattern(testdocs::buildSchema());
    tab.resize(900, 500);
    tab.show();
    tab.addLogFile(a);
    CHECK(settled(tab), label + ": loads");
    LogModel& model = *tab.model();
    LogListView& view = *tab.view();
    CHECK(model.isIndexedBackend() == indexed, label + ": backend");
    CHECK(model.rowCount() == 12, QStringLiteral("%1: 12 lines (%2)").arg(label).arg(model.rowCount()));

    checkAgainstBruteForce(tab, label + ", one file");
    // Многострочная запись: дельта у первой строки, у продолжений — пусто.
    const int epoch = rowWithText(model, QStringLiteral("at the epoch"));
    const int cont = rowWithText(model, QStringLiteral("continuation one"));
    const int before = rowWithText(model, QStringLiteral("before the epoch"));
    CHECK(epoch >= 0 && cont == epoch + 1 && view.timeDeltaText(cont).isEmpty()
              && view.timeDeltaText(epoch) == TimeDelta::format(
                     QDateTime(QDate(1970, 1, 1), QTime(0, 0)).toMSecsSinceEpoch()
                     - QDateTime(QDate(1965, 6, 1), QTime(12, 0)).toMSecsSinceEpoch())
              && before == epoch - 1,
          label + ": the epoch line counts from the 1965 line, its continuation is blank: "
              + view.timeDeltaText(epoch));
    // Строка продолжения: в Entry Details дельты записи.
    const TimeDelta::RowDelta contDelta = TimeDelta::forRow(model, cont);
    CHECK(contDelta.hasPrevious && !contDelta.firstOfRecord && contDelta.previousRow == before,
          label + ": a continuation line has its record's delta for Entry Details");
    // Строки без метки — пусто.
    const int preamble = rowWithText(model, QStringLiteral("preamble without"));
    CHECK(preamble >= 0 && view.timeDeltaText(preamble).isEmpty()
              && !TimeDelta::forRow(model, preamble).hasTimestamp,
          label + ": a line without a timestamp has no delta");
    if (indexed) {
        // Одно-файловая индексная не сортирует: дельта назад — со знаком.
        const int earlier = rowWithText(model, QStringLiteral("an hour earlier"));
        CHECK(view.timeDeltaText(earlier) == QString(QChar(0x2212)) + QStringLiteral("1 h 01 m"),
              label + ": a line out of order has a negative delta: " + view.timeDeltaText(earlier));
    }

    // Предел прохода назад: упёрлись — «предыдущей нет».
    {
        // Вторая строка продолжения: две строки выше — своя запись.
        const int two = rowWithText(model, QStringLiteral("continuation two"));
        bool exhausted = false;
        const int prev = TimeDelta::previousTimedRow(model, two, 2, &exhausted);
        CHECK(two == epoch + 2 && prev == -1 && exhausted
                  && TimeDelta::previousTimedRow(model, two, 3) == before,
              label + ": the back scan stops at its limit");
    }

    // Фильтры уровней и текста.
    model.setLogLevelFilter({LogLevel::Error, LogLevel::Warn});
    CHECK(settled(tab), label + ": level filter");
    checkAgainstBruteForce(tab, label + ", levels");
    FilterRuleSet rules;
    FilterRule needle;
    needle.action = FilterRule::Action::Include;
    needle.text = QStringLiteral("needle");
    rules.rules << needle;
    rules.bindFields(model.availableFields(), false);
    model.setLogLevelFilter({});
    model.setFilterRules(rules);
    CHECK(settled(tab), label + ": text filter");
    checkAgainstBruteForce(tab, label + ", text");
    model.setFilterRules(FilterRuleSet());
    CHECK(settled(tab), label + ": filters off");

    // «Нулевая».
    const int err = rowWithText(model, QStringLiteral("first error"));
    CHECK(!model.setTimeZero(preamble) && !model.hasTimeZero(), label + ": a line without time is no zero");
    CHECK(model.toggleTimeZero(err) && model.isTimeZeroRow(err) && view.timeDeltaText(err) == QStringLiteral("= 0"),
          label + ": the zero is set");
    checkAgainstBruteForce(tab, label + ", zero");
    const int minute = rowWithText(model, QStringLiteral("a minute later"));
    CHECK(view.timeDeltaText(minute) == QStringLiteral("+1 m 05 s"), label + ": delta from the zero: " + view.timeDeltaText(minute));
    {
        EntryDetailsPanel panel;
        panel.showEntry(model.entryAt(minute), model.logicalRecordLines(model.entryAt(minute)), {});
        panel.setTimeDelta(TimeDelta::forRow(model, minute));
        const QString s = panel.timeDeltaSummary();
        CHECK(s.contains(QStringLiteral("Δ previous: +1 m 04 s (64750 ms)"))
                  && s.contains(QStringLiteral("Δ time zero: +1 m 05 s (65000 ms)")),
              label + ": Entry Details shows both deltas: " + s);
        panel.setTimeDelta(TimeDelta::forRow(model, err));
        CHECK(panel.timeDeltaSummary().contains(QStringLiteral("time zero")), label + ": Entry Details on the zero");
    }
    // Скрыта фильтром — дельты от её момента, отметки нет.
    model.setLogLevelFilter({LogLevel::Info});
    CHECK(settled(tab), label + ": zero hidden");
    bool anyZero = false;
    for (int r = 0; r < model.rowCount(); ++r)
        anyZero = anyZero || model.isTimeZeroRow(r);
    CHECK(!anyZero && model.hasTimeZero(), label + ": a hidden zero is not shown but still set");
    checkAgainstBruteForce(tab, label + ", hidden zero");
    CHECK(view.timeDeltaText(rowWithText(model, QStringLiteral("a minute later"))) == QStringLiteral("+1 m 05 s"),
          label + ": deltas from a hidden zero");
    model.setLogLevelFilter({});
    CHECK(settled(tab), label + ": filter off again");
    CHECK(model.isTimeZeroRow(rowWithText(model, QStringLiteral("first error"))), label + ": the zero shows again");

    // Дозапись.
    CHECK(testdocs::appendToFile(a, line("2026-03-05 10:00:01.500", "INFO", "appended")), label + ": append");
    tab.reloadChangedFiles(true);
    CHECK(settled(tab) && model.rowCount() == 13, label + ": appended");
    CHECK(model.isTimeZeroRow(rowWithText(model, QStringLiteral("first error"))), label + ": the zero survives an append");
    CHECK(view.timeDeltaText(rowWithText(model, QStringLiteral("appended"))) == QStringLiteral("+1.500 s"),
          label + ": the appended line counts from the zero");
    checkAgainstBruteForce(tab, label + ", appended");

    // Полная перезагрузка: переписано начало той же длины.
    {
        QByteArray bytes = fileA() + line("2026-03-05 10:00:01.500", "INFO", "appended");
        bytes[0] = 'P';
        QFile f(a);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), label + ": rewrite");
        f.close();
        tab.reloadChangedFiles(true);
        CHECK(settled(tab) && rowWithText(model, QStringLiteral("Preamble")) >= 0, label + ": full reload");
        CHECK(model.isTimeZeroRow(rowWithText(model, QStringLiteral("first error"))),
              label + ": the zero survives a full reload");
        checkAgainstBruteForce(tab, label + ", reloaded");
    }

    // Переписан с другой меткой на той же строке: «нулевая» задана (дельты
    // от прежнего момента), но эта строка «= 0» не показывает.
    {
        QByteArray bytes = fileA() + line("2026-03-05 10:00:01.500", "INFO", "appended");
        bytes[0] = 'P';
        bytes.replace("2026-03-05 10:00:00.000 [w] ERROR", "2026-03-05 11:00:00.000 [w] ERROR");
        QFile f(a);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), label + ": rewrite the zero's time");
        f.close();
        tab.reloadChangedFiles(true);
        CHECK(settled(tab), label + ": reload with another time");
        const int moved = rowWithText(model, QStringLiteral("first error"));
        CHECK(model.hasTimeZero() && !model.isTimeZeroRow(moved)
                  && view.timeDeltaText(moved) == QStringLiteral("+1 h 00 m"),
              label + ": a line with another moment is not the zero: " + view.timeDeltaText(moved));
        bytes.replace("2026-03-05 11:00:00.000 [w] ERROR", "2026-03-05 10:00:00.000 [w] ERROR");
        CHECK(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), label + ": rewrite back");
        f.close();
        tab.reloadChangedFiles(true);
        CHECK(settled(tab) && model.isTimeZeroRow(rowWithText(model, QStringLiteral("first error"))),
              label + ": and is the zero again when the moment is back");
    }

    // Повтор снимает.
    const int errNow = rowWithText(model, QStringLiteral("first error"));
    CHECK(model.toggleTimeZero(errNow) && !model.hasTimeZero(), label + ": toggling again clears the zero");
    checkAgainstBruteForce(tab, label + ", cleared");

    // Слитая вкладка.
    const QString b = testdocs::writeFile(dir, QStringLiteral("b-%1.log").arg(label), fileB());
    tab.addLogFile(b);
    CHECK(settled(tab) && model.rowCount() == 17, QStringLiteral("%1: merged (%2)").arg(label).arg(model.rowCount()));
    checkAgainstBruteForce(tab, label + ", merged");
    model.setLogLevelFilter({LogLevel::Error, LogLevel::Warn});
    CHECK(settled(tab), label + ": merged, levels");
    checkAgainstBruteForce(tab, label + ", merged levels");
    model.setLogLevelFilter({});
    CHECK(settled(tab), label + ": merged, filter off");
    const int bFirst = rowWithText(model, QStringLiteral("b first"));
    CHECK(model.setTimeZero(bFirst), label + ": a zero in the second file");
    checkAgainstBruteForce(tab, label + ", merged zero");
    CHECK(view.timeDeltaText(rowWithText(model, QStringLiteral("first error"))) == QString(QChar(0x2212)) + QStringLiteral("100 ms"),
          label + ": a line before the zero has a negative delta");
    model.clearTimeZero();
    settings.setIndexedThresholdMB(512);
}

// Колонка: ширина постоянная, место выбранной строки, ini.
static void checkColumn(const QDir& dir, bool indexed)
{
    const QString label = indexed ? QStringLiteral("column, indexed") : QStringLiteral("column, resident");
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(indexed ? 0 : 512);
    settings.setTimeDelta(false);
    QByteArray bytes;
    for (int i = 0; i < 3000; ++i)
        bytes += QStringLiteral("2026-03-05 %1:%2:%3.%4 [w] INFO - line %5\n")
                     .arg(10 + i / 3600, 2, 10, QLatin1Char('0')).arg(i / 60 % 60, 2, 10, QLatin1Char('0'))
                     .arg(i % 60, 2, 10, QLatin1Char('0')).arg(i % 1000, 3, 10, QLatin1Char('0')).arg(i).toUtf8();
    const QString path = testdocs::writeFile(dir, QStringLiteral("column-%1.log").arg(indexed), bytes);
    LogViewWidget tab;
    tab.setParserPattern(testdocs::buildSchema());
    tab.resize(900, 500);
    tab.show();
    tab.addLogFile(path);
    CHECK(settled(tab), label + ": loads");
    LogListView& view = *tab.view();
    LogModel& model = *tab.model();
    CHECK(view.timeDeltaWidth() == 0 && view.timeDeltaText(5) == QStringLiteral("+1.001 s"),
          label + ": no column while off (the text is still there)");
    const int leftOff = view.textLeft();
    view.goToRow(1500);
    CHECK(testdocs::settleViews(model), label + ": go to");
    const int topBefore = view.visualRect(model.index(1500, 0)).top();

    settings.setTimeDelta(true);
    CHECK(testdocs::settleViews(model), label + ": on");
    const int width = view.timeDeltaWidth();
    const qreal cw = QFontMetricsF(view.font()).horizontalAdvance(QLatin1Char('M'));
    CHECK(width >= int(cw * TimeDelta::kMaxChars) && view.textLeft() == leftOff + width,
          QStringLiteral("%1: the column appears, the text moves by its width (%2)").arg(label).arg(width));
    CHECK(view.userSelectedRow() == 1500 && view.visualRect(model.index(1500, 0)).top() == topBefore,
          QStringLiteral("%1: the selected line keeps its place (%2 vs %3)").arg(label)
              .arg(view.visualRect(model.index(1500, 0)).top()).arg(topBefore));
    // Щелчок по колонке выбирает строку; подсказка — от предыдущей.
    const QRect cell = view.timeDeltaRect(1490);
    const LogListView::RowChromeHit hit = view.rowChromeAt(cell.center());
    CHECK(hit.kind == LogListView::RowChrome::TimeDelta && hit.row == 1490
              && hit.toolTip.contains(QStringLiteral("previous")),
          label + ": the delta cell is row chrome with a tooltip: " + hit.toolTip);
    // Ширина не зависит от строк: дозапись, длинные дельты.
    const int relayouts = view.leftColumnRelayouts();
    testdocs::appendToFile(path, "2030-01-01 00:00:00.000 [w] INFO - years later\n");
    tab.reloadChangedFiles(true);
    CHECK(settled(tab) && view.timeDeltaWidth() == width && view.leftColumnRelayouts() == relayouts,
          label + ": the width does not change with the rows");
    CHECK(view.timeDeltaText(model.rowCount() - 1) == QStringLiteral("+1397 d"),
          label + ": a long delta fits: " + view.timeDeltaText(model.rowCount() - 1));

    settings.setTimeDelta(false);
    CHECK(testdocs::settleViews(model), label + ": off");
    CHECK(view.timeDeltaWidth() == 0 && view.textLeft() == leftOff
              && view.visualRect(model.index(1500, 0)).top() == topBefore,
          label + ": off — the text and the selected line return");
    settings.setTimeDelta(true);
    settings.save();
    {
        QSettings ini(AppSettings::iniFilePath(), QSettings::IniFormat);
        CHECK(ini.value(QStringLiteral("View/timeDelta")).toBool(), label + ": stored in [View] timeDelta");
    }
    settings.setTimeDelta(false);
    settings.save();
    settings.setIndexedThresholdMB(512);
}

// Переход порога: резидентная вкладка становится индексной, «нулевая» та же.
static void checkThreshold(const QDir& dir)
{
    AppSettings& settings = AppSettings::instance();
    settings.setIndexedThresholdMB(1);
    QByteArray start;
    for (int i = 0; i < 2000; ++i)
        start += QStringLiteral("2026-03-05 10:%1:%2.000 [w] INFO - line %3\n")
                     .arg(i / 60 % 60, 2, 10, QLatin1Char('0')).arg(i % 60, 2, 10, QLatin1Char('0')).arg(i).toUtf8();
    const QString path = testdocs::writeFile(dir, QStringLiteral("grows.log"), start);
    LogViewWidget tab;
    tab.setParserPattern(testdocs::buildSchema());
    tab.addLogFile(path);
    CHECK(settled(tab) && !tab.model()->isIndexedBackend(), "threshold: resident load");
    CHECK(tab.model()->setTimeZero(100), "threshold: zero on line 101");
    const qint64 zeroMs = tab.model()->timeZero().ms;
    QByteArray more;
    for (int i = 2000; i < 40000; ++i)
        more += QStringLiteral("2026-03-05 %1:%2:%3.000 [w] INFO - line %4\n")
                    .arg(10 + i / 3600, 2, 10, QLatin1Char('0')).arg(i / 60 % 60, 2, 10, QLatin1Char('0'))
                    .arg(i % 60, 2, 10, QLatin1Char('0')).arg(i).toUtf8();
    CHECK(testdocs::appendToFile(path, more), "threshold: append past the threshold");
    tab.reloadChangedFiles(true);
    CHECK(settled(tab) && tab.model()->isIndexedBackend(), "threshold: indexed now");
    CHECK(tab.model()->hasTimeZero() && tab.model()->timeZero().ms == zeroMs && tab.model()->isTimeZeroRow(100),
          "threshold: the zero survives the conversion");
    const TimeDelta::RowDelta d = TimeDelta::forRow(*tab.model(), 160);
    CHECK(d.hasZero && d.fromZero == 60000, "threshold: deltas from the zero after the conversion");
    settings.setIndexedThresholdMB(512);
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    AppSettings::instance().load();
    CHECK(!AppSettings::instance().timeDelta(), "the column is off by default");
    QTemporaryDir tmp;
    CHECK(tmp.isValid(), "temporary directory");
    const QDir dir(tmp.path());

    checkFormat();
    checkTab(dir, false);
    checkTab(dir, true);
    checkColumn(dir, false);
    checkColumn(dir, true);
    checkThreshold(dir);

    if (failures) {
        std::fprintf(stderr, "time_delta: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "time_delta: OK\n");
    return 0;
}
