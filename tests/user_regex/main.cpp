// ============================================================================
// user_regex — F-14: регекс из текста пользователя понимает Юникод.
//
// Все места, где компилируется регекс пользователя, обязаны вести себя
// одинаково, и \w, \d, \s, \b, сворачивание регистра работают не только для
// ASCII:
//   • UserRegex — сама функция: классы, границы, не-ASCII цифры, регистр;
//   • быстрый поиск (TextMatcher + QuickSearch::scan), подсветка найденного
//     (TextQuery::highlightPattern → TextMatchHighlighter), Find All и Text
//     Filters (FilterRuleSet на модели), Row Highlighters (маркеры модели),
//     фильтр сканера каталога по содержимому (FileContentSearch) — на одном
//     логе с кириллицей в UTF-8 и cp1251, резидентный и индексный бэкенды;
//   • «слово целиком» быстрого поиска и \b находят одни и те же строки.
// ASCII-запросы находят то же, что и раньше (регрессия профилей и маркеров).
// ============================================================================

#include "filecontentsearch.h"
#include "filterruleset.h"
#include "quicksearch.h"
#include "testdocuments.h"
#include "textmatchhighlighter.h"
#include "textquery.h"
#include "userregex.h"

#include <QElapsedTimer>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>

using namespace Qt::StringLiterals;

static int failures = 0;
#define CHECK(condition, label) do { \
    if (!(condition)) { ++failures; std::fprintf(stderr, "FAIL: %s (line %d)\n", qPrintable(QString(label)), __LINE__); } \
} while (0)

// ---------------------------------------------------------------------------
// Сама функция
// ---------------------------------------------------------------------------

static bool hit(const QString& pattern, const QString& text, Qt::CaseSensitivity cs = Qt::CaseSensitive)
{
    const QRegularExpression re = UserRegex::compile(pattern, cs);
    return re.isValid() && re.match(text).hasMatch();
}

static void checkFunction()
{
    CHECK(hit(u"пользовател\\w+"_s, u"вход пользователя в систему"_s), "\\w takes Cyrillic letters");
    CHECK(hit(u"\\bошибка\\b"_s, u"произошла ошибка, повтор"_s), "\\b around a Cyrillic word");
    CHECK(!hit(u"\\bошибка\\b"_s, u"произошлаошибка"_s), "\\b needs a real boundary");
    CHECK(!hit(u"\\bошибк\\b"_s, u"ошибка"_s), "\\b does not cut a Cyrillic word");
    CHECK(hit(u"код \\d+ принят"_s, u"код 42 принят"_s), "\\d between Cyrillic words");
    CHECK(hit(u"^\\d+$"_s, u"٣٤"_s), "\\d takes non-ASCII digits (documented)");
    CHECK(hit(u"\\s"_s, u"a b"_s), "\\s takes a no-break space");
    CHECK(hit(u"^[[:alpha:]]+$"_s, u"щёлкнул"_s), "POSIX classes are Unicode too");
    CHECK(hit(u"ОШИБКА"_s, u"ошибка"_s, Qt::CaseInsensitive), "case folding is Unicode");
    CHECK(!hit(u"ОШИБКА"_s, u"ошибка"_s, Qt::CaseSensitive), "match case stays exact");
    // ASCII — как прежде.
    CHECK(hit(u"^ERROR.*timeout \\d+ms$"_s, u"ERROR db timeout 30ms"_s), "ASCII regex unchanged");
    CHECK(hit(u"\\bid=\\w+\\b"_s, u"user id=abc_42 done"_s), "ASCII \\w, \\b unchanged");
    CHECK(!UserRegex::compile(u"a(b"_s, Qt::CaseSensitive).isValid(), "invalid stays invalid");
    CHECK(UserRegex::options(Qt::CaseInsensitive) & QRegularExpression::CaseInsensitiveOption,
          "case-insensitive option is kept");
    CHECK(UserRegex::options(Qt::CaseSensitive) & QRegularExpression::UseUnicodePropertiesOption,
          "Unicode classes are always on");
}

// ---------------------------------------------------------------------------
// Все места на одном документе
// ---------------------------------------------------------------------------

struct Probe {
    QString text;
    bool regex = true;
    bool wholeWord = false;
    bool caseSensitive = false;
    bool expect = true;
};

static QString describe(const Probe& p)
{
    return u"'%1'%2%3%4"_s.arg(p.text, p.regex ? u" regex"_s : QString(),
                               p.wholeWord ? u" word"_s : QString(),
                               p.caseSensitive ? u" case"_s : QString());
}

