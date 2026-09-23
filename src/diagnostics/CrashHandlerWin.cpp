#include "diagnostics/CrashHandlerPlatform.h"

#include "diagnostics/BlackBoxData.h"
#include "diagnostics/CrashWriter.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

// The structured-exception filter does almost nothing itself: it hands the
// exception to a watcher thread started at install and waits. That thread has
// a healthy stack of its own — the crashed one may have none left — and
// writes the report, the minidump, and relaunches OpenChat to show it.
namespace OpenChat::CrashHandlerPlatform {

namespace {

using namespace BlackBoxData;
using CrashWriter::Writer;

// Our own exception codes, raised for failures that are not exceptions to
// Windows (abort, a pure virtual call, a bad CRT argument) so that every kind of
// death arrives at the same filter with a real CPU context.
constexpr DWORD abortCode = 0xE0C0AB01;
constexpr DWORD pureCallCode = 0xE0C0AB02;
constexpr DWORD invalidParameterCode = 0xE0C0AB03;
// GCC's C++ throw, as SEH sees it.
constexpr DWORD gccThrowCode = 0x20474343;
constexpr DWORD msvcThrowCode = 0xE06D7363;
constexpr DWORD heapCorruptionCode = 0xC0000374;
constexpr DWORD stackBufferOverrunCode = 0xC0000409;

Config g_config{};
wchar_t g_directory[1024] = {};
wchar_t g_executable[MAX_PATH] = {};
HANDLE g_wake = nullptr;
HANDLE g_done = nullptr;
HANDLE g_written = nullptr;
HANDLE g_watcher = nullptr;
DWORD g_watcherId = 0;
HANDLE g_mainThread = nullptr;
EXCEPTION_POINTERS *g_pointers = nullptr;
DWORD g_crashedThread = 0;
std::atomic<LONG> g_entered{0};
// Set when the stop comes from a Qt fatal error, whose stack has our message
// hook and Qt's fatal path between RaiseException and the code to blame.
std::atomic<bool> g_fatalFromQt{false};

using MiniDumpWriteDumpFunction = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                                  PMINIDUMP_EXCEPTION_INFORMATION,
                                                  PMINIDUMP_USER_STREAM_INFORMATION,
                                                  PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFunction g_miniDumpWriteDump = nullptr;

// Filled at report time. Static: nothing may be allocated then. One table
// for the crash watcher and one for the freeze detector, which can be writing
// at the same moment.
struct Module {
    std::uintptr_t base;
    std::uintptr_t size;
    DWORD timestamp;
    char name[MAX_PATH];
};
constexpr int maxModules = 768;
struct ModuleTable {
    Module modules[maxModules];
    int count;
};
ModuleTable g_crashModules;
ModuleTable g_freezeModules;

// The range a thread's stack may occupy; a walk that leaves it has met a
// corrupted frame and stops rather than reading wild memory.
struct StackBounds {
    std::uintptr_t low;
    std::uintptr_t high;
};
StackBounds g_crashedStack{};
StackBounds g_mainStack{};

void writeHandle(void *context, const char *data, std::size_t size) noexcept
{
    HANDLE file = *static_cast<HANDLE *>(context);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = size > (1u << 20) ? (1u << 20) : DWORD(size);
        if (!WriteFile(file, data, chunk, &written, nullptr) || written == 0)
            return;
        data += written;
        size -= written;
    }
}

void toUtf8(const wchar_t *source, char *out, int capacity) noexcept
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, source, -1, out, capacity, nullptr, nullptr);
    if (length <= 0)
        out[0] = '\0';
}

void toWide(const char *source, wchar_t *out, int capacity) noexcept
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, source, -1, out, capacity);
    if (length <= 0)
        out[0] = L'\0';
}

[[nodiscard]] bool readable(std::uintptr_t address, std::size_t size) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void *>(address), &info, sizeof(info)) == 0)
        return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address + size <= end;
}

