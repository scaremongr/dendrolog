#include "entrydetailspanel.h"
#include "appsettings.h"
#include "apptheme.h"
#include "logentry.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QShowEvent>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Пределы: панель перестраивается на каждое движение курсора, поэтому и разбор,
// и итоговый HTML жёстко ограничены — гигантская запись не должна замораживать GUI.
constexpr int kMaxMessageChars      = 256 * 1024; // текст записи в секции Message
constexpr int kMaxFieldValueChars   = 4096;       // значение одного поля в таблице
constexpr int kMaxJsonScanChars     = 256 * 1024; // сканируем на JSON только этот префикс
constexpr int kMaxJsonCandidates    = 64;         // попыток разбора кандидатов на запись
constexpr int kMaxJsonBlocks        = 3;          // показываемых JSON-фрагментов
constexpr int kMaxJsonHtmlChars     = 128 * 1024; // бюджет HTML на один JSON-фрагмент

QColor mixColors(const QColor& a, const QColor& b, qreal weightB)
{
    return QColor(int(a.red()   + (b.red()   - a.red())   * weightB),
                  int(a.green() + (b.green() - a.green()) * weightB),
                  int(a.blue()  + (b.blue()  - a.blue())  * weightB));
}

QString colorSpan(const QColor& color, const QString& escapedText, bool bold = false)
{
    return QStringLiteral("<span style=\"color:%1;%2\">%3</span>")
        .arg(color.name(), bold ? QStringLiteral("font-weight:bold;") : QString(), escapedText);
}

