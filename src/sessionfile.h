#ifndef SESSIONFILE_H
#define SESSIONFILE_H

#include "documentstate.h"

#include <QString>
#include <QVector>

#include <optional>

// ============================================================================
// SessionFile — файл сессии на диске (R-5, шаг 1; docs/design/R-5.md, «Три
// хранилища», «Изменённый или пропавший источник»). Без виджетов.
//
//   • save — атомарно (QSaveFile): прерванная запись не портит прежний файл;
//   • load — отказ на битом JSON, чужом формате, битых полях и вложенных
//     вкладках; файл большей версии не читается (newerVersion), сам файл не
//     трогается;
//   • rebind — перепривязка источников к месту, где их видно сейчас. Сессию
//     переносят вместе с логами в другую папку или на другую машину, поэтому
//     каждый путь проверяется в двух местах: сохранённый абсолютный и
//     относительный к каталогу файла сессии. Правило (по одному разу на путь):
//       1. по абсолютному пути источник цел (Valid) — он;
//       2. иначе по относительному цел — он (Relocated);
//       3. иначе файл есть хоть где-то — откроется чистым, закладки серые
//          (Changed): по абсолютному, если он там есть, иначе по
//          относительному;
//       4. нигде нет — Missing: вкладка его не откроет.
//     Найденный путь подменяется во всех местах вкладок (файлы, источники
//     закладок и текущей строки) — как у вторичного поиска памяти файла.
//
// Ничего не спрашивается: итог перепривязки — материал для отчёта о
// восстановлении (SessionController).
// ============================================================================
namespace SessionFile {

bool save(const QString& filePath, const SessionState& state, QString* error = nullptr);

struct LoadResult {
    std::optional<SessionState> state;
    QString error;              // непусто, если state пуст
    bool newerVersion = false;  // файл новой версии: не читается, не трогается
};
LoadResult load(const QString& filePath);

struct Resolution {
    enum class Kind { Valid, Relocated, Changed, Missing };
    QString savedPath;  // как в файле сессии
    QString path;       // где он теперь (Missing — сохранённый)
    Kind kind = Kind::Missing;
};
// Подменить пути в state по правилу выше; по записи на каждый путь
// SessionState::paths(), в том же порядке.
QVector<Resolution> rebind(SessionState& state, const QString& sessionFilePath);

// Путь с расширением .dlsession (диалог сохранения мог его не добавить).
QString withSuffix(const QString& filePath);
bool isSessionFile(const QString& filePath);

} // namespace SessionFile

#endif // SESSIONFILE_H
