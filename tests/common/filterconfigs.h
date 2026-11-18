#ifndef FILTERCONFIGS_H
#define FILTERCONFIGS_H

// ============================================================================
// filterconfigs.h — набор конфигураций фильтров для табличных тестов
// хранилища: 21 именованная (уровни, время, текст, колонки, маркеры,
// комбинации) и случайные по фиксированному seed. Вместе — «33 конфигурации»
// logstore_equivalence; их же прогоняет tests/bookmarks (закладки переживают
// каждую).
// ============================================================================

#include "testdocuments.h"

#include "filterruleset.h"
#include "textmatchhighlighter.h"

#include <QColor>
#include <QRandomGenerator>
#include <QSet>

namespace testdocs {

struct FilterConfig {
    QString                   name;
    QSet<LogLevel>            levels;              // пустое множество = без фильтра
    QDateTime                 start;
    QDateTime                 end;
    FilterRuleSet             rules;
    bool                      fieldScopeActive = false;
    bool                      fieldFilterEnabled = false;
    QVector<int>              visibleFields;
    QVector<HighlightPattern> markers;
};

inline FilterRule rule(FilterRule::Action action, const QString& text,
                       FilterRule::Connector connector = FilterRule::Connector::And,
                       const QString& field = QString(),
                       bool isRegex = false, bool caseSensitive = false)
{
    FilterRule r;
    r.action         = action;
    r.text           = text;
    r.connector      = connector;
    r.fieldName      = field;
    r.isRegex        = isRegex;
    r.caseSensitive  = caseSensitive;
    return r;
}

inline QVector<FilterConfig> namedConfigs(const QDateTime& from, const QDateTime& to)
{
    QVector<FilterConfig> cfgs;

    const qint64 a = from.isValid() ? from.toMSecsSinceEpoch() : 0;
    const qint64 b = to.isValid() ? to.toMSecsSinceEpoch() : 0;
    const QDateTime midLow  = QDateTime::fromMSecsSinceEpoch(a + (b - a) / 3);
    const QDateTime midHigh = QDateTime::fromMSecsSinceEpoch(a + 2 * (b - a) / 3);

    {
        FilterConfig c;
        c.name = QStringLiteral("без фильтров");
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: пустое множество (= без фильтра)");
        c.levels = {};
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: Error+Fatal");
        c.levels = { LogLevel::Error, LogLevel::Fatal };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: только Info");
        c.levels = { LogLevel::Info };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("уровни: Unknown (continuation/свободный текст)");
        c.levels = { LogLevel::Unknown };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: средняя треть");
        c.start = midLow;
        c.end   = midHigh;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: весь диапазон");
        c.start = from;
        c.end   = to;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name  = QStringLiteral("время: пустое окно в будущем");
        c.start = QDateTime::fromMSecsSinceEpoch(b + 3600000);
        c.end   = QDateTime::fromMSecsSinceEpoch(b + 7200000);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Include «timeout»");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("timeout"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Include «Timeout» с учётом регистра");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"),
                              FilterRule::Connector::And, QString(), false, true);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Exclude «at com.example»");
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("at com.example"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: регекс «node \\d+»");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("node \\d+"),
                              FilterRule::Connector::And, QString(), true);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Timeout AND Disk");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"))
                      << rule(FilterRule::Action::Include, QStringLiteral("Disk"),
                              FilterRule::Connector::And);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: Timeout OR Кэш");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Timeout"))
                      << rule(FilterRule::Action::Include, QStringLiteral("Кэш"),
                              FilterRule::Connector::Or);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("текст: A AND B OR C (приоритет AND)");
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("Disk"))
                      << rule(FilterRule::Action::Include, QStringLiteral("percent"),
                              FilterRule::Connector::And)
                      << rule(FilterRule::Action::Include, QStringLiteral("Checkpoint"),
                              FilterRule::Connector::Or);
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name             = QStringLiteral("текст: правило привязано к колонке Thread");
        c.fieldScopeActive = true;
        c.rules.rules << rule(FilterRule::Action::Include, QStringLiteral("worker-2"),
                              FilterRule::Connector::And, QStringLiteral("Thread"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name             = QStringLiteral("текст: колонка Message, Exclude");
        c.fieldScopeActive = true;
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("Retrying"),
                              FilterRule::Connector::And, QStringLiteral("Message"));
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name               = QStringLiteral("колонки: показаны только Level и Message");
        c.fieldFilterEnabled = true;
        c.visibleFields      = { 2, 3 };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name               = QStringLiteral("колонки: только Timestamp + уровни Warn/Error");
        c.fieldFilterEnabled = true;
        c.visibleFields      = { 0 };
        c.levels             = { LogLevel::Warn, LogLevel::Error };
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name = QStringLiteral("маркеры строк");
        HighlightPattern p1;
        p1.text  = QStringLiteral("Timeout");
        p1.color = QColor(200, 60, 60);
        HighlightPattern p2;
        p2.text            = QStringLiteral("checkpoint \\d+");
        p2.color           = QColor(60, 140, 200);
        p2.isRegex         = true;
        c.markers << p1 << p2;
        cfgs << c;
    }
    {
        FilterConfig c;
        c.name   = QStringLiteral("комбинация: уровни + время + текст");
        c.levels = { LogLevel::Info, LogLevel::Warn, LogLevel::Error };
        c.start  = midLow;
        c.end    = midHigh;
        c.rules.rules << rule(FilterRule::Action::Exclude, QStringLiteral("Retrying"));
        cfgs << c;
    }

    return cfgs;
}

