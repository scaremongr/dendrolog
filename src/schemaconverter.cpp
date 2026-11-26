#include "schemaconverter.h"

#include "linetemplate.h"
#include "logpattern.h"

#include <QVector>

#include <algorithm>
#include <optional>

namespace {

// Кусок будущего шаблона. Gap — «{? ?}» (пробелы могут быть, а могут и не
// быть), Space — « » (один или несколько), Opt — «{?…?}» из Lit и Space.
struct Piece {
    enum class Type { Lit, Gap, Space, Opt, Field };
    Type type = Type::Lit;
    QString text;               ///< Lit — литерал; Field — «{…}» целиком
    QVector<Piece> content;     ///< Opt
    int block = -1;             ///< Field — номер блока
    QString name;               ///< Field — имя («_» — без колонки)
    QString kind;               ///< Field — вид (пусто — вид по умолчанию)
};

Piece lit(const QString& t)
{
    Piece p;
    p.type = Piece::Type::Lit;
    p.text = t;
    return p;
}

Piece gap()
{
    Piece p;
    p.type = Piece::Type::Gap;
    return p;
}

Piece space()
{
    Piece p;
    p.type = Piece::Type::Space;
    return p;
}

Piece opt(QVector<Piece> content)
{
    Piece p;
    p.type = Piece::Type::Opt;
    p.content = std::move(content);
    return p;
}

bool isWhitespaceOnly(const QString& text)
{
    if (text.isEmpty())
        return false;
    for (const QChar c : text) {
        if (!c.isSpace())
            return false;
    }
    return true;
}

// Регекс в виде поля шаблона: «/» внутри экранируется.
QString regexSpec(const QString& rx)
{
    QString out = QStringLiteral("/");
    for (int i = 0; i < rx.size(); ++i) {
        const QChar c = rx.at(i);
        if (c == QLatin1Char('\\') && i + 1 < rx.size()) {
            out += c;
            out += rx.at(++i);
            continue;
        }
        if (c == QLatin1Char('/'))
            out += QLatin1Char('\\');
        out += c;
    }
    out += QLatin1Char('/');
    return out;
}

// Простой регекс-разделитель → куски: литеральные знаки (и «\»-экранированные
// не буквы), пробельные атомы ([ \t], [\t ], \s, пробел, табуляция) с
// квантификаторами ?, *, + у пробелов и ? у одного знака. Иначе — nullopt.
std::optional<QVector<Piece>> translateRegex(const QString& rx)
{
    QVector<Piece> out;
    const auto appendLiteral = [&out](QChar c) {
        if (!out.isEmpty() && out.constLast().type == Piece::Type::Lit)
            out.last().text += c;
        else
            out.append(lit(QString(c)));
    };
    int i = 0;
    const int n = int(rx.size());
    while (i < n) {
        bool ws = false;
        QChar ch;
        const QChar c = rx.at(i);
        if (c == QLatin1Char('\\')) {
            if (i + 1 >= n)
                return std::nullopt;
            const QChar e = rx.at(i + 1);
            if (e == QLatin1Char('s') || e == QLatin1Char('t')) {
                ws = true;
            } else if (e.isLetterOrNumber()) {
                return std::nullopt;
            } else {
                ch = e;
            }
            i += 2;
        } else if (c == QLatin1Char('[')) {
            const QString cls = rx.mid(i);
            if (cls.startsWith(QLatin1String("[ \\t]")) || cls.startsWith(QLatin1String("[\\t ]"))) {
                ws = true;
                i += 5;
            } else {
                return std::nullopt;
            }
        } else if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
            ws = true;
            ++i;
        } else if (QStringLiteral("().|^$?*+{}]").contains(c)) {
            return std::nullopt;
        } else {
            ch = c;
            ++i;
        }
        QChar quant;
        if (i < n && (rx.at(i) == QLatin1Char('?') || rx.at(i) == QLatin1Char('*')
                      || rx.at(i) == QLatin1Char('+'))) {
            quant = rx.at(i);
            ++i;
            if (i < n && (rx.at(i) == QLatin1Char('?') || rx.at(i) == QLatin1Char('+')))
                return std::nullopt;   // ленивые и захватнические квантификаторы
        }
        if (ws) {
            out.append(quant == QLatin1Char('?') || quant == QLatin1Char('*') ? gap() : space());
        } else if (quant.isNull()) {
            appendLiteral(ch);
        } else if (quant == QLatin1Char('?')) {
            if (ch == QLatin1Char('?'))
                return std::nullopt;   // «?» внутри «{?…?}» не выразить
            out.append(opt({ lit(QString(ch)) }));
        } else {
            return std::nullopt;
        }
    }
    return out;
}

