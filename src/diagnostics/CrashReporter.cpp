#include "diagnostics/CrashReporter.h"

#include "diagnostics/BlackBox.h"
#include "diagnostics/BlackBoxData.h"
#include "diagnostics/CrashHandlerPlatform.h"
#include "diagnostics/CrashWriter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimeZone>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>

#ifndef OPENCHAT_VERSION
#    define OPENCHAT_VERSION "unknown"
#endif
#ifndef OPENCHAT_BUILD_ID
#    define OPENCHAT_BUILD_ID "unknown revision"
#endif

namespace OpenChat::CrashReporter {

namespace {

using namespace BlackBoxData;

// Long enough that a slow start (unlocking a profile derives an Argon2id key)
// is never called a freeze, short enough that a genuinely stuck window is
// recorded long before the user gives up on it.
constexpr qint64 freezeThresholdMs = 10000;
constexpr int keptReports = 20;
constexpr auto blackBoxName = "blackbox.bin";

QString g_directory;
Options g_options;
bool g_installed = false;
bool g_sessionStarted = false;
PendingReport g_pending;
QtMessageHandler g_previousHandler = nullptr;
std::atomic<bool> g_cleanExit{false};

// The last warning, so a warning repeated in a loop occupies one line of the
// recent-events ring instead of all of it.
QMutex g_repeatMutex;
QString g_lastMessage;
int g_repeats = 0;

void copyUtf8(char *destination, int capacity, const QString &value)
{
    const QByteArray utf8 = value.toUtf8();
    copyText(destination, capacity, utf8.constData(), int(utf8.size()));
}

void writeToQFile(void *context, const char *data, std::size_t size)
{
    static_cast<QFile *>(context)->write(data, qint64(size));
}

void messageHook(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (type == QtFatalMsg) {
        BlackBox::setContext("fatal", QStringLiteral("Qt fatal error: ") + message.left(200));
    } else if (type == QtWarningMsg || type == QtCriticalMsg) {
        QMutexLocker lock(&g_repeatMutex);
        if (message == g_lastMessage) {
            ++g_repeats;
        } else {
            if (g_repeats > 0) {
                BlackBox::record("qt", QStringLiteral("(the previous message repeated %1 more times)")
                                           .arg(g_repeats));
            }
            g_repeats = 0;
            g_lastMessage = message;
            BlackBox::record(type == QtCriticalMsg ? "error" : "warning", message.left(160));
        }
    }
    if (g_previousHandler != nullptr) {
        g_previousHandler(type, context, message);
    } else {
        const QByteArray local = message.toLocal8Bit();
        std::fprintf(stderr, "%s\n", local.constData());
    }
    // Logged first, so the message is in the log as well as the report.
    if (type == QtFatalMsg)
        CrashHandlerPlatform::fatalMessage();
}

[[noreturn]] void terminateHandler()
{
    QString reason = QStringLiteral(
        "std::terminate was called with no exception in flight (a noexcept function threw, or a "
        "running std::thread was destroyed)");
    if (const std::exception_ptr current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception &error) {
            reason = QStringLiteral("an exception nobody caught: ") + QString::fromLocal8Bit(error.what());
        } catch (...) {
            reason = QStringLiteral("an exception of unknown type that nobody caught");
        }
    }
    BlackBox::setContext("fatal", reason);
    std::abort();
}

// UTC, like the names the crash handlers give their reports.
[[nodiscard]] QString timestampForFileName(qint64 epochMs)
{
    return QDateTime::fromMSecsSinceEpoch(epochMs, QTimeZone::UTC)
        .toString(QStringLiteral("yyyyMMdd-HHmmss"));
}

[[nodiscard]] QString terminated(const char *value, int capacity)
{
    return QString::fromUtf8(value, int(qstrnlen(value, uint(capacity))));
}

// Explains a session that ended without any handler running, from the black
// box it left behind, and saves that as a report like any other.
PendingReport writeUncleanExitReport(const Data &previous)
{
    const Header &header = previous.header;
    const qint64 lastSign = header.heartbeatMs.load();
    const qint64 frozenSince = header.unresponsiveSinceMs.load();
    const bool froze = frozenSince != 0;
    const QString path = QDir(g_directory).filePath(
        QStringLiteral("%1-%2.txt")
            .arg(froze ? QStringLiteral("frozen") : QStringLiteral("closed"),
                 timestampForFileName(lastSign > 0 ? lastSign : QDateTime::currentMSecsSinceEpoch())));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};

