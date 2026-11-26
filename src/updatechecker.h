#pragma once

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

// ---------------------------------------------------------------------------
// UpdateChecker — проверка новых релизов через GitHub Releases API.
//
// Дёргает GET /repos/<owner>/<repo>/releases/latest, сравнивает tag_name
// (формат "vX.Y.Z") с версией приложения и сообщает результат сигналами.
// Сетевые ошибки и rate-limit НЕ считаются «новой версии нет» — о них
// сообщает checkFailed(), чтобы ручная проверка могла показать ошибку,
// а фоновая — молча промолчать.
//
// Владелец решает, что делать с результатом (диалог, статус-бар и т.п.).
// ---------------------------------------------------------------------------
class UpdateChecker : public QObject
{
    Q_OBJECT

public:
    explicit UpdateChecker(QObject* parent = nullptr);

    // Репозиторий, на который указывают проверка и страница релизов.
    static QString repoUrl();      // https://github.com/<owner>/<repo>
    static QString releasesUrl();  // …/releases

    // Асинхронная проверка; результат придёт одним из сигналов ниже.
    // Повторный вызов во время активной проверки игнорируется.
    void check();

    // Шов для тестов (tests/mainwindow_smoke): без сети. check() тогда не
    // ходит на GitHub, а отвечает checkFailed (асинхронно, как настоящий
    // ответ). Программа его не вызывает.
    static void disableNetworkForTesting();

signals:
    void updateAvailable(const QString& newVersion, const QString& releaseUrl);
    void upToDate();
    void checkFailed(const QString& error);

private:
    void onReplyFinished(QNetworkReply* reply);

    QNetworkAccessManager* m_nam = nullptr;
    bool m_busy = false;
};
