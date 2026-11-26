#pragma once

#include <QPointer>
#include <QVector>
#include <QWidget>

class LogModel;
class LogListView;
class QuickSearch;
struct LogFile;

// ============================================================================
// FileLegend — легенда файлов слитой вкладки над логом (U-23, часть 2).
//
// Строка чипов «цвет + имя файла + число строк» в порядке добавления файлов
// (LogModel::labeledFiles); не помещается в ширину — переносится. Видна в
// режимах меток Name и Color Only, когда у вкладки два файла и больше
// (shouldShow; ставит и прячет её вкладка — LogViewWidget). Число строк — у
// хранилища за O(1) на файл, обновляется на события модели без прохода по
// строкам; больше 99 999 — сокращённо, точное — в подсказке вместе с путём.
// Число — всегда «всего», при любых фильтрах (и у скрытого файла), и стоит
// в поле постоянной ширины: чипы не меняют размер и легенда не «скачет».
// Контекстное меню чипа: следующая / предыдущая строка файла (от выбранной
// строки, через LogListView::goToRow — в историю переходов), Copy Path,
// Show in Folder. Строка файла ищется по снапшоту видимых строк
// (LogScanSnapshot::findRowOfFile): до fileJumpSyncRows видимых строк —
// сразу, больше — в пуле (QuickSearch::startFile, S-14), как F8: на слитой
// вкладке в сотни миллионов строк проход на GUI-потоке был бы фризом.
// Фоновый поиск уступает пользователю (действие во view после старта —
// результат отброшен); модель перестроилась посреди поиска, а пользователь
// ничего не делал — поиск заново от того же выбора, как у F3 (у загружающейся
// слитой индексной вкладки reset на каждый батч). Ход и «строк нет» —
// statusMessage. Цвета — AppTheme::fileColor, как у полос строк.
// ============================================================================
class FileLegend : public QWidget {
    Q_OBJECT
public:
    explicit FileLegend(QWidget* parent = nullptr);

    void setModel(LogModel* model);
    void setView(LogListView* view) { m_view = view; }

    // Режим меток и число файлов позволяют легенду.
    bool shouldShow() const;
    int chipCount() const { return int(m_chips.size()); }
    // Чип i: экранный прямоугольник, текст (имя и число), файл.
    QRect chipRect(int i) const;
    QString chipText(int i) const;
    const LogFile* chipFile(int i) const;
    int chipAt(const QPoint& pos) const;
    // Действия контекстного меню (тестам — без меню). true — перешли
    // (синхронный путь) или поиск ушёл в фон (isFileJumpRunning).
    bool goToFileLine(int chip, bool forward);
    bool isFileJumpRunning() const;
    void cancelFileJump();
    // Порог синхронного поиска строки файла (видимых строк; тестам — 0).
    void setFileJumpSyncRows(int rows) { m_fileJumpSyncRows = rows; }
    // Фильтр по файлу (U-23, часть 4) — то же, что щелчок по чипу: скрыть
    // или вернуть файл; only (Ctrl+щелчок) — оставить только этот файл,
    // повторно — вернуть все. Последний видимый файл не скрывается; false —
    // ничего не изменилось.
    bool toggleFile(int chip, bool only);
    bool isChipHidden(int chip) const;

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // Набор чипов или их размер сменился: вкладке — показать или спрятать.
    void visibilityWanted(bool show);
    // Сообщение для строки статуса: ход фонового поиска, у файла нет
    // видимых строк.
    void statusMessage(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Chip {
        const LogFile* file = nullptr;
        QString name;
        QString count;          // строк файла во вкладке (от фильтров не зависит)
        QString path;           // показывать (displayPath)
        QString filePath;       // ключ фильтра по файлу (LogModel::hiddenFiles)
        qint64 lines = 0;
        int colorOrdinal = 0;
        QRect rect;
    };
    void rebuild();
    void updateCounts();
    QString countText(const Chip& chip) const;
    // Раскладка чипов по ширине width; возвращает высоту.
    int layoutChips(int width, QVector<Chip>* chips) const;
    int chipWidth(const Chip& chip) const;
    int countFieldWidth() const;

    QPointer<LogModel> m_model;
    QPointer<LogListView> m_view;
    QVector<Chip> m_chips;
    QMetaObject::Connection m_setConnection;
    QMetaObject::Connection m_linesConnection;
    QMetaObject::Connection m_filterConnection;
    QString m_pressedFile;   // чип под нажатием (щелчок — на том же)
    bool startFileJump(const LogFile* file, bool forward);
    void finishFileJump(int row, bool background);

    QuickSearch* m_fileJump = nullptr;
    int m_fileJumpSyncRows = 1000000;
    QString m_fileJumpName;          // имя файла идущего фонового поиска
    const LogFile* m_fileJumpFile = nullptr;   // только сравнивается, не разыменовывается
    bool m_fileJumpForward = true;
    quint64 m_fileJumpUserActions = 0;   // LogListView::userActionCount на старте
};
