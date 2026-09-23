#include "diagnostics/BlackBox.h"
#include "diagnostics/BlackBoxData.h"
#include "diagnostics/CrashWriter.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

#include <atomic>
#include <string>
#include <thread>

using namespace OpenChat;

namespace {

void appendToString(void *context, const char *data, std::size_t size) noexcept
{
    static_cast<std::string *>(context)->append(data, size);
}

template <typename Function>
QString render(Function &&function)
{
    std::string text;
    {
        CrashWriter::Writer writer(appendToString, &text);
        function(writer);
    }
    return QString::fromStdString(text);
}

QString activityOf(std::uint64_t thread)
{
    char buffer[256];
    return CrashWriter::threadActivity(*BlackBoxData::current(), thread, buffer, sizeof(buffer))
        ? QString::fromUtf8(buffer)
        : QString();
}

QString contextOf(const char *area)
{
    char buffer[BlackBoxData::contextValueBytes];
    return CrashWriter::contextValue(*BlackBoxData::current(), area, buffer, sizeof(buffer))
        ? QString::fromUtf8(buffer)
        : QString();
}

#if defined(Q_OS_LINUX)
// The application, run the way a tester would, in a data directory of its own
// so nothing touches this machine's real profile or crash folder.
struct Sandbox final {
    QTemporaryDir root;

    [[nodiscard]] QString crashDirectory() const
    {
        return root.filePath(QStringLiteral("data/OpenChat/OpenChat/crashes"));
    }

    void configure(QProcess &process) const
    {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_DATA_HOME"), root.filePath(QStringLiteral("data")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), root.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("OPENCHAT_CRASH_UNATTENDED"), QStringLiteral("1"));
        process.setProcessEnvironment(environment);
        process.setProgram(QStringLiteral(OPENCHAT_EXECUTABLE));
    }

    [[nodiscard]] QStringList reports(const QString &pattern) const
    {
        return QDir(crashDirectory()).entryList({pattern}, QDir::Files, QDir::Name);
    }

    [[nodiscard]] QString read(const QString &name) const
    {
        QFile file(QDir(crashDirectory()).filePath(name));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }
};
#endif

} // namespace

class CrashReportTest final : public QObject
{
    Q_OBJECT

private slots:
    void timesAreComputedWithoutTheCalendarLibrary()
    {
        // A handler cannot call gmtime; the hand-rolled calendar must agree
        // with it across leap days and centuries.
        QCOMPARE(render([](auto &w) { CrashWriter::utcTime(w, 0); }),
                 QStringLiteral("1970-01-01 00:00:00 UTC"));
        QCOMPARE(render([](auto &w) { CrashWriter::utcTime(w, 1709164800123); }),
                 QStringLiteral("2024-02-29 00:00:00 UTC"));
        QCOMPARE(render([](auto &w) { CrashWriter::utcTime(w, 1790000000000); }),
                 QStringLiteral("2026-09-21 14:13:20 UTC"));
        QCOMPARE(render([](auto &w) { CrashWriter::utcTime(w, 4107542399000); }),
                 QStringLiteral("2100-02-28 23:59:59 UTC"));
        QCOMPARE(render([](auto &w) { CrashWriter::fileStamp(w, 1790000000000); }),
                 QStringLiteral("20260921-141320"));
        QCOMPARE(render([](auto &w) { CrashWriter::duration(w, 3725000); }),
                 QStringLiteral("1 h 2 min 5 s"));
        QCOMPARE(render([](auto &w) { CrashWriter::duration(w, 4200); }), QStringLiteral("4 s"));
        QCOMPARE(render([](auto &w) { w.hex(0x1a2b, 8).text(" ").decimal(-42); }),
                 QStringLiteral("0x00001a2b -42"));
    }

