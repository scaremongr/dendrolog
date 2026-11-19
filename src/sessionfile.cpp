#include "sessionfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace SessionFile {

bool save(const QString& filePath, const SessionState& state, QString* error)
{
    const QString absolute = QFileInfo(filePath).absoluteFilePath();
    QSaveFile file(absolute);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(state.toJson(absolute)).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

LoadResult load(const QString& filePath)
{
    LoadResult result;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("not a JSON object: %1").arg(parseError.errorString());
        return result;
    }
    result.state = SessionState::fromJson(doc.object(), &result.error, &result.newerVersion);
    return result;
}

namespace {

// Идентичность, по которой проверяется путь: файла вкладки, иначе закладки
// или текущей строки (файл пропал из вкладки до снятия).
std::optional<SourceIdentity> identityOf(const SessionState& state, const QString& path)
{
    for (const TabState& tab : state.tabs)
        for (const TabSource& s : tab.sources)
            if (SessionState::samePath(s.source.path, path))
                return s.source;
    for (const TabState& tab : state.tabs) {
        for (const BookmarkState& b : tab.bookmarks)
            if (SessionState::samePath(b.source.path, path))
                return b.source;
        if (tab.view.currentSource && SessionState::samePath(tab.view.currentSource->path, path))
            return *tab.view.currentSource;
    }
    return std::nullopt;
}

void replacePath(SessionState& state, const QString& from, const QString& to)
{
    for (TabState& tab : state.tabs) {
        for (TabSource& s : tab.sources)
            if (SessionState::samePath(s.source.path, from))
                s.source.path = to;
        for (BookmarkState& b : tab.bookmarks)
            if (SessionState::samePath(b.source.path, from))
                b.source.path = to;
        if (tab.view.currentSource && SessionState::samePath(tab.view.currentSource->path, from))
            tab.view.currentSource->path = to;
    }
}

} // namespace

QVector<Resolution> rebind(SessionState& state, const QString& sessionFilePath)
{
    QVector<Resolution> out;
    const QDir baseDir(QFileInfo(sessionFilePath).absolutePath());
    // Сначала решить всё по исходным путям, потом подменять: иначе новый
    // путь одного файла совпал бы с сохранённым путём другого.
    QVector<QPair<QString, QString>> renames;
    for (const QString& saved : state.paths()) {
        Resolution r;
        r.savedPath = saved;
        r.path = saved;
        const auto identity = identityOf(state, saved);
        const SourceIdentity::State atSaved = identity ? identity->checkAt(saved)
                                                       : SourceIdentity::State::Missing;
        QString candidate;
        const QString rel = state.relativePathOf(saved);
        if (!rel.isEmpty())
            candidate = QDir::cleanPath(baseDir.absoluteFilePath(rel));
        const bool distinct = !candidate.isEmpty() && !SessionState::samePath(candidate, saved);
        const SourceIdentity::State atRelative = (identity && distinct)
            ? identity->checkAt(candidate) : SourceIdentity::State::Missing;

        if (atSaved == SourceIdentity::State::Valid) {
            r.kind = Resolution::Kind::Valid;
        } else if (atRelative == SourceIdentity::State::Valid) {
            r.kind = Resolution::Kind::Relocated;
            r.path = candidate;
        } else if (atSaved == SourceIdentity::State::Changed) {
            r.kind = Resolution::Kind::Changed;
        } else if (atRelative == SourceIdentity::State::Changed) {
            r.kind = Resolution::Kind::Changed;
            r.path = candidate;
        } else {
            r.kind = Resolution::Kind::Missing;
        }
        if (r.path != saved)
            renames.append({ saved, r.path });
        out.append(r);
    }
    // Подмена через временные метки: цепочка a→b, b→c не сливает файлы.
    for (int i = 0; i < renames.size(); ++i)
        replacePath(state, renames[i].first, QStringLiteral("\x01session-rebind-%1").arg(i));
    for (int i = 0; i < renames.size(); ++i)
        replacePath(state, QStringLiteral("\x01session-rebind-%1").arg(i), renames[i].second);
    return out;
}

QString withSuffix(const QString& filePath)
{
    return isSessionFile(filePath)
        ? filePath : filePath + QLatin1Char('.') + QLatin1String(SessionState::kFileSuffix);
}

bool isSessionFile(const QString& filePath)
{
    return QFileInfo(filePath).suffix().compare(QLatin1String(SessionState::kFileSuffix),
                                                Qt::CaseInsensitive) == 0;
}

} // namespace SessionFile
