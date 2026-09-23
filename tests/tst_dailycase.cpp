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
        QCOMPARE(claim.result->nextAvailableAt.time(), QTime(0, 0));
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
            {"next", QDateTime::currentDateTimeUtc().addDays(1).toString(Qt::ISODate)}}).toJson());
        file.close();
        QCOMPARE(LocalDailyCaseService(directory.path()).status("alice").result->rewardId,
                 QStringLiteral("placeholder"));
        DailyCaseController controller(std::make_unique<LocalDailyCaseService>(directory.path()));
        controller.setAccountKey("alice");
        QCOMPARE(controller.state(), DailyCaseController::OpenedToday);
        QVERIFY(controller.reward().isEmpty());
    }
    void theBeltIsTheDaysAndStaysPutWhenTheClaimLands()
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
        QCOMPARE(first.state(), DailyCaseController::OpenedToday);
        QCOMPARE(first.fillers(), belt);
        QCOMPARE(rearranged.size(), 0);
        QCOMPARE(first.reward().value("id").toString(),
                 LocalDailyCaseService(directory.path()).status("alice").result->rewardId);
        QVERIFY(first.reward().value("rarityColor").value<QColor>().isValid());
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
        QCOMPARE(controller.state(), DailyCaseController::OpenedToday);
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
        QCOMPARE(controller.state(), DailyCaseController::OpenedToday);
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