bool hasOptional(const QVector<Piece>& pieces)
{
    for (const Piece& p : pieces) {
        if (p.type == Piece::Type::Opt || p.type == Piece::Type::Gap)
            return true;
    }
    return false;
}

// Gap и Space подряд: Space главнее; Gap в начале и в конце не нужен.
QVector<Piece> normalized(const QVector<Piece>& in, bool top)
{
    QVector<Piece> out;
    for (const Piece& p : in) {
        const bool blank = p.type == Piece::Type::Gap || p.type == Piece::Type::Space;
        if (blank && !out.isEmpty()) {
            Piece& last = out.last();
            if (last.type == Piece::Type::Space)
                continue;
            if (last.type == Piece::Type::Gap) {
                if (p.type == Piece::Type::Space)
                    last = p;
                continue;
            }
        }
        if (p.type == Piece::Type::Lit && !out.isEmpty() && out.constLast().type == Piece::Type::Lit) {
            out.last().text += p.text;
            continue;
        }
        out.append(p);
    }
    if (top) {
        while (!out.isEmpty() && out.constFirst().type == Piece::Type::Gap)
            out.removeFirst();
        while (!out.isEmpty() && (out.constLast().type == Piece::Type::Gap
                                  || out.constLast().type == Piece::Type::Space))
            out.removeLast();
    }
    return out;
}

QString render(const QVector<Piece>& pieces, QVector<std::pair<int, int>>* fieldPositions)
{
    QString out;
    for (const Piece& p : pieces) {
        switch (p.type) {
        case Piece::Type::Lit:
            out += LineTemplate::escapeLiteral(p.text);
            break;
        case Piece::Type::Gap:
            out += QStringLiteral("{? ?}");
            break;
        case Piece::Type::Space:
            out += QLatin1Char(' ');
            break;
        case Piece::Type::Opt:
            out += QStringLiteral("{?") + render(p.content, nullptr) + QStringLiteral("?}");
            break;
        case Piece::Type::Field:
            if (fieldPositions)
                fieldPositions->append({ int(out.size()), p.block });
            out += QLatin1Char('{') + p.name;
            if (!p.kind.isEmpty())
                out += QLatin1Char(':') + p.kind;
            out += QLatin1Char('}');
            break;
        }
    }
    return out;
}

