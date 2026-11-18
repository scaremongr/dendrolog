#ifndef GOTODIALOG_H
#define GOTODIALOG_H

#include "gotolocation.h"

#include <QDialog>
#include <QList>
#include <QPointer>

class LogModel;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// ============================================================================
// GoToDialog — Go to Line / Time (Ctrl+G): одно поле принимает номер строки
// файла либо время, под ним — куда именно приведёт переход (строка скрыта
// фильтром → ближайшая видимая). Разбор и поиск — GoToLocation, здесь только
// ввод и подпись.
//
// Вкладка с несколькими файлами: номер строки относится к одному файлу —
// появляется список файлов, выбран файл текущей строки. На время он не влияет.
//
// Диалог модальный, но вкладка под ним живёт (дочитывание хвоста, фоновый
// фильтр): подпись — предварительный просмотр, строка ищется заново в accept().
// ============================================================================
class GoToDialog : public QDialog
{
    Q_OBJECT
public:
    // currentRow — строка, выбранная пользователем (-1 — нет): её файл выбран
    // в списке файлов, её метка — опорный момент для времени без даты.
    GoToDialog(LogModel* model, const QList<LogFilePtr>& files, int currentRow,
               QWidget* parent = nullptr);

    // Куда перейти; row < 0 — некуда. Действителен после accept().
    GoToResult target() const { return m_result; }
    // Подпись результата для строки статуса (пустая для точного попадания).
    QString targetNote() const { return m_resultNote; }

    void accept() override;

private:
    GoToResult resolveInput(QString* description, bool* inexact) const;
    void updatePreview();

    QPointer<LogModel> m_model;
    // Опорный момент для времени без даты — на момент открытия диалога.
    QDateTime m_reference;
    QLineEdit* m_input = nullptr;
    QComboBox* m_fileCombo = nullptr;
    QLabel* m_fileLabel = nullptr;
    QLabel* m_preview = nullptr;
    QPushButton* m_goButton = nullptr;
    GoToResult m_result;
    QString m_resultNote;
};

#endif // GOTODIALOG_H
