#ifndef SCHEMAPROPOSAL_H
#define SCHEMAPROPOSAL_H

#include "schemafit.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

// ============================================================================
// SchemaProposal — ядро нейро-подсказчика (F-17, шаг 5; docs/design/F-17.md,
// раздел 5). Модель не разбирает лог: получает несколько десятков строк и
// возвращает ШАБЛОН строки (linetemplate.h) — одну строку. Здесь всё, что не
// сеть и не окно:
//
//   • какие строки уходят (pickLines: строки-кандидаты SchemaFit, начало
//     файла и равномерно по нему, без повторов);
//   • маскировка (maskLine): метки времени и слова уровней целы; IP — адреса
//     документационных диапазонов (192.0.2.x, 2001:db8::x; один адрес — один
//     и тот же); почта — user@example.com; UUID и hex от 8 знаков — «a» той
//     же длины (только hex-слово с буквой или «0x»; число из одних цифр —
//     цифрами); все остальные цифры — «0». Маскировка уменьшает, а не
//     исключает утечку: слова, имена, пути остаются (так и сказано в окне
//     согласия);
//   • текст запроса (requestBody — тело Claude Messages API; системный текст
//     — ресурс :/ai/schema-system.txt, грамматика шаблона, таблица видов,
//     правила, три примера) и уточняющего запроса (Refinement);
//   • разбор ответа (parseReply: stop_reason refusal / max_tokens — до чтения
//     содержимого; первый text-блок — JSON {"template", "comment"});
//   • проверка шаблона (checkTemplate): ответ — НЕДОВЕРЕННЫЕ данные. Длиннее
//     kMaxTemplateChars или больше kMaxFields полей — отказ; не компилируется
//     — уточнение с текстом ошибки; SchemaFit на ИСХОДНЫХ (немаскированных)
//     строках с бюджетом времени — не уложился (тяжёлый /регекс/) — отказ;
//     покрытие ниже kAcceptCoverage — уточнение с не подошедшими строками.
//     Уточнение — одно; во втором круге (finalRound) компилирующийся шаблон
//     принимается с тем покрытием, что есть. Ничего не применяется само.
//
// Приватность (раздел 5): в запросе только строки (маскированные по выбору
// пользователя) и неизменный системный текст; имени файла, путей, имени
// пользователя и машины нет. Функции чистые, без состояния; сети не знают.
// ============================================================================

