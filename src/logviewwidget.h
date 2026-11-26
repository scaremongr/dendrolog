// logviewwidget.h
#ifndef LOGVIEWWIDGET_H
#define LOGVIEWWIDGET_H

#include "navhistory.h"
#include "textquery.h"
#include <QWidget>
#include <QVBoxLayout>
#include "logmodel.h"
#include "logparser.h"
#include "logindexer.h"
#include <QVector>
#include <QSet>
#include <QStringConverter>
#include "logfile.h"
#include "LogListView.h"
#include "filechangedetector.h"
#include "bookmarkstore.h"
#include "documentstate.h"

class QuickSearch;
class CommandSpooler;
class SpoolItem;
class FileMemoryStore;
class SourceUnpacker;
class SpoolDir;
class FullLogController;
class FileLegend;
class QFrame;
class QLabel;
class QPushButton;
class QTimer;
class QToolButton;

class LogViewWidget : public QWidget {
    Q_OBJECT
public:
    explicit LogViewWidget(QWidget *parent = nullptr);
    ~LogViewWidget();

    LogModel*    model()    const { return m_model; }
    LogListView* view()     const { return m_view;  }
    // Легенда файлов над логом (U-23).
    FileLegend* fileLegend() const { return m_fileLegend; }

    // Добавляет файл в текущий view
    void addLogFile(const QString &filePath);
    // То же с начальным состоянием кодировки файла (F-1, шаг 4): выбор из
    // --encoding (FileEncoding().withUserChoice) или состояние, перенесённое
    // из другой вкладки при объединении. Файл читается этим состоянием с
    // первой строки — без лишнего перечитывания. Файл уже во вкладке — к нему
    // применяется выбор пользователя из initial (если он есть).
    void addLogFile(const QString &filePath, const FileEncoding& initial);
    // То же со своей схемой файла (F-5, PR 3; FieldSettings — только имя и
    // определение): файл читается ею с первой строки. Схема, равная схеме
    // вкладки, своей не считается. Файл уже во вкладке — схема не меняется
    // (смена — setFileSchema).
    void addLogFile(const QString &filePath, const FileEncoding& initial,
                    const FieldSettings& schema);
    // Объединение вкладок: файлы other добавляются сюда со своими состояниями
    // кодировки (выбор пользователя и решение автоматики не теряются) и
    // схемами (F-5, PR 3: схема файла в other — своя здесь, если отличается
    // от схемы этой вкладки; колонки вкладки растут), а его
    // закладки — до файлов (BookmarkStore::restore): строки находятся по ходу
    // загрузки, заметки не теряются.
    void addFilesFrom(const LogViewWidget& other);
    // Член zip или tar (F-2, шаги 4–5): извлекается во временную копию
    // (SourceUnpacker::unpackArchiveMember; член-gzip — распакованным) и дальше
    // живёт как сжатый файл: LogSource{Archive, архив, member}, F5, память
    // файла по архиву и имени, закладки и сессии с именем члена. Файл из
    // каталога, который сканер извлёк из архива (ArchiveRegistry), addLogFile
    // направляет сюда сам. Пустой member — addLogFile(archivePath).
    void addArchiveMember(const QString& archivePath, const QString& member,
                          const FileEncoding& initial = FileEncoding());

    // ---- Сжатые источники (F-2, docs/design/F-2.md, шаг 2) -----------------------
    // Файл с сигнатурой gzip (по байтам, не по имени) распаковывается целиком
    // во временную копию (SpoolDir) в фоне и открывается обычным путём: бэкенд
    // выбирается по размеру распакованного, память файла ищется после
    // распаковки. У LogFile источник — архив (source(), displayPath()),
    // читают копию (filePath). Пока идёт распаковка, вкладка isLoading();
    // прогресс — unpackingProgress, итог ошибки — fileParsingFailed
    // (loadFailureMessage называет причину). Авто-обновление сжатые файлы
    // пропускает; F5 (reloadChangedFiles(true)) распаковывает изменившийся
    // архив заново, пропавший — сообщает, строки остаются.
    // Корень временных файлов (по умолчанию AppSettings::spoolRoot); тесты —
    // свой QTemporaryDir. Действует на распаковки, начатые после вызова.
    void setSpoolRoot(const QString& root);
    QString spoolRoot() const;
    bool isUnpacking() const;
    // Отменить идущие распаковки вкладки: файл остаётся незагруженным с
    // причиной «cancelled», F5 пробует снова. Частичных файлов не остаётся.
    void cancelUnpacking();
    // Копия сжатого файла готова (распакована целиком).
    bool isUnpacked(const LogFilePtr& logFile) const;

    // ---- Потоки: stdin и вывод команды (F-7) --------------------------------------
    // Файл спула потока открывается как растущий лог: LogFile с источником
    // Stream (подпись label — «stdin» или строка команды, имя вкладки name),
    // владеет spool (удаляется, когда файл отпустят все). Временный источник
    // (SpoolDir::isStreamFile → Ephemeral): ни недавних, ни памяти файла, ни
    // сессий. Авто-обновление и follow-tail включает вызывающий (окно).
    void addStream(const std::shared_ptr<SpoolItem>& spool, const QString& name,
                   const QString& label, const FileEncoding& initial = FileEncoding());
    // Вкладка берёт спулер себе (родитель — вкладка): запускает его, если он
    // ещё не запущен, открывает его спул потоком, включает авто-обновление и
    // follow-tail, ставит над логом плашку команды (состояние, код возврата,
    // Stop / Restart). Строки после выхода процесса остаются. Закрытие вкладки
    // останавливает процесс (CommandSpooler::release) — вежливо, затем
    // принудительно; спул удаляется, когда процесс вышел.
    void addCommandOutput(CommandSpooler* spooler, const FileEncoding& initial = FileEncoding());
    QList<CommandSpooler*> commands() const;
    // В файл потока дописаны байты (пинок спулера; stdin — от окна): дочитать
    // хвост сейчас, а если загрузка в полёте — сразу после неё.
    void onStreamGrew();
    // Перезапуск после выхода: вывод дописывается в тот же файл, вид — к хвосту.
    bool restartCommand(CommandSpooler* spooler);
    // Объединение вкладок: спулеры other (с плашками) переходят сюда — процесс
    // не останавливается вместе с закрытой вкладкой-источником. Звать после
    // addFilesFrom(other).
    void adoptCommandsFrom(LogViewWidget& other);