    void recentEventsAreKeptOldestFirstAndTheRingWraps()
    {
        for (int index = 0; index < 200; ++index)
            BlackBox::record("test", QStringLiteral("event %1").arg(index));
        const QString sections = render([](auto &w) {
            CrashWriter::blackBoxSections(w, *BlackBoxData::current(), 0);
        });
        const qsizetype first = sections.indexOf(QStringLiteral("test: event 72\n"));
        const qsizetype last = sections.indexOf(QStringLiteral("test: event 199\n"));
        QVERIFY2(first >= 0, "the oldest event still in the ring is missing");
        QVERIFY(last > first);
        QVERIFY2(!sections.contains(QStringLiteral("test: event 71\n")),
                 "an event older than the ring holds survived");
    }

    void contextsCanBeSetReplacedAndCleared()
    {
        BlackBox::setContext("screen share", QStringLiteral("Windows Desktop Duplication, display 1"));
        QCOMPARE(contextOf("screen share"), QStringLiteral("Windows Desktop Duplication, display 1"));
        BlackBox::setContext("screen share", QStringLiteral("Windows.Graphics.Capture, a window"));
        QCOMPARE(contextOf("screen share"), QStringLiteral("Windows.Graphics.Capture, a window"));
        BlackBox::setContext("screen share", QString());
        QVERIFY(contextOf("screen share").isEmpty());
    }

    void activitiesNestAndComeBack()
    {
        const std::uint64_t self = BlackBoxData::currentThreadId();
        {
            BlackBox::Activity outer("screen share", "pulling a frame");
            QCOMPARE(activityOf(self), QStringLiteral("screen share: pulling a frame"));
            {
                BlackBox::Activity inner("screen share", "drawing the mouse pointer");
                QCOMPARE(activityOf(self), QStringLiteral("screen share: drawing the mouse pointer"));
            }
            QCOMPARE(activityOf(self), QStringLiteral("screen share: pulling a frame"));
        }
        QVERIFY(activityOf(self).isEmpty());
    }

    void anotherThreadIsDescribedWhileItLivesAndForgottenAfter()
    {
        std::atomic<std::uint64_t> id{0};
        std::atomic<bool> release{false};
        std::thread worker([&] {
            BlackBox::nameThread("worker");
            BlackBox::Activity activity("test", "waiting to be released");
            id.store(BlackBoxData::currentThreadId());
            while (!release.load())
                std::this_thread::yield();
        });
        QTRY_VERIFY(id.load() != 0);
        QCOMPARE(activityOf(id.load()), QStringLiteral("test: waiting to be released"));
        char name[BlackBoxData::threadNameBytes];
        QVERIFY(CrashWriter::threadName(*BlackBoxData::current(), id.load(), name, sizeof(name)));
        QCOMPARE(QString::fromUtf8(name), QStringLiteral("worker"));
        release.store(true);
        worker.join();
        // Its slot went back, so short-lived threads cannot use the table up.
        QVERIFY(!CrashWriter::threadName(*BlackBoxData::current(), id.load(), name, sizeof(name)));
    }

    void textIsCutAtACharacterBoundary()
    {
        char buffer[6];
        // "aé€" is 1 + 2 + 3 bytes; five fit, and the euro sign must not be cut.
        const char text[] = "a\xc3\xa9\xe2\x82\xac";
        BlackBoxData::copyText(buffer, int(sizeof(buffer)), text, int(sizeof(text) - 1));
        QCOMPARE(QByteArray(buffer), QByteArray("a\xc3\xa9"));
    }