QString escapeOfValue(const QString& v)
{
    QString out;
    for (const QChar c : v) {
        if (c == QLatin1Char('|') || c == QLatin1Char(')') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

} // namespace

namespace SchemaConverter {

Result fromBlocks(const PatternDefinition& definition)
{
    Result result;
    QString why;
    if (!LogPattern::validateDefinition(definition, &why)) {
        result.error = why;
        return result;
    }

    using K = PatternBlock::MatchKind;
    QVector<Piece> pieces;
    if (!definition.linePrefix.isEmpty())
        pieces.append(isWhitespaceOnly(definition.linePrefix) ? space() : lit(definition.linePrefix));

    const int count = int(definition.blocks.size());
    for (int i = 0; i < count; ++i) {
        const PatternBlock& b = definition.blocks[i];
        const bool last = i == count - 1;
        if (i > 0 || !definition.linePrefix.isEmpty())
            pieces.append(gap());

        if (!b.leadingText.isEmpty()) {
            if (isWhitespaceOnly(b.leadingText)) {
                pieces.append(space());
            } else {
                pieces.append(lit(b.leadingText.trimmed()));
                pieces.append(gap());
            }
        }

        // Значение.
        const bool field = LogPattern::blockCreatesField(b);
        if (b.matchKind == K::ConstantText && !field) {
            pieces.append(lit(b.customRegex));
        } else {
            Piece f;
            f.type = Piece::Type::Field;
            f.block = i;
            f.name = field ? b.name.trimmed() : QStringLiteral("_");
            if (field && (f.name.contains(QLatin1Char(':')) || f.name.contains(QLatin1Char('{'))
                          || f.name.contains(QLatin1Char('}')) || f.name == QLatin1String("_")
                          || f.name.startsWith(QLatin1Char('?')))) {
                // Имя колонки — контракт с правилами фильтров: молча не меняется.
                result.error = QStringLiteral("Block %1: the field name “%2” cannot be written in a "
                                              "template (“:”, a brace, “_” or a leading “?”).")
                                   .arg(i + 1).arg(f.name);
                return result;
            }
            const bool open = b.separator.isEmpty() && b.closingText.isEmpty();
            switch (b.matchKind) {
            case K::ConstantText:
                f.kind = QStringLiteral("of(") + escapeOfValue(b.customRegex) + QLatin1Char(')');
                break;
            case K::Timestamp: f.kind = QStringLiteral("time"); break;
            case K::Level: f.kind = QStringLiteral("level"); break;
            case K::HexText: f.kind = QStringLiteral("hex"); break;
            case K::Integer: f.kind = QStringLiteral("int"); break;
            case K::IpAddress:
                f.kind = QStringLiteral("ip");
                result.notes.append(QStringLiteral("Field “%1”: an IP address no longer takes a port "
                                                   "after it.").arg(f.name));
                break;
            case K::FilePath: f.kind = QStringLiteral("path"); break;
            case K::CustomRegex: f.kind = regexSpec(b.customRegex.trimmed()); break;
            case K::TextUntilSeparator:
                f.kind = (last && open) ? QStringLiteral("rest") : QStringLiteral("text");
                break;
            case K::GreedyTextUntilSeparator:
                f.kind = (last && open) ? QStringLiteral("rest") : QStringLiteral("greedy");
                break;
            case K::Remainder: f.kind = QStringLiteral("rest"); break;
            }
            pieces.append(f);
        }

        if (!b.closingText.isEmpty()) {
            if (isWhitespaceOnly(b.closingText)) {
                pieces.append(space());
            } else {
                pieces.append(gap());
                pieces.append(lit(b.closingText.trimmed()));
            }
        }

        if (!b.separator.isEmpty()) {
            const bool optional = LogPattern::blockIsSelfDelimiting(b);
            QVector<Piece> sep;
            bool translated = true;
            if (isWhitespaceOnly(b.separator)) {
                // Пробельный разделитель (и регексом « ») — пробел шаблона:
                // [ \t]*(?: ) прежнего генератора требует хотя бы один.
                sep.append(space());
            } else if (b.separatorIsRegex) {
                const std::optional<QVector<Piece>> t = translateRegex(b.separator.trimmed());
                if (t)
                    sep = *t;
                else
                    translated = false;
            } else {
                sep.append(lit(b.separator.trimmed()));
            }
            if (translated && optional && hasOptional(sep))
                translated = false;   // «{?…?}» не вкладываются
            if (!translated) {
                // Тот же регекс полем без имени, как его ставил прежний генератор.
                const QString body = b.separatorIsRegex
                    ? QStringLiteral("[ \\t]*(?:") + b.separator.trimmed() + QLatin1Char(')')
                    : QStringLiteral("[ \\t]*") + QRegularExpression::escape(b.separator.trimmed());
                Piece f;
                f.type = Piece::Type::Field;
                f.name = QStringLiteral("_");
                f.kind = regexSpec(optional ? QStringLiteral("(?:") + body + QStringLiteral(")?") : body);
                pieces.append(f);
            } else {
                pieces.append(gap());
                const QVector<Piece> inner = normalized(sep, false);
                const bool blankOnly = std::all_of(inner.cbegin(), inner.cend(), [](const Piece& p) {
                    return p.type == Piece::Type::Space || p.type == Piece::Type::Gap;
                });
                if (optional && !blankOnly) {
                    // Пробел внутри разделителя (« - ») становится частью участка.
                    pieces.append(opt(inner));
                } else if (optional) {
                    // Необязательный пробельный разделитель — тот же промежуток.
                } else {
                    for (const Piece& p : sep)
                        pieces.append(p);
                }
            }
        }
    }

    pieces = normalized(pieces, true);

    // Text без определённого конца (следом пробел и поле без формы) — word.
    for (int attempt = 0; attempt <= count; ++attempt) {
        QVector<std::pair<int, int>> positions;
        const QString text = render(pieces, &positions);
        const LineTemplate tpl(text);
        if (tpl.isValid()) {
            result.ok = true;
            result.templateText = text;
            return result;
        }
        const LineTemplateError& err = tpl.error();
        int block = -1;
        for (const auto& [pos, b] : positions) {
            if (pos == err.position)
                block = b;
        }
        bool fixed = false;
        if (err.code == LineTemplateError::Code::UndefinedEnd && block >= 0) {
            for (Piece& p : pieces) {
                if (p.type == Piece::Type::Field && p.block == block
                    && (p.kind == QLatin1String("text") || p.kind == QLatin1String("greedy"))) {
                    p.kind = QStringLiteral("word");
                    result.notes.append(QStringLiteral("Field “%1”: its end is not defined by the "
                                                       "next block — it is one word now.").arg(p.name));
                    fixed = true;
                }
            }
        }
        if (!fixed) {
            result.error = err.message;
            result.templateText = text;
            return result;
        }
    }
    result.error = QStringLiteral("The schema could not be converted.");
    return result;
}

} // namespace SchemaConverter