    const std::uint64_t mainThread = header.mainThreadId.load();
    char doing[256];
    bool known = CrashWriter::threadActivity(previous, mainThread, doing, sizeof(doing));
    if (!known) {
        for (const Thread &thread : previous.threads) {
            if (CrashWriter::threadActivity(previous, thread.threadId.load(), doing, sizeof(doing))) {
                known = true;
                break;
            }
        }
    }

    {
        CrashWriter::Writer out(writeToQFile, &file);
        out.text(froze ? "OpenChat stopped responding" : "OpenChat closed unexpectedly").newline();
        out.text("============================").newline().newline();
        out.text("What happened:  ");
        if (froze) {
            out.text("The main thread stopped responding for ")
                .decimal((lastSign - frozenSince) / 1000 + freezeThresholdMs / 1000)
                .text(" s or more, and OpenChat was then closed before it recovered.");
        } else {
            out.text("OpenChat ended without shutting down and without recording a crash. It was "
                     "ended from outside (for example from Task Manager), the computer lost "
                     "power, or it failed in a way no crash handler can catch (such as heap "
                     "corruption that Windows stops at once).");
        }
        out.newline();
        out.text("Doing:          ").text(known ? doing : "nothing that OpenChat records").newline();
        out.text("Thread:         main").newline();
        const QString freezeReport = terminated(header.reportPath, int(sizeof(header.reportPath)));
        if (froze && !freezeReport.isEmpty())
            out.text("Freeze report:  ").text(freezeReport.toUtf8().constData()).newline();
        out.newline();
        out.text("Application:    ").field(header.application, sizeof(header.application)).newline();
        out.text("Build:          ").field(header.build, sizeof(header.build)).newline();
        out.text("System:         ").field(header.system, sizeof(header.system)).newline();
        out.text("Started:        ");
        CrashWriter::utcTime(out, header.startedMs);
        out.newline().text("Last sign of life: ");
        CrashWriter::utcTime(out, lastSign);
        out.text(" (after ");
        CrashWriter::duration(out, lastSign - header.startedMs);
        out.text(")").newline();
        out.text("Process id:     ").unsignedDecimal(header.processId).newline();
        CrashWriter::blackBoxSections(out, previous, mainThread, froze ? "froze" : "was last seen here");
    }
    file.close();
    return {froze ? PendingReport::Kind::Freeze : PendingReport::Kind::UncleanExit, path};
}

PendingReport examinePreviousSession(const QString &blackBoxPath)
{
    QFile file(blackBoxPath);
    if (!file.exists() || file.size() < qint64(sizeof(Data)) || !file.open(QIODevice::ReadOnly))
        return {};
    auto previous = std::make_unique<Data>();
    if (file.read(reinterpret_cast<char *>(previous.get()), qint64(sizeof(Data))) != qint64(sizeof(Data)))
        return {};
    const Header &header = previous->header;
    if (header.magic != magic || header.version != formatVersion || header.dataSize != sizeof(Data))
        return {};
    const std::uint32_t state = header.state.load();
    if (state == StateCrashed) {
        const QString path = terminated(header.reportPath, int(sizeof(header.reportPath)));
        if (!path.isEmpty() && QFileInfo::exists(path)
            && !QFileInfo::exists(path + QStringLiteral(".seen"))) {
            return {PendingReport::Kind::Crash, path};
        }
        return {};
    }
    if (state != StateRunning)
        return {};
    return writeUncleanExitReport(*previous);
}

