#include "schemainference.h"

#include "linetemplate.h"
#include "logentry.h"
#include "patternheuristics.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <limits>
#include <memory>

// Устройство — в начале schemainference.h. Здесь: строки читаются «в лад»
// (у каждой своя позиция), на каждом шаге выбирается один элемент шаблона,
// строки, не согласные с ним, выбывают. Все проходы по строке ограничены
// окном (kWindowChars) — работа линейна по числу строк.

namespace SchemaInference {

namespace {

using Kind = LineTemplateElement::Kind;

constexpr qsizetype kWindowChars = 240;   // окно поиска якоря от позиции
constexpr qsizetype kLineChars = 1024;    // голова строки, которую смотрит вывод
constexpr int kDiversityWords = 4;        // разнообразие продолжений — до стольких слов
constexpr int kMaxElements = 96;
constexpr int kMaxOptionalUnits = 5;

inline bool isBlank(QChar c)
{
    return c == QLatin1Char(' ') || c == QLatin1Char('\t');
}

inline bool isAsciiAlnum(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9');
}

// Знак слова для чтения «в лад»: буква, цифра (любой письменности), «_».
inline bool isWordish(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// Знаки, на которых кончается слово и которые могут быть якорем вплотную.
inline bool isStructural(QChar c)
{
    switch (c.unicode()) {
    case u'[': case u']': case u'(': case u')': case u'{': case u'}': case u'<': case u'>':
    case u'|': case u':': case u';': case u',': case u'"':
        return true;
    default:
        return false;
    }
}

inline bool isOpening(QChar c)
{
    return c == QLatin1Char('[') || c == QLatin1Char('(') || c == QLatin1Char('<')
        || c == QLatin1Char('{');
}

inline QChar closingOf(QChar c)
{
    switch (c.unicode()) {
    case u'[': return QLatin1Char(']');
    case u'(': return QLatin1Char(')');
    case u'<': return QLatin1Char('>');
    case u'{': return QLatin1Char('}');
    default: return QChar();
    }
}

// Конец строки: дальше только пробелы.
inline bool atEnd(QStringView s, qsizetype p)
{
    while (p < s.size() && isBlank(s[p]))
        ++p;
    return p >= s.size();
}

inline qsizetype skipBlanks(QStringView s, qsizetype p)
{
    while (p < s.size() && isBlank(s[p]))
        ++p;
    return p;
}

// Длина «слова» (куска до пробела) с позиции.
inline qsizetype chunkLength(QStringView s, qsizetype p)
{
    qsizetype q = p;
    while (q < s.size() && !isBlank(s[q]))
        ++q;
    return q - p;
}

// Порядок видов с формой при согласии строк.
// path — раньше чисел: «0/0/1/12/13» — один токен, а не 0 и «/0/1/12/13».
constexpr Kind kFormOrder[] = {
    Kind::Time, Kind::Level, Kind::Uuid, Kind::Ip, Kind::Path, Kind::Int, Kind::Num, Kind::Hex,
    Kind::Quoted,
};

// Длина токена вида kind с позиции p, -1 — нет. Для вывода у hex и path
// строже, чем у разбора: hex — с «0x» или с цифрой и буквой a-f (иначе это
// число или слово), path — с разделителем пути (иначе это любое слово).
qsizetype formLength(Kind kind, QStringView s, qsizetype p)
{
    if (p >= s.size())
        return -1;
    switch (kind) {
    case Kind::Time: return TokenScan::timeAt(s, p);
    case Kind::Level: return TokenScan::levelAt(s, p);
    case Kind::Uuid: return TokenScan::uuidAt(s, p);
    case Kind::Ip: return TokenScan::ipAt(s, p);
    case Kind::Int: return s[p].isDigit() ? TokenScan::intAt(s, p) : -1;
    case Kind::Num: return s[p].isDigit() ? TokenScan::numAt(s, p) : -1;
    case Kind::Hex: {
        const qsizetype n = TokenScan::hexAt(s, p);
        if (n <= 0)
            return -1;
        if (n > 2 && s[p] == QLatin1Char('0') && (s[p + 1] == QLatin1Char('x') || s[p + 1] == QLatin1Char('X')))
            return n;
        bool digit = false, letter = false;
        for (qsizetype i = p; i < p + n; ++i) {
            if (s[i].isDigit())
                digit = true;
            else
                letter = true;
        }
        return (digit && letter && n >= 8) ? n : -1;
    }
    case Kind::Quoted: return TokenScan::quotedAt(s, p);
    case Kind::Path: {
        const qsizetype n = TokenScan::pathAt(s, p);
        if (n <= 0)
            return -1;
        bool sep = false;
        for (qsizetype i = p; i < p + n; ++i) {
            if (isStructural(s[i]))
                return -1;
            sep = sep || s[i] == QLatin1Char('/') || s[i] == QLatin1Char('\\');
        }
        return sep ? n : -1;
    }
    default:
        return -1;
    }
}

// Целое, за которым у части строк дробь («12345.678901»), — число: вид
// num, а не int с обрывом на точке.
bool fractionFollows(QStringView s, qsizetype p)
{
    const qsizetype i = formLength(Kind::Int, s, p);
    return i > 0 && formLength(Kind::Num, s, p) > i;
}

// Что стоит на позиции — для сверки «элемент следом сходится у всех».
struct Sig {
    enum class T { End, Space, Form, Punct, Word };
    T t = T::End;
    Kind form = Kind::Text;
    QChar c;
    bool operator==(const Sig& o) const
    {
        if (t != o.t)
            return false;
        if (t == T::Form) {
            const auto family = [](Kind k) {
                return (k == Kind::Int || k == Kind::Num || k == Kind::Hex) ? Kind::Num : k;
            };
            return family(form) == family(o.form);
        }
        if (t == T::Punct)
            return c == o.c;
        return true;
    }
};

struct Elem {
    enum class Type { Literal, Space, Field };
    Type type = Type::Literal;
    QString text;            // Literal
    Kind kind = Kind::Text;  // Field
    int count = 0;           // Width, Words
    QStringList values;      // Of
    int opt = -1;            // номер необязательного участка, -1 — вне участка
    bool hard = false;       // найдено по свидетельству «трудной границы»
    bool bracketed = false;  // text между скобками
    QString name;
    QVector<QStringView> samples;   // значения у строк (имя колонки)
};

struct Line {
    QStringView s;
    qsizetype pos = 0;
    bool active = true;
};

// Единица необязательного участка и обязательного элемента.
struct Unit {
    enum class T { Space, Form, Literal, Group };
    T t = T::Space;
    Kind form = Kind::Text;
    QChar c;
};

class Builder {
public:
    Builder(QVector<Line>& lines, qint64& steps, const std::atomic_bool* cancel)
        : L(lines), m_steps(steps), m_cancel(cancel)
    {
        m_initial = int(L.size());
    }

    QVector<Elem> run();
    int messageAfter = -1;     // элемент перед Message (заметка NoBoundary), -1 — нет

private:
    QVector<int> active() const
    {
        QVector<int> a;
        a.reserve(L.size());
        for (int i = 0; i < L.size(); ++i) {
            if (L[i].active)
                a.append(i);
        }
        return a;
    }
    bool cancelled() const { return m_cancel && m_cancel->load(std::memory_order_relaxed); }

    Sig sigAt(QStringView s, qsizetype p);
    bool unitAt(const Unit& u, QStringView s, qsizetype p, qsizetype* end);
    void groupInnerKind(const QVector<int>& lines, Kind* kind);
    bool consensusUnit(const QVector<int>& lines, const QVector<qsizetype>& pos, Unit* out);
    bool agree(const QVector<int>& a, const QVector<qsizetype>& posA, const QVector<int>& b,
               Sig* agreed = nullptr);

    bool tryOptional(const QVector<int>& act, const QVector<int>& x, const Unit& first);
    bool tryLeadingGroups(const QVector<int>& act);
    bool tryTailOptional(const QVector<int>& act);
    bool tryAnchor(const QVector<int>& act);
    bool tryHardBoundary(const QVector<int>& act);

    void emitSpace(int opt = -1);
    void emitLiteral(const QString& text, int opt = -1);
    Elem& emitField(Kind kind, int opt = -1);
    void emitMessage();
    void emitUnit(const Unit& u, const QVector<int>& lines, QVector<qsizetype>* pos, int opt);

    QVector<Line>& L;
    QVector<Elem> m_out;
    qint64& m_steps;
    const std::atomic_bool* m_cancel;
    int m_initial = 0;
    int m_nextOpt = 0;
};

Sig Builder::sigAt(QStringView s, qsizetype p)
{
    ++m_steps;
    Sig g;
    if (atEnd(s, p)) {
        g.t = Sig::T::End;
        return g;
    }
    if (isBlank(s[p])) {
        g.t = Sig::T::Space;
        return g;
    }
    for (Kind k : kFormOrder) {
        const qsizetype len = formLength(k, s, p);
        m_steps += std::max<qsizetype>(len, 1);
        if (len > 0) {
            g.t = Sig::T::Form;
            g.form = k;
            return g;
        }
    }
    if (!isWordish(s[p])) {
        g.t = Sig::T::Punct;
        g.c = s[p];
        return g;
    }
    g.t = Sig::T::Word;
    return g;
}

// Группа в скобках с позиции p: «[» … первая «]». Конец — за закрывающей.
qsizetype groupEnd(QStringView s, qsizetype p)
{
    if (p >= s.size() || !isOpening(s[p]))
        return -1;
    const QChar close = closingOf(s[p]);
    const qsizetype limit = std::min<qsizetype>(s.size(), p + kWindowChars);
    for (qsizetype q = p + 1; q < limit; ++q) {
        if (s[q] == close)
            return q + 1;
    }
    return -1;
}

bool Builder::unitAt(const Unit& u, QStringView s, qsizetype p, qsizetype* end)
{
    ++m_steps;
    switch (u.t) {
    case Unit::T::Space:
        if (p < s.size() && isBlank(s[p])) {
            *end = skipBlanks(s, p);
            return true;
        }
        return false;
    case Unit::T::Form: {
        const qsizetype n = formLength(u.form, s, p);
        if (n > 0) {
            *end = p + n;
            return true;
        }
        return false;
    }
    case Unit::T::Literal:
        if (p < s.size() && s[p] == u.c) {
            *end = p + 1;
            return true;
        }
        return false;
    case Unit::T::Group: {
        if (p >= s.size() || s[p] != u.c)
            return false;
        const qsizetype e = groupEnd(s, p);
        if (e < 0)
            return false;
        *end = e;
        return true;
    }
    }
    return false;
}

// Согласный элемент у строк (пробел, поле с формой, литерал, группа в
// скобках) — без слов: для участков.
bool Builder::consensusUnit(const QVector<int>& lines, const QVector<qsizetype>& pos, Unit* out)
{
    if (lines.isEmpty())
        return false;
    const int need = int(lines.size() * kAgree + 0.999);
    int blanks = 0;
    for (int k = 0; k < lines.size(); ++k) {
        const QStringView s = L[lines[k]].s;
        if (pos[k] < s.size() && isBlank(s[pos[k]]))
            ++blanks;
    }
    m_steps += lines.size();
    if (blanks >= need) {
        out->t = Unit::T::Space;
        return true;
    }
    for (Kind kind : kFormOrder) {
        int n = 0;
        for (int k = 0; k < lines.size(); ++k) {
            if (formLength(kind, L[lines[k]].s, pos[k]) > 0)
                ++n;
        }
        m_steps += lines.size();
        if (n >= need) {
            out->t = Unit::T::Form;
            out->form = kind;
            if (kind == Kind::Int) {
                for (int k = 0; k < lines.size(); ++k) {
                    if (fractionFollows(L[lines[k]].s, pos[k])) {
                        out->form = Kind::Num;
                        break;
                    }
                }
            }
            return true;
        }
    }
    QHash<QChar, int> chars;
    for (int k = 0; k < lines.size(); ++k) {
        const QStringView s = L[lines[k]].s;
        if (pos[k] < s.size() && !isWordish(s[pos[k]]) && !isBlank(s[pos[k]]))
            ++chars[s[pos[k]]];
    }
    for (auto it = chars.cbegin(); it != chars.cend(); ++it) {
        if (it.value() >= need) {
            out->c = it.key();
            out->t = isOpening(it.key()) ? Unit::T::Group : Unit::T::Literal;
            if (out->t == Unit::T::Group) {
                int closed = 0;
                for (int k = 0; k < lines.size(); ++k) {
                    if (groupEnd(L[lines[k]].s, pos[k]) > 0)
                        ++closed;
                }
                if (closed < need)
                    out->t = Unit::T::Literal;
            }
            return true;
        }
    }
    return false;
}

// Строки a (на позициях posA) и b (на своих позициях) согласны в том, что
// стоит следом.
bool Builder::agree(const QVector<int>& a, const QVector<qsizetype>& posA, const QVector<int>& b,
                    Sig* agreed)
{
    QVector<Sig> sigs;
    sigs.reserve(a.size() + b.size());
    for (int k = 0; k < a.size(); ++k)
        sigs.append(sigAt(L[a[k]].s, posA[k]));
    for (int i : b)
        sigs.append(sigAt(L[i].s, L[i].pos));
    // Самая частая сигнатура (сигнатур мало — квадрат по различным).
    QVector<Sig> kinds;
    QVector<int> counts;
    for (const Sig& g : sigs) {
        int j = 0;
        for (; j < kinds.size(); ++j) {
            if (kinds[j] == g)
                break;
        }
        if (j == kinds.size()) {
            kinds.append(g);
            counts.append(0);
        }
        ++counts[j];
        ++m_steps;
    }
    int best = 0;
    for (int j = 0; j < counts.size(); ++j) {
        if (counts[j] > best) {
            best = counts[j];
            if (agreed)
                *agreed = kinds[j];
        }
    }
    return best >= int(sigs.size() * kAgree + 0.999);
}

void Builder::emitSpace(int opt)
{
    if (!m_out.isEmpty() && m_out.constLast().type == Elem::Type::Space && m_out.constLast().opt == opt)
        return;
    Elem e;
    e.type = Elem::Type::Space;
    e.opt = opt;
    m_out.append(e);
}

void Builder::emitLiteral(const QString& text, int opt)
{
    Elem e;
    e.type = Elem::Type::Literal;
    e.text = text;
    e.opt = opt;
    m_out.append(e);
}

Elem& Builder::emitField(Kind kind, int opt)
{
    Elem e;
    e.type = Elem::Type::Field;
    e.kind = kind;
    e.opt = opt;
    m_out.append(e);
    return m_out.last();
}

void Builder::emitMessage()
{
    int prev = int(m_out.size()) - 1;
    while (prev >= 0 && m_out[prev].type == Elem::Type::Space)
        --prev;
    if (prev >= 0 && prev < int(m_out.size()) - 1 && m_out[prev].type == Elem::Type::Field
        && m_out[prev].opt < 0)
        messageAfter = prev;
    Elem& f = emitField(Kind::Rest);
    f.name = QStringLiteral("Message");
    for (Line& l : L) {
        if (!l.active)
            continue;
        const qsizetype a = skipBlanks(l.s, l.pos);
        if (f.samples.size() < kMaxLines)
            f.samples.append(l.s.mid(a));
        l.pos = l.s.size();
    }
}

// Внутренность группы в скобках у строк lines: все — одна форма целиком → её
// вид; иначе text.
void Builder::groupInnerKind(const QVector<int>& lines, Kind* kind)
{
    *kind = Kind::Text;
    if (lines.isEmpty())
        return;
    for (Kind k : { Kind::Level, Kind::Time, Kind::Uuid, Kind::Ip, Kind::Int, Kind::Num, Kind::Hex }) {
        int n = 0;
        for (int i : lines) {
            const QStringView s = L[i].s;
            const qsizetype p = L[i].pos;
            const qsizetype e = groupEnd(s, p);
            const qsizetype len = formLength(k, s, p + 1);
            if (e > 0 && len > 0 && p + 1 + len == e - 1)
                ++n;
            ++m_steps;
        }
        if (n == lines.size()) {
            *kind = k;
            break;
        }
    }
}

// Элемент u у строк lines с позиций pos: добавить в шаблон, сдвинуть позиции.
void Builder::emitUnit(const Unit& u, const QVector<int>& lines, QVector<qsizetype>* pos, int opt)
{
    switch (u.t) {
    case Unit::T::Space:
        emitSpace(opt);
        for (int k = 0; k < lines.size(); ++k)
            (*pos)[k] = skipBlanks(L[lines[k]].s, (*pos)[k]);
        return;
    case Unit::T::Literal:
        emitLiteral(QString(u.c), opt);
        for (int k = 0; k < lines.size(); ++k)
            ++(*pos)[k];
        return;
    case Unit::T::Form: {
        Elem& f = emitField(u.form, opt);
        for (int k = 0; k < lines.size(); ++k) {
            const QStringView s = L[lines[k]].s;
            const qsizetype n = formLength(u.form, s, (*pos)[k]);
            if (n > 0) {
                if (f.samples.size() < kMaxLines)
                    f.samples.append(s.mid((*pos)[k], n));
                (*pos)[k] += n;
            }
        }
        return;
    }
    case Unit::T::Group: {
        emitLiteral(QString(u.c), opt);
        // Внутренность: форма целиком или text до закрывающей.
        Kind inner = Kind::Text;
        {
            QVector<int> tmp = lines;
            QVector<qsizetype> saved;
            for (int k = 0; k < lines.size(); ++k) {
                saved.append(L[lines[k]].pos);
                L[lines[k]].pos = (*pos)[k];
            }
            groupInnerKind(tmp, &inner);
            for (int k = 0; k < lines.size(); ++k)
                L[lines[k]].pos = saved[k];
        }
        Elem& f = emitField(inner, opt);
        f.bracketed = true;
        for (int k = 0; k < lines.size(); ++k) {
            const QStringView s = L[lines[k]].s;
            const qsizetype e = groupEnd(s, (*pos)[k]);
            if (e > 0) {
                if (f.samples.size() < kMaxLines)
                    f.samples.append(s.mid((*pos)[k] + 1, e - 1 - (*pos)[k] - 1).trimmed());
                (*pos)[k] = e;
            }
        }
        emitLiteral(QString(closingOf(u.c)), opt);
        return;
    }
    }
}

// Необязательный участок: строки x начинаются с first, остальные — нет.
// Участок растёт по согласным у x элементам, пока то, что идёт следом у x,
// не сойдётся с тем, что стоит у остальных.
bool Builder::tryOptional(const QVector<int>& act, const QVector<int>& x, const Unit& first)
{
    QSet<int> inX(x.cbegin(), x.cend());
    QVector<int> rest;
    for (int i : act) {
        if (!inX.contains(i))
            rest.append(i);
    }
    if (rest.isEmpty() || x.isEmpty())
        return false;
    QVector<Unit> units;
    QVector<qsizetype> pos;
    for (int i : x)
        pos.append(L[i].pos);
    Unit u = first;
    for (int step = 0; step < kMaxOptionalUnits; ++step) {
        // Применить u ко всем x (не сошлось у кого-то — участок не выходит).
        for (int k = 0; k < x.size(); ++k) {
            qsizetype e = 0;
            if (!unitAt(u, L[x[k]].s, pos[k], &e))
                return false;
            pos[k] = e;
        }
        units.append(u);
        Sig agreed;
        if (agree(x, pos, rest, &agreed)) {
            // Следом слова у меньшинства строк — это уже текст сообщения, а не
            // участок заголовка.
            if (agreed.t == Sig::T::Word && x.size() * 2 < act.size())
                return false;
            break;
        }
        if (step + 1 == kMaxOptionalUnits || !consensusUnit(x, pos, &u))
            return false;
    }
    // Участок из одних пробелов — только если следом не то же самое.
    const int opt = m_nextOpt++;
    QVector<qsizetype> p2;
    for (int i : x)
        p2.append(L[i].pos);
    for (const Unit& v : units)
        emitUnit(v, x, &p2, opt);
    for (int k = 0; k < x.size(); ++k)
        L[x[k]].pos = p2[k];
    return true;
}

// Повтор групп в скобках подряд: у части строк на одну группу больше
// («[app] [thread]» и «[thread]») — первая группа необязательна.
bool Builder::tryLeadingGroups(const QVector<int>& act)
{
    QHash<int, int> byCount;
    QVector<int> counts(act.size());
    for (int k = 0; k < act.size(); ++k) {
        const QStringView s = L[act[k]].s;
        qsizetype p = L[act[k]].pos;
        int g = 0;
        while (g < 4 && p < s.size() && isOpening(s[p])) {
            const qsizetype e = groupEnd(s, p);
            if (e < 0)
                break;
            ++g;
            p = skipBlanks(s, e);
            ++m_steps;
        }
        counts[k] = g;
        ++byCount[g];
    }
    for (auto it = byCount.cbegin(); it != byCount.cend(); ++it) {
        const int g = it.key();
        if (g != 1)
            continue;   // «[a] [b]» и «[b]»; лишняя группа в конце — участок на своём месте
        const int more = byCount.value(g + 1);
        const int both = it.value() + more;
        if (more < act.size() * kOptionalMin || it.value() < act.size() * kOptionalMin
            || both < act.size() * kAgree)
            continue;
        QVector<int> x;
        for (int k = 0; k < act.size(); ++k) {
            if (counts[k] == g + 1)
                x.append(act[k]);
        }
        // Какая группа лишняя: первое слово внутри первой группы у строк с
        // одной группой чаще совпадает со второй группой строк с двумя
        // («[app] [main]» и «[main]») — первая; с первой («[pid …]
        // [client …]» и «[pid …]») — последняя, это участок на своём месте.
        const auto innerWord = [](QStringView s, qsizetype p) {
            const qsizetype e = groupEnd(s, p);
            qsizetype a = skipBlanks(s, p + 1);
            qsizetype b = a;
            while (b < e - 1 && !isBlank(s[b]) && s[b] != QLatin1Char(':'))
                ++b;
            QString w;
            for (qsizetype i = a; i < b; ++i)
                w += s[i].isDigit() ? QLatin1Char('#') : s[i];
            return w;
        };
        QSet<QString> single;
        for (int k = 0; k < act.size(); ++k) {
            if (counts[k] == g)
                single.insert(innerWord(L[act[k]].s, L[act[k]].pos));
        }
        int firstSame = 0, secondSame = 0;
        for (int i : x) {
            const QStringView s = L[i].s;
            const qsizetype p = L[i].pos;
            firstSame += single.contains(innerWord(s, p)) ? 1 : 0;
            secondSame += single.contains(innerWord(s, skipBlanks(s, groupEnd(s, p)))) ? 1 : 0;
            m_steps += 2;
        }
        if (secondSame <= firstSame)
            return false;
        Unit grp;
        grp.t = Unit::T::Group;
        grp.c = L[x.first()].s[L[x.first()].pos];
        return tryOptional(act, x, grp);
    }
    return false;
}

// Конец строки у 15–95 % строк: остальное — необязательный хвост, если у
// остальных он согласен поэлементно до конца.
bool Builder::tryTailOptional(const QVector<int>& act)
{
    QVector<int> x;
    for (int i : act) {
        if (!atEnd(L[i].s, L[i].pos))
            x.append(i);
    }
    if (x.isEmpty())
        return false;
    QVector<qsizetype> pos;
    for (int i : x)
        pos.append(L[i].pos);
    QVector<Unit> units;
    for (int step = 0; step < 12; ++step) {
        int ended = 0;
        for (int k = 0; k < x.size(); ++k) {
            if (atEnd(L[x[k]].s, pos[k]))
                ++ended;
        }
        if (ended >= x.size() * kAgree)
            break;
        Unit u;
        if (!consensusUnit(x, pos, &u))
            return false;
        for (int k = 0; k < x.size(); ++k) {
            qsizetype e = 0;
            if (!unitAt(u, L[x[k]].s, pos[k], &e))
                return false;
            pos[k] = e;
        }
        units.append(u);
        if (step == 11)
            return false;
    }
    if (units.isEmpty())
        return false;
    bool anyField = false;
    for (const Unit& u : units)
        anyField = anyField || u.t == Unit::T::Form || u.t == Unit::T::Group;
    if (!anyField)
        return false;
    const int opt = m_nextOpt++;
    QVector<qsizetype> p2;
    for (int i : x)
        p2.append(L[i].pos);
    for (const Unit& v : units)
        emitUnit(v, x, &p2, opt);
    for (int k = 0; k < x.size(); ++k)
        L[x[k]].pos = p2[k];
    return true;
}

// ---------------------------------------------------------------------------
// Якорь: литерал или поле с формой, что стоит следом в ≥ 95 % строк.
// ---------------------------------------------------------------------------

struct AnchorHit {
    qsizetype at = -1;     // начало якоря (без пробелов перед ним)
    qsizetype spanEnd = -1;// конец текста поля (без пробелов)
    int words = 0;         // слов в тексте поля
};

struct AnchorStat {
    bool form = false;
    Kind kind = Kind::Text;
    QString text;          // литерал
    bool blankBefore = false;
    QVector<AnchorHit> hits;   // по строке act; at < 0 — нет
    int count = 0;
    qsizetype medianOffset = 0;
};

// Слов в [a, b).
int wordsIn(QStringView s, qsizetype a, qsizetype b)
{
    int w = 0;
    bool in = false;
    for (qsizetype q = a; q < b; ++q) {
        const bool blank = isBlank(s[q]);
        if (!blank && !in)
            ++w;
        in = !blank;
    }
    return w;
}

bool Builder::tryAnchor(const QVector<int>& act)
{
    const int n = int(act.size());
    // Скобка, открытая последним литералом: закрывающая — якорь на любом
    // расстоянии в окне.
    QChar openBracket;
    {
        int j = int(m_out.size()) - 1;
        while (j >= 0 && m_out[j].type == Elem::Type::Space)
            --j;
        if (j >= 0 && m_out[j].type == Elem::Type::Literal && !m_out[j].text.isEmpty()
            && isOpening(m_out[j].text.back()))
            openBracket = m_out[j].text.back();
    }
    const QChar closeBracket = openBracket.isNull() ? QChar() : closingOf(openBracket);
    QHash<QString, int> index;
    QVector<AnchorStat> stats;
    const auto statFor = [&](const QString& key) -> AnchorStat& {
        auto it = index.constFind(key);
        if (it != index.constEnd())
            return stats[*it];
        index.insert(key, int(stats.size()));
        AnchorStat st;
        st.hits.resize(n);
        stats.append(st);
        return stats.last();
    };

    for (int k = 0; k < n; ++k) {
        const QStringView s = L[act[k]].s;
        const qsizetype start = skipBlanks(s, L[act[k]].pos);
        const qsizetype limit = std::min<qsizetype>(s.size(), start + kWindowChars);
        int word = 0;
        bool far = false;   // дальше kMaxAnchorWords слов: только закрывающая скобка
        QSet<QString> seen;
        for (qsizetype q = start; q < limit; ++q) {
            ++m_steps;
            const QChar c = s[q];
            if (isBlank(c))
                continue;
            const bool chunkStart = q == start || isBlank(s[q - 1]);
            if (chunkStart && q > start) {
                ++word;
                if (word > kMaxAnchorWords + 1) {
                    if (closeBracket.isNull())
                        break;
                    far = true;
                }
            }
            if (far && c != closeBracket)
                continue;
            const bool blankBefore = q > start && isBlank(s[q - 1]);
            // Литерал: знак-разделитель или отдельный кусок из одних знаков.
            QString lit;
            if (isStructural(c) && c != QLatin1Char('"')) {
                lit = QString(c);
            } else if (chunkStart && !isWordish(c)) {
                const qsizetype len = chunkLength(s, q);
                bool punct = true;
                for (qsizetype i = q; i < q + len && punct; ++i)
                    punct = !isWordish(s[i]) && !isStructural(s[i]);
                if (punct && len <= 4)
                    lit = s.mid(q, len).toString();
            }
            if (!lit.isEmpty() && q > start) {
                const QString key = QLatin1Char(blankBefore ? 'b' : 'n') + lit;
                if (!seen.contains(key)) {
                    seen.insert(key);
                    AnchorStat& st = statFor(key);
                    st.text = lit;
                    st.blankBefore = blankBefore;
                    qsizetype e = q;
                    while (e > start && isBlank(s[e - 1]))
                        --e;
                    st.hits[k] = { q, e, wordsIn(s, start, e) };
                    ++st.count;
                }
            }
            // Поле с формой в начале слова (после пробела).
            if (chunkStart && q > start) {
                for (Kind kind : { Kind::Level, Kind::Time, Kind::Ip, Kind::Uuid, Kind::Quoted,
                                   Kind::Int, Kind::Num }) {
                    if (formLength(kind, s, q) <= 0)
                        continue;
                    const QString key = QStringLiteral("f%1").arg(int(kind));
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                    AnchorStat& st = statFor(key);
                    st.form = true;
                    st.kind = kind;
                    st.blankBefore = true;
                    qsizetype e = q;
                    while (e > start && isBlank(s[e - 1]))
                        --e;
                    st.hits[k] = { q, e, wordsIn(s, start, e) };
                    ++st.count;
                }
            }
        }
    }

    // Кандидаты — в ≥ 95 % строк, ближайшие первыми.
    QVector<int> order;
    for (int i = 0; i < stats.size(); ++i) {
        AnchorStat& st = stats[i];
        if (st.count < n * kAgree)
            continue;
        QVector<qsizetype> offs;
        for (int k = 0; k < n; ++k) {
            if (st.hits[k].at >= 0)
                offs.append(st.hits[k].at - L[act[k]].pos);
        }
        std::nth_element(offs.begin(), offs.begin() + offs.size() / 2, offs.end());
        st.medianOffset = offs[offs.size() / 2];
        order.append(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (stats[a].medianOffset != stats[b].medianOffset)
            return stats[a].medianOffset < stats[b].medianOffset;
        return !stats[a].form && stats[b].form;   // литерал — раньше формы
    });

    for (int idx : order) {
        const AnchorStat& st = stats[idx];
        const bool closesBracket = !st.form && !closeBracket.isNull()
            && st.text == QString(closeBracket);
        int empty = 0;
        QVector<int> words;
        for (int k = 0; k < n; ++k) {
            const AnchorHit& h = st.hits[k];
            if (h.at < 0)
                continue;
            if (h.words == 0)
                ++empty;
            words.append(h.words);
        }
        if (empty > words.size() * (1.0 - kAgree))
            continue;
        std::sort(words.begin(), words.end());
        const int p95 = words[std::min<int>(int(words.size()) - 1, int(words.size() * kAgree))];
        if (!closesBracket && p95 > kMaxAnchorWords)
            continue;
        // Постоянное число слов.
        QHash<int, int> byWords;
        for (int w : words)
            ++byWords[w];
        int mode = 0, modeCount = 0;
        for (auto it = byWords.cbegin(); it != byWords.cend(); ++it) {
            if (it.value() > modeCount || (it.value() == modeCount && it.key() < mode)) {
                mode = it.key();
                modeCount = it.value();
            }
        }
        const bool constant = !closesBracket && mode >= 1 && modeCount >= words.size() * kConstantWords;
        if (st.form && !constant) {
            // Форма следом при переменном числе слов — только «сильная» форма.
            if (st.kind == Kind::Int || st.kind == Kind::Num)
                continue;
        }

        if (constant) {
            // mode полей word; у последнего слова — отрезать группу в скобках
            // на конце («sshd[1234]»), если так у ≥ 15 % строк.
            int withGroup = 0;
            QVector<qsizetype> cut(n, -1);
            for (int k = 0; k < n; ++k) {
                const AnchorHit& h = st.hits[k];
                if (h.at < 0 || h.words != mode)
                    continue;
                const QStringView s = L[act[k]].s;
                if (h.spanEnd > 0) {
                    const QChar last = s[h.spanEnd - 1];
                    for (const QChar open : { QLatin1Char('['), QLatin1Char('(') }) {
                        if (last != closingOf(open))
                            continue;
                        qsizetype q = h.spanEnd - 2;
                        while (q > L[act[k]].pos && !isBlank(s[q]) && s[q] != open)
                            --q;
                        if (q > L[act[k]].pos && s[q] == open && !isBlank(s[q - 1])) {
                            cut[k] = q;
                            ++withGroup;
                        }
                    }
                }
            }
            const bool cutGroups = withGroup >= n * kOptionalMin;
            const int first = int(m_out.size());
            for (int w = 0; w < mode; ++w) {
                if (w > 0)
                    emitSpace();
                emitField(Kind::Word);
            }
            for (int k = 0; k < n; ++k) {
                Line& l = L[act[k]];
                const AnchorHit& h = st.hits[k];
                if (h.at < 0 || h.words != mode) {
                    l.active = false;
                    continue;
                }
                qsizetype p = skipBlanks(l.s, l.pos);
                for (int w = 0; w < mode; ++w) {
                    p = skipBlanks(l.s, p);
                    qsizetype e = p;
                    while (e < h.spanEnd && !isBlank(l.s[e]))
                        ++e;
                    if (w == mode - 1 && cutGroups && cut[k] > p)
                        e = cut[k];
                    Elem& f = m_out[first + w * 2];
                    if (f.samples.size() < kMaxLines)
                        f.samples.append(l.s.mid(p, e - p));
                    p = e;
                }
                l.pos = p;
            }
            return true;
        }

        Elem& f = emitField(Kind::Text);
        f.bracketed = closesBracket;
        f.hard = st.form;
        for (int k = 0; k < n; ++k) {
            Line& l = L[act[k]];
            const AnchorHit& h = st.hits[k];
            if (h.at < 0) {
                l.active = false;
                continue;
            }
            const qsizetype a = skipBlanks(l.s, l.pos);
            if (f.samples.size() < kMaxLines)
                f.samples.append(l.s.mid(a, std::max<qsizetype>(0, h.spanEnd - a)));
            l.pos = h.spanEnd;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Трудная граница: промежуток, выравнивание, число слов, набор значений.
// ---------------------------------------------------------------------------

// Слово для разнообразия продолжений: число — один знак «#».
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

bool Builder::tryHardBoundary(const QVector<int>& act)
{
    const int n = int(act.size());
    if (n < 10)
        return false;

    // 1. Промежуток из двух и более пробелов (или табуляция) не дальше
    //    kMaxAnchorWords слов.
    {
        QVector<qsizetype> gap(n, -1);
        int found = 0;
        for (int k = 0; k < n; ++k) {
            const QStringView s = L[act[k]].s;
            const qsizetype start = skipBlanks(s, L[act[k]].pos);
            const qsizetype limit = std::min<qsizetype>(s.size(), start + kWindowChars);
            int words = 1;
            for (qsizetype q = start + 1; q < limit; ++q) {
                ++m_steps;
                if (!isBlank(s[q]))
                    continue;
                const bool wide = s[q] == QLatin1Char('\t')
                    || (q + 1 < s.size() && isBlank(s[q + 1]));
                if (wide) {
                    if (!atEnd(s, q)) {
                        gap[k] = q;
                        ++found;
                    }
                    break;
                }
                if (++words > kMaxAnchorWords)
                    break;
            }
        }
        if (found >= n * kAgree) {
            Elem& f = emitField(Kind::Padded);
            f.hard = true;
            for (int k = 0; k < n; ++k) {
                Line& l = L[act[k]];
                if (gap[k] < 0) {
                    l.active = false;
                    continue;
                }
                const qsizetype a = skipBlanks(l.s, l.pos);
                if (f.samples.size() < kMaxLines)
                    f.samples.append(l.s.mid(a, gap[k] - a));
                l.pos = gap[k];
            }
            return true;
        }
    }

    // 2. Выравнивание: следующее поле у всех начинается через W знаков
    //    (после одного пробела), и у части строк значение короче W.
    {
        constexpr int kMaxWidth = 64;
        QVector<int> hits(kMaxWidth + 1, 0);
        QVector<int> padded(kMaxWidth + 1, 0);
        for (int k = 0; k < n; ++k) {
            const QStringView s = L[act[k]].s;
            const qsizetype p = L[act[k]].pos;
            for (int w = 2; w <= kMaxWidth; ++w) {
                ++m_steps;
                const qsizetype q = p + w;
                if (q + 1 >= s.size())
                    break;
                if (isBlank(s[q]) && !isBlank(s[q + 1]) && !isBlank(s[p])) {
                    ++hits[w];
                    if (isBlank(s[q - 1]))
                        ++padded[w];
                }
            }
        }
        for (int w = 2; w <= kMaxWidth; ++w) {
            if (hits[w] < n * kAgree || padded[w] < n * kOptionalMin)
                continue;
            Elem& f = emitField(Kind::Width);
            f.count = w;
            f.hard = true;
            for (int k = 0; k < n; ++k) {
                Line& l = L[act[k]];
                const qsizetype q = l.pos + w;
                if (q + 1 >= l.s.size() || !isBlank(l.s[q]) || isBlank(l.s[q + 1])) {
                    l.active = false;
                    continue;
                }
                if (f.samples.size() < kMaxLines)
                    f.samples.append(l.s.mid(l.pos, w).trimmed());
                l.pos = q;
            }
            return true;
        }
    }

    // 3–4. Разнообразие продолжений: для начала из 1–4 слов — сколько разных
    //      слов следом. Внутри названия продолжение почти всегда одно, на
    //      границе — много разных.
    struct Words {
        QVector<QStringView> w;    // слова с позиции
        QVector<qsizetype> ends;   // конец каждого слова
    };
    QVector<Words> lw(n);
    for (int k = 0; k < n; ++k) {
        const QStringView s = L[act[k]].s;
        qsizetype p = skipBlanks(s, L[act[k]].pos);
        for (int w = 0; w <= kDiversityWords && p < s.size(); ++w) {
            const qsizetype len = chunkLength(s, p);
            m_steps += len + 1;
            lw[k].w.append(s.mid(p, len));
            lw[k].ends.append(p + len);
            p = skipBlanks(s, p + len);
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
    // Счётчики начал и продолжений.
    QHash<QString, int> prefixCount;
    QHash<QString, QHash<QString, int>> next;
    for (int k = 0; k < n; ++k) {
        for (int words = 1; words <= kDiversityWords && words <= lw[k].w.size(); ++words) {
            const QString p = prefix(k, words);
            ++prefixCount[p];
            const QString cont = words < lw[k].w.size() ? normalizedWord(lw[k].w[words])
                                                        : QStringLiteral("\n");
            ++next[p][cont];
            ++m_steps;
        }
    }
    QVector<int> boundary(n, 0);   // слов в значении, 0 — не определено
    int resolved = 0;
    for (int k = 0; k < n; ++k) {
        for (int words = 1; words <= kDiversityWords && words <= lw[k].w.size(); ++words) {
            const QString p = prefix(k, words);
            const int c = prefixCount.value(p);
            if (c < kMinValueCount)
                break;
            const QHash<QString, int>& conts = next[p];
            int top = 0;
            for (int v : conts)
                top = std::max(top, v);
            ++m_steps;
            if (conts.size() >= 3 && top < 0.5 * c) {
                if (words < lw[k].w.size()) {   // следом что-то есть
                    boundary[k] = words;
                    ++resolved;
                }
                break;
            }
            if (top < 0.8 * c)
                break;   // ни то ни другое
        }
    }
    if (resolved >= n * kAgree) {
        QHash<int, int> byWords;
        for (int b : boundary) {
            if (b > 0)
                ++byWords[b];
        }
        int mode = 0, modeCount = 0;
        for (auto it = byWords.cbegin(); it != byWords.cend(); ++it) {
            if (it.value() > modeCount) {
                mode = it.key();
                modeCount = it.value();
            }
        }
        // Значений у поля немного (иначе это первое слово сообщения).
        QSet<QString> distinctValues;
        for (int k = 0; k < n; ++k) {
            if (boundary[k] > 0)
                distinctValues.insert(prefix(k, boundary[k]));
        }
        const int maxValues = std::max(kMaxValues, int(n * 0.02));
        if (modeCount >= n * kConstantWords && distinctValues.size() <= maxValues) {
            Elem& f = emitField(mode == 1 ? Kind::Word : Kind::Words);
            f.count = mode;
            f.hard = true;
            for (int k = 0; k < n; ++k) {
                Line& l = L[act[k]];
                if (lw[k].w.size() <= mode) {
                    l.active = false;
                    continue;
                }
                const qsizetype a = skipBlanks(l.s, l.pos);
                const qsizetype e = lw[k].ends[mode - 1];
                if (f.samples.size() < kMaxLines)
                    f.samples.append(l.s.mid(a, e - a));
                l.pos = e;
            }
            return true;
        }
        // Закрытый набор значений.
        QHash<QString, int> values;
        for (int k = 0; k < n; ++k) {
            if (boundary[k] > 0)
                ++values[prefix(k, boundary[k])];
        }
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
            Elem& f = emitField(Kind::Of);
            f.hard = true;
            QSet<QString> set;
            for (const auto& v : list) {
                f.values.append(v.first);
                set.insert(v.first);
            }
            for (int k = 0; k < n; ++k) {
                Line& l = L[act[k]];
                if (boundary[k] == 0 || !set.contains(prefix(k, boundary[k]))) {
                    l.active = false;
                    continue;
                }
                const qsizetype a = skipBlanks(l.s, l.pos);
                const qsizetype e = lw[k].ends[boundary[k] - 1];
                if (f.samples.size() < kMaxLines)
                    f.samples.append(l.s.mid(a, e - a));
                l.pos = e;
            }
            return true;
        }
    }
    return false;
}

QVector<Elem> Builder::run()
{
    for (int iter = 0; iter < kMaxElements; ++iter) {
        if (cancelled())
            break;
        const QVector<int> act = active();
        const int n = int(act.size());
        if (n == 0)
            break;
        int fields = 0;
        for (const Elem& e : m_out)
            fields += e.type == Elem::Type::Field;
        if (fields >= kMaxFields - 1 || n < m_initial * 0.5) {
            emitMessage();
            break;
        }
        const int need = int(n * kAgree + 0.999);

        // Конец строки.
        int ended = 0, blanks = 0;
        for (int i : act) {
            ++m_steps;
            const Line& l = L[i];
            if (atEnd(l.s, l.pos))
                ++ended;
            else if (isBlank(l.s[l.pos]))
                ++blanks;
        }
        if (ended >= need)
            break;
        if (ended >= n * kOptionalMin) {
            if (!tryTailOptional(act))
                emitMessage();
            break;
        }
        for (int i : act) {
            if (atEnd(L[i].s, L[i].pos))
                L[i].active = false;   // строка кончилась раньше шаблона
        }
        if (ended > 0)
            continue;

        // Пробелы.
        if (blanks >= need) {
            emitSpace();
            for (int i : act)
                L[i].pos = skipBlanks(L[i].s, L[i].pos);
            for (int i : act) {
                if (L[i].pos == 0 || !isBlank(L[i].s[L[i].pos - 1]))
                    L[i].active = false;
            }
            continue;
        }
        if (blanks > 0) {
            QVector<int> x;
            for (int i : act) {
                if (isBlank(L[i].s[L[i].pos]))
                    x.append(i);
            }
            Unit sp;
            if (tryOptional(act, x, sp))
                continue;
            if (blanks >= n * kAgreeLoose) {
                emitSpace();
                for (int i : act) {
                    if (isBlank(L[i].s[L[i].pos]))
                        L[i].pos = skipBlanks(L[i].s, L[i].pos);
                    else
                        L[i].active = false;
                }
                continue;
            }
        }

        // Знак на позиции.
        QHash<QChar, int> chars;
        for (int i : act) {
            const Line& l = L[i];
            if (l.pos < l.s.size() && !isWordish(l.s[l.pos]) && !isBlank(l.s[l.pos]))
                ++chars[l.s[l.pos]];
        }
        QChar topChar;
        int topCount = 0;
        for (auto it = chars.cbegin(); it != chars.cend(); ++it) {
            if (it.value() > topCount || (it.value() == topCount && it.key() < topChar)) {
                topChar = it.key();
                topCount = it.value();
            }
        }
        if (topCount >= need && isOpening(topChar) && tryLeadingGroups(act))
            continue;

        // Поле с формой.
        Kind bestForm = Kind::Text;
        int bestCount = 0;
        bool formDone = false;
        for (Kind kind : kFormOrder) {
            QVector<qsizetype> len(n, -1);
            int count = 0;
            bool fraction = false;
            for (int k = 0; k < n; ++k) {
                len[k] = formLength(kind, L[act[k]].s, L[act[k]].pos);
                if (len[k] > 0)
                    ++count;
                if (kind == Kind::Int && !fraction)
                    fraction = fractionFollows(L[act[k]].s, L[act[k]].pos);
            }
            if (fraction)
                continue;   // дальше — num
            m_steps += n;
            if (count >= need) {
                Elem& f = emitField(kind);
                for (int k = 0; k < n; ++k) {
                    Line& l = L[act[k]];
                    if (len[k] <= 0) {
                        l.active = false;
                        continue;
                    }
                    if (f.samples.size() < kMaxLines)
                        f.samples.append(l.s.mid(l.pos, len[k]));
                    l.pos += len[k];
                }
                formDone = true;
                break;
            }
            if (count > bestCount) {
                bestCount = count;
                bestForm = kind;
            }
        }
        if (formDone)
            continue;

        // Знак-литерал.
        if (topCount >= need) {
            emitLiteral(QString(topChar));
            for (int i : act) {
                Line& l = L[i];
                if (l.pos < l.s.size() && l.s[l.pos] == topChar)
                    ++l.pos;
                else
                    l.active = false;
            }
            continue;
        }

        // Необязательный участок: форма или знак (группа в скобках) у 15–95 %.
        bool optionalDone = false;
        if (bestCount >= n * kOptionalMin && bestCount >= topCount) {
            QVector<int> x;
            for (int i : act) {
                if (formLength(bestForm, L[i].s, L[i].pos) > 0)
                    x.append(i);
            }
            Unit u;
            u.t = Unit::T::Form;
            u.form = bestForm;
            optionalDone = tryOptional(act, x, u);
            if (!optionalDone && bestCount * 2 >= n) {
                // Форма у большинства, у остальных — другое слово на её месте
                // («-» вместо числа байтов): поле word.
                int words = 0;
                for (int i : act) {
                    const qsizetype len = chunkLength(L[i].s, L[i].pos);
                    if (len > 0 && L[i].pos + len < L[i].s.size())
                        ++words;
                }
                if (words >= need) {
                    Elem& f = emitField(Kind::Word);
                    for (int i : act) {
                        Line& l = L[i];
                        const qsizetype len = chunkLength(l.s, l.pos);
                        if (len <= 0 || l.pos + len >= l.s.size()) {
                            l.active = false;
                            continue;
                        }
                        if (f.samples.size() < kMaxLines)
                            f.samples.append(l.s.mid(l.pos, len));
                        l.pos += len;
                    }
                    optionalDone = true;
                }
            }
            if (!optionalDone && bestCount >= n * kAgreeLoose) {
                Elem& f = emitField(bestForm);
                for (int i : act) {
                    Line& l = L[i];
                    const qsizetype len = formLength(bestForm, l.s, l.pos);
                    if (len <= 0) {
                        l.active = false;
                        continue;
                    }
                    if (f.samples.size() < kMaxLines)
                        f.samples.append(l.s.mid(l.pos, len));
                    l.pos += len;
                }
                optionalDone = true;
            }
        }
        if (!optionalDone && topCount >= n * kOptionalMin) {
            QVector<int> x;
            for (int i : act) {
                if (L[i].pos < L[i].s.size() && L[i].s[L[i].pos] == topChar)
                    x.append(i);
            }
            Unit u;
            u.t = isOpening(topChar) ? Unit::T::Group : Unit::T::Literal;
            u.c = topChar;
            optionalDone = tryOptional(act, x, u);
            if (!optionalDone && u.t == Unit::T::Group) {
                u.t = Unit::T::Literal;
                optionalDone = tryOptional(act, x, u);
            }
        }
        if (optionalDone)
            continue;

        // Два вида строк: на позиции у большинства один знак или форма, у
        // ≥ 15 % — другой, и участком это не выразить. Дальше — только
        // большинство (остальные строки — заметка TwoKinds по итогу).
        {
            QVector<Sig> sigs;
            QVector<int> counts;
            QVector<int> of(n);
            for (int k = 0; k < n; ++k) {
                const Sig g = sigAt(L[act[k]].s, L[act[k]].pos);
                int j = 0;
                for (; j < sigs.size(); ++j) {
                    if (sigs[j] == g)
                        break;
                }
                if (j == sigs.size()) {
                    sigs.append(g);
                    counts.append(0);
                }
                ++counts[j];
                of[k] = j;
            }
            int top = 0, second = -1;
            for (int j = 1; j < sigs.size(); ++j) {
                if (counts[j] > counts[top])
                    top = j;
            }
            for (int j = 0; j < sigs.size(); ++j) {
                if (j != top && (second < 0 || counts[j] > counts[second]))
                    second = j;
            }
            const auto structural = [](const Sig& g) { return g.t == Sig::T::Punct || g.t == Sig::T::Form; };
            if (second >= 0 && structural(sigs[top]) && structural(sigs[second])
                && counts[top] >= n * 0.5 && counts[second] >= n * kOptionalMin) {
                for (int k = 0; k < n; ++k) {
                    if (of[k] != top)
                        L[act[k]].active = false;
                }
                continue;
            }
        }

        // Короткий кусок (знак роли, буква уровня) и пробел следом.
        {
            int shortCount = 0;
            for (int i : act) {
                const qsizetype len = chunkLength(L[i].s, L[i].pos);
                if (len >= 1 && len <= 2 && !isBlank(L[i].s[L[i].pos])
                    && L[i].pos + len < L[i].s.size())
                    ++shortCount;
            }
            if (shortCount >= need) {
                if (tryAnchor(act))
                    continue;   // якорь нашёлся
                Elem& f = emitField(Kind::Word);
                for (int i : act) {
                    Line& l = L[i];
                    const qsizetype len = chunkLength(l.s, l.pos);
                    if (len < 1 || len > 2) {
                        l.active = false;
                        continue;
                    }
                    if (f.samples.size() < kMaxLines)
                        f.samples.append(l.s.mid(l.pos, len));
                    l.pos += len;
                }
                continue;
            }
        }

        // Слова: якорь следом, иначе «трудная граница», иначе Message.
        if (tryAnchor(act))
            continue;
        if (tryHardBoundary(act))
            continue;
        emitMessage();
        break;
    }
    return m_out;
}

// ---------------------------------------------------------------------------
// Имена колонок по статистике.
// ---------------------------------------------------------------------------

bool isLevelValue(QStringView v)
{
    if (StrToLevel(v) != LogLevel::Unknown)
        return true;
    // Буква уровня (logcat, glog): V D I W E F A.
    return v.size() == 1 && QStringView(u"VDIWEFA").contains(v[0]);
}

bool isDottedName(QStringView v)
{
    if (v.isEmpty() || v.contains(QLatin1Char(' ')))
        return false;
    int dots = 0;
    for (qsizetype i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == QLatin1Char('.') && v[i - 1].isLetterOrNumber() && v[i + 1].isLetter())
            ++dots;
    }
    return dots >= 1 && v[0].isLetter();
}

double share(const QVector<QStringView>& values, bool (*pred)(QStringView))
{
    if (values.isEmpty())
        return 0;
    int n = 0;
    for (QStringView v : values)
        n += pred(v) ? 1 : 0;
    return double(n) / values.size();
}

int distinctCount(const QVector<QStringView>& values)
{
    QSet<QString> set;
    for (QStringView v : values) {
        set.insert(v.toString());
        if (set.size() > SchemaFit::kDistinctCap)
            break;
    }
    return int(set.size());
}

void nameFields(QVector<Elem>& els)
{
    QSet<QString> used;
    const auto take = [&](Elem& e, const QString& name) {
        if (used.contains(name.toLower()))
            return false;
        used.insert(name.toLower());
        e.name = name;
        return true;
    };
    for (Elem& e : els) {
        if (e.type == Elem::Type::Field && e.kind == Kind::Rest)
            used.insert(e.name.toLower());   // Message
    }
    const auto prevNonSpace = [&](int i) {
        int j = i - 1;
        while (j >= 0 && els[j].type == Elem::Type::Space)
            --j;
        return j;
    };
    // Программа syslog: слово, за которым «[int]» (участком или нет) и «:».
    QVector<int> program;
    for (int i = 0; i < els.size(); ++i) {
        const Elem& e = els[i];
        if (e.type != Elem::Type::Field || e.kind != Kind::Word || !e.samples.size())
            continue;
        const int j = i + 1;
        if (j + 3 < els.size() && els[j].type == Elem::Type::Literal && els[j].text == QLatin1String("[")
            && els[j + 1].type == Elem::Type::Field && els[j + 1].kind == Kind::Int
            && els[j + 2].type == Elem::Type::Literal && els[j + 2].text.startsWith(QLatin1Char(']'))) {
            const int k = j + 3;
            if ((els[j + 2].text.size() > 1 && els[j + 2].text.at(1) == QLatin1Char(':'))
                || (k < els.size() && els[k].type == Elem::Type::Literal && els[k].text.startsWith(QLatin1Char(':'))))
                program.append(i);
        }
    }
    for (int i : program) {
        if (!take(els[i], QStringLiteral("Program")))
            continue;
        const int j = i + 2;   // [int]
        if (j < els.size() && els[j].type == Elem::Type::Field && els[j].kind == Kind::Int)
            take(els[j], QStringLiteral("PID"));
        const int h = prevNonSpace(i);
        if (h >= 0 && els[h].type == Elem::Type::Field && els[h].kind == Kind::Word && els[h].name.isEmpty())
            take(els[h], QStringLiteral("Host"));
    }
    for (int i = 0; i < els.size(); ++i) {
        Elem& e = els[i];
        if (e.type != Elem::Type::Field || !e.name.isEmpty())
            continue;
        switch (e.kind) {
        case Kind::Time:
            if (!take(e, QStringLiteral("Timestamp")))
                break;
            continue;
        case Kind::Level:
            if (!take(e, QStringLiteral("Level")))
                break;
            continue;
        case Kind::Ip:
            if (!take(e, QStringLiteral("Ip")))
                break;
            continue;
        case Kind::Uuid:
            if (!take(e, QStringLiteral("Id")))
                break;
            continue;
        case Kind::Hex:
            if (distinctCount(e.samples) > e.samples.size() / 2 && take(e, QStringLiteral("Id")))
                continue;
            break;
        case Kind::Int: {
            const int d = distinctCount(e.samples);
            bool status = !e.samples.isEmpty();
            for (QStringView v : e.samples) {
                const int code = v.toInt();
                status = status && v.size() == 3 && code >= 100 && code <= 599;
                if (!status)
                    break;
            }
            if (status && d >= 2 && take(e, QStringLiteral("Status")))
                continue;
            if ((d >= 2 || e.bracketed) && d <= std::max(3, int(e.samples.size() / 20))
                && take(e, QStringLiteral("PID")))
                continue;
            break;
        }
        default:
            break;
        }
        if (e.kind == Kind::Time || e.kind == Kind::Level || e.kind == Kind::Ip || e.kind == Kind::Uuid)
            continue;   // второе такое поле — FieldN
        const bool plain = e.kind == Kind::Word || e.kind == Kind::Text;
        if (plain && share(e.samples, isLevelValue) >= (e.bracketed ? 0.5 : 0.9)
            && take(e, QStringLiteral("Level")))
            continue;
        if (plain && share(e.samples, isDottedName) >= 0.6 && take(e, QStringLiteral("Logger")))
            continue;
        if (e.bracketed && e.kind == Kind::Text && distinctCount(e.samples) >= 2
            && take(e, QStringLiteral("Thread")))
            continue;
        if (e.hard && take(e, QStringLiteral("Component")))
            continue;
        if (e.kind == Kind::Path && take(e, QStringLiteral("Source")))
            continue;
    }
    int number = 0;
    for (Elem& e : els) {
        if (e.type != Elem::Type::Field || !e.name.isEmpty())
            continue;
        while (!take(e, QStringLiteral("Field%1").arg(++number))) {
        }
    }
}

// ---------------------------------------------------------------------------
// Шаблон из элементов.
// ---------------------------------------------------------------------------

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

QString render(const QVector<Elem>& els, QVector<int>* starts)
{
    QString t;
    int open = -1;
    if (starts)
        starts->clear();
    for (int i = 0; i < els.size(); ++i) {
        const Elem& e = els[i];
        if (open >= 0 && e.opt != open) {
            t += QLatin1String("?}");
            open = -1;
        }
        if (e.opt >= 0 && open != e.opt) {
            t += QLatin1String("{?");
            open = e.opt;
        }
        if (starts)
            starts->append(int(t.size()));
        switch (e.type) {
        case Elem::Type::Space:
            t += QLatin1Char(' ');
            break;
        case Elem::Type::Literal:
            t += LineTemplate::escapeLiteral(e.text);
            break;
        case Elem::Type::Field: {
            t += QLatin1Char('{');
            t += e.name;
            switch (e.kind) {
            case Kind::Text:
            case Kind::Rest:
                break;
            case Kind::Width:
            case Kind::Words:
                t += QStringLiteral(":%1(%2)").arg(LineTemplate::kindName(e.kind)).arg(e.count);
                break;
            case Kind::Of: {
                QStringList vs;
                for (const QString& v : e.values)
                    vs.append(escapeValue(v));
                t += QStringLiteral(":of(%1)").arg(vs.join(QLatin1Char('|')));
                break;
            }
            default:
                t += QLatin1Char(':') + LineTemplate::kindName(e.kind);
                break;
            }
            t += QLatin1Char('}');
            break;
        }
        }
    }
    if (open >= 0)
        t += QLatin1String("?}");
    return t;
}

// Участок из одного пробела перед text (значение без пробелов по краям) не
// нужен; пробел шаблона в начале — тоже.
void tidy(QVector<Elem>& els)
{
    for (int i = 0; i < els.size(); ++i) {
        const Elem& e = els[i];
        if (e.type == Elem::Type::Space && e.opt >= 0) {
            const bool alone = (i == 0 || els[i - 1].opt != e.opt)
                && (i + 1 >= els.size() || els[i + 1].opt != e.opt);
            if (alone && i + 1 < els.size() && els[i + 1].type == Elem::Type::Field
                && (els[i + 1].kind == Kind::Text || els[i + 1].kind == Kind::Rest)) {
                els.remove(i);
                --i;
            }
        }
    }
    while (!els.isEmpty() && els.first().type == Elem::Type::Space && els.first().opt < 0)
        els.removeFirst();
}

// Шаблон, который компилируется: ошибка в элементе — усечение до него и
// Message. Пусто — ничего, кроме Message, не осталось.
QString compileable(QVector<Elem> els, QString* finalText, QVector<Elem>* finalElems)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        QVector<int> starts;
        const QString text = render(els, &starts);
        LineTemplate tpl(text);
        if (tpl.isValid()) {
            int fields = 0;
            for (const Elem& e : els)
                fields += (e.type == Elem::Type::Field && e.kind != Kind::Rest) ? 1 : 0;
            if (fields == 0)
                return QString();
            *finalText = text;
            *finalElems = els;
            return text;
        }
        // Элемент с ошибкой.
        int bad = int(els.size()) - 1;
        for (int i = 0; i < starts.size(); ++i) {
            if (starts[i] <= tpl.error().position)
                bad = i;
        }
        if (els[bad].opt >= 0) {
            while (bad > 0 && els[bad - 1].opt == els[bad].opt)
                --bad;
        }
        if (bad <= 0)
            return QString();
        const bool wasMessage = els[bad].type == Elem::Type::Field && els[bad].kind == Kind::Rest;
        els.resize(bad);
        if (wasMessage) {
            // Message не встал за полем: убрать поле перед ним.
            while (!els.isEmpty() && els.last().type == Elem::Type::Space)
                els.removeLast();
            if (!els.isEmpty())
                els.removeLast();
        }
        while (!els.isEmpty() && els.last().opt >= 0)
            els.removeLast();
        if (els.isEmpty())
            return QString();
        Elem m;
        m.type = Elem::Type::Field;
        m.kind = Kind::Rest;
        m.name = QStringLiteral("Message");
        els.append(m);
    }
    return QString();
}

QString noBoundaryText(const QString& field)
{
    return QStringLiteral("After %1 comes text without a stable boundary; it can be split only by "
                          "hand — with a list of values, a width or a number of words.").arg(field);
}

struct Core {
    QVector<Variant> variants;
    Failure failure = Failure::None;
};

Core inferCore(const SchemaFit::Corpus& corpus, const QVector<int>& candidateRows,
               const Options& options, qint64& steps)
{
    Core core;
    // Вывод смотрит только голову строки: дальше заголовка — сообщение, а
    // цена каждого шага от длины строки не зависит.
    QVector<Line> lines;
    lines.reserve(candidateRows.size());
    for (int r : candidateRows) {
        Line l;
        const QString& text = corpus.samples[r].text;
        l.s = QStringView(text).first(std::min<qsizetype>(text.size(), kLineChars));
        lines.append(l);
    }
    Builder b(lines, steps, options.cancel);
    QVector<Elem> els = b.run();
    const auto cancelled = [&]() { return options.cancel && options.cancel->load(); };
    if (cancelled()) {
        core.failure = Failure::Cancelled;
        return core;
    }
    nameFields(els);
    const QString afterName = (b.messageAfter >= 0 && b.messageAfter < els.size())
        ? els[b.messageAfter].name : QString();
    tidy(els);

    struct Draft {
        Variant::Kind kind;
        QVector<Elem> els;
        int hard = 0;
    };
    QVector<Draft> drafts;
    int hard = 0;
    int firstHard = -1;
    for (int i = 0; i < els.size(); ++i) {
        if (els[i].type == Elem::Type::Field && els[i].hard) {
            ++hard;
            if (firstHard < 0)
                firstHard = i;
        }
    }
    drafts.append({ Variant::Kind::Full, els, hard });
    if (firstHard > 0) {
        QVector<Elem> c = els.mid(0, firstHard);
        while (!c.isEmpty() && c.last().opt >= 0)
            c.removeLast();
        Elem m;
        m.type = Elem::Type::Field;
        m.kind = Kind::Rest;
        m.name = QStringLiteral("Message");
        c.append(m);
        drafts.append({ Variant::Kind::Cautious, c, 0 });
    }

    SchemaFit::Budget budget = options.budget;
    if (!budget.cancel)
        budget.cancel = options.cancel;
    bool timedOut = false;
    QSet<QString> seen;
    for (const Draft& d : drafts) {
        QString text;
        QVector<Elem> final;
        if (compileable(d.els, &text, &final).isEmpty() || seen.contains(text))
            continue;
        seen.insert(text);
        Variant v;
        v.kind = d.kind;
        v.templateText = text;
        v.hardFields = d.hard;
        const TemplateExtractor ex(text);
        v.fit = SchemaFit::evaluate(ex, corpus, budget);
        steps += corpus.samples.size();
        if (cancelled()) {
            core.variants.clear();
            core.failure = Failure::Cancelled;
            return core;
        }
        if (v.fit.timedOut) {
            timedOut = true;
            continue;
        }
        if (!v.fit.valid || v.fit.coverage() < kMinCoverage)
            continue;
        // Заметка — если Message в итоговом шаблоне и правда идёт за этим
        // полем через пробел.
        bool afterField = false;
        if (!afterName.isEmpty() && !final.isEmpty() && final.last().kind == Kind::Rest) {
            int j = int(final.size()) - 2;
            const bool space = j >= 0 && final[j].type == Elem::Type::Space;
            while (j >= 0 && final[j].type == Elem::Type::Space)
                --j;
            afterField = space && j >= 0 && final[j].type == Elem::Type::Field && final[j].name == afterName;
        }
        if (afterField && d.kind == Variant::Kind::Full) {
            Note note;
            note.kind = Note::Kind::NoBoundary;
            note.after = afterName;
            note.text = noBoundaryText(afterName);
            v.notes.append(note);
        }
        core.variants.append(v);
    }
    std::stable_sort(core.variants.begin(), core.variants.end(),
                     [](const Variant& a, const Variant& b) { return SchemaFit::better(a.fit, b.fit); });
    if (core.variants.isEmpty())
        core.failure = timedOut ? Failure::TimedOut : Failure::NoStructure;
    return core;
}

} // namespace

QString failureText(Failure failure)
{
    switch (failure) {
    case Failure::None: return QString();
    case Failure::NoLines: return QStringLiteral("There are no record lines to learn from.");
    case Failure::KeyedLines:
        return QStringLiteral("The lines are JSON or logfmt — their fields are keys, no template is needed.");
    case Failure::NoStructure:
        return QStringLiteral("No stable structure was found: the lines differ from the first word on.");
    case Failure::TimedOut:
        return QStringLiteral("Checking the candidates took too long; try again on fewer lines.");
    case Failure::Cancelled: return QStringLiteral("Cancelled.");
    }
    return QString();
}

Result infer(const QVector<SchemaFit::Sample>& input, const Options& options)
{
    Result result;
    // Не больше kMaxLines строк — равномерно.
    QVector<SchemaFit::Sample> samples;
    if (input.size() > kMaxLines) {
        samples.reserve(kMaxLines);
        for (int i = 0; i < kMaxLines; ++i)
            samples.append(input[int(qint64(i) * input.size() / kMaxLines)]);
    } else {
        samples = input;
    }
    const SchemaFit::Corpus corpus = SchemaFit::corpus(samples);
    QVector<int> rows;
    int keyed = 0;
    for (int i = 0; i < corpus.samples.size(); ++i) {
        if (!corpus.candidate[i])
            continue;
        rows.append(i);
        if (PatternHeuristics::detectLineFormat(corpus.samples[i].text) != PatternHeuristics::LineShape::Blocks)
            ++keyed;
    }
    result.candidates = int(rows.size());
    if (rows.isEmpty()) {
        result.failure = Failure::NoLines;
        result.failureText = failureText(result.failure);
        return result;
    }
    if (keyed * 2 > rows.size()) {
        result.failure = Failure::KeyedLines;
        result.failureText = failureText(result.failure);
        return result;
    }

    Core core = inferCore(corpus, rows, options, result.steps);
    result.failure = core.failure;
    result.variants = core.variants;

    // Два вида строк: лучший вариант не разобрал ≥ 15 % кандидатов, а они
    // сами дают шаблон с покрытием ≥ 0,90.
    if (!result.variants.isEmpty() && result.variants.first().fit.coverage() < 0.90) {
        Variant& best = result.variants.first();
        const TemplateExtractor ex(best.templateText);
        QVector<SchemaFit::Sample> missed;
        LogEntryFields fields;
        for (int r : rows) {
            ++result.steps;
            if (ex.extract(corpus.samples[r].text, &fields) != FieldExtractor::Outcome::Full)
                missed.append(corpus.samples[r]);
        }
        if (missed.size() >= 20 && missed.size() >= rows.size() * kOptionalMin) {
            const SchemaFit::Corpus other = SchemaFit::corpus(missed);
            QVector<int> otherRows;
            for (int i = 0; i < other.samples.size(); ++i) {
                if (other.candidate[i])
                    otherRows.append(i);
            }
            Core second = inferCore(other, otherRows, options, result.steps);
            if (!second.variants.isEmpty() && second.variants.first().fit.coverage() >= 0.90) {
                Note note;
                note.kind = Note::Kind::TwoKinds;
                note.otherTemplate = second.variants.first().templateText;
                note.otherShare = double(missed.size()) / rows.size();
                note.text = QStringLiteral("The file has two kinds of lines; the other kind (%1% of "
                                           "lines) fits %2")
                                .arg(SchemaFit::percent(note.otherShare), note.otherTemplate);
                best.notes.append(note);
            }
        }
    }
    if (options.cancel && options.cancel->load()) {
        result.variants.clear();
        result.failure = Failure::Cancelled;
    }
    if (result.variants.size() > kMaxVariants)
        result.variants.resize(kMaxVariants);
    if (result.variants.isEmpty() && result.failure == Failure::None)
        result.failure = Failure::NoStructure;
    result.failureText = failureText(result.failure);
    return result;
}

Result infer(const QStringList& lines, const Options& options)
{
    QVector<SchemaFit::Sample> samples;
    samples.reserve(lines.size());
    for (const QString& l : lines)
        samples.append({ 0, l });
    return infer(samples, options);
}

} // namespace SchemaInference