// The loaded modules, read the way psapi does for any process: straight out of
// the loader's own lists, without taking the loader lock the crashed thread
// may be holding.
StackBounds currentStackBounds() noexcept
{
    using Limits = void(WINAPI *)(PULONG_PTR, PULONG_PTR);
    static const Limits limits = reinterpret_cast<Limits>(reinterpret_cast<void *>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadStackLimits")));
    StackBounds bounds{};
    if (limits != nullptr) {
        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        limits(&low, &high);
        bounds = {std::uintptr_t(low), std::uintptr_t(high)};
    }
    return bounds;
}

void snapshotModules(ModuleTable &table) noexcept
{
    static HMODULE crashHandles[maxModules];
    static HMODULE freezeHandles[maxModules];
    HMODULE *handles = &table == &g_crashModules ? crashHandles : freezeHandles;
    DWORD needed = 0;
    table.count = 0;
    HANDLE process = GetCurrentProcess();
    if (!EnumProcessModules(process, handles, DWORD(sizeof(HMODULE) * maxModules), &needed))
        return;
    const int listed = int(needed / sizeof(HMODULE));
    const int count = listed < maxModules ? listed : maxModules;
    for (int index = 0; index < count; ++index) {
        MODULEINFO info{};
        if (!GetModuleInformation(process, handles[index], &info, sizeof(info)))
            continue;
        Module &module = table.modules[table.count++];
        module.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
        module.size = info.SizeOfImage;
        module.timestamp = 0;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleFileNameExW(process, handles[index], path, MAX_PATH) == 0)
            path[0] = L'\0';
        char utf8[MAX_PATH * 3];
        toUtf8(path, utf8, int(sizeof(utf8)));
        const char *base = CrashWriter::baseName(utf8);
        BlackBoxData::copyText(module.name, int(sizeof(module.name)), base, int(std::strlen(base)));
        // The linker's timestamp, which is what matches a report to the exact
        // binary its symbols have to come from.
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module.base);
        if (readable(module.base, sizeof(IMAGE_DOS_HEADER)) && dos->e_magic == IMAGE_DOS_SIGNATURE
            && readable(module.base + std::uintptr_t(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS))) {
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module.base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE)
                module.timestamp = nt->FileHeader.TimeDateStamp;
        }
    }
}

const Module *moduleAt(const ModuleTable &table, std::uintptr_t address) noexcept
{
    for (int index = 0; index < table.count; ++index) {
        const Module &module = table.modules[index];
        if (address >= module.base && address < module.base + module.size)
            return &module;
    }
    return nullptr;
}

// The exported function at or below `rva`, which names a frame in any DLL that
// exports its code (Qt does, heavily). Every read is bounds-checked: the
// header being read belongs to a process that has just crashed.
const char *nearestExport(const Module &module, DWORD rva, DWORD &distance) noexcept
{
    const std::uintptr_t base = module.base;
    auto inside = [&](std::uintptr_t offset, std::size_t size) {
        return offset + size <= module.size && readable(base + offset, size);
    };
    if (!inside(0, sizeof(IMAGE_DOS_HEADER)))
        return nullptr;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || !inside(std::uintptr_t(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS)))
        return nullptr;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;
    const IMAGE_DATA_DIRECTORY &directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || !inside(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY)))
        return nullptr;
    const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + directory.VirtualAddress);
    if (!inside(exports->AddressOfFunctions, std::size_t(exports->NumberOfFunctions) * 4)
        || !inside(exports->AddressOfNames, std::size_t(exports->NumberOfNames) * 4)
        || !inside(exports->AddressOfNameOrdinals, std::size_t(exports->NumberOfNames) * 2)) {
        return nullptr;
    }
    const auto *functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
    const auto *names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
    const auto *ordinals = reinterpret_cast<const WORD *>(base + exports->AddressOfNameOrdinals);
    DWORD best = 0;
    const char *bestName = nullptr;
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const WORD ordinal = ordinals[index];
        if (ordinal >= exports->NumberOfFunctions)
            continue;
        const DWORD function = functions[ordinal];
        // Forwarders point into the export directory, not at code.
        if (function >= directory.VirtualAddress && function < directory.VirtualAddress + directory.Size)
            continue;
        if (function <= rva && function > best && inside(names[index], 1)) {
            best = function;
            bestName = reinterpret_cast<const char *>(base + names[index]);
        }
    }
    distance = rva - best;
    return bestName;
}

