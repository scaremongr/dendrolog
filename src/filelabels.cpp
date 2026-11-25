#include "filelabels.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <limits>

namespace {

bool isBoundary(QChar c)
{
    return c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.')
        || c == QLatin1Char(' ');
}

// Имя → куски: каждый кусок начинается с границы (кроме первого) —
// «payment-service.log» → «payment», «-service», «.log». Склейка кусков
// даёт имя обратно.
QStringList pieces(const QString& name)
{
    QStringList out;
    int start = 0;
    for (int i = 1; i < name.size(); ++i) {
        if (isBoundary(name[i]) && !isBoundary(name[i - 1])) {
            out << name.mid(start, i - start);
            start = i;
        }
    }
    out << name.mid(start);
    return out;
}

// Длиннее предела — выпускается середина.
QString elideMiddle(const QString& s, int maxChars)
{
    if (s.size() <= maxChars)
        return s;
    if (maxChars <= 1)
        return QString(FileLabels::kEllipsis);
    const int keep = maxChars - 1;
    const int head = (keep + 1) / 2;
    const int tail = keep - head;
    return s.left(head) + FileLabels::kEllipsis + s.right(tail);
}

} // namespace

namespace FileLabels {

QStringList build(const QStringList& names, int maxChars)
{
    maxChars = std::max(maxChars, 3);
    QStringList labels;
    const bool allFit = std::all_of(names.begin(), names.end(),
                                    [maxChars](const QString& n) { return n.size() <= maxChars; });
    if (allFit || names.size() < 2) {
        for (const QString& n : names)
            labels << elideMiddle(n, maxChars);
    } else {
        QVector<QStringList> split;
        split.reserve(names.size());
        int minPieces = std::numeric_limits<int>::max();
        for (const QString& n : names) {
            split << pieces(n);
            minPieces = std::min(minPieces, int(split.last().size()));
        }
        // Общий префикс и суффикс — целыми кусками; у каждого имени остаётся
        // хотя бы один кусок.
        const auto samePieceAt = [&](auto pieceOf) {
            const QString first = pieceOf(split.first());
            for (const QStringList& p : split)
                if (pieceOf(p) != first)
                    return false;
            return true;
        };
        int prefix = 0;
        while (prefix < minPieces - 1
               && samePieceAt([prefix](const QStringList& p) { return p[prefix]; }))
            ++prefix;
        int suffix = 0;
        while (prefix + suffix < minPieces - 1
               && samePieceAt([suffix](const QStringList& p) { return p[p.size() - 1 - suffix]; }))
            ++suffix;
        for (int k = 0; k < split.size(); ++k) {
            const QStringList& p = split[k];
            // Имя, которое помещается целиком, не сокращается.
            if (names[k].size() <= maxChars) {
                labels << names[k];
                continue;
            }
            QString middle;
            for (int i = prefix; i < p.size() - suffix; ++i)
                middle += p[i];
            // Граница, с которой начинается кусок, после «…» не нужна.
            if (prefix > 0 && !middle.isEmpty() && isBoundary(middle.front()))
                middle.remove(0, 1);
            const int ellipses = (prefix > 0 ? 1 : 0) + (suffix > 0 ? 1 : 0);
            middle = elideMiddle(middle, std::max(1, maxChars - ellipses));
            labels << (prefix > 0 ? QString(kEllipsis) : QString()) + middle
                          + (suffix > 0 ? QString(kEllipsis) : QString());
        }
    }

    // Попарно различны: повторам — «~2», «~3», … (и не совпасть с уже
    // занятой меткой).
    QSet<QString> taken;
    QHash<QString, int> seen;
    for (QString& label : labels) {
        if (!taken.contains(label)) {
            taken.insert(label);
            seen[label] = 1;
            continue;
        }
        const QString base = label;
        int n = seen.value(base, 1);
        QString candidate;
        do {
            ++n;
            const QString tag = QStringLiteral("~%1").arg(n);
            candidate = elideMiddle(base, std::max(1, maxChars - int(tag.size()))) + tag;
        } while (taken.contains(candidate));
        seen[base] = n;
        taken.insert(candidate);
        label = candidate;
    }
    return labels;
}

QStringList wrap(const QString& name, int charsPerLine, int maxLines)
{
    if (charsPerLine < 1 || maxLines < 1)
        return {};
    // Сначала — с разрывами после границ («payment-» / «service.log»);
    // линий вышло больше, чем есть, — по ширине, без оглядки на границы.
    for (const bool atBoundaries : {true, false}) {
        QStringList lines;
        int pos = 0;
        while (pos < name.size() && lines.size() <= maxLines) {
            int len = std::min<int>(charsPerLine, name.size() - pos);
            if (atBoundaries && pos + len < name.size()) {
                for (int cut = len; cut > charsPerLine / 2; --cut) {
                    if (isBoundary(name[pos + cut - 1])) {
                        len = cut;
                        break;
                    }
                }
            }
            lines << name.mid(pos, len);
            pos += len;
        }
        if (lines.size() <= maxLines)
            return lines;
    }
    return {};
}

QString modeName(FileLabelMode mode)
{
    switch (mode) {
    case FileLabelMode::ColorOnly: return QStringLiteral("colorOnly");
    case FileLabelMode::Hidden:    return QStringLiteral("hidden");
    case FileLabelMode::Name:      break;
    }
    return QStringLiteral("name");
}

FileLabelMode modeFromName(const QString& name)
{
    if (name == QLatin1String("colorOnly"))
        return FileLabelMode::ColorOnly;
    if (name == QLatin1String("hidden"))
        return FileLabelMode::Hidden;
    return FileLabelMode::Name;
}

} // namespace FileLabels
