#include "documentstate.h"

#include <QDir>
#include <algorithm>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace {

std::nullopt_t fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return std::nullopt;
}

QString timeToJson(const QDateTime& time)
{
    return time.toUTC().toString(Qt::ISODateWithMs);
}

// Отсутствующая граница — открыта; присутствующая обязана читаться.
bool timeFromJson(const QJsonObject& json, const char* key, QDateTime* out, QString* error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    const QDateTime time = value.isString()
        ? QDateTime::fromString(value.toString(), Qt::ISODateWithMs) : QDateTime();
    if (!time.isValid()) {
        fail(error, QStringLiteral("bad %1").arg(QLatin1String(key)));
        return false;
    }
    *out = time.toLocalTime(); // моменты модели — локальные; сравнение по моменту
    return true;
}

bool boolField(const QJsonObject& json, const char* key, bool* out, QString* error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isBool()) {
        fail(error, QStringLiteral("bad %1").arg(QLatin1String(key)));
        return false;
    }
    *out = value.toBool();
    return true;
}

struct LevelName {
    LogLevel level;
    const char* name;
};
constexpr LevelName kLevelNames[] = {
    { LogLevel::Unknown, "unknown" }, { LogLevel::Trace, "trace" }, { LogLevel::Debug, "debug" },
    { LogLevel::Info, "info" },       { LogLevel::Warn, "warn" },   { LogLevel::Error, "error" },
    { LogLevel::Fatal, "fatal" },
};

} // namespace

// ---------------------------------------------------------------------------
// ViewState
// ---------------------------------------------------------------------------

QString ViewState::levelName(LogLevel level)
{
    for (const LevelName& n : kLevelNames)
        if (n.level == level)
            return QLatin1String(n.name);
    return QStringLiteral("unknown");
}

std::optional<LogLevel> ViewState::levelFromName(QStringView name)
{
    for (const LevelName& n : kLevelNames)
        if (name == QLatin1String(n.name))
            return n.level;
    return std::nullopt;
}

bool ViewState::hasFilters() const
{
    // Правила, из которых ни одно не действует, — не фильтр: предлагать нечего.
    return !levels.isEmpty() || timeFrom.isValid() || timeTo.isValid() || textRules.isActive();
}

QJsonObject ViewState::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("version"), kFormatVersion);
    if (!levels.isEmpty()) {
        // Порядок — по шкале уровней, а не по хэшу множества: файл стабилен.
        QJsonArray names;
        for (const LevelName& n : kLevelNames)
            if (levels.contains(n.level))
                names.append(QLatin1String(n.name));
        json.insert(QStringLiteral("levels"), names);
    }
    if (timeFrom.isValid())
        json.insert(QStringLiteral("timeFrom"), timeToJson(timeFrom));
    if (timeTo.isValid())
        json.insert(QStringLiteral("timeTo"), timeToJson(timeTo));
    if (!textRules.rules.isEmpty())
        json.insert(QStringLiteral("textRules"), textRules.toJson());
    if (!markers.isEmpty())
        json.insert(QStringLiteral("markers"), highlightPatternsToJson(markers));
    if (hasCurrent()) {
        QJsonObject current;
        current.insert(QStringLiteral("source"), currentSource->toJson());
        current.insert(QStringLiteral("record"), this->current.toJson());
        current.insert(QStringLiteral("offset"), viewportOffset);
        current.insert(QStringLiteral("selected"), currentSelected);
        json.insert(QStringLiteral("current"), current);
    }
    json.insert(QStringLiteral("followTail"), followTail);
    json.insert(QStringLiteral("autoReload"), autoReload);
    return json;
}

