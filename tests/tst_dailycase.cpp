#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QLockFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include "case/DailyCaseController.h"
#include "cosmetics/CosmeticCatalog.h"

using namespace OpenChat;
namespace {
QString accountStem(const QString &directory, const QByteArray &account)
{
    return directory + '/' + QString::fromLatin1(
        QCryptographicHash::hash(account, QCryptographicHash::Sha256).toHex());
}
bool writeClaim(const QString &directory, const QByteArray &account, const QJsonObject &claim)
{
    QFile file(accountStem(directory, account) + ".json");
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(QJsonDocument(claim).toJson()) > 0;
}
}
class DailyCaseTest : public QObject {
    Q_OBJECT
private slots:
    void audioHasOneVoiceAndSilenceBetweenCrossings()
    {
        CaseSampleStream stream;
        QCOMPARE(stream.read(8), QByteArray(8, '\0'));
        stream.trigger("tick");
        QCOMPARE(stream.read(2), QByteArray("ti"));
        stream.trigger("hit"); // Replaces the voice; cannot queue overlapping ticks.
        QCOMPARE(stream.read(6), QByteArray("hit\0\0\0", 6));
        QCOMPARE(stream.read(8), QByteArray(8, '\0'));
    }
    void retriggerCrossfadesTheOutgoingVoice()
    {
        CaseSampleStream stream;
        stream.setCrossfadeBytes(4);
        stream.trigger("\x40\x40\x40\x40");
        QCOMPARE(stream.read(2), QByteArray("\x40\x40", 2));
        stream.trigger("\x01\x01\x01\x01"); // A mid-decay cut would click.
        QCOMPARE(stream.read(2), QByteArray("\x41\x41", 2)); // 0x0101 + unfaded 0x4040
        QCOMPARE(stream.read(2), QByteArray("\x01\x01", 2)); // window over: one voice again
        QCOMPARE(stream.read(4), QByteArray(4, '\0'));
    }
    void retriggerMixSaturatesInsteadOfWrapping()
    {
        CaseSampleStream stream;
        stream.setCrossfadeBytes(4);
        stream.trigger("\xff\x7f\xff\x7f"); // two full-scale words
        QCOMPARE(stream.read(2), QByteArray("\xff\x7f", 2));
        stream.trigger("\xff\x7f\xff\x7f"); // 32767 + fading 32767 must clamp, not wrap
        QCOMPARE(stream.read(4), QByteArray("\xff\x7f\xff\x7f", 4));
        QCOMPARE(stream.read(2), QByteArray(2, '\0'));
    }
    void claimsSurviveRestartAndAreIsolatedByAccount()
    {
        QTemporaryDir directory;
        LocalDailyCaseService first(directory.path()), second(directory.path());
        QVERIFY(!first.status("alice").result);
        const auto claim = first.claim("alice");
        QVERIFY(claim.newlyClaimed);
        QVERIFY(claim.result);
        const auto duplicate = second.claim("alice");
        QVERIFY(!duplicate.newlyClaimed);
        QCOMPARE(duplicate.result->claimId, claim.result->claimId);
        QCOMPARE(duplicate.result->seed, claim.result->seed);
        QCOMPARE(first.status("alice").result->claimId, claim.result->claimId);
        QVERIFY(second.claim("bob").result->claimId != claim.result->claimId);
        // A new account's first case was the one waiting; none waits now.
        QCOMPARE(claim.drops, 0);
        QCOMPARE(caseDropIntervalMs, qint64(30 * 60 * 1000));
    }
    void casesDropWithRunningTimeAndWaitToBeOpened()
    {
        QTemporaryDir directory;
        const qint64 interval = 1000;
        LocalDailyCaseService service(directory.path(), interval);
        // One case waits for a new account, and nothing counts toward the next yet.
        const auto offered = service.status("alice");
        QVERIFY(!offered.result);
        QCOMPARE(offered.drops, 1);
        QCOMPARE(offered.progressMs, 0);
        const auto claim = service.claim("alice");
        QVERIFY(claim.newlyClaimed);
        QCOMPARE(claim.drops, 0);
        // The opened case keeps the key it was offered under; the next has its own.
        QCOMPARE(claim.result->caseKey, offered.caseKey);
        QVERIFY(claim.caseKey != offered.caseKey);
        // With none waiting, asking again replays the last one.
        const auto replay = service.claim("alice");
        QVERIFY(!replay.newlyClaimed);
        QCOMPARE(replay.result->claimId, claim.result->claimId);
        // Running time counts toward the next drop...
        QCOMPARE(service.accrue("alice", 400).progressMs, 400);
        QCOMPARE(service.status("alice").drops, 0);
        // ...and one report credits no more than the drop still needs, so a
        // machine left asleep for hours earns one case, not a stack.
        const auto dropped = service.accrue("alice", 5 * interval);
        QCOMPARE(dropped.drops, 1);
        QCOMPARE(dropped.progressMs, 0);
        QCOMPARE(service.accrue("alice", -50).drops, 1);
        // Unopened drops stack, and survive a restart.
        QCOMPARE(service.accrue("alice", interval).drops, 2);
        QCOMPARE(service.accrue("alice", 250).progressMs, 250);
        LocalDailyCaseService restarted(directory.path(), interval);
        QCOMPARE(restarted.status("alice").drops, 2);
        QCOMPARE(restarted.status("alice").progressMs, 250);
        // Each open takes one.
        const auto next = restarted.claim("alice");
        QVERIFY(next.newlyClaimed);
        QVERIFY(next.result->claimId != claim.result->claimId);
        QCOMPARE(next.result->caseKey, claim.caseKey);
        QCOMPARE(next.drops, 1);
        QVERIFY(restarted.claim("alice").newlyClaimed);
        QCOMPARE(restarted.status("alice").drops, 0);
        // Other accounts count their own.
        QCOMPARE(restarted.status("bob").drops, 1);
    }
    void aClaimSavedByACooldownBuildLeavesOneCaseIfItWasDue()
    {
        QTemporaryDir directory;
        // Due already: one case waits.
        QVERIFY(writeClaim(directory.path(), "alice", {{"claim", "daily"}, {"reward", "bead.star"},
            {"seed", 7.0}, {"next", QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODate)}}));
        // Still waiting for midnight: none does, and the claim stays on show.
        QVERIFY(writeClaim(directory.path(), "bob", {{"claim", "hourly"}, {"reward", "frame.neon"},
            {"seed", 7.0}, {"next", QDateTime::currentDateTimeUtc().addSecs(3600).toString(Qt::ISODate)}}));
        LocalDailyCaseService service(directory.path());
        const auto alice = service.status("alice");
        QVERIFY(alice.error.isEmpty());
        QCOMPARE(alice.drops, 1);
        QCOMPARE(alice.result->claimId, QStringLiteral("daily"));
        QCOMPARE(alice.owned, QStringList{"bead.star"});
        QVERIFY(service.claim("alice").newlyClaimed);
        const auto bob = service.status("bob");
        QCOMPARE(bob.drops, 0);
        QCOMPARE(bob.result->rewardId, QStringLiteral("frame.neon"));
        QVERIFY(!service.claim("bob").newlyClaimed);
    }
    void lockAndCorruptionFailClosed()
    {
        QTemporaryDir directory;
        LocalDailyCaseService service(directory.path());
        const auto path = directory.path() + '/' + QString::fromLatin1(
            QCryptographicHash::hash("alice", QCryptographicHash::Sha256).toHex()) + ".json";
        QLockFile lock(path + ".lock");
        QVERIFY(lock.tryLock(0));
        QVERIFY(!service.claim("alice").error.isEmpty());
        lock.unlock();
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("broken");
        file.close();
        QVERIFY(!service.claim("alice").error.isEmpty());
        QVERIFY(!service.claim("alice").result);
    }
    void curveAcceleratesThenSlowsWithoutOvershoot()
    {
        QCOMPARE(CaseMotion::progress(0), 0.0);
        QVERIFY(std::abs(CaseMotion::progress(1) - 1) < 1e-12);
        double previous = 0;
        for (int i = 1; i <= 10000; ++i) {
            const auto p = CaseMotion::progress(i / 10000.0);
            QVERIFY(p >= previous && p <= 1.000000000001);
            previous = p;
        }
        QVERIFY(CaseMotion::progress(.002) - CaseMotion::progress(.001)
                > CaseMotion::progress(.001));
        const double tailTravel = 48 * (1 - CaseMotion::progress(.8));
        QVERIFY(tailTravel > .3 && tailTravel < .5);
        QVERIFY(CaseMotion::progress(.91) - CaseMotion::progress(.90)
                < CaseMotion::progress(.81) - CaseMotion::progress(.80));
        // At 30/60/144 Hz crossings remain monotonic, bounded and end at the
        // same predetermined card. The tail has progressively longer intervals.
        for (const int fps : {30, 60, 144}) {
            int selected = CaseMotion::startIndex;
            QList<double> times;
            for (int frame = 1; frame <= fps * 8; ++frame) {
                const double t = std::min(1.0, frame * 1000.0 / (fps * CaseMotion::durationMs));
                int current = CaseMotion::selectedIndex(8 + 48 * CaseMotion::progress(t));
                if (current != selected) {
                    QCOMPARE(current, selected + 1);
                    times.append(t);
                    selected = current;
                }
            }
            QCOMPARE(selected, 56);
            QCOMPARE(times.size(), 48);
            const auto n = times.size();
            QVERIFY(times[n-1] - times[n-2] > times[n-2] - times[n-3]);
        }
    }
    void claimsDrawACatalogueRewardAndReplayIt()
    {
        QTemporaryDir directory;
        LocalDailyCaseService service(directory.path());
        const auto claim = service.claim("alice");
        QVERIFY(claim.result);
        const auto *reward = CosmeticCatalog::find(claim.result->rewardId);
        QVERIFY2(reward, qPrintable(claim.result->rewardId));
        QCOMPARE(service.status("alice").result->rewardId, reward->id);
        QCOMPARE(LocalDailyCaseService(directory.path()).claim("alice").result->rewardId, reward->id);
    }
    void aClaimSavedBeforeRewardsKeepsThePlaceholder()
    {
        QTemporaryDir directory;
        const auto path = directory.path() + '/' + QString::fromLatin1(
            QCryptographicHash::hash("alice", QCryptographicHash::Sha256).toHex()) + ".json";
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject{{"claim", "earlier"}, {"seed", 7.0},
            {"next", QDateTime::currentDateTimeUtc().addSecs(1800).toString(Qt::ISODate)}}).toJson());
        file.close();
        QCOMPARE(LocalDailyCaseService(directory.path()).status("alice").result->rewardId,
                 QStringLiteral("placeholder"));
        DailyCaseController controller(std::make_unique<LocalDailyCaseService>(directory.path()));
        controller.setAccountKey("alice");
        QCOMPARE(controller.state(), DailyCaseController::Opened);
        QVERIFY(controller.reward().isEmpty());
    }
    void unboxedItemsStayInTheAccountsCollection()
    {
        QTemporaryDir directory;
        LocalDailyCaseService service(directory.path());
        const auto before = service.status("alice");
        QVERIFY(before.error.isEmpty());
        QCOMPARE(before.owned, QStringList());
        const auto first = service.claim("alice");
        QVERIFY(first.result);
        QCOMPARE(first.owned, QStringList{first.result->rewardId});
        // Kept across restarts and repeat requests, and nobody else's.
        QCOMPARE(LocalDailyCaseService(directory.path()).status("alice").owned, first.owned);
        QCOMPARE(service.claim("alice").owned, first.owned);
        QCOMPARE(service.status("bob").owned, QStringList());
        // The next case's item joins it; one already owned is not kept twice.
        QCOMPARE(service.accrue("alice", caseDropIntervalMs).drops, 1);
        QCOMPARE(service.status("alice").owned, first.owned);
        const auto second = service.claim("alice");
        QVERIFY(second.newlyClaimed);
        QStringList expected = first.owned;
        if (!expected.contains(second.result->rewardId))
            expected.append(second.result->rewardId);
        QCOMPARE(second.owned, expected);
        QCOMPARE(LocalCosmeticInventory(directory.path()).owned("alice").value_or(QStringList{"unread"}),
                 expected);
    }
    void aClaimSavedBeforeCollectionsHandsItsRewardOver()
    {
        QTemporaryDir directory;
        // Yesterday's claim, from before anything was kept: the item is the
        // account's now, whether or not the claim is still today's.
        QVERIFY(writeClaim(directory.path(), "alice", {{"claim", "earlier"}, {"reward", "frame.neon"},
            {"seed", 7.0}, {"next", QDateTime::currentDateTimeUtc().addDays(-1).toString(Qt::ISODate)}}));
        LocalDailyCaseService service(directory.path());
        const auto status = service.status("alice");
        QCOMPARE(status.result->claimId, QStringLiteral("earlier"));
        QCOMPARE(status.owned, QStringList{"frame.neon"});
        QCOMPARE(LocalCosmeticInventory(directory.path()).owned("alice").value_or(QStringList{}),
                 QStringList{"frame.neon"});
        // A claim saved before rewards existed hands nothing over.
        QVERIFY(writeClaim(directory.path(), "bob", {{"claim", "earlier"}, {"seed", 7.0},
            {"next", QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}}));
        QCOMPARE(service.status("bob").owned, QStringList());
    }
    void anUnreadableCollectionFailsClosed()
    {
        QTemporaryDir directory;
        QFile file(accountStem(directory.path(), "alice") + ".owned.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("broken");
        file.close();
        LocalDailyCaseService service(directory.path());
        QVERIFY(!service.status("alice").error.isEmpty());
        const auto claim = service.claim("alice");
        QVERIFY(!claim.error.isEmpty());
        QVERIFY(!claim.result);
        QVERIFY(!LocalCosmeticInventory(directory.path()).grant("alice", {"frame.neon"}));
        DailyCaseController controller(std::make_unique<LocalDailyCaseService>(directory.path()));
        controller.setAccountKey("alice");
        QVERIFY(!controller.ownershipKnown());
        QCOMPARE(controller.owned(), QStringList());
    }
    void theControllerReportsTheAccountsCollection()
    {
        QTemporaryDir directory;
        QVERIFY(LocalCosmeticInventory(directory.path()).grant("alice", {"bead.gem", "bead.gem", "frame.neon"}));
        DailyCaseController controller(std::make_unique<LocalDailyCaseService>(directory.path()));
        controller.setProperty("muted", true);
        controller.setProperty("reducedMotion", true);
        QVERIFY(!controller.ownershipKnown());
        QSignalSpy changed(&controller, &DailyCaseController::ownedChanged);
        controller.setAccountKey("alice");
        QVERIFY(controller.ownershipKnown());
        QCOMPARE(controller.owned(), (QStringList{"bead.gem", "frame.neon"}));
        QCOMPARE(changed.size(), 1);
        // The unboxed item is in the collection as soon as the claim is recorded.
        controller.open();
        QCOMPARE(controller.state(), DailyCaseController::Opened);
        QVERIFY(controller.owned().contains(controller.reward().value("id").toString()));
        // A failed reply (another window mid-claim) keeps what is known.
        const QStringList known = controller.owned();
        QLockFile lock(accountStem(directory.path(), "alice") + ".json.lock");
        QVERIFY(lock.tryLock(0));
        controller.refresh();
        QVERIFY(!controller.error().isEmpty());
        QVERIFY(controller.ownershipKnown());
        QCOMPARE(controller.owned(), known);
        lock.unlock();
        // Another account never sees this one's collection.
        controller.setAccountKey("bob");
        QVERIFY(controller.ownershipKnown());
        QCOMPARE(controller.owned(), QStringList());
    }
    void theBeltIsTheCasesAndStaysPutWhenTheClaimLands()
    {
        QTemporaryDir directory;
        DailyCaseController first(std::make_unique<LocalDailyCaseService>(directory.path()));
        DailyCaseController again(std::make_unique<LocalDailyCaseService>(directory.path()));
        DailyCaseController other(std::make_unique<LocalDailyCaseService>(directory.path()));
        first.setAccountKey("alice");
        again.setAccountKey("alice");
        other.setAccountKey("bob");
        const auto belt = first.fillers();
        QCOMPARE(belt.size(), first.tileCount());
        QCOMPARE(again.fillers(), belt);
        QVERIFY(other.fillers() != belt);
        for (const auto &tile : belt)
            QVERIFY(CosmeticCatalog::find(tile.toMap().value("id").toString()));
        QVERIFY(first.reward().isEmpty());
        first.setProperty("muted", true);
        first.setProperty("reducedMotion", true);
        QSignalSpy rearranged(&first, &DailyCaseController::fillersChanged);
        first.open();
        QCOMPARE(first.state(), DailyCaseController::Opened);
        QCOMPARE(first.fillers(), belt);
        QCOMPARE(rearranged.size(), 0);
        QCOMPARE(first.reward().value("id").toString(),
                 LocalDailyCaseService(directory.path()).status("alice").result->rewardId);
        QVERIFY(first.reward().value("rarityColor").value<QColor>().isValid());
        // Another window, and this one refreshed, still show this case's belt.
        again.refresh();
        QCOMPARE(again.state(), DailyCaseController::Opened);
        QCOMPARE(again.fillers(), belt);
        first.refresh();
        QCOMPARE(first.fillers(), belt);
        // The next case drops after half an hour of running.
        QCOMPARE(first.drops(), 0);
        const qint64 wait = QDateTime::currentDateTimeUtc().msecsTo(first.nextDropAt());
        QVERIFY2(wait > caseDropIntervalMs - 5000 && wait <= caseDropIntervalMs, qPrintable(QString::number(wait)));
        // Once it has, the reveal on show stays until the popup closes; then
        // the new case waits, with a belt of its own.
        QCOMPARE(LocalDailyCaseService(directory.path()).accrue("alice", caseDropIntervalMs).drops, 1);
        first.refresh();
        QCOMPARE(first.drops(), 1);
        QCOMPARE(first.state(), DailyCaseController::Opened);
        QCOMPARE(first.fillers(), belt);
        first.dismiss();
        QCOMPARE(first.state(), DailyCaseController::Available);
        QVERIFY(first.reward().isEmpty());
        QVERIFY(first.fillers() != belt);
        QCOMPARE(rearranged.size(), 1);
        again.refresh();
        QCOMPARE(again.state(), DailyCaseController::Available);
        QCOMPARE(again.fillers(), first.fillers());
    }
    void casesDropOnlyWhileTheAppIsOpen()
    {
        QTemporaryDir directory;
        const qint64 interval = 600;
        const auto service = [&] { return std::make_unique<LocalDailyCaseService>(directory.path(), interval); };
        {
            DailyCaseController running(service());
            running.setProperty("muted", true);
            running.setProperty("reducedMotion", true);
            running.setAccountKey("alice");
            // A case to start with; opening it leaves none.
            QCOMPARE(running.drops(), 1);
            running.open();
            QCOMPARE(running.drops(), 0);
            QVERIFY(running.nextDropAt().isValid());
            // While it runs, a case drops every interval, and they stack.
            QTRY_COMPARE_WITH_TIMEOUT(running.drops(), 1, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(running.drops(), 2, 5000);
            // The reveal stays on show until dismissed; then a case waits.
            QCOMPARE(running.state(), DailyCaseController::Opened);
            running.dismiss();
            QCOMPARE(running.state(), DailyCaseController::Available);
            QTest::qWait(int(interval / 3));
        }
        // Closed: the time it ran toward the next case is kept, and nothing more
        // is added however long it stays closed.
        const auto closed = LocalDailyCaseService(directory.path(), interval).status("alice");
        QCOMPARE(closed.drops, 2);
        QVERIFY2(closed.progressMs >= interval / 4 && closed.progressMs < interval,
                 qPrintable(QString::number(closed.progressMs)));
        QTest::qWait(int(3 * interval));
        const auto later = LocalDailyCaseService(directory.path(), interval).status("alice");
        QCOMPARE(later.drops, 2);
        QCOMPARE(later.progressMs, closed.progressMs);
        // Opened again, it picks up where it left off.
        DailyCaseController reopened(service());
        reopened.setAccountKey("alice");
        QCOMPARE(reopened.drops(), 2);
        QVERIFY(QDateTime::currentDateTimeUtc().msecsTo(reopened.nextDropAt()) <= interval - closed.progressMs);
        QTRY_COMPARE_WITH_TIMEOUT(reopened.drops(), 3, 5000);
        // Switching accounts keeps each one's running time apart.
        reopened.setAccountKey("bob");
        QCOMPARE(reopened.drops(), 1);
        QVERIFY(LocalDailyCaseService(directory.path(), interval).status("alice").drops >= 3);
    }
    void duplicateClickDismissAndReducedMotion()
    {
        QTemporaryDir directory;
        DailyCaseController controller(std::make_unique<LocalDailyCaseService>(directory.path()));
        controller.setProperty("muted", true);
        controller.setProperty("reducedMotion", false);
        controller.setAccountKey("alice");
        QSignalSpy reveal(&controller, &DailyCaseController::revealed);
        controller.open();
        QCOMPARE(controller.state(), DailyCaseController::Opening);
        const auto winner = controller.winnerIndex();
        controller.open();
        QCOMPARE(controller.winnerIndex(), winner);
        controller.dismiss();
        QCOMPARE(controller.state(), DailyCaseController::Opened);
        QCOMPARE(controller.position(), double(winner));
        QSignalSpy movement(&controller, &DailyCaseController::positionChanged);
        QTest::qWait(80);
        QCOMPARE(movement.size(), 0);
        QCOMPARE(reveal.size(), 0);
        controller.refresh();
        controller.open();
        QCOMPARE(controller.position(), double(winner));
        QCOMPARE(reveal.size(), 0);
        controller.setAccountKey("bob");
        controller.setProperty("reducedMotion", true);
        controller.open();
        QCOMPARE(controller.state(), DailyCaseController::Opened);
        QCOMPARE(reveal.size(), 1);
        controller.open();
        QCOMPARE(reveal.size(), 1);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    QCoreApplication::setOrganizationName("OpenChatTests");
    QCoreApplication::setApplicationName("daily-case");
    DailyCaseTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "tst_dailycase.moc"
