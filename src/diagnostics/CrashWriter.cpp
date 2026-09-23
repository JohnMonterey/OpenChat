#include "diagnostics/CrashWriter.h"

#include <atomic>
#include <cstring>

namespace OpenChat::CrashWriter {

using namespace BlackBoxData;

Writer &Writer::text(const char *value) noexcept
{
    return value == nullptr ? *this : text(value, std::strlen(value));
}

Writer &Writer::text(const char *value, std::size_t length) noexcept
{
    if (value == nullptr)
        return *this;
    while (length > 0) {
        if (m_used == sizeof(m_buffer))
            flush();
        const std::size_t room = sizeof(m_buffer) - m_used;
        const std::size_t chunk = length < room ? length : room;
        std::memcpy(m_buffer + m_used, value, chunk);
        m_used += chunk;
        value += chunk;
        length -= chunk;
    }
    return *this;
}

Writer &Writer::field(const char *value, std::size_t capacity) noexcept
{
    if (value == nullptr)
        return *this;
    for (std::size_t index = 0; index < capacity && value[index] != '\0'; ++index) {
        const char c = value[index];
        const bool control = static_cast<unsigned char>(c) < 0x20;
        text(control ? "?" : &c, 1);
    }
    return *this;
}

Writer &Writer::decimal(std::int64_t value) noexcept
{
    if (value < 0) {
        text("-", 1);
        // Through unsigned so the most negative value does not overflow.
        return unsignedDecimal(~std::uint64_t(value) + 1);
    }
    return unsignedDecimal(std::uint64_t(value));
}

Writer &Writer::unsignedDecimal(std::uint64_t value) noexcept
{
    char digits[20];
    int count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0)
        text(&digits[--count], 1);
    return *this;
}

Writer &Writer::hex(std::uint64_t value, int digits) noexcept
{
    static const char alphabet[] = "0123456789abcdef";
    char buffer[16];
    int count = 0;
    do {
        buffer[count++] = alphabet[value & 0xF];
        value >>= 4;
    } while (value != 0);
    text("0x", 2);
    for (int pad = count; pad < digits && pad < 16; ++pad)
        text("0", 1);
    while (count > 0)
        text(&buffer[--count], 1);
    return *this;
}

Writer &Writer::twoDigits(int value) noexcept
{
    const char pair[2] = {char('0' + (value / 10) % 10), char('0' + value % 10)};
    return text(pair, 2);
}

void Writer::flush() noexcept
{
    if (m_used > 0 && m_sink != nullptr)
        m_sink(m_context, m_buffer, m_used);
    m_used = 0;
}

void utcTime(Writer &writer, std::int64_t epochMs) noexcept
{
    std::int64_t seconds = epochMs / 1000;
    std::int64_t days = seconds / 86400;
    std::int64_t secondOfDay = seconds % 86400;
    if (secondOfDay < 0) {
        secondOfDay += 86400;
        --days;
    }
    // Howard Hinnant's civil_from_days.
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t dayOfEra = z - era * 146097;
    const std::int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t mp = (5 * dayOfYear + 2) / 153;
    const int day = int(dayOfYear - (153 * mp + 2) / 5 + 1);
    const int month = int(mp < 10 ? mp + 3 : mp - 9);
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
    writer.decimal(year).text("-").twoDigits(month).text("-").twoDigits(day).text(" ");
    writer.twoDigits(int(secondOfDay / 3600))
        .text(":")
        .twoDigits(int(secondOfDay / 60 % 60))
        .text(":")
        .twoDigits(int(secondOfDay % 60))
        .text(" UTC");
}

void fileStamp(Writer &writer, std::int64_t epochMs) noexcept
{
    // Rendered through utcTime into a scratch buffer, then stripped of its
    // separators: one calendar implementation, two layouts.
    char scratch[40];
    MemorySink sink{scratch, sizeof(scratch), 0};
    {
        Writer temporary(MemorySink::write, &sink);
        utcTime(temporary, epochMs);
    }
    // "2026-09-23 12:05:13 UTC" -> "20260923-120513": the first 19 characters.
    for (std::size_t index = 0; index < sink.used && index < 19; ++index) {
        const char c = scratch[index];
        if (c >= '0' && c <= '9')
            writer.text(&c, 1);
        else if (c == ' ')
            writer.text("-", 1);
    }
}

