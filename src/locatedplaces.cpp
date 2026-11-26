#include "locatedplaces.h"
#include "logmodel.h"
#include "recordlocator.h"

LocatedPlaces::LocatedPlaces(LogModel& model)
    : QObject(&model)
    , m_model(model)
{
    RecordLocator& loc = locator();
    connect(&loc, &RecordLocator::resolved, this, &LocatedPlaces::onResolved);
    connect(&loc, &RecordLocator::refused, this,
            [this](int token, RecordLocator::Refusal) { onRefused(token); });
    connect(&loc, &RecordLocator::completed, this, &LocatedPlaces::onCompleted);
}

RecordLocator& LocatedPlaces::locator() const
{
    return m_model.recordLocator();
}

QString LocatedPlaces::previewOf(int row) const
{
    return m_model.messageAt(row).left(kPreviewLength);
}

// ---------------------------------------------------------------------------
// Привязка и токены
// ---------------------------------------------------------------------------

bool LocatedPlaces::fill(LocatedPlace& p, int row) const
{
    const RecordKey key = m_model.keyAt(row);
    if (!key.isValid())
        return false;
    const QString text = m_model.messageAt(row);
    p.key = key;
    p.filePath = key.file->filePath;
    p.line = key.line;
    p.preview = text.left(kPreviewLength);
    p.record = locator().idFor(key, text);
    p.verified = true;
    p.lost = false;
    return true;
}

void LocatedPlaces::track(int id, LocatedPlace& p, int token)
{
    p.token = token;
    if (token)
        m_byToken.insert(token, id);
}

bool LocatedPlaces::bindToRow(int id, LocatedPlace& p, int row)
{
    release(p);
    if (!fill(p, row))
        return false;
    watchCompletion(id, p, row);
    return true;
}

void LocatedPlaces::watchCompletion(int id, LocatedPlace& p, int row)
{
    if (p.token != 0 || !p.verified || p.record.hasText || !p.record.isValid() || row < 0)
        return;
    track(id, p, locator().watchCompletion(p.key, p.record, m_model.messageAt(row)));
}

void LocatedPlaces::release(LocatedPlace& p)
{
    if (p.token == 0)
        return;
    locator().cancel(p.token);
    m_byToken.remove(p.token);
    p.token = 0;
}

void LocatedPlaces::reacquire(int id, LocatedPlace& p)
{
    release(p);
    p.verified = false;
    p.lost = false; // файл могли вернуть к прежнему содержимому
    if (p.key.file)
        track(id, p, locator().reacquire(p.key.file, p.record));
}

void LocatedPlaces::requestSaved(int id, LocatedPlace& p)
{
    release(p);
    p.verified = false;
    p.lost = false;
    track(id, p, locator().request(p.source, p.record));
}

// ---------------------------------------------------------------------------
// Сброс строк
// ---------------------------------------------------------------------------

void LocatedPlaces::invalidatePlacesOfFile(const QString& filePath)
{
    const QString key = RecordLocator::pathKey(filePath);
    forEachPlace([&](int id, LocatedPlace& p) {
        if (p.key.file) {
            if (p.key.file->filePath == filePath)
                reacquire(id, p);
        } else if (p.lost && RecordLocator::pathKey(p.filePath) == key) {
            // Сохранённое место получило отказ (файл был другим) — файл
            // читается заново, проверяем ещё раз. Ждущее ждёт само.
            requestSaved(id, p);
        }
    });
}

void LocatedPlaces::invalidateAllPlaces()
{
    forEachPlace([&](int id, LocatedPlace& p) {
        if (p.key.file)
            reacquire(id, p);
        else if (p.lost)
            requestSaved(id, p);
    });
}

void LocatedPlaces::recheckSavedSource(const QString& sourceKey)
{
    forEachPlace([&](int id, LocatedPlace& p) {
        if (!p.key.file && p.lost && p.source.isValid()
            && RecordLocator::sourceKey(p.source) == sourceKey)
            requestSaved(id, p);
    });
}

bool LocatedPlaces::refreshVisiblePreviews(const QString& filePath)
{
    bool anyChange = false;
    forEachPlace([&](int, LocatedPlace& p) {
        if (p.filePath != filePath || !p.isLive())
            return;
        const int row = m_model.rowForKey(p.key);
        if (row < 0)
            return;
        const QString preview = previewOf(row);
        if (preview != p.preview) {
            p.preview = preview;
            anyChange = true;
        }
    });
    return anyChange;
}

// ---------------------------------------------------------------------------
// Ответы RecordLocator
// ---------------------------------------------------------------------------

void LocatedPlaces::onResolved(int token, const RecordKey& key)
{
    const auto it = m_byToken.find(token);
    if (it == m_byToken.end())
        return;
    const int id = it.value();
    LocatedPlace* p = placeById(id);
    if (!p) {
        m_byToken.erase(it);
        return;
    }
    // Недописанная строка: локатор следит за ней тем же токеном до completed.
    if (p->record.hasText) {
        m_byToken.erase(it);
        p->token = 0;
    }
    const RecordKey previous = p->key;
    p->key = key;
    p->filePath = key.file->filePath;
    p->line = key.line;
    p->verified = true;
    p->lost = false;
    placeResolved(id, previous);
}

void LocatedPlaces::onRefused(int token)
{
    const auto it = m_byToken.find(token);
    if (it == m_byToken.end())
        return;
    const int id = it.value();
    m_byToken.erase(it);
    LocatedPlace* p = placeById(id);
    if (!p)
        return;
    p->token = 0;
    p->verified = true;
    p->lost = true;
    placeRefused(id);
}

void LocatedPlaces::onCompleted(int token, const RecordId& record, const RecordKey& key)
{
    const auto it = m_byToken.find(token);
    if (it == m_byToken.end())
        return;
    const int id = it.value();
    m_byToken.erase(it);
    LocatedPlace* p = placeById(id);
    if (!p)
        return;
    p->token = 0;
    p->record = record;
    // Дописанная строка могла начать запись: ключ другой, строка та же.
    const bool keyChanged = key.isValid() && key != p->key && key.file == p->key.file
        && key.line == p->line;
    if (keyChanged)
        p->key = key;
    placeCompleted(id, keyChanged);
}
