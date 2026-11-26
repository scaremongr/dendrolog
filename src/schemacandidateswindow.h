#ifndef SCHEMACANDIDATESWINDOW_H
#define SCHEMACANDIDATESWINDOW_H

#include "schemacandidates.h"

#include <QPointer>
#include <QUrl>
#include <QWidget>

class FieldSchemaController;
class LogViewWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

// ============================================================================
// Окно «Find a Schema» (F-17, шаг 5; F-17.md, раздел 7) — только вид над
// SchemaCandidates. Немодальное, одно на окно программы, следует за вкладкой,
// для которой открыто (setView). Слева — варианты: схемы каталога, вывод
// структуры, AI (кнопка «Ask AI…» / «Set Up AI…»); справа — шаблон, мера,
// заметки, таблица образцов (колонка — поле, строка — настоящая строка
// вкладки) и не подошедшие строки с причиной словами. Кнопки: Use for This
// Tab (схема — вкладке окна через FieldSchemaController), Save to My
// Schemas… (шаблон — файлом каталога), Close.
//
// Окно согласия на отправку строк (AiConsentDialog) — модальное, перед
// каждой отправкой. Настройки AI (включён, модель, ключ) окно берёт из
// AppSettings и SecretStore и следит за их сменой.
// ============================================================================
class SchemaCandidatesWindow : public QWidget {
    Q_OBJECT
public:
    explicit SchemaCandidatesWindow(FieldSchemaController* schemas, QWidget* parent = nullptr);
    ~SchemaCandidatesWindow() override;

    /// Вкладка, для которой ищется схема (файл текущей строки).
    void setView(LogViewWidget* view);
    LogViewWidget* view() const { return m_view; }
    /// Без вкладки — модель и файл напрямую (тест темы).
    void setSourceModel(LogModel* model, const LogFile* file);

    SchemaCandidates* candidates() const { return m_candidates; }
    /// Выбранный вариант (номер в candidates()->variants()), -1 — нет.
    int currentVariant() const;
    void selectVariant(int variant);

    /// Шов теста: адрес сервера модели вместо api.anthropic.com.
    static void setAiEndpointForTesting(const QUrl& endpoint);

signals:
    /// «Set Up AI…» — открыть Settings → AI.
    void settingsRequested();
    /// Схема сохранена в каталог (имя).
    void schemaSaved(const QString& name);
    /// Схема поставлена вкладке.
    void schemaUsed(LogViewWidget* view);

private:
    void refreshAiSettings();
    void rebuildList();
    void showVariant();
    void updateAiControls();
    void onAskAi();
    void onConsentNeeded();
    void useForTab();
    void saveToSchemas();

    FieldSchemaController* m_schemas = nullptr;
    QPointer<LogViewWidget> m_view;
    SchemaCandidates* m_candidates = nullptr;

    QLabel* m_sourceLabel = nullptr;
    QListWidget* m_list = nullptr;
    QLabel* m_status = nullptr;
    QPushButton* m_askAi = nullptr;
    QLabel* m_aiStatus = nullptr;
    QLabel* m_title = nullptr;
    QLineEdit* m_template = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_notes = nullptr;
    QTableWidget* m_samples = nullptr;
    QLabel* m_missesLabel = nullptr;
    QTableWidget* m_misses = nullptr;
    QPushButton* m_use = nullptr;
    QPushButton* m_save = nullptr;
    bool m_consentOpen = false;
};

#endif // SCHEMACANDIDATESWINDOW_H