std::optional<ViewState> ViewState::fromJson(const QJsonObject& json, QString* error)
{
    if (json.value(QLatin1String("version")).toInt(-1) != kFormatVersion)
        return fail(error, QStringLiteral("unsupported view state version"));
    ViewState s;

    const QJsonValue levels = json.value(QLatin1String("levels"));
    if (!levels.isUndefined()) {
        if (!levels.isArray())
            return fail(error, QStringLiteral("bad levels"));
        for (const QJsonValue& v : levels.toArray()) {
            const auto level = v.isString() ? levelFromName(v.toString()) : std::nullopt;
            if (!level)
                return fail(error, QStringLiteral("unknown level %1").arg(v.toString()));
            s.levels.insert(*level);
        }
    }
    if (!timeFromJson(json, "timeFrom", &s.timeFrom, error)
        || !timeFromJson(json, "timeTo", &s.timeTo, error))
        return std::nullopt;

    const QJsonValue rules = json.value(QLatin1String("textRules"));
    if (!rules.isUndefined()) {
        if (!rules.isObject())
            return fail(error, QStringLiteral("bad textRules"));
        s.textRules = FilterRuleSet::fromJson(rules.toObject());
    }
    const QJsonValue markers = json.value(QLatin1String("markers"));
    if (!markers.isUndefined()) {
        if (!markers.isArray())
            return fail(error, QStringLiteral("bad markers"));
        s.markers = highlightPatternsFromJson(markers.toArray());
    }

    const QJsonValue currentValue = json.value(QLatin1String("current"));
    if (!currentValue.isUndefined()) {
        if (!currentValue.isObject())
            return fail(error, QStringLiteral("bad current"));
        const QJsonObject current = currentValue.toObject();
        QString why;
        const auto source = SourceIdentity::fromJson(current.value(QLatin1String("source")).toObject(), &why);
        if (!source)
            return fail(error, QStringLiteral("current source: ") + why);
        const auto record = RecordId::fromJson(current.value(QLatin1String("record")).toObject(), &why);
        if (!record)
            return fail(error, QStringLiteral("current record: ") + why);
        const QJsonValue offset = current.value(QLatin1String("offset"));
        if (!offset.isUndefined() && !offset.isDouble())
            return fail(error, QStringLiteral("bad current offset"));
        s.currentSource = *source;
        s.current = *record;
        s.viewportOffset = offset.toInt(0);
        if (!boolField(current, "selected", &s.currentSelected, error))
            return std::nullopt;
    }
    if (!boolField(json, "followTail", &s.followTail, error)
        || !boolField(json, "autoReload", &s.autoReload, error))
        return std::nullopt;
    return s;
}

// ---------------------------------------------------------------------------
// TabSource
// ---------------------------------------------------------------------------

QJsonObject TabSource::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("source"), source.toJson());
    if (userEncoding)
        json.insert(QStringLiteral("encoding"), TextEncodings::displayName(*userEncoding));
    return json;
}

std::optional<TabSource> TabSource::fromJson(const QJsonObject& json, QString* error)
{
    TabSource s;
    QString why;
    const auto source = SourceIdentity::fromJson(json.value(QLatin1String("source")).toObject(), &why);
    if (!source)
        return fail(error, QStringLiteral("source: ") + why);
    s.source = *source;
    const QJsonValue encoding = json.value(QLatin1String("encoding"));
    if (!encoding.isUndefined()) {
        const auto e = encoding.isString() ? TextEncodings::fromName(encoding.toString())
                                           : std::nullopt;
        if (!e)
            return fail(error, QStringLiteral("unknown encoding %1").arg(encoding.toString()));
        s.userEncoding = *e;
    }
    return s;
}

// ---------------------------------------------------------------------------
// TabState
// ---------------------------------------------------------------------------

QJsonObject TabState::toJson() const
{
    QJsonObject json;
    json.insert(QStringLiteral("format"), QLatin1String(kFormatName));
    json.insert(QStringLiteral("version"), kFormatVersion);
    QJsonArray list;
    for (const TabSource& s : sources)
        list.append(s.toJson());
    json.insert(QStringLiteral("sources"), list);
    QJsonArray marks;
    for (const BookmarkState& b : bookmarks)
        marks.append(b.toJson());
    json.insert(QStringLiteral("bookmarks"), marks);
    json.insert(QStringLiteral("view"), view.toJson());
    return json;
}

