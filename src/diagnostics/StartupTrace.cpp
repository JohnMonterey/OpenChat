#include "diagnostics/StartupTrace.h"

#include <QFile>
#include <QStringList>
#include <QSysInfo>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#elif defined(__linux__)
#    include <unistd.h>
#endif

namespace OpenChat::StartupTrace {
namespace {

using Clock = std::chrono::steady_clock;

// A step long enough to be worth flagging, and one that is clearly a problem.
constexpr qint64 slowMs = 250;
constexpr qint64 verySlowMs = 2000;
// A step that has not finished after this long is announced, so a hang shows
// where it is even though the step's own line cannot be printed yet.
constexpr qint64 announceAfterMs = 1000;
// And these are the moments the terminal hears that it is still going.
constexpr int stillRunningAfterSeconds[] = {3, 5, 10, 20, 30, 45, 60, 90, 120, 180, 300};

struct OpenStep {
    int id = 0;
    QString what;
    int depth = 0;
    Clock::time_point started;
    std::thread::id thread;
    bool announced = false;
    int stillReported = 0; // entries of stillRunningAfterSeconds already said
};

struct DoneStep {
    QString path;
    qint64 ms = 0;
};

struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<QByteArray> queue;
    QList<QByteArray> history;
    std::FILE *file = nullptr;
    QString filePath;
    std::vector<OpenStep> open;
    std::vector<DoneStep> done;
    QStringList milestones;
    int nextId = 0;
    bool stopping = false;
    bool summarized = false;
    std::thread writer;
    Clock::time_point start;
    // How long the system took to start the process before main() ran:
    // loading the program and every DLL it links, antivirus scans included.
    qint64 beforeMainMs = 0;
};

std::atomic<bool> g_enabled{false};
thread_local int t_depth = 0;

State &state()
{
    // Leaked on purpose: lines may still be written while static destructors
    // run at exit.
    static State *const s = new State;
    return *s;
}

qint64 msSince(Clock::time_point then, Clock::time_point now = Clock::now())
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
}

QString duration(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(double(ms) / 1000.0, 0, 'f', 1);
}

QString slowness(qint64 ms)
{
    if (ms >= verySlowMs)
        return QStringLiteral("   <-- VERY SLOW");
    if (ms >= slowMs)
        return QStringLiteral("   <-- slow");
    return {};
}

// Caller holds the mutex.
void pushLine(State &s, Clock::time_point at, const QString &text)
{
    const double seconds = double(s.beforeMainMs + msSince(s.start, at)) / 1000.0;
    const QByteArray line =
        (QStringLiteral("[%1 s]  ").arg(seconds, 8, 'f', 3) + text + QLatin1Char('\n')).toUtf8();
    s.queue.push_back(line);
    s.history.append(line);
    s.wake.notify_one();
}

QString indent(int depth)
{
    return QString(depth * 2, QLatin1Char(' '));
}

// Prints the start of every step that is still open on `thread` and has not
// said so yet, outermost first, so the lines that follow have their context.
// Caller holds the mutex.
void announceOpenSteps(State &s, std::thread::id thread)
{
    for (OpenStep &step : s.open) {
        if (step.thread != thread || step.announced)
            continue;
        step.announced = true;
        pushLine(s, step.started, indent(step.depth) + QStringLiteral("> ") + step.what);
    }
}

QString pathOf(const State &s, const OpenStep &step)
{
    QStringList names;
    for (const OpenStep &other : s.open) {
        if (other.thread == step.thread && other.depth < step.depth)
            names.append(other.what);
    }
    names.append(step.what);
    return names.join(QStringLiteral(" > "));
}

// Caller holds the mutex. Announces steps that have been running a while,
// and repeats that they are still running at growing intervals.
void checkLongSteps(State &s)
{
    const Clock::time_point now = Clock::now();
    const int thresholds = int(std::size(stillRunningAfterSeconds));
    for (OpenStep &step : s.open) {
        const qint64 ms = msSince(step.started, now);
        if (!step.announced && ms >= announceAfterMs)
            announceOpenSteps(s, step.thread);
        int crossed = step.stillReported;
        while (crossed < thresholds && ms >= qint64(stillRunningAfterSeconds[crossed]) * 1000)
            ++crossed;
        if (crossed == step.stillReported)
            continue;
        step.stillReported = crossed;
        // Only the innermost step says it is still running; the ones around
        // it are running because of it.
        const bool hasInner = std::any_of(s.open.cbegin(), s.open.cend(), [&](const OpenStep &other) {
            return other.thread == step.thread && other.depth > step.depth;
        });
        if (!hasInner) {
            pushLine(s, now,
                     indent(step.depth + 1) + QStringLiteral("... still in \"%1\" after %2 s")
                                                  .arg(step.what)
                                                  .arg(stillRunningAfterSeconds[crossed - 1]));
        }
    }
}