void MemorySink::write(void *context, const char *data, std::size_t size) noexcept
{
    auto *sink = static_cast<MemorySink *>(context);
    if (sink->capacity == 0)
        return;
    const std::size_t room = sink->capacity - 1 - sink->used;
    const std::size_t count = size < room ? size : room;
    std::memcpy(sink->buffer + sink->used, data, count);
    sink->used += count;
    sink->buffer[sink->used] = '\0';
}

void duration(Writer &writer, std::int64_t milliseconds) noexcept
{
    if (milliseconds < 0)
        milliseconds = 0;
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = totalSeconds / 60 % 60;
    const std::int64_t seconds = totalSeconds % 60;
    if (hours > 0)
        writer.decimal(hours).text(" h ");
    if (hours > 0 || minutes > 0)
        writer.decimal(minutes).text(" min ");
    writer.decimal(seconds).text(" s");
}

namespace {

// Copies one seqlock-guarded thread slot out consistently, or reports that it
// kept changing underneath the reader.
struct ThreadCopy {
    std::uint64_t id = 0;
    char name[threadNameBytes] = {};
    char area[threadAreaBytes] = {};
    char activity[threadActivityBytes] = {};
};

bool copyThread(const Thread &thread, ThreadCopy &out) noexcept
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = thread.sequence.load(std::memory_order_acquire);
        out.id = thread.threadId.load(std::memory_order_acquire);
        std::memcpy(out.name, thread.name, sizeof(out.name));
        std::memcpy(out.area, thread.area, sizeof(out.area));
        std::memcpy(out.activity, thread.activity, sizeof(out.activity));
        // Keeps the copies above from being reordered after the second read.
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t after = thread.sequence.load(std::memory_order_relaxed);
        out.name[sizeof(out.name) - 1] = '\0';
        out.area[sizeof(out.area) - 1] = '\0';
        out.activity[sizeof(out.activity) - 1] = '\0';
        if (before == after && (before & 1u) == 0)
            return true;
    }
    // Still printed: a torn line is better than none in a crash report.
    return false;
}

bool equalsIgnoringCase(const char *a, const char *b) noexcept
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        char x = *a;
        char y = *b;
        if (x >= 'A' && x <= 'Z')
            x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z')
            y = char(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return *a == *b;
}

bool startsWithIgnoringCase(const char *value, const char *prefix) noexcept
{
    for (; *prefix != '\0'; ++value, ++prefix) {
        char x = *value;
        if (x == '\0')
            return false;
        if (x >= 'A' && x <= 'Z')
            x = char(x - 'A' + 'a');
        if (x != *prefix)
            return false;
    }
    return true;
}

} // namespace

bool contextValue(const Data &data, const char *area, char *out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0)
        return false;
    out[0] = '\0';
    for (const Context &context : data.contexts) {
        if (std::strncmp(context.area, area, contextAreaBytes) != 0)
            continue;
        const std::size_t limit = capacity - 1 < std::size_t(contextValueBytes)
            ? capacity - 1
            : std::size_t(contextValueBytes);
        std::memcpy(out, context.value, limit);
        out[limit] = '\0';
        return out[0] != '\0';
    }
    return false;
}

bool threadActivity(const Data &data, std::uint64_t threadId, char *out,
                    std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0)
        return false;
    out[0] = '\0';
    for (const Thread &thread : data.threads) {
        ThreadCopy copy;
        copyThread(thread, copy);
        if (copy.id != threadId || copy.id == 0)
            continue;
        if (copy.activity[0] == '\0')
            return false;
        std::size_t used = 0;
        auto append = [&](const char *value) {
            for (; *value != '\0' && used + 1 < capacity; ++value)
                out[used++] = *value;
            out[used] = '\0';
        };
        if (copy.area[0] != '\0') {
            append(copy.area);
            append(": ");
        }
        append(copy.activity);
        return true;
    }
    return false;
}

