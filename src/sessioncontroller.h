#ifndef SESSIONCONTROLLER_H
#define SESSIONCONTROLLER_H

#include "documentstate.h"
#include "schemastore.h"
#include "sessionfile.h"

#include <QObject>
#include <QPointer>
#include <QVector>

#include <functional>

class LogViewWidget;

// ============================================================================
// SessionController — сессии без собственных виджетов (R-5, шаг 1;
// docs/design/R-5.md). Окно только зовёт его: пункты меню, диалоги выбора
// файла, схема окна и строка состояния — в MainWindow, логика — здесь.
//
//   • capture — снять сессию с набора вкладок: TabState каждой (несколько
//     файлов во вкладке сохраняются), активная вкладка. Вкладка stdin в
//     сессию не попадает (captureState не снимает спул; вкладка, у которой не
//     осталось ни файлов, ни закладок, пропускается), слитая «stdin + файл»
//     сохраняет файл. Источник, пропавший при открытии сессии и так и не
//     указанный, снимается снова: пересохранённая сессия его не теряет, и
//     его относительный путь ведёт туда же, куда вёл прежний файл сессии
//     (SessionState::relativeTargets) — в том числе у пути другой ОС (R-8).
//     Схема, флаг и выбор колонок — у каждой вкладки (F-5, ViewState.fields);
//     верхний уровень сессии — схема активной вкладки (её читает 0.6).
//   • open — открыть: перепривязка путей (SessionFile::rebind: абсолютный,
//     затем относительный к файлу сессии), затем вкладки в порядке файла
//     сессии — newTab() и applyState(tab, Apply) до файлов: фильтры,
//     маркеры, закладки, позиция, follow-tail, авто-обновление, выбор
//     кодировки. Схему вкладки ставит её applyState ДО фильтров (сигнал
//     savedFieldSettingsFound → FieldSchemaController); вкладка без своей
//     схемы (сессия 0.6) получает схему верхнего уровня. Уже открытые вкладки не трогаются — сессия добавляется.
//     Память файла дополняет снимок только закладками («снимок главнее»).
//   • отчёт о восстановлении — что открыто не так, как сохранено: файла нет
//     (вкладка его не открыла, закладки ждут), файл изменился (открыт чистым,
//     закладки серые), найден по относительному пути. Ничего не спрашивается
//     модально: окно показывает summary() в строке состояния со ссылкой на
//     отчёт (SessionReportDialog).
//   • locate — «указать файл» для пропавшего источника: принимается только
//     путь, где источник цел (SourceIdentity::checkAt == Valid); вкладка
//     открывает его, закладки переезжают (LogViewWidget::addRelocatedSource).
//   • chooseSchema — схема сессии против каталога схем: совпадающее
//     определение (имя не важно) — она; иначе добавить под именем с
//     пометкой «(from session)», при занятом имени — с номером.
//
// Шаг 2 (docs/design/R-5.md, «Восстановление вкладок и Ctrl+Shift+T»,
// «Portable-режим и несколько экземпляров»):
//   • сессия окна — setWindow даёт контроллеру вкладки окна по порядку,
//     активную и схему с колонками; captureWindow снимает всё вместе (Save
//     Session As и последняя сессия);
//   • последняя сессия — <configDir>/last-session.json (setLastSessionFile).
//     Пишет и читает только первичное окно — то, что держит ключ одного
//     экземпляра (SingleInstance::isPrimary); окно --new-instance и stdin
//     файла не касается. Пишется при штатном выходе (saveLastSession). Окно без вкладок (и без сохраняемых
//     вкладок: только stdin) прошлую сессию не затирает — файл остаётся как
//     есть; файл новой версии не перезаписывается. startUp: включена
//     настройка — восстановить, затем открыть командную строку; выключена —
//     canRestoreLastSession() держит ссылку на стартовом экране, пока сессию
//     не восстановили. Файлы от второго экземпляра идут мимо startUp;
//   • закрытые вкладки — rememberClosedTab в closeTab ДО удаления страницы
//     снимает TabState в стек (последние kClosedTabsLimit; вкладка stdin и
//     пустая не идут), reopenClosedTab возвращает верхнюю тем же путём, что
//     сессия (applyState(…, Apply), отчёт о пропавших и изменённых файлах), на
//     прежнее место в ряду. Стек живёт до выхода, на диск не пишется.
// ============================================================================
class SessionController : public QObject {
    Q_OBJECT
public:
    explicit SessionController(QObject* parent = nullptr);

    struct ReportEntry {
        enum class Kind { Missing, Changed, Relocated, Located };
        Kind kind = Kind::Missing;
        QPointer<LogViewWidget> tab;
        const QObject* tabObject = nullptr; // только для сравнения при уничтожении вкладки
        QString tabTitle;       // «2: app.log, db.log» — номер вкладки в сессии и её файлы
        QString savedPath;      // путь в файле сессии
        QString path;           // где источник теперь (Missing — сохранённый)
        QString relativeTarget; // куда указывал относительный путь сессии (для пересохранения)
        TabSource source;       // для «указать файл» и пересохранения
    };

    SessionState capture(const QList<LogViewWidget*>& tabs, LogViewWidget* active) const;

