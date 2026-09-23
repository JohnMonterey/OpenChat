#include "diagnostics/CrashHandlerPlatform.h"

#include "diagnostics/BlackBoxData.h"
#include "diagnostics/CrashWriter.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>

extern char **environ;

#if __has_include(<execinfo.h>)
#    include <execinfo.h>
#    define OPENCHAT_HAVE_BACKTRACE 1
#else
#    define OPENCHAT_HAVE_BACKTRACE 0
#endif

#if defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <sys/sysctl.h>
#    include <sys/ucontext.h>
#else
#    include <ucontext.h>
#endif

// Everything reachable from a signal handler below is async-signal-safe in
// practice: raw open/write/close, fork/execve, and the CrashWriter formatter.
// backtrace() is the one exception the whole industry accepts; it is warmed up
// at install so its first call does not have to load the unwinder.
namespace OpenChat::CrashHandlerPlatform {

namespace {

using namespace BlackBoxData;
using CrashWriter::Writer;

Config g_config{};
char g_executable[1024] = {};
std::atomic<int> g_entered{0};
pthread_t g_mainThread{};
bool g_haveMainThread = false;

// A crash on the main thread's stack — an overflow above all — needs somewhere
// else to run the handler.
alignas(16) char g_alternateStack[256 * 1024];

const int fatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
// The system or the user ending the process: not a crash, and must not be
// explained as one on the next launch.
const int endingSignals[] = {SIGTERM, SIGHUP, SIGINT};

// The freeze report's handshake with the main thread: the detector signals it,
// the main thread records its own return addresses here, and the detector
// writes them out. The handler never touches a file descriptor, so one that
// answers late cannot write into a descriptor the detector has since closed
// and the process has reused.
constexpr int stackRequestSignal = SIGUSR2;
constexpr int stackRequestFrames = 64;
void *g_requestedFrames[stackRequestFrames];
std::atomic<int> g_requestedCount{0};
std::atomic<std::uint32_t> g_stackRequest{0};
std::atomic<std::uint32_t> g_stackAnswer{0};

void writeFd(void *context, const char *data, std::size_t size) noexcept
{
    const int fd = *static_cast<const int *>(context);
    while (size > 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        data += written;
        size -= std::size_t(written);
    }
}

// "<directory>/<prefix>-20260923-120513-4321.txt" into `out`.
void buildPath(char *out, std::size_t capacity, const char *prefix, std::int64_t epochMs) noexcept
{
    CrashWriter::MemorySink sink{out, capacity, 0};
    out[0] = '\0';
    Writer writer(CrashWriter::MemorySink::write, &sink);
    writer.text(g_config.directory).text("/").text(prefix).text("-");
    CrashWriter::fileStamp(writer, epochMs);
    writer.text("-").unsignedDecimal(std::uint64_t(getpid())).text(".txt");
}

const char *signalName(int signal) noexcept
{
    switch (signal) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    case SIGSYS: return "SIGSYS";
    default: return "signal";
    }
}

void describeSignal(Writer &out, int signal, const siginfo_t *info, std::uintptr_t stack) noexcept
{
    const auto address = std::uint64_t(reinterpret_cast<std::uintptr_t>(info ? info->si_addr : nullptr));
    const int code = info ? info->si_code : 0;
    // A fault just beyond the stack pointer is the guard page below a stack
    // that has run out.
    const bool stackOverflow = stack != 0 && address + 65536 > stack && address < stack + 65536;
    switch (signal) {
    case SIGSEGV:
    case SIGBUS:
        if (stackOverflow) {
            out.text("The program ran out of stack space (a runaway recursion).");
            return;
        }
        if (signal == SIGBUS)
            break;
        out.text("The program tried to use memory at address ").hex(address, 1);
        if (address < 0x10000)
            out.text(", which is not valid: a null pointer.");
        else if (code == SEGV_ACCERR)
            out.text(" in a way it is not allowed to (writing to read-only memory, or running data "
                     "as code).");
        else
            out.text(", which does not exist (freed or never allocated).");
        return;
    case SIGILL:
        out.text("The processor met an instruction it cannot run (this build uses an instruction "
                 "this CPU lacks, or code was corrupted).");
        return;
    case SIGFPE:
        out.text(code == FPE_INTDIV ? "An integer was divided by zero."
                                    : "An arithmetic error stopped the program.");
        return;
    case SIGABRT: {
        char reason[contextValueBytes];
        if (CrashWriter::contextValue(*current(), "fatal", reason, sizeof(reason)))
            out.text("OpenChat stopped itself on purpose: ").field(reason, sizeof(reason));
        else
            out.text("OpenChat stopped itself on purpose (abort) without recording why; the "
                     "stack below shows where.");
        return;
    }
    case SIGTRAP:
        out.text("A trap instruction was reached (a failed check inside the code).");
        return;
    case SIGSYS:
        out.text("A forbidden system call was made.");
        return;
    default:
        out.text("The program received fatal signal ").decimal(signal).text(".");
        return;
    }
    // SIGBUS that is not a stack overflow.
    out.text("Bus error at address ").hex(address, 1).text(
        " (a memory-mapped file that shrank underneath the program, or a misaligned access).");
}

std::uintptr_t stackPointer(const void *context) noexcept
{
    if (context == nullptr)
        return 0;
    const auto *ucontext = static_cast<const ucontext_t *>(context);
#if defined(__linux__) && defined(__x86_64__)
    return std::uintptr_t(ucontext->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
    return std::uintptr_t(ucontext->uc_mcontext.sp);
#elif defined(__APPLE__) && defined(__x86_64__)
    return std::uintptr_t(ucontext->uc_mcontext->__ss.__rsp);
#else
    (void)ucontext;
    return 0;
#endif
}

std::uintptr_t programCounter(const void *context) noexcept
{
    if (context == nullptr)
        return 0;
    const auto *ucontext = static_cast<const ucontext_t *>(context);
#if defined(__linux__) && defined(__x86_64__)
    return std::uintptr_t(ucontext->uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
    return std::uintptr_t(ucontext->uc_mcontext.pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    return std::uintptr_t(ucontext->uc_mcontext->__ss.__rip);
#else
    // Apple Silicon hides the program counter behind pointer authentication;
    // the stack below still starts at the faulting frame.
    (void)ucontext;
    return 0;
#endif
}

// "OpenChat+0x1a2b3c (symbol+0x12)" for an address, and the module's base name
// for the hint.
const char *writeLocation(Writer &out, std::uintptr_t address) noexcept
{
    Dl_info info{};
    if (address == 0 || dladdr(reinterpret_cast<void *>(address), &info) == 0 || info.dli_fname == nullptr) {
        out.hex(address, 1).text(" (in no loaded module: a jump to a bad address)");
        return nullptr;
    }
    const char *module = CrashWriter::baseName(info.dli_fname);
    out.text(module).text("+").hex(address - reinterpret_cast<std::uintptr_t>(info.dli_fbase), 1);
    if (info.dli_sname != nullptr) {
        out.text(" (").text(info.dli_sname).text("+")
            .hex(address - reinterpret_cast<std::uintptr_t>(info.dli_saddr), 1).text(")");
    }
    return module;
}

const char *moduleOf(void *address) noexcept
{
    Dl_info info{};
    if (dladdr(address, &info) == 0 || info.dli_fname == nullptr)
        return "";
    return CrashWriter::baseName(info.dli_fname);
}

bool startsWith(const char *value, const char *prefix) noexcept
{
    return std::strncmp(value, prefix, std::strlen(prefix)) == 0;
}

// The C and C++ runtimes: where abort(), raise() and std::terminate live, and
// where the signal trampoline is.
bool isRuntime(const char *module) noexcept
{
    return startsWith(module, "libc.so") || startsWith(module, "libc-") || startsWith(module, "libstdc++")
        || startsWith(module, "libgcc_s") || startsWith(module, "libpthread")
        || startsWith(module, "libsystem_") || startsWith(module, "libc++") || startsWith(module, "libdyld")
        || startsWith(module, "libunwind");
}

bool isQtCore(const char *module) noexcept
{
    return startsWith(module, "libQt6Core") || std::strcmp(module, "QtCore") == 0;
}

// The frame to blame, found in the stack: the first one after the signal
// trampoline, or — for an abort — the first one that is not abort() itself,
// the C++ runtime's terminate machinery, Qt's fatal-message path, or our own
// terminate handler that it calls. Zero when the stack cannot say.
std::uintptr_t blamedFrame(int signal) noexcept
{
#if OPENCHAT_HAVE_BACKTRACE
    void *frames[64];
    const int count = backtrace(frames, 64);
    int index = 0;
    // Our own handler's frames come first, up to the trampoline in the runtime.
    while (index < count && !isRuntime(moduleOf(frames[index])))
        ++index;
    while (index < count && isRuntime(moduleOf(frames[index])))
        ++index;
    if (signal != SIGABRT)
        return index < count ? reinterpret_cast<std::uintptr_t>(frames[index]) : 0;
    while (index < count) {
        const char *module = moduleOf(frames[index]);
        const bool calledByRuntime = index + 1 < count && isRuntime(moduleOf(frames[index + 1]));
        if (isRuntime(module) || isQtCore(module) || calledByRuntime) {
            ++index;
            continue;
        }
        return reinterpret_cast<std::uintptr_t>(frames[index]);
    }
#else
    (void)signal;
#endif
    return 0;
}

void writeStack(Writer &out, int fd) noexcept
{
#if OPENCHAT_HAVE_BACKTRACE
    // Everything so far goes to disk first: if unwinding a corrupted stack
    // faults, the process dies here with the rest of the report already saved.
    out.flush();
    void *frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fd);
#else
    (void)fd;
    out.text("  (this platform offers no backtrace)").newline();
#endif
}

#if defined(__linux__)
// The executable mappings, so a stack of bare addresses can be symbolised
// later against the exact libraries that were loaded.
void writeExecutableMappings(Writer &out) noexcept
{
    const int maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (maps < 0)
        return;
    char chunk[2048];
    char line[512];
    std::size_t length = 0;
    for (;;) {
        const ssize_t got = ::read(maps, chunk, sizeof(chunk));
        if (got <= 0)
            break;
        for (ssize_t index = 0; index < got; ++index) {
            const char c = chunk[index];
            if (c != '\n') {
                if (length + 1 < sizeof(line))
                    line[length++] = c;
                continue;
            }
            line[length] = '\0';
            // "start-end perms offset dev inode path": keep executable ones.
            const char *perms = std::strchr(line, ' ');
            if (perms != nullptr && perms[1] != '\0' && perms[2] != '\0' && perms[3] == 'x')
                out.text("  ").text(line, length).newline();
            length = 0;
        }
    }
    ::close(maps);
}
#endif

void relaunchViewer(const char *reportPath) noexcept
{
    if (!g_config.relaunch || g_executable[0] == '\0')
        return;
    // posix_spawn, not fork: fork() runs the atfork handlers and takes every
    // malloc arena lock, so a crash inside malloc — heap corruption — would
    // hang right here. glibc spawns with CLONE_VFORK and macOS with a system
    // call; neither touches the allocator.
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0)
        return;
    // The viewer must not inherit the crashing signal blocked, nor our handlers.
    sigset_t none;
    sigemptyset(&none);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int signal : fatalSignals)
        sigaddset(&defaults, signal);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char flag[] = "--crash-report";
    char *arguments[] = {g_executable, flag, const_cast<char *>(reportPath), nullptr};
    pid_t child = 0;
    posix_spawn(&child, g_executable, nullptr, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
}

void writeCrashReport(int signal, siginfo_t *info, void *context) noexcept
{
    Data &data = *current();
    const std::int64_t now = nowMs();
    char path[1024];
    buildPath(path, sizeof(path), "crash", now);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        fd = STDERR_FILENO;
    // Recorded before anything that could fault again, so even a report cut
    // short is found and shown by the next launch.
    if (fd != STDERR_FILENO) {
        BlackBoxData::copyText(data.header.reportPath, int(sizeof(data.header.reportPath)), path,
                               int(std::strlen(path)));
        data.header.state.store(StateCrashed);
    }

    const std::uint64_t thread = currentThreadId();
    {
        Writer out(writeFd, &fd);
        out.text("OpenChat crash report").newline();
        out.text("=====================").newline().newline();
        out.text("What happened:  ");
        describeSignal(out, signal, info, stackPointer(context));
        // Saved before the stack is unwound to find who to blame.
        out.flush();
        // For an abort the program counter is inside abort(); blame its caller.
        const std::uintptr_t pc = signal == SIGABRT || programCounter(context) == 0
            ? blamedFrame(signal)
            : programCounter(context);
        out.newline().text("Where:          ");
        const char *module = pc != 0 ? writeLocation(out, pc) : nullptr;
        if (pc == 0)
            out.text("see the first frame after the signal handler in the stack below");
        out.newline().text("Thread:         ");
        char name[threadNameBytes];
        if (CrashWriter::threadName(data, thread, name, sizeof(name)))
            out.text(name).text(" ");
        out.text("(id ").unsignedDecimal(thread).text(")").newline();
        char doing[256];
        out.text("Doing:          ")
            .text(CrashWriter::threadActivity(data, thread, doing, sizeof(doing))
                      ? doing
                      : "nothing that OpenChat records on this thread")
            .newline();
        if (const char *hint = CrashWriter::moduleHint(module))
            out.text("Hint:           ").text(hint).newline();
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
        out.text("Signal:         ").text(signalName(signal)).text(" (code ")
            .decimal(info ? info->si_code : 0).text(")").newline();
        out.text("Process id:     ").unsignedDecimal(std::uint64_t(getpid())).newline();

        CrashWriter::blackBoxSections(out, data, thread);

        out.newline().text("Stack of the crashed thread (the first frames are the crash handler "
                           "itself)").newline();
        out.text("-----------------------------------------------------------------------")
            .newline();
        writeStack(out, fd);
#if defined(__linux__)
        out.newline().text("Executable mappings").newline().text("-------------------").newline();
        writeExecutableMappings(out);
#elif defined(__APPLE__)
        out.newline().text("macOS also keeps its own report of this crash under "
                           "~/Library/Logs/DiagnosticReports.").newline();
#endif
    }
    if (fd != STDERR_FILENO)
        ::close(fd);

    // One line on the terminal too, for whoever launched it from one.
    int err = STDERR_FILENO;
    Writer console(writeFd, &err);
    console.text("\nOpenChat crashed (").text(signalName(signal)).text("). Report: ").text(path)
        .newline();
    console.flush();

    relaunchViewer(path);
}

void fatalHandler(int signal, siginfo_t *info, void *context)
{
    // A backstop: whatever happens below, the process does not outlive the
    // crash by more than this.
    alarm(15);
    if (g_entered.exchange(1) == 0) {
        writeCrashReport(signal, info, context);
    } else {
        // Another thread is already writing the report, or the handler itself
        // faulted: let that one finish, then fall through to the default.
        sleep(3);
    }
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    // Back to the system: a core file, or macOS's own crash report.
    raise(signal);
}

void endingHandler(int signal)
{
    current()->header.state.store(StateCleanExit);
    struct sigaction fallback{};
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(signal, &fallback, nullptr);
    raise(signal);
}

void stackRequestHandler(int)
{
    const std::uint32_t request = g_stackRequest.load();
#if OPENCHAT_HAVE_BACKTRACE
    g_requestedCount.store(backtrace(g_requestedFrames, stackRequestFrames));
#endif
    g_stackAnswer.store(request);
}

void installHandlers() noexcept
{
    struct sigaction action{};
    action.sa_sigaction = fatalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal : fatalSignals)
        sigaction(signal, &action, nullptr);

    struct sigaction ending{};
    ending.sa_handler = endingHandler;
    sigemptyset(&ending.sa_mask);
    for (int signal : endingSignals) {
        struct sigaction existing{};
        sigaction(signal, nullptr, &existing);
        // Leave alone a signal someone deliberately ignores (nohup).
        if (existing.sa_handler != SIG_IGN)
            sigaction(signal, &ending, nullptr);
    }

    struct sigaction stack{};
    stack.sa_handler = stackRequestHandler;
    stack.sa_flags = SA_RESTART;
    sigemptyset(&stack.sa_mask);
    sigaction(stackRequestSignal, &stack, nullptr);
}

} // namespace

