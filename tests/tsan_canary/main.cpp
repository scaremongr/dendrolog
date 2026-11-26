// ============================================================================
// tsan_canary — проверка самой проверки. Собирается только при
// -DDENDROLOG_SANITIZER=thread.
//
// tests/sanitizers/tsan_qt_sync.cpp объясняет ThreadSanitizer, что передача
// данных через очередь событий, пул потоков и QFuture — не гонка. Ошибка в
// таком слое опасна в обе стороны, поэтому режимы идут парами:
//   clean — правильные передачи через Qt; отчётов быть НЕ должно. Упал —
//           слой перестал покрывать какой-то путь (сменилась версия Qt?);
//   race  — настоящая гонка, идущая через те же пути; отчёт ОБЯЗАН появиться.
//           Нет отчёта — слой ослепил TSan, и зелёный прогон остальных тестов
//           ничего не значит.
// Вторая пара так же проверяет tests/sanitizers/tsan_qt_refcount.h (счётчик
// ссылок контейнеров Qt как синхронизация):
//   cow      — запись на месте после того, как воркер отпустил свою копию
//              списка; отчётов быть НЕ должно;
//   cow-race — воркер читает буфер, не держа копии; отчёт ОБЯЗАН появиться.
// Третья — ребро вокруг деструктора QFutureInterfaceBase в tsan_qt_sync.cpp:
//   future      — последний по результатам и последний по d — разные потоки,
//                 удаление d упорядочено после записи в хранилище; отчётов
//                 быть НЕ должно;
//   future-race — два НЕСВЯЗАННЫХ QFuture в двух потоках и настоящая гонка
//                 между этими потоками; отчёт ОБЯЗАН появиться (общий на все
//                 QFuture мьютекс в прежней версии слоя его прятал).
// Четвёртая — ребро QThread::wait (поток проекта, StdinSpooler):
//   thread      — поток пишет до своего последнего emit, главный читает и
//                 удаляет объект потока после wait(); отчётов быть НЕ должно;
//   thread-race — поток пишет ПОСЛЕ последнего emit; отчёт ОБЯЗАН появиться.
//
// Как режимы race проверяют отчёт. Не по тексту: какой из двух доступов TSan
// назовёт первым (и, значит, в строке SUMMARY), решает планировщик, а под
// нагрузкой (ctest -j) компилятор ещё и встраивает функцию-читателя в лямбду.
// Режим регистрирует диапазон адресов СВОИХ гоночных данных, а хук рантайма
// __tsan_on_report разбирает каждый отчёт штатным API (__tsan_get_report_*):
// засчитывается «data-race», у которого хоть один доступ попал в диапазон. В
// конце режим печатает «N report(s) on canary data» — CTest требует N > 0.
// Переменная DENDRO_CANARY_BLIND=1 намеренно ослепляет режимы race: оба
// гоночных доступа берут общий мьютекс, гонки нет, N обязано быть 0 (тесты
// *_blinded) — так проверяется, что сама проверка не засчитывает лишнего.
// ============================================================================

#include <QCoreApplication>
#include <QFuture>
#include <QFutureInterface>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QDeadlineTimer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// --- Учёт отчётов TSan о гонках на данных канарейки --------------------------

extern "C" {
// Отладочный API рантайма TSan (tsan_debugging.cpp), заголовка у него нет.
int __tsan_get_report_data(void* report, const char** description, int* count,
                           int* stackCount, int* mopCount, int* locCount,
                           int* mutexCount, int* threadCount, int* uniqueTidCount,
                           void** sleepTrace, unsigned long traceSize);
int __tsan_get_report_mop(void* report, unsigned long idx, int* tid, void** addr,
                          int* size, int* write, int* atomic, void** trace,
                          unsigned long traceSize);
}

