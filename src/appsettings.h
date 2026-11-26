#pragma once

#include <QObject>
#include <QStringList>

#include "textencoding.h"
#include "filelabels.h"

#include <optional>

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

// Выбор темы окна (Settings → Appearance). Какая схема при этом действует,
// решает effectiveColorScheme() в colorschemecontroller.h.
enum class ThemeMode {
    System,   // как в системе — следовать теме ОС (по умолчанию)
    Light,
    Dark,
};

class QSettings;

class AppSettings : public QObject
{
    Q_OBJECT

public:
    static AppSettings& instance();

    // Каталог пользовательских данных приложения (см. комментарий класса):
    // либо каталог exe (portable), либо AppConfigLocation. Здесь живут
    // DendroLog.ini и подкаталог patterns/ со схемами полей.
    static QString configDir();
    // Шов для тестов (tests/mainwindow_smoke): все данные приложения — ini,
    // схемы, память файлов, last-session.json — в заданном каталоге, а не в
    // настоящем каталоге пользователя. Можно звать снова (новый каталог для
    // следующего окна); прочитанное раньше в памяти синглтонов (AppSettings,
    // ShortcutManager) остаётся — после смены каталога звать load().
    // Программа его не вызывает: для пользователя поведение не меняется.
    static void overrideConfigDirForTesting(const QString& dir);
    // Корень временных файлов распаковки (F-2, SpoolDir): кэш приложения
    // (SpoolDir::defaultRoot) — и в portable-режиме; при подмене каталога
    // настроек тестом — <configDir>/spool, настоящий кэш тест не трогает.
    static QString spoolRoot();

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

    // How the scanned tree keeps up with the directory after the first scan.
    // Values match DirectoryScanner::RefreshMode (0 = Manual, 1 = Auto, 2 = Off);
    // stored as a plain int so this header stays free of the scanner include.
    int  scanRefreshMode() const;
    void setScanRefreshMode(int mode);

    // Auto-refresh period in seconds (5 – 3600).
    int  scanRefreshIntervalSecs() const;
    void setScanRefreshIntervalSecs(int secs);

    // ---- View preferences ---------------------------------------------- //

    // Whether word-wrap is enabled by default (and remembered across sessions).
    bool wordWrap() const;
    void setWordWrap(bool enabled);
    // Метка файла слитой вкладки (U-23): Name / Color Only / Hidden,
    // [View] fileLabels.
    FileLabelMode fileLabelMode() const { return m_fileLabelMode; }
    void setFileLabelMode(FileLabelMode mode);
    // Ширина колонки метки файла в символах ([View] fileColumnChars);
    // 0 — авто: по самому длинному имени, но не шире kDefaultMaxChars.
    int fileColumnChars() const { return m_fileColumnChars; }
    void setFileColumnChars(int chars);
    // Номера строк в логе (U-23), [View] lineNumbers, по умолчанию выключены.
    bool lineNumbers() const { return m_lineNumbers; }
    void setLineNumbers(bool visible);
    // Колонка дельты времени (F-8), [View] timeDelta, по умолчанию выключена.
    bool timeDelta() const { return m_timeDelta; }
    void setTimeDelta(bool visible);
    // Цвета ANSI в строках лога (F-3), [View] ansiColors, по умолчанию
    // включены. Выключено — коды всё равно сняты, строка раскрашивается как
    // обычная.
    bool ansiColors() const { return m_ansiColors; }
    void setAnsiColors(bool enabled);

    // ---- Theme ---------------------------------------------------------- //

    // Тема окна. В ini — словом: [App] theme=system|light|dark; неизвестное
    // значение читается как System. Цвета вкладки Colors ([Theme]) от выбора
    // не зависят: набор один на обе темы.
    ThemeMode themeMode() const;
    void      setThemeMode(ThemeMode mode);
    static QString   themeModeToString(ThemeMode mode);
    static ThemeMode themeModeFromString(const QString& text);

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

    // Point size for the log view font (kMinFontSize – kMaxFontSize).
    static constexpr int kDefaultFontSize = 10;
    static constexpr int kMinFontSize     = 6;
    static constexpr int kMaxFontSize     = 32;
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

    // ---- Кодировки (F-1) ------------------------------------------------- //

    // Запасная кодировка автоопределения — однобайтная, которую автоматика
    // берёт, когда байты файла не похожи на UTF-8. nullopt — по локали системы
    // (TextEncodings::systemFallback, умолчание). В ini — словом:
    // [App] fallbackEncoding=system | Windows-1251 | IBM866 | KOI8-R |
    // Windows-1252; неизвестное или не однобайтное значение читается как
    // system. Сеттер шлёт fallbackEncodingChanged (вкладки применяют сами:
    // новые проходы и предварительные решения) и settingsChanged.
    std::optional<TextEncoding> fallbackEncodingChoice() const { return m_fallbackEncoding; }
    void setFallbackEncodingChoice(std::optional<TextEncoding> encoding);
    // Действующая запасная: выбранная либо по локали системы.
    TextEncoding fallbackEncoding() const;
    static QString fallbackEncodingToString(std::optional<TextEncoding> encoding);
    static std::optional<TextEncoding> fallbackEncodingFromString(const QString& text);