// Keeps the newest reports and whatever sits beside them (minidumps, seen
// markers). A minidump is megabytes; a tester's machine should not collect
// them forever.
void pruneOldReports()
{
    QDir directory(g_directory);
    const QFileInfoList reports = directory.entryInfoList(
        {QStringLiteral("crash-*.txt"), QStringLiteral("frozen-*.txt"),
         QStringLiteral("freeze-*.txt"), QStringLiteral("closed-*.txt")},
        QDir::Files, QDir::Time);
    for (qsizetype index = keptReports; index < reports.size(); ++index) {
        const QString base = reports.at(index).completeBaseName();
        const QFileInfoList related =
            directory.entryInfoList({base + QStringLiteral(".*")}, QDir::Files);
        for (const QFileInfo &file : related)
            QFile::remove(file.absoluteFilePath());
    }
}

bool mapBlackBox(const QString &path)
{
    // Leaked on purpose: closing the file would unmap the recorder, and it
    // must stay valid until the very last instruction the process runs.
    auto *file = new QFile(path);
    if (!file->open(QIODevice::ReadWrite | QIODevice::Truncate) || !file->resize(qint64(sizeof(Data)))) {
        delete file;
        return false;
    }
    uchar *memory = file->map(0, qint64(sizeof(Data)));
    if (memory == nullptr) {
        delete file;
        return false;
    }
    std::memset(memory, 0, sizeof(Data));
    adopt(reinterpret_cast<Data *>(memory));
    return true;
}

void fillHeader()
{
    Header &header = current()->header;
    header.magic = magic;
    header.version = formatVersion;
    header.dataSize = std::uint32_t(sizeof(Data));
    header.processId = std::uint64_t(QCoreApplication::applicationPid());
    header.startedMs = nowMs();
    header.heartbeatMs.store(header.startedMs);
    header.unresponsiveSinceMs.store(0);
    header.mainThreadId.store(currentThreadId());
    copyUtf8(header.application, int(sizeof(header.application)),
             QStringLiteral("OpenChat %1").arg(QStringLiteral(OPENCHAT_VERSION)));
    copyUtf8(header.build, int(sizeof(header.build)),
             QStringLiteral("%1, compiled %2 %3")
                 .arg(QStringLiteral(OPENCHAT_BUILD_ID), QStringLiteral(__DATE__),
                      QStringLiteral(__TIME__)));
    copyUtf8(header.system, int(sizeof(header.system)),
             QStringLiteral("%1 (kernel %2), %3")
                 .arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                      QSysInfo::currentCpuArchitecture()));
    header.state.store(StateRunning);
}

void freezeWatcher()
{
    BlackBox::nameThread("freeze detector");
    qint64 lastTick = nowMs();
    bool frozen = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_cleanExit.load())
            return;
        Header &header = current()->header;
        // A crash is being reported (and may be waiting on the user to close
        // its message box): the main thread is not frozen, it is gone.
        if (header.state.load() == StateCrashed)
            return;
        const qint64 now = nowMs();
        // A gap this long in our own ticking means the machine slept; the
        // main thread did not freeze, it was simply not scheduled.
        const bool slept = now - lastTick > 5000;
        lastTick = now;
        if (slept) {
            header.heartbeatMs.store(now);
            continue;
        }
        const qint64 beat = header.heartbeatMs.load();
        if (!frozen && now - beat >= freezeThresholdMs && !CrashHandlerPlatform::debuggerAttached()) {
            frozen = true;
            header.unresponsiveSinceMs.store(beat);
            BlackBox::record("freeze", "the main thread has not responded for 10 seconds");
            char path[512];
            if (CrashHandlerPlatform::writeFreezeReport(beat, path, int(sizeof(path)))) {
                copyText(header.reportPath, int(sizeof(header.reportPath)), path,
                         int(std::strlen(path)));
            }
        } else if (frozen && now - beat < freezeThresholdMs) {
            frozen = false;
            const qint64 since = header.unresponsiveSinceMs.exchange(0);
            header.reportPath[0] = '\0';
            BlackBox::record("freeze", QStringLiteral("the main thread is responding again after %1 s")
                                           .arg((now - since) / 1000));
        }
    }
}

