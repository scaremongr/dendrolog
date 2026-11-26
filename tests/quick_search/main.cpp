// ============================================================================
// quick_search — фоновый быстрый поиск (F3 / Shift+F3):
//   • скан по снапшоту даёт ту же строку, что синхронный LogModel::findNext/
//     PreviousOccurrence, на обоих бэкендах, при любом старте, в обе стороны,
//     с заворачиванием и без;
//   • найденная строка пересчитывается через вставки, случившиеся за время
//     скана; reset/удаление строк — invalidated() вместо устаревшего номера;
//   • отменённый поиск ничего не присылает;
//   • запрос с флагами (регистр, слово целиком, регекс) — синхронный поиск,
//     фоновый скан и подсветка найденного против независимого перебора на
//     обоих бэкендах; невалидный регекс — ошибка с позицией и -1 у поиска;
//   • история запросов: свежий первым, без дублей, с пределом длины;
//   • строка старта (startRowFor): выбранная строка, а если её скрыл фильтр —
//     её место среди видимых строк, сверенное с перебором и с синхронным
//     поиском по полной модели.
// ============================================================================

#include "quicksearch.h"
#include "quicksearchhistory.h"
#include "filterruleset.h"
#include "testdocuments.h"
#include "userregex.h"

#include <QElapsedTimer>
#include <QHash>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cstdio>

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

// 3000 строк, NEEDLE (в разном регистре) на нескольких строках.
static QByteArray fixture(int firstSecond)
{
    QByteArray out;
    for (int i = 0; i < 3000; ++i) {
        QByteArray text = "ordinary " + QByteArray::number(i);
        if (i % 700 == 13)
            text = "NEEDLE " + QByteArray::number(i);
        else if (i % 900 == 5)
            text = "needle lower " + QByteArray::number(i);
        out += line(firstSecond + i / 20, i % 3 ? "INFO" : "ERROR", text);
    }
    return out;
}