// Экранирование строкового значения в JSON-нотации (обратное разбору):
// показываем строку так, как она выглядела бы в валидном JSON-документе.
QString jsonEscape(const QString& s)
{
    QString out;
    out.reserve(s.size() + 8);
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'\\': out += QLatin1String("\\\\"); break;
        case u'"':  out += QLatin1String("\\\""); break;
        case u'\n': out += QLatin1String("\\n");  break;
        case u'\r': out += QLatin1String("\\r");  break;
        case u'\t': out += QLatin1String("\\t");  break;
        default:
            if (c.unicode() < 0x20)
                // int-каст обязателен: у Qt 6.11 нет перегрузки arg(char16_t).
                out += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Эвристическое обнаружение JSON в произвольном тексте записи.
// ---------------------------------------------------------------------------

// Индекс ПОСЛЕ скобки, закрывающей значение, начатое в text[start] ('{'/'[');
// -1 — баланс не сошёлся до scanLimit. Скобки внутри строковых литералов
// не считаются. Типы скобок не сверяются — смешанные пары отбросит парсер.
int findBalancedEnd(const QString& text, int start, int scanLimit)
{
    int depth = 0;
    bool inString = false;
    for (int i = start; i < scanLimit; ++i) {
        const QChar c = text.at(i);
        if (inString) {
            if (c == u'\\')
                ++i;
            else if (c == u'"')
                inString = false;
        } else if (c == u'"') {
            inString = true;
        } else if (c == u'{' || c == u'[') {
            ++depth;
        } else if (c == u'}' || c == u']') {
            if (--depth == 0)
                return i + 1;
        }
    }
    return -1;
}

// Отсекает валидный, но неинформативный JSON: "[8]" в логах — это thread id,
// а не payload. Непустой объект показываем всегда; массив — если в нём больше
// одного элемента либо единственный элемент сам составной или строка.
bool jsonLooksInteresting(const QJsonDocument& doc)
{
    if (doc.isObject())
        return !doc.object().isEmpty();
    if (doc.isArray()) {
        const QJsonArray arr = doc.array();
        if (arr.isEmpty())
            return false;
        if (arr.size() >= 2)
            return true;
        const QJsonValue v = arr.first();
        return v.isObject() || v.isArray() || v.isString();
    }
    return false;
}

QVector<QJsonDocument> detectJsonBlocks(const QString& text)
{
    QVector<QJsonDocument> blocks;
    const int scanLimit = int(qMin<qsizetype>(text.size(), kMaxJsonScanChars));
    int candidates = 0;
    for (int i = 0; i < scanLimit && blocks.size() < kMaxJsonBlocks; ++i) {
        const QChar c = text.at(i);
        if (c != u'{' && c != u'[')
            continue;
        if (++candidates > kMaxJsonCandidates)
            break;
        const int end = findBalancedEnd(text, i, scanLimit);
        if (end < 0)
            continue; // незакрытая скобка; вложенные кандидаты ещё возможны
        QJsonParseError err;
        const QJsonDocument doc =
            QJsonDocument::fromJson(text.mid(i, end - i).toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && jsonLooksInteresting(doc)) {
            blocks.append(doc);
            i = end - 1; // продолжаем поиск после найденного блока
        }
        // Не распарсилось — идём дальше: попробуем вложенные скобки.
    }
    return blocks;
}

// ---------------------------------------------------------------------------
// JsonHtmlWriter — pretty-print разобранного JSON в подсвеченный HTML.
// Работает по дереву QJsonValue (не по исходному тексту), поэтому вывод
// всегда корректно отформатирован независимо от исходного вида фрагмента.
// ---------------------------------------------------------------------------
class JsonHtmlWriter {
public:
    JsonHtmlWriter()
    {
        const AppTheme& t = AppTheme::instance();
        m_keyColor     = t.logInfo;
        m_stringColor  = t.syntaxString;
        m_numberColor  = t.syntaxNumber;
        m_keywordColor = t.syntaxGuid; // true/false/null
    }

    QString toHtml(const QJsonDocument& doc, const QColor& mutedColor)
    {
        m_out.clear();
        m_truncated = false;
        writeValue(doc.isObject() ? QJsonValue(doc.object()) : QJsonValue(doc.array()), 0);
        if (m_truncated)
            m_out += QLatin1Char('\n')
                   + colorSpan(mutedColor, QStringLiteral("… (truncated)"));
        return m_out;
    }

private:
    bool overBudget()
    {
        if (m_out.size() > kMaxJsonHtmlChars)
            m_truncated = true;
        return m_truncated;
    }

    void writeValue(const QJsonValue& v, int indent)
    {
        if (overBudget())
            return;
        switch (v.type()) {
        case QJsonValue::Object: writeObject(v.toObject(), indent); break;
        case QJsonValue::Array:  writeArray(v.toArray(), indent);   break;
        case QJsonValue::String:
            m_out += colorSpan(m_stringColor,
                               (QLatin1Char('"') + jsonEscape(v.toString()) + QLatin1Char('"'))
                                   .toHtmlEscaped());
            break;
        case QJsonValue::Double: {
            const double d = v.toDouble();
            // Целые значения печатаем без дробной части (как в исходном JSON).
            const bool integral = std::floor(d) == d && std::abs(d) < 1e15;
            m_out += colorSpan(m_numberColor,
                               integral ? QString::number(qint64(d))
                                        : QString::number(d, 'g', 15));
            break;
        }
        case QJsonValue::Bool:
            m_out += colorSpan(m_keywordColor, v.toBool() ? QStringLiteral("true")
                                                          : QStringLiteral("false"));
            break;
        default:
            m_out += colorSpan(m_keywordColor, QStringLiteral("null"));
            break;
        }
    }

    void writeObject(const QJsonObject& obj, int indent)
    {
        if (obj.isEmpty()) {
            m_out += QLatin1String("{}");
            return;
        }
        m_out += QLatin1String("{\n");
        const QString pad((indent + 1) * 2, QLatin1Char(' '));
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (overBudget())
                break;
            m_out += pad;
            m_out += colorSpan(m_keyColor,
                               (QLatin1Char('"') + jsonEscape(it.key()) + QLatin1Char('"'))
                                   .toHtmlEscaped());
            m_out += QLatin1String(": ");
            writeValue(it.value(), indent + 1);
            if (std::next(it) != obj.end())
                m_out += QLatin1Char(',');
            m_out += QLatin1Char('\n');
        }
        m_out += QString(indent * 2, QLatin1Char(' ')) + QLatin1Char('}');
    }

    void writeArray(const QJsonArray& arr, int indent)
    {
        if (arr.isEmpty()) {
            m_out += QLatin1String("[]");
            return;
        }
        m_out += QLatin1String("[\n");
        const QString pad((indent + 1) * 2, QLatin1Char(' '));
        for (int i = 0; i < arr.size(); ++i) {
            if (overBudget())
                break;
            m_out += pad;
            writeValue(arr.at(i), indent + 1);
            if (i + 1 < arr.size())
                m_out += QLatin1Char(',');
            m_out += QLatin1Char('\n');
        }
        m_out += QString(indent * 2, QLatin1Char(' ')) + QLatin1Char(']');
    }

    QString m_out;
    QColor m_keyColor, m_stringColor, m_numberColor, m_keywordColor;
    bool m_truncated = false;
};

} // namespace