namespace {
// Диапазон гоночных данных режима и число отчётов о нём. Пишутся до запуска
// потоков и после их завершения; хук читает и считает атомиками без
// инструментирования — сам учёт не должен давать TSan ни доступов, ни рёбер.
const char* g_canaryBegin = nullptr;
const char* g_canaryEnd = nullptr;
int g_canaryReports = 0;
std::mutex g_blindMutex;

bool blinded()
{
    const char* v = std::getenv("DENDRO_CANARY_BLIND");
    return v && *v && std::strcmp(v, "0") != 0;
}

// Ослеплённый режим: оба гоночных доступа под общим мьютексом.
template <typename Fn>
auto maybeBlind(bool blind, Fn&& fn)
{
    if (!blind)
        return fn();
    std::lock_guard<std::mutex> lock(g_blindMutex);
    return fn();
}

// Гоночное значение на своей кэш-линии. TSan помнит в ячейке теневой памяти
// (8 байт программы) лишь несколько последних доступов; если рядом лежит флаг,
// который другой поток крутит в цикле ожидания, его чтения вытесняют запись о
// гоночном доступе, и гонка пропадает — так future-race молча проходил мимо,
// пока int стоял на стеке рядом с флагами.
struct alignas(64) RacyInt {
    int value = 0;
};

void watchCanaryData(const void* begin, std::size_t size)
{
    g_canaryBegin = static_cast<const char*>(begin);
    g_canaryEnd = g_canaryBegin + size;
}

int printCanaryReports(const char* mode)
{
    const int n = __atomic_load_n(&g_canaryReports, __ATOMIC_SEQ_CST);
    std::fprintf(stderr, "tsan_canary %s: %d report(s) on canary data%s\n", mode, n,
                 blinded() ? " (blinded)" : "");
    return 0;
}
} // namespace

