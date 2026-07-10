#include "updatechecker.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace {
// Единственное место, где зашит адрес репозитория.
const QLatin1String kOwnerRepo("scaremongr/dendrolog");
}

UpdateChecker::UpdateChecker(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    // GitHub отвечает 301 при переименовании репозитория — следуем молча.
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

QString UpdateChecker::repoUrl()
{
    return QStringLiteral("https://github.com/") + kOwnerRepo;
}

QString UpdateChecker::releasesUrl()
{
    return repoUrl() + QStringLiteral("/releases");
}

void UpdateChecker::check()
{
    if (m_busy)
        return;
    m_busy = true;

    QNetworkRequest req(QUrl(QStringLiteral("https://api.github.com/repos/") +
                             kOwnerRepo + QStringLiteral("/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    req.setTransferTimeout(10000);

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        m_busy = false;

        if (reply->error() != QNetworkReply::NoError) {
            emit checkFailed(reply->errorString());
            return;
        }

        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        QString tag = obj.value(QStringLiteral("tag_name")).toString();
        if (tag.startsWith(QLatin1Char('v')))
            tag.remove(0, 1);

        const QVersionNumber latest  = QVersionNumber::fromString(tag);
        const QVersionNumber current =
            QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if (latest.isNull()) {
            // Релизов ещё нет или tag_name неожиданного формата.
            emit checkFailed(QStringLiteral("no releases found"));
            return;
        }

        if (latest > current) {
            const QString url = obj.value(QStringLiteral("html_url")).toString();
            emit updateAvailable(tag, url.isEmpty() ? releasesUrl() : url);
        } else {
            emit upToDate();
        }
    });
}
