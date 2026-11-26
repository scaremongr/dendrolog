#ifndef DOCUMENTSTATE_H
#define DOCUMENTSTATE_H

#include "bookmarkstore.h"
#include "filterruleset.h"
#include "logentry.h"
#include "lineformat.h"
#include "recordid.h"
#include "textencoding.h"
#include "textmatchhighlighter.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

// ============================================================================
// Состояние документа (вкладки) вне вкладки — R-4, шаг 1
// (docs/design/R-5.md, «Модель»). Чистые данные и JSON, без QObject и без
// виджетов: снимает и применяет их LogViewWidget (captureState / applyState),
// на диск пишет память файла (R-4, шаг 2) и сессия (R-5).
//
// Второй копии правды здесь нет: во время работы фильтры и маркеры живут в
// LogModel, закладки — в BookmarkStore, позиция — в LogListView. Эти
// структуры — только снимок для переноса.
//
// Формат (версия 1; эталон — tests/document_state/tab-state-v1.json):
//   TabState  {"format":"dendrolog-tab-state","version":1,
//              "sources":[{"source":{SourceIdentity},"encoding":"Windows-1251"}],
//              "bookmarks":[{BookmarkState}…], "view":{ViewState}}
//   ViewState {"version":1,"levels":["error",…],"timeFrom":…,"timeTo":…,
//              "textRules":{FilterRuleSet},"markers":[HighlightPattern…],
//              "current":{"source":{…},"record":{RecordId},"offset":120,
//                         "selected":true},
//              "followTail":false,"autoReload":true}
// Пустое (нет уровней, границы времени, правил, маркеров, текущей строки) не
// пишется. Объекты R-1 (SourceIdentity, RecordId) и закладки (BookmarkState)
// несут свои version / hash и читаются своими fromJson.
//
// Чтение — с явным отказом, как у SourceIdentity::fromJson: чужой format или
// version, неверный тип поля, неизвестное имя уровня или кодировки, битый
// вложенный объект — nullopt и причина; лишние ключи не мешают (их может
// добавить следующая версия того же формата).
//
// Решения (docs/design/R-5.md, «Сделано → PR 2»):
//   • закладка — самодостаточный BookmarkState (источник внутри), а не индекс
//     в sources: у неё свой формат с R-1, и закладка файла, пропавшего из
//     вкладки, не теряет источник;
//   • текущая строка — тоже SourceIdentity + RecordId, без индекса: ViewState
//     годится памяти одного файла (R-4) без TabState вокруг;
//   • кодировка — только ВЫБОР пользователя (FileEncoding::Source::User);
//     решение автоматики не хранится: файл решает его заново тем же правилом;
//   • SessionState (R-5) — массив TabState плюс схема и выбор колонок; формат
//     вкладки от него не зависит (файл сессии — src/sessionfile.h).
//   • схема полей вкладки (F-5, PR 1) — необязательное поле ViewState
//     «fields» (FieldSettings): версия формата не растёт, читатель версии 1
//     незнакомый ключ пропускает; снимок без поля — схему решает тот, кто
//     применяет (сессия 0.6 — схема верхнего уровня, иначе схема по умолчанию).
// ============================================================================

// Схема полей вкладки (F-5, PR 1): схема, показ выбранных колонок и выбор
// колонок.
// Схема — определением (как SchemaEntry::schema) и именем в каталоге для
// показа; при применении её ищут в каталоге по определению
// (SessionController::chooseSchema), имя не важно.
//   {"schema":{"name":"Spring Boot","definition":{…}},"showFields":true,
//    "visibleFields":["Level","Message"]}
// Нет схемы — нет ключа schema; visibleFields пусто — все колонки.
// showFields (F-17, шаг 3) — галочка дока «Show selected fields only»:
// только показ. Поля и правила по полю работают при любом её значении.
// Снимки до шага 3 несут ключ «extraction» («Filter blocks») — он читается
// как showFields.
struct FieldSettings {
    QString schemaName;          // имя в каталоге; пусто — без имени
    QString schema;              // определение; пусто — без схемы
    bool showFields = false;     // «Show selected fields only» (было «Filter blocks»)
    QStringList visibleFields;   // пусто — все колонки