extern "C" __attribute__((no_sanitize_thread)) void __tsan_on_report(void* report)
{
    const char* description = nullptr;
    int count = 0, stacks = 0, mops = 0, locs = 0, mutexes = 0, threads = 0, tids = 0;
    void* sleepTrace = nullptr;
    if (!__tsan_get_report_data(report, &description, &count, &stacks, &mops, &locs,
                                &mutexes, &threads, &tids, &sleepTrace, 1))
        return;
    if (!description || std::strcmp(description, "data-race") != 0)
        return;
    const char* begin = __atomic_load_n(&g_canaryBegin, __ATOMIC_RELAXED);
    const char* end = __atomic_load_n(&g_canaryEnd, __ATOMIC_RELAXED);
    for (int i = 0; i < mops; ++i) {
        int tid = 0, size = 0, write = 0, atomic = 0;
        void* addr = nullptr;
        void* trace = nullptr;
        if (!__tsan_get_report_mop(report, unsigned(i), &tid, &addr, &size, &write,
                                   &atomic, &trace, 1))
            continue;
        const char* a = static_cast<const char*>(addr);
        if (a < end && a + size > begin) {
            __atomic_fetch_add(&g_canaryReports, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

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
    const bool blind = blinded();
    QList<int> list(64, 1);
    QList<int> keeper = list; // общий буфер, счётчик 2
    watchCanaryData(list.constData(), sizeof(int) * std::size_t(list.size()));
    std::atomic_bool stop{false};
    long long sum = 0;
    std::thread reader([raw = keeper.constData(), &stop, &sum, blind] {
        while (!stop.load(std::memory_order_relaxed))
            sum += maybeBlind(blind, [raw] { return racyListRead(raw); });
    });
    keeper = QList<int>(); // счётчик 2 → 1: дальше запись идёт на месте
    for (int i = 0; i < 2000; ++i) {
        maybeBlind(blind, [&list, i] { racyListWrite(list, i); });
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    std::fprintf(stderr, "tsan_canary cow-race: sum %lld\n", sum);
    return printCanaryReports("cow-race");
}

// Несинхронизированные доступы вынесены в функции с приметными именами — для
// читаемости отчёта; засчитывается он по адресу данных (см. начало файла).
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
    const bool blind = blinded();
    Worker worker;
    auto racyCell = std::make_unique<RacyInt>();
    int& racy = racyCell->value;
    watchCanaryData(&racy, sizeof racy);
    std::atomic_bool finished{false};
    QObject::connect(&worker, &Worker::batchReady, &worker,
                     [&](const QList<int>&, int* plain) {
                         maybeBlind(blind, [&] { racyGuiWrite(racy, *plain); });
                         delete plain;
                     },
                     Qt::QueuedConnection);
    QFutureWatcher<void> watcher;
    QObject::connect(&watcher, &QFutureWatcher<void>::finished, &watcher,
                     [&] { finished = true; });
    watcher.setFuture(QtConcurrent::run([&worker, &racy, blind]() {
        for (int i = 0; i < 200; ++i) {
            emit worker.batchReady(QList<int>{i}, new int(i));
            // после emit, пока GUI-поток пишет туда же
            maybeBlind(blind, [&racy] { racyWorkerWrite(racy); });
            QThread::usleep(200);
        }
    }));
    pumpUntil(finished);
    QThreadPool::globalInstance()->waitForDone();
    std::fprintf(stderr, "tsan_canary race: counter %d\n", racy);
    return printCanaryReports("race");
}

// Ребро деструктора QFutureInterfaceBase, правильная сторона. Как задача
// QtConcurrent::run: поток-исполнитель держит обещание (QFutureInterface<T>)
// и, дольше всех, ещё одну копию базового интерфейса — последним по d будет
// он; потребитель читает результат и отпускает свой QFuture последним по
// результатам, то есть пишет в хранилище внутри d (clear<T>). Удаление d в
// исполнителе обязано быть упорядочено после этой записи. Потоки —
// std::thread, о готовности стороны сообщают relaxed-флагами: никаких других
// рёбер, кроме проверяемых обёрток derefT и деструктора, у TSan нет.
static int runFuture()
{
    int failures = 0;
    for (int round = 0; round < 200; ++round) {
        QFutureInterface<int> promise;
        promise.reportStarted();
        QFuture<int> future = promise.future();
        std::atomic_bool consumerDone{false};
        std::thread producer([promise = std::move(promise), &consumerDone, round]() mutable {
            QFutureInterfaceBase keepAlive = promise; // держит d дольше всех
            promise.reportResult(round);
            promise.reportFinished();
            promise = QFutureInterface<int>(); // не последний по результатам
            while (!consumerDone.load(std::memory_order_relaxed))
                std::this_thread::yield();
            // keepAlive разрушается здесь: последний владелец d удаляет его
        });
        if (future.result() != round)
            ++failures;
        future = QFuture<int>(); // последний по результатам: clear<T> в d
        consumerDone.store(true, std::memory_order_relaxed);
        producer.join();
    }
    std::fprintf(stderr, "tsan_canary future: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

__attribute__((noinline)) static void racyFutureWrite(int& value, int v)
{
    value = v;
}

__attribute__((noinline)) static int racyFutureRead(const int& value)
{
    return value;
}

// Ребро деструктора, неправильная сторона: два потока, у каждого СВОЙ QFuture
// (свой d). Первый пишет в общий int и разрушает своё будущее, второй
// дожидается этого (relaxed-флаг, ребра нет), разрушает своё и читает int.
// Между потоками нет ничего общего, кроме того, что оба разрушили какой-то
// QFuture, — это гонка, отчёт обязан появиться. Общий на все деструкторы
// мьютекс дал бы ребро «первый → второй» и спрятал бы её.
static int runFutureRace()
{
    const bool blind = blinded();
    auto racyCell = std::make_unique<RacyInt>();
    int& racy = racyCell->value;
    watchCanaryData(&racy, sizeof racy);
    long long sum = 0;
    for (int round = 0; round < 20; ++round) {
        std::atomic_bool firstDone{false};
        std::thread first([&racy, &firstDone, round, blind] {
            QFutureInterface<int> promise;
            promise.reportStarted();
            QFuture<int> own = promise.future();
            maybeBlind(blind, [&] { racyFutureWrite(racy, round); });
            own = QFuture<int>();
            promise = QFutureInterface<int>(); // d первого удалён
            firstDone.store(true, std::memory_order_relaxed);
        });
        std::thread second([&racy, &firstDone, &sum, blind] {
            QFutureInterface<int> promise;
            promise.reportStarted();
            QFuture<int> own = promise.future();
            while (!firstDone.load(std::memory_order_relaxed))
                std::this_thread::yield();
            own = QFuture<int>();
            promise = QFutureInterface<int>(); // d второго удалён
            sum += maybeBlind(blind, [&] { return racyFutureRead(racy); });
        });
        first.join();
        second.join();
    }
    std::fprintf(stderr, "tsan_canary future-race: sum %lld\n", sum);
    return printCanaryReports("future-race");
}

// Четвёртая пара — ребро QThread::wait в tsan_qt_sync.cpp (StdinSpooler):
// поток проекта заканчивает run() своими emit, wait() его ждёт.
__attribute__((noinline)) static void racyThreadWrite(int& value, int v)
{
    value = v;
}

__attribute__((noinline)) static int racyThreadRead(const int& value)
{
    return value;
}

class EmittingThread : public QThread {
    Q_OBJECT
public:
    int* cell = nullptr;
    int value = 0;
    bool writeAfterEmit = false; // режим thread-race
    bool blind = false;
    std::atomic_bool stop{false}; // как m_stop спулера: читает поток, объект удаляет GUI
signals:
    void progress(int total);
    void done(int total);

protected:
    void run() override
    {
        // Члены — до последнего emit, как у спулера: после него объект
        // может удалить тот, кто дождался wait().
        int* const target = cell;
        const int v = value;
        const bool after = writeAfterEmit;
        const bool underMutex = blind;
        for (int i = 0; i < 3 && !stop.load(std::memory_order_relaxed); ++i)
            emit progress(i);
        if (!after)
            racyThreadWrite(*target, v);
        emit done(v);
        if (after) // после последнего emit — ребра нет
            maybeBlind(underMutex, [&] { racyThreadWrite(*target, v); });
    }
};

// thread — запись до последнего emit, чтение и удаление объекта после
// wait(); отчётов быть НЕ должно.
static int runThread()
{
    int failures = 0;
    auto cell = std::make_unique<RacyInt>();
    for (int round = 0; round < 20; ++round) {
        auto thread = std::make_unique<EmittingThread>();
        thread->cell = &cell->value;
        thread->value = round + 1;
        thread->start();
        if (!thread->wait(QDeadlineTimer(30000)))
            ++failures;
        if (racyThreadRead(cell->value) != round + 1)
            ++failures;
        thread->stop.store(true);
        thread.reset(); // объект, чей stop читал поток
    }
    std::fprintf(stderr, "tsan_canary thread: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

// thread-race — запись ПОСЛЕ последнего emit потока, чтение после wait();
// отчёт ОБЯЗАН появиться: ребро wait() не должно упорядочивать всё подряд.
static int runThreadRace()
{
    const bool blind = blinded();
    auto cell = std::make_unique<RacyInt>();
    int& racy = cell->value;
    watchCanaryData(&racy, sizeof racy);
    long long sum = 0;
    for (int round = 0; round < 20; ++round) {
        EmittingThread thread;
        thread.cell = &racy;
        thread.value = round;
        thread.writeAfterEmit = true;
        thread.blind = blind;
        thread.start();
        thread.wait();
        sum += maybeBlind(blind, [&] { return racyThreadRead(racy); });
    }
    std::fprintf(stderr, "tsan_canary thread-race: sum %lld\n", sum);
    return printCanaryReports("thread-race");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    struct Mode {
        const char* name;
        int (*run)();
    };
    static const Mode modes[] = {
        {"clean", runClean},   {"race", runRace},     {"cow", runCow},
        {"cow-race", runCowRace}, {"future", runFuture}, {"future-race", runFutureRace},
        {"thread", runThread}, {"thread-race", runThreadRace},
    };
    if (argc == 2)
        for (const Mode& mode : modes)
            if (std::strcmp(argv[1], mode.name) == 0)
                return mode.run();
    std::fprintf(stderr, "usage: tsan_canary clean|race|cow|cow-race|future|future-race|thread|thread-race\n");
    return 2;
}

#include "main.moc"
