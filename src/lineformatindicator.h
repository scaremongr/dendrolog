#ifndef LINEFORMATINDICATOR_H
#define LINEFORMATINDICATOR_H

#include "lineformat.h"
#include "logfile.h"

#include <QDialog>
#include <QPointer>
#include <QToolButton>

class LogViewWidget;
class QActionGroup;
class QComboBox;
class QLineEdit;
class QMenu;

// ============================================================================
// LineFormatIndicator — формат строк файла в строке статуса (F-17, шаг 7),
// рядом с кодировкой и устроен как EncodingIndicator: файл ТЕКУЩЕЙ строки
// активной вкладки (у слитой — с его именем), подпись «JSON Lines (auto)»,
// «Plain text (manual)», «Plain text (auto, few lines)» — пока в файле меньше
// 20 непустых строк.
//
// Щелчок — меню: «Plain text», «JSON Lines», «logfmt» → LogViewWidget::
// setLineFormat (выбор пользователя; файл перечитывается, если выбор читает
// строки иначе); «Keys…» — свои ключи времени, уровня и сообщения, единица
// эпохи и шкала уровней (LineFormatKeysDialog); «Detect automatically» →
// restoreAutomaticLineFormat. Вся логика — во вкладке, здесь только вид.
// ============================================================================
class LineFormatIndicator : public QToolButton {
    Q_OBJECT
public:
    explicit LineFormatIndicator(QWidget* parent = nullptr);

    void setTab(LogViewWidget* tab);
    LogViewWidget* tab() const { return m_tab; }
    LogFilePtr file() const { return m_file; }

    static QString label(const LineFormat& format);

public slots:
    void refresh();

private:
    void choose(LineFormat::Kind kind);
    void chooseKeys();
    void chooseAutomatic();

    QPointer<LogViewWidget> m_tab;
    LogFilePtr m_file;
    QMenu* m_menu = nullptr;
    QActionGroup* m_group = nullptr;
    QAction* m_plainAction = nullptr;
    QAction* m_jsonAction = nullptr;
    QAction* m_logfmtAction = nullptr;
    QAction* m_keysAction = nullptr;
    QAction* m_autoAction = nullptr;
    QList<QMetaObject::Connection> m_connections;
};

// Свои ключи формата JSON / logfmt: ключи ролей через запятую, единица эпохи,
// шкала числовых уровней. format() — выбор пользователя (Origin::User).
class LineFormatKeysDialog : public QDialog {
    Q_OBJECT
public:
    explicit LineFormatKeysDialog(const LineFormat& current, QWidget* parent = nullptr);
    LineFormat format() const;

private:
    QComboBox* m_kind = nullptr;
    QLineEdit* m_time = nullptr;
    QLineEdit* m_level = nullptr;
    QLineEdit* m_message = nullptr;
    QComboBox* m_epoch = nullptr;
    QComboBox* m_levels = nullptr;
    KeyedDefinition m_columns;   ///< Колонки прежнего выбора (ключи начала файла).
};

#endif // LINEFORMATINDICATOR_H
