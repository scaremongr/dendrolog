#pragma once

#include <QObject>
#include <QStringList>

// ---------------------------------------------------------------------------
// AppSettings — application-level preferences singleton.
//
// Responsibilities:
//   • Owns every preference that can be changed via the Settings dialog.
//   • Persists to the same INI file that MainWindow uses (DendroLog.ini),
//     under its own [App] group so there is no key-name collision.
//   • Emits settingsChanged() after any setter so interested objects can
//     react without polling or knowing which field was modified.
//
// Where the INI lives (configDir()):
//   • Portable mode — рядом с exe, если там лежит файл-маркер "portable"
//     либо уже существующий DendroLog.ini / LogViewer.ini (наследие старых
//     версий). Так работает portable-ZIP и dev-сборка.
//   • Иначе — стандартный пользовательский каталог настроек
//     (QStandardPaths::AppConfigLocation, например %LOCALAPPDATA%/DendroLog):
//     установленное в Program Files приложение не имеет прав писать рядом
//     с собой.
//
// Lifetime / thread safety:
//   • Must be used from the main thread only (same as all QObject children).
//   • The Meyers-singleton instance is created on first access and lives
//     until program exit.
//
// Usage:
//   AppSettings::instance().load();           // once, early in main()
//   AppSettings::instance().scanExtensions(); // read anywhere
//   AppSettings::instance().save();           // on close or after dialog
// ---------------------------------------------------------------------------

class AppSettings : public QObject
{
    Q_OBJECT

public:
    static AppSettings& instance();

    // Каталог пользовательских данных приложения (см. комментарий класса):
    // либо каталог exe (portable), либо AppConfigLocation. Здесь живут
    // DendroLog.ini и подкаталог patterns/ со схемами полей.
    static QString configDir();

    // Path of the shared INI file (also used by MainWindow and the
    // pattern editor for presets / sample text).
    static QString iniFilePath();

    // Load preferences from the INI file.
    // Safe to call multiple times; later calls overwrite in-memory state.
    void load();

    // Persist current in-memory preferences to the INI file.
    void save();

    // ---- Scanner preferences ------------------------------------------- //

    // Extensions without the leading dot, e.g. {"log", "txt"}.
    QStringList scanExtensions() const;
    void        setScanExtensions(const QStringList& extensions);

    // ---- View preferences ---------------------------------------------- //

    // Whether word-wrap is enabled by default (and remembered across sessions).
    bool wordWrap() const;
    void setWordWrap(bool enabled);

    // ---- File reload preferences --------------------------------------- //

    // Whether loaded files are polled for changes automatically.
    bool autoReload() const;
    void setAutoReload(bool enabled);

    // Polling interval in seconds (1 – 60).
    int  autoReloadIntervalSecs() const;
    void setAutoReloadIntervalSecs(int secs);

    // ---- Font preferences ---------------------------------------------- //

    // Family name of the monospaced font used in the log view.
    // Empty string means "not yet configured" — load() sets a first-run default.
    QString fontFamily() const;
    void    setFontFamily(const QString& family);

    // Point size for the log view font.
    int  fontSize() const;
    void setFontSize(int size);

    // ---- Performance (большие файлы) ------------------------------------ //

    // Порог индексного бэкенда, МБ: файлы >= порога открываются через
    // LineIndex (текст на диске). 0 — индексировать всегда (для тестов).
    int  indexedThresholdMB() const;
    void setIndexedThresholdMB(int mb);
    qint64 indexedThresholdBytes() const;

    // Бюджет кэша текста индексного бэкенда, МБ (64–2048).
    int  textCacheBudgetMB() const;
    void setTextCacheBudgetMB(int mb);

signals:
    // Emitted after any call to a set*() method, so consumers (e.g. the
    // directory scanner) can update themselves without each class needing
    // to know every individual preference.
    void settingsChanged();

private:
    AppSettings()  = default;
    ~AppSettings() = default;
    Q_DISABLE_COPY_MOVE(AppSettings)

    // Default values match the hard-coded defaults previously in MainWindow.
    QStringList m_scanExtensions { QStringLiteral("log"), QStringLiteral("txt") };
    bool        m_wordWrap        { true };
    bool        m_autoReload      { false };
    int         m_autoReloadIntervalSecs { 2 };
    QString     m_fontFamily;
    int         m_fontSize        { 10 };
    int         m_indexedThresholdMB { 512 };
    int         m_textCacheBudgetMB { 256 };
};
