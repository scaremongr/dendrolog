// ============================================================================
// tsan_qt_sync.cpp — рассказывает ThreadSanitizer о синхронизации внутри Qt.
//
// Компилируется ТОЛЬКО при -DDENDROLOG_SANITIZER=thread (см. корневой
// CMakeLists.txt) и в обычную сборку не попадает.
//
// Зачем. Qt, с которым мы линкуемся, не инструментирован. Его мьютексы —
// фьютексы и атомики внутри libQt6Core — TSan не видит, поэтому любая
// ПРАВИЛЬНАЯ передача данных между потоками через Qt выглядит гонкой:
//   • воркер записал батч и послал queued-сигнал, GUI-поток читает батч;
//   • GUI-поток собрал лямбду и отдал её в QThreadPool, воркер читает захват;
//   • воркер положил результат в QFuture, GUI-поток его забирает.
// В каждом случае между записью и чтением стоит настоящий мьютекс Qt (очередь
// событий потока, очередь пула, мьютекс QFutureInterface). Подавлять такие
// отчёты по именам НАШИХ функций нельзя: в их стеках нет ни одного кадра Qt,
// и подавление спрятало бы заодно настоящие гонки в том же коде.
//
// Как. Вызовы из нашего кода в Qt, которые берут эти мьютексы, перехвачены
// линкером (--wrap=<символ>, список — в CMakeLists.txt рядом с этим файлом);
// обёртка сообщает TSan ребро happens-before парой __tsan_release (сторона,
// которая отдаёт) / __tsan_acquire (сторона, которая принимает) и зовёт
// настоящую функцию. Доставку события в поток-получатель ловит штатный хук
// QInternal::EventNotifyCallback. Поведение программы не меняется.
//
// Тонкость отправки. Копии аргументов queued-сигнала Qt делает ВНУТРИ
// activate(), уже после входа в обёртку, поэтому ребро объявляется после
// возврата из настоящей функции. Получатель мог бы начать доставку раньше —
// в щель между постановкой события в очередь и возвратом отправителя. Её
// закрывает счётчик отправок «в полёте»: хук доставки коротко ждёт, пока
// отправители других потоков выйдут из обёртки (ожидание ограничено по
// времени, взаимоблокировка невозможна).
//
// Модель грубее настоящей, и это цена подхода:
//   • один «мьютекс» на все очереди событий, один на все пулы; ребро
//     объявляет каждый emit, даже прямой или без получателей. Поэтому пара
//     «воркер записал — что-то послал — GUI-поток что-то получил — прочитал»
//     считается упорядоченной, даже если послано было не то и не туда.
//     Рёбра направленные (отдал → принял): одновременный доступ двух потоков
//     без передачи между ними по-прежнему ловится (tests/tsan_canary). Так же
//     грубо — report*/wait* у QFuture: один ключ на все будущие процесса;
//   • точные рёбра — только у счётчиков ссылок QFuture (derefT и деструктор):
//     ключ — разделяемое состояние d конкретного будущего, несвязанные QFuture
//     друг друга не упорядочивают (режимы future/future-race канарейки).
//     Старую копию при КОПИРУЮЩЕМ присваивании QFuture<void>
//     (QFutureInterfaceBase::operator=) Qt разрушает внутри себя, мимо
//     обёртки; перемещающее присваивание встроено в наш код и её проходит;
//   • покрыты только пути, которыми пользуется проект: сигналы, postEvent,
//     QtConcurrent::run/map/mapped, QFuture/QFutureWatcher, QThread::wait
//     после последнего emit потока. Голый QRunnable,
//     QThreadPool::start(std::function), queued invokeMethod из воркера рёбер
//     не получат и дадут ложные отчёты — слой нужно дополнить;
//   • манглинг символов — Qt 6.8 без пространства имён (проверка версии — в
//     корневом CMakeLists.txt).
// ============================================================================

// Qt открывает поле d у QFutureInterfaceBase (и QFuture) для своих тестов
// макросом QFUTURE_TEST. Слою оно нужно как ключ разделяемого состояния
// будущего: доступ по имени проверяет компилятор, а не догадка о смещении.
// Раскладка от этого не меняется — убирается только метка private:.
#define QFUTURE_TEST

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEvent>
#include <QFutureInterface>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QObject>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/qtconcurrentthreadengine.h>

#include <sanitizer/tsan_interface.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <sched.h>

