#ifndef SECRETSTORE_H
#define SECRETSTORE_H

#include <QString>

// ============================================================================
// SecretStore — ключ нейро-подсказчика ВНЕ ini (F-17, шаг 5; F-17.md,
// раздел 5). Решение исполнителя (записано в «Сделано → шаг 5»):
//
//   • Windows — DPAPI (CryptProtectData, ключ пользователя Windows, без
//     окна): зашифрованный файл <configDir>/ai-key.dpapi. Расшифровать может
//     только тот же пользователь на той же машине; перенесённый portable-
//     каталог ключ теряет (подсказчик попросит ввести его снова).
//   • Linux и прочие Unix — файл 0600 в каталоге 0700 вне каталога настроек:
//     <AppDataLocation>/ai-key (~/.local/share/…). Ключ в нём открытым
//     текстом, защищён правами — Settings → AI об этом предупреждает.
//     libsecret не используется: новая зависимость, на машинах без сеанса
//     D-Bus (сервер, контейнер, CI) её нет, а запасной путь всё равно нужен.
//   • Portable-режим на Unix — ключ не сохраняется (каталог exe переносят на
//     флешке): только переменная окружения или ключ на этот запуск (в
//     памяти процесса).
//   • Переменная окружения ANTHROPIC_API_KEY главнее сохранённого ключа.
//
// Ключ не пишется в ini, сессии, память файлов и журналы; в сообщениях об
// ошибках его нет. Чтение и запись — на GUI-потоке (файл в сотню байт).
// ============================================================================
class SecretStore {
public:
    enum class Kind {
        Unavailable,      ///< ключ не сохраняется (portable на Unix)
        SystemProtected,  ///< DPAPI (Windows)
        PrivateFile,      ///< файл 0600 (Unix)
    };

    /// Хранилище в каталоге dir (шов для теста; программа зовёт forApplication).
    SecretStore(const QString& dir, Kind kind);
    /// Хранилище приложения по правилам выше.
    static SecretStore forApplication();
    /// Вид хранилища этой платформы (SystemProtected на Windows, иначе PrivateFile).
    static Kind platformKind();

    Kind kind() const { return m_kind; }
    bool isAvailable() const { return m_kind != Kind::Unavailable; }
    QString path() const { return m_path; }
    /// Где и как хранится — словами для Settings → AI.
    QString description() const;

    bool hasKey() const;
    /// false — нет ключа или не читается (error, без ключа в тексте).
    bool readKey(QString* key, QString* error = nullptr) const;
    bool writeKey(const QString& key, QString* error = nullptr);
    bool removeKey(QString* error = nullptr);

    /// ANTHROPIC_API_KEY (пусто — не задана).
    static QString environmentKey();
    /// Ключ на этот запуск (Settings → AI, когда сохранить нельзя или не
    /// хочется): только память процесса.
    static void setSessionKey(const QString& key);
    static QString sessionKey();

    enum class Source { None, Environment, Stored, Session };
    /// Действующий ключ: переменная окружения, затем сохранённый, затем ключ
    /// на этот запуск.
    static QString effectiveKey(const SecretStore& store, Source* source = nullptr);

private:
    QString m_dir;
    QString m_path;
    Kind m_kind = Kind::Unavailable;
};

#endif // SECRETSTORE_H
