#ifndef ENTRYDETAILSPANEL_H
#define ENTRYDETAILSPANEL_H

#include <QStringList>
#include <QVector>
#include <QWidget>
#include <memory>

struct LogEntry;
class QTextBrowser;
class QShowEvent;

// ============================================================================
// EntryDetailsPanel — содержимое дока «Entry Details».
//
// Показывает выбранную в активном view запись максимально подробно:
//   • метаданные — уровень, полный таймстамп (с миллисекундами), файл-источник,
//     номера строк и id логической записи;
//   • извлечённые по активной схеме поля (имя → значение);
//   • полный текст логической записи (ВСЕ её строки, включая continuation)
//     с переносом — удобно листать лог в однострочном режиме без WordWrap;
//   • эвристически найденные в тексте JSON-фрагменты в отформатированном
//     виде (pretty-print с подсветкой ключей/значений).
//
// Панель пассивна: MainWindow скармливает ей текущую запись на каждый
// currentRowChanged (showEntry/clearEntry). Пока док скрыт, HTML не строится —
// данные запоминаются и рендерятся при первом показе (showEvent). shared_ptr
// на записи держатся намеренно: содержимое панели переживает reset модели.
// ============================================================================
class EntryDetailsPanel : public QWidget {
    Q_OBJECT
public:
    explicit EntryDetailsPanel(QWidget* parent = nullptr);

    // line — выбранная строка; logicalLines — все строки её логической записи
    // в порядке файла (содержат саму line); fieldNames — схема полей модели.
    void showEntry(const std::shared_ptr<LogEntry>& line,
                   const QVector<std::shared_ptr<LogEntry>>& logicalLines,
                   const QStringList& fieldNames);
    void clearEntry();

protected:
    void showEvent(QShowEvent* event) override;

private:
    // Перестраивает HTML, если панель видима; иначе откладывает до showEvent.
    void scheduleRebuild();
    QString buildHtml() const;

    QTextBrowser* m_browser = nullptr;

    std::shared_ptr<LogEntry> m_line;
    QVector<std::shared_ptr<LogEntry>> m_logicalLines;
    QStringList m_fieldNames;
    bool m_dirty = false;
};

#endif // ENTRYDETAILSPANEL_H