void writeAddress(Writer &out, const ModuleTable &table, std::uintptr_t address) noexcept
{
    const Module *module = moduleAt(table, address);
    if (module == nullptr) {
        out.text("(in no loaded module: a jump to a bad address)");
        return;
    }
    const DWORD rva = DWORD(address - module->base);
    out.text(module->name).text("+").hex(rva, 1);
    DWORD distance = 0;
    // A DLL's exports name most of its code (Qt exports nearly everything).
    // Far from one, or in the executable — which exports only a handful of
    // unrelated entry points — the nearest name would mislead, so none is
    // given; tools/windows/symbolize-crash.sh names those frames.
    const bool executable = module == &table.modules[0];
    const char *name = executable ? nullptr : nearestExport(*module, rva, distance);
    if (name != nullptr && distance <= 0x4000)
        out.text("  (near ").text(name).text("+").hex(distance, 1).text(")");
}

// Walks a stack from a CPU context with the unwind tables every x64 module
// carries — MinGW's included — so no symbols and no dbghelp are needed.
int collectFrames(CONTEXT context, const StackBounds &bounds, DWORD64 *frames,
                  int capacity) noexcept
{
    int count = 0;
#if defined(_M_X64) || defined(__x86_64__)
    for (int frame = 0; frame < capacity; ++frame) {
        const DWORD64 pc = context.Rip;
        // Address 0 ends a walk, except where the crash itself is: a call
        // through an empty pointer (or, in this build, to a pure virtual
        // function) lands there, and its caller is on top of the stack.
        if (pc == 0 && frame > 0)
            break;
        frames[count++] = pc;
        // The unwinder reads saved registers out of the frame; a stack
        // pointer outside the thread's stack, misaligned or unreadable is a
        // corrupted frame, and reading through it would fault in the crash
        // path itself.
        const std::uintptr_t sp = std::uintptr_t(context.Rsp);
        if ((bounds.high != 0 && (sp < bounds.low || sp >= bounds.high)) || (sp & 7) != 0
            || !readable(sp, 64)) {
            break;
        }
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (function == nullptr) {
            // A leaf function, or a jump into nowhere: the return address is
            // what sits on top of the stack.
            if (!readable(std::uintptr_t(context.Rsp), 8))
                break;
            context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        } else {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        }
        if (context.Rip == pc && frame > 0)
            break;
    }
#else
    (void)context;
    (void)frames;
    (void)capacity;
#endif
    return count;
}

void writeStack(Writer &out, const ModuleTable &table, const CONTEXT &context,
                const StackBounds &bounds) noexcept
{
    DWORD64 frames[64];
    const int count = collectFrames(context, bounds, frames, 64);
    if (count == 0) {
        out.text("  (no stack could be walked; see the minidump)").newline();
        return;
    }
    for (int frame = 0; frame < count; ++frame) {
        out.text("  #").decimal(frame).text(frame < 10 ? "   " : "  ").hex(frames[frame], 16).text("  ");
        writeAddress(out, table, std::uintptr_t(frames[frame]));
        out.newline();
    }
}

bool nameStartsWith(const char *name, const char *prefix) noexcept
{
    for (; *prefix != '\0'; ++name, ++prefix) {
        char c = *name;
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c != *prefix)
            return false;
    }
    return true;
}

// Where raise(), abort(), RaiseException and std::terminate live.
bool isRuntimeModule(const Module *module) noexcept
{
    if (module == nullptr)
        return false;
    static const char *const runtimes[] = {
        "kernelbase.dll", "ntdll.dll", "kernel32.dll", "ucrtbase", "msvcrt", "api-ms-win-",
        "libstdc++", "libgcc_s", "libwinpthread", "vcruntime", "msvcp",
    };
    for (const char *runtime : runtimes) {
        if (nameStartsWith(module->name, runtime))
            return true;
    }
    return false;
}