    // ---- Состояние вкладки вне вкладки (R-4, documentstate.h) ------------------
    // Снимок: файлы (идентичность по концу прочитанного, ручной выбор
    // кодировки), закладки, фильтры и маркеры модели, текущая строка и её
    // место на экране (LogListView::viewAnchor), follow-tail, авто-обновление.
    // Спул stdin (Ephemeral) в снимок не попадает — ни файлом, ни закладками,
    // ни текущей строкой. Предложенные и не принятые фильтры (Offer) —
    // снимаются вместо пустых: отказ от щелчка их не теряет.
    TabState captureState() const;
    // Как применять фильтры состояния: Apply — сразу (сессия, Ctrl+Shift+T);
    // Offer — не трогать вид, а предложить «вернуть одним щелчком» (память
    // файла, docs/design/R-5.md, «Решения за автором»). Маркеры, закладки,
    // позиция, follow-tail и авто-обновление ставятся в обоих режимах.
    enum class StateFilters { Apply, Offer };
    struct StateApplyReport {
        QStringList missingSources;  // файла нет — не открыт, закладки ждут его
        QStringList changedSources;  // переписан — открыт чистым, закладки серые
    };
    // Применить снимок к вкладке. Звать на новой вкладке ДО загрузки: закладки
    // и позиция ищутся по ходу загрузки (RecordLocator), фильтры стоят в модели
    // до первой порции, файлы открываются с выбором кодировки. Можно и на
    // вкладке с файлами (посреди загрузки) — поздним проходом.
    // Пропавший источник пропускается, переписанный открывается, но его
    // закладки и позиция получают отказ: «строка не найдена», вид не двигается.
    // Позиция ставится, когда локатор найдёт строку, и только если пользователь
    // к тому времени не выбрал строку и не прокрутил вид сам (инвариант 10).
    StateApplyReport applyState(const TabState& state, StateFilters filters = StateFilters::Apply);
    // Пропавший при applyState источник найден в другом месте (отчёт о
    // восстановлении сессии, «указать файл»; R-5): закладки, ждущие файл по
    // savedPath, переезжают на новый путь, файл открывается с выбором
    // кодировки source — как файл снимка (память дополняет только закладки).
    // Проверку источника (Valid) делает вызывающий.
    void addRelocatedSource(const QString& savedPath, const TabSource& source);
    // Фильтры, предложенные applyState(…, Offer) и ещё не принятые.
    bool hasOfferedFilters() const { return m_offeredFilters.has_value(); }
    const std::optional<ViewState>& offeredFilters() const { return m_offeredFilters; }
    // «Вернуть фильтры прошлого раза» / отказ.
    void applyOfferedFilters();
    void dismissOfferedFilters();
    // Ждёт ли восстановленная позиция ответа локатора.
    bool isPositionPending() const
    { return m_pendingPosition.token != 0 || m_pendingPosition.waitingRow; }

    // ---- Память файла на диске (R-4, шаг 2; filememorystore.h) ----------------
    // Хранилище памяти (общее на окно; nullptr — вкладка ничего не помнит).
    // Действует, пока включена настройка AppSettings::rememberFileState:
    // выключена — память не читается и не пишется.
    //
    // Чтение — в addLogFile, до старта загрузки (одна точка входа для
    // командной строки, drag&drop, недавних, сканера): первый файл вкладки
    // получает состояние целиком — applyState(…, Offer): закладки, маркеры и
    // позиция сразу, фильтры — предложением (плашка над логом «вернуть
    // фильтры прошлого раза»); файл, добавленный вторым, — только закладки и
    // выбор кодировки. --encoding (выбор в initial) главнее памяти. Итог
    // (найдено по содержимому, файл изменился — закладки серые, память новой
    // версии не тронута) — statusMessage.
    // Запись — saveFileMemory: при закрытии вкладки и выходе (окно), и сама
    // через fileMemorySaveDelay после правки закладки или заметки.
    void setFileMemory(FileMemoryStore* store) { m_fileMemory = store; }
    FileMemoryStore* fileMemory() const { return m_fileMemory; }
    // Задержка записи после правки закладки (по умолчанию 2 с; тесты — меньше).
    void setFileMemorySaveDelay(int ms);
    bool isFileMemorySavePending() const;
    // Снять состояние и записать память каждого файла вкладки: закладки
    // файла (кроме серых закладок прежнего содержимого — они в отсоединённой
    // памяти), выбор кодировки; ViewState — только если файл во вкладке один.
    // Спул stdin не пишется. Запись — в фоне (FileMemoryStore::save).
    void saveFileMemory();

    // Сообщение для строки статуса, почему файл не загрузился
    // (fileParsingFailed): понятная причина — UTF-16/32 во вкладке с
    // несколькими файлами (индексный путь его не читает, а откат в
    // резидентный возможен, только пока файл во вкладке один), иначе общее
    // «Failed to parse: <файл>».
    QString loadFailureMessage(const LogFilePtr& logFile) const;
    
    // Путь файла вкладки: абсолютный, без «.» и «..» (один файл — один
    // LogFile); сравнение — с учётом регистра платформы.
    static QString normalizedPath(const QString& path);
    static bool samePath(const QString& a, const QString& b);
    // Возвращает список загруженных файлов
    QList<LogFilePtr> loadedFiles() const { return m_loadedFiles.toList(); }
    
    // Возвращает количество загруженных файлов
    int fileCount() const { return m_loadedFiles.size(); }
    // Пустая вкладка, которую обычное открытие файла может занять: без файлов
    // и не созданная из снимка (applyState). Вкладка сессии, у которой пропали
    // все файлы, и возвращённая Ctrl+Shift+T держат фильтры и закладки снимка —
    // чужой файл в неё не ложится (R-5, шаг 2).
    bool isBlank() const { return m_loadedFiles.isEmpty() && !m_fromState; }

    // Проверяет, изменились ли файлы на диске, и дочитывает новые данные.
    // Возвращает true если хотя бы один файл был обновлён.
    // force (ручной F5) дополнительно повторяет попытку по файлам, чью
    // прошлую загрузку сорвало и на диске с тех пор ничего не изменилось.
    bool reloadChangedFiles(bool force = false);

    // Per-tab auto-reload. Default is taken from AppSettings at construction time.
    bool autoReload() const { return m_autoReload; }
    void setAutoReload(bool enabled);

    // Идёт ли по вкладке фоновая загрузка данных: первичный разбор/индексация
    // либо дочитывание хвоста. Потребители (панель статистики) используют это,
    // чтобы не считать по заведомо неполному документу.
    // Сюда же входит фоновая сверка строк RecordLocator (R-1).
    bool isLoading() const { return m_loading; }

