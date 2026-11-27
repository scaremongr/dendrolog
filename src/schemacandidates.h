#ifndef SCHEMACANDIDATES_H
#define SCHEMACANDIDATES_H

#include "aischemaclient.h"
#include "fieldextractor.h"
#include "logfile.h"
#include "schemafit.h"
#include "schemainference.h"
#include "schemaproposal.h"
#include "schemastore.h"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <memory>

#include "logmodel.h"
class QTimer;

// ============================================================================
// SchemaCandidates — логика окна «Find a Schema» без виджетов (F-17, шаг 5;
// docs/design/F-17.md, раздел 7). Окно (SchemaCandidatesWindow) — только
// вид; тест — mainwindow_smoke и ai_schema_client.
//
// Варианты разбиения для файла вкладки, каждый с мерой SchemaFit на одних и
// тех же строках (SchemaFitJob::sampleLines: первые 300 строк файла и 2000
// равномерно):
//   • схемы каталога — до kCatalogueVariants лучших (SchemaFit::rank), не
//     «нулевые» (покрытие > 0, specificity ≥ 1);
//   • вывод структуры — варианты SchemaInference (полный, осторожный) с
//     заметками;
//   • AI — по запросу пользователя: строки (pickLines, маска и обрезка по
//     выбору в окне согласия) → AiSchemaClient (только с согласием на ЭТО
//     тело) → parseReply → checkTemplate на исходных строках в пуле → вариант,
//     одно уточнение (снова с согласием), отказ — словами.
//
// Расчёт вариантов — QtConcurrent::run по снапшоту всех строк модели
// (scanSnapshot(false)) + QFutureWatcher, поколение и флаг отмены; reset
// модели — пересчёт через 500 мс (растущий лог не перезапускает расчёт на
// каждой порции). Проверка ответа модели — тоже в пуле (тяжёлый /регекс/ не
// замораживает окно; бюджет — SchemaProposal::kCheckBudgetMs).
// ============================================================================
class SchemaCandidates : public QObject {
    Q_OBJECT
public:
    static constexpr int kCatalogueVariants = 3;
    static constexpr int kTableRows = 60;

    struct Variant {
        enum class Source { Catalogue, Inferred, Ai };
        Source source = Source::Inferred;
        QString title;            ///< «Spring Boot», «Inferred (full header)», «AI suggestion»
        QString templateText;     ///< шаблон строки (Inferred, Ai); у каталога — пусто
        SchemaStore::SchemaEntry entry; ///< Catalogue: схема каталога
        FieldExtractorPtr extractor;
        SchemaFit::Result fit;
        QStringList notes;        ///< заметки словами (вывод структуры, «медленный»)
        QString comment;          ///< комментарий модели (Ai)
        bool slow = false;        ///< шаблон с /регексом/
        QString model;            ///< модель, ответившая на запрос (Ai)
    };

    enum class AiState {
        Off,          ///< выключен в Settings → AI
        NoKey,        ///< включён, ключа нет
        Ready,        ///< можно спросить
        NeedsConsent, ///< ждёт согласия (первый запрос или уточнение)
        Sending,
        Checking,     ///< ответ проверяется на строках
        Done,         ///< вариант добавлен
        Failed,       ///< отказ (aiMessage)
    };

    /// Что покажет окно согласия и что уйдёт в сеть (body).
    struct ConsentRequest {
        QString host;
        QString model;
        int lines = 0;            ///< строк лога в запросе
        int characters = 0;       ///< знаков лога в запросе
        QString text;             ///< ТОЧНЫЙ текст сообщений (все реплики)
        QByteArray body;          ///< тело запроса (с системным текстом)
        bool refinement = false;
        bool isEmpty() const { return body.isEmpty(); }
    };

    struct Row {
        qint64 line = 0;
        QString text;
        QStringList values;       ///< по columns()
        bool partial = false;
    };
    struct MissRow {
        qint64 line = 0;
        QString text;
        QString reason;           ///< словами
        bool partial = false;
    };

    explicit SchemaCandidates(QObject* parent = nullptr);
    ~SchemaCandidates() override;