    struct OpenResult {
        QVector<LogViewWidget*> tabs;   // в порядке сессии
        LogViewWidget* active = nullptr;
        int missing = 0, changed = 0, relocated = 0;
        bool hasProblems() const { return missing || changed; }
    };
    OpenResult open(SessionState state, const QString& sessionFilePath,
                    const std::function<LogViewWidget*()>& newTab);

    // Отчёт: записи вкладок, которые ещё открыты (последняя сессия — в конце).
    QVector<ReportEntry> report() const;
    static QString describe(const ReportEntry& entry);
    // Строка состояния после open / reopenClosedTab.
    enum class Origin { SessionFile, LastSession, ClosedTab };
    static QString summary(const OpenResult& result, Origin origin = Origin::SessionFile);

    // «Указать файл» для записи report()[index] (Missing). false — и причина.
    bool locate(int index, const QString& filePath, QString* error = nullptr);

    struct SchemaChoice {
        int index = -1;                 // в list; -1 — без схемы
        bool added = false;             // index указывает на newEntry, его надо добавить в каталог
        SchemaStore::SchemaEntry newEntry;
    };
    static SchemaChoice chooseSchema(const SchemaStore::SchemaList& list, const QString& name,
                                     const QString& schema);

    // ---- Сессия окна ---------------------------------------------------------
    // Что окно добавляет к вкладкам: схема и колонки активной вкладки —
    // верхний уровень сессии (F-5: у каждой вкладки своя в ViewState.fields).
    struct WindowSettings {
        QString schemaName, schema;
        bool showFields = false;
        QStringList visibleFields;
    };
    struct Window {
        std::function<QList<LogViewWidget*>()> tabs;   // в порядке ряда
        std::function<LogViewWidget*()> active;
        std::function<WindowSettings()> settings;
    };
    void setWindow(Window window) { m_window = std::move(window); }
    SessionState captureWindow() const;
    // Имя схемы для снимка: выбранная в списке, если её определение и есть
    // действующее; иначе без имени.
    static QString schemaNameFor(const SchemaStore::SchemaList& list, int currentIndex,
                                 const QString& schema);

    // ---- Последняя сессия -----------------------------------------------------
    // primary == false — файл не читается и не пишется (вторичный экземпляр).
    void setLastSessionFile(const QString& path, bool primary);
    QString lastSessionFile() const { return m_lastSessionPath; }
    bool ownsLastSession() const { return m_ownsLastSession; }
    // Есть что восстановить: свой файл, в нём есть вкладки, в этом запуске
    // его ещё не восстанавливали.
    bool canRestoreLastSession() const { return m_lastSessionAvailable; }
    enum class SaveOutcome { Saved, NotOwner, KeptEmpty, KeptNewer, Failed };
    SaveOutcome saveLastSession(QString* error = nullptr);
    // Прочитать для восстановления; после этого canRestoreLastSession() == false.
    std::optional<SessionState> takeLastSession(QString* error = nullptr);
    // Запуск окна: включена настройка (restore) — восстановить последнюю
    // сессию (openSession), затем открыть командную строку.
    void startUp(bool restore,
                 const std::function<void(const SessionState&, const QString& path)>& openSession,
                 const std::function<void()>& openCommandLine, QString* error = nullptr);

    // ---- Закрытые вкладки (Ctrl+Shift+T) ---------------------------------------
    static constexpr int kClosedTabsLimit = 10;
    // Снять вкладку перед закрытием; position — её место в ряду. false — не
    // запомнена (stdin, пустая).
    bool rememberClosedTab(LogViewWidget* tab, int position);
    int closedTabCount() const { return int(m_closedTabs.size()); }
    // Вернуть последнюю закрытую: newTabAt(position) создаёт вкладку на этом
    // месте ряда (если оно ещё есть — иначе в конце, решает окно). Стек пуст —
    // результат без вкладок.
    OpenResult reopenClosedTab(const std::function<LogViewWidget*(int position)>& newTabAt);

signals:
    void reportChanged();
    void lastSessionAvailabilityChanged(bool available);
    void closedTabsChanged(int count);

private:
    void prune();
    TabState captureTab(LogViewWidget* tab) const;
    // Пропавшие источники вкладки → куда указывал относительный путь сессии.
    QHash<QString, QString> relativeTargetsOf(LogViewWidget* tab) const;
    OpenResult openTabs(SessionState state, const QString& sessionFilePath,
                        const std::function<LogViewWidget*()>& newTab, bool numbered);
    void setLastSessionAvailable(bool available);

    QVector<ReportEntry> m_entries;
    Window m_window;
    QString m_lastSessionPath;
    bool m_ownsLastSession = false;
    bool m_lastSessionAvailable = false;
    std::optional<SessionState> m_lastSessionRead; // прочитанное в setLastSessionFile
    struct ClosedTab {
        TabState state;
        int position = 0;
        QHash<QString, QString> relativeTargets; // SessionState::relativeTargets вкладки

    };
    QVector<ClosedTab> m_closedTabs; // последняя закрытая — в конце
};

#endif // SESSIONCONTROLLER_H
