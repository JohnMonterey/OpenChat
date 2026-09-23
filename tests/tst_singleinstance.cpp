#include "app/SingleInstance.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QTest>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace OpenChat;
using namespace std::chrono_literals;

namespace {

// `tst_singleinstance --hold-lock <directory> <milliseconds>`: another
// process holding the instance lock the way a running OpenChat does, without
// answering. It lets go after the given time, or waits to be killed, so a
// test can leave a lock behind whose owner has died.
int holdLock(const char *directory, const char *milliseconds)
{
    QLockFile lock(QDir(QString::fromLocal8Bit(directory)).filePath(QStringLiteral("instance.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 1;
    std::printf("locked\n");
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(milliseconds)));
    return 0;
}

QString lockPath(const QTemporaryDir &directory)
{
    return directory.filePath(QStringLiteral("instance.lock"));
}

// Starts the holder and waits until it has the lock.
bool startHolder(QProcess &holder, const QTemporaryDir &directory, int milliseconds)
{
    holder.start(QCoreApplication::applicationFilePath(),
                 {QStringLiteral("--hold-lock"), directory.path(), QString::number(milliseconds)});
    return holder.waitForStarted() && holder.waitForReadyRead(10000)
        && holder.readLine().trimmed() == "locked";
}

} // namespace

class SingleInstanceTest final : public QObject
{
    Q_OBJECT

private slots:
    void theFirstLaunchRuns()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SingleInstance first(directory.path());
        QCOMPARE(first.claim(0ms), SingleInstance::Claim::Primary);
        QVERIFY(QFileInfo::exists(lockPath(directory)));
    }

    void aSecondLaunchHandsOverAndTheFirstShowsItself()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SingleInstance first(directory.path());
        QCOMPARE(first.claim(0ms), SingleInstance::Claim::Primary);
        QSignalSpy shown(&first, &SingleInstance::activationRequested);