void install(const Config &config)
{
    g_config = config;
#if defined(__APPLE__)
    std::uint32_t size = sizeof(g_executable);
    if (_NSGetExecutablePath(g_executable, &size) != 0)
        g_executable[0] = '\0';
#elif defined(__linux__)
    const ssize_t length = readlink("/proc/self/exe", g_executable, sizeof(g_executable) - 1);
    g_executable[length > 0 ? length : 0] = '\0';
#endif

    stack_t alternate{};
    alternate.ss_sp = g_alternateStack;
    alternate.ss_size = sizeof(g_alternateStack);
    sigaltstack(&alternate, nullptr);

#if OPENCHAT_HAVE_BACKTRACE
    // The first backtrace() loads the unwinder, which allocates; do that now
    // rather than inside a handler.
    void *warm[4];
    (void)backtrace(warm, 4);
#endif
    installHandlers();
}

void reinstall()
{
    installHandlers();
}

void fatalMessage()
{
    // Qt calls abort() next, which arrives as SIGABRT.
}

void setInteractive(bool relaunch)
{
    g_config.interactive = true;
    g_config.relaunch = relaunch && !g_config.viewer;
}

void rememberMainThread()
{
    g_mainThread = pthread_self();
    g_haveMainThread = true;
}

bool debuggerAttached()
{
#if defined(__APPLE__)
    int query[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info{};
    std::size_t size = sizeof(info);
    if (sysctl(query, 4, &info, &size, nullptr, 0) != 0)
        return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    // Under a debugger on Linux every thread stops together, so the detector
    // never sees the main thread alone fall silent.
    return false;
#endif
}

bool writeFreezeReport(std::int64_t frozenSinceMs, char *path, int pathCapacity)
{
    Data &data = *current();
    const std::int64_t now = nowMs();
    buildPath(path, std::size_t(pathCapacity), "freeze", now);
    int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    const std::uint64_t mainThread = data.header.mainThreadId.load();
    {
        Writer out(writeFd, &fd);
        out.text("OpenChat stopped responding").newline();
        out.text("===========================").newline().newline();
        out.text("What happened:  The main thread, which draws the window and handles input, "
                 "has not responded for ")
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
    }
    // Ask the main thread for its own stack. A thread stuck in a loop or a
    // system call still takes a signal; one that does not answer in two
    // seconds is stuck somewhere even a signal cannot reach. Each request is
    // numbered, so a late answer to an earlier one is not taken for this one.
    bool answered = false;
    if (g_haveMainThread) {
        const std::uint32_t request = g_stackRequest.fetch_add(1) + 1;
        if (pthread_kill(g_mainThread, stackRequestSignal) == 0) {
            for (int wait = 0; wait < 200 && g_stackAnswer.load() != request; ++wait)
                usleep(10000);
            answered = g_stackAnswer.load() == request;
        }
    }
    {
        Writer out(writeFd, &fd);
        if (!answered)
            out.text("  (the main thread did not answer a request for its stack)").newline();
        else
            out.text("  (the first frames are the stack request handler itself)").newline();
    }
#if OPENCHAT_HAVE_BACKTRACE
    if (answered)
        backtrace_symbols_fd(g_requestedFrames, g_requestedCount.load(), fd);
#endif
    ::close(fd);
    return true;
}

} // namespace OpenChat::CrashHandlerPlatform
