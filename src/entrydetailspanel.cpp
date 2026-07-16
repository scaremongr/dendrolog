#include "entrydetailspanel.h"
#include "appsettings.h"
#include "apptheme.h"
#include "cardframe.h"
#include "logentry.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLabel>
#include <QScrollBar>
#include <QSettings>
#include <QShowEvent>
#include <QTextBlock>
#include <QTextBrowser>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
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

// Группа настроек панели в общем DendroLog.ini (см. AppSettings::iniFilePath).
const QLatin1String kSettingsGroup("EntryDetailsPanel");

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
    layout->setContentsMargins(3, 3, 3, 0);
    layout->setSpacing(3);

    // ---- Шапка: какие секции показывать (стиль Text Filters: CardFrame
    // с плоскими tool-кнопками; включённая секция тонируется highlight'ом).
    auto* card = new CardFrame(this);
    card->setAccentColor(palette().color(QPalette::Mid)); // нейтральный акцент
    auto* toggleRow = new QHBoxLayout();
    toggleRow->setSpacing(2);
    toggleRow->addWidget(new QLabel(tr("Show:"), card));

    const QColor hl = palette().color(QPalette::Highlight);
    const QString toggleStyle = QStringLiteral(
        "QToolButton { padding: 1px 7px; border: 1px solid transparent; border-radius: 3px; }"
        "QToolButton:hover { background-color: rgba(%1,%2,%3,30); }"
        "QToolButton:checked { background-color: rgba(%1,%2,%3,60);"
        " border: 1px solid rgba(%1,%2,%3,120); }")
        .arg(hl.red()).arg(hl.green()).arg(hl.blue());
    const auto makeToggle = [&](const QString& text, const QString& toolTip) {
        QToolButton* btn = card->makeToolButton(text, toolTip);
        btn->setCheckable(true);
        btn->setChecked(true);
        btn->setStyleSheet(toggleStyle);
        connect(btn, &QToolButton::toggled, this, [this]() {
            saveSectionSettings();
            scheduleRebuild();
        });
        toggleRow->addWidget(btn);
        return btn;
    };
    m_headerButton  = makeToggle(tr("Header"),
        tr("Level, timestamp, source file and line numbers"));
    m_fieldsButton  = makeToggle(tr("Fields"),
        tr("Fields extracted by the active schema"));
    m_messageButton = makeToggle(tr("Message"),
        tr("Full text of the logical entry (all its lines)"));
    m_jsonButton    = makeToggle(tr("JSON"),
        tr("Pretty-printed JSON fragments found in the entry text"));
    toggleRow->addStretch(1);
    card->rowsLayout()->addLayout(toggleRow);
    layout->addWidget(card);

    m_browser = new QTextBrowser(this);
    m_browser->setOpenLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->document()->setDocumentMargin(8);
    // Ссылки "copy:*" в заголовках секций — копирование в буфер обмена.
    connect(m_browser, &QTextBrowser::anchorClicked,
            this, &EntryDetailsPanel::onAnchorClicked);
    layout->addWidget(m_browser, 1);

    loadSectionSettings();
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
    if (m_dirty)
        rebuildNow();
}

void EntryDetailsPanel::scheduleRebuild()
{
    if (!isVisible()) {
        m_dirty = true;
        return;
    }
    rebuildNow();
}

// ---------------------------------------------------------------------------
// Персист выбора секций: общий DendroLog.ini, группа [EntryDetailsPanel].
// Пишем сразу по клику — переключение секций редкое, а состояние не должно
// теряться при аварийном завершении.
// ---------------------------------------------------------------------------

void EntryDetailsPanel::loadSectionSettings()
{
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(kSettingsGroup);
    const auto restore = [&s](QToolButton* btn, const char* key) {
        btn->blockSignals(true); // без save/rebuild на каждую кнопку
        btn->setChecked(s.value(QLatin1String(key), true).toBool());
        btn->blockSignals(false);
    };
    restore(m_headerButton,  "showHeader");
    restore(m_fieldsButton,  "showFields");
    restore(m_messageButton, "showMessage");
    restore(m_jsonButton,    "showJson");
    const QStringList collapsed =
        s.value(QStringLiteral("collapsedFields")).toStringList();
    m_collapsedFields = QSet<QString>(collapsed.begin(), collapsed.end());
    s.endGroup();
}