// ===========================================================================

EntryDetailsPanel::EntryDetailsPanel(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->document()->setDocumentMargin(8);
    layout->addWidget(m_browser);

    clearEntry();
}

void EntryDetailsPanel::showEntry(const std::shared_ptr<LogEntry>& line,
                                  const QVector<std::shared_ptr<LogEntry>>& logicalLines,
                                  const QStringList& fieldNames)
{
    m_line = line;
    m_logicalLines = logicalLines;
    m_fieldNames = fieldNames;
    scheduleRebuild();
}

void EntryDetailsPanel::clearEntry()
{
    m_line.reset();
    m_logicalLines.clear();
    m_fieldNames.clear();
    scheduleRebuild();
}

void EntryDetailsPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_dirty) {
        m_browser->setHtml(buildHtml());
        m_dirty = false;
    }
}

void EntryDetailsPanel::scheduleRebuild()
{
    if (!isVisible()) {
        m_dirty = true;
        return;
    }
    m_browser->setHtml(buildHtml());
    m_dirty = false;
}

QString EntryDetailsPanel::buildHtml() const
{
    const QPalette pal = m_browser->palette();
    const QColor textColor = pal.color(QPalette::Text);
    const QColor muted = mixColors(textColor, pal.color(QPalette::Base), 0.45);
    const QString monoStyle =
        QStringLiteral("font-family:'%1'; white-space:pre-wrap;")
            .arg(AppSettings::instance().fontFamily());

    const auto sectionHeader = [&muted](const QString& title) {
        return QStringLiteral(
                   "<div style=\"color:%1;font-weight:bold;margin-top:12px;\">%2</div>")
            .arg(muted.name(), title);
    };

    if (!m_line) {
        return QStringLiteral("<div style=\"color:%1;\">%2</div>")
            .arg(muted.name(),
                 tr("Select a row in the log view to see its details."));
    }

    QString html;

    // ---- Шапка: уровень, таймстамп, файл и позиция -------------------------
    {
        const AppTheme& t = AppTheme::instance();
        QString head;
        if (m_line->level() != LogLevel::Unknown)
            head += colorSpan(t.forLevel(m_line->level()), LevelToStr(m_line->level()),
                              /*bold=*/true)
                  + QLatin1String("&nbsp;&nbsp;");
        if (m_line->timestamp().isValid())
            head += QStringLiteral("<b>%1</b>")
                        .arg(m_line->timestamp()
                                 .toString(QStringLiteral("dd.MM.yyyy HH:mm:ss.zzz")));
        else
            head += colorSpan(muted, tr("no timestamp"));
        html += QStringLiteral("<div>%1</div>").arg(head);

        QStringList location;
        if (m_line->sourceFile())
            location << m_line->sourceFile()->filePath;
        if (m_logicalLines.size() > 1)
            location << tr("lines %1–%2 (%3)")
                            .arg(m_logicalLines.first()->originalLineNumber())
                            .arg(m_logicalLines.last()->originalLineNumber())
                            .arg(m_logicalLines.size());
        else
            location << tr("line %1").arg(m_line->originalLineNumber());
        // У свободного текста id логической записи номинален (весь файл —
        // «запись #0»), не показываем его, чтобы не путать.
        if (!m_line->isPlainText())
            location << tr("entry #%1").arg(m_line->logicalEntryId());
        html += QStringLiteral("<div style=\"color:%1;\">%2</div>")
                    .arg(muted.name(), location.join(QStringLiteral(" · ")).toHtmlEscaped());
    }

    // ---- Извлечённые поля ---------------------------------------------------
    // У continuation-строк fields пуст — берём их с первой строки логической
    // записи, у которой они извлеклись (обычно это первая строка). Для строк
    // свободного текста секцию не показываем вовсе: полей там нет по
    // определению, а примечание «не совпало со схемой» на каждой строке
    // не-лог файла — только шум.
    if (!m_fieldNames.isEmpty() && !m_line->isPlainText()) {
        const LogEntry* fieldsSource = nullptr;
        for (const auto& e : m_logicalLines) {
            if (e && !e->fields().isEmpty()) {
                fieldsSource = e.get();
                break;
            }
        }
        html += sectionHeader(tr("Fields"));
        if (fieldsSource) {
            html += QLatin1String("<table cellspacing=\"0\" cellpadding=\"2\">");
            const int count = int(qMin(m_fieldNames.size(),
                                       qsizetype(fieldsSource->fields().size())));
            for (int i = 0; i < count; ++i) {
                QString value;
                if (fieldsSource->fields().has(i)) {
                    value = fieldsSource->fields().get(i, fieldsSource->message()).toString();
                    if (value.size() > kMaxFieldValueChars)
                        value = value.left(kMaxFieldValueChars) + QStringLiteral("…");
                    value = value.toHtmlEscaped();
                } else {
                    value = colorSpan(muted, QStringLiteral("—"));
                }
                html += QStringLiteral(
                            "<tr><td style=\"color:%1;\">%2&nbsp;&nbsp;</td>"
                            "<td style=\"%3\">%4</td></tr>")
                            .arg(muted.name(), m_fieldNames.at(i).toHtmlEscaped(),
                                 monoStyle, value);
            }
            html += QLatin1String("</table>");
        } else {
            html += QStringLiteral("<div style=\"color:%1;\">%2</div>")
                        .arg(muted.name(),
                             tr("The line does not match the extraction schema."));
        }
    }

    // ---- Полный текст логической записи -------------------------------------
    QString message;
    {
        QStringList parts;
        parts.reserve(m_logicalLines.size());
        for (const auto& e : m_logicalLines)
            if (e)
                parts << e->message();
        message = parts.join(QLatin1Char('\n'));
    }
    const bool messageTruncated = message.size() > kMaxMessageChars;
    if (messageTruncated)
        message.truncate(kMaxMessageChars);

    html += sectionHeader(m_logicalLines.size() > 1 ? tr("Message (%1 lines)")
                                                          .arg(m_logicalLines.size())
                                                    : tr("Message"));
    html += QStringLiteral("<div style=\"%1\">%2</div>")
                .arg(monoStyle, message.toHtmlEscaped());
    if (messageTruncated)
        html += QStringLiteral("<div style=\"color:%1;\">%2</div>")
                    .arg(muted.name(), tr("… message truncated for display"));

    // ---- Эвристика: JSON-фрагменты в тексте записи --------------------------
    const QVector<QJsonDocument> jsonBlocks = detectJsonBlocks(message);
    JsonHtmlWriter writer;
    for (int i = 0; i < jsonBlocks.size(); ++i) {
        html += sectionHeader(jsonBlocks.size() > 1 ? tr("JSON #%1").arg(i + 1)
                                                    : tr("JSON"));
        html += QStringLiteral("<div style=\"%1\">%2</div>")
                    .arg(monoStyle, writer.toHtml(jsonBlocks.at(i), muted));
    }

    return html;
}