    bool hasSchema() const { return !schema.isEmpty(); }
    bool operator==(const FieldSettings& o) const
    {
        return schemaName == o.schemaName && schema == o.schema && showFields == o.showFields
            && visibleFields == o.visibleFields;
    }
    bool operator!=(const FieldSettings& o) const { return !(*this == o); }

    QJsonObject toJson() const;
    static std::optional<FieldSettings> fromJson(const QJsonObject& json, QString* error = nullptr);
};

struct ViewState {
    static constexpr int kFormatVersion = 1;

    QSet<LogLevel> levels;          // пусто — показывать все
    QDateTime timeFrom, timeTo;     // невалидная — граница открыта
    FilterRuleSet textRules;        // привязка к колонкам — при применении (bindFields)
    QVector<HighlightPattern> markers;
    // Скрытые файлы (фильтр по файлу, U-23): индексы в TabState::sources.
    // Необязательное поле «hiddenFiles» — читатель версии 1 незнакомые ключи
    // пропускает, поэтому версия формата не растёт; без поля — ничего не скрыто.
    QVector<int> hiddenSources;

    // Текущая строка и её место на экране: смещение верха строки от верха
    // вьюпорта в пикселях. selected — строку выбрал пользователь (выделена,
    // якорь инварианта 10); иначе это просто верхняя строка экрана.
    std::optional<SourceIdentity> currentSource;
    RecordId current;
    int viewportOffset = 0;
    bool currentSelected = false;

    bool followTail = false;
    bool autoReload = false;

    // Схема полей вкладки (F-5, PR 1), необязательное поле «fields»; nullopt —
    // снимок её не несёт (версии до F-5).
    std::optional<FieldSettings> fields;

    bool hasCurrent() const { return currentSource.has_value() && current.isValid(); }
    // Есть ли что применять фильтром (уровни, время, текстовые правила).
    bool hasFilters() const;

    QJsonObject toJson() const;
    static std::optional<ViewState> fromJson(const QJsonObject& json, QString* error = nullptr);

    // Имена уровней в формате ("trace", "debug", "info", "warn", "error",
    // "fatal", "unknown") — свои, не LevelToStr: формат не должен меняться
    // вместе с подписями.
    static QString levelName(LogLevel level);
    static std::optional<LogLevel> levelFromName(QStringView name);
};

// Файл вкладки: идентичность по концу прочитанного, ручной выбор кодировки и
// своя схема полей файла слитой вкладки (F-5, PR 3).
//   {"source":{…},"encoding":"Windows-1251",
//    "schema":{"name":"nginx access","definition":{…}}}
// schema — необязательное поле (версия формата не растёт): только если схема
// файла отличается от схемы вкладки (ViewState.fields); у памяти файла слитой
// вкладки — схема файла. "schema":null — файл без схемы во вкладке со схемой.
// Значение — FieldSettings, у которого значат только schemaName и schema.
struct TabSource {
    SourceIdentity source;
    std::optional<TextEncoding> userEncoding; // nullopt — решает автоматика
    std::optional<FieldSettings> schema;      // nullopt — схема вкладки
    // Выбор формата строк пользователем (F-17, шаг 7; lineformat.h);
    // nullopt — решает автоматика. Ключ «lineFormat» необязателен.
    std::optional<LineFormat> lineFormat;

    QJsonObject toJson() const;
    static std::optional<TabSource> fromJson(const QJsonObject& json, QString* error = nullptr);
};

struct TabState {
    static constexpr int kFormatVersion = 1;
    static constexpr char kFormatName[] = "dendrolog-tab-state";

    QVector<TabSource> sources;     // в порядке файлов вкладки
    QVector<BookmarkState> bookmarks;
    ViewState view;

    QJsonObject toJson() const;
    static std::optional<TabState> fromJson(const QJsonObject& json, QString* error = nullptr);
};