static QList<Probe> probes()
{
    return {
        { u"пользовател\\w+"_s },
        { u"\\bошибка\\b"_s },
        { u"\\bошибк\\b"_s, true, false, false, false },
        { u"\\bтаймаут"_s },
        { u"попытка \\d+ \\w+"_s },
        { u"\\w+ \\d+ после"_s },
        { u"ОШИБКА \\d+"_s },
        { u"ОШИБКА \\d+"_s, true, false, true, false },
        { u"\\bвошёл\\b"_s },
        { u"^\\S+ \\S+ \\[\\w+-\\d\\] \\w+ - Пользователь\\s\\w+"_s },
        { u"Хранилище\\.\\w+ \\(строка \\d+\\)"_s },
        { u"[[:upper:]]{5,}:"_s, true, false, true, true },
        // «Слово целиком» быстрого поиска — без регекса и с ним.
        { u"ошибка"_s, false, true },
        { u"ошибк"_s, false, true, false, false },
        { u"Иванов"_s, false, true },
        { u"\\w+ов"_s, true, true },
        // ASCII — как прежде.
        { u"ascii preamble line \\d+$"_s },
        { u"\\bpreamble\\b"_s },
        { u"ERROR.*таймаут"_s },
        { u"\\bascii\\w"_s, true, false, false, false },
    };
}

static TextQuery toQuery(const Probe& p)
{
    TextQuery q;
    q.text = p.text;
    q.regex = p.regex;
    q.wholeWord = p.wholeWord;
    q.caseSensitive = p.caseSensitive;
    return q;
}

// Строки, где запрос находит совпадение (независимый перебор).
static QVector<qint64> matcherRows(const LogScanSnapshot& snapshot, const TextMatcher& matcher,
                                   QStringList* texts = nullptr)
{
    QVector<qint64> rows;
    snapshot.forEachLine(0, [&](qint64 row, const LogEntryMeta&, QStringView text) {
        if (texts)
            texts->append(text.toString());
        if (matcher.matches(text))
            rows.append(row);
        return true;
    });
    return rows;
}

static void checkDocument(const QString& what, testdocs::Document& doc, const QString& path)
{
    CHECK(doc.errors.isEmpty(), what + u": loads"_s);
    LogModel& model = *doc.model;
    const LogScanSnapshot snapshot = model.scanSnapshot(true);
    for (const Probe& p : probes()) {
        const QString label = what + u" "_s + describe(p);
        const TextQuery q = toQuery(p);
        CHECK(q.isValid(), label + u": valid"_s);
        const TextMatcher matcher(q);
        QStringList texts;
        const QVector<qint64> rows = matcherRows(snapshot, matcher, &texts);
        CHECK(rows.isEmpty() != p.expect, label + u": quick search matcher finds "_s
                                                  + QString::number(rows.size()) + u" rows"_s);

        // Быстрый поиск (фоновый скан) — первая из найденных.
        const qint64 found = QuickSearch::scan(snapshot, q, true, -1, false);
        CHECK(found == (rows.isEmpty() ? -1 : rows.first()), label + u": QuickSearch::scan"_s);

        // Подсветка найденного — ровно на тех строках, что нашёл поиск.
        TextMatchHighlighter highlighter;
        highlighter.setPatterns({ q.highlightPattern(Qt::yellow) });
        const QSet<qint64> rowSet(rows.cbegin(), rows.cend());
        int highlightMismatch = 0;
        for (int r = 0; r < texts.size(); ++r) {
            if (highlighter.computeSpans(texts[r]).isEmpty() == rowSet.contains(r))
                ++highlightMismatch;
        }
        CHECK(highlightMismatch == 0, label + u": highlight differs on "_s
                                          + QString::number(highlightMismatch) + u" rows"_s);

        // Row Highlighters: маркер с тем же паттерном красит те же строки.
        model.setRowMarkers({ q.highlightPattern(Qt::cyan) });
        int marked = 0;
        int markerMismatch = 0;
        for (int r = 0; r < model.rowCount(); ++r) {
            const bool colored = model.data(model.index(r), LogModel::RowMarkerColorRole)
                                     .value<QColor>().isValid();
            marked += colored;
            if (colored != rowSet.contains(r))
                ++markerMismatch;
        }
        CHECK(markerMismatch == 0, label + u": row markers differ on "_s
                                       + QString::number(markerMismatch) + u" rows"_s);
        model.setRowMarkers({});

        // Find All / Text Filters: правило из запроса (как FilterPanelWidget::
        // quickRule) находит строки тогда же.
        FilterRuleSet rules;
        FilterRule rule;
        rule.action = FilterRule::Action::Include;
        rule.text = q.patternText();
        rule.isRegex = q.needsRegex();
        rule.caseSensitive = q.caseSensitive;
        rules.rules.append(rule);
        rules.bindFields({}, false);
        model.setFilterRules(rules);
        CHECK(testdocs::settle(model, 0), label + u": filter settles"_s);
        const bool filtered = model.rowCount() > 0;
        CHECK(filtered == p.expect, label + u": Text Filters"_s);
        model.setFilterRules(FilterRuleSet());
        CHECK(testdocs::settle(model, 0), label + u": filter reset settles"_s);

        // Сканер каталога по содержимому — по тому же файлу.
        FileContentSearch::Options options;
        options.fallback = testdocs::kTestFallbackEncoding;
        const ContentQuery content(q.patternText(), q.needsRegex(), q.caseSensitivity());
        CHECK(FileContentSearch::search(path, content, options).matched == p.expect,
              label + u": directory scanner content filter"_s);
    }

    // «Слово целиком» ⇔ \b на кириллице: одни и те же строки.
    for (const QString& word : { u"ошибка"_s, u"ошибк"_s, u"Иванов"_s, u"повтор"_s, u"диск"_s }) {
        TextQuery whole(word, Qt::CaseInsensitive);
        whole.wholeWord = true;
        TextQuery boundary(u"\\b"_s + word + u"\\b"_s, Qt::CaseInsensitive);
        boundary.regex = true;
        CHECK(matcherRows(snapshot, TextMatcher(whole)) == matcherRows(snapshot, TextMatcher(boundary)),
              what + u": whole word and \\b agree on '"_s + word + u"'"_s);
    }
}