bool threadName(const Data &data, std::uint64_t threadId, char *out, std::size_t capacity) noexcept
{
    if (out == nullptr || capacity == 0)
        return false;
    out[0] = '\0';
    for (const Thread &thread : data.threads) {
        ThreadCopy copy;
        copyThread(thread, copy);
        if (copy.id != threadId || copy.id == 0 || copy.name[0] == '\0')
            continue;
        std::size_t used = 0;
        for (const char *c = copy.name; *c != '\0' && used + 1 < capacity; ++c)
            out[used++] = *c;
        out[used] = '\0';
        return true;
    }
    return false;
}

void blackBoxSections(Writer &writer, const Data &data, std::uint64_t crashedThreadId,
                      const char *marker) noexcept
{
    writer.newline().text("What OpenChat was set up to do").newline();
    writer.text("------------------------------").newline();
    bool anyContext = false;
    for (const Context &context : data.contexts) {
        char area[contextAreaBytes];
        char value[contextValueBytes];
        std::memcpy(area, context.area, sizeof(area));
        std::memcpy(value, context.value, sizeof(value));
        area[sizeof(area) - 1] = '\0';
        value[sizeof(value) - 1] = '\0';
        if (area[0] == '\0')
            continue;
        anyContext = true;
        writer.text("  ").field(area, sizeof(area)).text(": ").field(value, sizeof(value)).newline();
    }
    if (!anyContext)
        writer.text("  (nothing in particular: no call, no screen share)").newline();

    writer.newline().text("Threads").newline().text("-------").newline();
    bool anyThread = false;
    for (const Thread &thread : data.threads) {
        ThreadCopy copy;
        const bool consistent = copyThread(thread, copy);
        if (copy.id == 0)
            continue;
        anyThread = true;
        const bool crashed = copy.id == crashedThreadId;
        writer.text(crashed ? "  * " : "    ");
        if (copy.name[0] != '\0')
            writer.field(copy.name, sizeof(copy.name)).text(" ");
        writer.text("(id ").unsignedDecimal(copy.id).text("): ");
        if (copy.activity[0] == '\0') {
            writer.text("nothing recorded");
        } else {
            if (copy.area[0] != '\0')
                writer.field(copy.area, sizeof(copy.area)).text(": ");
            writer.field(copy.activity, sizeof(copy.activity));
        }
        if (!consistent)
            writer.text(" [was changing]");
        if (crashed)
            writer.text("   <-- this thread ").text(marker);
        writer.newline();
    }
    if (!anyThread)
        writer.text("  (none described)").newline();

    writer.newline().text("Recent events, oldest first").newline();
    writer.text("---------------------------").newline();
    const std::uint32_t next = data.header.nextEvent.load(std::memory_order_acquire);
    const std::uint32_t first = next > std::uint32_t(eventSlots) ? next - eventSlots : 0;
    bool anyEvent = false;
    for (std::uint32_t number = first; number != next; ++number) {
        const Event &event = data.events[number % eventSlots];
        if (event.sequence.load(std::memory_order_acquire) != 2 * number + 2)
            continue;
        char text[eventTextBytes];
        std::memcpy(text, event.text, sizeof(text));
        std::atomic_thread_fence(std::memory_order_acquire);
        // Overwritten by a newer event while being copied: skipped, not torn.
        if (event.sequence.load(std::memory_order_relaxed) != 2 * number + 2)
            continue;
        text[sizeof(text) - 1] = '\0';
        anyEvent = true;
        writer.text("  ").field(text, sizeof(text)).newline();
    }
    if (!anyEvent)
        writer.text("  (none)").newline();
}

const char *baseName(const char *path) noexcept
{
    if (path == nullptr)
        return "";
    const char *base = path;
    for (const char *c = path; *c != '\0'; ++c) {
        if (*c == '/' || *c == '\\')
            base = c + 1;
    }
    return base;
}