    // ---- Память файла (R-4) ---------------------------------------------- //

    // Помнить ли состояние файлов между запусками: закладки, маркеры,
    // позицию, фильтры (предложением), ручной выбор кодировки. Выключено —
    // память не читается и не пишется (уже записанное остаётся на диске до
    // «Clear Memory»). В ini — [App] rememberFileState, по умолчанию true.
    bool rememberFileState() const { return m_rememberFileState; }
    void setRememberFileState(bool enabled);
    // Каталог памяти файлов: <configDir>/memory (FileMemoryStore).
    static QString fileMemoryDir();

    // ---- Сессии (R-5) ------------------------------------------------------ //

    // Восстанавливать ли вкладки прошлого сеанса при запуске (первичное окно).
    // Выключено — стартовый экран предлагает ссылку «Restore previous
    // session». В ini — [App] restoreSessionOnStartup, по умолчанию false.
    bool restoreSessionOnStartup() const { return m_restoreSessionOnStartup; }
    void setRestoreSessionOnStartup(bool enabled);
    // Последняя сессия окна: <configDir>/last-session.json.
    static QString lastSessionFile();

    // ---- Быстрый поиск (поле в тулбаре) ----------------------------------- //

    // Переключатели «Aa», «слово целиком», «.*» и история запросов; в ini —
    // группа [QuickSearch]. История — только тексты, свежий первым (правила —
    // QuickSearchHistory). Это состояние поля, а не предпочтение: сеттеры не
    // шлют settingsChanged (иначе каждый запуск поиска переприменял бы
    // настройки ко всем вкладкам). В ini — save() или saveQuickSearch()
    // (только эта группа, сразу после изменения).
    bool quickSearchMatchCase() const { return m_quickSearchMatchCase; }
    void setQuickSearchMatchCase(bool on);
    bool quickSearchWholeWord() const { return m_quickSearchWholeWord; }
    void setQuickSearchWholeWord(bool on);
    bool quickSearchRegex() const { return m_quickSearchRegex; }
    void setQuickSearchRegex(bool on);
    QStringList quickSearchHistory() const { return m_quickSearchHistory; }
    void setQuickSearchHistory(const QStringList& entries);
    void saveQuickSearch();

    // ---- Недавние команды (F-7, File → Open Command Output…) --------------- //
    // [Commands] recent: JSON на команду (CommandHistory, commandspooler.h),
    // свежая первой. Как и история поиска — состояние, а не предпочтение:
    // без settingsChanged; в ini — save() или saveRecentCommands().
    QStringList recentCommands() const { return m_recentCommands; }
    void setRecentCommands(const QStringList& entries) { m_recentCommands = entries; }
    void saveRecentCommands();

signals:
    // Emitted after any call to a set*() method, so consumers (e.g. the
    // directory scanner) can update themselves without each class needing
    // to know every individual preference.
    void settingsChanged();
    // Действующая запасная кодировка сменилась (Settings → General).
    void fallbackEncodingChanged(TextEncoding encoding);

private:
    void writeQuickSearch(QSettings& s) const;
    void writeRecentCommands(QSettings& s) const;
    QStringList m_recentCommands;

    AppSettings()  = default;
    ~AppSettings() = default;
    Q_DISABLE_COPY_MOVE(AppSettings)

    // Default values match the hard-coded defaults previously in MainWindow.
    QStringList m_scanExtensions { QStringLiteral("log"), QStringLiteral("txt") };
    int         m_scanRefreshMode { 0 };   // Manual
    int         m_scanRefreshIntervalSecs { 30 };
    bool        m_wordWrap        { true };
    FileLabelMode m_fileLabelMode { FileLabelMode::Name };
    int m_fileColumnChars { 0 };
    bool        m_lineNumbers     { false };
    bool        m_timeDelta       { false };
    bool        m_ansiColors      { true };
    ThemeMode   m_themeMode       { ThemeMode::System };
    bool        m_autoReload      { false };
    int         m_autoReloadIntervalSecs { 2 };
    QString     m_fontFamily;
    int         m_fontSize        { kDefaultFontSize };
    int         m_indexedThresholdMB { 512 };
    int         m_textCacheBudgetMB { 256 };
    bool        m_rememberFileState { true };
    bool        m_restoreSessionOnStartup { false };
    bool        m_quickSearchMatchCase { false };
    bool        m_quickSearchWholeWord { false };
    bool        m_quickSearchRegex { false };
    QStringList m_quickSearchHistory;
    std::optional<TextEncoding> m_fallbackEncoding; // nullopt — по локали системы
};