// For a deliberate stop (abort, terminate, a Qt fatal error) the faulting
// address is inside RaiseException; the frame to blame is the code that asked
// for the stop. Walking out from RaiseException: the runtime first (raise,
// abort); then, for a Qt fatal error, our message hook and Qt's own
// fatal-message path; for an uncaught exception, our terminate handler and the
// C++ runtime's unwinder that called it.
std::uintptr_t blamedFrame(const ModuleTable &table, const CONTEXT &context,
                           const StackBounds &bounds) noexcept
{
    DWORD64 frames[64];
    const int count = collectFrames(context, bounds, frames, 64);
    auto module = [&](int index) { return moduleAt(table, std::uintptr_t(frames[index])); };
    auto isQtCore = [&](int index) {
        const Module *found = module(index);
        return found != nullptr && nameStartsWith(found->name, "qt6core");
    };
    int index = 0;
    while (index < count && isRuntimeModule(module(index)))
        ++index;
    if (g_fatalFromQt.load()) {
        // Every installed message handler (the log file's, ours) chains into
        // the next, so there can be several of our frames before Qt's.
        while (index < count && module(index) == &table.modules[0])
            ++index;
        while (index < count && isQtCore(index))
            ++index;
    } else if (index + 1 < count && module(index) == &table.modules[0]
               && isRuntimeModule(module(index + 1))) {
        ++index;
        while (index < count && isRuntimeModule(module(index)))
            ++index;
    }
    return index < count ? std::uintptr_t(frames[index]) : 0;
}

// For a call into nothing: the code that made it, which is the first frame
// below the bad address that is in a loaded module. That is where the bug is.
std::uintptr_t callerOfBadJump(const ModuleTable &table, const CONTEXT &context,
                               const StackBounds &bounds) noexcept
{
    DWORD64 frames[8];
    const int count = collectFrames(context, bounds, frames, 8);
    for (int index = 1; index < count; ++index) {
        if (moduleAt(table, std::uintptr_t(frames[index])) != nullptr)
            return std::uintptr_t(frames[index]);
    }
    return 0;
}

void describeException(Writer &out, const EXCEPTION_RECORD &record, const Data &data) noexcept
{
    const ULONG_PTR kind = record.NumberParameters >= 1 ? record.ExceptionInformation[0] : 0;
    const ULONG_PTR address = record.NumberParameters >= 2 ? record.ExceptionInformation[1] : 0;
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        out.text(kind == 1   ? "The program tried to write to memory at "
                 : kind == 8 ? "The program tried to run code at "
                             : "The program tried to read memory at ")
            .hex(address, 1);
        if (record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
            out.text(", but the page could not be read in from disk or the network.");
        // This build never links the C++ runtime's handler for a pure virtual
        // call; every reference to it stays at address 0. A pure call through
        // a vtable therefore lands near 0, and one the compiler turned into a
        // direct call lands just below the program. Both look like a jump into
        // nothing, so say what they usually are.
        else if (kind == 8 && address < 0x10000)
            out.text(", which is not valid: a call through an empty function pointer, or to a "
                     "pure virtual function (this build leaves those at address 0).");
        else if (kind == 8 && moduleAt(g_crashModules, std::uintptr_t(address)) == nullptr)
            out.text(", where nothing is loaded: a call to a function that was never linked "
                     "into this build (such as a pure virtual function the compiler called "
                     "directly), or through a corrupted pointer. \"Where\" is the code that made "
                     "the call.");
        else if (address < 0x10000)
            out.text(", which is not valid: a null pointer.");
        else if (kind == 8)
            out.text(", which is not code (data execution prevention stopped it).");
        else
            out.text(", which it may not use (freed, never allocated, or protected).");
        return;
    case EXCEPTION_STACK_OVERFLOW:
        out.text("The program ran out of stack space (a runaway recursion).");
        return;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        out.text("An integer was divided by zero.");
        return;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        out.text("The processor met an instruction it cannot run (this build uses an instruction "
                 "this CPU lacks, or code was corrupted).");
        return;
    case EXCEPTION_BREAKPOINT:
        out.text("A breakpoint or a failed check inside the code was reached.");
        return;
    case heapCorruptionCode:
        out.text("Windows found that the program's memory heap was corrupted.");
        return;
    case stackBufferOverrunCode:
        out.text("Windows stopped the program: a buffer on the stack was overrun, or a fatal "
                 "check failed.");
        return;
    case pureCallCode:
        out.text("A pure virtual function was called: an object was used while it was being "
                 "destroyed.");
        return;
    case invalidParameterCode:
        out.text("The C runtime was handed an invalid argument.");
        return;
    case gccThrowCode:
    case msvcThrowCode:
        out.text("A C++ exception was thrown and nothing caught it.");
        return;
    case abortCode: {
        char reason[contextValueBytes];
        if (CrashWriter::contextValue(data, "fatal", reason, sizeof(reason)))
            out.text("OpenChat stopped itself on purpose: ").field(reason, sizeof(reason));
        else
            out.text("OpenChat stopped itself on purpose (abort) without recording why; the "
                     "stack below shows where.");
        return;
    }
    default:
        out.text("The program raised exception ").hex(record.ExceptionCode, 8).text(".");
        return;
    }
}