void markCleanExit(const char *why)
{
    BlackBox::record("app", why);
    g_cleanExit.store(true);
    current()->header.state.store(StateCleanExit);
}

[[noreturn, gnu::noinline]] void throwUncaught()
{
    throw std::runtime_error("crash self-test: an exception nobody catches");
}

void throwThroughNoexcept() noexcept
{
    throwUncaught();
}

volatile bool g_stopRecursion = false;

[[gnu::noinline]] int recurseForever(int depth)
{
    volatile char padding[1024];
    padding[0] = char(depth);
    if (g_stopRecursion)
        return padding[0];
    return recurseForever(depth + 1) + padding[0];
}

} // namespace

QString defaultDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("crashes"));
}

void install(const Options &options)
{
    if (g_installed)
        return;
    g_installed = true;
    g_options = options;
    g_directory = options.directory.isEmpty() ? defaultDirectory() : options.directory;
    QDir().mkpath(g_directory);

    BlackBox::nameThread("main");
    fillHeader();

    CrashHandlerPlatform::Config config{};
    const QByteArray directory = QDir::toNativeSeparators(g_directory).toUtf8();
    copyText(config.directory, int(sizeof(config.directory)), directory.constData(),
             int(directory.size()));
    config.viewer = options.viewer;
    // Off until startSession(): an unattended preview or test that crashes
    // must not leave a window waiting for somebody to close it. The viewer
    // itself is interactive but never relaunches.
    config.relaunch = false;
    config.interactive = options.viewer;
    CrashHandlerPlatform::rememberMainThread();
    CrashHandlerPlatform::install(config);

    std::set_terminate(terminateHandler);
    g_previousHandler = qInstallMessageHandler(messageHook);

    BlackBox::record("app", options.viewer ? "crash report viewer started" : "started");
}

void startSession()
{
    if (!g_installed || g_options.viewer || g_sessionStarted)
        return;
    g_sessionStarted = true;
    const QString blackBoxPath = QDir(g_directory).filePath(QString::fromLatin1(blackBoxName));
    // One running OpenChat owns the recorder file. A second instance would
    // read the first one's live recorder as a session that died, and would
    // truncate a file the first one has mapped; it keeps its recorder in
    // memory instead, which still goes into its own reports. Leaked on
    // purpose: the lock is held until the process ends, and a crashed
    // holder's stale lock is taken over by process id.
    auto *lock = new QLockFile(QDir(g_directory).filePath(QStringLiteral("session.lock")));
    lock->setStaleLockTime(0);
    if (lock->tryLock(0)) {
        g_pending = examinePreviousSession(blackBoxPath);
        pruneOldReports();
        // If the file cannot be mapped the recorder simply stays in memory:
        // every report still has it, only the next launch's post-mortem is lost.
        if (!mapBlackBox(blackBoxPath)) {
            std::fprintf(stderr, "OpenChat: crash recorder could not map %s\n",
                         qPrintable(blackBoxPath));
        }
    } else {
        delete lock;
        BlackBox::record("app", "another OpenChat is running; this one's recorder stays in memory");
    }
    current()->header.state.store(StateRunning);
    // OPENCHAT_CRASH_UNATTENDED: reports are still written, but nothing is put
    // on screen — for scripted runs of the crash self-tests.
    if (qEnvironmentVariableIsEmpty("OPENCHAT_CRASH_UNATTENDED"))
        CrashHandlerPlatform::setInteractive(true);
    BlackBox::record("app", QStringLiteral("interactive session started; reports go to ")
                                + QDir::toNativeSeparators(g_directory));
    if (g_pending.kind != PendingReport::Kind::None)
        BlackBox::record("app", "the previous session did not end cleanly; its report will be shown");
    // Watching starts once the event loop runs and the heartbeat timer with
    // it: a slow start (keychain, profile unlock, the first QML load) is not a
    // freeze.
    if (QCoreApplication::instance() != nullptr) {
        QTimer::singleShot(0, QCoreApplication::instance(), [] {
            BlackBox::heartbeat();
            std::thread(freezeWatcher).detach();
        });
    }
}

