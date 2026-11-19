#include "sessioncontroller.h"

#include "logviewwidget.h"

#include <algorithm>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

SessionController::SessionController(QObject* parent)
    : QObject(parent)
{
}

SessionState SessionController::capture(const QList<LogViewWidget*>& tabs, LogViewWidget* active) const
{
    SessionState session;
    for (LogViewWidget* tab : tabs) {
        if (!tab)
            continue;
        TabState state = tab->captureState();
        // Пропавший при открытии сессии и так и не указанный источник —
        // снова в снимок: пересохранённая сессия его не теряет.
        for (const ReportEntry& e : m_entries) {
            if (e.tab != tab || e.kind != ReportEntry::Kind::Missing)
                continue;
            const bool present = std::any_of(state.sources.cbegin(), state.sources.cend(),
                [&e](const TabSource& s) { return LogViewWidget::samePath(s.source.path, e.path); });
            if (!present)
                state.sources.append(e.source);
        }
        // Вкладка stdin: спул не снимается — пустая вкладка в сессию не идёт.
        if (state.sources.isEmpty() && state.bookmarks.isEmpty())
            continue;
        if (tab == active)
            session.activeTab = int(session.tabs.size());
        session.tabs.append(state);
    }
    return session;
}

SessionController::OpenResult SessionController::open(SessionState state, const QString& sessionFilePath,
                                                      const std::function<LogViewWidget*()>& newTab)
{
    OpenResult result;
    prune();
    const QVector<SessionFile::Resolution> resolutions = SessionFile::rebind(state, sessionFilePath);
    const auto resolutionOf = [&resolutions](const QString& path) -> const SessionFile::Resolution* {
        for (const SessionFile::Resolution& r : resolutions)
            if (SessionState::samePath(r.path, path))
                return &r;
        return nullptr;
    };

    for (int i = 0; i < state.tabs.size(); ++i) {
        const TabState& tab = state.tabs[i];
        LogViewWidget* view = newTab ? newTab() : nullptr;
        if (!view)
            continue;
        result.tabs.append(view);
        if (i == state.activeTab)
            result.active = view;
        connect(view, &QObject::destroyed, this, [this](QObject* gone) {
            // QPointer записи в этот момент может быть ещё не обнулён, а приводить
            // уничтожаемый объект к LogViewWidget нельзя (UBSan) — сравнение по QObject*.
            m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                           [gone](const ReportEntry& e) {
                                               return e.tab.isNull() || e.tabObject == gone;
                                           }),
                            m_entries.end());
            emit reportChanged();
        });

        QStringList names;
        for (const TabSource& s : tab.sources)
            names << QFileInfo(s.source.path).fileName();
        const QString title = QStringLiteral("%1: %2").arg(i + 1).arg(names.join(QStringLiteral(", ")));

        const LogViewWidget::StateApplyReport applied =
            view->applyState(tab, LogViewWidget::StateFilters::Apply);
        for (const TabSource& s : tab.sources) {
            const QString& path = s.source.path;
            const SessionFile::Resolution* r = resolutionOf(path);
            ReportEntry e;
            e.tab = view;
            e.tabObject = view;
            e.tabTitle = title;
            e.savedPath = r ? r->savedPath : path;
            e.path = path;
            e.source = s;
            if (applied.missingSources.contains(path)) {
                e.kind = ReportEntry::Kind::Missing;
                ++result.missing;
            } else if (applied.changedSources.contains(path)) {
                e.kind = ReportEntry::Kind::Changed;
                ++result.changed;
            } else if (r && r->kind == SessionFile::Resolution::Kind::Relocated) {
                e.kind = ReportEntry::Kind::Relocated;
                ++result.relocated;
            } else {
                continue;
            }
            m_entries.append(e);
        }
    }
    if (!result.active && !result.tabs.isEmpty())
        result.active = result.tabs.first();
    emit reportChanged();
    return result;
}

void SessionController::prune()
{
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [](const ReportEntry& e) { return e.tab.isNull(); }),
                    m_entries.end());
}

QVector<SessionController::ReportEntry> SessionController::report() const
{
    QVector<ReportEntry> alive;
    for (const ReportEntry& e : m_entries)
        if (!e.tab.isNull())
            alive.append(e);
    return alive;
}