void writeRegisters(Writer &out, const CONTEXT &context) noexcept
{
#if defined(_M_X64) || defined(__x86_64__)
    struct Register {
        const char *name;
        DWORD64 value;
    };
    const Register registers[] = {
        {"rax", context.Rax}, {"rbx", context.Rbx}, {"rcx", context.Rcx}, {"rdx", context.Rdx},
        {"rsi", context.Rsi}, {"rdi", context.Rdi}, {"rbp", context.Rbp}, {"rsp", context.Rsp},
        {"r8", context.R8},   {"r9", context.R9},   {"r10", context.R10}, {"r11", context.R11},
        {"r12", context.R12}, {"r13", context.R13}, {"r14", context.R14}, {"r15", context.R15},
        {"rip", context.Rip},
    };
    int column = 0;
    for (const Register &reg : registers) {
        out.text("  ").text(reg.name).text(reg.name[2] == '\0' ? "  " : " ").hex(reg.value, 16);
        if (++column % 4 == 0)
            out.newline();
    }
    if (column % 4 != 0)
        out.newline();
#else
    (void)out;
    (void)context;
#endif
}

void writeModules(Writer &out, const ModuleTable &table) noexcept
{
    for (int index = 0; index < table.count; ++index) {
        const Module &module = table.modules[index];
        out.text("  ").hex(module.base, 16).text("  size ").hex(module.size, 8).text("  stamp ")
            .hex(module.timestamp, 8).text("  ").text(module.name).newline();
    }
}

// "<directory>\<prefix>-20260923-120513-4321<extension>", UTF-8 and UTF-16.
void buildPath(char *utf8, int utf8Capacity, wchar_t *wide, int wideCapacity, const char *prefix,
               std::int64_t epochMs, const char *extension) noexcept
{
    CrashWriter::MemorySink sink{utf8, std::size_t(utf8Capacity), 0};
    utf8[0] = '\0';
    {
        Writer writer(CrashWriter::MemorySink::write, &sink);
        writer.text(g_config.directory).text("\\").text(prefix).text("-");
        CrashWriter::fileStamp(writer, epochMs);
        writer.text("-").unsignedDecimal(GetCurrentProcessId()).text(extension);
    }
    toWide(utf8, wide, wideCapacity);
}