namespace SchemaProposal {

constexpr int kMaxTemplateChars = 2000;
constexpr int kMaxFields = 32;
constexpr int kDefaultLines = 40;       ///< строк в запросе
constexpr int kCutChars = 300;          ///< «Cut lines to 300 characters»
constexpr int kRefineLines = 10;        ///< не подошедших строк в уточнении
constexpr double kAcceptCoverage = 0.90;
constexpr qint64 kCheckBudgetMs = 1500; ///< бюджет SchemaFit на шаблон ответа
constexpr int kMaxTokens = 16000;

inline constexpr char kDefaultModel[] = "claude-opus-5-5";
inline constexpr char kDefaultEffort[] = "medium";
inline constexpr char kApiVersion[] = "2026-10-21";
inline constexpr char kBetaFallback[] = "server-side-fallback-2026-07-01";
inline constexpr char kDefaultHost[] = "api.anthropic.com";

struct MaskOptions {
    bool mask = true;       ///< «Mask values»
    bool cut = true;        ///< «Cut lines to 300 characters»
    int cutChars = kCutChars;
};

/// Маскировка значений строки (см. заголовок). Детерминированна: один и
/// тот же IP строки получает один и тот же адрес в пределах ipMap (nullptr —
/// своя таблица на вызов).
QString maskLine(const QString& line, QHash<QString, QString>* ipMap = nullptr);

/// Строки для запроса: кандидаты (SchemaFit::corpus) — первые
/// maxLines / 2 по порядку и остальные равномерно; без пустых и повторов
/// (после подготовки). Подготовка — маска и обрезка по options.
/// ipMap (может быть nullptr) — таблица IP, общая с другими строками того
/// же запроса (не подошедшие строки уточнения).
QStringList pickLines(const QVector<SchemaFit::Sample>& samples, const MaskOptions& options,
                      int maxLines = kDefaultLines, QHash<QString, QString>* ipMap = nullptr);
/// Подготовка набора строк одними правилами (одна таблица IP на набор;
/// ipMap — общая с другими строками запроса).
QStringList prepareLines(const QStringList& lines, const MaskOptions& options,
                         QHash<QString, QString>* ipMap = nullptr);

struct RequestOptions {
    QString model = QString::fromLatin1(kDefaultModel);
    QString effort = QString::fromLatin1(kDefaultEffort);
    int maxTokens = kMaxTokens;
};

/// Уточняющий запрос: прежний шаблон и комментарий, ошибка компиляции или
/// мера словами, до kRefineLines не подошедших строк (уже подготовленных).
struct Refinement {
    QString previousTemplate;
    QString previousComment;
    QString problem;
    QStringList failedLines;
    bool isEmpty() const { return previousTemplate.isEmpty() && problem.isEmpty(); }
};

/// Системный текст (ресурс :/ai/schema-system.txt). Неизменен.
QString systemPrompt();
/// Текст пользователя с образцами — ровно то, что покажет окно согласия.
QString userText(const QStringList& preparedLines);
/// Прежний ответ модели в уточняющем запросе (реплика assistant) — ровно
/// как в теле.
QString previousAnswerText(const Refinement& refinement);
/// Текст уточнения (последнее сообщение пользователя).
QString refinementText(const Refinement& refinement);
/// Тело POST /v1/messages (компактный JSON, ключи в стабильном порядке).
QByteArray requestBody(const QStringList& preparedLines, const RequestOptions& options,
                       const Refinement& refinement = {});

/// Заголовки запроса, кроме ключа: content-type, anthropic-version и (у
/// моделей с серверными запасными) anthropic-beta. Ключ ставит клиент.
QList<QPair<QByteArray, QByteArray>> requestHeaders(const RequestOptions& options);

struct Reply {
    enum class Status {
        Ok,
        Refused,      ///< stop_reason refusal — модель отклонила запрос
        Truncated,    ///< stop_reason max_tokens — ответ оборван
        NotJson,      ///< тело или текст ответа — не JSON
        NoTemplate,   ///< JSON без строки template
    };
    Status status = Status::NotJson;
    QString templateText;
    QString comment;
    QString message;          ///< для человека (английский — язык интерфейса)
    QString model;            ///< модель, ответившая на запрос (поле model)
    bool ok() const { return status == Status::Ok; }
};
/// Разбор тела ответа 200 OK Messages API.
Reply parseReply(const QByteArray& body);

struct Check {
    enum class Status {
        Accepted,         ///< показать вариантом
        NeedsRefinement,  ///< один уточняющий запрос (refinement)
        Rejected,         ///< отказ (message)
    };
    Status status = Status::Rejected;
    QString templateText;
    QString comment;
    QString message;          ///< причина отказа или уточнения словами
    SchemaFit::Result fit;    ///< на исходных строках (если компилируется)
    bool slow = false;        ///< в шаблоне /регекс/ — поля во view будут в фоне
    Refinement refinement;    ///< для NeedsRefinement; failedLines — СЫРЫЕ строки
};
/// Проверка шаблона из ответа на исходных строках (см. заголовок).
/// budget — nullptr: kCheckBudgetMs без флага отмены.
Check checkTemplate(const QString& templateText, const QString& comment,
                    const QVector<SchemaFit::Sample>& lines, bool finalRound,
                    const SchemaFit::Budget* budget = nullptr);

} // namespace SchemaProposal

#endif // SCHEMAPROPOSAL_H
