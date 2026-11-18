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
// Вторая пара режимов так же проверяет tests/sanitizers/tsan_qt_refcount.h
// (счётчик ссылок контейнеров Qt как синхронизация):
//   cow      — запись на месте после того, как воркер отпустил свою копию
//              списка; отчётов быть НЕ должно;
//   cow-race — воркер читает буфер, не держа копии; отчёт ОБЯЗАН появиться.
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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>

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

// Копия контейнера Qt у воркера и запись в оригинал после того, как воркер её
// отпустил, — так живут снапшоты резидентного хранилища (LogScanSnapshot):
// воркер читает неявно разделяемую копию списка, GUI-поток потом дописывает в
// свой. Пока копия жива, запись отсоединяется в новый буфер; когда отпущена —
// идёт на месте, в буфер, который воркер только что читал. Порядок «чтение →
// запись» держит один счётчик ссылок, без единого вызова в Qt, поэтому здесь
// нет ни сигнала, ни QFuture: о готовности воркер сообщает relaxed-флагом,
// который TSan синхронизацией не считает. Отчётов быть не должно благодаря
// tests/sanitizers/tsan_qt_refcount.h. Потоки — std::thread: их запуск и join
// TSan видит сам, слой tsan_qt_sync.cpp в проверке не участвует.
static int runCow()
{
    int failures = 0;
    for (int round = 0; round < 200; ++round) {
        QList<QString> list;
        list.reserve(256); // запись на месте не должна перевыделять буфер
        for (int i = 0; i < 64; ++i)
            list.append(QString::number(i + round));

        std::atomic_bool released{false};
        qsizetype seen = 0;
        std::thread reader([snapshot = list, &released, &seen]() mutable {
            for (const QString& s : std::as_const(snapshot))
                seen += s.size();
            snapshot = QList<QString>(); // отпустить копию: счётчик 2 → 1
            released.store(true, std::memory_order_relaxed);
        });
        while (!released.load(std::memory_order_relaxed))
            std::this_thread::yield();

        const QString* before = list.constData();
        list.append(QStringLiteral("tail")); // на месте
        list[0] = QStringLiteral("head");    // и поверх прочитанного
        if (list.constData() != before)
            ++failures; // отсоединился — сценарий не тот, что проверяем
        reader.join();
        if (seen <= 0)
            ++failures;
    }
    std::fprintf(stderr, "tsan_canary cow: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

// То же, но воркер читает буфер, НЕ держа копии (по голому указателю), а
// GUI-поток в это время пишет: счётчик ссылок тут ничего не упорядочивает, и
// отчёт обязан появиться — проверка, что tsan_qt_refcount.h не ослепил TSan.
__attribute__((noinline)) static void racyListWrite(QList<int>& list, int value)
{
    list[0] = value;
}

__attribute__((noinline)) static int racyListRead(const int* raw)
{
    return raw[0];
}

static int runCowRace()
{
    QList<int> list(64, 1);
    QList<int> keeper = list; // общий буфер, счётчик 2
    std::atomic_bool stop{false};
    long long sum = 0;
    std::thread reader([raw = keeper.constData(), &stop, &sum] {
        while (!stop.load(std::memory_order_relaxed))
            sum += racyListRead(raw);
    });
    keeper = QList<int>(); // счётчик 2 → 1: дальше запись идёт на месте
    for (int i = 0; i < 2000; ++i) {
        racyListWrite(list, i);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    std::fprintf(stderr, "tsan_canary cow-race: sum %lld\n", sum);
    return 0;
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
    if (argc == 2 && std::strcmp(argv[1], "cow") == 0)
        return runCow();
    if (argc == 2 && std::strcmp(argv[1], "cow-race") == 0)
        return runCowRace();
    std::fprintf(stderr, "usage: tsan_canary clean|race|cow|cow-race\n");
    return 2;
}

#include "main.moc"
