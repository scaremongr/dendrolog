#ifndef AICONSENTDIALOG_H
#define AICONSENTDIALOG_H

#include "aischemaclient.h"
#include "schemacandidates.h"

#include <QDialog>
#include <functional>

class QCheckBox;
class QLabel;
class QPlainTextEdit;

// ============================================================================
// Окно согласия на отправку строк лога модели (F-17, шаг 5; F-17.md,
// раздел 5). Показывается перед КАЖДОЙ отправкой (и перед уточняющим
// запросом); галочки «больше не спрашивать» нет. Показывает хост, модель,
// число строк и символов и ТОЧНЫЙ текст сообщений; «Mask values» и «Cut lines
// to 300 characters» включены по умолчанию, их смена пересобирает текст.
// Неизменный системный текст — под раскрывающейся кнопкой. «Send» выдаёт
// согласие (AiConsent) ровно на показанное тело — consent().
// ============================================================================
class AiConsentDialog : public QDialog {
    Q_OBJECT
public:
    using Builder = std::function<SchemaCandidates::ConsentRequest(const SchemaProposal::MaskOptions&)>;

    AiConsentDialog(Builder builder, bool refinement, const QString& note, QWidget* parent = nullptr);

    /// Запрос, показанный в момент «Send».
    const SchemaCandidates::ConsentRequest& request() const { return m_request; }
    /// Согласие на request().body; пустое, пока не нажато «Send».
    AiConsent& consent() { return m_consent; }

protected:
    void accept() override;

private:
    void rebuild();

    Builder m_builder;
    SchemaCandidates::ConsentRequest m_request;
    AiConsent m_consent;
    QCheckBox* m_mask = nullptr;
    QCheckBox* m_cut = nullptr;
    QLabel* m_summary = nullptr;
    QPlainTextEdit* m_text = nullptr;
};

#endif // AICONSENTDIALOG_H
