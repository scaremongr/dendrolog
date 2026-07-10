#ifndef STATISTICSPANEL_H
#define STATISTICSPANEL_H

#include <QDateTime>
#include <QFutureWatcher>
#include <QVector>
#include <QWidget>
#include <atomic>
#include <memory>
#include "logentry.h"
#include "logfile.h"

class LogModel;
class QLabel;
class QShowEvent;
class QTextBrowser;
class QTimer;
class QToolButton;
class ToggleSwitch;

// ============================================================================
// DocumentStats — результат фонового сбора статистики по документу (вкладке).
// Заполняется воркером collectDocumentStats() в пуле потоков; GUI только
// читает готовую структуру и рендерит её в HTML.
// ============================================================================
struct DocumentStats {
    // Шаблон сообщения: текст записи с «выцветшими» переменными частями
    // (числа/hex/GUID → «#»), что группирует «Connection to 10.0.0.7 lost»
    // и «Connection to 10.0.0.9 lost» в одну строку статистики.
    struct Template {
        QString    text;               // нормализованный шаблон (для показа)
        QString    example;            // исходная строка первого вхождения
        int        count = 0;          // записей с этим шаблоном
        LogLevel   level = LogLevel::Unknown; // уровень первого вхождения
        qint64     firstMs = -1, lastMs = -1; // время первого/последнего (мс epoch)
        int        firstEntryId = 0;   // прыжок к первому вхождению
        LogFilePtr firstFile;
    };

    // Найденная эвристиками аномалия. Воркер отдаёт только числа/интервалы,
    // человекочитаемый текст собирает панель (tr() нельзя дёргать из пула).
    // Для навигации валидна одна из адресаций: интервал времени ИЛИ запись.
    struct Insight {
        enum Kind { ErrorSpike, Burst, Spam, Gap, RisingErrors, RisingVolume };
        Kind       kind = Burst;
        qint64     fromMs = -1, toMs = -1; // интервал аномалии (мс epoch)
        qint64     count = 0;          // записей/ошибок в интервале или шаблоне
        double     factor = 0;         // ×N к типичному/среднему уровню
        double     share = 0;          // доля от всех записей (спам/тренд)
        QString    snippet;            // шаблон сообщения (спам)
        int        entryId = -1;       // < 0 — прыжок к записи не задан
        LogFilePtr file;
        bool       preferErrors = false;
        double     magnitude = 0;      // для сортировки внутри одного Kind
    };

    struct FileStat {
        LogFilePtr file;
        int        records = 0, lines = 0;
        int        warn = 0, error = 0, fatal = 0;
        qint64     firstMs = -1, lastMs = -1;
    };

    bool   valid = false;              // false — статистики нет (нет документа)
    bool   cancelled = false;          // сбор оборван (результат не применять)
    int    generation = 0;             // поколение запроса (отсев устаревших)

    int    rows = 0;                   // строк в снапшоте
    int    records = 0;                // логических записей
    int    recordsWithTs = 0;          // из них с валидным таймстампом
    qint64 tMinMs = -1, tMaxMs = -1;   // диапазон времени (мс epoch)

    // Счётчики по уровням логических записей; индекс — int(LogLevel).
    qint64 levelCounts[7] = {};

    double avgPerSec  = 0;             // средний темп записей
    double peakPerSec = 0;             // темп в самой плотной корзине
    qint64 peakFromMs = -1, peakToMs = -1; // границы пиковой корзины

    QVector<Template> topMessages;     // топ шаблонов по количеству
    QVector<Template> topErrors;       // топ шаблонов уровня Error/Fatal
    QVector<Insight>  insights;        // аномалии в порядке важности
    QVector<FileStat> files;           // разбивка по файлам-источникам

    bool   templatesCapped = false;    // словарь шаблонов упёрся в предел
    qint64 collectMs = 0;              // длительность сбора (для подписи)
};

// ============================================================================
// StatisticsPanel — содержимое дока «Statistics»: сводная статистика по
// активной вкладке (документу).
//
//   • сводные плитки: записи/строки/файлы, длительность, средний и пиковый
//     темп (records/sec);
//   • распределение по уровням с цветными барами;
//   • Insights — эвристики: всплески активности и ошибок (робастный порог
//     median+6·MAD по корзинам времени), спам-сообщения (доля + плотность),
//     паузы в логе, рост объёма/ошибок между половинами интервала;
//   • топ повторяющихся сообщений и топ ошибок — по нормализованным
//     шаблонам (числа → «#»), клик прыгает к первому вхождению;
//   • разбивка по файлам, когда в вкладке слито несколько.
//
// Сбор — одна проходка по снапшоту записей в пуле потоков (COW-копия вектора
// shared_ptr; читает только message/timestamp/level/id/sourceFile — поля
// fields может конкурентно переписывать переизвлечение схемы). Поколение +
// cancel-флаг по образцу асинхронной фильтрации LogModel. Пока док скрыт,
// сбор не запускается — данные помечаются dirty и считаются при показе.
// ============================================================================
class StatisticsPanel : public QWidget {
    Q_OBJECT
public:
    explicit StatisticsPanel(QWidget* parent = nullptr);
    ~StatisticsPanel() override;

    // Модель активной вкладки; nullptr — вкладок нет. Панель подписывается
    // на изменения данных и пересобирает статистику сама (если видима).
    void setModel(LogModel* model);

    // Дебаунс-пересбор (изменение данных). Пока панель скрыта — только dirty.
    void scheduleRefresh();

signals:
    // Клик по времени (всплеск/пауза): перейти к моменту в основном view.
    void jumpToTimeRequested(const QDateTime& time, bool preferErrors);
    // Клик по шаблону сообщения: перейти к его первому вхождению.
    void jumpToEntryRequested(int logicalEntryId, const LogFilePtr& file);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void startCollection();            // немедленный асинхронный пересбор
    void onCollectionFinished();
    void cancelCollection();
    void renderStats();                // m_stats → HTML в браузер
    void renderPlaceholder(const QString& text);
    QString buildHtml();               // строит HTML и заполняет m_navTargets
    void onAnchorClicked(const QUrl& url);

    QLabel*       m_statusLabel = nullptr;
    ToggleSwitch* m_filteredSwitch = nullptr; // on — считать по filteredEntries
    QToolButton*  m_refreshButton = nullptr;
    QTextBrowser* m_browser = nullptr;

    LogModel* m_model = nullptr;
    QTimer*   m_refreshTimer = nullptr;   // дебаунс изменений данных
    bool      m_dirty = false;            // данные изменились, пока были скрыты

    // Асинхронный сбор: поколение отсекает устаревшие результаты, cancel-флаг
    // досрочно обрывает ненужный воркер (снапшот держит записи живыми).
    int m_generation = 0;
    std::shared_ptr<std::atomic_bool> m_cancelFlag;
    QFutureWatcher<DocumentStats> m_watcher;

    DocumentStats m_stats;

    // Адресаты ссылок «nav:N» текущего HTML (индекс — N).
    struct NavTarget {
        bool       toEntry = false;
        qint64     timeMs = -1;
        bool       preferErrors = false;
        int        entryId = -1;
        LogFilePtr file;
    };
    QVector<NavTarget> m_navTargets;
};

#endif // STATISTICSPANEL_H