// Случайные наборы правил по фиксированному seed: перебирает комбинации,
// которые вручную никто бы не выписал.
inline QVector<FilterConfig> randomConfigs(int count, quint32 seed,
                                           const QDateTime& from, const QDateTime& to)
{
    QRandomGenerator rng(seed);
    QVector<FilterConfig> cfgs;

    const QStringList needles = {
        QStringLiteral("Timeout"), QStringLiteral("Disk"),  QStringLiteral("node"),
        QStringLiteral("Кэш"),     QStringLiteral("worker"), QStringLiteral("INFO"),
        QStringLiteral("at com"),  QStringLiteral("percent"),
    };
    const QStringList regexes = {
        QStringLiteral("\\d{3}"), QStringLiteral("Module\\d"),
        QStringLiteral("(Timeout|Disk)"), QStringLiteral("^\\s+at"),
    };
    const QStringList fields = { QString(), QStringLiteral("Thread"),
                                 QStringLiteral("Level"), QStringLiteral("Message") };
    const QVector<LogLevel> allLevels = { LogLevel::Unknown, LogLevel::Trace,
                                          LogLevel::Debug,   LogLevel::Info,
                                          LogLevel::Warn,    LogLevel::Error,
                                          LogLevel::Fatal };

    const qint64 a = from.isValid() ? from.toMSecsSinceEpoch() : 0;
    const qint64 b = to.isValid() ? to.toMSecsSinceEpoch() : 0;

    for (int i = 0; i < count; ++i) {
        FilterConfig c;
        c.name = QStringLiteral("случайная #%1").arg(i);

        const int ruleCount = 1 + int(rng.bounded(3));
        for (int r = 0; r < ruleCount; ++r) {
            const bool isRegex = rng.bounded(100) < 30;
            c.rules.rules << rule(
                rng.bounded(100) < 70 ? FilterRule::Action::Include
                                      : FilterRule::Action::Exclude,
                isRegex ? regexes[int(rng.bounded(quint32(regexes.size())))]
                        : needles[int(rng.bounded(quint32(needles.size())))],
                rng.bounded(100) < 60 ? FilterRule::Connector::And
                                      : FilterRule::Connector::Or,
                fields[int(rng.bounded(quint32(fields.size())))],
                isRegex,
                rng.bounded(100) < 40);
        }
        c.fieldScopeActive = rng.bounded(100) < 60;

        if (rng.bounded(100) < 50) {
            for (const LogLevel lvl : allLevels)
                if (rng.bounded(100) < 40)
                    c.levels.insert(lvl);
        }
        if (rng.bounded(100) < 40 && b > a) {
            const qint64 s = a + qint64(rng.bounded(quint32(qMax<qint64>(1, b - a))));
            const qint64 e = s + qint64(rng.bounded(quint32(qMax<qint64>(1, b - s))));
            c.start = QDateTime::fromMSecsSinceEpoch(s);
            c.end   = QDateTime::fromMSecsSinceEpoch(e);
        }
        if (rng.bounded(100) < 30) {
            c.fieldFilterEnabled = true;
            for (int f = 0; f < 4; ++f)
                if (rng.bounded(100) < 50)
                    c.visibleFields << f;
            if (c.visibleFields.isEmpty())
                c.visibleFields << 3;
        }
        cfgs << c;
    }
    return cfgs;
}

// Число случайных конфигураций и seed, с которыми их прогоняют тесты.
constexpr int kRandomConfigCount = 12;
constexpr quint32 kRandomConfigSeed = 20260917u;

// Все конфигурации: именованные плюс случайные.
inline QVector<FilterConfig> allConfigs(const QDateTime& from, const QDateTime& to)
{
    return namedConfigs(from, to) + randomConfigs(kRandomConfigCount, kRandomConfigSeed, from, to);
}

// Поставить конфигурацию в модель (правила привязываются к колонкам схемы) и
// дождаться окончания фильтра; false — модель не успокоилась.
inline bool applyConfig(LogModel& model, const FilterConfig& cfg, const QStringList& fieldNames)
{
    model.setAvailableFields(fieldNames);
    model.setFieldDisplaySelection(cfg.fieldFilterEnabled, cfg.visibleFields);

    FilterRuleSet rules = cfg.rules;
    rules.bindFields(fieldNames, cfg.fieldScopeActive);

    model.setLogLevelFilter(cfg.levels);
    model.setTimeRangeFilter(cfg.start, cfg.end);
    model.setFilterRules(rules);
    model.setRowMarkers(cfg.markers);
    return settle(model);
}

} // namespace testdocs

#endif // FILTERCONFIGS_H