    void setCatalogue(const SchemaStore::SchemaList& catalogue);
    /// Источник строк: модель вкладки и файл (nullptr — файл первой строки).
    /// Запускает расчёт вариантов.
    void setSource(LogModel* model, const LogFile* file);
    LogModel* model() const { return m_model; }
    void recompute();
    bool isComputing() const;
    /// Подпись состояния расчёта / причины, почему вариантов вывода нет.
    QString statusText() const { return m_status; }
    int sampleCount() const { return int(m_samples.size()); }

    const QVector<Variant>& variants() const { return m_variants; }
    /// Принятые ответы модели (в variants() — в конце).
    const QVector<Variant>& aiVariants() const { return m_aiVariants; }
    QStringList columns(int variant) const;
    QVector<Row> sampleRows(int variant, int maxRows = kTableRows) const;
    QVector<MissRow> misses(int variant) const;
    /// Причина промаха словами.
    /// addValueHint — у списка значений подсказать Add Value… (редактор схем:
    /// там у колонки есть меню).
    static QString reasonText(const MatchReport& report, bool addValueHint = false);

    // ---- AI -------------------------------------------------------------------
    struct AiSettings {
        bool enabled = false;
        QString apiKey;
        QString model = QString::fromLatin1(SchemaProposal::kDefaultModel);
        QUrl endpoint = ClaudeProvider::defaultEndpoint();
    };
    void setAiSettings(const AiSettings& settings);
    AiState aiState() const { return m_aiState; }
    QString aiMessage() const { return m_aiMessage; }
    /// Начать: состояние NeedsConsent (окно показывает согласие).
    void askAi();
    /// Текст и тело для окна согласия при этих галочках.
    ConsentRequest prepareAi(const SchemaProposal::MaskOptions& options) const;
    /// Отправка с согласием, выданным окном на request.body.
    bool sendAi(const ConsentRequest& request, AiConsent& consent);
    /// Пользователь отказался от отправки или отменил идущий запрос.
    void cancelAi();
    AiSchemaClient* aiClient() const { return m_client; }

    /// Сохранить шаблон в каталог схем (файл версии 4, <patterns>/…json).
    /// Имя занято в каталоге — к нему добавляется « (2)» и далее. Пусто —
    /// ошибка (error). Возвращает имя сохранённой схемы.
    QString saveTemplate(int variant, const QString& name, QString* error = nullptr) const;

signals:
    void computingChanged();
    void variantsChanged();
    void aiStateChanged();
    /// Нужно согласие пользователя (первый запрос или уточнение).
    void consentNeeded();

private:
    struct Output {
        int generation = 0;
        QVector<SchemaFit::Sample> samples;
        QVector<SchemaFit::Ranked> catalogue;
        SchemaInference::Result inference;
        bool cancelled = false;
    };
    struct AiCheck {
        int generation = 0;
        SchemaProposal::Check check;
        QString model;
    };

    void onComputed();
    void onAiFinished(const AiSchemaClient::Result& result);
    void onAiChecked();
    void setAiState(AiState state, const QString& message = QString());
    void updateAiIdleState();
    /// Сброс подсказчика при смене источника: отмена, ответы и уточнение — прочь.
    void resetAi();
    void rebuildVariants();

    QPointer<LogModel> m_model;
    const LogFile* m_file = nullptr;
    SchemaStore::SchemaList m_catalogue;
    QVector<FieldExtractorPtr> m_catalogueExtractors;
    QVector<SchemaFit::Sample> m_samples;
    QVector<SchemaFit::Ranked> m_ranked;
    SchemaInference::Result m_inference;
    QVector<Variant> m_variants;
    QVector<Variant> m_aiVariants;
    QString m_status;

    QFutureWatcher<Output>* m_watcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
    QTimer* m_resetTimer = nullptr;
    QMetaObject::Connection m_resetConnection;

    AiSettings m_ai;
    AiSchemaClient* m_client = nullptr;
    QFutureWatcher<AiCheck>* m_checkWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> m_checkCancel;
    int m_aiGeneration = 0;
    AiState m_aiState = AiState::Off;
    QString m_aiMessage;
    int m_aiRound = 0;                      ///< 0 — первый запрос, 1 — уточнение
    SchemaProposal::Refinement m_refinement;
};

#endif // SCHEMACANDIDATES_H
