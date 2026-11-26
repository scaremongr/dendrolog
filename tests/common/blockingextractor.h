#pragma once

// Подставной медленный извлекатель полей (F-17, шаг 6): Cost::Background,
// разбор — шаблоном строки (TemplateExtractor), но каждый вызов extract
// проходит через «шлагбаум» Gate, который тест закрывает и открывает. Так
// архитектурное требование «разбор полей не мешает показу и прокрутке, даже
// если извлекатель медленный — вплоть до сети на каждую строку» проверяется
// тестом, а не обещанием: пока шлагбаум закрыт, фоновое задание стоит, а
// GUI-поток обязан работать дальше и не вызвать extract ни разу.

#include "fieldextractor.h"
#include "fieldlayout.h"
#include "linetemplate.h"

#include <QCoreApplication>
#include <QThread>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace testdocs {

// std::mutex / std::condition_variable, а не QMutex / QWaitCondition: Qt не
// инструментирован, и TSan не видит, что QWaitCondition::wait отпускает
// мьютекс («double lock», гонка на open).
struct Gate {
    std::mutex mutex;
    std::condition_variable cond;
    bool open = true;
    std::atomic<qint64> calls{0};       // вызовов extract всего
    std::atomic<qint64> guiCalls{0};    // из них на GUI-потоке (обязано быть 0)
    std::atomic<int> waiting{0};        // стоят у закрытого шлагбаума сейчас

    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        cond.notify_all();
    }
    void pass()
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++waiting;
        cond.wait(lock, [this] { return open; });
        --waiting;
    }
};

class BlockingExtractor : public FieldExtractor {
public:
    BlockingExtractor(const QString& templateText, std::shared_ptr<Gate> gate)
        : m_inner(templateText), m_gate(std::move(gate)) {}

    bool isValid() const override { return m_inner.isValid(); }
    QStringList fields() const override { return m_inner.fields(); }
    Cost cost() const override { return Cost::Background; }
    int tailField() const override { return m_inner.tailField(); }
    Outcome extract(QStringView line, LogEntryFields* out, int upTo = -1,
                    Stop* stop = nullptr) const override
    {
        m_gate->calls.fetch_add(1);
        if (QThread::currentThread() == QCoreApplication::instance()->thread())
            m_gate->guiCalls.fetch_add(1);
        m_gate->pass();
        return m_inner.extract(line, out, upTo, stop);
    }
    MatchReport explain(QStringView line) const override { return m_inner.explain(line); }

private:
    TemplateExtractor m_inner;
    std::shared_ptr<Gate> m_gate;
};

// Шаблон строк фикстур makeLogBytes: «2026-03-05 10:00:00.000 [worker-1]
// INFO - текст» — те же колонки, что у buildSchema (Timestamp, Thread,
// Level, Message).
inline QString fixtureTemplate()
{
    return QStringLiteral("{Timestamp:time} [{Thread}] {Level:level} - {Message}");
}

// Схема файла с подставным извлекателем в раскладке layout (колонки — как у
// того же шаблона без шлагбаума).
inline FileSchemaPtr blockingSchema(FieldLayout& layout, const std::shared_ptr<Gate>& gate,
                                    const QString& templateText = fixtureTemplate())
{
    const FileSchemaPtr base = layout.schemaFor(LineTemplateSchema::definition(templateText));
    auto schema = std::make_shared<FileSchema>(*base);
    schema->extractor = std::make_shared<BlockingExtractor>(templateText, gate);
    return schema;
}

} // namespace testdocs
