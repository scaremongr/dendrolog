// ============================================================================
// tsan_canary — проверка самой проверки. Собирается только при
// -DDENDROLOG_SANITIZER=thread.
//
// tests/sanitizers/tsan_qt_sync.cpp объясняет ThreadSanitizer, что передача
// данных через очередь событий, пул потоков и QFuture — не гонка. Ошибка в
// таком слое опасна в обе стороны, поэтому у теста два режима:
//   clean — правильные передачи через Qt; отчётов быть НЕ должно. Упал —
//           слой перестал покрывать какой-то путь (сменилась версия Qt?);
//   race  — настоящая гонка, идущая через те же пути; отчёт ОБЯЗАН появиться
//           (CTest ищет его в выводе). Нет отчёта — слой ослепил TSan, и
//           зелёный прогон остальных тестов ничего не значит.
// ============================================================================

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>

class Worker : public QObject {
    Q_OBJECT
signals:
    void batchReady(const QList<int>& batch, int* plain);
};

static bool pumpUntil(const std::atomic_bool& done, int timeoutMs = 30000)
{
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return done;
}

// Правильные передачи: каждая запись отделена от чтения в другом потоке
// механизмом Qt, и одновременно данные никто не трогает.
static int runClean()
{
    int failures = 0;
    for (int round = 0; round < 50; ++round) {
        // 1. GUI → воркер через захват лямбды; воркер → GUI через queued-сигнал
        //    (батч и простая память по указателю).
        Worker worker;
        long long sum = 0;
        int received = 0;
        std::atomic_bool finished{false};
        QObject::connect(&worker, &Worker::batchReady, &worker,
                         [&](const QList<int>& batch, int* plain) {
                             for (int v : batch)
                                 sum += v;
                             sum += *plain;
                             delete plain;
                             ++received;
                         },
                         Qt::QueuedConnection);
        auto input = std::make_shared<QList<int>>();
        for (int i = 0; i < 100; ++i)
            input->append(i + round);
        QFutureWatcher<int> watcher;
        QObject::connect(&watcher, &QFutureWatcher<int>::finished, &watcher,
                         [&] { finished = true; });
        watcher.setFuture(QtConcurrent::run([input, &worker]() {
            int total = 0;
            for (int i = 0; i < 20; ++i) {
                QList<int> batch = *input;
                batch.append(i);
                total += int(batch.size());
                emit worker.batchReady(batch, new int(i));
            }
            return total;
        }));
        if (!pumpUntil(finished) || watcher.result() != 20 * 101)
            ++failures;
        QCoreApplication::processEvents();
        if (received != 20)
            ++failures;

        // 2. QtConcurrent::map по общему буферу и чтение результата после
        //    finished — как FieldReextraction.
        auto jobs = std::make_shared<QList<QList<int>>>();
        for (int i = 0; i < 400; ++i)
            jobs->append(QList<int>{i});
        std::atomic_bool mapped{false};
        QFutureWatcher<void> mapWatcher;
        QObject::connect(&mapWatcher, &QFutureWatcher<void>::finished, &mapWatcher,
                         [&] { mapped = true; });
        mapWatcher.setFuture(QtConcurrent::map(*jobs, [](QList<int>& job) {
            job.append(job.first() * 2);
        }));
        if (!pumpUntil(mapped))
            ++failures;
        for (const QList<int>& job : *jobs)
            if (job.size() != 2)
                ++failures;

        // 3. Блокирующее ожидание вместо сигнала.
        auto slot = std::make_shared<int>(0);
        QFuture<void> f = QtConcurrent::run([slot] { *slot = 42; });
        f.waitForFinished();
        if (*slot != 42)
            ++failures;
    }
    QThreadPool::globalInstance()->waitForDone();
    std::fprintf(stderr, "tsan_canary clean: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

// Несинхронизированные записи вынесены в функции с приметными именами: по ним
// CTest отличает нужный отчёт от любого другого.
__attribute__((noinline)) static void racyGuiWrite(int& counter, int value)
{
    counter += value;
}

__attribute__((noinline)) static void racyWorkerWrite(int& counter)
{
    ++counter;
}

// Настоящая гонка: воркер и GUI-поток пишут в обычный int без синхронизации,
// при этом между ними идёт та же переписка сигналами, что и в runClean().
static int runRace()
{
    Worker worker;
    int racy = 0;
    std::atomic_bool finished{false};
    QObject::connect(&worker, &Worker::batchReady, &worker,
                     [&](const QList<int>&, int* plain) {
                         racyGuiWrite(racy, *plain);
                         delete plain;
                     },
                     Qt::QueuedConnection);
    QFutureWatcher<void> watcher;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &watcher,
                     [&] { finished = true; });
    watcher.setFuture(QtConcurrent::run([&worker, &racy]() {
        for (int i = 0; i < 200; ++i) {
            emit worker.batchReady(QList<int>{i}, new int(i));
            racyWorkerWrite(racy); // после emit, пока GUI-поток пишет туда же
            QThread::usleep(200);
        }
    }));
    pumpUntil(finished);
    QThreadPool::globalInstance()->waitForDone();
    std::fprintf(stderr, "tsan_canary race: counter %d\n", racy);
    return 0;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && std::strcmp(argv[1], "clean") == 0)
        return runClean();
    if (argc == 2 && std::strcmp(argv[1], "race") == 0)
        return runRace();
    std::fprintf(stderr, "usage: tsan_canary clean|race\n");
    return 2;
}

#include "main.moc"