std::optional<TabState> TabState::fromJson(const QJsonObject& json, QString* error)
{
    if (json.value(QLatin1String("format")).toString() != QLatin1String(kFormatName))
        return fail(error, QStringLiteral("not a tab state"));
    if (json.value(QLatin1String("version")).toInt(-1) != kFormatVersion)
        return fail(error, QStringLiteral("unsupported tab state version"));
    TabState s;
    QString why;

    const QJsonValue sources = json.value(QLatin1String("sources"));
    if (!sources.isArray())
        return fail(error, QStringLiteral("bad sources"));
    for (const QJsonValue& v : sources.toArray()) {
        const auto source = v.isObject() ? TabSource::fromJson(v.toObject(), &why) : std::nullopt;
        if (!source)
            return fail(error, QStringLiteral("sources: ") + why);
        s.sources.append(*source);
    }

    const QJsonValue bookmarks = json.value(QLatin1String("bookmarks"));
    if (!bookmarks.isUndefined()) {
        if (!bookmarks.isArray())
            return fail(error, QStringLiteral("bad bookmarks"));
        for (const QJsonValue& v : bookmarks.toArray()) {
            const auto b = v.isObject() ? BookmarkState::fromJson(v.toObject(), &why) : std::nullopt;
            if (!b)
                return fail(error, QStringLiteral("bookmarks: ") + why);
            s.bookmarks.append(*b);
        }
    }

    const QJsonValue view = json.value(QLatin1String("view"));
    if (!view.isObject())
        return fail(error, QStringLiteral("bad view"));
    const auto v = ViewState::fromJson(view.toObject(), &why);
    if (!v)
        return fail(error, QStringLiteral("view: ") + why);
    s.view = *v;
    return s;
}

// ---------------------------------------------------------------------------
// SessionState
// ---------------------------------------------------------------------------