void attachToApplication()
{
    if (!g_installed || QCoreApplication::instance() == nullptr)
        return;
    CrashHandlerPlatform::reinstall();
    auto *heartbeat = new QTimer(QCoreApplication::instance());
    heartbeat->setInterval(500);
    QObject::connect(heartbeat, &QTimer::timeout, [] { BlackBox::heartbeat(); });
    heartbeat->start();
    BlackBox::heartbeat();
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     [] { markCleanExit("quitting normally"); });
    if (auto *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        // Logging off or shutting down ends the process without aboutToQuit on
        // some platforms; that is not a crash and must not be reported as one.
        // Recorded as clean without stopping anything: if the user cancels the
        // logout and OpenChat is still running half a minute later, the
        // session is live again and a later death must still be explained.
        QObject::connect(gui, &QGuiApplication::commitDataRequest, gui,
                         [gui](QSessionManager &) {
                             BlackBox::record("app", "the desktop session is ending");
                             current()->header.state.store(StateCleanExit);
                             QTimer::singleShot(30000, gui, [] {
                                 if (!g_cleanExit.load())
                                     current()->header.state.store(StateRunning);
                             });
                         },
                         Qt::DirectConnection);
    }
}

PendingReport pendingReport()
{
    return g_pending;
}

void markSeen(const QString &reportPath)
{
    if (reportPath.isEmpty())
        return;
    QFile marker(reportPath + QStringLiteral(".seen"));
    if (marker.open(QIODevice::WriteOnly))
        marker.close();
    if (g_pending.path == reportPath)
        g_pending = {};
}

bool isSelfTestKind(const QString &kind)
{
    static const QStringList kinds = {
        QStringLiteral("segv"),  QStringLiteral("abort"),          QStringLiteral("throw"),
        QStringLiteral("qfatal"), QStringLiteral("stack-overflow"), QStringLiteral("hang"),
        QStringLiteral("screen-frame"),
    };
    return kinds.contains(kind);
}

void runSelfTest(const QString &kind)
{
    BlackBox::record("self-test", QStringLiteral("crash self-test requested: ") + kind);
    if (kind == QStringLiteral("screen-frame"))
        return; // Armed by the caller in the capture path.
    BlackBox::Activity activity("self-test", "failing on purpose to test crash reporting");
    if (kind == QStringLiteral("segv")) {
        volatile int *nowhere = nullptr;
        *nowhere = 0x0badf00d;
    } else if (kind == QStringLiteral("abort")) {
        std::abort();
    } else if (kind == QStringLiteral("throw")) {
        // Through a noexcept frame, so it terminates the same way everywhere
        // rather than depending on how the event loop treats exceptions.
        throwThroughNoexcept();
    } else if (kind == QStringLiteral("qfatal")) {
        qFatal("crash self-test: a deliberate Qt fatal error");
    } else if (kind == QStringLiteral("stack-overflow")) {
        std::printf("%d\n", recurseForever(0));
    } else if (kind == QStringLiteral("hang")) {
        BlackBox::Activity freeze("self-test", "freezing the main thread on purpose for 45 s");
        std::this_thread::sleep_for(std::chrono::seconds(45));
        return;
    }
    std::abort();
}

} // namespace OpenChat::CrashReporter