// ============================================================================
// Сессия (R-5, шаг 1; docs/design/R-5.md, «Модель», «Три хранилища»): вкладки
// окна целиком, активная вкладка, схема полей и выбор колонок. Пишется в файл
// *.dlsession по команде пользователя (SessionFile) — это снимок, сам он не
// обновляется.
//
// Формат (версия 1; эталон — tests/document_state/session-v1.json):
//   {"format":"dendrolog-session","version":1,
//    "schema":{"name":"Spring Boot","definition":{…шаблон строки, версия 4…}},
//    "fieldExtraction":true,"visibleFields":["Level","Message"],
//    "activeTab":1,
//    "files":[{"path":"/logs/app.log","relative":"../logs/app.log"}],
//    "tabs":[{TabState}…]}
// Пути внутри вкладок — абсолютные (форматы R-1 и TabState относительных не
// принимают). Таблица files рядом с каждым абсолютным путём держит
// относительный — к каталогу файла сессии: сессия, перенесённая вместе с
// логами в другую папку или на другую машину, находит их по нему
// (SessionFile::rebind). Относительный не пишется, если его не построить
// (другой диск на Windows); разделитель в нём — '/' на всех платформах.
// Файл с другой ОС (R-8): абсолютный путь — по правилу SavedPath (путь
// Windows читается на Linux и наоборот), источник по чужому пути — только по
// относительному; у чужого пути относительный при записи не строится заново
// от него самого (см. relativeTargets).
// Нет схемы — нет ключа schema; visibleFields пусто — все колонки.
// Схема на вкладку (F-5, PR 1) — у каждой вкладки в view.fields; верхний
// уровень — схема активной вкладки (её читает 0.6, ею получают схему вкладки
// без своей — сессии 0.6).
//
// Чтение — с явным отказом: чужой format, версия не 1, битые поля и вложенные
// вкладки — nullopt и причина; большая версия отличается от битых данных
// (newerVersion). Лишние ключи не мешают.
// ============================================================================

struct SessionState {
    static constexpr int kFormatVersion = 1;
    static constexpr char kFormatName[] = "dendrolog-session";
    static constexpr char kFileSuffix[] = "dlsession"; // без точки

    QString schemaName;          // пусто вместе с schema — без схемы
    QString schema;              // определение схемы (как SchemaEntry::schema)
    bool showFields = false;     // ключ «fieldExtraction» (его читает 0.6): показ полей
    QStringList visibleFields;   // пусто — все колонки
    int activeTab = -1;          // индекс в tabs; -1 — не задана
    QVector<TabState> tabs;

    // Прочитанное из таблицы files: абсолютный путь (как сохранён) →
    // относительный к каталогу файла сессии. toJson его не читает — строит
    // заново от sessionFilePath.
    // Путь — SavedPath::normalized, как у SourceIdentity.
    QHash<QString, QString> relativePaths;
    // Для пересохранения (заполняет SessionController::capture): путь
    // пропавшего источника → локальный абсолютный путь, куда указывал
    // относительный путь прежнего файла сессии. toJson строит относительный
    // от него, а не от сохранённого пути: у пути другой ОС (R-8) иначе
    // относительного не было бы, и пересохранённая на Linux сессия с Windows
    // потеряла бы запасной путь. В файл не пишется.
    QHash<QString, QString> relativeTargets;

    bool hasSchema() const { return !schema.isEmpty(); }
    // Схема верхнего уровня как схема вкладки (F-5): её получает вкладка
    // снимка без своей (сессии 0.6). Верхний уровень пишется по активной
    // вкладке — его прочитает 0.6.
    FieldSettings topLevelFields() const
    {
        return FieldSettings{ schemaName, schema, showFields, visibleFields };
    }
    // Сравнение сохранённых путей — SavedPath::same (путь Windows без учёта
    // регистра на любой ОС, как у SourceIdentity).
    static bool samePath(const QString& a, const QString& b);
    // Относительный путь из таблицы files для абсолютного; пусто — нет.
    QString relativePathOf(const QString& path) const;
    QString relativeTargetOf(const QString& path) const;
    // Все абсолютные пути вкладок (без повторов): файлы, источники закладок и
    // текущей строки.
    QStringList paths() const;

    // sessionFilePath — куда пишется файл: относительные пути — к его каталогу.
    // Пусто — без относительных путей.
    QJsonObject toJson(const QString& sessionFilePath = QString()) const;
    static std::optional<SessionState> fromJson(const QJsonObject& json, QString* error = nullptr,
                                                bool* newerVersion = nullptr);
};

#endif // DOCUMENTSTATE_H
