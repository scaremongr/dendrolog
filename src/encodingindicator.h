#ifndef ENCODINGINDICATOR_H
#define ENCODINGINDICATOR_H

#include "logfile.h"
#include "textencoding.h"

#include <QPointer>
#include <QToolButton>

class LogViewWidget;
class QActionGroup;
class QMenu;

// ============================================================================
// EncodingIndicator — кодировка в строке статуса (F-1, шаг 4,
// docs/design/F-1.md «5. Интерфейс»).
//
// Показывает кодировку файла ТЕКУЩЕЙ строки активной вкладки и откуда она:
// «Windows-1251 (auto)», «… (auto, provisional)», «UTF-8 (BOM)», «KOI8-R
// (manual)»; пока в файле только ASCII — «ASCII (auto)». У слитой вкладки —
// файла текущей строки (переход между строками разных файлов меняет
// подпись), без текущей строки — первого файла вкладки.
//
// Щелчок — меню: UTF-8, Windows-1251, IBM866, KOI8-R, Windows-1252 →
// LogViewWidget::setFileEncoding; «Detect automatically» →
// LogViewWidget::restoreAutomaticEncoding (решение автоматики, которое под
// выбором пользователя продолжало копиться рядом). Выбор доступен и пока
// файл «не решён». UTF-16/32 (BOM, только резидентный путь) — подпись без
// меню: такой файл читается своей кодировкой целиком.
//
// Вся логика — во вкладке; здесь только вид, поэтому индикатор проверяется
// offscreen-тестом без MainWindow (tests/encoding_ui). Окно лишь кладёт его в
// строку статуса и сообщает активную вкладку (setTab).
//
// Вид: QToolButton с autoRaise, без своего style sheet — цвета и отступы от
// палитры и CompactStyle, смена темы перекрашивает его как любую кнопку.
// ============================================================================
class EncodingIndicator : public QToolButton {
    Q_OBJECT
public:
    explicit EncodingIndicator(QWidget* parent = nullptr);

    // Активная вкладка; nullptr — индикатор скрыт.
    void setTab(LogViewWidget* tab);
    LogViewWidget* tab() const { return m_tab; }
    // Файл, чья кодировка сейчас показана (nullptr — вкладки или файлов нет).
    LogFilePtr file() const { return m_file; }

    // Подпись для состояния файла: stream — кодировка, в которой байты
    // превращаются в текст (LogViewWidget::fileStreamEncoding).
    static QString label(const FileEncoding& encoding, TextEncoding stream);
    // Кодировки, которые можно выбрать в меню, по порядку пунктов.
    static QList<TextEncoding> choices();

public slots:
    // Перечитать файл текущей строки и его кодировку. Дёшево (ключ текущей
    // строки и состояние файла) — зовётся на каждое событие вкладки.
    void refresh();

private:
    void choose(TextEncoding encoding);
    void chooseAutomatic();

    QPointer<LogViewWidget> m_tab;
    LogFilePtr m_file;
    QMenu* m_menu = nullptr;
    QActionGroup* m_group = nullptr;
    QAction* m_autoAction = nullptr;
    QList<QMetaObject::Connection> m_connections;
};

#endif // ENCODINGINDICATOR_H