const char *moduleHint(const char *module) noexcept
{
    if (module == nullptr || *module == '\0')
        return nullptr;
    struct Rule {
        const char *prefix;
        const char *hint;
    };
    static const Rule rules[] = {
        {"nvwgf2um", "The crash happened inside the NVIDIA graphics driver. Updating the graphics "
                     "driver usually fixes this."},
        {"nvd3dum", "The crash happened inside the NVIDIA graphics driver. Updating the graphics "
                    "driver usually fixes this."},
        {"nvoglv", "The crash happened inside the NVIDIA graphics driver. Updating the graphics "
                   "driver usually fixes this."},
        {"nvldumd", "The crash happened inside the NVIDIA graphics driver. Updating the graphics "
                    "driver usually fixes this."},
        {"igd", "The crash happened inside the Intel graphics driver. Updating the graphics "
                "driver usually fixes this."},
        {"igx", "The crash happened inside the Intel graphics driver. Updating the graphics "
                "driver usually fixes this."},
        {"igc64", "The crash happened inside the Intel graphics driver. Updating the graphics "
                  "driver usually fixes this."},
        {"ig7icd", "The crash happened inside the Intel graphics driver. Updating the graphics "
                   "driver usually fixes this."},
        {"ig9icd", "The crash happened inside the Intel graphics driver. Updating the graphics "
                   "driver usually fixes this."},
        {"atidxx", "The crash happened inside the AMD graphics driver. Updating the graphics "
                   "driver usually fixes this."},
        {"atiumd", "The crash happened inside the AMD graphics driver. Updating the graphics "
                   "driver usually fixes this."},
        {"atio6axx", "The crash happened inside the AMD graphics driver. Updating the graphics "
                     "driver usually fixes this."},
        {"amdxx", "The crash happened inside the AMD graphics driver. Updating the graphics "
                  "driver usually fixes this."},
        {"amdxc", "The crash happened inside the AMD graphics driver. Updating the graphics "
                  "driver usually fixes this."},
        {"d3d11", "The crash happened inside Direct3D, Windows' graphics layer. This is almost "
                  "always the graphics driver underneath it; updating the driver usually fixes it."},
        {"dxgi", "The crash happened inside DXGI, Windows' display layer. This is almost always "
                 "the graphics driver underneath it; updating the driver usually fixes it."},
        {"d3d10warp", "The crash happened inside Windows' software renderer (WARP)."},
        {"graphicscapture", "The crash happened inside Windows' own screen capture service."},
        {"rtsshooks", "The crash happened inside RivaTuner Statistics Server's overlay, which it "
                      "injects into other programs. Try closing RivaTuner or excluding OpenChat."},
        {"gameoverlayrenderer", "The crash happened inside the Steam overlay, which Steam injects "
                                "into other programs. Try disabling the Steam overlay."},
        {"discordhook", "The crash happened inside Discord's overlay, which Discord injects into "
                        "other programs. Try disabling Discord's in-game overlay."},
        {"graphics-hook", "The crash happened inside OBS's game-capture hook, which OBS injects "
                          "into other programs."},
        {"screencapturekit", "The crash happened inside macOS's ScreenCaptureKit framework."},
        {"skylight", "The crash happened inside macOS's window server framework (SkyLight)."},
        {"agxmetal", "The crash happened inside the Apple GPU driver."},
        {"libnvidia", "The crash happened inside the NVIDIA graphics driver. Updating the graphics "
                      "driver usually fixes this."},
        {"libgallium", "The crash happened inside the Mesa graphics driver."},
        {"iris_dri", "The crash happened inside the Mesa graphics driver."},
        {"radeonsi_dri", "The crash happened inside the Mesa graphics driver."},
        {"libpipewire", "The crash happened inside PipeWire, the desktop's screen capture service."},
    };
    for (const Rule &rule : rules) {
        if (startsWithIgnoringCase(module, rule.prefix))
            return rule.hint;
    }
    if (equalsIgnoringCase(module, "ntdll.dll") || equalsIgnoringCase(module, "kernelbase.dll")) {
        return "The crash was reported by Windows itself (usually heap corruption or a "
               "deliberate abort); the stack below shows who called in.";
    }
    return nullptr;
}

} // namespace OpenChat::CrashWriter
