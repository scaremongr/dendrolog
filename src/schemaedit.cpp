#include "schemaedit.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <memory>

namespace SchemaEdit {

namespace {

using Type = LineTemplateElement::Type;

constexpr int kDiversityWords = 4;

inline bool isBlank(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t');
}

inline bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

inline qsizetype skipBlanks(QStringView s, qsizetype p)
{
    while (p < s.size() && isBlank(s[p]))
        ++p;
    return p;
}

inline qsizetype chunkLength(QStringView s, qsizetype p)
{
    qsizetype q = p;
    while (q < s.size() && !isBlank(s[q]))
        ++q;
    return q - p;
}

int wordCount(QStringView s)
{
    int n = 0;
    qsizetype p = skipBlanks(s, 0);
    while (p < s.size()) {
        ++n;
        p = skipBlanks(s, p + chunkLength(s, p));
    }
    return n;
}

QString escapeValue(const QString& v)
{
    QString out;
    for (QChar c : v) {
        if (c == QLatin1Char('|') || c == QLatin1Char(')') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

bool cancelled(const Options& o)
{
    return (o.cancel && o.cancel->load(std::memory_order_relaxed))
        || (o.budget.cancel && o.budget.cancel->load(std::memory_order_relaxed));
}

SchemaFit::Budget budgetOf(const Options& o)
{
    SchemaFit::Budget b = o.budget;
    if (!b.cancel)
        b.cancel = o.cancel;
    return b;
}

// Литералы и пробелы текста (разделитель, начало поля до выделения).
QVector<Piece> tokens(QStringView text, int opt)
{
    QVector<Piece> out;
    qsizetype i = 0;
    while (i < text.size()) {
        Piece p;
        p.opt = opt;
        if (isBlank(text[i])) {
            p.type = Type::Space;
            while (i < text.size() && isBlank(text[i]))
                ++i;
        } else {
            p.type = Type::Literal;
            while (i < text.size() && !isBlank(text[i]))
                p.text += text[i++];
        }
        out.append(p);
    }
    return out;
}

Piece fieldPiece(const QString& name, Kind kind, bool given, int opt)
{
    Piece p;
    p.type = Type::Field;
    p.name = name;
    p.kind = kind;
    p.kindGiven = given;
    p.opt = opt;
    return p;
}

int pieceOfColumn(const QVector<Piece>& pieces, int column)
{
    int c = 0;
    for (int i = 0; i < pieces.size(); ++i) {
        if (!pieces[i].isField() || pieces[i].name.isEmpty())
            continue;
        if (c++ == column)
            return i;
    }
    return -1;
}

int columnOfPiece(const QVector<Piece>& pieces, int index)
{
    int c = 0;
    for (int i = 0; i < index; ++i) {
        if (pieces[i].isField() && !pieces[i].name.isEmpty())
            ++c;
    }
    return pieces[index].name.isEmpty() ? -1 : c;
}

QString uniqueName(const QVector<Piece>& pieces, const QString& base, bool numberFirst)
{
    QSet<QString> taken;
    for (const Piece& p : pieces) {
        if (p.isField() && !p.name.isEmpty())
            taken.insert(p.name.toLower());
    }
    if (!numberFirst && !taken.contains(base.toLower()))
        return base;
    for (int n = numberFirst ? 1 : 2;; ++n) {
        const QString name = base + QString::number(n);
        if (!taken.contains(name.toLower()))
            return name;
    }
}

// Шаблон из элементов: компилируется? Вид поля без kindGiven — выводится; если
// вывод даёт другой вид, чем записан в элементе, вид пишется явно (поле
// сохраняет свой конец при перестановке соседей).
bool build(QVector<Piece>& pieces, QString* text, std::shared_ptr<const LineTemplate>* compiled)
{
    QString t = render(pieces);
    auto tpl = std::make_shared<const LineTemplate>(t);
    if (!tpl->isValid())
        return false;
    bool changed = false;
    int field = 0;
    const auto& els = tpl->elements();
    QVector<int> fieldElements;
    for (int i = 0; i < els.size(); ++i) {
        if (els[i].type == Type::Field)
            fieldElements.append(i);
    }
    for (Piece& p : pieces) {
        if (!p.isField())
            continue;
        const int e = fieldElements.value(field++, -1);
        if (e < 0)
            return false;
        if (!p.kindGiven && els[e].kind != p.kind) {
            p.kindGiven = true;
            changed = true;
        }
    }
    if (changed) {
        const QString forced = render(pieces);
        auto tpl2 = std::make_shared<const LineTemplate>(forced);
        if (tpl2->isValid()) {
            t = forced;
            tpl = tpl2;
        }
    }
    // Элементы — как вышло (вид выведенный или записанный).
    {
        const auto& final = tpl->elements();
        int k = 0;
        for (const auto& el : final) {
            if (el.type != Type::Field)
                continue;
            while (k < pieces.size() && !pieces[k].isField())
                ++k;
            if (k >= pieces.size())
                break;
            Piece& p = pieces[k++];
            p.kind = el.kind;
            p.kindGiven = el.kindGiven;
            p.count = el.count;
            p.values = el.values;
        }
    }
    if (text)
        *text = t;
    if (compiled)
        *compiled = tpl;
    return true;
}

SchemaFit::Result evaluate(const std::shared_ptr<const LineTemplate>& tpl, const SchemaFit::Corpus& corpus,
                           const Options& options)
{
    TemplateExtractor ex(tpl);
    return SchemaFit::evaluate(ex, corpus, budgetOf(options));
}

bool usable(const SchemaFit::Result& r)
{
    return r.valid && !r.timedOut && !r.cancelled;
}

// Позиции поля по строкам образцов.
struct Positions {
    QVector<QStringView> texts;
    QVector<qsizetype> pos;      ///< начало значения
    QVector<qsizetype> end;      ///< конец значения
    QVector<int> sample;
};

Positions positionsOf(const LineTemplate& tpl, int column, const QVector<SchemaFit::Sample>& samples,
                      const Options& options)
{
    Positions out;
    LogEntryFields f;
    for (int k = 0; k < samples.size(); ++k) {
        if ((k & 63) == 0 && cancelled(options))
            break;
        const QString& s = samples[k].text;
        if (tpl.match(s, &f) == FieldExtractor::Outcome::Rejected || !f.has(column))
            continue;
        const FieldSpan& span = f.spans[column];
        out.texts.append(QStringView(s));
        out.pos.append(span.start);
        out.end.append(span.start + span.size());
        out.sample.append(k);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Свидетельства по строкам (раздел 2 F-17.md, те же пороги, что у вывода
// структуры).
// ---------------------------------------------------------------------------

bool gapEvidence(const Positions& p)
{
    const int n = int(p.texts.size());
    if (n < 10)
        return false;
    int found = 0;
    for (int k = 0; k < n; ++k) {
        const QStringView s = p.texts[k];
        const qsizetype start = skipBlanks(s, p.pos[k]);
        int words = 1;
        for (qsizetype q = start + 1; q < s.size() && q < start + 240; ++q) {
            if (!isBlank(s[q]))
                continue;
            const bool wide = s[q] == QLatin1Char('\t') || (q + 1 < s.size() && isBlank(s[q + 1]));
            if (wide) {
                if (skipBlanks(s, q) < s.size())
                    ++found;
                break;
            }
            if (++words > kMaxAnchorWords)
                break;
        }
    }
    return found >= n * kAgree;
}

int widthEvidence(const Positions& p)
{
    const int n = int(p.texts.size());
    if (n < 10)
        return 0;
    QVector<int> hits(kMaxWidth + 1, 0);
    QVector<int> padded(kMaxWidth + 1, 0);
    for (int k = 0; k < n; ++k) {
        const QStringView s = p.texts[k];
        const qsizetype a = p.pos[k];
        for (int w = 2; w <= kMaxWidth; ++w) {
            const qsizetype q = a + w;
            if (q + 1 >= s.size())
                break;
            if (isBlank(s[q]) && !isBlank(s[q + 1]) && !isBlank(s[a])) {
                ++hits[w];
                if (isBlank(s[q - 1]))
                    ++padded[w];
            }
        }
    }
    for (int w = 2; w <= kMaxWidth; ++w) {
        if (hits[w] >= n * kAgree && padded[w] >= n * 0.15)
            return w;
    }
    return 0;
}

QString normalizedWord(QStringView w)
{
    bool digits = !w.isEmpty();
    for (QChar c : w) {
        if (!c.isDigit() && c != QLatin1Char('.') && c != QLatin1Char(',') && c != QLatin1Char('-')
            && c != QLatin1Char('+') && c != QLatin1Char('%')) {
            digits = false;
            break;
        }
    }
    return digits ? QStringLiteral("#") : w.toString();
}

struct Diversity {
    int constantWords = 0;                  ///< граница после стольких слов у ≥ 98 % строк
    QVector<QPair<QString, int>> values;    ///< закрытый набор (частые первыми)
};

// Разнообразие продолжений: для начала из 1–4 слов — сколько разных слов
// следом. Внутри названия продолжение почти всегда одно, на границе — много
// разных.
Diversity diversity(const Positions& p)
{
    Diversity out;
    const int n = int(p.texts.size());
    if (n < 10)
        return out;
    struct Words {
        QVector<QStringView> w;
    };
    QVector<Words> lw(n);
    for (int k = 0; k < n; ++k) {
        const QStringView s = p.texts[k];
        qsizetype q = skipBlanks(s, p.pos[k]);
        for (int w = 0; w <= kDiversityWords && q < s.size(); ++w) {
            const qsizetype len = chunkLength(s, q);
            lw[k].w.append(s.mid(q, len));
            q = skipBlanks(s, q + len);
        }
    }
    const auto prefix = [&](int k, int words) {
        QString key;
        for (int w = 0; w < words; ++w) {
            if (w > 0)
                key += QLatin1Char(' ');
            key += lw[k].w[w];
        }
        return key;
    };
    QHash<QString, int> prefixCount;
    QHash<QString, QHash<QString, int>> next;
    for (int k = 0; k < n; ++k) {
        for (int words = 1; words <= kDiversityWords && words <= lw[k].w.size(); ++words) {
            const QString pr = prefix(k, words);
            ++prefixCount[pr];
            ++next[pr][words < lw[k].w.size() ? normalizedWord(lw[k].w[words]) : QStringLiteral("\n")];
        }
    }
    QVector<int> boundary(n, 0);
    int resolved = 0;
    for (int k = 0; k < n; ++k) {
        for (int words = 1; words <= kDiversityWords && words <= lw[k].w.size(); ++words) {
            const QString pr = prefix(k, words);
            const int c = prefixCount.value(pr);
            if (c < kMinValueCount)
                break;
            const QHash<QString, int>& conts = next[pr];
            int top = 0;
            for (int v : conts)
                top = std::max(top, v);
            if (conts.size() >= 3 && top < 0.5 * c) {
                if (words < lw[k].w.size()) {
                    boundary[k] = words;
                    ++resolved;
                }
                break;
            }
            if (top < 0.8 * c)
                break;
        }
    }
    if (resolved < n * kAgree)
        return out;
    QHash<int, int> byWords;
    QHash<QString, int> values;
    for (int k = 0; k < n; ++k) {
        if (boundary[k] > 0) {
            ++byWords[boundary[k]];
            ++values[prefix(k, boundary[k])];
        }
    }
    int mode = 0, modeCount = 0;
    for (auto it = byWords.cbegin(); it != byWords.cend(); ++it) {
        if (it.value() > modeCount || (it.value() == modeCount && it.key() < mode)) {
            mode = it.key();
            modeCount = it.value();
        }
    }
    const int maxValues = std::max(kMaxValues, int(n * 0.02));
    if (modeCount >= n * kConstantWords && values.size() <= maxValues)
        out.constantWords = mode;
    QVector<QPair<QString, int>> list;
    int covered = 0;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.value() >= kMinValueCount) {
            list.append({ it.key(), it.value() });
            covered += it.value();
        }
    }
    if (list.size() >= 2 && list.size() <= maxValues && covered >= n * kAgree) {
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        out.values = list;
    }
    return out;
}

// Форма значения целиком: вид с формой, чей сканер берёт ровно его.
constexpr Kind kFormOrder[] = {
    Kind::Time, Kind::Level, Kind::Uuid, Kind::Ip, Kind::Int, Kind::Num, Kind::Hex, Kind::Quoted, Kind::Path,
};
constexpr Kind kStrongForms[] = { Kind::Level, Kind::Time, Kind::Ip, Kind::Uuid, Kind::Quoted };

qsizetype scan(Kind kind, QStringView s, qsizetype p)
{
    switch (kind) {
    case Kind::Time: return TokenScan::timeAt(s, p);
    case Kind::Level: return TokenScan::levelAt(s, p);
    case Kind::Int: return TokenScan::intAt(s, p);
    case Kind::Num: return TokenScan::numAt(s, p);
    case Kind::Hex: return TokenScan::hexAt(s, p);
    case Kind::Uuid: return TokenScan::uuidAt(s, p);
    case Kind::Ip: return TokenScan::ipAt(s, p);
    case Kind::Path: return TokenScan::pathAt(s, p);
    case Kind::Quoted: return TokenScan::quotedAt(s, p);
    default: return -1;
    }
}

// Строже, чем у разбора (как у вывода структуры): hex — с «0x» или от 8
// знаков с цифрой и буквой (иначе имя хоста «db01» — hex), path — с
// разделителем пути (иначе любое слово с дефисом — путь).
bool strictForm(Kind k, QStringView v)
{
    if (k == Kind::Path)
        return v.contains(QLatin1Char('/')) || v.contains(QLatin1Char('\\'));
    if (k == Kind::Hex) {
        if (v.startsWith(QLatin1String("0x")) || v.startsWith(QLatin1String("0X")))
            return true;
        bool digit = false, letter = false;
        for (QChar c : v) {
            digit = digit || c.isDigit();
            letter = letter || c.isLetter();
        }
        return v.size() >= 8 && digit && letter;
    }
    return true;
}

bool formOfValue(QStringView value, Kind* kind)
{
    if (value.isEmpty())
        return false;
    for (Kind k : kFormOrder) {
        if (scan(k, value, 0) == value.size() && strictForm(k, value)) {
            *kind = k;
            return true;
        }
    }
    return false;
}

QString formFieldName(Kind kind)
{
    switch (kind) {
    case Kind::Level: return QStringLiteral("Level");
    case Kind::Time: return QStringLiteral("Time");
    case Kind::Ip: return QStringLiteral("Ip");
    case Kind::Uuid: return QStringLiteral("Id");
    default: return QStringLiteral("Field");
    }
}

QString formText(Kind kind)
{
    switch (kind) {
    case Kind::Time: return QStringLiteral("timestamp");
    case Kind::Level: return QStringLiteral("level word");
    case Kind::Int: return QStringLiteral("whole number");
    case Kind::Num: return QStringLiteral("number");
    case Kind::Hex: return QStringLiteral("hex value");
    case Kind::Uuid: return QStringLiteral("UUID");
    case Kind::Ip: return QStringLiteral("IP address");
    case Kind::Path: return QStringLiteral("path");
    case Kind::Quoted: return QStringLiteral("quoted string");
    default: return LineTemplate::kindName(kind);
    }
}

QString valuesText(int n)
{
    return n == 1 ? QStringLiteral("1 value")
         : n > SchemaFit::kDistinctCap ? QStringLiteral("%1+ values").arg(SchemaFit::kDistinctCap)
                                        : QStringLiteral("%1 values").arg(n);
}

// Лучше по покрытию (шаг kCoverageStep), затем меньше разных значений.
int compareFit(const SchemaFit::Result& a, int da, const SchemaFit::Result& b, int db)
{
    if (usable(a) != usable(b))
        return usable(a) ? -1 : 1;
    const qint64 ca = qRound64(a.coverage() / SchemaFit::kCoverageStep);
    const qint64 cb = qRound64(b.coverage() / SchemaFit::kCoverageStep);
    if (ca != cb)
        return ca > cb ? -1 : 1;
    if (da != db)
        return da < db ? -1 : 1;
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Элементы и текст шаблона
// ---------------------------------------------------------------------------

QVector<Piece> piecesOf(const LineTemplate& tpl)
{
    QVector<Piece> out;
    const auto& els = tpl.elements();
    int open = -1, openEnd = -1;
    for (int i = 0; i < els.size(); ++i) {
        if (open >= 0 && i >= openEnd)
            open = -1;
        const auto& e = els[i];
        if (e.type == Type::Optional) {
            open = i;
            openEnd = e.end;
            continue;
        }
        Piece p;
        p.type = e.type;
        p.opt = open;
        p.text = e.text;
        p.name = e.name;
        p.kind = e.kind;
        p.kindGiven = e.kindGiven;
        p.values = e.values;
        p.count = e.count;
        p.regex = e.regexSource;
        out.append(p);
    }
    return out;
}

QString render(const QVector<Piece>& pieces)
{
    QString t;
    int open = -1;
    for (const Piece& p : pieces) {
        if (open >= 0 && p.opt != open) {
            t += QLatin1String("?}");
            open = -1;
        }
        if (p.opt >= 0 && open != p.opt) {
            t += QLatin1String("{?");
            open = p.opt;
        }
        switch (p.type) {
        case Type::Space:
            t += QLatin1Char(' ');
            break;
        case Type::Literal:
            t += LineTemplate::escapeLiteral(p.text);
            break;
        case Type::Field: {
            t += QLatin1Char('{');
            t += p.name.isEmpty() ? QStringLiteral("_") : p.name;
            if (p.kindGiven) {
                switch (p.kind) {
                case Kind::Width:
                case Kind::Words:
                    t += QStringLiteral(":%1(%2)").arg(LineTemplate::kindName(p.kind)).arg(p.count);
                    break;
                case Kind::Of: {
                    QStringList vs;
                    for (const QString& v : p.values)
                        vs.append(escapeValue(v));
                    t += QStringLiteral(":of(%1)").arg(vs.join(QLatin1Char('|')));
                    break;
                }
                case Kind::Regex: {
                    QString rx = p.regex;
                    rx.replace(QLatin1Char('/'), QStringLiteral("\\/"));
                    t += QStringLiteral(":/%1/").arg(rx);
                    break;
                }
                default:
                    t += QLatin1Char(':') + LineTemplate::kindName(p.kind);
                    break;
                }
            }
            t += QLatin1Char('}');
            break;
        }
        case Type::Optional:
            break;
        }
    }
    if (open >= 0)
        t += QLatin1String("?}");
    return t;
}

QString kindLabel(const Piece& p)
{
    switch (p.kind) {
    case Kind::Width:
    case Kind::Words:
        return QStringLiteral("%1(%2)").arg(LineTemplate::kindName(p.kind)).arg(p.count);
    case Kind::Of:
        return p.values.size() == 1 ? QStringLiteral("of(%1)").arg(p.values.first())
                                    : QStringLiteral("of(%1 values)").arg(p.values.size());
    case Kind::Regex:
        return QStringLiteral("/regex/");
    default:
        return LineTemplate::kindName(p.kind);
    }
}

QString evidenceText(Evidence evidence)
{
    switch (evidence) {
    case Evidence::Literal: return QStringLiteral("the text right after it");
    case Evidence::Form: return QStringLiteral("its form");
    case Evidence::FormFollows: return QStringLiteral("the form that follows it");
    case Evidence::Gap: return QStringLiteral("a wide gap");
    case Evidence::Width: return QStringLiteral("a fixed width");
    case Evidence::Words: return QStringLiteral("the same number of words");
    case Evidence::Values: return QStringLiteral("a closed set of values");
    case Evidence::LineEnd: return QStringLiteral("the end of the line");
    case Evidence::SingleValue: return QStringLiteral("the selected value only");
    }
    return QString();
}

QString Variant::summary() const
{
    return QStringLiteral("%1: %2 · %3%").arg(kind, valuesText(distinct()), SchemaFit::percent(fit.coverage()));
}

QString EndOption::summary() const
{
    if (!available)
        return reason;
    return QStringLiteral("%1: %2 · %3%").arg(kind, valuesText(distinct()), SchemaFit::percent(fit.coverage()));
}

bool better(const Variant& a, const Variant& b)
{
    if (a.supported != b.supported)
        return a.supported;
    const int c = compareFit(a.fit, a.distinct(), b.fit, b.distinct());
    if (c != 0)
        return c < 0;
    return int(a.evidence) < int(b.evidence);
}

// ---------------------------------------------------------------------------
// Make Field
// ---------------------------------------------------------------------------

MakeFieldResult makeField(const QString& templateText, const QString& line, qsizetype start,
                          qsizetype length, const QVector<SchemaFit::Sample>& samples, const Options& options)
{
    MakeFieldResult out;
    const LineTemplate tpl(templateText.trimmed());
    if (!tpl.isValid()) {
        out.error = QStringLiteral("The template has an error — fix it first.");
        return out;
    }
    qsizetype a = std::clamp<qsizetype>(start, 0, line.size());
    qsizetype b = std::clamp<qsizetype>(start + length, a, line.size());
    while (a < b && isBlank(line[a]))
        ++a;
    while (b > a && isBlank(line[b - 1]))
        --b;
    if (a >= b) {
        out.error = QStringLiteral("Select some text of a field value first.");
        return out;
    }
    LogEntryFields fields;
    if (tpl.match(line, &fields) == FieldExtractor::Outcome::Rejected) {
        out.error = QStringLiteral("The template does not parse this line — select text in a line it parses.");
        return out;
    }
    int column = -1;
    for (int c = 0; c < fields.size(); ++c) {
        const FieldSpan& sp = fields.spans[c];
        if (sp.isValid() && sp.start <= a && b <= sp.start + sp.size()) {
            column = c;
            break;
        }
    }
    if (column < 0) {
        out.error = QStringLiteral("Select text inside a field value — the selection is in the fixed text "
                                   "of the template.");
        return out;
    }
    const QVector<Piece> pieces = piecesOf(tpl);
    const int iF = pieceOfColumn(pieces, column);
    const Piece F = pieces[iF];
    const qsizetype s = fields.spans[column].start;
    const qsizetype e = s + fields.spans[column].size();
    const QString pre = line.mid(s, a - s);
    const QString post = line.mid(b, e - b);
    const QString sel = line.mid(a, b - a);
    if (pre.isEmpty() && post.isEmpty()) {
        out.error = QStringLiteral("The selection is the whole value of “%1” — choose where it ends in the "
                                   "column menu (Ends at) or rename it.").arg(F.name);
        return out;
    }
    const bool remainder = !post.isEmpty();
    bool fIsTail = true;
    for (int i = iF + 1; i < pieces.size(); ++i) {
        if (pieces[i].type != Type::Space)
            fIsTail = false;
    }

    const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
    std::shared_ptr<const LineTemplate> original = std::make_shared<const LineTemplate>(tpl.text());
    const SchemaFit::Result base = evaluate(original, corpus, options);
    if (cancelled(options)) {
        out.error = QStringLiteral("Cancelled.");
        return out;
    }

    // Начало поля до выделения: литералом (P1) или прежним полем (P2).
    struct Pre {
        QVector<Piece> pieces;
        bool literal = true;
    };
    QVector<Pre> pres;
    if (pre.isEmpty()) {
        pres.append(Pre{});
    } else {
        pres.append(Pre{tokens(pre, F.opt), true});
        qsizetype head = pre.size();
        while (head > 0 && !isWordChar(pre[head - 1]))
            --head;
        if (head > 0) {
            Pre p2;
            p2.literal = false;
            p2.pieces.append(fieldPiece(remainder ? uniqueName(pieces, QStringLiteral("Field"), true) : F.name,
                                        Kind::Text, false, F.opt));
            p2.pieces += tokens(QStringView(pre).mid(head), F.opt);
            pres.append(p2);
        }
    }

    // Разделитель после выделения и остаток поля.
    qsizetype sepEnd = 0;
    while (sepEnd < post.size() && !isWordChar(post[sepEnd]))
        ++sepEnd;
    if (sepEnd == post.size() && remainder)
        sepEnd = 0;   // остаток без слов — целиком прежнему полю
    const QVector<Piece> sep = tokens(QStringView(post).left(sepEnd), F.opt);
    Piece rest = F;   // прежнее поле — остаток
    const bool postBlank = remainder && isBlank(post[0]);

    // Позиции нового поля по строкам — пробный шаблон: всё до поля, начало
    // литералом, «остальное» — пробой.
    Positions probe;
    {
        QVector<Piece> pp = pieces.mid(0, iF);
        pp += pres.first().pieces;
        Piece pr = fieldPiece(uniqueName(pieces, QStringLiteral("Probe"), true), Kind::Rest, true, F.opt);
        pp.append(pr);
        std::shared_ptr<const LineTemplate> ptpl;
        if (pres.first().literal && build(pp, nullptr, &ptpl))
            probe = positionsOf(*ptpl, columnOfPiece(pp, int(pp.size()) - 1), samples, options);
    }
    const Diversity div = diversity(probe);

    struct Hyp {
        Evidence evidence;
        Piece field;
        QVector<Piece> after;   // вместо sep + остатка (форма следом)
        bool custom = false;
        bool precondition = true;
    };
    QVector<Hyp> hyps;
    // Знак сразу за выделением.
    if (remainder && !isBlank(post[0]) && !isWordChar(post[0]))
        hyps.append(Hyp{Evidence::Literal, fieldPiece(QString(), Kind::Text, false, F.opt), {}, false, true});
    // Форма самого выделения.
    {
        Kind k;
        if (formOfValue(sel, &k))
            hyps.append(Hyp{Evidence::Form, fieldPiece(QString(), k, true, F.opt), {}, false, true});
    }
    // Форма следом (через один пробел).
    if (postBlank && (post.size() < 2 || !isBlank(post[1]))) {
        const qsizetype q = skipBlanks(line, b);
        for (Kind k : kStrongForms) {
            const qsizetype len = scan(k, line, q);
            if (len <= 0 || q + len > e)
                continue;
            if (q + len < line.size() && isWordChar(line[q + len]) && k != Kind::Quoted)
                continue;
            Hyp h{Evidence::FormFollows, fieldPiece(QString(), Kind::Text, false, F.opt), {}, true, true};
            h.after.append(Piece{Type::Space});
            h.after.last().opt = F.opt;
            h.after.append(fieldPiece(QStringLiteral("\x01") + formFieldName(k), k, true, F.opt));
            const QString post2 = line.mid(q + len, e - (q + len));
            if (!post2.isEmpty()) {
                qsizetype se = 0;
                while (se < post2.size() && !isWordChar(post2[se]))
                    ++se;
                if (se == post2.size())
                    se = 0;
                h.after += tokens(QStringView(post2).left(se), F.opt);
                h.after.append(rest);
            }
            hyps.append(h);
            break;
        }
    }
    // Промежуток из пробелов.
    if (remainder && (post.startsWith(QLatin1String("  ")) || post.startsWith(QLatin1Char('\t'))))
        hyps.append(Hyp{Evidence::Gap, fieldPiece(QString(), Kind::Padded, true, F.opt), {}, false,
                        gapEvidence(probe)});
    // Постоянная позиция.
    if (postBlank) {
        const int w = widthEvidence(probe);
        if (w > 0) {
            Piece f = fieldPiece(QString(), Kind::Width, true, F.opt);
            f.count = w;
            hyps.append(Hyp{Evidence::Width, f, {}, false, true});
        }
    }
    // Число слов.
    if (postBlank || !remainder) {
        const int k = wordCount(sel);
        if (k >= 1 && k <= 16) {
            Piece f = fieldPiece(QString(), k == 1 ? Kind::Word : Kind::Words, true, F.opt);
            f.count = k;
            hyps.append(Hyp{Evidence::Words, f, {}, false, div.constantWords == k});
        }
    }
    // Закрытый набор значений и одно выделенное значение.
    {
        Piece f = fieldPiece(QString(), Kind::Of, true, F.opt);
        f.values.append(sel);
        if (!div.values.isEmpty()) {
            // Значение кончается раньше литерала, что стоит за выделением
            // («main]» — это «main» и «]»).
            QString stop;
            for (const Piece& t : sep) {
                if (t.type == Type::Literal) {
                    stop = t.text;
                    break;
                }
            }
            QStringList vs;
            for (const auto& v : div.values) {
                QString value = v.first;
                const qsizetype at = stop.isEmpty() || sel.contains(stop) ? -1 : value.indexOf(stop);
                if (at > 0)
                    value.truncate(at);
                if (!vs.contains(value))
                    vs.append(value);
            }
            if (!vs.contains(sel))
                vs.append(sel);
            Piece fv = f;
            fv.values = vs;
            hyps.append(Hyp{Evidence::Values, fv, {}, false, true});
        }
        hyps.append(Hyp{Evidence::SingleValue, f, {}, false, false});
    }
    // До конца строки / конец прежнего поля.
    if (!remainder) {
        if (fIsTail) {
            hyps.append(Hyp{Evidence::LineEnd, fieldPiece(QString(), Kind::Rest, false, F.opt), {}, false, true});
        } else {
            Piece f = F;
            f.name.clear();
            Evidence ev = Evidence::Literal;
            switch (F.kind) {
            case Kind::Padded: ev = Evidence::Gap; break;
            case Kind::Width: ev = Evidence::Width; break;
            case Kind::Word:
            case Kind::Words: ev = Evidence::Words; break;
            case Kind::Of: ev = Evidence::Values; break;
            case Kind::Text:
            case Kind::Greedy: {
                ev = Evidence::Literal;
                for (int i = iF + 1; i < pieces.size(); ++i) {
                    if (pieces[i].type == Type::Space)
                        continue;
                    if (pieces[i].isField() && LineTemplateElement::isFormKind(pieces[i].kind))
                        ev = Evidence::FormFollows;
                    break;
                }
                break;
            }
            default: ev = Evidence::Form; break;
            }
            bool dup = false;
            for (const Hyp& h : hyps)
                dup = dup || h.evidence == ev;
            if (!dup)
                hyps.append(Hyp{ev, f, {}, false, true});
        }
    }

    for (const Hyp& h : hyps) {
        if (cancelled(options))
            break;
        Variant best;
        bool have = false;
        for (const Pre& p : pres) {
            QVector<Piece> np = pieces.mid(0, iF);
            np += p.pieces;
            Piece f = h.field;
            const bool takesName = !remainder && p.literal;
            QVector<Piece> withPre = pieces + p.pieces;
            f.name = takesName && !F.name.isEmpty() ? F.name : uniqueName(withPre, QStringLiteral("Field"), true);
            np.append(f);
            const int newIndex = int(np.size()) - 1;
            if (h.custom) {
                for (Piece q : h.after) {
                    if (q.isField() && q.name.startsWith(QLatin1Char('\x01')))
                        q.name = uniqueName(np + pieces, q.name.mid(1), false);
                    np.append(q);
                }
            } else if (remainder) {
                np += sep;
                np.append(rest);
            }
            np += pieces.mid(iF + 1);
            QString text;
            std::shared_ptr<const LineTemplate> compiled;
            if (!build(np, &text, &compiled))
                continue;
            Variant v;
            v.evidence = h.evidence;
            v.templateText = text;
            v.field = f.name;
            v.column = columnOfPiece(np, newIndex);
            v.kind = kindLabel(np[newIndex]);
            v.fit = evaluate(compiled, corpus, options);
            v.supported = h.precondition && usable(v.fit) && usable(base)
                && v.fit.coverage() >= base.coverage() - kCoverageLoss;
            if (h.evidence == Evidence::SingleValue)
                v.supported = false;
            if (!have || better(v, best)) {
                best = v;
                have = true;
            }
        }
        if (!have)
            continue;
        bool dup = false;
        for (const Variant& v : out.variants)
            dup = dup || v.templateText == best.templateText;
        if (!dup)
            out.variants.append(best);
    }
    if (cancelled(options)) {
        out.variants.clear();
        out.error = QStringLiteral("Cancelled.");
        return out;
    }
    std::stable_sort(out.variants.begin(), out.variants.end(), better);
    if (out.variants.isEmpty()) {
        out.error = QStringLiteral("“%1” cannot become a field here: every way to end it breaks the template.")
                        .arg(sel);
        return out;
    }
    if (!out.variants.first().supported) {
        // Нет свидетельства: список из одного выделенного значения.
        out.noEvidence = true;
        for (int i = 0; i < out.variants.size(); ++i) {
            if (out.variants[i].evidence == Evidence::SingleValue) {
                out.variants.move(i, 0);
                break;
            }
        }
        out.message = QStringLiteral(
            "Nothing in the lines tells where “%1” ends: no separator right after it, no form after it, "
            "no wide gap or fixed column, and neither the number of words nor the values repeat. "
            "%2 is the listed value “%1” — lines with other values stop there; add them with Add Value…, "
            "or choose where it ends in the column menu.")
                          .arg(sel, out.variants.first().field);
    } else {
        const Variant& v = out.variants.first();
        out.message = QStringLiteral("%1 ends at %2 (%3).").arg(v.field, evidenceText(v.evidence), v.summary());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Ends at
// ---------------------------------------------------------------------------

QVector<EndOption> endOptions(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
                              const Options& options)
{
    QVector<EndOption> out;
    const LineTemplate tpl(templateText.trimmed());
    if (!tpl.isValid() || column < 0 || column >= tpl.columns().size())
        return out;
    const QVector<Piece> pieces = piecesOf(tpl);
    const int iF = pieceOfColumn(pieces, column);
    const Piece& F = pieces[iF];
    const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
    const Positions pos = positionsOf(tpl, column, samples, options);

    QStringList values;
    QHash<QString, int> counts;
    QHash<int, int> wordCounts;
    int maxLen = 0;
    for (int k = 0; k < pos.texts.size(); ++k) {
        const QStringView v = pos.texts[k].mid(pos.pos[k], pos.end[k] - pos.pos[k]);
        if (v.isEmpty())
            continue;
        values.append(v.toString());
        ++counts[values.last()];
        ++wordCounts[wordCount(v)];
        maxLen = std::max(maxLen, int(v.size()));
    }

    int nextNonSpace = -1;
    for (int i = iF + 1; i < pieces.size(); ++i) {
        if (pieces[i].type != Type::Space) {
            nextNonSpace = i;
            break;
        }
    }
    bool tail = true;
    for (int i = iF + 1; i < pieces.size(); ++i)
        tail = tail && pieces[i].type == Type::Space;

    using C = EndOption::Choice;
    const auto add = [&](C choice, const QString& label, Piece f, const QString& unavailable = QString()) {
        EndOption o;
        o.choice = choice;
        o.label = label;
        o.column = column;
        o.kind = kindLabel(f);
        if (!unavailable.isEmpty()) {
            o.reason = unavailable;
            out.append(o);
            return;
        }
        QVector<Piece> np = pieces;
        np[iF] = f;
        QString text;
        std::shared_ptr<const LineTemplate> compiled;
        if (!build(np, &text, &compiled)) {
            const LineTemplate failed(render(np));
            o.reason = failed.error().message;
            out.append(o);
            return;
        }
        o.templateText = text;
        o.available = true;
        o.fit = evaluate(compiled, corpus, options);
        o.current = F.kind == f.kind && F.count == f.count && (f.kind != Kind::Of || F.values == f.values);
        out.append(o);
    };
    const auto withKind = [&](Kind k, bool given = true) {
        Piece f = F;
        f.kind = k;
        f.kindGiven = given;
        f.count = 0;
        f.values.clear();
        f.regex.clear();
        return f;
    };

    add(C::NextSpace, QStringLiteral("at the next space"), withKind(Kind::Word));
    if (nextNonSpace >= 0 && pieces[nextNonSpace].type == Type::Literal && pieces[nextNonSpace].opt == F.opt)
        add(C::Literal, QStringLiteral("at “%1”").arg(pieces[nextNonSpace].text), withKind(Kind::Text, false));
    else
        add(C::Literal, QStringLiteral("at the next separator"), withKind(Kind::Text, false),
            QStringLiteral("no separator follows the field in the template"));
    if (nextNonSpace >= 0 && pieces[nextNonSpace].isField()
        && LineTemplateElement::isFormKind(pieces[nextNonSpace].kind))
        add(C::NextForm, QStringLiteral("at the next %1").arg(pieces[nextNonSpace].name.isEmpty()
                                                                  ? formText(pieces[nextNonSpace].kind)
                                                                  : pieces[nextNonSpace].name),
            withKind(Kind::Text, false));
    else
        add(C::NextForm, QStringLiteral("at the next field with a form"), withKind(Kind::Text, false),
            QStringLiteral("no field with a form follows"));
    add(C::WideGap, QStringLiteral("at a wide gap"), withKind(Kind::Padded));
    {
        int w = widthEvidence(pos);
        if (w <= 0)
            w = std::max(1, maxLen);
        Piece f = withKind(Kind::Width);
        f.count = w;
        add(C::Width, QStringLiteral("after %1 characters").arg(w), f);
    }
    {
        int mode = 2, modeCount = 0;
        for (auto it = wordCounts.cbegin(); it != wordCounts.cend(); ++it) {
            if (it.key() >= 2 && (it.value() > modeCount || (it.value() == modeCount && it.key() < mode))) {
                mode = it.key();
                modeCount = it.value();
            }
        }
        Piece f = withKind(Kind::Words);
        f.count = mode;
        add(C::Words, QStringLiteral("after %1 words").arg(mode), f);
    }
    {
        const Diversity div = diversity(pos);
        QStringList vs;
        for (const auto& v : div.values)
            vs.append(v.first);
        if (vs.isEmpty() && !counts.isEmpty() && counts.size() <= kMaxValues) {
            QVector<QPair<QString, int>> list;
            for (auto it = counts.cbegin(); it != counts.cend(); ++it)
                list.append({ it.key(), it.value() });
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            for (const auto& v : list)
                vs.append(v.first);
        }
        Piece f = withKind(Kind::Of);
        f.values = vs;
        if (F.kind == Kind::Of)
            f.values = F.values;
        add(C::Values, QStringLiteral("one of the listed values"), f,
            f.values.isEmpty() ? QStringLiteral("the values do not repeat") : QString());
    }
    {
        Kind form = Kind::Text;
        bool found = false;
        for (Kind k : kFormOrder) {
            int hits = 0;
            for (const QString& v : values)
                hits += scan(k, v, 0) == v.size() && strictForm(k, v) ? 1 : 0;
            if (!values.isEmpty() && hits >= values.size() * kAgree) {
                form = k;
                found = true;
                break;
            }
        }
        if (found)
            add(C::Form, QStringLiteral("where the %1 ends").arg(formText(form)), withKind(form));
        else
            add(C::Form, QStringLiteral("where its form ends"), withKind(Kind::Text, false),
                QStringLiteral("the values have no common form"));
    }
    if (tail)
        add(C::LineEnd, QStringLiteral("at the end of the line"), withKind(Kind::Rest, false));
    else
        add(C::LineEnd, QStringLiteral("at the end of the line"), withKind(Kind::Rest, false),
            QStringLiteral("other fields follow it"));
    // Вид поля сейчас — один пункт «current» (text — у первого подходящего).
    bool marked = false;
    for (EndOption& o : out) {
        if (o.current && marked)
            o.current = false;
        marked = marked || o.current;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Простые операции
// ---------------------------------------------------------------------------

namespace {

Edit finish(QVector<Piece> pieces, const QString& what)
{
    Edit out;
    QString text;
    if (!build(pieces, &text, nullptr)) {
        const LineTemplate failed(render(pieces));
        out.error = QStringLiteral("%1 would break the template: %2").arg(what, failed.error().message);
        return out;
    }
    out.templateText = text;
    return out;
}

bool loadColumn(const QString& templateText, int column, LineTemplate* tpl, QVector<Piece>* pieces, int* index,
                Edit* error)
{
    *tpl = LineTemplate(templateText.trimmed());
    if (!tpl->isValid()) {
        error->error = QStringLiteral("The template has an error — fix it first.");
        return false;
    }
    if (column < 0 || column >= tpl->columns().size()) {
        error->error = QStringLiteral("There is no such column.");
        return false;
    }
    *pieces = piecesOf(*tpl);
    *index = pieceOfColumn(*pieces, column);
    return true;
}

// Элементы [from, to] — одно поле name; вид — лучший по образцам из
// подходящих (конец — конец второго поля).
Edit mergeRange(const LineTemplate& tpl, const QVector<Piece>& pieces, int from, int to, const QString& name,
                const QVector<SchemaFit::Sample>& samples, const Options& options, const QString& what)
{
    Edit out;
    QSet<int> groups;
    for (int i = from; i <= to; ++i) {
        if (pieces[i].opt >= 0)
            groups.insert(pieces[i].opt);
    }
    const bool sameGroup = pieces[from].opt >= 0 && pieces[from].opt == pieces[to].opt;
    for (int g : groups) {
        if (sameGroup && g == pieces[from].opt)
            continue;
        for (int i = 0; i < pieces.size(); ++i) {
            if (pieces[i].opt == g && (i < from || i > to)) {
                out.error = QStringLiteral("%1 would cut an optional part {? … ?} in two.").arg(what);
                return out;
            }
        }
    }
    const Piece& G = pieces[to];
    bool tail = true;
    for (int i = to + 1; i < pieces.size(); ++i)
        tail = tail && pieces[i].type == Type::Space;

    // Слов в объединённом значении (по строкам, где оба поля есть).
    int mode = 0;
    {
        const int ca = columnOfPiece(pieces, from);
        const int cb = columnOfPiece(pieces, to);
        if (ca >= 0 && cb >= 0) {
            QHash<int, int> counts;
            LogEntryFields f;
            for (const auto& s : samples) {
                if (tpl.match(s.text, &f) == FieldExtractor::Outcome::Rejected || !f.has(ca) || !f.has(cb))
                    continue;
                const qsizetype a = f.spans[ca].start;
                const qsizetype b = f.spans[cb].start + f.spans[cb].size();
                if (b > a)
                    ++counts[wordCount(QStringView(s.text).mid(a, b - a))];
            }
            int best = 0;
            for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
                if (it.value() > best) {
                    best = it.value();
                    mode = it.key();
                }
            }
        }
    }
    QVector<Piece> kinds;
    const auto kindPiece = [&](Kind k, bool given, int count = 0) {
        Piece p = fieldPiece(name, k, given, sameGroup ? pieces[from].opt : -1);
        p.count = count;
        return p;
    };
    if (!LineTemplateElement::isFormKind(G.kind) && G.kind != Kind::Word && G.kind != Kind::Words
        && G.kind != Kind::Width) {
        Piece p = G;
        p.name = name;
        p.opt = sameGroup ? pieces[from].opt : -1;
        kinds.append(p);
    }
    kinds.append(kindPiece(Kind::Text, false));
    kinds.append(kindPiece(Kind::Greedy, true));
    if (mode >= 2)
        kinds.append(kindPiece(Kind::Words, true, mode));
    if (tail)
        kinds.append(kindPiece(Kind::Rest, false));

    const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
    SchemaFit::Result bestFit;
    QString bestText;
    QString firstError;
    for (const Piece& k : kinds) {
        QVector<Piece> np = pieces.mid(0, from);
        np.append(k);
        np += pieces.mid(to + 1);
        QString text;
        std::shared_ptr<const LineTemplate> compiled;
        if (!build(np, &text, &compiled)) {
            if (firstError.isEmpty())
                firstError = LineTemplate(render(np)).error().message;
            continue;
        }
        if (samples.isEmpty()) {
            bestText = text;
            break;
        }
        const SchemaFit::Result fit = evaluate(compiled, corpus, options);
        if (bestText.isEmpty() || compareFit(fit, 0, bestFit, 0) < 0) {
            bestText = text;
            bestFit = fit;
        }
    }
    if (bestText.isEmpty()) {
        out.error = QStringLiteral("%1 would break the template: %2").arg(what, firstError);
        return out;
    }
    out.templateText = bestText;
    return out;
}

int nextField(const QVector<Piece>& pieces, int from, int step)
{
    for (int i = from + step; i >= 0 && i < pieces.size(); i += step) {
        if (pieces[i].isField())
            return i;
    }
    return -1;
}

} // namespace

Edit rename(const QString& templateText, int column, const QString& name)
{
    Edit out;
    LineTemplate tpl;
    QVector<Piece> pieces;
    int i = -1;
    if (!loadColumn(templateText, column, &tpl, &pieces, &i, &out))
        return out;
    const QString n = name.trimmed();
    if (n.isEmpty() || n == QLatin1String("_")) {
        out.error = QStringLiteral("The name is empty — use Hide to keep the field out of the columns.");
        return out;
    }
    if (n.contains(QLatin1Char('{')) || n.contains(QLatin1Char('}')) || n.contains(QLatin1Char(':'))) {
        out.error = QStringLiteral("A field name cannot contain “{”, “}” or “:”.");
        return out;
    }
    for (int c = 0; c < tpl.columns().size(); ++c) {
        if (c != column && tpl.columns()[c].compare(n, Qt::CaseInsensitive) == 0) {
            out.error = QStringLiteral("There is already a column named “%1”.").arg(tpl.columns()[c]);
            return out;
        }
    }
    pieces[i].name = n;
    return finish(pieces, QStringLiteral("Renaming"));
}

Edit hide(const QString& templateText, int column)
{
    Edit out;
    LineTemplate tpl;
    QVector<Piece> pieces;
    int i = -1;
    if (!loadColumn(templateText, column, &tpl, &pieces, &i, &out))
        return out;
    pieces[i].name.clear();
    return finish(pieces, QStringLiteral("Hiding the field"));
}

Edit remove(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
            const Options& options)
{
    Edit out;
    LineTemplate tpl;
    QVector<Piece> pieces;
    int i = -1;
    if (!loadColumn(templateText, column, &tpl, &pieces, &i, &out))
        return out;
    const int prev = nextField(pieces, i, -1);
    // Сосед скрыт ({_}) — объединённое поле тоже скрыто: колонка уходит.
    if (prev >= 0) {
        const QString name = pieces[prev].name;
        return mergeRange(tpl, pieces, prev, i, name, samples, options, QStringLiteral("Removing the field"));
    }
    const int next = nextField(pieces, i, 1);
    if (next >= 0) {
        const QString name = pieces[next].name;
        return mergeRange(tpl, pieces, i, next, name, samples, options, QStringLiteral("Removing the field"));
    }
    out.error = QStringLiteral("The only field of the template cannot be removed.");
    return out;
}

Edit mergeWithNext(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
                   const Options& options)
{
    Edit out;
    LineTemplate tpl;
    QVector<Piece> pieces;
    int i = -1;
    if (!loadColumn(templateText, column, &tpl, &pieces, &i, &out))
        return out;
    const int next = nextField(pieces, i, 1);
    if (next < 0) {
        out.error = QStringLiteral("“%1” is the last field — there is nothing to merge it with.")
                        .arg(pieces[i].name);
        return out;
    }
    return mergeRange(tpl, pieces, i, next, pieces[i].name, samples, options, QStringLiteral("Merging"));
}

Edit addValue(const QString& templateText, int column, const QString& value)
{
    Edit out;
    LineTemplate tpl;
    QVector<Piece> pieces;
    int i = -1;
    if (!loadColumn(templateText, column, &tpl, &pieces, &i, &out))
        return out;
    if (pieces[i].kind != Kind::Of) {
        out.error = QStringLiteral("“%1” is not a list of values.").arg(pieces[i].name);
        return out;
    }
    const QString v = value.trimmed();
    if (v.isEmpty()) {
        out.error = QStringLiteral("The value is empty.");
        return out;
    }
    if (pieces[i].values.contains(v)) {
        out.error = QStringLiteral("“%1” is already in the list.").arg(v);
        return out;
    }
    pieces[i].values.append(v);
    return finish(pieces, QStringLiteral("Adding the value"));
}

QStringList suggestValues(const QString& templateText, int column, const QVector<SchemaFit::Sample>& samples,
                          const Options& options)
{
    QStringList out;
    const LineTemplate tpl(templateText.trimmed());
    if (!tpl.isValid() || column < 0 || column >= tpl.columns().size())
        return out;
    const QString name = tpl.columns()[column];
    const QVector<Piece> pieces = piecesOf(tpl);
    const int iF = pieceOfColumn(pieces, column);
    const QStringList existing = pieces[iF].kind == Kind::Of ? pieces[iF].values : QStringList();
    // Поле в конце строки: значение может кончаться последним словом строки.
    bool tail = true;
    for (int i = iF + 1; i < pieces.size(); ++i)
        tail = tail && pieces[i].type == Type::Space;
    // Строки, где разбор остановился на этом поле: слова с места остановки.
    QVector<QVector<QStringView>> words;
    LogEntryFields f;
    for (int k = 0; k < samples.size(); ++k) {
        if ((k & 63) == 0 && cancelled(options))
            return out;
        const QString& s = samples[k].text;
        MatchReport report;
        const auto outcome = tpl.match(s, &f, -1, nullptr, &report);
        if (outcome == FieldExtractor::Outcome::Full || report.field != name)
            continue;
        QVector<QStringView> w;
        qsizetype q = skipBlanks(s, report.stopOffset);
        while (q < s.size() && w.size() < kDiversityWords) {
            const qsizetype len = chunkLength(s, q);
            w.append(QStringView(s).mid(q, len));
            q = skipBlanks(s, q + len);
        }
        if (!w.isEmpty())
            words.append(w);
    }
    const auto prefix = [&](const QVector<QStringView>& w, int n) {
        QString key;
        for (int i = 0; i < n; ++i) {
            if (i > 0)
                key += QLatin1Char(' ');
            key += w[i];
        }
        return key;
    };
    QHash<QString, int> prefixCount;
    for (const auto& w : words) {
        for (int n = 1; n <= w.size(); ++n)
            ++prefixCount[prefix(w, n)];
    }
    // У строки — самое длинное начало, общее со всеми строками с тем же
    // первым словом (название из нескольких слов); иначе первое слово.
    QHash<QString, int> chosen;
    for (const auto& w : words) {
        const int first = prefixCount.value(prefix(w, 1));
        int n = 1;
        const int limit = tail ? int(w.size()) : int(w.size()) - 1;
        for (int m = 2; m <= limit; ++m) {
            if (first >= 2 && prefixCount.value(prefix(w, m)) == first)
                n = m;
            else
                break;
        }
        ++chosen[prefix(w, n)];
    }
    QVector<QPair<QString, int>> list;
    for (auto it = chosen.cbegin(); it != chosen.cend(); ++it) {
        if (!existing.contains(it.key()))
            list.append({ it.key(), it.value() });
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (const auto& v : list)
        out.append(v.first);
    return out;
}

} // namespace SchemaEdit