void EntryDetailsPanel::saveSectionSettings() const
{
    QSettings s(AppSettings::iniFilePath(), QSettings::IniFormat);
    s.beginGroup(kSettingsGroup);
    s.setValue(QStringLiteral("showHeader"),  m_headerButton->isChecked());
    s.setValue(QStringLiteral("showFields"),  m_fieldsButton->isChecked());
    s.setValue(QStringLiteral("showMessage"), m_messageButton->isChecked());
    s.setValue(QStringLiteral("showJson"),    m_jsonButton->isChecked());
    s.setValue(QStringLiteral("collapsedFields"),
               QStringList(m_collapsedFields.begin(), m_collapsedFields.end()));
    s.endGroup();
}

// ---------------------------------------------------------------------------
// Перестроение с сохранением позиции чтения.
//
// Прокрутка запоминается не в пикселях, а как «секция в верху вьюпорта +
// смещение внутри неё»: высота секций от записи к записи меняется, а вот
// сам интерес пользователя («я смотрю JSON») — нет. Если прежней секции в
// новой записи не оказалось (например, JSON #2), откатываемся к ближайшей
// предыдущей по каноническому порядку.
// ---------------------------------------------------------------------------

void EntryDetailsPanel::rebuildNow()
{
    QScrollBar* vbar = m_browser->verticalScrollBar();

    QString topSection;
    int offsetInSection = 0;
    if (m_hadEntry && m_line) { // переход запись→запись — сохраняем позицию
        const int oldY = vbar->value();
        const QVector<SectionAnchor> anchors = collectSectionAnchors();
        for (const SectionAnchor& a : anchors) {
            if (a.y > oldY)
                break;
            topSection = a.name;
            offsetInSection = oldY - a.y;
        }
    }

    m_browser->setHtml(buildHtml());
    m_dirty = false;
    const bool hasEntry = (m_line != nullptr);

    if (hasEntry && !topSection.isEmpty()) {
        const QVector<SectionAnchor> anchors = collectSectionAnchors();
        const auto findY = [&anchors](const QString& name) {
            for (const SectionAnchor& a : anchors)
                if (a.name == name)
                    return a.y;
            return -1;
        };
        static const QStringList kOrder = {
            QStringLiteral("sec-header"), QStringLiteral("sec-fields"),
            QStringLiteral("sec-message"), QStringLiteral("sec-json0"),
            QStringLiteral("sec-json1"), QStringLiteral("sec-json2"),
        };
        int y = findY(topSection);
        int delta = offsetInSection;
        if (y < 0) {
            delta = 0; // смещение внутри исчезнувшей секции не имеет смысла
            for (int i = int(kOrder.indexOf(topSection)) - 1; i >= 0 && y < 0; --i)
                y = findY(kOrder.at(i));
        }
        if (y >= 0) {
            int target = y + delta;
            // Секция в новой записи короче прежней — не заезжаем в следующую.
            for (const SectionAnchor& a : anchors) {
                if (a.y > y) {
                    target = qMin(target, a.y - 1);
                    break;
                }
            }
            vbar->setValue(qBound(0, target, vbar->maximum()));
        }
    }
    m_hadEntry = hasEntry;
}

QVector<EntryDetailsPanel::SectionAnchor> EntryDetailsPanel::collectSectionAnchors() const
{
    QVector<SectionAnchor> anchors;
    const QTextDocument* doc = m_browser->document();
    QAbstractTextDocumentLayout* docLayout = doc->documentLayout();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QStringList names = it.fragment().charFormat().anchorNames();
            if (names.isEmpty() || !names.first().startsWith(QLatin1String("sec-")))
                continue;
            // Якорь может быть размазан по нескольким фрагментам (вложенные
            // span'ы внутри <a name>) — берём только первое вхождение.
            if (!anchors.isEmpty() && anchors.last().name == names.first())
                continue;
            anchors.append({ names.first(),
                             int(docLayout->blockBoundingRect(block).top()) });
        }
    }
    return anchors;
}

// ---------------------------------------------------------------------------

const LogEntry* EntryDetailsPanel::fieldsSource() const
{
    // У continuation-строк fields пуст — берём их с первой строки логической
    // записи, у которой они извлеклись (обычно это первая строка).
    for (const auto& e : m_logicalLines)
        if (e && !e->fields().isEmpty())
            return e.get();
    return nullptr;
}

