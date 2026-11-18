#ifndef OPENROUTING_H
#define OPENROUTING_H

#include <QString>
#include <QStringList>

// ============================================================================
// OpenRouting — «что из переданного куда идёт», без виджетов.
//
// Пути приходят в окно тремя дорогами: drag&drop, командная строка и второй
// экземпляр (SingleInstance). Правило у всех одно и записано здесь, а не в
// MainWindow:
//   • обычные файлы открываются, как и раньше, в порядке передачи;
//   • каталог уходит в Directory Scanner (панель показывается, скан
//     запускается). Сканер держит один корень, поэтому из нескольких
//     каталогов сканируется ПЕРВЫЙ; остальные перечислены в
//     skippedDirectories — окно сообщает о них в строке статуса;
//   • файлы вперемешку с каталогами: и то и другое, файлы — первыми;
//   • чего нет на диске — в missing; молча, как и раньше, не открывается.
// Пути в плане абсолютные и очищенные (QDir::cleanPath), повторы убраны.
//
// RecentFiles — список недавних файлов и правило, что в него НЕ попадает:
// спул stdin (`program | DendroLog -`) — временный файл, после выхода его
// нет, запись была бы мёртвой. Запись спула, оставшаяся в ini от прежних
// версий, отбрасывается при загрузке (setPaths).
//
// Саму механику открытия (выбор вкладки, бэкенд) этот модуль не трогает —
// она остаётся в MainWindow до шага 2 из docs/design/F-2.md.
// ============================================================================
namespace OpenRouting {

struct Plan {
    QStringList files;               // открыть вкладками
    QString     scanDirectory;       // открыть в Directory Scanner; пусто — нет
    QStringList skippedDirectories;  // каталоги сверх первого
    QStringList missing;             // не существует (или не файл и не каталог)

    bool isEmpty() const { return files.isEmpty() && scanDirectory.isEmpty(); }
};

Plan plan(const QStringList& paths);

// Абсолютные пути относительно текущего каталога ЭТОГО процесса. Второй
// экземпляр обязан разрешить пути до передачи первому: у того другой cwd.
QStringList absolutePaths(const QStringList& paths);

} // namespace OpenRouting

class RecentFiles {
public:
    static constexpr int MaxCount = 10;

    const QStringList& paths() const { return m_paths; }
    // Список из настроек: без временных файлов и повторов, не длиннее MaxCount.
    void setPaths(const QStringList& paths);
    // Ставит путь первым. false — путь в недавние не годится (спул stdin).
    bool add(const QString& path);
    bool remove(const QString& path);
    void clear() { m_paths.clear(); }

    // Годится ли путь в недавние: false у временных файлов приложения.
    static bool accepts(const QString& path);

private:
    QStringList m_paths;
};

#endif // OPENROUTING_H