bool SessionState::samePath(const QString& a, const QString& b)
{
#ifdef Q_OS_WIN
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

QString SessionState::relativePathOf(const QString& path) const
{
    for (auto it = relativePaths.cbegin(); it != relativePaths.cend(); ++it)
        if (samePath(it.key(), path))
            return it.value();
    return QString();
}

QStringList SessionState::paths() const
{
    QStringList out;
    const auto add = [&out](const QString& path) {
        if (!path.isEmpty() && std::none_of(out.cbegin(), out.cend(),
                                            [&](const QString& p) { return samePath(p, path); }))
            out.append(path);
    };
    for (const TabState& tab : tabs) {
        for (const TabSource& s : tab.sources)
            add(s.source.path);
        for (const BookmarkState& b : tab.bookmarks)
            add(b.source.path);
        if (tab.view.currentSource)
            add(tab.view.currentSource->path);
    }
    return out;
}

QJsonObject SessionState::toJson(const QString& sessionFilePath) const
{
    QJsonObject json;
    json.insert(QStringLiteral("format"), QLatin1String(kFormatName));
    json.insert(QStringLiteral("version"), kFormatVersion);
    if (hasSchema()) {
        QJsonObject s;
        s.insert(QStringLiteral("name"), schemaName);
        const QJsonDocument doc = QJsonDocument::fromJson(schema.toUtf8());
        // Определение — объектом (читается глазами); не JSON — строкой как есть.
        if (doc.isObject())
            s.insert(QStringLiteral("definition"), doc.object());
        else
            s.insert(QStringLiteral("definition"), schema);
        json.insert(QStringLiteral("schema"), s);
    }
    json.insert(QStringLiteral("fieldExtraction"), fieldExtraction);
    if (!visibleFields.isEmpty())
        json.insert(QStringLiteral("visibleFields"), QJsonArray::fromStringList(visibleFields));
    json.insert(QStringLiteral("activeTab"), activeTab);

    QJsonArray files;
    const QString baseDir = sessionFilePath.isEmpty()
        ? QString() : QFileInfo(sessionFilePath).absolutePath();
    for (const QString& path : paths()) {
        QJsonObject f;
        f.insert(QStringLiteral("path"), path);
        if (!baseDir.isEmpty()) {
            const QString rel = QDir(baseDir).relativeFilePath(path);
            // Другой диск на Windows — относительного нет.
            if (!rel.isEmpty() && QDir::isRelativePath(rel))
                f.insert(QStringLiteral("relative"), QDir::fromNativeSeparators(rel));
        }
        files.append(f);
    }
    json.insert(QStringLiteral("files"), files);

    QJsonArray list;
    for (const TabState& tab : tabs)
        list.append(tab.toJson());
    json.insert(QStringLiteral("tabs"), list);
    return json;
}

std::optional<SessionState> SessionState::fromJson(const QJsonObject& json, QString* error,
                                                   bool* newerVersion)
{
    if (newerVersion)
        *newerVersion = false;
    if (json.value(QLatin1String("format")).toString() != QLatin1String(kFormatName))
        return fail(error, QStringLiteral("not a DendroLog session"));
    const QJsonValue versionValue = json.value(QLatin1String("version"));
    if (!versionValue.isDouble())
        return fail(error, QStringLiteral("bad session version"));
    const int version = versionValue.toInt(-1);
    if (version > kFormatVersion) {
        if (newerVersion)
            *newerVersion = true;
        return fail(error, QStringLiteral("session version %1 is newer than %2")
                               .arg(version).arg(kFormatVersion));
    }
    if (version != kFormatVersion)
        return fail(error, QStringLiteral("unsupported session version"));

    SessionState s;
    QString why;

    const QJsonValue schema = json.value(QLatin1String("schema"));
    if (!schema.isUndefined()) {
        if (!schema.isObject())
            return fail(error, QStringLiteral("bad schema"));
        const QJsonObject o = schema.toObject();
        const QJsonValue name = o.value(QLatin1String("name"));
        const QJsonValue def = o.value(QLatin1String("definition"));
        if (!name.isString())
            return fail(error, QStringLiteral("bad schema name"));
        if (def.isObject())
            s.schema = QString::fromUtf8(QJsonDocument(def.toObject()).toJson(QJsonDocument::Compact));
        else if (def.isString() && !def.toString().isEmpty())
            s.schema = def.toString();
        else
            return fail(error, QStringLiteral("bad schema definition"));
        s.schemaName = name.toString();
    }

    if (!boolField(json, "fieldExtraction", &s.fieldExtraction, error))
        return std::nullopt;

    const QJsonValue visible = json.value(QLatin1String("visibleFields"));
    if (!visible.isUndefined()) {
        if (!visible.isArray())
            return fail(error, QStringLiteral("bad visibleFields"));
        for (const QJsonValue& v : visible.toArray()) {
            if (!v.isString())
                return fail(error, QStringLiteral("bad visibleFields"));
            s.visibleFields.append(v.toString());
        }
    }

    const QJsonValue tabs = json.value(QLatin1String("tabs"));
    if (!tabs.isArray())
        return fail(error, QStringLiteral("bad tabs"));
    for (const QJsonValue& v : tabs.toArray()) {
        const auto tab = v.isObject() ? TabState::fromJson(v.toObject(), &why) : std::nullopt;
        if (!tab)
            return fail(error, QStringLiteral("tabs: ") + (v.isObject() ? why : QStringLiteral("not an object")));
        s.tabs.append(*tab);
    }

    const QJsonValue active = json.value(QLatin1String("activeTab"));
    if (!active.isUndefined()) {
        if (!active.isDouble())
            return fail(error, QStringLiteral("bad activeTab"));
        s.activeTab = active.toInt(-1);
        if (s.activeTab < -1 || s.activeTab >= s.tabs.size())
            return fail(error, QStringLiteral("activeTab out of range"));
    }

    const QJsonValue files = json.value(QLatin1String("files"));
    if (!files.isUndefined()) {
        if (!files.isArray())
            return fail(error, QStringLiteral("bad files"));
        for (const QJsonValue& v : files.toArray()) {
            const QJsonObject f = v.toObject();
            const QJsonValue path = f.value(QLatin1String("path"));
            const QJsonValue rel = f.value(QLatin1String("relative"));
            if (!v.isObject() || !path.isString() || (!rel.isUndefined() && !rel.isString()))
                return fail(error, QStringLiteral("bad files"));
            if (rel.isString() && !rel.toString().isEmpty())
                s.relativePaths.insert(path.toString(), rel.toString());
        }
    }
    return s;
}
