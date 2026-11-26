#include "userregex.h"

namespace UserRegex {

// Юникод-классы включены всегда, а не только когда в шаблоне есть \w, \d, \s,
// \b или POSIX-класс. Замер (F-14, 2 млн строк генератора): регекс без
// классов («ERROR.*timeout», «lag=\d+ms») и с \w, \b — разница в пределах
// шума и на быстром поиске, и на фильтре обоих бэкендов; заметно дороже
// только \d+ на строках, где цифр много (фильтр +10%, чистый движок +27%) —
// это и есть цена Юникод-\d, условное включение её бы не убрало. Разбор
// шаблона на «есть ли класс» (\p, (?u), классы внутри [...]) — лишний
// источник расхождений между местами, которые обязаны совпадать.
QRegularExpression::PatternOptions options(Qt::CaseSensitivity cs)
{
    QRegularExpression::PatternOptions opts = QRegularExpression::UseUnicodePropertiesOption;
    if (cs == Qt::CaseInsensitive)
        opts |= QRegularExpression::CaseInsensitiveOption;
    return opts;
}

QRegularExpression compile(const QString& pattern, Qt::CaseSensitivity cs)
{
    QRegularExpression re(pattern, options(cs));
    re.optimize();
    return re;
}

} // namespace UserRegex
