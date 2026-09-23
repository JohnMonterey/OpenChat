#include "diagnostics/BlackBox.h"

#include "diagnostics/BlackBoxData.h"

#include <QByteArray>
#include <QTime>

#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__APPLE__)
#    include <pthread.h>
#    include <time.h>
#elif defined(__linux__)
#    include <sys/syscall.h>
#    include <time.h>
#    include <unistd.h>
#else
#    include <pthread.h>
#    include <time.h>
#endif

namespace OpenChat {

namespace BlackBoxData {

namespace {

// Zero-initialised, so usable from the very first line of main() and from
// static initialisers, before anything is mapped.
Data g_staticData;
std::atomic<Data *> g_data{&g_staticData};

} // namespace

Data *current() noexcept
{
    return g_data.load(std::memory_order_acquire);
}

void adopt(Data *mapped) noexcept
{
    Data *previous = current();
    if (mapped == nullptr || mapped == previous)
        return;
    // Plain bytes on both sides: the atomics are lock-free and the same size as
    // the integers they wrap (asserted in the header). Adoption happens on the
    // main thread before any other thread records anything.
    std::memcpy(static_cast<void *>(mapped), static_cast<const void *>(previous), sizeof(Data));
    g_data.store(mapped, std::memory_order_release);
}

std::uint64_t currentThreadId() noexcept
{
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
#elif defined(__linux__)
    return std::uint64_t(syscall(SYS_gettid));
#else
    return std::uint64_t(reinterpret_cast<std::uintptr_t>(pthread_self()));
#endif
}

std::int64_t nowMs() noexcept
{
#if defined(_WIN32)
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks;
    ticks.LowPart = time.dwLowDateTime;
    ticks.HighPart = time.dwHighDateTime;
    // 100 ns ticks since 1601 -> milliseconds since 1970.
    return std::int64_t((ticks.QuadPart - 116444736000000000ULL) / 10000ULL);
#else
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return std::int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
}

void copyText(char *destination, int capacity, const char *source, int length) noexcept
{
    if (destination == nullptr || capacity <= 0)
        return;
    int count = 0;
    if (source != nullptr) {
        while (count < length && count < capacity - 1 && source[count] != '\0')
            ++count;
        // Cut at a character boundary: back off over continuation bytes, then
        // over the lead byte whose sequence they did not finish.
        if (count < length && source[count] != '\0' && count > 0) {
            int cut = count;
            while (cut > 0 && (static_cast<unsigned char>(source[cut]) & 0xC0) == 0x80)
                --cut;
            count = cut;
        }
        std::memcpy(destination, source, size_t(count));
    }
    destination[count] = '\0';
}

} // namespace BlackBoxData

namespace BlackBox {

namespace {

using namespace BlackBoxData;

// Guards only the choice of a context slot. Readers never take it.
std::mutex g_contextMutex;

void copyLiteral(char *destination, int capacity, const char *source) noexcept
{
    copyText(destination, capacity, source, source ? int(std::strlen(source)) : 0);
}

// The calling thread's slot in Data::threads, claimed on first use and handed
// back when the thread ends, so short-lived threads do not use the table up.
struct ThreadState final {
    int slot = -1;
    const char *area = nullptr;
    const char *activity = nullptr;

    ~ThreadState()
    {
        if (slot < 0)
            return;
        Thread &thread = current()->threads[slot];
        thread.sequence.fetch_add(1, std::memory_order_acq_rel);
        thread.name[0] = '\0';
        thread.area[0] = '\0';
        thread.activity[0] = '\0';
        thread.sequence.fetch_add(1, std::memory_order_acq_rel);
        thread.threadId.store(0, std::memory_order_release);
    }

    Thread *claim() noexcept
    {
        if (slot >= 0)
            return &current()->threads[slot];
        if (slot == -2)
            return nullptr;
        const std::uint64_t id = currentThreadId();
        Data *data = current();
        for (int index = 0; index < threadSlots; ++index) {
            std::uint64_t expected = 0;
            if (data->threads[index].threadId.compare_exchange_strong(
                    expected, id, std::memory_order_acq_rel)) {
                slot = index;
                return &data->threads[index];
            }
        }
        // Table full: this thread is simply not described. Reports still show
        // every other thread and the events.
        slot = -2;
        return nullptr;
    }
};

ThreadState &threadState() noexcept
{
    thread_local ThreadState state;
    return state;
}

void publishActivity(ThreadState &state) noexcept
{
    Thread *thread = state.claim();
    if (thread == nullptr)
        return;
    thread->sequence.fetch_add(1, std::memory_order_acq_rel);
    copyLiteral(thread->area, threadAreaBytes, state.area);
    copyLiteral(thread->activity, threadActivityBytes, state.activity);
    thread->sequence.fetch_add(1, std::memory_order_acq_rel);
}

void recordLine(const QByteArray &line) noexcept
{
    Data *data = current();
    const std::uint32_t number = data->header.nextEvent.fetch_add(1, std::memory_order_acq_rel);
    Event &event = data->events[number % eventSlots];
    // Seqlock writer: mark the slot odd, and fence so the text written next
    // cannot become visible before the mark does.
    event.sequence.store(2 * number + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    copyText(event.text, eventTextBytes, line.constData(), int(line.size()));
    event.sequence.store(2 * number + 2, std::memory_order_release);
}

} // namespace

void record(const char *area, const QString &text)
{
    const QByteArray line = QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")).toLatin1()
        + ' ' + QByteArray(area ? area : "app") + ": " + text.toUtf8();
    recordLine(line);
}

void record(const char *area, const char *text)
{
    record(area, QString::fromUtf8(text ? text : ""));
}

void setContext(const char *area, const QString &value)
{
    if (area == nullptr || *area == '\0')
        return;
    const QByteArray utf8 = value.toUtf8();
    std::lock_guard lock(g_contextMutex);
    Data *data = current();
    Context *target = nullptr;
    Context *unused = nullptr;
    for (Context &context : data->contexts) {
        if (context.area[0] == '\0') {
            if (unused == nullptr)
                unused = &context;
        } else if (std::strncmp(context.area, area, contextAreaBytes - 1) == 0) {
            target = &context;
            break;
        }
    }
    if (target == nullptr) {
        if (utf8.isEmpty() || unused == nullptr)
            return;
        target = unused;
    }
    target->sequence.fetch_add(1, std::memory_order_acq_rel);
    if (utf8.isEmpty()) {
        target->area[0] = '\0';
        target->value[0] = '\0';
    } else {
        copyLiteral(target->area, contextAreaBytes, area);
        copyText(target->value, contextValueBytes, utf8.constData(), int(utf8.size()));
    }
    target->sequence.fetch_add(1, std::memory_order_acq_rel);
}

void nameThread(const char *name)
{
    ThreadState &state = threadState();
    Thread *thread = state.claim();
    if (thread == nullptr)
        return;
    thread->sequence.fetch_add(1, std::memory_order_acq_rel);
    copyLiteral(thread->name, threadNameBytes, name);
    thread->sequence.fetch_add(1, std::memory_order_acq_rel);
}

void heartbeat()
{
    current()->header.heartbeatMs.store(nowMs(), std::memory_order_release);
}

Activity::Activity(const char *area, const char *activity) noexcept
{
    ThreadState &state = threadState();
    m_previousArea = state.area;
    m_previousActivity = state.activity;
    state.area = area;
    state.activity = activity;
    publishActivity(state);
}

Activity::~Activity()
{
    ThreadState &state = threadState();
    state.area = m_previousArea;
    state.activity = m_previousActivity;
    publishActivity(state);
}

} // namespace BlackBox

} // namespace OpenChat