QString SessionController::describe(const ReportEntry& entry)
{
    const QString where = QDir::toNativeSeparators(entry.path);
    switch (entry.kind) {
    case ReportEntry::Kind::Missing:
        return QCoreApplication::translate("SessionController",
            "File not found: not opened. Its bookmarks wait for it — use Locate File… "
            "to point at the file.");
    case ReportEntry::Kind::Changed:
        return QCoreApplication::translate("SessionController",
            "The file changed since the session was saved: opened from scratch, its "
            "bookmarks are shown grey and the position is not restored.");
    case ReportEntry::Kind::Relocated:
        return QCoreApplication::translate("SessionController",
            "Found next to the session file: %1").arg(where);
    case ReportEntry::Kind::Located:
        return QCoreApplication::translate("SessionController", "Opened from %1").arg(where);
    }
    return QString();
}

QString SessionController::summary(const OpenResult& result)
{
    QString text = QCoreApplication::translate("SessionController", "Session opened: %n tab(s).",
                                               nullptr, int(result.tabs.size()));
    if (result.missing)
        text += QLatin1Char(' ') + QCoreApplication::translate("SessionController",
                    "%n file(s) not found.", nullptr, result.missing);
    if (result.changed)
        text += QLatin1Char(' ') + QCoreApplication::translate("SessionController",
                    "%n file(s) changed.", nullptr, result.changed);
    if (result.relocated)
        text += QLatin1Char(' ') + QCoreApplication::translate("SessionController",
                    "%n file(s) found next to the session file.", nullptr, result.relocated);
    return text;
}

bool SessionController::locate(int index, const QString& filePath, QString* error)
{
    const auto fail = [error](const QString& text) {
        if (error)
            *error = text;
        return false;
    };
    int alive = -1;
    ReportEntry* entry = nullptr;
    for (ReportEntry& e : m_entries) {
        if (!e.tab.isNull() && ++alive == index) {
            entry = &e;
            break;
        }
    }
    if (!entry || entry->kind != ReportEntry::Kind::Missing)
        return fail(QCoreApplication::translate("SessionController", "Nothing to locate here."));
    const QString path = LogViewWidget::normalizedPath(filePath);
    switch (entry->source.source.checkAt(path)) {
    case SourceIdentity::State::Valid:
        break;
    case SourceIdentity::State::Missing:
        return fail(QCoreApplication::translate("SessionController", "%1 cannot be read.")
                        .arg(QDir::toNativeSeparators(path)));
    default:
        return fail(QCoreApplication::translate("SessionController",
            "%1 is not the file saved in the session: its beginning or its saved part differs.")
                        .arg(QDir::toNativeSeparators(path)));
    }
    TabSource source = entry->source;
    source.source.path = path;
    const QString oldPath = entry->path;
    entry->kind = ReportEntry::Kind::Located;
    entry->path = path;
    entry->source = source;
    entry->tab->addRelocatedSource(oldPath, source);
    emit reportChanged();
    return true;
}

SessionController::SchemaChoice SessionController::chooseSchema(const SchemaStore::SchemaList& list,
                                                                const QString& name, const QString& schema)
{
    SchemaChoice choice;
    if (schema.isEmpty())
        return choice;
    const QJsonDocument wanted = QJsonDocument::fromJson(schema.toUtf8());
    for (int i = 0; i < list.size(); ++i) {
        const QString& have = list[i].schema;
        if (have == schema
            || (wanted.isObject() && QJsonDocument::fromJson(have.toUtf8()) == wanted)) {
            choice.index = i;
            return choice;
        }
    }
    const QString base = name.isEmpty()
        ? QCoreApplication::translate("SessionController", "Session schema")
        : QCoreApplication::translate("SessionController", "%1 (from session)").arg(name);
    const auto taken = [&list](const QString& candidate) {
        return std::any_of(list.cbegin(), list.cend(), [&](const SchemaStore::SchemaEntry& e) {
            return e.name.compare(candidate, Qt::CaseInsensitive) == 0;
        });
    };
    QString unique = base;
    for (int n = 2; taken(unique); ++n)
        unique = QStringLiteral("%1 %2").arg(base).arg(n);
    choice.newEntry.name = unique;
    choice.newEntry.schema = schema;
    choice.index = int(list.size());
    choice.added = true;
    return choice;
}
