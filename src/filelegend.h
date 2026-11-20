#pragma once

#include <QPointer>
#include <QVector>
#include <QWidget>

class LogModel;
class FileShownCounter;
class LogListView;
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
// Контекстное меню чипа: следующая / предыдущая строка файла (от выбранной
// строки, через LogListView::goToRow — в историю переходов), Copy Path,
// Show in Folder. Цвета — AppTheme::fileColor, как у полос строк.
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
    // Действия контекстного меню (тестам — без меню).
    bool goToFileLine(int chip, bool forward);
    // Фильтр по файлу (U-23, часть 4) — то же, что щелчок по чипу: скрыть
    // или вернуть файл; only (Ctrl+щелчок) — оставить только этот файл,
    // повторно — вернуть все. Последний видимый файл не скрывается; false —
    // ничего не изменилось.
    bool toggleFile(int chip, bool only);
    bool isChipHidden(int chip) const;
    FileShownCounter* shownCounter() const { return m_counter; }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    // Набор чипов или их размер сменился: вкладке — показать или спрятать.
    void visibilityWanted(bool show);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Chip {
        const LogFile* file = nullptr;
        QString name;
        QString count;          // «всего» или «показано / всего»
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

    QPointer<LogModel> m_model;
    QPointer<LogListView> m_view;
    QVector<Chip> m_chips;
    FileShownCounter* m_counter = nullptr;
    QMetaObject::Connection m_setConnection;
    QMetaObject::Connection m_linesConnection;
    QMetaObject::Connection m_filterConnection;
};