void writerLoop()
{
    State &s = state();
    for (;;) {
        std::deque<QByteArray> lines;
        std::FILE *file = nullptr;
        bool stop = false;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait_for(lock, std::chrono::milliseconds(250),
                            [&] { return !s.queue.empty() || s.stopping; });
            checkLongSteps(s);
            lines.swap(s.queue);
            file = s.file;
            stop = s.stopping && s.queue.empty();
        }
        for (const QByteArray &line : lines) {
            std::fwrite(line.constData(), 1, std::size_t(line.size()), stdout);
            if (file != nullptr)
                std::fwrite(line.constData(), 1, std::size_t(line.size()), file);
        }
        std::fflush(stdout);
        if (file != nullptr)
            std::fflush(file);
        if (stop)
            return;
    }
}

qint64 measureBeforeMain()
{
#if defined(_WIN32)
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
        return 0;
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER created;
    created.LowPart = creation.dwLowDateTime;
    created.HighPart = creation.dwHighDateTime;
    ULARGE_INTEGER current;
    current.LowPart = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    return current.QuadPart > created.QuadPart ? qint64((current.QuadPart - created.QuadPart) / 10000)
                                               : 0;
#elif defined(__linux__)
    // Field 22 of /proc/self/stat is the start time in clock ticks since boot.
    QFile stat(QStringLiteral("/proc/self/stat"));
    QFile uptime(QStringLiteral("/proc/uptime"));
    if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray statText = stat.readAll();
    const qsizetype close = statText.lastIndexOf(')');
    const QList<QByteArray> fields = statText.mid(close + 2).split(' ');
    // Fields after the name start at field 3, so field 22 is index 19.
    if (fields.size() < 20)
        return 0;
    const double ticks = double(sysconf(_SC_CLK_TCK));
    const double startedSeconds = fields.at(19).toDouble() / ticks;
    const double nowSeconds = uptime.readAll().split(' ').value(0).toDouble();
    return nowSeconds > startedSeconds ? qint64((nowSeconds - startedSeconds) * 1000.0) : 0;
#else
    return 0;
#endif
}

void prepareConsole()
{
#if defined(_WIN32)
    // Paths and names print as they are, whatever the console's code page.
    SetConsoleOutputCP(CP_UTF8);
    // A click into a console window with Quick Edit on starts a selection,
    // and every write to that console waits until it ends: the program looks
    // frozen. Tracing is when someone is most likely to click in there.
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input != INVALID_HANDLE_VALUE && GetConsoleMode(input, &mode))
        SetConsoleMode(input, (mode & ~DWORD(ENABLE_QUICK_EDIT_MODE)) | ENABLE_EXTENDED_FLAGS);
#endif
}

} // namespace

void enable()
{
    if (g_enabled.exchange(true))
        return;
    State &s = state();
    s.start = Clock::now();
    s.beforeMainMs = measureBeforeMain();
    prepareConsole();
    {
        std::lock_guard lock(s.mutex);
        pushLine(s, s.start, QStringLiteral("OpenChat startup trace"));
        pushLine(s, s.start,
                 QStringLiteral("System: %1 (kernel %2), %3, %4 logical processors")
                     .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                          QSysInfo::currentCpuArchitecture())
                     .arg(std::thread::hardware_concurrency()));
        pushLine(s, s.start,
                 QStringLiteral("Times count from the moment the system created the process. "
                                "\">\" starts a step, \"<\" ends one; a step that ends quickly "
                                "gets one line."));
        pushLine(s, s.start,
                 s.beforeMainMs > 0
                     ? QStringLiteral("OpenChat's own code starts: loading OpenChat.exe and the "
                                      "DLLs it needs (and any antivirus scan of them) took %1%2")
                           .arg(duration(s.beforeMainMs), slowness(s.beforeMainMs))
                     : QStringLiteral("OpenChat's own code starts"));
    }
    s.writer = std::thread(writerLoop);
}

bool enabled()
{
    return g_enabled.load(std::memory_order_relaxed);
}

void saveTo(const QString &path)
{
    if (!enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    if (s.file != nullptr)
        return;
#if defined(_WIN32)
    s.file = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"wb");
#else
    s.file = std::fopen(QFile::encodeName(path).constData(), "wb");
#endif
    if (s.file == nullptr)
        return;
    s.filePath = path;
    // Everything already printed; what is still queued, the writer adds.
    const qsizetype printed = s.history.size() - qsizetype(s.queue.size());
    for (qsizetype index = 0; index < printed; ++index)
        std::fwrite(s.history.at(index).constData(), 1, std::size_t(s.history.at(index).size()), s.file);
    std::fflush(s.file);
}

