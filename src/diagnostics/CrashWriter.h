#pragma once

#include "diagnostics/BlackBoxData.h"

#include <cstddef>
#include <cstdint>

// What a crash handler writes a report with.
//
// A handler runs on a process that is already broken: the heap may be
// corrupt, another thread may hold the allocator's or the locale's lock, and on
// POSIX it is inside a signal handler. So nothing here allocates, locks, or
// touches stdio, locale or Qt — it formats into a fixed buffer and hands full
// buffers to a platform callback that writes them with a raw system call.
namespace OpenChat::CrashWriter {

class Writer final
{
public:
    using Sink = void (*)(void *context, const char *data, std::size_t size);

    Writer(Sink sink, void *context) noexcept
        : m_sink(sink)
        , m_context(context)
    {
    }
    ~Writer() { flush(); }

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    Writer &text(const char *value) noexcept;
    Writer &text(const char *value, std::size_t length) noexcept;
    // Text read out of the black box: stops at the terminator or `capacity`,
    // and replaces control characters so a torn slot cannot break the layout.
    Writer &field(const char *value, std::size_t capacity) noexcept;
    Writer &newline() noexcept { return text("\n", 1); }
    Writer &decimal(std::int64_t value) noexcept;
    Writer &unsignedDecimal(std::uint64_t value) noexcept;
    // "0x" and at least `digits` hexadecimal digits.
    Writer &hex(std::uint64_t value, int digits = 1) noexcept;
    // Two-digit zero-padded decimal, for clock fields.
    Writer &twoDigits(int value) noexcept;
    void flush() noexcept;

private:
    char m_buffer[4096];
    std::size_t m_used = 0;
    Sink m_sink;
    void *m_context;
};

// "2026-09-23 12:05:13 UTC", computed by hand: gmtime is not signal-safe.
void utcTime(Writer &writer, std::int64_t epochMs) noexcept;
// "1 h 5 min 12 s".
void duration(Writer &writer, std::int64_t milliseconds) noexcept;
// "20260923-120513", UTC, for file names.
void fileStamp(Writer &writer, std::int64_t epochMs) noexcept;

// A Writer's sink that fills a fixed character array, always terminated.
struct MemorySink final {
    char *buffer;
    std::size_t capacity;
    std::size_t used;
    static void write(void *context, const char *data, std::size_t size) noexcept;
};

// The value of a black box context (e.g. "fatal"), copied out into `out`.
// False when it is not set.
bool contextValue(const BlackBoxData::Data &data, const char *area, char *out,
                  std::size_t capacity) noexcept;

// "area: activity" of one thread into `out`. False when the thread is not
// described or is doing nothing in particular.
bool threadActivity(const BlackBoxData::Data &data, std::uint64_t threadId, char *out,
                    std::size_t capacity) noexcept;
// The recorded name of a thread, or false.
bool threadName(const BlackBoxData::Data &data, std::uint64_t threadId, char *out,
                std::size_t capacity) noexcept;

// The three sections every report shares: what each subsystem was set up to
// do, what every described thread was doing (the crashed one marked), and the
// ring of recent events, oldest first.
// `marker` labels that thread ("crashed", "froze").
void blackBoxSections(Writer &writer, const BlackBoxData::Data &data,
                      std::uint64_t crashedThreadId, const char *marker = "crashed") noexcept;

// A module's file name without its directory, for "Where:" lines.
const char *baseName(const char *path) noexcept;

// A plain-language hint for a crash inside a module this application does not
// own, or null. Graphics drivers are by far the most common case for anything
// that captures or renders a screen.
const char *moduleHint(const char *moduleBaseName) noexcept;

} // namespace OpenChat::CrashWriter