        SingleInstance second(directory.path());
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(second.claim(10s), SingleInstance::Claim::HandedOver);
        // Handed over at once, not after waiting out the lock.
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
        QTRY_COMPARE(shown.count(), 1);
    }

    void aLaunchWaitsForThePreviousOneToFinishClosing()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // The previous OpenChat, closing: it holds the lock but has stopped
        // answering, and lets go a moment later.
        QProcess closing;
        QVERIFY(startHolder(closing, directory, 600));

        SingleInstance next(directory.path());
        QElapsedTimer timer;
        timer.start();
        const SingleInstance::Claim claim = next.claim(10s);
        const qint64 waited = timer.elapsed();
        closing.waitForFinished();
        QCOMPARE(claim, SingleInstance::Claim::Primary);
        QVERIFY2(waited >= 200, qPrintable(QString::number(waited)));
    }

    void aClosingInstanceNoLongerTakesLaunches()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        SingleInstance first(directory.path());
        QCOMPARE(first.claim(0ms), SingleInstance::Claim::Primary);
        QSignalSpy shown(&first, &SingleInstance::activationRequested);
        first.stopAnswering();

        // Still holding the lock but gone from the socket: nobody to hand to.
        SingleInstance second(directory.path());
        QCOMPARE(second.claim(200ms), SingleInstance::Claim::StillRunning);
        QCoreApplication::processEvents();
        QCOMPARE(shown.count(), 0);

        first.release();
        SingleInstance third(directory.path());
        QCOMPARE(third.claim(0ms), SingleInstance::Claim::Primary);
    }

    void aStuckInstanceIsReportedRatherThanWaitedForForever()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess stuck;
        QVERIFY(startHolder(stuck, directory, 60000));

        SingleInstance next(directory.path());
        QElapsedTimer timer;
        timer.start();
        const SingleInstance::Claim claim = next.claim(300ms);
        const qint64 elapsed = timer.elapsed();
        stuck.kill();
        stuck.waitForFinished();
        QCOMPARE(claim, SingleInstance::Claim::StillRunning);
        QVERIFY2(elapsed < 5000, qPrintable(QString::number(elapsed)));
    }

    void anInstanceThatDiedIsTakenOver()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess holder;
        QVERIFY(startHolder(holder, directory, 60000));
        holder.kill();
        QVERIFY(holder.waitForFinished());
        QVERIFY(QFileInfo::exists(lockPath(directory)));

        SingleInstance next(directory.path());
        QCOMPARE(next.claim(0ms), SingleInstance::Claim::Primary);
    }

    // A lock left by an OpenChat whose process id the system has since given
    // to this launch names a live owner: us. It is a leftover all the same.
    void aLeftoverLockNamingThisProcessIsTakenOver()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QByteArray contents;
        {
            QLockFile earlier(lockPath(directory));
            QVERIFY(earlier.tryLock(0));
            QFile file(lockPath(directory));
            QVERIFY(file.open(QIODevice::ReadOnly));
            contents = file.readAll();
        }
        QFile leftover(lockPath(directory));
        QVERIFY(leftover.open(QIODevice::WriteOnly));
        leftover.write(contents);
        leftover.close();

        SingleInstance next(directory.path());
        QElapsedTimer timer;
        timer.start();
        QCOMPARE(next.claim(10s), SingleInstance::Claim::Primary);
        // Taken over at once, not after waiting out the lock.
        QVERIFY2(timer.elapsed() < 3000, qPrintable(QString::number(timer.elapsed())));
    }

    // The whole thing, with the real application: start OpenChat, start it
    // again, and the second one hands over and exits while the first runs on.
    void startingOpenChatTwiceLeavesOneRunning()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_DATA_HOME"), root.filePath(QStringLiteral("data")));
        environment.insert(QStringLiteral("XDG_CONFIG_HOME"), root.filePath(QStringLiteral("config")));
        environment.insert(QStringLiteral("XDG_CACHE_HOME"), root.filePath(QStringLiteral("cache")));
        environment.insert(QStringLiteral("OPENCHAT_CRASH_UNATTENDED"), QStringLiteral("1"));
        // Nothing listens there: the test never talks to a real relay.
        environment.insert(QStringLiteral("OPENCHAT_RELAY_BASE_URL"),
                           QStringLiteral("https://127.0.0.1:9/v1"));
        const QStringList arguments = {QStringLiteral("-platform"), QStringLiteral("offscreen")};
        const QString instanceLock = QDir(root.filePath(QStringLiteral("data")))
                                         .filePath(QStringLiteral("OpenChat/OpenChat/instance.lock"));

        QProcess first;
        first.setProcessEnvironment(environment);
        first.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        first.start(QStringLiteral(OPENCHAT_EXECUTABLE), arguments);
        QVERIFY(first.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(instanceLock)
                                     || first.state() != QProcess::Running,
                                 20000);
        if (first.state() != QProcess::Running)
            QSKIP("OpenChat did not start here (no system keychain?)");

        QProcess second;
        second.setProcessEnvironment(environment);
        second.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QElapsedTimer timer;
        timer.start();
        second.start(QStringLiteral(OPENCHAT_EXECUTABLE), arguments);
        const bool secondFinished = second.waitForFinished(15000);
        const bool firstStillRunning = first.state() == QProcess::Running;
        first.kill();
        first.waitForFinished();
        if (!secondFinished) {
            second.kill();
            second.waitForFinished();
        }

        QVERIFY2(secondFinished, "the second OpenChat kept running alongside the first");
        QCOMPARE(second.exitStatus(), QProcess::NormalExit);
        QCOMPARE(second.exitCode(), 0);
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
        QVERIFY(firstStillRunning);
    }
};

int main(int argc, char *argv[])
{
    if (argc == 4 && std::strcmp(argv[1], "--hold-lock") == 0)
        return holdLock(argv[2], argv[3]);
    QCoreApplication application(argc, argv);
    SingleInstanceTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_singleinstance.moc"