void mark(const QString &what)
{
    if (!enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    const Clock::time_point now = Clock::now();
    pushLine(s, now, indent(t_depth) + QStringLiteral("* ") + what);
    s.milestones.append(QStringLiteral("%1 s  %2")
                            .arg(double(s.beforeMainMs + msSince(s.start, now)) / 1000.0, 7, 'f', 3)
                            .arg(what));
}

void note(const QString &what)
{
    if (!enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    announceOpenSteps(s, std::this_thread::get_id());
    pushLine(s, Clock::now(), indent(t_depth) + QStringLiteral("  ") + what);
}

void summarize(const QString &why)
{
    if (!enabled())
        return;
    State &s = state();
    std::lock_guard lock(s.mutex);
    s.summarized = true;
    const Clock::time_point now = Clock::now();
    pushLine(s, now, QString());
    pushLine(s, now, QStringLiteral("===== Summary: %1 =====").arg(why));
    pushLine(s, now, QStringLiteral("Before OpenChat's own code ran: %1").arg(duration(s.beforeMainMs)));
    pushLine(s, now, QStringLiteral("Moments:"));
    for (const QString &milestone : std::as_const(s.milestones))
        pushLine(s, now, QStringLiteral("  ") + milestone);
    std::vector<DoneStep> slowest = s.done;
    std::stable_sort(slowest.begin(), slowest.end(),
                     [](const DoneStep &a, const DoneStep &b) { return a.ms > b.ms; });
    pushLine(s, now, QStringLiteral("Slowest steps (a step includes the ones inside it):"));
    int shown = 0;
    for (const DoneStep &step : slowest) {
        if (step.ms < 20 || shown == 12)
            break;
        pushLine(s, now, QStringLiteral("  %1  %2").arg(duration(step.ms), 8).arg(step.path));
        ++shown;
    }
    if (shown == 0)
        pushLine(s, now, QStringLiteral("  (none took 20 ms or more)"));
    for (const OpenStep &step : s.open)
        pushLine(s, now, QStringLiteral("Still running: %1 (%2 so far)")
                             .arg(pathOf(s, step), duration(msSince(step.started, now))));
    if (!s.filePath.isEmpty())
        pushLine(s, now, QStringLiteral("This trace is saved in %1").arg(s.filePath));
    pushLine(s, now, QString());
}

void finish()
{
    if (!enabled())
        return;
    State &s = state();
    bool summarized = false;
    {
        std::lock_guard lock(s.mutex);
        summarized = s.summarized;
    }
    if (!summarized)
        summarize(QStringLiteral("OpenChat is closing"));
    {
        std::lock_guard lock(s.mutex);
        s.stopping = true;
        s.wake.notify_one();
    }
    if (s.writer.joinable())
        s.writer.join();
    std::lock_guard lock(s.mutex);
    if (s.file != nullptr) {
        std::fclose(s.file);
        s.file = nullptr;
    }
}

Step::Step(const char *what)
    : Step(what, QString())
{
}

Step::Step(const char *what, const QString &detail)
{
    if (!enabled())
        return;
    m_what = detail.isEmpty() ? QString::fromUtf8(what)
                              : QStringLiteral("%1 (%2)").arg(QString::fromUtf8(what), detail);
    m_started = Clock::now();
    State &s = state();
    std::lock_guard lock(s.mutex);
    // The enclosing steps now have something inside them: they get a line of
    // their own, so this one reads in context.
    announceOpenSteps(s, std::this_thread::get_id());
    m_id = s.nextId++;
    s.open.push_back({m_id, m_what, t_depth, m_started, std::this_thread::get_id()});
    ++t_depth;
}

Step::~Step()
{
    if (m_id < 0)
        return;
    --t_depth;
    State &s = state();
    std::lock_guard lock(s.mutex);
    const Clock::time_point now = Clock::now();
    const qint64 ms = msSince(m_started, now);
    auto entry = std::find_if(s.open.begin(), s.open.end(),
                              [&](const OpenStep &step) { return step.id == m_id; });
    if (entry == s.open.end())
        return;
    const QString path = pathOf(s, *entry);
    const bool announced = entry->announced;
    const int depth = entry->depth;
    s.open.erase(entry);
    s.done.push_back({path, ms});
    const QString result = m_result.isEmpty() ? QString() : QStringLiteral(": ") + m_result;
    pushLine(s, now,
             indent(depth) + (announced ? QStringLiteral("< ") : QStringLiteral("  ")) + m_what
                 + result + QStringLiteral("  ") + duration(ms) + slowness(ms));
}

void Step::setResult(const QString &result)
{
    m_result = result;
}

} // namespace OpenChat::StartupTrace