namespace {

// Адреса-ключи «невидимых мьютексов» Qt. Значения не используются.
char g_eventQueue; // очередь событий потока: postEvent → доставка
char g_threadPool; // очередь QThreadPool: start → начало задачи
char g_future;     // QFutureInterface: report* воркера → ожидание/чтение

inline void gives(char& key) { __tsan_release(&key); }
inline void takes(char& key) { __tsan_acquire(&key); }

// Отправки в очередь событий, которые ещё не вернулись из Qt (все потоки), и
// глубина таких отправок в текущем потоке: свои вложенные emit получатель не
// ждёт — он сам внутри них.
std::atomic<int> g_postsInFlight{0};
std::atomic<int> g_postsStuck{0};
thread_local int t_postDepth = 0;

// Обёртка вокруг вызова Qt, который ставит событие в чужую очередь.
struct PostScope {
    PostScope()
    {
        ++t_postDepth;
        g_postsInFlight.fetch_add(1, std::memory_order_relaxed);
    }
    ~PostScope()
    {
        gives(g_eventQueue);
        g_postsInFlight.fetch_sub(1, std::memory_order_relaxed);
        --t_postDepth;
    }
};

// Ждёт, пока чужие потоки выйдут из обёртки (inFlight опустится до числа
// собственных вложенных входов). Обычно это ноль-две итерации. Потолок нужен
// на случай потока, который сам ждёт нас или надолго застрял внутри вызова
// (долгий direct-слот в воркере): таких «застрявших» запоминаем в stuck и в
// следующие разы не ждём, иначе каждая доставка события стоила бы потолка.
void waitForOthers(const std::atomic<int>& inFlight, int ownDepth, std::atomic<int>& stuck)
{
    int others = inFlight.load(std::memory_order_relaxed) - ownDepth;
    if (others < stuck.load(std::memory_order_relaxed))
        stuck.store(qMax(others, 0), std::memory_order_relaxed);
    if (others <= stuck.load(std::memory_order_relaxed))
        return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (true) {
        others = inFlight.load(std::memory_order_relaxed) - ownDepth;
        if (others <= stuck.load(std::memory_order_relaxed))
            return;
        if (std::chrono::steady_clock::now() >= deadline) {
            stuck.store(others, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
}

// Потоки пула внутри ядра QtConcurrent::map/filter (ThreadEngineBase::run).
// Ядро само сводит свои потоки барьером-семафором внутри Qt: последний
// вышедший поток завершает задание и удаляет ядро. Ребро «остальные потоки
// ядра → завершающий» объявляется на выходе из run() и принимается перед
// reportFinished(); щель между ними закрыта так же, как у отправок.
char g_engine;
std::atomic<int> g_engineRunsInFlight{0};
std::atomic<int> g_engineRunsStuck{0};
thread_local int t_engineDepth = 0;

struct EngineRunScope {
    EngineRunScope()
    {
        ++t_engineDepth;
        g_engineRunsInFlight.fetch_add(1, std::memory_order_relaxed);
    }
    ~EngineRunScope()
    {
        gives(g_engine);
        g_engineRunsInFlight.fetch_sub(1, std::memory_order_relaxed);
        --t_engineDepth;
    }
};

// Взаимное исключение деструкторов копий одного QFutureInterfaceBase (ключ —
// разделяемый d), невидимое для TSan: см. обёртку деструктора ниже. Таблица
// занятых ключей под спин-блокировкой; функции не инструментируются
// (no_sanitize_thread), поэтому ни их атомики, ни обычные чтения и записи
// TSan не видит и рёбер между потоками не получает. Деструкторы, идущие
// одновременно, — редкость, таблица на 256 ключей с запасом; переполненная —
// ждёт освобождения.
namespace gate {

constexpr int kSlots = 256;
struct Slot {
    const void* key;
    const void* owner; // адрес thread_local-маркера потока-владельца
    int depth;
};
Slot g_slots[kSlots];
int g_tableLock;
thread_local char t_threadMarker;

#define DENDRO_TSAN_INVISIBLE __attribute__((no_sanitize_thread, noinline))

DENDRO_TSAN_INVISIBLE void lockTable()
{
    while (__atomic_exchange_n(&g_tableLock, 1, __ATOMIC_ACQUIRE) != 0)
        sched_yield();
}

DENDRO_TSAN_INVISIBLE void unlockTable()
{
    __atomic_store_n(&g_tableLock, 0, __ATOMIC_RELEASE);
}

// true — ключ взят этим потоком (впервые или повторно).
DENDRO_TSAN_INVISIBLE bool tryEnter(const void* key, const void* me)
{
    lockTable();
    Slot* freeSlot = nullptr;
    for (Slot& slot : g_slots) {
        if (slot.key == key) {
            const bool mine = slot.owner == me;
            if (mine)
                ++slot.depth;
            unlockTable();
            return mine;
        }
        if (!slot.key && !freeSlot)
            freeSlot = &slot;
    }
    if (freeSlot) {
        freeSlot->key = key;
        freeSlot->owner = me;
        freeSlot->depth = 1;
    }
    unlockTable();
    return freeSlot != nullptr;
}

DENDRO_TSAN_INVISIBLE void leave(const void* key)
{
    lockTable();
    for (Slot& slot : g_slots) {
        if (slot.key == key) {
            if (--slot.depth == 0)
                slot.key = nullptr;
            break;
        }
    }
    unlockTable();
}

#undef DENDRO_TSAN_INVISIBLE

} // namespace gate

// Секция деструктора копии разделяемого состояния d: исключение по d, затем
// видимое TSan «атомарное» ребро на d (принять от прежних владельцев, отдать
// своё). d == nullptr — перемещённая копия, ей нечего упорядочивать.
class DestructorGate {
public:
    explicit DestructorGate(void* shared) : m_shared(shared)
    {
        if (!m_shared)
            return;
        while (!gate::tryEnter(m_shared, &gate::t_threadMarker))
            sched_yield();
        __tsan_acquire(m_shared);
        __tsan_release(m_shared);
    }
    ~DestructorGate()
    {
        if (m_shared)
            gate::leave(m_shared);
    }
    DestructorGate(const DestructorGate&) = delete;
    DestructorGate& operator=(const DestructorGate&) = delete;

private:
    void* const m_shared;
};

// Доставка события получателю. Интересны только события, которые ставят в
// очередь из другого потока: queued-вызовы и уведомления QFuture.
bool onEventDelivery(void** data)
{
    const auto* event = static_cast<const QEvent*>(data[1]);
    const int type = event ? int(event->type()) : 0;
    if (type == QEvent::MetaCall || type == QEvent::FutureCallOut) {
        waitForOthers(g_postsInFlight, t_postDepth, g_postsStuck);
        takes(g_eventQueue);
    }
    return false; // событие идёт дальше обычным путём
}

const bool g_hookInstalled =
    QInternal::registerCallback(QInternal::EventNotifyCallback, onEventDelivery);

} // namespace

extern "C" {

// --- Очередь событий: отправляющая сторона ---------------------------------

// Любой emit сигнала из нашего moc-кода. Для queued-соединения внутри
// ставится QMetaCallEvent в очередь потока получателя.
void __real__ZN11QMetaObject8activateEP7QObjectPKS_iPPv(QObject*, const QMetaObject*,
                                                        int, void**);
void __wrap__ZN11QMetaObject8activateEP7QObjectPKS_iPPv(QObject* sender,
                                                        const QMetaObject* mo,
                                                        int signalIndex, void** argv)
{
    PostScope post;
    __real__ZN11QMetaObject8activateEP7QObjectPKS_iPPv(sender, mo, signalIndex, argv);
}

// QCoreApplication::postEvent(QObject*, QEvent*, int)
void __real__ZN16QCoreApplication9postEventEP7QObjectP6QEventi(QObject*, QEvent*, int);
void __wrap__ZN16QCoreApplication9postEventEP7QObjectP6QEventi(QObject* receiver,
                                                               QEvent* event, int priority)
{
    PostScope post;
    __real__ZN16QCoreApplication9postEventEP7QObjectP6QEventi(receiver, event, priority);
}

// --- Пул потоков ------------------------------------------------------------

// QThreadPool::start(QRunnable*, int): задача уходит в очередь пула.
void __real__ZN11QThreadPool5startEP9QRunnablei(QThreadPool*, QRunnable*, int);
void __wrap__ZN11QThreadPool5startEP9QRunnablei(QThreadPool* pool, QRunnable* runnable,
                                                int priority)
{
    gives(g_threadPool);
    __real__ZN11QThreadPool5startEP9QRunnablei(pool, runnable, priority);
}

// QtConcurrent::ThreadEngineBase::startThread(): map/filter отдаёт себя в пул.
void __real__ZN12QtConcurrent16ThreadEngineBase11startThreadEv(
    QtConcurrent::ThreadEngineBase*);
void __wrap__ZN12QtConcurrent16ThreadEngineBase11startThreadEv(
    QtConcurrent::ThreadEngineBase* engine)
{
    gives(g_threadPool);
    __real__ZN12QtConcurrent16ThreadEngineBase11startThreadEv(engine);
}

// QFutureInterfaceBase::isCanceled() const — первое, что делает задача
// QtConcurrent::run в потоке пула (RunFunctionTaskBase::run), до чтения
// захваченных данных.
bool __real__ZNK20QFutureInterfaceBase10isCanceledEv(const QFutureInterfaceBase*);
bool __wrap__ZNK20QFutureInterfaceBase10isCanceledEv(const QFutureInterfaceBase* f)
{
    takes(g_threadPool);
    return __real__ZNK20QFutureInterfaceBase10isCanceledEv(f);
}

// QtConcurrent::ThreadEngineBase::run() — вход потока пула в ядро map/filter.
// Шаблоны ядер этот метод не переопределяют, поэтому в их таблицах виртуальных
// методов (они живут в наших объектных файлах) стоит ссылка на символ Qt, и
// --wrap подменяет её так же, как прямой вызов. Так ребро получает каждый
// поток ядра, включая те, что Qt запускает сам изнутри библиотеки.
void __real__ZN12QtConcurrent16ThreadEngineBase3runEv(QtConcurrent::ThreadEngineBase*);
void __wrap__ZN12QtConcurrent16ThreadEngineBase3runEv(QtConcurrent::ThreadEngineBase* engine)
{
    takes(g_threadPool);
    EngineRunScope run;
    __real__ZN12QtConcurrent16ThreadEngineBase3runEv(engine);
}

// --- QFuture: воркер сообщает, потребитель ждёт ------------------------------
// report* берут мьютекс QFutureInterface и ставят уведомление наблюдателю
// (QFutureWatcher) в очередь событий его потока.

void __real__ZN20QFutureInterfaceBase14reportFinishedEv(QFutureInterfaceBase*);
void __wrap__ZN20QFutureInterfaceBase14reportFinishedEv(QFutureInterfaceBase* f)
{
    if (t_engineDepth > 0) {
        // Завершающий поток ядра map/filter: принять работу остальных.
        waitForOthers(g_engineRunsInFlight, t_engineDepth, g_engineRunsStuck);
        takes(g_engine);
    }
    gives(g_future);
    PostScope post;
    __real__ZN20QFutureInterfaceBase14reportFinishedEv(f);
}

void __real__ZN20QFutureInterfaceBase18reportResultsReadyEii(QFutureInterfaceBase*, int, int);
void __wrap__ZN20QFutureInterfaceBase18reportResultsReadyEii(QFutureInterfaceBase* f,
                                                             int begin, int end)
{
    gives(g_future);
    PostScope post;
    __real__ZN20QFutureInterfaceBase18reportResultsReadyEii(f, begin, end);
}

// QFutureInterfaceBase::derefT() const — счётчик ссылок на хранилище
// результатов (атомик внутри Qt). Кто уронил его до нуля, тот и удаляет
// результаты: обычно поток пула, разрушая задачу после того, как GUI-поток
// прочитал result() и отпустил свой QFuture. Ведёт себя как атомарная
// операция: принимает всё, что отдали прежние владельцы, и отдаёт своё. Ключ —
// общий для копий указатель d (разделяемое состояние), а не this.
// У перемещённой копии d == nullptr (Qt тогда ничего не считает): ребро по
// адресу 0 связало бы между собой ВСЕ потоки, разрушившие хоть одну такую
// копию, — а их создаёт каждое перемещающее присваивание QFuture.
bool __real__ZNK20QFutureInterfaceBase6derefTEv(const QFutureInterfaceBase*);
bool __wrap__ZNK20QFutureInterfaceBase6derefTEv(const QFutureInterfaceBase* f)
{
    void* const shared = f->d;
    if (!shared)
        return __real__ZNK20QFutureInterfaceBase6derefTEv(f);
    __tsan_release(shared);
    const bool stillReferenced = __real__ZNK20QFutureInterfaceBase6derefTEv(f);
    __tsan_acquire(shared);
    return stillReferenced;
}

// QFutureInterfaceBase::~QFutureInterfaceBase — второй счётчик ссылок, на сам
// разделяемый d (атомик внутри Qt): последний владелец удаляет d вместе с
// хранилищем результатов. ~QFutureInterface<T> сначала пишет в хранилище
// (clear<T>, если derefT сказал «последний по результатам»), потом зовёт этот
// деструктор; поток пула, разрушая задачу, может оказаться последним по d и
// удалить его. Нужное ребро — «всё, что сделал любой прежний владелец ЭТОГО d
// → удаление d».
//
// Пара release/acquire вокруг вызова его не даёт: удаление — ВНУТРИ настоящей
// функции, после декремента, и acquire перед вызовом может случиться раньше
// release другого владельца, который затем уронит счётчик не последним. Поэтому
// деструкторы копий одного d взаимно исключены (DestructorGate): кто удаляет
// d, тот вошёл в свою секцию после того, как все прежние владельцы вышли из
// своих, и его acquire видит их release. Пара acquire + release стоит до
// вызова: после него d может быть уже удалён, а синхронизационный объект на
// освобождённом адресе достался бы следующему владельцу памяти. Освобождение
// d TSan видит сам (перехват operator delete) и стирает объект вместе с ним.
//
// Исключение настоящее, но TSan его не видит: это спин-блокировка по точному
// ключу d в функциях без инструментирования. Видимое TSan ребро — только
// acquire/release на самом d, поэтому несвязанные QFuture (другой d) друг
// друга не упорядочивают, и гонку между их потоками TSan по-прежнему поймает
// (режим future-race в tests/tsan_canary). Прежняя версия держала один
// рекурсивный std::mutex на все деструкторы процесса и прятала такие гонки.
//
// Вложенность: удаление d может разрушить вложенный QFutureInterface
// (продолжения) — это другой d, своя секция; тот же d из своей же секции
// блокировка допускает (счётчик глубины). Взаимоблокировки нет: поток ждёт
// чужой d, только входя в деструктор или удаляя свой d, а цикл «удаление d1
// держит ссылку на d2, удаление d2 — на d1» невозможен — d с живой ссылкой не
// удаляется.

void __real__ZN20QFutureInterfaceBaseD1Ev(QFutureInterfaceBase*);
void __wrap__ZN20QFutureInterfaceBaseD1Ev(QFutureInterfaceBase* f)
{
    DestructorGate section(f->d);
    __real__ZN20QFutureInterfaceBaseD1Ev(f);
}

void __real__ZN20QFutureInterfaceBaseD2Ev(QFutureInterfaceBase*);
void __wrap__ZN20QFutureInterfaceBaseD2Ev(QFutureInterfaceBase* f)
{
    DestructorGate section(f->d);
    __real__ZN20QFutureInterfaceBaseD2Ev(f);
}

void __real__ZN20QFutureInterfaceBase15waitForFinishedEv(QFutureInterfaceBase*);
void __wrap__ZN20QFutureInterfaceBase15waitForFinishedEv(QFutureInterfaceBase* f)
{
    __real__ZN20QFutureInterfaceBase15waitForFinishedEv(f);
    takes(g_future);
}

void __real__ZN20QFutureInterfaceBase13waitForResultEi(QFutureInterfaceBase*, int);
void __wrap__ZN20QFutureInterfaceBase13waitForResultEi(QFutureInterfaceBase* f, int index)
{
    __real__ZN20QFutureInterfaceBase13waitForResultEi(f, index);
    takes(g_future);
}

void __real__ZN18QFutureWatcherBase15waitForFinishedEv(QFutureWatcherBase*);
void __wrap__ZN18QFutureWatcherBase15waitForFinishedEv(QFutureWatcherBase* w)
{
    __real__ZN18QFutureWatcherBase15waitForFinishedEv(w);
    takes(g_future);
}

// QThreadPool::waitForDone(QDeadlineTimer): все задачи пула завершились, то
// есть каждая уже прошла свой reportFinished.
bool __real__ZN11QThreadPool11waitForDoneE14QDeadlineTimer(QThreadPool*, QDeadlineTimer);
bool __wrap__ZN11QThreadPool11waitForDoneE14QDeadlineTimer(QThreadPool* pool,
                                                           QDeadlineTimer deadline)
{
    const bool done = __real__ZN11QThreadPool11waitForDoneE14QDeadlineTimer(pool, deadline);
    if (done)
        takes(g_future);
    return done;
}

// QThread::wait(QDeadlineTimer): свой поток (StdinSpooler) вышел из run().
// Qt создаёт потоки отсоединёнными и ждёт их конца на своём мьютексе, ребра
// pthread_join у TSan нет. Поток проекта заканчивает run() своими emit
// (bytesAppended, streamFinished) — их обёртка activate отдаёт g_eventQueue;
// wait, вернувший true, его принимает. Упорядочено то, что поток сделал до
// последнего emit; записанное после него ребра не получает (режимы
// thread / thread-race канарейки). Начало потока TSan видит сам: pthread_create
// он перехватывает и в неинструментированном Qt.
bool __real__ZN7QThread4waitE14QDeadlineTimer(QThread*, QDeadlineTimer);
bool __wrap__ZN7QThread4waitE14QDeadlineTimer(QThread* thread, QDeadlineTimer deadline)
{
    const bool finished = __real__ZN7QThread4waitE14QDeadlineTimer(thread, deadline);
    if (finished)
        takes(g_eventQueue);
    return finished;
}

} // extern "C"
