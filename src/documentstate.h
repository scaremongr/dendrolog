#ifndef DOCUMENTSTATE_H
#define DOCUMENTSTATE_H

#include "bookmarkstore.h"
#include "filterruleset.h"
#include "logentry.h"
#include "recordid.h"
#include "textencoding.h"
#include "textmatchhighlighter.h"

#include <QDateTime>
#include <QJsonObject>
#include <QSet>
#include <QString>
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
//   • SessionState (R-5) здесь не заведён: формат вкладки от него не зависит,
//     сессия — массив TabState плюс схема и выбор колонок.
// ============================================================================

struct ViewState {
    static constexpr int kFormatVersion = 1;

    QSet<LogLevel> levels;          // пусто — показывать все
    QDateTime timeFrom, timeTo;     // невалидная — граница открыта
    FilterRuleSet textRules;        // привязка к колонкам — при применении (bindFields)
    QVector<HighlightPattern> markers;

    // Текущая строка и её место на экране: смещение верха строки от верха
    // вьюпорта в пикселях. selected — строку выбрал пользователь (выделена,
    // якорь инварианта 10); иначе это просто верхняя строка экрана.
    std::optional<SourceIdentity> currentSource;
    RecordId current;
    int viewportOffset = 0;
    bool currentSelected = false;

    bool followTail = false;
    bool autoReload = false;

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

// Файл вкладки: идентичность по концу прочитанного и ручной выбор кодировки.
struct TabSource {
    SourceIdentity source;
    std::optional<TextEncoding> userEncoding; // nullopt — решает автоматика

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

#endif // DOCUMENTSTATE_H