    // Задаёт схему извлечения блоков для парсера (применяется к следующим загрузкам).
    void setParserPattern(const QString& pattern);
    // «Show selected fields only» (F-17, шаг 3): только показ колонок.
    void setShowFields(bool enabled);
    // Схема полей вкладки (F-5, PR 1): схема, показ полей и выбор колонок —
    // состояние вкладки, у соседних вкладок свои. setFieldSettings ставит
    // схему модели (поля — функция текста и схемы, F-17, шаг 3: O(1), записи
    // не трогаются) и парсерам; показ колонок ставит и правила перепривязывает
    // FieldSchemaController::applyToView.
    // Смена того, что схема решает при ЧТЕНИИ (F-4, PR 3: ключевая часть —
    // момент, уровень и граница записи из ключей), перечитывает файлы вкладки
    // сама (rereadForSchema): регексная → регексная — только поля.
    FieldSettings fieldSettings() const;
    void setFieldSettings(const FieldSettings& settings);
    // Показ колонок включён: «Show selected fields only» и у схемы есть колонки.
    bool fieldDisplayEnabled() const { return m_fieldDisplay; }
    // Индексы выбранных колонок в схеме вкладки.
    QVector<int> visibleFieldIndexes() const;
    // Запасная кодировка автоопределения (F-1): однобайтная, которую
    // индексатор и резидентный парсер выбирают, если окно у первого байта
    // ≥ 0x80 не похоже на UTF-8. Действует на проходы, начатые после вызова,
    // — новые файлы и дозапись файлов, кодировка которых ещё не решена;
    // ПРЕДВАРИТЕЛЬНОЕ решение (шаг 3а) пересчитывается по накопленному
    // свидетельству сразу (EncodingDetector::reconsider) и, если ответ
    // сменился, перечитывается путём пересмотра; окончательные решения и
    // выбор пользователя не меняются. Умолчание — настройка приложения
    // (AppSettings::fallbackEncoding); её смену вкладка применяет сама.
    void setFallbackEncoding(TextEncoding encoding);
    TextEncoding fallbackEncoding() const { return m_fallbackEncoding; }
    // Кодировка файла вкладки и откуда она (F-1) — на любом бэкенде: у
    // индексного её держит стор, у резидентного — вкладка (m_fileEncodings).
    // Файл UTF-16/32 (только резидентный путь) — «не решено»; как он читается,
    // говорит fileStreamEncoding.
    FileEncoding fileEncoding(const LogFilePtr& logFile) const;
    // Кодировка, в которой байты файла превращаются в текст: UTF-16/32 по
    // BOM, иначе кодировка декодера файла (UTF-8, пока «не решено»).
    TextEncoding fileStreamEncoding(const LogFilePtr& logFile) const;
    // Выбор кодировки пользователем (F-1; интерфейс — шаг 4). Индексная
    // вкладка — IndexedLogStore::setFileEncoding, без переиндексации.
    // Резидентная — файл перечитывается этой кодировкой в сторону, и его
    // записи подменяются под одним reset (startReencode): текущая запись,
    // выделение и позиция остаются. Загрузка в полёте — сразу после неё. Выбор живёт, пока открыта
    // вкладка: переживает дозапись, перечитывание переписанного файла и
    // переход порога в обе стороны. UTF-16/32 и недекодируемые кодировки
    // игнорируются.
    void setFileEncoding(const LogFilePtr& logFile, TextEncoding encoding);
    // «Определить автоматически» (F-1, шаг 4): выбор пользователя снимается,
    // действует решение автоматики (FileEncoding::automatic — под выбором она
    // продолжала решать рядом). Тем же путём, что выбор: индексная —
    // без переиндексации, резидентная — перечитывание с подменой записей.
    void restoreAutomaticEncoding(const LogFilePtr& logFile);
    QString parserPattern() const;
    QStringList parserFieldNames() const;

    // ---- Схема файла слитой вкладки (F-5, PR 3) ---------------------------------
    // У файла своя схема (имя в каталоге + определение; FieldSettings, у
    // которого значат только schemaName и schema) либо схема вкладки. Своя
    // хранится, только пока отличается от схемы вкладки; показ полей и
    // выбор колонок — вкладки. Колонки вкладки — объединение колонок схем её
    // файлов по имени (FieldLayout): файл со своей схемой дописывает недостающие
    // в конец, смена схемы пользователем строит раскладку заново.
    // Схема, которой читается файл (своя или вкладки).
    FieldSettings fileSchema(const LogFilePtr& logFile) const;
    // Свои схемы файлов: filePath → схема.
    QHash<QString, FieldSettings> fileSchemas() const { return m_fileSchemas; }
    bool hasFileSchemas() const { return !m_fileSchemas.isEmpty(); }
    // Схема вкладки и свои схемы файлов разом (files — filePath → схема; схема,
    // равная схеме вкладки, и файл не из вкладки отбрасываются). Однопараметрный
    // setFieldSettings свои схемы файлов оставляет. Смена ключевой части схемы
    // файла (F-4) перечитывает только этот файл; поля — функция текста и
    // схемы (F-17, шаг 3), их ничто не переизвлекает. Пользовательский путь
    // (показ колонок, перепривязка правил) — FieldSchemaController::
    // applyToView / applyToFile.
    void setFieldSettings(const FieldSettings& settings, const QHash<QString, FieldSettings>& files);
    void setFileSchema(const LogFilePtr& logFile, const FieldSettings& schema);
    // Файл вкладки по ключу источника (RecordLocator::sourceKey); nullptr — нет.
    LogFilePtr fileForSourceKey(const QString& sourceKey) const;
    // Своя схема источника, который ещё откроется (снимок, память файла):
    // файл получит её, как только появится во вкладке (файл уже во вкладке,
    // но без строк — сразу). Ставит FieldSchemaController по
    // savedFileSchemaFound. Схема, уже ждущая этот источник (явная —
    // addLogFile со схемой), не заменяется.
    void setPendingFileSchema(const QString& sourceKey, const FieldSettings& schema);
    // У файла есть (или идут) строки: смена его схемы — смена схемы на
    // загруженных строках (иначе схема ждёт файл и только растит раскладку).
    bool fileHasRows(const LogFilePtr& logFile) const;

