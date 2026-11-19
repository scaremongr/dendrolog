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
//     указанный, снимается снова: пересохранённая сессия его не теряет.
//     Схему и выбор колонок ставит окно (они глобальные до F-5).
//   • open — открыть: перепривязка путей (SessionFile::rebind: абсолютный,
//     затем относительный к файлу сессии), затем вкладки в порядке файла
//     сессии — newTab() и applyState(tab, Apply) до файлов: фильтры,
//     маркеры, закладки, позиция, follow-tail, авто-обновление, выбор
//     кодировки. Схему сессии окно ставит ДО open: правила привязываются к
//     схеме вкладки. Уже открытые вкладки не трогаются — сессия добавляется.
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
    // Строка состояния после open.
    static QString summary(const OpenResult& result);

    // «Указать файл» для записи report()[index] (Missing). false — и причина.
    bool locate(int index, const QString& filePath, QString* error = nullptr);

    struct SchemaChoice {
        int index = -1;                 // в list; -1 — без схемы
        bool added = false;             // index указывает на newEntry, его надо добавить в каталог
        SchemaStore::SchemaEntry newEntry;
    };
    static SchemaChoice chooseSchema(const SchemaStore::SchemaList& list, const QString& name,
                                     const QString& schema);

signals:
    void reportChanged();

private:
    void prune();
    QVector<ReportEntry> m_entries;
};

#endif // SESSIONCONTROLLER_H
