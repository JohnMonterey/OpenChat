#pragma once

#include <atomic>
#include <cstdint>

// The flight recorder's memory layout, shared by the recorder (BlackBox.cpp)
// and the crash handlers that read it back. Nothing outside src/diagnostics
// should include this; use BlackBox.h.
//
// Everything is fixed-size and plain, because two very different readers have
// to understand it without help: a crash handler running on a broken process,
// which may not allocate, lock or call into Qt, and the next launch, which reads
// the previous session's copy back out of a file.
namespace OpenChat::BlackBoxData {

inline constexpr std::uint32_t magic = 0x4B42434Fu; // "OCBK", little-endian
// Bump whenever the layout below changes: a file in another layout is ignored
// rather than misread.
inline constexpr std::uint32_t formatVersion = 1;

inline constexpr int eventSlots = 128;
inline constexpr int eventTextBytes = 188;
inline constexpr int contextSlots = 16;
inline constexpr int contextAreaBytes = 28;
inline constexpr int contextValueBytes = 224;
inline constexpr int threadSlots = 48;
inline constexpr int threadNameBytes = 24;
inline constexpr int threadAreaBytes = 28;
inline constexpr int threadActivityBytes = 96;

enum SessionState : std::uint32_t {
    StateNone = 0,
    // Set once the process is up; still set on the next launch means the
    // process ended without a handler getting to run.
    StateRunning = 1,
    StateCleanExit = 2,
    // A handler ran and wrote `reportPath`.
    StateCrashed = 3,
};

// One recent event. `sequence` is odd while the slot is being written and
// 2 * (n + 1) once event number n is complete in it, so a reader can tell a
// finished line from a torn or recycled one without taking a lock.
struct Event {
    std::atomic<std::uint32_t> sequence;
    char text[eventTextBytes];
};

// A standing fact about a subsystem ("screen share" -> which API, which
// source). Odd sequence while being written.
struct Context {
    std::atomic<std::uint32_t> sequence;
    char area[contextAreaBytes];
    char value[contextValueBytes];
};

// What one thread is doing right now. `threadId` 0 means the slot is free.
struct Thread {
    std::atomic<std::uint64_t> threadId;
    std::atomic<std::uint32_t> sequence;
    char name[threadNameBytes];
    char area[threadAreaBytes];
    char activity[threadActivityBytes];
};

struct Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t dataSize;
    std::atomic<std::uint32_t> state;
    std::uint64_t processId;
    std::int64_t startedMs;
    // The main thread's last sign of life, in UTC milliseconds. The hang
    // detector compares against it; the next launch reads it to tell how long
    // a frozen session was left before somebody killed it.
    std::atomic<std::int64_t> heartbeatMs;
    // Nonzero while the main thread is judged unresponsive.
    std::atomic<std::int64_t> unresponsiveSinceMs;
    std::atomic<std::uint64_t> mainThreadId;
    std::atomic<std::uint32_t> nextEvent;
    std::uint32_t reserved;
    char application[64];
    char build[96];
    char system[128];
    // UTF-8, written by whichever handler produced a report.
    char reportPath[512];
};

struct Data {
    Header header;
    Event events[eventSlots];
    Context contexts[contextSlots];
    Thread threads[threadSlots];
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t));

// The recorder in use: a static block until CrashReporter maps the file, the
// mapping afterwards. Never null.
[[nodiscard]] Data *current() noexcept;
// Copies everything recorded so far into `mapped` (whose contents are
// discarded) and records into it from now on. The mapping must outlive the
// process.
void adopt(Data *mapped) noexcept;

// The calling thread's identifier as the handlers see it: the Win32 thread id,
// the kernel tid on Linux, the Mach thread id on macOS. Async-signal-safe.
[[nodiscard]] std::uint64_t currentThreadId() noexcept;
// UTC milliseconds since the epoch. Async-signal-safe.
[[nodiscard]] std::int64_t nowMs() noexcept;

// Bounded copy that always terminates `destination`, never splits a UTF-8
// sequence, and never reads past `length` source bytes.
void copyText(char *destination, int capacity, const char *source, int length) noexcept;

} // namespace OpenChat::BlackBoxData