    // Быстрый поиск (F3 / Shift+F3) — в фоне (QuickSearch): найденная строка
    // становится текущей, когда скан закончится. Новый поиск отменяет прежний.
    // Невалидный запрос (TextQuery::isValid) не запускается — проверяет вызывающий.
    void searchTextNext(const TextQuery& query);
    void searchTextPrevious(const TextQuery& query);
    void cancelQuickSearch();
    // Запрос последнего быстрого поиска вкладки.
    const TextQuery& quickSearchQuery() const { return m_quickSearchQuery; }
    bool isQuickSearchRunning() const;

    // Переход к записи (из Search Results, из статистики) как выбор
    // пользователя (LogListView::goToRow). Запись скрыта фильтром — к ближайшей
    // видимой строке (LogModel::nearestVisibleRow); false — перейти некуда.
    // kind — вид перехода для истории (NavHistory).
    bool goToEntry(int logicalEntryId, const LogFile* sourceFile,
                   NavKind kind = NavKind::Other);

    // Переход к следующей / предыдущей видимой записи с уровнем из levels
    // (F8 / Shift+F8; к первой показанной строке записи) от строки, выбранной
    // пользователем; через край — заворачивает. Переход — выбор пользователя
    // (LogListView::goToRow).
    // Небольшая вкладка ищется сразу (LogModel::findRowWithLevel), большая —
    // в фоне по снапшоту (QuickSearch::startLevel): проход по сотням
    // миллионов строк на GUI-потоке был бы фризом. Итог в обоих случаях —
    // levelJumpFinished; новый переход отменяет прежний.
    enum class LevelJumpOutcome {
        Moved,        // перешли к следующей по направлению
        Wrapped,      // дальше таких нет — перешли через край вкладки
        OnlyCurrent,  // единственная такая строка — та, на которой стоим
        NotFound,     // среди видимых строк таких нет
        Interrupted,  // модель перестроилась посреди фонового поиска
    };
    void goToLevel(LogLevelMask levels, bool forward);
    void cancelLevelJump();

    // ---- Закладки (R-2) ---------------------------------------------------------
    // Хранилище закладок вкладки — у модели (BookmarkStore).
    BookmarkStore& bookmarks() const { return m_model->bookmarks(); }
    // Поставить / снять закладку на видимой строке; true — теперь стоит.
    bool toggleBookmark(int row);
    // Ctrl+F2: на строке, выбранной пользователем (инвариант 10); false —
    // строка не выбрана.
    bool toggleBookmarkAtSelection();
    // F2 / Shift+F2: к следующей / предыдущей видимой закладке от выбранной
    // строки, через край — заворачивает (как F8). Итог — bookmarkJumpFinished.
    void goToBookmark(bool forward);
    // Переход к закладке из панели: видна — к ней, скрыта фильтром — к
    // ближайшей видимой строке; false — строки нет (Pending, Missing) или не
    // видно ни одной строки. Переход — выбор пользователя (goToRow).
    bool goToBookmarkId(int id);
    // Вкладка не больше стольких видимых строк ищется синхронно.
    void setLevelJumpSyncRows(int rows) { m_levelJumpSyncRows = rows; }

    // ---- Полный лог (R-3, fulllogcontroller.h) -----------------------------------
    // «Show in Full Log»: фильтры вкладки, скрывающие строки (уровни, время,
    // текстовые правила), приостановлены, выбранная строка — на том же месте
    // экрана; над логом плашка «Return to Filtered View». Возврат — ровно
    // прежние фильтры. Правка фильтра пользователем показ заканчивает.
    // Состояние — вкладки (инвариант 7): переживает смену вкладки, дозапись,
    // перезагрузку, добавление файла и переход порога.
    FullLogController& fullLog() const { return *m_fullLog; }
    bool isFullLogShown() const;
    // Одна команда (Ctrl+Shift+L, меню, контекстное меню, кнопка плашки):
    // показ идёт — возврат, иначе показ. row >= 0 — строка под курсором.
    // Сразу после перехода к закладке, скрытой фильтром (пользователь вид с
    // тех пор не трогал), показ ставит выбранной саму закладку. false —
    // показывать нечего: фильтров, скрывающих строки, нет.
    bool toggleFullLog(int row = -1);
    // Reset всех фильтров во время показа: полный лог остаётся, приостановленные
    // фильтры отбрасываются.
    void discardFullLogView();

    // ---- История переходов (R-7) ------------------------------------------------
    // Хранилище — у модели (NavHistory); пишет LogListView::goToRow.
    NavHistory& navHistory() const { return m_model->navHistory(); }
    // Alt+← / Alt+→ (и боковые кнопки мыши): к предыдущему / следующему месту
    // истории через goToRow (выбор пользователя, follow-tail снимается), без
    // новых записей. false — идти некуда (все места пропали или истории нет).
    bool navigateHistory(bool forward);

signals: // Сигналы, которые LogViewWidget будет пробрасывать для MainWindow
    void fileParsingStarted(const LogFilePtr& logFile);
    void fileParsingProgress(const LogFilePtr& logFile, int progressPercentage);
    void fileParsingFinished(const LogFilePtr& logFile, int totalEntries);
    void fileParsingFailed(const LogFilePtr& logFile);

    // Состояние процесса команды (F-7) изменилось: имя и подсказка вкладки.
    void commandStateChanged();
    // Emitted when an incremental reload finishes (new entry count passed).
    void reloadFinished(int newEntriesCount);

    // Началась/закончилась фоновая загрузка данных вкладки (см. isLoading()).
    void loadingChanged(bool loading);

    // Новые сигналы для информации о строках
    void totalRowCountChanged(int totalRows);
    void currentRowChanged(int currentRow, int totalRows); // Передает и текущую, и общую
    void modelFiltered(int totalRowsAfterFilter); // Signal when model has been filtered

