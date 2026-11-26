#ifndef TESTS_COMMON_V3SCHEMA_H
#define TESTS_COMMON_V3SCHEMA_H

// Схема прежнего формата (версия 3, «dynamic-block-schema») — ДАННЫЕ для
// тестов перевода (F-17, шаг 9): каталог, сохранённые состояния, ini. Блочной
// модели в программе нет; файл такого вида читает только SchemaConverter.
// Ключи и порядок — как писал прежний LogPattern::serializeDefinition (JSON
// компактный, ключи по алфавиту — QJsonObject).

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace v3schema {

struct Block {
    QString kind;          ///< constant, text, greedy-text, timestamp, level, hex, integer, ip, path, regex, remainder
    QString name;
    QString lead;
    QString closing;
    QString separator;
    QString customRegex;
    bool ignored = false;
    bool separatorIsRegex = false;
};

inline QJsonObject object(const QVector<Block>& blocks, const QString& linePrefix = QString())
{
    QJsonObject root;
    root.insert(QStringLiteral("version"), 3);
    root.insert(QStringLiteral("type"), QStringLiteral("dynamic-block-schema"));
    root.insert(QStringLiteral("linePrefix"), linePrefix);
    QJsonArray array;
    for (const Block& b : blocks) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), b.name);
        o.insert(QStringLiteral("kind"), b.kind);
        o.insert(QStringLiteral("leadingText"), b.lead);
        o.insert(QStringLiteral("closingText"), b.closing);
        o.insert(QStringLiteral("separator"), b.separator);
        if (b.separatorIsRegex)
            o.insert(QStringLiteral("separatorIsRegex"), true);
        o.insert(QStringLiteral("customRegex"), b.customRegex);
        if (b.ignored)
            o.insert(QStringLiteral("ignored"), true);
        array.append(o);
    }
    root.insert(QStringLiteral("blocks"), array);
    return root;
}

/// Определение (как SchemaEntry::schema и FieldSettings::schema прежних версий).
inline QString definition(const QVector<Block>& blocks, const QString& linePrefix = QString())
{
    return QString::fromUtf8(QJsonDocument(object(blocks, linePrefix)).toJson(QJsonDocument::Compact));
}

/// Схема фикстур testdocs (Timestamp [Thread] Level - Message) прежнего вида.
inline QString fixtureDefinition(const QString& level = QStringLiteral("Level"),
                                 const QString& message = QStringLiteral("Message"))
{
    return definition({
        {QStringLiteral("timestamp"), QStringLiteral("Timestamp")},
        {QStringLiteral("text"), QStringLiteral("Thread"), QStringLiteral("["), QStringLiteral("]")},
        {QStringLiteral("level"), level, QString(), QString(), QStringLiteral("-")},
        {QStringLiteral("remainder"), message},
    });
}

} // namespace v3schema

#endif // TESTS_COMMON_V3SCHEMA_H