static void checkDocuments()
{
    QTemporaryDir tmp;
    const QDir dir(tmp.path());
    const QString text = testdocs::cyrillicLogText(4);
    const struct { const char* name; TextEncoding encoding; } kinds[] = {
        { "UTF-8", TextEncoding::Utf8 },
        { "Windows-1251", TextEncoding::Windows1251 },
    };
    for (const auto& kind : kinds) {
        const QString path = testdocs::writeFile(dir, QString::fromLatin1(kind.name) + u".log"_s,
                                                 testdocs::encodeText(text, kind.encoding));
        auto resident = testdocs::buildResident({ path }, QString());
        checkDocument(QString::fromLatin1(kind.name) + u" resident"_s, resident, path);
        auto indexed = testdocs::buildIndexed({ path }, QString());
        checkDocument(QString::fromLatin1(kind.name) + u" indexed"_s, indexed, path);
    }
}

// Замер: фильтр по регексу (Text Filters) на обоих бэкендах.
// user_regex --bench <file>
static int bench(const QString& path)
{
    const QString patterns[] = {
        u"ERROR.*timeout"_s, u"\\w+"_s, u"\\d+"_s, u"lag=\\d+ms, peers=\\d"_s, u"absent_\\w+_text"_s,
    };
    for (const bool indexed : { false, true }) {
        QElapsedTimer t;
        t.start();
        auto doc = indexed ? testdocs::buildIndexed({ path }, QString())
                           : testdocs::buildResident({ path }, QString());
        std::printf("%s: loaded %d rows in %lld ms\n", indexed ? "indexed" : "resident",
                    doc.model->rowCount(), (long long)t.elapsed());
        for (const QString& pattern : patterns) {
            qint64 best = -1;
            int rows = 0;
            for (int run = 0; run < 3; ++run) {
                FilterRuleSet rules;
                FilterRule rule;
                rule.action = FilterRule::Action::Include;
                rule.text = pattern;
                rule.isRegex = true;
                rules.rules.append(rule);
                rules.bindFields({}, false);
                t.restart();
                doc.model->setFilterRules(rules);
                testdocs::settle(*doc.model, 0);
                const qint64 ms = t.elapsed();
                rows = doc.model->rowCount();
                best = best < 0 ? ms : qMin(best, ms);
                doc.model->setFilterRules(FilterRuleSet());
                testdocs::settle(*doc.model, 0);
            }
            std::printf("  filter %-28s rows %8d  best of 3: %lld ms\n", qPrintable(pattern), rows,
                        (long long)best);
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc >= 3 && QByteArray(argv[1]) == "--bench")
        return bench(QString::fromLocal8Bit(argv[2]));
    checkFunction();
    checkDocuments();
    if (failures) {
        std::fprintf(stderr, "user_regex: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("user_regex: OK\n");
    return 0;
}