    // Быстрый поиск: прогресс долгого скана и итог (отменённый поиск итога
    // не присылает).
    void quickSearchProgress(const QString& term, int percent);
    void quickSearchFinished(const QString& term, bool found);
    // Переход по уровню: прогресс долгого скана и итог.
    void levelJumpProgress(LogLevelMask levels, bool forward, int percent);
    void levelJumpFinished(LogLevelMask levels, LogViewWidget::LevelJumpOutcome outcome);
    // Итог F2 / Shift+F2 (Interrupted не бывает: поиск синхронный).
    void bookmarkJumpFinished(bool forward, LogViewWidget::LevelJumpOutcome outcome);
    // История переходов изменилась: состояние пунктов Back / Forward.
    void navHistoryChanged();
    // Состояние кодировки файла могло измениться (решение автоматики,
    // пересмотр, выбор пользователя, «авто», перечитывание переписанного
    // файла) — индикатор кодировки перечитывает fileEncoding.
    void fileEncodingChanged(const LogFilePtr& logFile);
    // Флаг авто-обновления вкладки сменился (setAutoReload, применение
    // состояния) — окно обновляет таймер и кнопку.
    void autoReloadChanged(bool enabled);
    // Распаковка сжатого файла (F-2): началась, прогресс в процентах по
    // прочитанным сжатым байтам, закончилась (ok — дальше обычная загрузка,
    // иначе за ним придёт fileParsingFailed).
    void unpackingStarted(const LogFilePtr& logFile);
    void unpackingProgress(const LogFilePtr& logFile, int percent);
    void unpackingFinished(const LogFilePtr& logFile, bool ok);
    // Сообщение для строки состояния (итог применения памяти файла).
    void statusMessage(const QString& text);
    // applyState нашёл в снимке схему полей вкладки (F-5): её ищет в каталоге
    // и ставит FieldSchemaController (подписан в configureView) — синхронно,
    // до привязки правил снимка. fromMemory — снимок из памяти файла.
    void savedFieldSettingsFound(const FieldSettings& fields, bool fromMemory);
    // Снимок или память файла несут свою схему файла (TabSource.schema, F-5,
    // PR 3): её ищет в каталоге FieldSchemaController и ставит —
    // setPendingFileSchema (файл ещё не открыт) или схемой файла (applyToView).
    void savedFileSchemaFound(const QString& sourceKey, const FieldSettings& schema,
                              bool fromMemory);
    // Файл со своей схемой дописал колонки вкладки (раскладка выросла, поля
    // загруженных записей не меняются): контроллер ставит показ колонок и
    // перепривязывает правила.
    void fieldLayoutGrown();
    // Предложение фильтров (applyState(…, Offer)) появилось или ушло.
    void offeredFiltersChanged(bool available);
    // Показ полного лога (R-3) начался или закончился — индикаторы фильтров
    // окна и пункт меню перечитывают состояние вкладки.
    void fullLogViewChanged(bool shown);
    // Восстановленная позиция поставлена (true) или не будет поставлена:
    // строка не найдена, пользователь успел выбрать строку или прокрутить.
    void positionRestoreFinished(bool applied);
    // Пункт контекстного меню «заметка к закладке»: закладка уже стоит,
    // заметку спрашивает окно. created — закладку поставил сам этот пункт:
    // отказ от заметки её снимает.
    void bookmarkNoteEditRequested(int bookmarkId, bool created);

private slots:
    void handleEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& entriesBatch, const LogFilePtr& parsedLogFile);
    void handleParsingFinished(int totalEntries, const LogFilePtr& parsedLogFile);
    void handleParsingFailed(const LogFilePtr& parsedLogFile);
    void handleParsingProgress(int progressPercentage, const LogFilePtr& parsedLogFile);
    void handleModelFilteredRelay(int totalRowsAfterFilter);
    void handleQuickSearchFinished(int row);
    // Incremental reload: append new entries without model reset
    void handleIncrementalEntriesParsed(const QVector<std::shared_ptr<LogEntry>>& batch, const LogFilePtr& logFile);
    void handleIncrementalParsingFinished(int totalEntries, const LogFilePtr& logFile);
    void handleIncrementalParsingFailed(const LogFilePtr& logFile);
    // Индексный бэкенд: приём опубликованных порций строк и завершения.
    void handleIndexBatchReady(const LogFilePtr& logFile, qint64 firstLine, qint64 count);
    void handleIndexingFinished(qint64 newLines, const LogFilePtr& logFile);
    void handleIndexingFailed(const LogFilePtr& logFile);
    void handleResidentFallback(const LogFilePtr& logFile, const QString& reason);

