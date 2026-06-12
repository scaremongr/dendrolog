// Smoke test for the refactored LogPattern engine + PatternHeuristics.
#include "logpattern.h"
#include "patternheuristics.h"

#include <QCoreApplication>
#include <QDebug>

static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            qInfo().noquote() << "[ OK ]" << msg;                            \
        } else {                                                             \
            ++g_failures;                                                    \
            qCritical().noquote() << "[FAIL]" << msg;                        \
        }                                                                    \
    } while (0)

static PatternBlock mk(PatternBlock::MatchKind kind, const QString& name,
                       const QString& lead = QString(), const QString& closing = QString(),
                       const QString& sep = QString())
{
    PatternBlock b;
    b.matchKind = kind;
    b.name = name;
    b.leadingText = lead;    // opening wrapper
    b.closingText = closing; // closing wrapper
    b.separator = sep;       // glue to the next block
    return b;
}

static QString fieldValue(const LogPattern& p, const QString& line, const QString& fieldName)
{
    const QStringList names = p.fieldNames();
    const int idx = names.indexOf(fieldName);
    if (idx < 0)
        return QStringLiteral("<no such field>");
    const LogEntryFields f = p.extractFields(line);
    return f.get(idx, line).toString();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Schema: Timestamp  [Thread]  LEVEL - Message
    PatternDefinition def;
    def.blocks = {
        mk(PatternBlock::MatchKind::Timestamp, "Timestamp"),
        mk(PatternBlock::MatchKind::TextUntilSeparator, "Thread", "[", "]"),
        mk(PatternBlock::MatchKind::Level, "Level", "", "", "-"),
        mk(PatternBlock::MatchKind::Remainder, "Message"),
    };
    LogPattern p(LogPattern::serializeDefinition(def));
    CHECK(p.isValid(), "schema compiles");

    // 1. Full match
    {
        const QString line = "2025-06-11 12:00:01.123 [worker-1] ERROR - Something failed";
        const LineMatchResult r = p.matchLine(line);
        CHECK(r.ok && r.unparsedStart == -1, "full match accepted");
        CHECK(fieldValue(p, line, "Timestamp") == "2025-06-11 12:00:01.123", "timestamp value");
        CHECK(fieldValue(p, line, "Thread") == "worker-1", "thread value");
        CHECK(fieldValue(p, line, "Level") == "ERROR", "level value");
        CHECK(fieldValue(p, line, "Message") == "Something failed", "message value");
    }

    // 2. Whitespace collapsing: extra spaces / tabs between blocks
    {
        const QString line = "2025-06-11 12:00:01   [worker-1]\tWARN  -  padded";
        const LineMatchResult r = p.matchLine(line);
        CHECK(r.ok && r.unparsedStart == -1, "extra whitespace collapses");
    }

    // 3. Anchor: garbage before the timestamp
    {
        const QString line = "\x01garbage>> 2025-06-11 12:00:01 [t] INFO - ok";
        const LineMatchResult r = p.matchLine(line);
        CHECK(r.ok, "leading garbage before anchor tolerated");
        CHECK(fieldValue(p, line, "Level") == "INFO", "level found after garbage");
    }

    // 4. Graceful degradation: thread block broken mid-line
    {
        const QString line = "2025-06-11 12:00:01 !!corrupted!! rest of the line";
        const LogEntryFields f = p.extractFields(line);
        CHECK(!f.isEmpty(), "degraded line still accepted");
        CHECK(fieldValue(p, line, "Timestamp") == "2025-06-11 12:00:01", "timestamp kept on degraded line");
        CHECK(fieldValue(p, line, "Message") == "!!corrupted!! rest of the line",
              QString("tail routed to Message, got '%1'").arg(fieldValue(p, line, "Message")));
    }

    // 5. Continuation line must NOT be misclassified
    {
        const QString line = "    at com.example.Foo.bar(Foo.java:12)";
        CHECK(p.extractFields(line).isEmpty(), "stack-trace continuation rejected");
    }

    // 6. Optional separator after self-delimiting Level ("-" missing)
    {
        const QString line = "2025-06-11 12:00:01 [w] INFO no dash here";
        const LineMatchResult r = p.matchLine(line);
        CHECK(r.ok, "missing optional separator after Level tolerated");
    }

    // 7. Ignored block produces no field
    {
        PatternDefinition d2 = def;
        d2.blocks[1].ignored = true; // hide Thread
        LogPattern p2(LogPattern::serializeDefinition(d2));
        CHECK(p2.isValid(), "schema with ignored block compiles");
        CHECK(p2.fieldNames() == QStringList({"Timestamp", "Level", "Message"}),
              "ignored block excluded from fields");
        const QString line = "2025-06-11 12:00:01 [secret-id] ERROR - msg";
        CHECK(fieldValue(p2, line, "Message") == "msg", "ignored block still consumed");
    }

    // 8. Validation: bad custom regex
    {
        PatternDefinition bad;
        bad.blocks = { mk(PatternBlock::MatchKind::CustomRegex, "X") };
        bad.blocks[0].customRegex = "([";
        QString err;
        CHECK(!LogPattern::validateDefinition(bad, &err) && !err.isEmpty(),
              QString("invalid regex rejected: %1").arg(err));
    }

    // 9. Validation: duplicate names, Remainder not last
    {
        PatternDefinition bad;
        bad.blocks = { mk(PatternBlock::MatchKind::TextUntilSeparator, "A", "", "", " "),
                       mk(PatternBlock::MatchKind::TextUntilSeparator, "a", "", "", " ") };
        CHECK(!LogPattern::validateDefinition(bad), "duplicate field names rejected");

        PatternDefinition bad2;
        bad2.blocks = { mk(PatternBlock::MatchKind::Remainder, "M"),
                        mk(PatternBlock::MatchKind::Level, "L") };
        CHECK(!LogPattern::validateDefinition(bad2), "mid-schema Remainder rejected");
    }

    // 10. Auto-detect
    {
        const QString line = "2025-06-11 12:00:01,123 [worker-1] ERROR Some.Logger - message here";
        const PatternDefinition d = PatternHeuristics::suggestSchema(line);
        LogPattern ap(LogPattern::serializeDefinition(d));
        CHECK(ap.isValid(), "auto-detected schema compiles");
        qInfo().noquote() << "       auto-detected fields:" << ap.fieldNames().join(", ");
        const LineMatchResult r = ap.matchLine(line);
        CHECK(r.ok && r.unparsedStart == -1, "auto-detected schema matches its own sample");
        CHECK(ap.fieldNames().contains("Timestamp") && ap.fieldNames().contains("Level"),
              "auto-detect found timestamp and level");
    }

    // 11. Grok import
    {
        PatternDefinition d;
        QString warn;
        const bool ok = PatternHeuristics::definitionFromGrok(
            "%{TIMESTAMP_ISO8601:time} \\[%{WORD:thread}\\] %{LOGLEVEL:level} %{GREEDYDATA:msg}",
            &d, &warn);
        CHECK(ok && d.blocks.size() == 4, "grok expression parsed");
        LogPattern gp(LogPattern::serializeDefinition(d));
        CHECK(gp.isValid(), "grok schema compiles");
        const QString line = "2025-06-11 12:00:01 [main] INFO started ok";
        CHECK(fieldValue(gp, line, "msg") == "started ok",
              QString("grok msg field, got '%1'").arg(fieldValue(gp, line, "msg")));
        // Unnamed token becomes a hidden (ignored) block.
        PatternDefinition d2;
        PatternHeuristics::definitionFromGrok("%{TIMESTAMP_ISO8601} %{GREEDYDATA:msg}", &d2, nullptr);
        CHECK(d2.blocks.size() == 2 && d2.blocks[0].ignored, "unnamed grok token hidden");
    }

    // 12. Legacy serialized schema (v1, no 'ignored' key) still loads
    {
        const QString legacyJson =
            R"({"version":1,"type":"dynamic-block-schema","linePrefix":"",)"
            R"("blocks":[{"name":"Timestamp","kind":"timestamp","leadingText":"","separator":"","customRegex":""},)"
            R"({"name":"Message","kind":"remainder","leadingText":"","separator":"","customRegex":""}]})";
        LogPattern lp(legacyJson);
        CHECK(lp.isValid(), "v1 schema loads");
        CHECK(lp.fieldNames() == QStringList({"Timestamp", "Message"}), "v1 fields preserved");
    }

    // 13. Legacy log4j conversion pattern migration
    {
        LogPattern lp("%d %p %m");
        CHECK(lp.isValid(), "legacy conversion pattern migrates");
    }

    // 14a. v2 schema migration: enclosed block's "separator" was its
    // closing bracket; it must become closingText with empty glue.
    {
        const QString v2Json =
            R"({"version":2,"type":"dynamic-block-schema","linePrefix":"",)"
            R"("blocks":[{"name":"Thread","kind":"text","leadingText":"[","separator":"]","customRegex":""},)"
            R"({"name":"Message","kind":"remainder","leadingText":"","separator":"","customRegex":""}]})";
        LogPattern lp(v2Json);
        CHECK(lp.isValid(), "v2 schema loads");
        CHECK(lp.definition().blocks[0].closingText == "]"
                  && lp.definition().blocks[0].separator.isEmpty(),
              "v2 wrapper migrated to closingText");
        const QString line = "[worker-9] all good";
        CHECK(fieldValue(lp, line, "Thread") == "worker-9", "migrated wrapper still matches");
    }

    // 14b. Regex separator (explicit glue with Rx toggle)
    {
        PatternDefinition d;
        d.blocks = {
            mk(PatternBlock::MatchKind::TextUntilSeparator, "F1"),
            mk(PatternBlock::MatchKind::Remainder, "Message"),
        };
        d.blocks[0].separator = "-{2,}";
        d.blocks[0].separatorIsRegex = true;
        LogPattern rp(LogPattern::serializeDefinition(d));
        CHECK(rp.isValid(), "regex separator schema compiles");
        const QString line = "alpha --- beta";
        CHECK(fieldValue(rp, line, "F1") == "alpha" && fieldValue(rp, line, "Message") == "beta",
              QString("regex separator splits, got F1='%1' Msg='%2'")
                  .arg(fieldValue(rp, line, "F1"), fieldValue(rp, line, "Message")));

        d.blocks[0].separator = "([";  // invalid regex must be rejected
        QString err;
        CHECK(!LogPattern::validateDefinition(d, &err), "invalid separator regex rejected");
    }

    // 14c. Constant text is a full block: named constant becomes a field,
    // unnamed constant stays pure structure.
    {
        PatternDefinition d;
        PatternBlock marker;
        marker.matchKind = PatternBlock::MatchKind::ConstantText;
        marker.customRegex = "REQ>";
        d.blocks = { marker, mk(PatternBlock::MatchKind::Remainder, "Message") };

        LogPattern unnamed(LogPattern::serializeDefinition(d));
        CHECK(unnamed.isValid() && unnamed.fieldNames() == QStringList({"Message"}),
              "unnamed constant creates no field");

        d.blocks[0].name = "Marker";
        LogPattern named(LogPattern::serializeDefinition(d));
        CHECK(named.isValid() && named.fieldNames() == QStringList({"Marker", "Message"}),
              "named constant creates a field");
        const QString line = "REQ> hello";
        CHECK(fieldValue(named, line, "Marker") == "REQ>", "named constant value captured");

        d.blocks[0].ignored = true; // hide wins over the name
        LogPattern hidden(LogPattern::serializeDefinition(d));
        CHECK(hidden.isValid() && hidden.fieldNames() == QStringList({"Message"}),
              "hidden named constant creates no field");
    }

    // 14d. Text allows an empty value (the former Optional text)
    {
        const QString line = "2025-06-11 12:00:01 [] INFO - empty thread";
        const LineMatchResult r = p.matchLine(line);
        CHECK(r.ok && r.unparsedStart == -1, "empty Text value tolerated");
        CHECK(fieldValue(p, line, "Message") == "empty thread", "message after empty field");
    }

    // 14e. Legacy kind aliases map onto the unified kinds
    {
        const QString aliasJson =
            R"({"version":2,"type":"dynamic-block-schema","linePrefix":"",)"
            R"("blocks":[{"name":"A","kind":"optional-text","leadingText":"","separator":"|","customRegex":""},)"
            R"({"name":"B","kind":"optional-path","leadingText":"","separator":"","customRegex":""}]})";
        LogPattern lp(aliasJson);
        CHECK(lp.isValid(), "legacy optional kinds load");
        CHECK(lp.definition().blocks[0].matchKind == PatternBlock::MatchKind::TextUntilSeparator
                  && lp.definition().blocks[1].matchKind == PatternBlock::MatchKind::FilePath,
              "optional kinds folded into Text / File path");
    }

    // 14f. Blocks with a closing wrapper (and constants) are self-delimiting:
    // the glue after them becomes optional.
    {
        PatternDefinition d;
        d.blocks = {
            mk(PatternBlock::MatchKind::TextUntilSeparator, "Thread", "[", "]", ":"),
            mk(PatternBlock::MatchKind::Remainder, "Message"),
        };
        LogPattern wp(LogPattern::serializeDefinition(d));
        CHECK(wp.isValid(), "wrapped block with glue compiles");
        CHECK(LogPattern::blockIsSelfDelimiting(d.blocks[0]), "closing wrapper => self-delimiting");
        CHECK(wp.matchLine("[w]: hello").ok && wp.matchLine("[w]: hello").unparsedStart == -1,
              "glue present matches");
        const LineMatchResult noGlue = wp.matchLine("[w] hello");
        CHECK(noGlue.ok && noGlue.unparsedStart == -1, "missing glue after wrapper tolerated");

        PatternBlock constant;
        constant.matchKind = PatternBlock::MatchKind::ConstantText;
        constant.customRegex = "X";
        CHECK(LogPattern::blockIsSelfDelimiting(constant), "constant => self-delimiting");
    }

    // 14. Built-in presets all compile
    {
        bool allValid = true;
        for (const auto& preset : PatternHeuristics::builtInPresets()) {
            LogPattern pp(LogPattern::serializeDefinition(preset.definition));
            if (!pp.isValid()) {
                allValid = false;
                qCritical().noquote() << "       preset failed:" << preset.name;
            }
        }
        CHECK(allValid, "all built-in presets compile");
    }

    qInfo().noquote() << (g_failures == 0 ? "\nALL TESTS PASSED"
                                          : QString("\n%1 TEST(S) FAILED").arg(g_failures));
    return g_failures == 0 ? 0 : 1;
}