    void crashesInGraphicsDriversAndOverlaysGetAHint()
    {
        QVERIFY(QString::fromUtf8(CrashWriter::moduleHint("nvwgf2umx.dll")).contains(QStringLiteral("NVIDIA")));
        QVERIFY(QString::fromUtf8(CrashWriter::moduleHint("igd10iumd64.dll")).contains(QStringLiteral("Intel")));
        QVERIFY(QString::fromUtf8(CrashWriter::moduleHint("amdxx64.dll")).contains(QStringLiteral("AMD")));
        QVERIFY(QString::fromUtf8(CrashWriter::moduleHint("RTSSHooks64.dll")).contains(QStringLiteral("RivaTuner")));
        QVERIFY(CrashWriter::moduleHint("OpenChat.exe") == nullptr);
        QVERIFY(CrashWriter::moduleHint(nullptr) == nullptr);
    }

#if defined(Q_OS_LINUX)
    void aRealCrashWritesAReportThatSaysWhatOpenChatWasDoing()
    {
        Sandbox sandbox;
        QVERIFY(sandbox.root.isValid());
        QProcess process;
        sandbox.configure(process);
        process.setArguments({QStringLiteral("--crash-test"), QStringLiteral("segv")});
        process.start();
        QVERIFY(process.waitForFinished(60000));
        const QStringList reports = sandbox.reports(QStringLiteral("crash-*.txt"));
        if (reports.isEmpty() && process.exitStatus() == QProcess::NormalExit)
            QSKIP("OpenChat could not start here (no keychain?), so it never reached the crash");
        QCOMPARE(process.exitStatus(), QProcess::CrashExit);
        QCOMPARE(reports.size(), 1);
        const QString report = sandbox.read(reports.first());
        QVERIFY(report.startsWith(QStringLiteral("OpenChat crash report\n")));
        QVERIFY(report.contains(QStringLiteral("What happened:  The program tried to use memory at "
                                               "address 0x0, which is not valid: a null pointer.")));
        QVERIFY(report.contains(QStringLiteral("Doing:          self-test: failing on purpose")));
        QVERIFY(report.contains(QStringLiteral("Thread:         main")));
        QVERIFY(report.contains(QStringLiteral("<-- this thread crashed")));
        QVERIFY(report.contains(QStringLiteral("self-test: crash self-test requested: segv")));
        QVERIFY(report.contains(QStringLiteral("Stack of the crashed thread")));
    }

    void aSessionThatWasKilledIsExplainedOnTheNextLaunch()
    {
        Sandbox sandbox;
        QVERIFY(sandbox.root.isValid());
        QProcess frozen;
        sandbox.configure(frozen);
        frozen.setArguments({QStringLiteral("--crash-test"), QStringLiteral("hang")});
        frozen.start();
        QVERIFY(frozen.waitForStarted());
        // Long enough to be inside the deliberate freeze, far short of the
        // freeze detector's ten seconds: this is a plain kill.
        QTest::qWait(4000);
        if (frozen.state() != QProcess::Running)
            QSKIP("OpenChat could not start here (no keychain?)");
        frozen.kill();
        QVERIFY(frozen.waitForFinished(10000));
        QVERIFY(sandbox.reports(QStringLiteral("*.txt")).isEmpty());

        QProcess next;
        sandbox.configure(next);
        next.start();
        QVERIFY(next.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(!sandbox.reports(QStringLiteral("closed-*.txt")).isEmpty(), 15000);
        next.terminate();
        QVERIFY(next.waitForFinished(10000));
        const QString report = sandbox.read(sandbox.reports(QStringLiteral("closed-*.txt")).first());
        QVERIFY(report.startsWith(QStringLiteral("OpenChat closed unexpectedly\n")));
        QVERIFY(report.contains(QStringLiteral("Doing:          self-test: freezing the main thread")));
        QVERIFY(report.contains(QStringLiteral("self-test: crash self-test requested: hang")));

        // That launch ended on SIGTERM, which is a deliberate end: the one
        // after it has nothing to explain.
        QProcess third;
        sandbox.configure(third);
        third.start();
        QVERIFY(third.waitForStarted());
        QTest::qWait(3000);
        third.terminate();
        QVERIFY(third.waitForFinished(10000));
        QCOMPARE(sandbox.reports(QStringLiteral("closed-*.txt")).size(), 1);
    }
#endif
};

QTEST_GUILESS_MAIN(CrashReportTest)
#include "tst_crashreport.moc"