QString EntryDetailsPanel::joinedMessage() const
{
    QStringList parts;
    parts.reserve(m_logicalLines.size());
    for (const auto& e : m_logicalLines)
        if (e)
            parts << e->message();
    return parts.join(QLatin1Char('\n'));
}

void EntryDetailsPanel::onAnchorClicked(const QUrl& url)
{
    const QString link = url.toString();

    // Свернуть/развернуть поле секции Fields. В ссылке — индекс поля (имена
    // могут содержать что угодно), в наборе — имя (переживает смену схемы).
    if (link.startsWith(QLatin1String("fold:"))) {
        const int idx = link.mid(5).toInt();
        if (idx >= 0 && idx < m_fieldNames.size()) {
            const QString& name = m_fieldNames.at(idx);
            if (!m_collapsedFields.remove(name))
                m_collapsedFields.insert(name);
            saveSectionSettings();
            scheduleRebuild();
        }
        return;
    }

    if (!link.startsWith(QLatin1String("copy:")) || !m_line)
        return;
    const QString what = link.mid(5);

    QString text;
    if (what == QLatin1String("message")) {
        text = joinedMessage(); // полный, без обрезки для отображения
    } else if (what == QLatin1String("fields")) {
        if (const LogEntry* src = fieldsSource()) {
            QStringList rows;
            const int count = int(qMin(m_fieldNames.size(),
                                       qsizetype(src->fields().size())));
            for (int i = 0; i < count; ++i)
                if (src->fields().has(i))
                    rows << m_fieldNames.at(i) + QLatin1Char('\t')
                            + src->fields().get(i, src->message()).toString();
            text = rows.join(QLatin1Char('\n'));
        }
    } else if (what.startsWith(QLatin1String("json"))) {
        const int idx = what.mid(4).toInt();
        if (idx >= 0 && idx < m_jsonBlocks.size())
            text = QString::fromUtf8(m_jsonBlocks.at(idx).toJson(QJsonDocument::Indented));
    }
    if (text.isNull())
        return;

    QApplication::clipboard()->setText(text);
    QToolTip::showText(QCursor::pos(), tr("Copied"), m_browser);
}

// ---------------------------------------------------------------------------