private:
    // Загрузка файла после того, как он заведён во вкладке: выбор бэкенда по
    // размеру (у сжатого — распакованной копии) и старт разбора/индексации.
    void startLoad(const LogFilePtr& logFile, const FileEncoding& initial);


    // Ленивая инициализация LogIndexer с подключением сигналов.
    void ensureIndexer();
    // Запуск полной индексации файла (store уже индексный). encoding —
    // кодировка, уже решённая для файла резидентным путём (переход порога):
    // индексатор и стор начинают с неё, а не определяют заново.
    void startIndexedLoad(const LogFilePtr& logFile, FileEncoding encoding = {});
    // Состояние кодировок резидентных файлов → индексная вкладка и обратно:
    // одна точка на каждый переход бэкенда, чтобы решение не терялось.
    QHash<QString, FileEncoding> takeEncodingsForIndexed();
    void captureEncodingsFromIndexed();
    // Файл переписан (полное перечитывание): решение автоматики относится к
    // старому содержимому — «не решено»; выбор пользователя остаётся (как
    // IndexedLogStore::resetFileIndex).
    void forgetAutoEncoding(const QString& filePath);
    // Смена кодировки резидентного файла: полный разбор отдельным парсером в
    // буфер, затем LogModel::replaceEntriesForFile (один reset, якорь view
    // переживает — в отличие от startFullReload, где строки сначала
    // удаляются). Загрузка файла в полёте на всё время разбора.
    // rekey — ключи строк файла меняются (смена схемы, F-4 PR 3: моменты,
    // id записей): закладки, история и позиция ищут строки заново через
    // RecordLocator.
    void startReencode(const LogFilePtr& logFile, bool rekey = false);
    void finishReencode(const LogFilePtr& logFile);
    void handleReencodeFailed(const LogFilePtr& logFile);
    LogParser* m_reencodeParser = nullptr; // ленивый
    QHash<QString, QVector<std::shared_ptr<LogEntry>>> m_reencodeBuffers;

    // Дешёвая подпись файла на диске: пара (размер, mtime). Нужна только
    // чтобы понять «изменилось ли на диске хоть что-то» после неудачной
    // загрузки — иначе нечитаемый (или всё ещё удалённый) файл перечитывался
    // бы на каждый тик авто-обновления. size < 0 означает «файла нет».
    struct DiskStamp {
        qint64 size    = -1;
        qint64 mtimeMs = -1;
        bool exists() const { return size >= 0; }
        bool operator==(const DiskStamp& o) const
        { return size == o.size && mtimeMs == o.mtimeMs; }
        static DiskStamp of(const QString& filePath);
    };

    // ---- Сжатые источники (F-2) --------------------------------------------------
    struct ArchiveState {
        SourceUnpacker* unpacker = nullptr;
        quint64 job = 0;
        bool running = false;      // распаковка идёт
        bool unpacked = false;     // копия готова целиком
        bool awaitingLoad = false; // распакован, память файла решает, как грузить
        bool loadStarted = false;  // startLoad уже был (F5 — полная перезагрузка)
        QString error;             // причина последней неудачи (пусто — нет)
        DiskStamp archiveStamp;    // архив на момент распаковки
        FileEncoding initial;
        // Идентичность, пока копия не готова (captureState): прежнего
        // содержимого (F5) или из снимка, по которому файл открыт.
        std::optional<SourceIdentity> identity;
        bool applyingState = false; // флаги addLogFile на момент вызова —
        bool memoryApplying = false; // распаковка их переживает
    };
    QHash<QString, ArchiveState> m_archives; // ключ — filePath копии
    QString m_spoolRoot;
    std::shared_ptr<SpoolDir> m_spoolDir;
    // Скрытые файлы снимка (U-23), которые ещё распаковываются: ключ источника
    // (RecordLocator::sourceKey) → скрыть копию, когда начнётся её загрузка.
    QSet<QString> m_hiddenArchiveSources;
    // Идентичности источников снимка (applyState), ключ — sourceKey: сжатый
    // файл, ещё не распакованный, снимается с ней (ArchiveState::identity).
    QHash<QString, SourceIdentity> m_stateIdentities;
    static bool isArchive(const LogFilePtr& logFile)
    { return logFile && logFile->source().kind == LogSource::Kind::Archive; }
    // Файл ещё не загружается: распаковка идёт, сорвалась или ждёт памяти.
    bool isArchivePending(const LogFilePtr& logFile) const;
    // Кодировка файла с учётом выбора, отложенного до загрузки копии, и
    // запись такого выбора (ревью шагов 2–3: --encoding, меню индикатора,
    // слияние и конвертация посреди распаковки).
    FileEncoding pendingArchiveEncoding(const LogFilePtr& logFile) const;
    void setPendingArchiveEncoding(const LogFilePtr& logFile, const FileEncoding& encoding);
    // Тот же источник, что путь, открытый пользователем.
    // Член архива — по пути архива и имени (F-2, шаг 4).
    static bool sameSource(const LogFilePtr& logFile, const QString& path,
                           const QString& member = QString());
    void addArchive(const QString& archivePath, const FileEncoding& initial,
                    const QString& member = QString());
    // Файл уже во вкладке, его открывают снова (память, снимок, --encoding).
    void reopenLoaded(const LogFilePtr& file, const FileEncoding& initial);
    // Источник снимка или памяти: обычный файл, .gz или член zip.
    void openSource(const QString& path, const QString& member, const FileEncoding& initial);
    void startUnpack(const LogFilePtr& logFile);
    void handleUnpackFinished(const LogFilePtr& logFile, quint64 job, bool ok,
                              const QString& message);
    // Распакован впервые: память файла (по пути архива и содержимому копии),
    // затем startLoad.
    void continueArchive(const LogFilePtr& logFile);
    bool reloadArchive(const LogFilePtr& logFile);

    // Проход по файлу закончился — RecordLocator: конец прочитанного и
    // недописанная последняя строка (R-1).
    void locatorFileLoaded(const LogFilePtr& logFile);

    // Пересчитывает агрегат «идёт фоновая загрузка» по состояниям файлов и
    // сигналит об изменении. Вызывается в конце каждого события, способного
    // это состояние изменить (старт загрузки, её завершение или обрыв).
    void updateLoadingState();

    // Сбрасывает уже показанные строки файла (файл исчез или переписан).
    void dropRowsForFile(const LogFilePtr& logFile);
    // Полная перезагрузка файла с нуля: строки сбрасываются и файл читается
    // заново. Используется, когда файл переписан, обрезан или удалён и создан
    // заново. Состояние файла обнуляется и помечается «загрузка в полёте».
    void startFullReload(const LogFilePtr& logFile);

    // ---- Перечитывание по смене схемы (F-4, PR 3) ----------------------------
    // Что из схемы файла действует при чтении: ключевая часть без колонок,
    // пока извлечение включено; пусто — эвристика (регексная схема при смене
    // только переизвлекает поля, как прежде). На файл (F-5, PR 3): смена схемы
    // одного файла перечитывает только его.
    QString readingSignature(const QString& filePath) const;
    QHash<QString, QString> readingSignatures() const;
    // Смена схемы: подписи файлов и позиция — до (begin), сравнение и
    // перечитывание изменившихся — после (end).
    struct SchemaChange;
    SchemaChange beginSchemaChange() const;
    void endSchemaChange(const SchemaChange& change);
    // Один файл: резидентный — startReencode(rekey), индексный — полная
    // переиндексация (startFullReload).
    void rereadForSchema(const LogFilePtr& logFile);
    // Проход по файлу закончился: отложенное перечитывание (кодировка,
    // схема). true — запущено.
    bool startPendingReread(const LogFilePtr& logFile);
    // Позиция на время перечитывания: строка (источник + RecordId) и её место
    // на экране; запрос к локатору — после сброса строк её файла
    // (requestRereadPosition), дальше — путь m_pendingPosition.
    struct RereadPosition {
        QString sourceKey; // RecordLocator::sourceKey файла строки
        SourceIdentity source;
        RecordId record;
        int offset = 0;
        bool selected = false;
        quint64 userActions = 0;
    };
    std::optional<RereadPosition> m_rereadPosition;
    // Резидентная вкладка (F-5, PR 3): позиция на всё время перечитываний по
    // смене схемы. Перечитанный файл подменяет записи под reset, и вид
    // восстанавливает ЗАПИСЬ (начало многострочной), а не строку: строка
    // другого файла после такого reset запрашивается у локатора заново.
    std::optional<RereadPosition> m_heldPosition;
    void requestPosition(const RereadPosition& pos);
    struct SchemaChange {
        QHash<QString, QString> signatures; // filePath → readingSignature
        std::optional<RereadPosition> position;
    };
    // Строка и её место на экране ДО смены схемы (смена схемы в модели
    // успевает сдвинуть вид); follow-tail — nullopt.
    std::optional<RereadPosition> captureRereadPosition() const;
    // Подпись сменилась у файлов changed (filePath): они перечитываются (в
    // полёте — по окончании прохода, schemaRereadPending), позиция — снятая до
    // смены, если её файл среди них.
    void rereadAfterSchemaChange(const std::optional<RereadPosition>& position,
                                 const QSet<QString>& changed);
    void requestRereadPosition(const LogFilePtr& logFile);
    // Загрузка файла закончилась — закладки его строк сверяются (fullLoad —
    // файл прочитан с нуля).
    void bookmarksFileLoaded(const LogFilePtr& logFile, bool fullLoad);

    LogListView *m_view;
    FileLegend* m_fileLegend = nullptr;
    LogModel    *m_model;
    QVector<LogFilePtr> m_loadedFiles;  // Список загруженных файлов

    // Быстрый поиск и его запрос — чтобы повторить, если модель перестроится
    // посреди скана.
    void startQuickSearch();
    QuickSearch* m_quickSearch = nullptr;
    TextQuery m_quickSearchQuery;
    bool m_quickSearchForward = true;

    void finishLevelJump(int row, int startRow);
    QuickSearch* m_levelJump = nullptr;
    LogLevelMask m_levelJumpLevels = 0;
    bool m_levelJumpForward = true;
    int m_levelJumpSyncRows = 1000000;

    LogParser* m_logParser;

    // Per-file reload state: the anchor fingerprinting the already-consumed
    // prefix (used to tell appends from rewrites) and the next logicalEntryId.
    //
    // Инварианты (проверяются в reloadChangedFiles):
    //   loadInFlight            — фоновый джоб пишет данные этого файла;
    //                             второй по нему не запускаем;
    //   !initialLoadDone && !loadInFlight — файла нет либо загрузка сорвалась;
    //                             failedStamp хранит то, что мы видели на диске,
    //                             и повтор случится, когда подпись изменится.
    struct FileReloadState {
        FileChangeDetector::Anchor anchor;        // prefix size + head and boundary fingerprints
        int    nextLogicalEntryId = 0;
        bool   initialLoadDone = false;           // файл успешно прочитан целиком
        // Загрузка (полная или дозапись) в полёте: вторую по тому же файлу не
        // запускаем — LineIndex single-writer, а резидентный ре-парс иначе
        // продублировал бы записи.
        bool   loadInFlight = false;
        // Ни один бэкенд не тянет этот файл (например UTF-16 во вкладке с
        // несколькими файлами) — повторять загрузку бессмысленно.
        bool   unsupported = false;
        QString unsupportedReason; // "UTF-16" / "UTF-32" — для loadFailureMessage
        // Подпись файла на момент исчезновения/неудачной загрузки.
        DiskStamp failedStamp;
        // Резидентный путь (LogParser::tailState): где кончилось прочитанное,
        // и не оборвана ли последняя строка — тогда она показана
        // предварительно и при дозаписи читается заново с lastLineStart.
        qint64 endOffset = -1;
        bool   lastLinePartial = false;
        qint64 lastLineStart = -1;
        // Как читались байты (LogParser::tailState): UTF-16/32 по BOM — у
        // дозаписи BOM нет, она читается в той же кодировке; иначе —
        // кодировка декодера файла. Декодирует однобайтный хвост состояние
        // файла (m_fileEncodings), не это поле.
        TextEncoding encoding = TextEncoding::Utf8;
        // Пользователь сменил кодировку, пока файл читался: перечитать его,
        // как только загрузка закончится.
        bool   reparsePending = false;
        // Схему сменили, пока файл читался (F-4, PR 3): перечитать по
        // окончании прохода; у резидентного — с перепривязкой ключей.
        bool   schemaRereadPending = false;
        // Идущий startReencode меняет ключи строк (смена схемы).
        bool   reencodeRekey = false;
        // Две последние строки файла: контекст дочитывания (продолжение в
        // начале дозаписи принадлежит последней записи) и предварительная
        // строка, которую надо убрать перед перечитыванием.
        std::shared_ptr<LogEntry> lastEntry;
        std::shared_ptr<LogEntry> prevEntry;
    };
    QHash<QString, FileReloadState> m_fileReloadStates; // key = filePath
    static void rememberLastLines(FileReloadState& state,
                                  const QVector<std::shared_ptr<LogEntry>>& batch);
    void handleTailState(const LogFilePtr& logFile, qint64 endOffset,
                         bool lastLinePartial, qint64 lastLineStart, TextEncoding encoding);
    // Резидентный путь: кодировка файла и откуда она (F-1). Ключ — путь
    // файла. Переживает сброс FileReloadState (перечитывание, дозапись) и
    // переходы бэкенда: при конвертации в индексный путь уходит в стор
    // (takeEncodingsForIndexed), при откате — возвращается из стора. У
    // индексной вкладки не используется: там состояние держит стор.
    QHash<QString, FileEncoding> m_fileEncodings;
    void handleEncodingDecided(const LogFilePtr& logFile, const FileEncoding& encoding);
    // Состояние кодировки резидентного файла целиком: декодер сменился —
    // перечитывание (startReencode; загрузка в полёте — после неё).
    void applyResidentEncoding(const LogFilePtr& logFile, const FileEncoding& encoding);
    // Индексная вкладка: решение индексатора / новое состояние в сторе;
    // текст показанных строк сменился — фрагменты закладок и истории.
    void adoptIndexedEncoding(const LogFilePtr& logFile, const FileEncoding& encoding,
                              bool wholeState);
    // Текст строк файла стал другим (сменился декодер): фрагменты закладок и
    // истории переходов переписываются по видимым строкам; идёт
    // перефильтрация — ещё раз по её окончании (m_previewRefreshFiles).
    void refreshTextFragments(const QString& filePath);
    QSet<QString> m_previewRefreshFiles;
    // Запасная сменилась: предварительные решения — по свидетельству с новой
    // запасной; посреди загрузки — когда она закончится (m_reconsiderPending).
    void reconsiderProvisional();
    bool m_reconsiderPending = false;
    // Файлы, которые индексный путь вернул в резидентный (UTF-16/32): по
    // порогу размера вкладка с таким файлом не конвертируется — индексатор
    // откатил бы её снова, и файл перечитывался бы целиком на каждый тик.
    // Отметка относится к содержимому: переписанный файл её теряет.
    QSet<QString> m_residentOnlyFiles;

    // Parser used exclusively for incremental reads (so it doesn't interfere with
    // the initial parse that uses m_logParser).
    LogParser* m_reloadParser = nullptr;

    // Индексатор больших файлов (ленивый: создаётся при первом индексном файле).
    LogIndexer* m_indexer = nullptr;
    // Показ колонок: «Show selected fields only» и у схемы есть колонки.
    bool m_fieldDisplay = false;
    // Схема полей вкладки (F-5): имя схемы в каталоге, показ полей как
    // выбрал пользователь, выбранные колонки, сама схема и раскладка колонок
    // вкладки (PR 2: одна схема на все файлы — columnOf тождественен; схема
    // на файл — PR 3). Схемы файлов, которыми читают парсеры, индексатор и
    // стор, — у модели (LogModel::fileSchemas), отсюда их ставит
    // applySchemaToModel; парсеры получают схему файла на старте прохода
    // (passSchema).
    LogPattern m_pattern;
    FieldLayout m_fieldLayout;
    FileSchemaPtr m_tabSchema;
    // Свои схемы файлов (F-5, PR 3): filePath → схема (только отличающиеся от
    // схемы вкладки); схемы источников, которые ещё откроются: sourceKey → схема.
    QHash<QString, FieldSettings> m_fileSchemas;
    QHash<QString, FieldSettings> m_pendingFileSchemas;
    // Выбор колонок снимка (applyState): колонки, которые принесут схемы
    // файлов снимка, показываются по нему, пока ждущие схемы не разобраны.
    std::optional<QStringList> m_stateVisibleFields;
    // Схемы раскладки по определению (одно определение — один указатель:
    // модель сравнивает наборы схем по указателям) и что раскладка уже
    // содержит: определения схемы вкладки и своих схем файлов по порядку.
    QHash<QString, FileSchemaPtr> m_layoutSchemas;
    QStringList m_layoutInputs;
    QStringList layoutInputs() const;
    FileSchemaPtr layoutSchema(const QString& definition);
    // Раскладка (если входы изменились), флаг извлечения, схемы модели,
    // колонки модели — после любой смены схемы.
    void refreshSchemas();
    void rebuildFieldLayout();
    void applySchemaToModel();
    // Своя схема файла, ждавшая его (setPendingFileSchema): ставится, пока у
    // файла нет строк, раскладка растёт.
    void adoptPendingSchema(const LogFilePtr& logFile);
    QHash<QString, FieldSettings> normalizedFileSchemas(const QHash<QString, FieldSettings>& files) const;
    FileSchemaPtr passSchema(const QString& filePath) const;
    QString m_schemaName;
    bool m_showFieldsFlag = false;
    QStringList m_visibleFields;
    TextEncoding m_fallbackEncoding = TextEncodings::systemFallback(); // из AppSettings в конструкторе

    // Per-tab auto-reload flag. Defaults to the global AppSettings value at construction.
    bool m_autoReload = false;

    // Кэш агрегата loadInFlight по всем файлам вкладки (см. updateLoadingState).
    bool m_loading = false;

    // ---- Состояние вкладки (R-4) ---------------------------------------------------
    // Фильтры снимка с привязкой к схеме вкладки (bindFields).
    // hiddenFiles — пути скрытых файлов (U-23); nullptr — оставить как есть.
    void setStateFilters(const ViewState& view, const QSet<QString>* hiddenFiles = nullptr);
    std::optional<ViewState> m_offeredFilters;
    // Позиция из снимка ждёт строку от RecordLocator.
    struct PendingPosition {
        int token = 0;
        int offset = 0;
        bool selected = false;
        quint64 userActions = 0; // LogListView::userActionCount на момент применения
        // Строка найдена, но ещё не показана (фоновый фильтр, загрузка):
        // ждёт конца фильтра или загрузки (retryPendingPosition).
        bool waitingRow = false;
        RecordKey key;
        // Что искать заново, если ключ пропал (строки файла сброшены:
        // конверсия, перезагрузка) — не больше kPositionRequests запросов.
        SourceIdentity source;
        RecordId record;
        int requests = 0;
    };
    static constexpr int kPositionRequests = 3;
    PendingPosition m_pendingPosition;
    void finishPendingPosition(const RecordKey* key);
    void retryPendingPosition();

    // ---- Память файла (R-4, шаг 2) -------------------------------------------------
    bool fileMemoryActive() const;
    // Найти и применить память файла; true — файл уже добавлен (applyState
    // или addLogFile изнутри), вызывающему addLogFile делать нечего.
    // self — уже заведённый сжатый файл (F-2): память — по пути архива,
    // содержимое — по его копии, «первый файл вкладки» — без него самого.
    bool applyFileMemory(const QString& filePath, const FileEncoding& initial,
                         const LogFilePtr& self = nullptr);
    // Закладки изменились: правка пользователя (а не сверка) — отложенная запись.
    void onBookmarksChanged();
    QString bookmarkSignature() const;
    void updateFiltersOfferBar();
    FileMemoryStore* m_fileMemory = nullptr;
    QTimer* m_memorySaveTimer = nullptr;
    // Закладки файла, прочитанные из памяти или записанные прошлый раз (ключ —
    // FileMemoryStore::normalizedKeyPath): «база» объединения при записи.
    QHash<QString, QVector<RecordId>> m_memoryBase;
    QString m_bookmarkSignature;
    bool m_memoryApplying = false; // applyState памяти, перенос вкладки — без памяти
    bool m_applyingState = false;  // файлы applyState: из памяти — только закладки
    bool m_fromState = false;      // вкладка получила снимок (isBlank)
    // Вывод команд (F-7): спулеры вкладки и их плашки (над остальными).
    void attachCommand(CommandSpooler* spooler);
    QList<CommandSpooler*> m_commands;
    bool m_streamKickPending = false;
    bool anyLoadInFlight() const;
    void retryStreamKick();
    QWidget* m_commandBars = nullptr;
    // Плашка «вернуть фильтры прошлого раза»; закрыта — до закрытия вкладки.
    QFrame* m_filtersOfferBar = nullptr;
    QLabel* m_filtersOfferText = nullptr;
    bool m_filtersOfferDismissed = false;

    // ---- Полный лог (R-3) ---------------------------------------------------------
    FullLogController* m_fullLog = nullptr;
    QFrame* m_fullLogBar = nullptr;
    QLabel* m_fullLogText = nullptr;
    void updateFullLogBar();
    // Закладка, к которой только что перешли, скрыта фильтром: следующий
    // показ полного лога выбирает её, если пользователь вид не трогал.
    struct HiddenTarget {
        RecordKey key;
        quint64 userActions = 0;
    };
    HiddenTarget m_hiddenTarget;
};

#endif // LOGVIEWWIDGET_H