// Скан снапшота обязан совпасть с синхронным поиском модели во всех режимах.
static void compareWithModel(const QString& name, LogModel& model)
{
    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    const int n = model.rowCount();
    CHECK(snapshot.rowCount() == n, name + ": snapshot covers the visible rows");
    const QVector<int> starts = {-1, 0, 1, n / 2, n - 2, n - 1, n, n + 5};
    const QStringList texts = {QStringLiteral("NEEDLE"), QStringLiteral("absent text"),
                               QStringLiteral("ordinary 2999")};
    int mismatches = 0;
    for (const QString& text : texts) {
        for (const Qt::CaseSensitivity cs : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
            for (const bool wrap : {true, false}) {
                for (const int start : starts) {
                    const QModelIndex next = model.findNextOccurrence(text, start, cs, wrap);
                    const qint64 scanNext = QuickSearch::scan(snapshot, TextQuery(text, cs), true, start, wrap);
                    const QModelIndex prev = model.findPreviousOccurrence(text, start, cs, wrap);
                    const qint64 scanPrev = QuickSearch::scan(snapshot, TextQuery(text, cs), false, start, wrap);
                    if (scanNext != (next.isValid() ? next.row() : -1)
                        || scanPrev != (prev.isValid() ? prev.row() : -1)) {
                        if (++mismatches <= 5)
                            std::fprintf(stderr, "  %s '%s' cs=%d wrap=%d start=%d: next %lld/%d prev %lld/%d\n",
                                         qPrintable(name), qPrintable(text), int(cs), int(wrap), start,
                                         (long long)scanNext, next.isValid() ? next.row() : -1,
                                         (long long)scanPrev, prev.isValid() ? prev.row() : -1);
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0, name + ": background scan matches the synchronous search");
}

// Строка старта против перебора. order — ключи всех строк модели БЕЗ фильтра
// (её порядок), модель сейчас отфильтрована. Для каждой строки-выбора место
// среди видимых считается перебором: p — число видимых строк, стоящих раньше;
// matches — есть ли в строке полной модели NEEDLE.
// Старт: строка видна — она сама; скрыта — вперёд p - 1 (первой проверяется
// строка p), назад p (первой проверяется p - 1), в том числе когда скрытая
// запись стоит после ВСЕХ видимых строк (p == n: вперёд — сразу через край).
static void checkStartRows(const QString& name, LogModel& model,
                           const QVector<LogModel::EntryKey>& order,
                           const QVector<bool>& matches)
{
    const int n = model.rowCount();
    QHash<QPair<int, const LogFile*>, int> visibleRow;
    for (int row = 0; row < n; ++row) {
        const LogModel::EntryKey key = model.keyForRow(row);
        visibleRow.insert({key.logicalEntryId, key.sourceFile}, row);
    }
    CHECK(visibleRow.size() == n && n > 0 && n < order.size(), name + ": fixture rows are unique records");
    const int total = order.size();
    QVector<int> rowOf(total, -1); // позиция полной модели → видимая строка
    for (int i = 0; i < total; ++i)
        rowOf[i] = visibleRow.value({order[i].logicalEntryId, order[i].sourceFile}, -1);
    // Первое видимое совпадение от позиции i полной модели, не включая её.
    const auto expected = [&](int i, int step) {
        for (int k = 1; k <= total; ++k) {
            const int j = ((i + step * k) % total + total) % total;
            if (rowOf[j] >= 0 && matches[j])
                return rowOf[j];
        }
        return -1;
    };

    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    const QString needle = QStringLiteral("NEEDLE");
    int mismatches = 0, hiddenChecked = 0, beyondLast = 0, searchMismatches = 0;
    int p = 0; // видимых строк раньше позиции i
    for (int i = 0; i < order.size(); ++i) {
        const LogModel::EntryKey key = order[i];
        const int row = rowOf[i];
        int wantForward, wantBackward;
        if (row >= 0) {
            wantForward = wantBackward = row;
        } else {
            wantForward = p - 1;
            wantBackward = p;
            ++hiddenChecked;
            if (p == n)
                ++beyondLast;
        }
        // Выбранная строка скрыта: view отдаёт -1 и якорь-запись.
        const int forward = QuickSearch::startRowFor(model, -1, key.logicalEntryId, key.sourceFile, true);
        const int backward = QuickSearch::startRowFor(model, -1, key.logicalEntryId, key.sourceFile, false);
        if (forward != wantForward || backward != wantBackward) {
            if (++mismatches <= 5)
                std::fprintf(stderr, "  %s origin %d: forward %d/%d backward %d/%d\n", qPrintable(name),
                             i, forward, wantForward, backward, wantBackward);
        }
        // Поиск от скрытой строки находит ближайшее видимое совпадение в
        // порядке ПОЛНОЙ модели — то, к которому пользователь пришёл бы от
        // выбранной строки, пролистав скрытые.
        // (Каждая 17-я скрытая строка и все за последней видимой: скан читает
        // текст, а под санитайзером это дорого.)
        if (row < 0 && (p == n || hiddenChecked % 17 == 0)) {
            const qint64 next = QuickSearch::scan(snapshot, TextQuery(needle, Qt::CaseSensitive), true, forward, true);
            const qint64 previous = QuickSearch::scan(snapshot, TextQuery(needle, Qt::CaseSensitive), false, backward, true);
            if (next != expected(i, 1) || previous != expected(i, -1))
                ++searchMismatches;
        }
        if (row >= 0)
            ++p;
    }
    CHECK(mismatches == 0, name + ": start row matches the brute-force position");
    CHECK(searchMismatches == 0,
          name + ": search from a hidden row finds the nearest visible match in tab order");
    CHECK(hiddenChecked > 0 && beyondLast > 0, name + ": hidden rows inside and past the visible ones are covered");

    // Выбранная видимая строка важнее якоря; без выбора — с начала или с конца.
    CHECK(QuickSearch::startRowFor(model, 7, order[0].logicalEntryId, order[0].sourceFile, true) == 7
              && QuickSearch::startRowFor(model, 7, -1, nullptr, false) == 7,
          name + ": the selected row wins");
    CHECK(QuickSearch::startRowFor(model, -1, -1, nullptr, true) == -1
              && QuickSearch::startRowFor(model, -1, -1, nullptr, false) == n,
          name + ": no selection starts from the top or from the end");
    CHECK(QuickSearch::startRowFor(model, n + 3, -1, nullptr, true) == -1,
          name + ": a row past the end is not a selection");
    CHECK(QuickSearch::startRowFor(model, -1, 1 << 30, order[0].sourceFile, true) == -1
              && QuickSearch::startRowFor(model, -1, 1 << 30, order[0].sourceFile, false) == n,
          name + ": a record that is gone is not a selection");
}

// ---------------------------------------------------------------------------
// Запрос с флагами. Эталон написан независимо от TextQuery: подстрока —
// перебор indexOf с проверкой соседей вручную, регекс — QRegularExpression
// по тексту запроса с тем же перебором соседей.

static QByteArray flagsFixture(bool cyrillicOnly = false)
{
    // Строки, на которых «слово целиком», регистр и регекс расходятся.
    // cyrillicOnly — без латиницы с диакритикой: такой текст представим в
    // Windows-1251, IBM866 и KOI8-R (F-1).
    static const char* texts[] = {
        "plain error here", "errors 7 counted", "[error] bracketed", "my_error underscore",
        "ERROR-1 upper dash", "xerror glued", "Ошибка error cyrillic", "ошибкаerror glued",
        "value a.b.c dotted", "call x(y) parens", "abc code 123", "code123 glued",
        "456 leading", "Error capital", "nothing here", "tab\terror\tsep", "érror accent",
        "errorй cyr after", "ОШИБКА диска", "Ёжик ошибкой назван", "предошибка склеена",
        "ошибка, запятая", // число строк не кратно 7 — шаг (i * 7) обходит все
    };
    QByteArray out;
    const int kinds = int(sizeof(texts) / sizeof(texts[0]));
    for (int i = 0; i < 400; ++i) {
        QByteArray text(texts[(i * 7) % kinds]);
        if (cyrillicOnly && text.startsWith("\xC3\xA9")) // «é»
            text = "ёrror accent";
        out += line(i / 4, i % 2 ? "INFO" : "ERROR", text + " #" + QByteArray::number(i));
    }
    return out;
}

static bool isWordChar(QChar c) { return c.isLetterOrNumber() || c.isMark() || c == u'_'; }

static bool boundaryOk(const QString& text, qsizetype start, qsizetype length)
{
    const bool before = start > 0 && isWordChar(text.at(start - 1));
    const bool after = start + length < text.size() && isWordChar(text.at(start + length));
    return !before && !after;
}

static bool referenceMatches(const TextQuery& q, const QString& text)
{
    const Qt::CaseSensitivity cs = q.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!q.regex) {
        for (qsizetype from = 0;;) {
            const qsizetype pos = text.indexOf(q.text, from, cs);
            if (pos < 0)
                return false;
            if (!q.wholeWord || boundaryOk(text, pos, q.text.size()))
                return true;
            from = pos + 1;
        }
    }
    const QRegularExpression re(q.text, q.caseSensitive ? QRegularExpression::NoPatternOption
                                                        : QRegularExpression::CaseInsensitiveOption);
    if (!q.wholeWord) {
        // Пустое совпадение не находка (показать нечего).
        for (auto it = re.globalMatch(text); it.hasNext();)
            if (it.next().capturedLength() > 0)
                return true;
        return false;
    }
    // Слово целиком: годится любое совпадение (с любой позиции) с чистыми
    // соседями — перебор стартов, якорь на старте через AnchorAtOffset.
    for (qsizetype start = 0; start <= text.size(); ++start) {
        const QRegularExpressionMatch m = re.match(text, start, QRegularExpression::NormalMatch,
                                                   QRegularExpression::AnchorAtOffsetMatchOption);
        if (m.hasMatch() && m.capturedLength() > 0 && boundaryOk(text, start, m.capturedLength()))
            return true;
    }
    return false;
}

// Ближайшая подходящая строка по правилам обхода (scanRowsFrom), перебором.
static int referenceFind(const QVector<bool>& hit, int start, bool forward, bool wrap)
{
    const int n = hit.size();
    if (forward) {
        for (int i = qBound(0, start + 1, n); i < n; ++i)
            if (hit[i]) return i;
        if (wrap)
            for (int i = 0; i <= start && i < n; ++i)
                if (hit[i]) return i;
    } else {
        const bool inside = start >= 0 && start < n;
        for (int i = inside ? start - 1 : n - 1; i >= 0; --i)
            if (hit[i]) return i;
        if (wrap && inside)
            for (int i = n - 1; i >= start; --i)
                if (hit[i]) return i;
    }
    return -1;
}

static QVector<TextQuery> flagQueries()
{
    QVector<TextQuery> out;
    const auto add = [&](const char* text, bool cs, bool word, bool regex) {
        TextQuery q;
        q.text = QString::fromUtf8(text);
        q.caseSensitive = cs;
        q.wholeWord = word;
        q.regex = regex;
        out.append(q);
    };
    for (const bool cs : {false, true}) {
        for (const bool word : {false, true}) {
            add("error", cs, word, false);
            add("[error]", cs, word, false);   // литерал со скобками: не класс символов
            add("a.b", cs, word, false);       // точка — литерал
            add("x(y)", cs, word, false);
            add("err.r", cs, word, true);
            add("^(plain|456)", cs, word, true);
            add("\\d{3}", cs, word, true);
            add("ERROR-\\d", cs, word, true);
            add("absent", cs, word, false);
            // Кириллица (F-1: те же запросы на индексной вкладке в cp1251,
            // cp866 и KOI8-R): регистр сворачивается по Unicode.
            add("ошибка", cs, word, false);
            add("ош[а-яё]+а", cs, word, true);
        }
    }
    return out;
}

static void checkFlags(const QString& name, LogModel& model)
{
    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    const int n = model.rowCount();
    const QVector<int> starts = {-1, 0, 1, n / 3, n - 1, n};
    const QColor color(Qt::yellow);
    int mismatches = 0, highlightMismatches = 0, matchedQueries = 0;
    for (const TextQuery& q : flagQueries()) {
        CHECK(q.isValid(), name + ": query is valid: " + q.text);
        const TextMatcher matcher(q);
        TextMatchHighlighter highlighter;
        highlighter.setPatterns({q.highlightPattern(color)});
        // Find All: правило панели Search из запроса находит те же строки.
        FilterRuleSet rules;
        FilterRule rule;
        rule.text = q.patternText();
        rule.isRegex = q.needsRegex();
        rule.caseSensitive = q.caseSensitive;
        rules.rules.append(rule);
        rules.bindFields({}, false);
        QVector<bool> hit(n);
        int hits = 0;
        for (int row = 0; row < n; ++row) {
            const QString text = model.messageAt(row);
            hit[row] = referenceMatches(q, text);
            hits += hit[row];
            if (matcher.matches(text) != hit[row] && ++mismatches <= 5)
                std::fprintf(stderr, "  %s matcher '%s' cs=%d word=%d re=%d on '%s'\n", qPrintable(name),
                             qPrintable(q.text), q.caseSensitive, q.wholeWord, q.regex, qPrintable(text));
            if (rules.matchesLine(text, LogEntryFields()) != hit[row] && ++mismatches <= 5)
                std::fprintf(stderr, "  %s Find All rule '%s' word=%d re=%d on '%s'\n", qPrintable(name),
                             qPrintable(q.text), q.wholeWord, q.regex, qPrintable(text));
            // Подсветка: что-то залито ровно на найденных строках, и залитое —
            // само совпадение с чистыми соседями.
            const QVector<HighlightSpan> spans = highlighter.computeSpans(text);
            bool spansOk = spans.isEmpty() != hit[row];
            for (const HighlightSpan& span : spans) {
                if (q.wholeWord && !boundaryOk(text, span.start, span.length))
                    spansOk = false;
                if (!q.regex && text.mid(span.start, span.length).compare(q.text, q.caseSensitivity()) != 0)
                    spansOk = false;
            }
            if (!spansOk && ++highlightMismatches <= 5)
                std::fprintf(stderr, "  %s highlight '%s' word=%d re=%d on '%s'\n", qPrintable(name),
                             qPrintable(q.text), q.wholeWord, q.regex, qPrintable(text));
        }
        matchedQueries += hits > 0 && hits < n;
        for (const bool wrap : {true, false}) {
            for (const int start : starts) {
                for (const bool forward : {true, false}) {
                    const int want = referenceFind(hit, start, forward, wrap);
                    const QModelIndex sync = model.findOccurrence(q, start, forward, wrap);
                    const qint64 scanned = QuickSearch::scan(snapshot, q, forward, start, wrap);
                    if ((sync.isValid() ? sync.row() : -1) != want || scanned != want) {
                        if (++mismatches <= 5)
                            std::fprintf(stderr, "  %s '%s' cs=%d word=%d re=%d start=%d fwd=%d wrap=%d: "
                                         "sync %d scan %lld want %d\n", qPrintable(name), qPrintable(q.text),
                                         q.caseSensitive, q.wholeWord, q.regex, start, forward, wrap,
                                         sync.isValid() ? sync.row() : -1, (long long)scanned, want);
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0, name + ": flagged queries match the brute-force reference");
    CHECK(highlightMismatches == 0, name + ": highlight marks exactly what was found");
    CHECK(matchedQueries >= 16, name + QStringLiteral(": fixture discriminates the queries (%1)").arg(matchedQueries));
}

static void checkQueryValidity()
{
    TextQuery q;
    q.text = QStringLiteral("a(b");
    CHECK(q.isValid(), "unbalanced parenthesis is a literal without regex");
    q.wholeWord = true;
    CHECK(q.isValid(), "... and with whole word (escaped)");
    q.regex = true;
    QString error;
    int offset = -2;
    CHECK(!q.isValid(&error, &offset), "invalid regex is reported");
    const QRegularExpression direct(q.text);
    CHECK(offset == direct.patternErrorOffset() && offset >= 0 && !error.isEmpty(),
          "error offset is in the user's text");
    q.wholeWord = false;
    CHECK(!q.isValid(), "invalid regex without whole word too");
    CHECK(!TextMatcher(q).isUsable() && !TextMatcher(q).matches(QStringLiteral("a(b")),
          "invalid regex matches nothing");
    q.text = QStringLiteral("\\Qabc");    // годен сам, а обёртку «слово целиком» съедает \Q
    CHECK(q.isValid(), "\\Q alone is valid");
    q.wholeWord = true;
    error.clear();
    const bool wrappedValid = q.isValid(&error, &offset);
    CHECK(wrappedValid || (!error.isEmpty() && offset == -1), "wrapped failure has no position");
    CHECK(TextQuery().isValid() && !TextMatcher(TextQuery()).isUsable(), "empty query finds nothing");

    // Пустое совпадение регекса — не находка: подсветить было бы нечего.
    TextQuery empty;
    empty.text = QStringLiteral("x*");
    empty.regex = true;
    const TextMatcher emptyMatcher(empty);
    CHECK(!emptyMatcher.matches(QStringLiteral("abc")) && emptyMatcher.matches(QStringLiteral("abxc")),
          "an empty regex match is not a hit, a later non-empty one is");
    empty.text = QStringLiteral("^");
    CHECK(!TextMatcher(empty).matches(QStringLiteral("abc")), "an anchor alone finds nothing");
    TextQuery digits;
    digits.text = QStringLiteral("\\d*");
    digits.regex = true;
    digits.wholeWord = true;
    CHECK(!TextMatcher(digits).matches(QStringLiteral("[error]"))
              && TextMatcher(digits).matches(QStringLiteral("code 42 here")),
          "whole word with an empty-capable regex needs a real word");

    TextQuery word(QStringLiteral("error"), Qt::CaseInsensitive);
    word.wholeWord = true;
    CHECK(word.needsRegex() && word.patternText().contains(QLatin1String("error")),
          "whole word becomes a regex around the text");
    TextQuery plain(QStringLiteral("a.b"), Qt::CaseSensitive);
    CHECK(!plain.needsRegex() && plain.patternText() == QLatin1String("a.b"), "plain query stays a substring");
}

static void checkHistory()
{
    QuickSearchHistory h;
    CHECK(!h.add(QString()), "empty query is not remembered");
    CHECK(h.add("one") && h.add("two") && h.add("three"), "queries are added");
    CHECK(h.entries() == QStringList({"three", "two", "one"}), "newest first");
    CHECK(!h.add("three"), "repeating the newest changes nothing");
    CHECK(h.add("one") && h.entries() == QStringList({"one", "three", "two"}),
          "a repeated query moves to the top without a duplicate");
    CHECK(h.add("One") && h.entries().size() == 4, "case makes a different query");
    CHECK(!h.add(QString(QuickSearchHistory::kMaxEntryLength + 1, u'x')), "an overlong query is not remembered");
    CHECK(h.add(QString(QuickSearchHistory::kMaxEntryLength, u'x')), "a query at the limit is remembered");
    for (int i = 0; i < 50; ++i)
        h.add(QStringLiteral("q%1").arg(i));
    CHECK(h.entries().size() == QuickSearchHistory::kMaxEntries, "the list is capped");
    CHECK(h.entries().first() == QLatin1String("q49") && !h.entries().contains("one"),
          "the oldest queries go first");
    const QuickSearchHistory loaded(QStringList{"a", "", "b", "a", QString(5000, u'y'), "c"});
    CHECK(loaded.entries() == QStringList({"a", "b", "c"}), "loading drops empties, duplicates, overlong");
    QStringList many;
    for (int i = 0; i < 40; ++i)
        many << QStringLiteral("m%1").arg(i);
    CHECK(QuickSearchHistory(many).entries().size() == QuickSearchHistory::kMaxEntries,
          "loading keeps the cap");
    h.clear();
    CHECK(h.isEmpty(), "clear empties the history");
}

// Замер: подстрока против регекса на индексном бэкенде (текст с диска в
// воркере). quick_search --bench <file> [запрос]
static int bench(const QString& path, const QString& needle)
{
    const QString schema = testdocs::buildSchema();
    QElapsedTimer t;
    t.start();
    auto doc = testdocs::buildIndexed({path}, schema);
    std::printf("indexed %lld rows in %lld ms\n", (long long)doc.model->rowCount(), (long long)t.elapsed());
    const LogScanSnapshot snapshot = doc.model->scanSnapshot(true);
    struct Case { const char* name; bool cs, word, regex; QString text; };
    const QString escaped = QRegularExpression::escape(needle);
    const Case cases[] = {
        {"substring, ignore case", false, false, false, needle},
        {"substring, match case", true, false, false, needle},
        {"whole word", false, true, false, needle},
        {"regex (same literal)", false, false, true, escaped},
        {"regex \\d{4}-x\\w+", false, false, true, QStringLiteral("\\d{4}-x\\w+")},
        {"regex error.*timeout", false, false, true, QStringLiteral("error.*timeout_absent")},
    };
    for (const Case& c : cases) {
        TextQuery q;
        q.text = c.text; q.caseSensitive = c.cs; q.wholeWord = c.word; q.regex = c.regex;
        qint64 best = -1;
        qint64 found = 0;
        for (int run = 0; run < 3; ++run) {
            t.restart();
            found = QuickSearch::scan(snapshot, q, true, -1, false);
            const qint64 ms = t.elapsed();
            best = best < 0 ? ms : qMin(best, ms);
        }
        std::printf("%-28s found %lld  best of 3: %lld ms\n", c.name, (long long)found, (long long)best);
    }
    // Прежний путь скана (до S-8): forEachLine + contains — для сравнения
    // цены курсора scanRowsFrom.
    qint64 best = -1;
    for (int run = 0; run < 3; ++run) {
        t.restart();
        qint64 hit = -1;
        snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta&, QStringView text) {
            if (text.contains(needle, Qt::CaseInsensitive)) {
                hit = row;
                return false;
            }
            return true;
        });
        const qint64 ms = t.elapsed();
        best = best < 0 ? ms : qMin(best, ms);
    }
    std::printf("%-28s best of 3: %lld ms\n", "forEachLine, ignore case", (long long)best);

    // F-14: цена Юникод-классов (UseUnicodePropertiesOption) — полный проход
    // по снапшоту, совпадения считаются во всех строках; без опции и с ней.
    const QString patterns[] = {
        QStringLiteral("ERROR.*timeout"), QStringLiteral("\\w+"), QStringLiteral("\\d+"),
        QStringLiteral("lag=\\d+ms, peers=\\d"), QStringLiteral("\\bexpired\\b.*\\bu\\d+\\b"),
        QStringLiteral("absent_\\w+_text"), QStringLiteral("Ошибка \\w+"),
    };
    // Текст строк заранее в памяти — чистая цена движка, без чтения с диска.
    QStringList lines;
    snapshot.forEachLine(0, [&](qint64, const LogEntryMeta&, QStringView text) {
        lines.append(text.toString());
        return true;
    });
    for (const QString& pattern : patterns) {
        for (const bool unicode : { false, true }) {
            // Опции приложения (UserRegex), без Юникод-классов — как до F-14.
            QRegularExpression::PatternOptions options = UserRegex::options(Qt::CaseInsensitive);
            if (!unicode)
                options &= ~QRegularExpression::PatternOptions(QRegularExpression::UseUnicodePropertiesOption);
            QRegularExpression re(pattern, options);
            re.optimize();
            qint64 rawBest = -1;
            qint64 hits = 0;
            for (int run = 0; run < 3; ++run) {
                hits = 0;
                t.restart();
                snapshot.forEachLine(0, [&](qint64, const LogEntryMeta&, QStringView text) {
                    hits += re.matchView(text).hasMatch();
                    return true;
                });
                const qint64 ms = t.elapsed();
                rawBest = rawBest < 0 ? ms : qMin(rawBest, ms);
            }
            qint64 memBest = -1;
            for (int run = 0; run < 3; ++run) {
                qint64 memHits = 0;
                t.restart();
                for (const QString& text : std::as_const(lines))
                    memHits += re.matchView(text).hasMatch();
                const qint64 ms = t.elapsed();
                memBest = memBest < 0 ? ms : qMin(memBest, ms);
                if (memHits != hits)
                    std::printf("  (in-memory hits differ: %lld)\n", (long long)memHits);
            }
            std::printf("%-34s %-7s hits %8lld  indexed scan %5lld ms, in memory %5lld ms\n",
                        qPrintable(pattern), unicode ? "unicode" : "ascii", (long long)hits,
                        (long long)rawBest, (long long)memBest);
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 3 && QByteArray(argv[1]) == "--bench")
        return bench(QString::fromLocal8Bit(argv[2]),
                     argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QStringLiteral("absent_needle_text"));
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return 1;
    const QDir dir(temporary.path());
    const QString schema = testdocs::buildSchema();
    const QString a = testdocs::writeFile(dir, "a.log", fixture(0));
    const QString b = testdocs::writeFile(dir, "b.log", fixture(30));

    // --- Эквивалентность синхронному поиску -----------------------------------
    {
        auto resident = testdocs::buildResident({a}, schema);
        auto indexed = testdocs::buildIndexed({a}, schema);
        auto residentMerged = testdocs::buildResident({a, b}, schema);
        auto indexedMerged = testdocs::buildIndexed({a, b}, schema, testdocs::IndexedLoad::Concurrent);
        struct { const char* name; LogModel* model; } models[] = {
            {"resident", resident.model.get()},
            {"indexed", indexed.model.get()},
            {"resident merged", residentMerged.model.get()},
            {"indexed merged", indexedMerged.model.get()},
        };
        for (const auto& m : models) {
            compareWithModel(QString::fromLatin1(m.name), *m.model);
            // Порядок полной модели и строки с совпадением в ней — эталон для
            // строки старта под фильтром.
            QVector<LogModel::EntryKey> order;
            QVector<bool> matches;
            for (int row = 0; row < m.model->rowCount(); ++row) {
                order.append(m.model->keyForRow(row));
                matches.append(m.model->messageAt(row).contains(QLatin1String("NEEDLE")));
            }
            // Видимое подмножество: поиск идёт только по нему.
            m.model->setLogLevelFilter({LogLevel::Error});
            CHECK(testdocs::settle(*m.model), "filter settles");
            compareWithModel(QString::fromLatin1(m.name) + " filtered", *m.model);
            checkStartRows(QString::fromLatin1(m.name) + " filtered", *m.model, order, matches);
            m.model->setLogLevelFilter({});
            CHECK(testdocs::settle(*m.model), "filter reset settles");
        }
    }

    // --- Флаги запроса: слово целиком, регекс, регистр -----------------------
    checkQueryValidity();
    checkHistory();
    {
        const QString c = testdocs::writeFile(dir, "c.log", flagsFixture());
        const QString d = testdocs::writeFile(dir, "d.log", flagsFixture());
        auto resident = testdocs::buildResident({c}, schema);
        auto indexed = testdocs::buildIndexed({c}, schema);
        auto residentMerged = testdocs::buildResident({c, d}, schema);
        auto indexedMerged = testdocs::buildIndexed({c, d}, schema, testdocs::IndexedLoad::Concurrent);
        struct { const char* name; LogModel* model; } models[] = {
            {"flags resident", resident.model.get()},
            {"flags indexed", indexed.model.get()},
            {"flags resident merged", residentMerged.model.get()},
            {"flags indexed merged", indexedMerged.model.get()},
        };
        for (const auto& m : models) {
            checkFlags(QString::fromLatin1(m.name), *m.model);
            m.model->setLogLevelFilter({LogLevel::Error});
            CHECK(testdocs::settle(*m.model), "flags filter settles");
            checkFlags(QString::fromLatin1(m.name) + " filtered", *m.model);
        }
    }

    // --- Кириллица в однобайтных кодировках на индексной вкладке (F-1) -------
    // TextMatcher получает уже декодированный текст: тот же лог в cp1251,
    // cp866 и KOI8-R обязан показать строка в строку тот же текст, что в
    // UTF-8, и найтись теми же запросами (без учёта регистра, слово целиком,
    // регекс) синхронно, сканом снапшота и правилом Find All.
    {
        const QByteArray utf8 = flagsFixture(/*cyrillicOnly=*/true);
        const QString text = QString::fromUtf8(utf8);
        const QString ref = testdocs::writeFile(dir, "cyr-utf8.log", utf8);
        auto refDoc = testdocs::buildIndexed({ref}, schema);
        QStringList want;
        for (int row = 0; row < refDoc.model->rowCount(); ++row)
            want << refDoc.model->messageAt(row);
        QString cp1251Path;
        for (const TextEncoding enc : {TextEncoding::Windows1251, TextEncoding::Ibm866,
                                       TextEncoding::Koi8R}) {
            const QString name = QStringLiteral("cyrillic indexed %1").arg(TextEncodings::displayName(enc));
            const QString path = testdocs::writeFile(
                dir, QStringLiteral("cyr-%1.log").arg(int(enc)), testdocs::encodeText(text, enc));
            if (enc == TextEncoding::Windows1251)
                cp1251Path = path;
            auto doc = testdocs::buildIndexed({path}, schema);
            QStringList got;
            for (int row = 0; row < doc.model->rowCount(); ++row)
                got << doc.model->messageAt(row);
            CHECK(got == want, name + ": text equals the UTF-8 file's");
            checkFlags(name, *doc.model);
        }
        auto merged = testdocs::buildIndexed({ref, cp1251Path}, schema,
                                             testdocs::IndexedLoad::Concurrent);
        int replacement = 0;
        for (int row = 0; row < merged.model->rowCount(); ++row)
            replacement += merged.model->messageAt(row).contains(QChar::ReplacementCharacter);
        CHECK(merged.model->rowCount() == 2 * int(want.size()) && replacement == 0,
              "cyrillic merged UTF-8 + Windows-1251: every file read with its own decoder");
        checkFlags(QStringLiteral("cyrillic indexed merged UTF-8 + Windows-1251"), *merged.model);

        // Резидентный путь (F-1, шаг 3): те же файлы ниже порога — тот же
        // текст (эталон — резидентная вкладка на UTF-8-файле) и те же находки.
        auto refResident = testdocs::buildResident({ref}, schema);
        QStringList wantResident;
        for (int row = 0; row < refResident.model->rowCount(); ++row)
            wantResident << refResident.model->messageAt(row);
        for (const TextEncoding enc : {TextEncoding::Windows1251, TextEncoding::Ibm866,
                                       TextEncoding::Koi8R}) {
            const QString name = QStringLiteral("cyrillic resident %1").arg(TextEncodings::displayName(enc));
            auto doc = testdocs::buildResident(
                {dir.filePath(QStringLiteral("cyr-%1.log").arg(int(enc)))}, schema);
            QStringList got;
            for (int row = 0; row < doc.model->rowCount(); ++row)
                got << doc.model->messageAt(row);
            CHECK(!got.isEmpty() && got == wantResident, name + ": text equals the UTF-8 file's");
            checkFlags(name, *doc.model);
        }
        auto mergedResident = testdocs::buildResident({ref, cp1251Path}, schema);
        replacement = 0;
        for (int row = 0; row < mergedResident.model->rowCount(); ++row)
            replacement += mergedResident.model->messageAt(row).contains(QChar::ReplacementCharacter);
        CHECK(mergedResident.model->rowCount() == 2 * int(want.size()) && replacement == 0,
              "cyrillic resident merged UTF-8 + Windows-1251: every file read with its own decoder");
        checkFlags(QStringLiteral("cyrillic resident merged UTF-8 + Windows-1251"), *mergedResident.model);
    }

    // --- Поиск через границу цвета ANSI (F-3) ---------------------------------
    // Коды сняты на обоих бэкендах до поиска: «ERROR foo» находит
    // `\x1b[31mERROR\x1b[0m foo` синхронно, сканом снапшота и правилом Find
    // All, а текст самих кодов не находится.
    {
        QByteArray colored;
        QVector<int> expected; // строки файла с «ERROR foo»
        for (int i = 0; i < 400; ++i) {
            const QByteArray stamp = "\x1b[2m2026-03-05 10:" + QByteArray::number(i / 60).rightJustified(2, '0')
                                   + ':' + QByteArray::number(i % 60).rightJustified(2, '0') + ".000\x1b[0m [w] ";
            if (i % 97 == 11) {
                colored += stamp + "\x1b[31mERROR\x1b[0m foo \x1b]8;;http://x/\x1b\\link\x1b]8;;\x1b\\ " + QByteArray::number(i) + '\n';
                expected.append(i);
            } else if (i % 89 == 7) {
                colored += stamp + "\x1b[31mERROR\x1b[0m\x1b[1m bar\x1b[0m " + QByteArray::number(i) + '\n';
            } else {
                colored += stamp + "\x1b[32mINFO\x1b[0m ordinary " + QByteArray::number(i) + '\n';
            }
        }
        const QString c1 = testdocs::writeFile(dir, "ansi-a.log", colored);
        auto resident = testdocs::buildResident({c1}, schema);
        auto indexed = testdocs::buildIndexed({c1}, schema);
        const auto query = [](const char* text, bool word, bool regex) {
            TextQuery q;
            q.text = QString::fromUtf8(text);
            q.wholeWord = word;
            q.regex = regex;
            return q;
        };
        for (LogModel* model : {resident.model.get(), indexed.model.get()}) {
            const QString name = model->isIndexedBackend() ? QStringLiteral("ANSI indexed")
                                                           : QStringLiteral("ANSI resident");
            const LogScanSnapshot snapshot = model->scanSnapshot(true);
            for (const TextQuery& q : {query("ERROR foo", false, false), query("error foo", true, false),
                                       query("ERROR\\s+foo\\s+link", false, true),
                                       query("] ERROR foo link", false, false)}) {
                QVector<int> found;
                int row = -1;
                for (;;) {
                    const QModelIndex next = model->findOccurrence(q, row, true, false);
                    if (!next.isValid())
                        break;
                    found.append(next.row());
                    CHECK(QuickSearch::scan(snapshot, q, true, row, false) == next.row(),
                          name + ": scan = sync for " + q.text);
                    row = next.row();
                }
                QVector<int> want;
                for (const int i : expected)
                    want.append(i); // одно-файловая вкладка, метки по порядку: строка = номер
                CHECK(found == want, name + ": '" + q.text + QStringLiteral("' found %1 of %2")
                                                     .arg(found.size()).arg(want.size()));
                FilterRuleSet rules;
                FilterRule rule;
                rule.text = q.patternText();
                rule.isRegex = q.needsRegex();
                rules.rules.append(rule);
                rules.bindFields({}, false);
                int ruleHits = 0;
                for (int r = 0; r < model->rowCount(); ++r)
                    ruleHits += rules.matchesLine(model->messageAt(r), LogEntryFields());
                CHECK(ruleHits == want.size(), name + ": Find All rule for " + q.text);
            }
            for (const char* code : {"[31m", "\x1b", "8;;http", "38;5"}) {
                const TextQuery q = query(code, false, false);
                CHECK(!model->findOccurrence(q, -1, true, true).isValid(),
                      name + ": escape text is not found: " + QString::fromUtf8(code));
            }
        }
    }

    // --- Пересчёт номера через вставки ---------------------------------------
    {
        using I = QuickSearch::Insertion;
        CHECK(QuickSearch::mapRow(10, {}) == 10, "no insertions");
        CHECK(QuickSearch::mapRow(10, {I{11, 5}}) == 10, "insertion after the row");
        CHECK(QuickSearch::mapRow(10, {I{10, 5}}) == 15, "insertion at the row shifts it");
        CHECK(QuickSearch::mapRow(10, {I{0, 2}, I{12, 3}}) == 15,
              "second insertion counts in updated coordinates");
        CHECK(QuickSearch::mapRow(10, {I{0, 2}, I{13, 3}}) == 12, "insertion after the shifted row");
    }

    // --- Асинхронно: результат, сдвиг, перестройка, отмена --------------------
    {
        auto doc = testdocs::buildResident({a}, schema);
        LogModel& model = *doc.model;
        QuickSearch search;
        int found = -2, invalidated = 0;
        QObject::connect(&search, &QuickSearch::finished, [&](int row) { found = row; });
        QObject::connect(&search, &QuickSearch::invalidated, [&] { ++invalidated; });

        search.start(&model, TextQuery("NEEDLE", Qt::CaseSensitive), true, 100);
        CHECK(search.isRunning(), "search runs");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "search finishes");
        CHECK(found == 713, "forward from row 100 finds row 713");
        CHECK(!search.isRunning(), "search is done");

        // Строки, вставленные перед найденной, пока идёт скан: номер в текущей
        // модели сдвигается. Слияние — синхронно, до того как результат мог
        // прийти (он доставляется циклом событий).
        auto earlier = testdocs::buildResident({b}, schema);
        QVector<std::shared_ptr<LogEntry>> batch;
        for (const auto& entry : earlier.model->residentEntriesForFieldMutation()) {
            if (entry->timestamp().toString("mm") == "00") // 10:00:xx — раньше, чем 10:01 у row 1400
                batch.append(entry);
        }
        std::sort(batch.begin(), batch.end(), logEntryPtrLess);
        found = -2;
        search.start(&model, TextQuery("NEEDLE 1413", Qt::CaseSensitive), true, 0);
        const int before = model.rowCount();
        model.mergeEntries(batch);
        const int inserted = model.rowCount() - before;
        CHECK(inserted > 0, "entries merged during the search");
        CHECK(testdocs::waitFor([&] { return found != -2; }), "shifted search finishes");
        CHECK(found >= 0 && model.messageAt(found).endsWith("NEEDLE 1413"),
              "found row is mapped through insertions to the current model");
        CHECK(invalidated == 0, "insertions do not invalidate");

        // Перестройка модели посреди скана — invalidated, а не номер.
        found = -2;
        search.start(&model, TextQuery("NEEDLE", Qt::CaseSensitive), false, -1);
        model.setLogLevelFilter({LogLevel::Error}); // < 100k записей — синхронный reset
        CHECK(testdocs::waitFor([&] { return invalidated == 1; }), "reset invalidates the search");
        testdocs::pump(50);
        CHECK(found == -2, "invalidated search reports no row");

        // Отмена: ни результата, ни invalidated.
        search.start(&model, TextQuery("NEEDLE", Qt::CaseSensitive), true, -1);
        search.cancel();
        CHECK(!search.isRunning(), "cancelled search is not running");
        testdocs::pump(200);
        CHECK(found == -2 && invalidated == 1, "cancelled search stays silent");

        // Не найдено.
        search.start(&model, TextQuery("absent text", Qt::CaseSensitive), true, -1);
        CHECK(testdocs::waitFor([&] { return found != -2; }), "search without matches finishes");
        CHECK(found == -1, "no match reports -1");
    }

    std::fprintf(stdout, "Quick search: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