QString EntryDetailsPanel::buildHtml()
{
    const QPalette pal = m_browser->palette();
    const QColor textColor = pal.color(QPalette::Text);
    const QColor muted = mixColors(textColor, pal.color(QPalette::Base), 0.45);
    const QString monoStyle =
        QStringLiteral("font-family:'%1'; white-space:pre-wrap;")
            .arg(AppSettings::instance().fontFamily());

    // Заголовок секции: якорь sec-<id> для восстановления прокрутки и
    // (опционально) ссылка ⧉ copy:<copyId> для копирования содержимого.
    const auto sectionHeader = [&muted](const QString& title, const QString& id,
                                        const QString& copyId = QString()) {
        QString h = QStringLiteral(
                        "<div style=\"color:%1;font-weight:bold;margin-top:12px;\">"
                        "<a name=\"sec-%2\">%3</a>")
                        .arg(muted.name(), id, title);
        if (!copyId.isEmpty())
            h += QStringLiteral("&nbsp;&nbsp;<a href=\"copy:%1\" "
                                "style=\"color:%2;text-decoration:none;\">⧉</a>")
                     .arg(copyId, muted.name());
        h += QLatin1String("</div>");
        return h;
    };
    const auto mutedDiv = [&muted](const QString& escapedText) {
        return QStringLiteral("<div style=\"color:%1;\">%2</div>")
            .arg(muted.name(), escapedText);
    };

    m_jsonBlocks.clear();

    if (!m_line)
        return mutedDiv(tr("Select a row in the log view to see its details."));

    const bool showHeader  = m_headerButton->isChecked();
    const bool showFields  = m_fieldsButton->isChecked();
    const bool showMessage = m_messageButton->isChecked();
    const bool showJson    = m_jsonButton->isChecked();

    QString html;

    // ---- Header: уровень, таймстамп, файл и позиция -------------------------
    if (showHeader) {
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
        html += QStringLiteral("<div><a name=\"sec-header\">%1</a></div>").arg(head);

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
        html += mutedDiv(location.join(QStringLiteral(" · ")).toHtmlEscaped());
    }

    // ---- Извлечённые поля ---------------------------------------------------
    // Для строк свободного текста секцию не показываем вовсе: полей там нет по
    // определению, а примечание «не совпало со схемой» на каждой строке
    // не-лог файла — только шум.
    if (showFields && !m_fieldNames.isEmpty() && !m_line->isPlainText()) {
        const LogEntry* src = fieldsSource();
        html += sectionHeader(tr("Fields"), QStringLiteral("fields"),
                              src ? QStringLiteral("fields") : QString());
        if (src) {
            html += QLatin1String("<table cellspacing=\"0\" cellpadding=\"2\">");
            const int count = int(qMin(m_fieldNames.size(),
                                       qsizetype(src->fields().size())));
            for (int i = 0; i < count; ++i) {
                // Имя поля со стрелкой ▾/▸ — ссылка сворачивания (как узел
                // tree view); у свёрнутого поля вместо значения «…».
                const bool collapsed = m_collapsedFields.contains(m_fieldNames.at(i));
                QString value;
                if (!src->fields().has(i)) {
                    value = colorSpan(muted, QStringLiteral("—"));
                } else if (collapsed) {
                    value = colorSpan(muted, QStringLiteral("…"));
                } else {
                    value = src->fields().get(i, src->message()).toString();
                    if (value.size() > kMaxFieldValueChars)
                        value = value.left(kMaxFieldValueChars) + QStringLiteral("…");
                    value = value.toHtmlEscaped();
                }
                html += QStringLiteral(
                            "<tr><td><a href=\"fold:%1\" style=\"color:%2;"
                            "text-decoration:none;\">%3&nbsp;%4</a>&nbsp;&nbsp;</td>"
                            "<td style=\"%5\">%6</td></tr>")
                            .arg(QString::number(i), muted.name(),
                                 collapsed ? QStringLiteral("▸") : QStringLiteral("▾"),
                                 m_fieldNames.at(i).toHtmlEscaped(),
                                 monoStyle, value);
            }
            html += QLatin1String("</table>");
        } else {
            html += mutedDiv(tr("The line does not match the extraction schema."));
        }
    }

    // Текст записи собирается даже при скрытой секции Message: по нему же
    // ищутся JSON-фрагменты.
    QString message;
    bool messageTruncated = false;
    if (showMessage || showJson) {
        message = joinedMessage();
        messageTruncated = message.size() > kMaxMessageChars;
        if (messageTruncated)
            message.truncate(kMaxMessageChars);
    }

    // ---- Полный текст логической записи -------------------------------------
    if (showMessage) {
        html += sectionHeader(m_logicalLines.size() > 1
                                  ? tr("Message (%1 lines)").arg(m_logicalLines.size())
                                  : tr("Message"),
                              QStringLiteral("message"), QStringLiteral("message"));
        html += QStringLiteral("<div style=\"%1\">%2</div>")
                    .arg(monoStyle, message.toHtmlEscaped());
        if (messageTruncated)
            html += mutedDiv(tr("… message truncated for display"));
    }

    // ---- Эвристика: JSON-фрагменты в тексте записи --------------------------
    if (showJson) {
        m_jsonBlocks = detectJsonBlocks(message);
        JsonHtmlWriter writer;
        for (int i = 0; i < m_jsonBlocks.size(); ++i) {
            html += sectionHeader(m_jsonBlocks.size() > 1 ? tr("JSON #%1").arg(i + 1)
                                                          : tr("JSON"),
                                  QStringLiteral("json%1").arg(i),
                                  QStringLiteral("json%1").arg(i));
            html += QStringLiteral("<div style=\"%1\">%2</div>")
                        .arg(monoStyle, writer.toHtml(m_jsonBlocks.at(i), muted));
        }
    }

    // ---- Заглушки: запись есть, а показывать нечего --------------------------
    if (html.isEmpty()) {
        if (!showHeader && !showFields && !showMessage && !showJson)
            return mutedDiv(tr("All sections are hidden — enable them on the bar above."));
        if (showJson && !showHeader && !showFields && !showMessage)
            return mutedDiv(tr("No JSON found in this entry."));
        return mutedDiv(tr("Nothing to show for this entry."));
    }
    return html;
}
