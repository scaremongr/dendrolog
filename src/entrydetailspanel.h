#ifndef ENTRYDETAILSPANEL_H
#define ENTRYDETAILSPANEL_H

#include <QJsonDocument>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <memory>

class LogEntry;
class QShowEvent;
class QTextBrowser;
class QToolButton;
class QUrl;

// ============================================================================
// EntryDetailsPanel — содержимое дока «Entry Details».
//
// Показывает выбранную в активном view запись максимально подробно:
//   • Header — уровень, полный таймстамп (с миллисекундами), файл-источник,
//     номера строк и id логической записи;
//   • Fields — извлечённые по активной схеме поля (имя → значение);
//   • Message — полный текст логической записи (ВСЕ её строки, включая
//     continuation) с переносом — удобно листать лог в однострочном режиме;
//   • JSON — эвристически найденные в тексте JSON-фрагменты в
//     отформатированном виде (pretty-print с подсветкой ключей/значений).
//
// Секции переключаются кнопками в шапке панели (например, только JSON);
// состояние живёт в INI [EntryDetailsPanel]. У секций есть ссылка ⧉ —
// копирование содержимого в буфер (JSON — отформатированный целиком,
// Message — без обрезки).
//
// Каждое поле в секции Fields сворачивается стрелкой ▾/▸ (клик по имени),
// как в tree view: свёрнутое поле показывает «…» вместо значения. Набор
// свёрнутых полей помнится ПО ИМЕНИ поля — переживает переход к другой
// записи и перезапуск (тот же INI).
//
// При переходе на другую запись прокрутка НЕ сбрасывается в начало: панель
// запоминает секцию в верху вьюпорта (по якорям "sec-*") и возвращается к
// ней в новой записи — прокрутив до JSON, можно листать записи стрелками.
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

private slots:
    void onAnchorClicked(const QUrl& url); // ссылки "copy:*" и "fold:*"

private:
    // Перестраивает HTML, если панель видима; иначе откладывает до showEvent.
    void scheduleRebuild();
    // setHtml + восстановление прокрутки к прежней секции.
    void rebuildNow();
    // Не-const: запоминает найденные JSON-блоки для копирования по ⧉.
    QString buildHtml();
    void loadSectionSettings();
    void saveSectionSettings() const;

    // Строка логической записи, у которой извлеклись поля (обычно первая);
    // nullptr — ни у одной не извлеклись.
    const LogEntry* fieldsSource() const;
    // Полный текст логической записи (без обрезки — для копирования).
    QString joinedMessage() const;

    // Позиции якорей секций в текущем документе (в порядке следования).
    struct SectionAnchor {
        QString name; // "sec-header", "sec-fields", "sec-message", "sec-json0"…
        int y;        // верх блока в координатах документа
    };
    QVector<SectionAnchor> collectSectionAnchors() const;

    QToolButton* m_headerButton  = nullptr;
    QToolButton* m_fieldsButton  = nullptr;
    QToolButton* m_messageButton = nullptr;
    QToolButton* m_jsonButton    = nullptr;
    QTextBrowser* m_browser = nullptr;

    std::shared_ptr<LogEntry> m_line;
    QVector<std::shared_ptr<LogEntry>> m_logicalLines;
    QStringList m_fieldNames;
    QVector<QJsonDocument> m_jsonBlocks; // найденные при последнем buildHtml
    QSet<QString> m_collapsedFields;     // свёрнутые поля секции Fields (по имени)
    bool m_dirty = false;
    bool m_hadEntry = false; // в документе сейчас отрисована запись (не заглушка)
};

#endif // ENTRYDETAILSPANEL_H