void announce(const char *reportPath, const char *summary) noexcept
{
    // Whoever launched it from a console sees it there too.
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    if (console != nullptr && console != INVALID_HANDLE_VALUE) {
        Writer line(writeHandle, &console);
        line.text("\r\nOpenChat crashed. Report: ").text(reportPath).text("\r\n");
    }
    if (g_config.relaunch && g_executable[0] != L'\0') {
        static wchar_t command[4096];
        static wchar_t widePath[1024];
        toWide(reportPath, widePath, 1024);
        command[0] = L'\0';
        lstrcpynW(command, L"\"", 4096);
        lstrcatW(command, g_executable);
        lstrcatW(command, L"\" --crash-report \"");
        lstrcatW(command, widePath);
        lstrcatW(command, L"\"");
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(g_executable, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            return;
        }
    }
    // Unattended (a preview, a capture, a test): the report on disk is enough,
    // and a message box would only hold the run up until it timed out.
    if (!g_config.interactive)
        return;
    // No viewer (it is the viewer that crashed, or it could not start): the
    // plainest thing Windows can show.
    static wchar_t message[4096];
    toWide(summary, message, 4096);
    MessageBoxW(nullptr, message, L"OpenChat crashed",
                MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

void writeMinidump(const wchar_t *path) noexcept
{
    if (g_miniDumpWriteDump == nullptr)
        return;
    HANDLE dump = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = g_crashedThread;
    exception.ExceptionPointers = g_pointers;
    exception.ClientPointers = FALSE;
    const auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory
                                    | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, type, &exception,
                        nullptr, nullptr);
    CloseHandle(dump);
}

// Written in order of how much each part is worth against how likely it is to
// fault on a broken process: the summary and the black box first, the state
// that tells the next launch a report exists, the minidump, and only then the
// stack walk, which reads the crashed thread's possibly corrupted stack.
void writeCrashReport() noexcept
{
    Data &data = *current();
    const EXCEPTION_RECORD &record = *g_pointers->ExceptionRecord;
    const CONTEXT &context = *g_pointers->ContextRecord;
    const std::int64_t now = nowMs();
    static char reportPath[1024];
    static wchar_t reportWide[1024];
    static char dumpPath[1024];
    static wchar_t dumpWide[1024];
    buildPath(reportPath, 1024, reportWide, 1024, "crash", now, ".txt");
    buildPath(dumpPath, 1024, dumpWide, 1024, "crash", now, ".dmp");

    // What goes in the message box if the viewer cannot be started.
    static char summary[2048];
    CrashWriter::MemorySink summarySink{summary, sizeof(summary), 0};

    HANDLE file = CreateFileW(reportWide, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        file = GetStdHandle(STD_ERROR_HANDLE);
    } else {
        BlackBoxData::copyText(data.header.reportPath, int(sizeof(data.header.reportPath)),
                               reportPath, int(std::strlen(reportPath)));
        data.header.state.store(StateCrashed);
    }
    snapshotModules(g_crashModules);
    const std::uintptr_t faultAddress = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
    {
        Writer out(writeHandle, &file);
        Writer brief(CrashWriter::MemorySink::write, &summarySink);
        out.text("OpenChat crash report").newline();
        out.text("=====================").newline().newline();
        out.text("What happened:  ");
        describeException(out, record, data);
        brief.text("OpenChat crashed.\n\n");
        describeException(brief, record, data);
        out.flush();

        const bool deliberate = record.ExceptionCode == abortCode
            || record.ExceptionCode == pureCallCode || record.ExceptionCode == invalidParameterCode
            || record.ExceptionCode == gccThrowCode || record.ExceptionCode == msvcThrowCode;
        std::uintptr_t blamed = deliberate ? blamedFrame(g_crashModules, context, g_crashedStack) : 0;
        // A jump into nothing has no "where" of its own; the caller does.
        const bool intoNothing = moduleAt(g_crashModules, faultAddress) == nullptr;
        if (blamed == 0 && intoNothing)
            blamed = callerOfBadJump(g_crashModules, context, g_crashedStack);
        if (blamed == 0)
            blamed = faultAddress;
        const Module *faultModule = moduleAt(g_crashModules, blamed);
        out.newline().text("Where:          ");
        writeAddress(out, g_crashModules, blamed);
        if (intoNothing && blamed != faultAddress)
            out.text(", calling ").hex(faultAddress, 1).text(" where nothing is loaded");
        out.newline().text("Thread:         ");
        char name[threadNameBytes];
        if (CrashWriter::threadName(data, g_crashedThread, name, sizeof(name)))
            out.text(name).text(" ");
        out.text("(id ").unsignedDecimal(g_crashedThread).text(")").newline();
        char doing[256];
        const bool known = CrashWriter::threadActivity(data, g_crashedThread, doing, sizeof(doing));
        out.text("Doing:          ")
            .text(known ? doing : "nothing that OpenChat records on this thread")
            .newline();
        if (known)
            brief.text("\n\nWhile: ").text(doing);
        if (const char *hint = CrashWriter::moduleHint(faultModule ? faultModule->name : nullptr)) {
            out.text("Hint:           ").text(hint).newline();
            brief.text("\n\n").text(hint);
        }
        brief.text("\n\nA report was saved to:\n").text(reportPath);
        brief.flush();
        out.newline();
        out.text("Application:    ").field(data.header.application, sizeof(data.header.application)).newline();
        out.text("Build:          ").field(data.header.build, sizeof(data.header.build)).newline();
        out.text("System:         ").field(data.header.system, sizeof(data.header.system)).newline();
        out.text("Started:        ");
        CrashWriter::utcTime(out, data.header.startedMs);
        out.newline().text("Crashed:        ");
        CrashWriter::utcTime(out, now);
        out.text(" (after ");
        CrashWriter::duration(out, now - data.header.startedMs);
        out.text(")").newline();
        out.text("Exception:      ").hex(record.ExceptionCode, 8).text(" at ").hex(faultAddress, 16)
            .newline();
        out.text("Process id:     ").unsignedDecimal(GetCurrentProcessId()).newline();
        out.text("Minidump:       ").text(dumpPath).newline();

        CrashWriter::blackBoxSections(out, data, g_crashedThread);
        out.newline().text("Registers").newline().text("---------").newline();
        writeRegisters(out, context);
        out.newline().text("Loaded modules (base, size, link timestamp, name)").newline();
        out.text("-------------------------------------------------").newline();
        writeModules(out, g_crashModules);
        out.flush();

        writeMinidump(dumpWide);

        out.newline().text("Stack of the crashed thread").newline();
        out.text("---------------------------").newline();
        out.flush();
        writeStack(out, g_crashModules, context, g_crashedStack);
    }
    if (file != GetStdHandle(STD_ERROR_HANDLE)) {
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    SetEvent(g_written);
    announce(reportPath, summary);
}

DWORD WINAPI watcherMain(void *)
{
    WaitForSingleObject(g_wake, INFINITE);
    writeCrashReport();
    SetEvent(g_done);
    return 0;
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS *pointers)
{
    // The watcher itself faulted while writing: what is on disk is all there
    // will be.
    if (GetCurrentThreadId() == g_watcherId)
        TerminateProcess(GetCurrentProcess(), pointers->ExceptionRecord->ExceptionCode);
    if (g_entered.exchange(1) != 0) {
        // A second thread crashing while the first is being reported waits for
        // that report; the process ends with it.
        if (g_done != nullptr)
            WaitForSingleObject(g_done, 60000);
        TerminateProcess(GetCurrentProcess(), pointers->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    g_pointers = pointers;
    g_crashedThread = GetCurrentThreadId();
    g_crashedStack = currentStackBounds();
    const bool watcherAlive = g_watcher != nullptr && WaitForSingleObject(g_watcher, 0) == WAIT_TIMEOUT;
    if (watcherAlive) {
        SetEvent(g_wake);
        // A minute for the report itself: a watcher still writing after that
        // is stuck on a lock the crashed code holds. Then up to ten minutes for
        // whatever it shows — the fallback message box waits for the user.
        if (WaitForSingleObject(g_written, 60000) == WAIT_OBJECT_0)
            WaitForSingleObject(g_done, 600000);
    } else {
        // Shutdown already stopped the watcher: write from here, on what
        // stack is left.
        writeCrashReport();
    }
    TerminateProcess(GetCurrentProcess(), pointers->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

// A stack overflow is taken first-chance, on the faulting thread, while the
// guaranteed reserve is still there: by the time it would reach the
// unhandled-exception filter, some systems cannot even suspend the thread.
LONG WINAPI stackOverflowHandler(EXCEPTION_POINTERS *pointers)
{
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    return unhandledFilter(pointers);
}

void abortHandler(int)
{
    RaiseException(abortCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void pureCallHandler()
{
    RaiseException(pureCallCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void invalidParameterHandler(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
{
    RaiseException(invalidParameterCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void installFilters() noexcept
{
    SetUnhandledExceptionFilter(unhandledFilter);
    static PVOID vectored = nullptr;
    if (vectored == nullptr)
        vectored = AddVectoredExceptionHandler(1, stackOverflowHandler);
    signal(SIGABRT, abortHandler);
    // No CRT "abort() has been called" box and no Windows Error Reporting
    // dialog on top of our own report.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_purecall_handler(pureCallHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
}

BOOL WINAPI consoleHandler(DWORD event)
{
    // Ctrl+C, closing the console window, logging off: ended on purpose.
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT
        || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        current()->header.state.store(StateCleanExit);
    }
    return FALSE;
}

} // namespace

void install(const Config &config)
{
    g_config = config;
    toWide(config.directory, g_directory, 1024);
    if (GetModuleFileNameW(nullptr, g_executable, MAX_PATH) == 0)
        g_executable[0] = L'\0';

    // Loaded now, not while crashing: loading a library takes the loader lock.
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
        g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFunction>(
            reinterpret_cast<void *>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    }
    // An exception inside a window procedure is otherwise swallowed by the
    // kernel-to-user callback boundary on 64-bit Windows, and the program
    // carries on in whatever state it left.
    using PolicyGetter = BOOL(WINAPI *)(LPDWORD);
    using PolicySetter = BOOL(WINAPI *)(DWORD);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto getPolicy = reinterpret_cast<PolicyGetter>(
        reinterpret_cast<void *>(GetProcAddress(kernel, "GetProcessUserModeExceptionPolicy")));
    auto setPolicy = reinterpret_cast<PolicySetter>(
        reinterpret_cast<void *>(GetProcAddress(kernel, "SetProcessUserModeExceptionPolicy")));
    DWORD policy = 0;
    if (getPolicy != nullptr && setPolicy != nullptr && getPolicy(&policy))
        setPolicy(policy & ~DWORD(0x1)); // PROCESS_CALLBACK_FILTER_ENABLED

    // Room for the filter to run after a stack overflow.
    ULONG guarantee = 64 * 1024;
    SetThreadStackGuarantee(&guarantee);

    g_wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_written = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watcher = CreateThread(nullptr, 256 * 1024, watcherMain, nullptr, 0, &g_watcherId);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    installFilters();
}

void reinstall()
{
    installFilters();
}

void fatalMessage()
{
    // Qt ends a fatal error on Windows with RaiseFailFastException, which no
    // handler ever sees. Raising our own exception first gets the report
    // written, with the stack still showing who called qFatal.
    g_fatalFromQt.store(true);
    RaiseException(abortCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void setInteractive(bool relaunch)
{
    g_config.interactive = true;
    g_config.relaunch = relaunch && !g_config.viewer;
}

void rememberMainThread()
{
    g_mainStack = currentStackBounds();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
}

bool debuggerAttached()
{
    return IsDebuggerPresent() != FALSE;
}

bool writeFreezeReport(std::int64_t frozenSinceMs, char *path, int pathCapacity)
{
    // A crash is being reported: the process is ending, not frozen.
    if (g_entered.load() != 0)
        return false;
    Data &data = *current();
    const std::int64_t now = nowMs();
    static wchar_t wide[1024];
    buildPath(path, pathCapacity, wide, 1024, "freeze", now, ".txt");
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    snapshotModules(g_freezeModules);
    const std::uint64_t mainThread = data.header.mainThreadId.load();
    {
        Writer out(writeHandle, &file);
        out.text("OpenChat stopped responding").newline();
        out.text("===========================").newline().newline();
        out.text("What happened:  The main thread, which draws the window and handles input, has "
                 "not responded for ")
            .decimal((now - frozenSinceMs) / 1000).text(" s.").newline();
        char doing[256];
        out.text("Doing:          ")
            .text(CrashWriter::threadActivity(data, mainThread, doing, sizeof(doing))
                      ? doing
                      : "nothing that OpenChat records on the main thread")
            .newline();
        out.text("Thread:         main (id ").unsignedDecimal(mainThread).text(")").newline();
        out.newline();
        out.text("Application:    ").field(data.header.application, sizeof(data.header.application)).newline();
        out.text("Build:          ").field(data.header.build, sizeof(data.header.build)).newline();
        out.text("System:         ").field(data.header.system, sizeof(data.header.system)).newline();
        out.text("Noticed:        ");
        CrashWriter::utcTime(out, now);
        out.newline();
        CrashWriter::blackBoxSections(out, data, mainThread, "froze");
        out.newline().text("Stack of the main thread").newline().text("------------------------")
            .newline();
        // Suspended only for as long as it takes to read its registers: the
        // unwinder can take a loader lock the main thread might hold, so the
        // walk runs after it is resumed. A thread that is truly frozen has not
        // moved its stack in the meantime; one that has is not frozen, and the
        // bounds checks stop the walk at the first frame that no longer fits.
        bool walked = false;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (g_mainThread != nullptr && SuspendThread(g_mainThread) != DWORD(-1)) {
            walked = GetThreadContext(g_mainThread, &context) != FALSE;
            ResumeThread(g_mainThread);
        }
        if (walked)
            writeStack(out, g_freezeModules, context, g_mainStack);
        if (!walked)
            out.text("  (the main thread's stack could not be read)").newline();
        out.newline().text("Loaded modules (base, size, link timestamp, name)").newline();
        out.text("-------------------------------------------------").newline();
        writeModules(out, g_freezeModules);
    }
    CloseHandle(file);
    return true;
}

} // namespace OpenChat::CrashHandlerPlatform
