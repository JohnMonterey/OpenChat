#include "PageSyncTestSupport.h"

#include "domain/ProfileUpdate.h"
#include "storage/RepositorySql.h"

#include <QCborMap>
#include <QCborValue>
#include <QtTest/QtTest>

#include <memory>

using namespace OpenChat;
using namespace PageSyncTest;

// Profile page sync between real peers: every page message is encrypted by
// one peer's engine under a real MLS group and decrypted by the other's, so
// what a test counts is what really crossed the wire.

namespace {

using Kind = ProfilePayloadKind;
constexpr auto Background = Profile::MediaKind::BackgroundImageMedia;
constexpr auto Song = Profile::MediaKind::SongMedia;
constexpr qint64 minute = 60'000;
constexpr qint64 hour = 60 * minute;

int countFrom(const Peer &receiver, const Peer &sender, Kind kind)
{
    return int(TwoPeerFixture::payloadsFrom(receiver, sender, kind).size());
}

QVector<qint64> coreRevisionsFrom(const Peer &receiver, const Peer &sender)
{
    QVector<qint64> revisions;
    for (const QByteArray &payload : TwoPeerFixture::payloadsFrom(receiver, sender, Kind::PageCore)) {
        const auto page = decodePageCore(payload);
        revisions.push_back(page ? page->revision : -1);
    }
    return revisions;
}

QVector<QByteArray> mediaHashesFrom(const Peer &receiver, const Peer &sender)
{
    QVector<QByteArray> hashes;
    for (const QByteArray &payload : TwoPeerFixture::payloadsFrom(receiver, sender, Kind::PageMedia)) {
        const auto media = decodePageMedia(payload);
        hashes.push_back(media ? media->sha256 : QByteArray());
    }
    return hashes;
}

QVector<PageRequestMessage> requestsFrom(const Peer &receiver, const Peer &sender)
{
    QVector<PageRequestMessage> requests;
    for (const QByteArray &payload : TwoPeerFixture::payloadsFrom(receiver, sender, Kind::PageRequest)) {
        if (const auto request = decodePageRequest(payload))
            requests.push_back(*request);
    }
    return requests;
}

// The viewer's client shows it understands pages: a page of its own reaches
// everyone it knows, who from then on push media to it.
bool makeCapable(TwoPeerFixture &net, Peer &viewer)
{
    if (!viewer.sync || publishPage(*viewer.sync, viewer.name + QStringLiteral("'s page")) <= 0)
        return false;
    net.settle();
    return true;
}

PageDelivery deliveryOf(const Peer &owner, const Peer &contact)
{
    const auto delivery = owner.pages().delivery(contact.account);
    return delivery.hasValue() ? delivery.value() : PageDelivery{contact.account};
}

PageRequestState requestStateOf(const Peer &viewer, const Peer &contact)
{
    const auto state = viewer.pages().requestState(contact.account);
    return state.hasValue() ? state.value() : PageRequestState{contact.account};
}

bool wasSent(const Peer &owner, const Peer &contact, const QByteArray &blob)
{
    const auto sentAt = owner.pages().mediaSentAt(contact.account, pageMediaHash(blob));
    return sentAt.hasValue() && sentAt.value().has_value();
}

bool holds(const Peer &viewer, const Peer &owner, const QByteArray &blob, Profile::MediaKind kind)
{
    const auto has = viewer.pages().hasContactMedia(owner.account, pageMediaHash(blob), int(kind));
    return has.hasValue() && has.value();
}

int pendingMedia(const Peer &viewer, const Peer &owner)
{
    const auto count = viewer.pages().pendingContactMediaCount(owner.account);
    return count.hasValue() ? count.value() : -1;
}

// Counts emissions of `signal` that name `account`.
template<typename Signal>
std::shared_ptr<int> countSignals(ProfilePageSync &sync, Signal signal, const AccountId &account)
{
    auto count = std::make_shared<int>(0);
    QObject::connect(&sync, signal, &sync, [count, account](const AccountId &which) {
        if (which == account)
            ++*count;
    });
    return count;
}

// Reads a peer's outbox table through a connection of its own.
class OutboxInspector final
{
public:
    OutboxInspector(const QString &path, const QByteArray &key)
    {
        sqlite3 *handle = nullptr;
        const int opened = sqlite3_open_v2(path.toUtf8().constData(), &handle, SQLITE_OPEN_READONLY, nullptr);
        m_connection.reset(handle);
        m_keyed = opened == SQLITE_OK && sqlite3_key(handle, key.constData(), int(key.size())) == SQLITE_OK;
    }

    [[nodiscard]] bool isReadable() const { return m_keyed && rows() >= 0; }
    [[nodiscard]] int rows() const { return count("SELECT count(*) FROM outbox"); }
    // Envelopes with no visible message behind them: receipts, profile and
    // page updates, call signals.
    [[nodiscard]] int controlRows() const
    {
        return count("SELECT count(*) FROM outbox WHERE NOT EXISTS "
                     "(SELECT 1 FROM messages WHERE messages.id = outbox.message_id)");
    }

private:
    [[nodiscard]] int count(const char *sql) const
    {
        RepositorySql::Statement statement(m_connection.get(), sql);
        if (!statement.isValid() || sqlite3_step(statement.get()) != SQLITE_ROW)
            return -1;
        return sqlite3_column_int(statement.get(), 0);
    }

    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> m_connection{nullptr, &sqlite3_close};
    bool m_keyed = false;
};

} // namespace

class ProfilePageSyncTest final : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        // Every warning the sync logs is a failure unless a test expects it.
        QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    }

    // --- Owner side: publishing and the pump -------------------------------

    void publishDeliversTheCoreToEveryAcceptedContact()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol"));
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        net.startSync(b);
        net.startSync(*c);
        net.settle();

        const qint64 revision = publishPage(*a.sync, QStringLiteral("Hello from Alice"));
        QCOMPARE(revision, net.clock.nowMs); // the first revision is the publish time
        // Publishing only stores the page; the pump sends from the event loop.
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QCOMPARE(TwoPeerFixture::envelopesTo(a, c->device), 0);

        net.settle();
        for (Peer *viewer : {&b, c}) {
            QCOMPARE(coreRevisionsFrom(*viewer, a), QVector<qint64>{revision});
            const auto received = viewer->sync->contactPage(a.account);
            QVERIFY(received);
            QVERIFY(received->page == a.sync->publishedPage());
            QCOMPARE(received->page.content.headline, QStringLiteral("Hello from Alice"));
            const PageDelivery delivery = deliveryOf(a, *viewer);
            QCOMPARE(delivery.sentRevision, revision);
            QVERIFY(delivery.device == std::optional<DeviceId>(viewer->device));
        }
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void mediaIsPushedOnlyToPageCapableContacts()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol")); // runs an older client: no sync
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        QVERIFY(deliveryOf(a, b).pageCapable);

        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        QVERIFY(publishPage(*a.sync, QStringLiteral("With media"), picture, song) > 0);
        net.settle();

        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);
        QCOMPARE(mediaHashesFrom(b, a), (QVector<QByteArray>{pageMediaHash(picture), pageMediaHash(song)}));
        const auto bobsView = b.sync->contactPage(a.account);
        QVERIFY(bobsView && bobsView->backgroundPresent && bobsView->songPresent);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(picture), Background), picture);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(song), Song), song);

        // Carol never sent a page message: she gets the small core, nothing large.
        QCOMPARE(TwoPeerFixture::envelopesTo(a, c->device), 1);
        QCOMPARE(countFrom(*c, a, Kind::PageCore), 1);
        QCOMPARE(countFrom(*c, a, Kind::PageMedia), 0);
        QVERIFY(!deliveryOf(a, *c).pageCapable);
        QVERIFY(!wasSent(a, *c, picture));
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void republishWithUnchangedMediaSendsOnlyTheCore()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        const qint64 first = publishPage(*a.sync, QStringLiteral("First"), picture, song);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);

        net.clock.advance(1'000);
        const qint64 second = a.sync->publish(pageWith(QStringLiteral("Second"), picture, song));
        QVERIFY(second > first);
        net.settle();

        QCOMPARE(coreRevisionsFrom(b, a), (QVector<qint64>{first, second}));
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);
        const auto received = b.sync->contactPage(a.account);
        QVERIFY(received && received->backgroundPresent && received->songPresent);
        QCOMPARE(received->page.content.headline, QStringLiteral("Second"));
    }

    void changedBackgroundSendsOnlyTheNewBlob()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        const QByteArray oldPicture = testJpeg('\x61');
        const QByteArray newPicture = testJpeg('\x71');
        const QByteArray song = testSong('\x62');
        QVERIFY(publishPage(*a.sync, QStringLiteral("Old picture"), oldPicture, song) > 0);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);

        net.clock.advance(1'000);
        QVERIFY(publishPage(*a.sync, QStringLiteral("New picture"), newPicture, song) > 0);
        net.settle();

        QCOMPARE(countFrom(b, a, Kind::PageCore), 2);
        const QVector<QByteArray> hashes = mediaHashesFrom(b, a);
        QCOMPARE(hashes.size(), 3);
        QCOMPARE(hashes.last(), pageMediaHash(newPicture));
        const auto received = b.sync->contactPage(a.account);
        QVERIFY(received && received->backgroundPresent && received->songPresent);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(newPicture), Background), newPicture);
        QVERIFY(b.sync->contactMedia(a.account, pageMediaHash(oldPicture), Background).isEmpty());
        // The replaced picture's record is forgotten, so a page naming it
        // again would send it again; the song's stays.
        QVERIFY(!wasSent(a, b, oldPicture));
        QVERIFY(wasSent(a, b, song));
    }

    void publishesWhileOfflineCoalesceIntoTheLatestRevision()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        net.settle();

        a.transport.connected = false;
        const qint64 first = publishPage(*a.sync, QStringLiteral("One"));
        net.clock.advance(1'000);
        const qint64 second = publishPage(*a.sync, QStringLiteral("Two"));
        net.clock.advance(1'000);
        const qint64 third = publishPage(*a.sync, QStringLiteral("Three"));
        QVERIFY(first > 0 && first < second && second < third);
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QVERIFY(a.sync->hasOwedDeliveries());

        a.transport.connectLink();
        net.settle();
        QCOMPARE(coreRevisionsFrom(b, a), QVector<qint64>{third});
        QCOMPARE(b.sync->contactPage(a.account)->page.content.headline, QStringLiteral("Three"));
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void pumpWaitsWhileTheLinkIsBacklogged()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));

        // Above the pump's 64 KiB, below the engine's own drain gate: whatever
        // the pump handed over would leave at once, so nothing may be handed.
        a.transport.backlog = 100 * 1024;
        const QByteArray picture = testJpeg('\x61', 100 * 1024);
        const QByteArray song = testSong('\x62', 60, 1'000);
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Busy link"), picture, song);
        QVERIFY(revision > 0);
        net.settle(100);
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QCOMPARE(deliveryOf(a, b).sentRevision, qint64(-1));
        QVERIFY(a.sync->hasOwedDeliveries());

        // The backlog is looked at before every payload. Counting each one as
        // unsent until the socket drains: the core and the 100 KB picture go,
        // then the link is busy and the song waits.
        a.transport.backlog = 0;
        a.transport.backlogGrowsWithSends = true;
        net.settle(100);
        QCOMPARE(coreRevisionsFrom(b, a), QVector<qint64>{revision});
        QCOMPARE(mediaHashesFrom(b, a), QVector<QByteArray>{pageMediaHash(picture)});
        QVERIFY(!wasSent(a, b, song));

        a.transport.flush();
        net.settle(100);
        QCOMPARE(mediaHashesFrom(b, a), (QVector<QByteArray>{pageMediaHash(picture), pageMediaHash(song)}));

        // A link that cannot tell its backlog never holds the pump back.
        a.transport.backlogGrowsWithSends = false;
        a.transport.flush();
        a.transport.backlog = -1;
        net.clock.advance(1'000);
        QVERIFY(publishPage(*a.sync, QStringLiteral("Unknown backlog")) > 0);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 2);
    }

    void mediaAndAnswersWaitForTheCallToEnd()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));

        a.sync->setCallActive(true);
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        QVERIFY(publishPage(*a.sync, QStringLiteral("In a call"), picture, song) > 0);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1); // cores still go out
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 0);

        // A request during the call is admitted (it counts), then waits.
        QVERIFY(net.sendRaw(b, a, rawRequest()));
        net.settle();
        QCOMPARE(deliveryOf(a, b).answersInWindow, 1);
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 0);
        QVERIFY(a.sync->hasOwedDeliveries());

        a.sync->setCallActive(false);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 2); // the answer's core
        // Each blob once, although both the answer and the push wanted it.
        QCOMPARE(mediaHashesFrom(b, a), (QVector<QByteArray>{pageMediaHash(picture), pageMediaHash(song)}));
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void deliveryIsReconciledAfterRestartAndReconnect()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        net.settle();

        // A crash right after the page was stored, before the pump ever ran.
        const qint64 first = publishPage(*a.sync, QStringLiteral("Stored, then crashed"));
        QVERIFY(first > 0);
        net.stopSync(a);
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QVERIFY(net.reopen(a));
        net.startSync(a);
        net.settle();
        QCOMPARE(coreRevisionsFrom(b, a), QVector<qint64>{first});

        // Published offline, then restarted still offline: owed, not sent…
        a.transport.connected = false;
        net.clock.advance(1'000);
        const qint64 second = publishPage(*a.sync, QStringLiteral("Published offline"));
        QVERIFY(second > first);
        net.stopSync(a);
        QVERIFY(net.reopen(a));
        net.startSync(a);
        net.settle();
        QCOMPARE(coreRevisionsFrom(b, a), QVector<qint64>{first});
        QVERIFY(a.sync->hasOwedDeliveries());

        // …until the link comes back.
        a.transport.connectLink();
        net.settle();
        QCOMPARE(coreRevisionsFrom(b, a), (QVector<qint64>{first, second}));
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void failedClosedEngineRecordsNothing()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.settle();

        // An encrypt into a group this device never joined stops the engine
        // for the rest of the session.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("MLS operation failed")));
        a.engine().sendProfileUpdate(ConversationId::generate(), b.device, QByteArrayLiteral("x"));
        QVERIFY(a.engine().isFailedClosed());

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("sync engine has stopped")));
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Never leaves"));
        QVERIFY(revision > 0); // stored all the same
        net.settle();
        // The engine reports the link even while stopped; still nothing goes.
        a.transport.connectLink();
        net.settle();

        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QCOMPARE(countFrom(b, a, Kind::PageCore), 0);
        QCOMPARE(deliveryOf(a, b).sentRevision, qint64(-1));
        QVERIFY(a.sync->hasOwedDeliveries());
    }

    void failedClosedEngineRecordsNothingForTheSendThatStoppedIt()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer *c = net.addPeer(QStringLiteral("carol"));
        QVERIFY(c);
        // Carol's contact row names a conversation Alice's MLS never joined
        // (a damaged store), so encrypting to her stops the engine mid-pump.
        const ConversationId broken = ConversationId::generate();
        ContactRecord record{c->account, c->name, QString(), ContactState::PendingOutgoing,
                             broken,     net.clock.nowMs, net.clock.nowMs};
        record.peerDeviceId = c->device;
        QVERIFY(a.contacts().recordOutgoingRequest(record).hasValue());
        QVERIFY(a.contacts().markAccepted(c->account, broken, net.clock.nowMs).hasValue());
        net.startSync(a);
        net.settle();

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("MLS operation failed")));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("sync engine has stopped")));
        QVERIFY(publishPage(*a.sync, QStringLiteral("Stops the engine")) > 0);
        net.settle();
        QVERIFY(a.engine().isFailedClosed());
        // "Sent" means the engine took it: the send that stopped it does not count.
        QCOMPARE(deliveryOf(a, *c).sentRevision, qint64(-1));
        QVERIFY(a.sync->hasOwedDeliveries());
    }

    void peerDeviceChangeRestartsDelivery()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        const QByteArray picture = testJpeg('\x61');
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Before the new phone"), picture);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 1);
        const int toOldDevice = TwoPeerFixture::envelopesTo(a, b.device);

        // Bob is back on a new device: a fresh handshake rebinds and accepts.
        const DeviceId newDevice = DeviceId::generate();
        QVERIFY(a.contacts().setPeerDeviceId(b.account, newDevice).hasValue());
        a.sync->onContactAccepted(b.account);
        net.settle();

        // The page goes to the new device, core only until it shows it
        // understands pages; nothing sent to the old device counts any more.
        QCOMPARE(TwoPeerFixture::envelopesTo(a, newDevice), 1);
        const PageDelivery delivery = deliveryOf(a, b);
        QVERIFY(delivery.device == std::optional<DeviceId>(newDevice));
        QCOMPARE(delivery.sentRevision, revision);
        QVERIFY(!delivery.pageCapable);
        QVERIFY(!wasSent(a, b, picture));
        QVERIFY(!a.sync->hasOwedDeliveries());

        // And the old device's messages are no longer Bob's.
        QVERIFY(net.sendRaw(b, a, rawRequest()));
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), toOldDevice);
        QCOMPARE(TwoPeerFixture::envelopesTo(a, newDevice), 1);
    }

    void newlyAcceptedContactGetsThePublishedCore()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer *c = net.addPeer(QStringLiteral("carol"));
        // Alice asked Carol; Carol accepted, but Alice has not heard yet.
        QVERIFY(c && net.link(a, *c, false, true));
        net.startSync(a);
        net.startSync(*c);
        net.settle();

        const qint64 revision = publishPage(*a.sync, QStringLiteral("Hello"));
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, c->device), 0);
        QCOMPARE(coreRevisionsFrom(net.b(), a), QVector<qint64>{revision});

        QVERIFY(net.acceptContact(a, *c));
        a.sync->onContactAccepted(c->account);
        net.settle();
        QCOMPARE(coreRevisionsFrom(*c, a), QVector<qint64>{revision});
        QVERIFY(c->sync->contactPage(a.account));
    }

    void nothingIsSentBeforeTheFirstPublish()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        a.sync->onContactAccepted(b.account);
        b.sync->onContactAccepted(a.account);
        a.transport.connectLink();
        b.transport.connectLink();
        net.settle();

        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QCOMPARE(TwoPeerFixture::envelopesTo(b, a.device), 0);
        QVERIFY(!a.sync->hasOwedDeliveries());
        QCOMPARE(a.sync->publishedRevision(), qint64(0));
        QVERIFY(a.sync->publishedPage() == Profile::defaultPage());

        QVERIFY(publishPage(*a.sync, QStringLiteral("First")) > 0);
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 1);
    }

    // --- Viewer side: background requests -----------------------------------

    void acceptanceSchedulesABackgroundRequestWhenNoPageArrives()
    {
        TwoPeerFixture net;
        net.limits.acceptRequestDelayMs = 30;
        net.limits.acceptRequestJitterMs = 20;
        net.limits.pageLoadingWindowMs = 50;
        // Due 40 ms after acceptance; startup requests stay an hour away.
        net.random = [](qint64 bound) { return bound == 20 ? qint64(10) : bound - 1; };
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol")); // older client: never answers
        Peer *d = net.addPeer(QStringLiteral("dave"));
        QVERIFY(c && d && net.link(a, *c) && net.link(a, *d));
        net.startSync(a);
        net.startSync(b);
        net.startSync(*d);
        net.settle();
        const auto awaitingBob = countSignals(*a.sync, &ProfilePageSync::awaitingChanged, b.account);

        a.sync->onContactAccepted(b.account);
        QVERIFY(a.sync->isAwaitingPage(b.account));
        QCOMPARE(*awaitingBob, 1);
        // Not before its time…
        net.clock.advance(39);
        net.settle(80);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0);
        QVERIFY(a.sync->isAwaitingPage(b.account));
        // …then "I have no page of yours".
        net.clock.advance(1);
        QVERIFY(TwoPeerFixture::waitFor([&] { return !net.pendingTo(a, b).isEmpty(); }));
        QVERIFY(a.sync->isAwaitingPage(b.account)); // the request just went out
        net.settle();
        const auto requests = requestsFrom(b, a);
        QCOMPARE(requests.size(), 1);
        QVERIFY(!requests.first().haveRevision);
        QVERIFY(requests.first().wantMedia.isEmpty());
        // Bob never published: his answer is the default page, and the wait is over.
        const auto bobsPage = a.sync->contactPage(b.account);
        QVERIFY(bobsPage);
        QCOMPARE(bobsPage->page.revision, qint64(0));
        QVERIFY(!a.sync->isAwaitingPage(b.account));

        // Carol never answers: "Getting…" lasts for the loading window only.
        a.sync->onContactAccepted(c->account);
        net.clock.advance(40);
        QVERIFY(TwoPeerFixture::waitFor([&] { return !net.pendingTo(a, *c).isEmpty(); }));
        net.settle();
        QCOMPARE(countFrom(*c, a, Kind::PageRequest), 1);
        QVERIFY(a.sync->isAwaitingPage(c->account));
        const auto awaitingCarol = countSignals(*a.sync, &ProfilePageSync::awaitingChanged, c->account);
        net.clock.advance(50);
        QVERIFY(!a.sync->isAwaitingPage(c->account));
        QTRY_COMPARE(*awaitingCarol, 1); // the window's end is announced

        // Dave's page comes first: nothing is asked.
        a.sync->onContactAccepted(d->account);
        QVERIFY(a.sync->isAwaitingPage(d->account));
        QVERIFY(publishPage(*d->sync, QStringLiteral("Dave's page")) > 0);
        net.settle();
        QVERIFY(!a.sync->isAwaitingPage(d->account));
        net.clock.advance(100);
        net.settle(100);
        QCOMPARE(countFrom(*d, a, Kind::PageRequest), 0);
    }

    void startupRequestsPagelessContacts()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol")); // older client: never answers
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        net.startSync(b);
        QVERIFY(publishPage(*b.sync, QStringLiteral("Bob's page")) > 0);
        net.settle();
        QVERIFY(a.sync->contactPage(b.account));

        // A restart: the new sync spreads its requests over the jitter.
        net.stopSync(a);
        net.limits.startupRequestJitterMs = 50;
        net.random = [](qint64) { return qint64(20); };
        net.startSync(a);
        net.settle(60);
        QCOMPARE(countFrom(*c, a, Kind::PageRequest), 0);
        net.clock.advance(20);
        net.settle(80);
        const auto requests = requestsFrom(*c, a);
        QCOMPARE(requests.size(), 1);
        QVERIFY(!requests.first().haveRevision);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0); // Bob's page is here

        // Every reconnect asks again, once the back-off allows.
        net.clock.advance(hour);
        a.transport.connectLink();
        net.clock.advance(20);
        net.settle(80);
        QCOMPARE(countFrom(*c, a, Kind::PageRequest), 2);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0);
    }

    void openingAPageSendsNothing()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol")); // sends by hand
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, a));
        // Bob's page, complete.
        QVERIFY(publishPage(*b.sync, QStringLiteral("Bob"), testJpeg('\x0B')) > 0);
        // Carol's page, whose picture is still on its way.
        QVERIFY(net.sendRaw(*c, a, rawCore(5'000, QStringLiteral("Carol"), backgroundRef(testJpeg('\x0C')))));
        net.settle();
        QVERIFY(a.sync->contactPage(b.account)->backgroundPresent);
        QVERIFY(!a.sync->contactPage(c->account)->backgroundPresent);
        const qsizetype sentBefore = a.transport.sent.size();

        a.sync->markViewed(b.account);
        a.sync->markViewed(c->account);
        net.settle();
        QCOMPARE(a.transport.sent.size(), sentBefore);
        // The look was recorded: it orders eviction.
        QCOMPARE(a.pages().contactPage(b.account).value()->viewedAtMs, net.clock.nowMs);
    }

    void evictedMediaIsRequestedWhenViewed()
    {
        TwoPeerFixture net;
        // Any received blob is over the cap.
        net.limits.receivedMediaSoftCapBytes = 1;
        net.limits.receivedMediaTargetBytes = 0;
        net.limits.missingMediaGraceMs = 30;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, a));
        const auto changes = countSignals(*a.sync, &ProfilePageSync::contactPageChanged, b.account);

        const QByteArray picture = testJpeg('\x0B');
        const qint64 revision = publishPage(*b.sync, QStringLiteral("Bob"), picture);
        net.settle();
        // The core, the picture, and its eviction at once; the core stays.
        QCOMPARE(countFrom(a, b, Kind::PageMedia), 1);
        QCOMPARE(*changes, 3);
        const auto page = a.sync->contactPage(b.account);
        QVERIFY(page && !page->backgroundPresent);
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(0));
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0); // nothing asked in the background

        // Opening the page asks for it again: the documented exception.
        a.sync->markViewed(b.account);
        net.settle();
        auto requests = requestsFrom(b, a);
        QCOMPARE(requests.size(), 1);
        QVERIFY(requests.first().haveRevision == std::optional<qint64>(revision));
        QCOMPARE(requests.first().wantMedia, QVector<QByteArray>{pageMediaHash(picture)});
        // Bob sent it minutes ago, so his answer holds it back (one hour), and
        // another look asks nothing while the back-off runs.
        QCOMPARE(countFrom(a, b, Kind::PageMedia), 1);
        a.sync->markViewed(b.account);
        net.settle();
        QCOMPARE(requestsFrom(b, a).size(), 1);

        // An hour on, a look asks again and Bob sends it.
        net.clock.advance(hour);
        a.sync->markViewed(b.account);
        net.settle();
        QCOMPARE(requestsFrom(b, a).size(), 2);
        QCOMPARE(countFrom(a, b, Kind::PageMedia), 2);

        // Media asked for once already (by the grace request), then evicted,
        // is asked for again on a look as well.
        Peer *c = net.addPeer(QStringLiteral("carol")); // sends by hand
        QVERIFY(c && net.link(a, *c));
        const QByteArray carolsPicture = testJpeg('\x0C');
        QVERIFY(net.sendRaw(*c, a, rawCore(7'000, QStringLiteral("Carol"), backgroundRef(carolsPicture))));
        net.settle();
        net.clock.advance(30);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(*c, a, Kind::PageRequest) == 1;
        }));
        QCOMPARE(requestStateOf(a, *c).mediaRequestedRevision, qint64(7'000));
        QVERIFY(net.sendRaw(*c, a, rawMedia(Background, carolsPicture))); // her answer
        net.settle();
        QVERIFY(!a.sync->contactPage(c->account)->backgroundPresent); // evicted at once
        net.clock.advance(30 * minute); // she answered: the base interval applies
        a.sync->markViewed(c->account);
        net.settle();
        const auto carolsRequests = requestsFrom(*c, a);
        QCOMPARE(carolsRequests.size(), 2);
        QCOMPARE(carolsRequests.last().wantMedia, QVector<QByteArray>{pageMediaHash(carolsPicture)});

        // A newer revision clears the mark: its media is merely on its way,
        // which is the grace request's to fetch, in the background.
        QVERIFY(net.sendRaw(*c, a, rawCore(8'000, QStringLiteral("Carol"), backgroundRef(carolsPicture))));
        net.settle();
        QCOMPARE(requestStateOf(a, *c).mediaRequestedRevision, PageRequestState::neverAsked);
        net.clock.advance(30);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return requestsFrom(*c, a).size() == 3;
        }));
        QVERIFY(requestsFrom(*c, a).last().haveRevision == std::optional<qint64>(8'000));
        QCOMPARE(requestStateOf(a, *c).mediaRequestedRevision, qint64(8'000));
    }

    // Eviction marks a page for a look only when it took media the page
    // names: a pending blob going does not make still-missing media evicted.
    void evictingPendingMediaMarksNothing()
    {
        TwoPeerFixture net;
        net.limits.receivedMediaSoftCapBytes = 1;
        net.limits.receivedMediaTargetBytes = 0;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        QVERIFY(net.sendRaw(b, a, rawCore(5'000, QStringLiteral("Bob"), backgroundRef(testJpeg('\x0B')))));
        QVERIFY(net.sendRaw(b, a, rawMedia(Background, testJpeg('\x0D')))); // named by no core of his
        net.settle();
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(0)); // evicted at once
        QCOMPARE(requestStateOf(a, b).mediaRequestedRevision, PageRequestState::neverAsked);

        a.sync->markViewed(b.account);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0);
    }

    // --- Viewer side: receiving ---------------------------------------------

    void olderAndEqualRevisionsAreIgnored()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        const auto changes = countSignals(*a.sync, &ProfilePageSync::contactPageChanged, b.account);

        QVERIFY(net.sendRaw(b, a, rawCore(5'000, QStringLiteral("Newest"))));
        net.settle();
        QCOMPARE(*changes, 1);
        QVERIFY(net.sendRaw(b, a, rawCore(5'000, QStringLiteral("Same revision, other words"))));
        QVERIFY(net.sendRaw(b, a, rawCore(4'999, QStringLiteral("Older"))));
        net.settle();
        QCOMPARE(countFrom(a, b, Kind::PageCore), 3);
        QCOMPARE(*changes, 1);
        auto page = a.sync->contactPage(b.account);
        QVERIFY(page);
        QCOMPARE(page->page.revision, qint64(5'000));
        QCOMPARE(page->page.content.headline, QStringLiteral("Newest"));

        QVERIFY(net.sendRaw(b, a, rawCore(5'001, QStringLiteral("Newer"))));
        net.settle();
        QCOMPARE(*changes, 2);
        QCOMPARE(a.sync->contactPage(b.account)->page.content.headline, QStringLiteral("Newer"));
    }

    void mediaBeforeCoreIsAdopted()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        const QByteArray picture = testJpeg('\x61');
        QVERIFY(publishPage(*a.sync, QStringLiteral("Picture"), picture) > 0);
        // The pump hands over the core, then the picture; nothing is routed yet.
        QVERIFY(TwoPeerFixture::waitFor([&] { return net.pendingTo(a, b).size() == 2; }));
        const QVector<qsizetype> pending = net.pendingTo(a, b);

        net.deliver(a, b, pending.at(1)); // the picture overtakes its core
        QCOMPARE(classifyProfilePayload(b.arrivals.last().payload), Kind::PageMedia);
        QVERIFY(!b.sync->contactPage(a.account));
        QCOMPARE(pendingMedia(b, a), 1);

        net.deliver(a, b, pending.at(0));
        const auto page = b.sync->contactPage(a.account);
        QVERIFY(page && page->backgroundPresent);
        QCOMPARE(pendingMedia(b, a), 0);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(picture), Background), picture);
    }

    void mediaInTheWrongSlotIsNotAdopted()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        const QByteArray song = testSong('\x33');
        const QByteArray hash = pageMediaHash(song);

        // Bob's core names his song's hash as his background…
        QVERIFY(net.sendRaw(b, a, rawCore(7'000, QStringLiteral("Mislabelled"), backgroundRef(song))));
        // …and he sends that blob, as the song it is.
        QVERIFY(net.sendRaw(b, a, rawMedia(Song, song)));
        net.settle();

        const auto page = a.sync->contactPage(b.account);
        QVERIFY(page);
        QVERIFY(page->page.background.sha256 == hash);
        QVERIFY(!page->backgroundPresent);
        QVERIFY(!page->songPresent);
        QVERIFY(a.sync->contactMedia(b.account, hash, Background).isEmpty());
        QVERIFY(a.sync->contactMedia(b.account, hash, Song).isEmpty());
        // Kept only as the song he declared, never readable as his picture.
        QVERIFY(holds(a, b, song, Song));
        QVERIFY(!holds(a, b, song, Background));
    }

    void contactMediaIsNotSharedAcrossOwners()
    {
        TwoPeerFixture net;
        net.limits.missingMediaGraceMs = 30;
        QVERIFY(net.setUp());
        Peer &a = net.a(); // the viewer
        Peer &b = net.b(); // sends by hand
        Peer *c = net.addPeer(QStringLiteral("carol")); // sends by hand
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        const QByteArray picture = testJpeg('\x44');

        QVERIFY(net.sendRaw(*c, a, rawCore(8'000, QStringLiteral("Carol"), backgroundRef(picture))));
        QVERIFY(net.sendRaw(*c, a, rawMedia(Background, picture)));
        net.settle();
        QVERIFY(a.sync->contactPage(c->account)->backgroundPresent);

        // Bob's core names Carol's picture, which Bob never sent.
        QVERIFY(net.sendRaw(b, a, rawCore(9'000, QStringLiteral("Bob"), backgroundRef(picture))));
        net.settle();
        const auto bobsPage = a.sync->contactPage(b.account);
        QVERIFY(bobsPage && !bobsPage->backgroundPresent);
        QVERIFY(a.sync->contactMedia(b.account, pageMediaHash(picture), Background).isEmpty());
        QCOMPARE(a.sync->contactMedia(c->account, pageMediaHash(picture), Background), picture);

        // So Alice asks Bob for it as if she had never seen it: what she holds
        // from others is nobody else's business.
        net.clock.advance(30);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(b, a, Kind::PageRequest) == 1;
        }));
        const auto request = requestsFrom(b, a).first();
        QVERIFY(request.haveRevision == std::optional<qint64>(9'000));
        QCOMPARE(request.wantMedia, QVector<QByteArray>{pageMediaHash(picture)});
        net.settle();
        QCOMPARE(countFrom(*c, a, Kind::PageRequest), 0); // Carol's page is complete
    }

    void unreferencedMediaIsBoundedAndExpires()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        const QByteArray first = testJpeg('\x01');
        const QByteArray second = testSong('\x02');
        const QByteArray third = testJpeg('\x03');

        QVERIFY(net.sendRaw(b, a, rawMedia(Background, first)));
        QVERIFY(net.sendRaw(b, a, rawMedia(Song, second)));
        QVERIFY(net.sendRaw(b, a, rawMedia(Background, third)));
        net.settle();
        // Media may overtake its core, but only two blobs per contact wait.
        QCOMPARE(pendingMedia(a, b), 2);
        QVERIFY(holds(a, b, first, Background));
        QVERIFY(holds(a, b, second, Song));
        QVERIFY(!holds(a, b, third, Background));

        // Sent again, one only has its time-to-live renewed.
        net.clock.advance(12 * hour);
        QVERIFY(net.sendRaw(b, a, rawMedia(Background, first)));
        net.settle();
        QCOMPARE(pendingMedia(a, b), 2);

        // A day after arriving, what no core adopted is collected.
        net.clock.advance(12 * hour + 1);
        a.sync->collectGarbage();
        QCOMPARE(pendingMedia(a, b), 1);
        QVERIFY(holds(a, b, first, Background));
        QVERIFY(!holds(a, b, second, Song));
        net.clock.advance(12 * hour);
        a.sync->collectGarbage();
        QCOMPARE(pendingMedia(a, b), 0);
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(0));
    }

    void mediaWithWrongHashIsRejected()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        const QByteArray picture = testJpeg('\x21');
        const QByteArray otherHash = pageMediaHash(testJpeg('\x22'));

        // Built by hand: encodePageMedia insists on the true hash.
        QCborMap forged;
        forged.insert(qint64(0), 1);
        forged.insert(qint64(1), 2);
        forged.insert(qint64(2), 1);
        forged.insert(qint64(3), otherHash);
        forged.insert(qint64(4), picture);
        const QByteArray payload = QByteArray(1, char(0xFF)) + forged.toCborValue().toCbor();
        QCOMPARE(classifyProfilePayload(payload), Kind::PageMedia);
        QVERIFY(net.sendRaw(b, a, rawCore(3'000, QStringLiteral("Bob"), backgroundRef(picture))));
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("failed its checks")));
        QVERIFY(net.sendRaw(b, a, payload));
        net.settle();

        QCOMPARE(countFrom(a, b, Kind::PageMedia), 1);
        QVERIFY(!a.sync->contactPage(b.account)->backgroundPresent);
        QVERIFY(!holds(a, b, picture, Background));
        const auto underOtherHash = a.pages().hasContactMedia(b.account, otherHash, 1);
        QVERIFY(underOtherHash.hasValue() && !underOtherHash.value());
        QCOMPARE(pendingMedia(a, b), 0);
    }

    // --- Owner side: answering requests -------------------------------------

    void requestWithoutRevisionIsAnsweredWithTheWholePage()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // asks by hand
        net.startSync(a);
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Whole page"), picture, song);
        net.settle();
        // So far only the core: Bob has not shown he understands pages.
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 0);

        QVERIFY(net.sendRaw(b, a, rawRequest()));
        net.settle();
        QCOMPARE(coreRevisionsFrom(b, a), (QVector<qint64>{revision, revision}));
        QCOMPARE(mediaHashesFrom(b, a), (QVector<QByteArray>{pageMediaHash(picture), pageMediaHash(song)}));
        QVERIFY(deliveryOf(a, b).pageCapable);
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    void upgradedPeerGetsTheWholePageInOneExchange()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // on 0.2.8 at first: no sync
        net.startSync(a);
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Before the upgrade"), picture, song);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 0); // nothing large to an older client

        // Bob upgrades; his new client holds no page of Alice's (0.2.8 could
        // not read the core it got) and asks at start.
        net.limits.startupRequestJitterMs = 10;
        net.random = [](qint64) { return qint64(0); };
        net.startSync(b);
        const qsizetype arrivalsBefore = b.arrivals.size();
        net.settle();

        const auto requests = requestsFrom(a, b);
        QCOMPARE(requests.size(), 1);
        QVERIFY(!requests.first().haveRevision);
        QCOMPARE(b.arrivals.size() - arrivalsBefore, qsizetype(3)); // one exchange: core + both blobs
        const auto page = b.sync->contactPage(a.account);
        QVERIFY(page && page->backgroundPresent && page->songPresent);
        QCOMPARE(page->page.revision, revision);
    }

    void requestIsAnsweredWithOnlyPublishedMedia()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // asks by hand
        net.startSync(a);
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        const qint64 revision = publishPage(*a.sync, QStringLiteral("Published"), picture, song);
        net.settle();
        QVERIFY(net.sendRaw(b, a, rawRequest()));
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);

        // Later Alice imports a new picture into her draft, not yet published.
        net.clock.advance(hour + minute);
        const QByteArray draftPicture = testJpeg('\x77');
        const auto draftHash = a.sync->addLocalMedia(Background, draftPicture);
        QVERIFY(draftHash);
        QCOMPARE(a.sync->localMedia(*draftHash), draftPicture);

        // Bob, holding the current revision, asks for the draft's blob and the
        // published picture: only the published one comes, without a core.
        QVERIFY(net.sendRaw(b, a, rawRequest(revision, {*draftHash, pageMediaHash(picture)})));
        net.settle();
        const QVector<QByteArray> hashes = mediaHashesFrom(b, a);
        QCOMPARE(hashes.size(), 3);
        QCOMPARE(hashes.last(), pageMediaHash(picture));
        QVERIFY(!hashes.contains(*draftHash));
        QCOMPARE(countFrom(b, a, Kind::PageCore), 2); // the push and the first answer
    }

    void answersAreThrottledPerContactAndPerDay()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // asks by hand
        net.startSync(a);
        QVERIFY(publishPage(*a.sync, QStringLiteral("Throttled")) > 0);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1);

        // Each answer to "I have no page of yours" is one more core.
        const auto ask = [&](qint64 afterMs) {
            net.clock.advance(afterMs);
            if (!net.sendRaw(b, a, rawRequest()))
                return -1;
            net.settle();
            return countFrom(b, a, Kind::PageCore);
        };
        QCOMPARE(ask(0), 2);                        // answered
        QCOMPARE(ask(minute), 2);                   // a minute later: too soon
        QCOMPARE(ask(9 * minute - 1), 2);           // still under ten minutes since the answer
        QCOMPARE(ask(1), 3);                        // ten minutes
        QCOMPARE(ask(10 * minute), 4);
        QCOMPARE(ask(10 * minute), 5);              // the fourth answer today
        QCOMPARE(ask(10 * minute), 5);              // the fifth would be over the day's budget
        QCOMPARE(ask(10 * minute), 5);
        QCOMPARE(ask(24 * hour - 50 * minute), 6);  // a day after the first: a new budget
    }

    void mediaIsNotResentWithinAnHour()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // asks by hand
        net.startSync(a);
        const QByteArray picture = testJpeg('\x61');
        QVERIFY(publishPage(*a.sync, QStringLiteral("Picture"), picture) > 0);
        net.settle();

        const auto ask = [&](qint64 afterMs) {
            net.clock.advance(afterMs);
            return net.sendRaw(b, a, rawRequest());
        };
        QVERIFY(ask(0));
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 2);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 1);

        QVERIFY(ask(10 * minute)); // the picture went ten minutes ago
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 3);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 1);

        QVERIFY(ask(50 * minute - 1)); // a moment short of the hour
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 4);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 1);

        QVERIFY(ask(10 * minute)); // past the hour
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageCore), 5);
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);
    }

    void missingMediaIsRequestedOncePerRevisionAfterTheGrace()
    {
        TwoPeerFixture net;
        net.limits.missingMediaGraceMs = 50;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand, never answers
        net.startSync(a);
        const QByteArray picture = testJpeg('\x55');

        // Alice asked Bob something a moment ago: an ordinary request would
        // now wait for the back-off.
        PageRequestState recent{b.account};
        recent.lastRequestAtMs = net.clock.nowMs;
        recent.unanswered = 1;
        QVERIFY(a.pages().saveRequestState(recent).hasValue());

        QVERIFY(net.sendRaw(b, a, rawCore(1'000, QStringLiteral("Picture to come"), backgroundRef(picture))));
        net.settle();
        net.clock.advance(49);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0); // within the grace: it may be on its way
        net.clock.advance(1);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(b, a, Kind::PageRequest) == 1;
        }));
        const auto first = requestsFrom(b, a).first();
        QVERIFY(first.haveRevision == std::optional<qint64>(1'000));
        QCOMPARE(first.wantMedia, QVector<QByteArray>{pageMediaHash(picture)});
        QCOMPARE(requestStateOf(a, b).mediaRequestedRevision, qint64(1'000));

        // Once per revision: the same core again, time, or a look ask nothing more.
        QVERIFY(net.sendRaw(b, a, rawCore(1'000, QStringLiteral("Picture to come"), backgroundRef(picture))));
        net.settle();
        net.clock.advance(1'000);
        a.sync->markViewed(b.account);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 1);

        // A new revision still missing it asks once more after its own grace.
        QVERIFY(net.sendRaw(b, a, rawCore(2'000, QStringLiteral("Still to come"), backgroundRef(picture))));
        net.settle();
        net.clock.advance(50);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(b, a, Kind::PageRequest) == 2;
        }));
        QVERIFY(requestsFrom(b, a).last().haveRevision == std::optional<qint64>(2'000));
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 2);
    }

    // The grace request lives only in memory. A quit before it went out must
    // neither lose the media for good nor let a later look ask for it: only
    // evicted media may tell the owner a page was opened.
    void missingMediaRequestSurvivesARestart()
    {
        TwoPeerFixture net;
        net.limits.missingMediaGraceMs = 50;
        net.limits.startupRequestJitterMs = 20;
        net.random = [](qint64) { return qint64(10); };
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand, never pushes its picture
        net.startSync(a);
        const QByteArray picture = testJpeg('\x0C');
        QVERIFY(net.sendRaw(b, a, rawCore(5'000, QStringLiteral("Bob"), backgroundRef(picture))));
        net.settle();
        QVERIFY(!a.sync->contactPage(b.account)->backgroundPresent);

        // Alice quits within the grace. Opening the page then asks nothing:
        // none of it was evicted.
        QVERIFY(net.reopen(a));
        net.startSync(a);
        a.sync->markViewed(b.account);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0);
        QCOMPARE(requestStateOf(a, b).mediaRequestedRevision, PageRequestState::neverAsked);

        // The grace request is armed again, in the background, and not
        // before the core's own grace is over.
        net.clock.advance(49);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 0);
        net.clock.advance(1);
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(b, a, Kind::PageRequest) == 1;
        }));
        const auto first = requestsFrom(b, a).first();
        QVERIFY(first.haveRevision == std::optional<qint64>(5'000));
        QCOMPARE(first.wantMedia, QVector<QByteArray>{pageMediaHash(picture)});
        QCOMPARE(requestStateOf(a, b).mediaRequestedRevision, qint64(5'000));

        // A newer revision whose grace request is still waiting for the link
        // when Alice quits: after the restart a look queues nothing, and the
        // re-armed request goes once the link is back.
        QVERIFY(net.sendRaw(b, a, rawCore(6'000, QStringLiteral("Bob again"), backgroundRef(picture))));
        net.settle();
        a.transport.connected = false;
        net.clock.advance(50);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 1);
        QVERIFY(net.reopen(a));
        net.startSync(a);
        a.sync->markViewed(b.account);
        net.clock.advance(hour);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 1);
        a.transport.connectLink();
        QVERIFY(TwoPeerFixture::waitFor([&] {
            net.routeAll();
            return countFrom(b, a, Kind::PageRequest) == 2;
        }));
        QVERIFY(requestsFrom(b, a).last().haveRevision == std::optional<qint64>(6'000));
        QCOMPARE(requestStateOf(a, b).mediaRequestedRevision, qint64(6'000));

        // Once per revision: not a restart, a reconnect nor a look asks again.
        QVERIFY(net.reopen(a));
        net.startSync(a);
        a.transport.connectLink();
        a.sync->markViewed(b.account);
        net.clock.advance(hour);
        net.settle(100);
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 2);
    }

    void viewerRequestsBackOffWhileUnanswered()
    {
        TwoPeerFixture net;
        net.limits.startupRequestJitterMs = 1;
        net.random = [](qint64) { return qint64(0); };
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // an older client: never answers
        net.startSync(a);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 1);

        // Each reconnect asks again only once the back-off since the last
        // request has passed: 1 h, 2 h, 4 h, 8 h, 16 h, then a day at most.
        int sent = 1;
        for (const qint64 interval : {hour, 2 * hour, 4 * hour, 8 * hour, 16 * hour, 24 * hour, 24 * hour}) {
            net.clock.advance(interval - 1);
            a.transport.connectLink();
            net.settle();
            QCOMPARE(countFrom(b, a, Kind::PageRequest), sent);
            net.clock.advance(1);
            a.transport.connectLink();
            net.settle();
            QCOMPARE(countFrom(b, a, Kind::PageRequest), ++sent);
        }
        QCOMPARE(requestStateOf(a, b).unanswered, sent);
    }

    void anyPageMessageResetsTheBackOff_data()
    {
        QTest::addColumn<QByteArray>("message");
        QTest::newRow("core") << rawCore(3'000, QStringLiteral("Hi"));
        QTest::newRow("media") << rawMedia(Background, testJpeg('\x12'));
        QTest::newRow("request") << rawRequest();
        // A newer client's message this one cannot read says the same.
        QCborMap future;
        future.insert(qint64(0), 2);
        future.insert(qint64(1), 1);
        QTest::newRow("future version") << QByteArray(1, char(0xFF)) + future.toCborValue().toCbor();
    }

    void anyPageMessageResetsTheBackOff()
    {
        QFETCH(QByteArray, message);
        TwoPeerFixture net;
        net.limits.startupRequestJitterMs = 1;
        net.random = [](qint64) { return qint64(0); };
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        net.settle();
        net.clock.advance(hour);
        a.transport.connectLink();
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageRequest), 2);
        QCOMPARE(requestStateOf(a, b).unanswered, 2);
        QVERIFY(!deliveryOf(a, b).pageCapable);

        QVERIFY(net.sendRaw(b, a, message));
        net.settle();
        QCOMPARE(requestStateOf(a, b).unanswered, 0);
        QVERIFY(deliveryOf(a, b).pageCapable);

        // Still no page of Bob's: the next ask comes after the base interval,
        // not after the two hours two unanswered requests had earned.
        if (classifyProfilePayload(message) != Kind::PageCore) {
            net.clock.advance(30 * minute);
            a.transport.connectLink();
            net.settle();
            QCOMPARE(countFrom(b, a, Kind::PageRequest), 3);
        }
    }

    // --- Who and what is listened to ----------------------------------------

    void strangersPendingAndBlockedPeersGetNothing()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *stranger = net.addPeer(QStringLiteral("sam"));
        Peer *pending = net.addPeer(QStringLiteral("pat"));
        Peer *blocked = net.addPeer(QStringLiteral("kim"));
        QVERIFY(stranger && pending && blocked);
        // Sam shares a group with Alice, who has no contact row for him;
        QVERIFY(net.link(a, *stranger));
        QVERIFY(a.contacts().remove(stranger->account).hasValue());
        // Pat's request is still pending on Alice's side;
        QVERIFY(net.link(a, *pending, false, true));
        // Kim was a contact, now blocked.
        QVERIFY(net.link(a, *blocked));
        QVERIFY(a.contacts().block(blocked->account, net.clock.nowMs).hasValue());

        net.startSync(a);
        QVERIFY(publishPage(*a.sync, QStringLiteral("For contacts"), testJpeg('\x61')) > 0);
        for (Peer *peer : {stranger, pending, blocked}) {
            QVERIFY(net.sendRaw(*peer, a, rawCore(4'000, peer->name)));
            QVERIFY(net.sendRaw(*peer, a, rawRequest()));
        }
        net.settle();

        for (Peer *peer : {stranger, pending, blocked}) {
            QCOMPARE(countFrom(a, *peer, Kind::PageCore), 1); // it did arrive
            QCOMPARE(TwoPeerFixture::envelopesTo(a, peer->device), 0);
            QVERIFY(!a.sync->contactPage(peer->account));
            QVERIFY(!deliveryOf(a, *peer).pageCapable);
        }
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1); // Bob is a contact
    }

    // The sender cache remembers whose conversation it is; a contact blocked
    // after their page messages filled it (from a request row: the sync hears
    // of no new binding) must still be refused from then on.
    void contactBlockedAfterEarlierPagesIsIgnored()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer *kim = net.addPeer(QStringLiteral("kim")); // sends by hand
        QVERIFY(kim && net.link(a, *kim));
        net.startSync(a);
        QVERIFY(publishPage(*a.sync, QStringLiteral("For contacts"), testJpeg('\x61')) > 0);
        QVERIFY(net.sendRaw(*kim, a, rawCore(4'000, QStringLiteral("Kim"))));
        net.settle();
        QCOMPARE(a.pages().contactPage(kim->account).value()->revision, qint64(4'000));
        QVERIFY(deliveryOf(a, *kim).pageCapable);
        const int sentToKim = TwoPeerFixture::envelopesTo(a, kim->device);
        const auto changes = countSignals(*a.sync, &ProfilePageSync::contactPageChanged, kim->account);

        QVERIFY(a.contacts().block(kim->account, net.clock.nowMs).hasValue());
        // Any page message of a contact's would reset this back-off.
        PageRequestState backedOff{kim->account};
        backedOff.unanswered = 3;
        QVERIFY(a.pages().saveRequestState(backedOff).hasValue());
        net.clock.advance(hour); // past the answer interval
        QVERIFY(net.sendRaw(*kim, a, rawCore(5'000, QStringLiteral("Kim, blocked"))));
        QVERIFY(net.sendRaw(*kim, a, rawMedia(Background, testJpeg('\x62'))));
        QVERIFY(net.sendRaw(*kim, a, rawRequest()));
        net.settle();

        QCOMPARE(countFrom(a, *kim, Kind::PageCore), 2); // it did arrive
        const auto stored = a.pages().contactPage(kim->account);
        QVERIFY(stored.hasValue());
        QVERIFY(!stored.value() || stored.value()->revision == 4'000);
        QCOMPARE(*changes, 0);
        QCOMPARE(pendingMedia(a, *kim), 0);
        QCOMPARE(requestStateOf(a, *kim).unanswered, 3);
        QCOMPARE(TwoPeerFixture::envelopesTo(a, kim->device), sentToKim);
    }

    void groupConversationTrafficIsIgnored()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        QVERIFY(publishPage(*a.sync, QStringLiteral("Alice")) > 0);
        net.settle();
        const auto group = net.makeGroup(b, a);
        QVERIFY(group);

        // Bob, a contact, sends page messages into their group chat.
        b.engine().sendProfileUpdate(*group, a.device, rawCore(6'000, QStringLiteral("In the group")));
        b.engine().sendProfileUpdate(*group, a.device, rawRequest());
        net.settle();
        int inGroup = 0;
        for (const Arrival &arrival : a.arrivals)
            inGroup += arrival.conversation == *group ? 1 : 0;
        QCOMPARE(inGroup, 2); // Alice's engine did decrypt them…
        // …but a group is nobody's page channel.
        QVERIFY(!a.sync->contactPage(b.account));
        QCOMPARE(countFrom(b, a, Kind::PageCore), 1); // the push, no answer
        QVERIFY(!deliveryOf(a, b).pageCapable);

        QVERIFY(net.sendRaw(b, a, rawCore(6'000, QStringLiteral("Direct"))));
        net.settle();
        QVERIFY(a.sync->contactPage(b.account));
    }

    void legacyUpdatesAreLeftAlone()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // sends by hand
        net.startSync(a);
        PageRequestState waiting{b.account};
        waiting.lastRequestAtMs = net.clock.nowMs;
        waiting.unanswered = 3;
        QVERIFY(a.pages().saveRequestState(waiting).hasValue());
        const auto changes = countSignals(*a.sync, &ProfilePageSync::contactPageChanged, b.account);
        const auto awaiting = countSignals(*a.sync, &ProfilePageSync::awaitingChanged, b.account);

        ProfileUpdateMessage legacy;
        legacy.presence = 1;
        legacy.statusText = QStringLiteral("Out to lunch");
        QVERIFY(net.sendRaw(b, a, encodeProfileUpdate(legacy)));
        net.settle();

        QCOMPARE(countFrom(a, b, Kind::Legacy), 1);
        QCOMPARE(*changes, 0);
        QCOMPARE(*awaiting, 0);
        QCOMPARE(requestStateOf(a, b).unanswered, 3);
        QVERIFY(!deliveryOf(a, b).pageCapable);
        QVERIFY(!a.sync->contactPage(b.account));
        QCOMPARE(a.contacts().find(b.account).value()->statusText, QString()); // ChatController's job
    }

    void oldClientPeerIsHarmless()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // 0.2.8: no page support at all
        net.startSync(a);
        const QByteArray picture = testJpeg('\x61');
        const QByteArray song = testSong('\x62');
        QVERIFY(publishPage(*a.sync, QStringLiteral("Hello, old friend"), picture, song) > 0);
        net.settle();
        net.clock.advance(1'000);
        QVERIFY(a.sync->publish(pageWith(QStringLiteral("Hello again"), picture, song)) > 0);
        net.settle();

        // Two small cores, which his decoder reads as nothing at all.
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 2);
        QCOMPARE(b.arrivals.size(), qsizetype(2));
        for (const Arrival &arrival : b.arrivals) {
            QVERIFY(arrival.payload.size() <= maxPageCoreBytes);
            QVERIFY(!decodeProfileUpdate(arrival.payload));
        }
        QVERIFY(!a.engine().isFailedClosed());
        QVERIFY(!b.engine().isFailedClosed());
        QVERIFY(!a.sync->hasOwedDeliveries());
    }

    // --- Storage -------------------------------------------------------------

    void garbageCollectionAndSoftCapEviction()
    {
        TwoPeerFixture net;
        const QByteArray bobsPicture = testJpeg('\x0B', 10'000);
        const QByteArray carolsPicture = testJpeg('\x0C', 10'000);
        // Room for one received picture, not two.
        net.limits.receivedMediaSoftCapBytes = 15'000;
        net.limits.receivedMediaTargetBytes = 11'000;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        Peer *c = net.addPeer(QStringLiteral("carol"));
        QVERIFY(c && net.link(a, *c));
        net.startSync(a);
        net.startSync(b);
        net.startSync(*c);
        QVERIFY(makeCapable(net, a));

        QVERIFY(publishPage(*b.sync, QStringLiteral("Bob"), bobsPicture) > 0);
        net.settle();
        QVERIFY(a.sync->contactPage(b.account)->backgroundPresent);
        net.clock.advance(1'000);
        a.sync->markViewed(b.account); // Bob's page was looked at, Carol's never
        net.clock.advance(1'000);

        const auto carolChanges = countSignals(*a.sync, &ProfilePageSync::contactPageChanged, c->account);
        QVERIFY(publishPage(*c->sync, QStringLiteral("Carol"), carolsPicture) > 0);
        QVERIFY(TwoPeerFixture::waitFor([&] { return net.pendingTo(*c, a).size() == 2; }));
        const QVector<qsizetype> pending = net.pendingTo(*c, a);
        // Her core changes nothing about storage…
        net.deliver(*c, a, pending.at(0));
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(bobsPicture.size()));
        // …her picture takes it over the cap, and the least recently viewed
        // page's media goes: hers. Its core stays.
        net.deliver(*c, a, pending.at(1));
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(bobsPicture.size()));
        QVERIFY(a.sync->contactPage(b.account)->backgroundPresent);
        const auto carolsPage = a.sync->contactPage(c->account);
        QVERIFY(carolsPage && !carolsPage->backgroundPresent);
        QCOMPARE(*carolChanges, 3); // core, picture, eviction

        // Collection drops the pages of anyone no longer a contact.
        QVERIFY(a.contacts().remove(b.account).hasValue());
        a.sync->collectGarbage();
        QVERIFY(!a.sync->contactPage(b.account));
        QCOMPARE(a.pages().receivedMediaBytes().value(), qint64(0));
        QVERIFY(a.sync->contactPage(c->account));
    }

    void freshlyImportedBlobSurvivesACollection()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        net.startSync(a);
        const QByteArray picture = testJpeg('\x3C');
        const auto hash = a.sync->addLocalMedia(Background, picture);
        QVERIFY(hash);

        // However long the draft names it, no collection takes it.
        net.clock.advance(11 * minute);
        a.sync->collectGarbage();
        QCOMPARE(a.sync->localMedia(*hash), picture);

        // Let go of, it is kept for the grace, so undo can bring it back…
        QVERIFY(a.sync->saveDraft(pageWith(QStringLiteral("No picture")), QString()));
        QVERIFY(a.sync->localMedia(*hash).isEmpty()); // not the local page's now
        net.clock.advance(9 * minute);
        a.sync->collectGarbage();
        QVERIFY(a.sync->saveDraft(pageWith(QStringLiteral("Picture back"), picture), QString()));
        QCOMPARE(a.sync->localMedia(*hash), picture);

        // …but not beyond it.
        QVERIFY(a.sync->saveDraft(pageWith(QStringLiteral("No picture again")), QString()));
        net.clock.advance(10 * minute + 1);
        a.sync->collectGarbage();
        QVERIFY(a.sync->saveDraft(pageWith(QStringLiteral("Too late"), picture), QString()));
        QVERIFY(a.sync->localMedia(*hash).isEmpty());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("no usable local blob")));
        QCOMPARE(a.sync->publish(pageWith(QStringLiteral("Too late"), picture)), qint64(0));
    }

    // --- Size safety ---------------------------------------------------------

    void oversizedPayloadNeverReachesTheEngine()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b(); // asks by hand
        // Only a damaged store can hold a core this big (publish refuses
        // anything over 24 KiB). Encrypting it would pass the MLS plaintext
        // cap and stop the engine for the session.
        const QByteArray damaged(300 * 1024, 'x');
        QVERIFY(a.pages().savePublished(damaged, 5, std::nullopt, std::nullopt, net.clock.nowMs).hasValue());

        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression(QStringLiteral("Not sending a profile page payload of 307200 bytes")));
        net.startSync(a);
        net.settle();
        // An answer would carry the same core: held back as well.
        QVERIFY(net.sendRaw(b, a, rawRequest()));
        net.settle();

        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
        QVERIFY(!a.engine().isFailedClosed());
        QCOMPARE(deliveryOf(a, b).sentRevision, qint64(-1));
        QCOMPARE(a.sync->publishedRevision(), qint64(5));
    }

    void publishLargestMediaThroughRealMls()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        const QByteArray picture = largestJpeg();
        const QByteArray song = largestSong();
        QCOMPARE(picture.size(), maxBackgroundImageBytes);
        QCOMPARE(song.size(), maxSongBytes);
        QVERIFY(rawMedia(Background, picture).size() <= maxPageMediaMessageBytes);
        QVERIFY(rawMedia(Song, song).size() <= maxPageMediaMessageBytes);

        QVERIFY(publishPage(*a.sync, QStringLiteral("Largest"), picture, song) > 0);
        net.settle(200);

        QVERIFY(!a.engine().isFailedClosed());
        QVERIFY(!b.engine().isFailedClosed());
        const auto page = b.sync->contactPage(a.account);
        QVERIFY(page && page->backgroundPresent && page->songPresent);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(picture), Background), picture);
        QCOMPARE(b.sync->contactMedia(a.account, pageMediaHash(song), Song), song);
    }

    void publishLeavesNoSettledEnvelopesBehind()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);
        net.startSync(b);
        QVERIFY(makeCapable(net, b));
        QVERIFY(publishPage(*a.sync, QStringLiteral("Settled"), testJpeg('\x61'), testSong('\x62')) > 0);
        net.settle();
        QCOMPARE(countFrom(b, a, Kind::PageMedia), 2);

        // Every envelope was taken by the relay: none of their ciphertext stays.
        for (Peer *peer : {&a, &b}) {
            const OutboxInspector outbox(peer->paths.database, peer->vault.databaseKey());
            QVERIFY(outbox.isReadable());
            QCOMPARE(outbox.controlRows(), 0);
            QCOMPARE(outbox.rows(), 0);
        }
    }

    // --- The own page's store ------------------------------------------------

    void draftRoundTripsThroughTheStore()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        net.startSync(a);
        QVERIFY(!a.sync->draft());
        const QByteArray picture = testJpeg('\x0D');
        QVERIFY(a.sync->addLocalMedia(Background, picture));
        net.clock.advance(500);

        const Profile::Page draft = pageWith(QStringLiteral("  Draft   headline "), picture);
        const QString songSource = QStringLiteral("{\"path\":\"/music/song.wav\",\"startMs\":1000}");
        QVERIFY(a.sync->saveDraft(draft, songSource));
        QVERIFY(a.sync->draft() == std::optional<Profile::Page>(Profile::normalized(draft)));
        QCOMPARE(a.sync->draft()->content.headline, QStringLiteral("Draft headline"));
        QCOMPARE(a.sync->draftSongSource(), songSource);
        QCOMPARE(a.sync->draftUpdatedAtMs(), net.clock.nowMs);

        // It survives a restart; publishing it clears it.
        net.stopSync(a);
        QVERIFY(net.reopen(a));
        net.startSync(a);
        QVERIFY(a.sync->draft() == std::optional<Profile::Page>(Profile::normalized(draft)));
        QVERIFY(a.sync->publish(*a.sync->draft()) > 0);
        QVERIFY(!a.sync->draft());
        QCOMPARE(a.sync->publishedPage().content.headline, QStringLiteral("Draft headline"));
        QVERIFY(a.sync->publishedPage().background.sha256 == pageMediaHash(picture));

        // Discarding drops a later draft and leaves the published page.
        QVERIFY(a.sync->saveDraft(pageWith(QStringLiteral("Another")), songSource));
        QVERIFY(a.sync->draft());
        QVERIFY(a.sync->discardDraft());
        QVERIFY(!a.sync->draft());
        QCOMPARE(a.sync->draftSongSource(), QString());
        QCOMPARE(a.sync->publishedPage().content.headline, QStringLiteral("Draft headline"));
    }

    void publishRefusesMediaItCannotSend()
    {
        TwoPeerFixture net;
        QVERIFY(net.setUp());
        Peer &a = net.a();
        Peer &b = net.b();
        net.startSync(a);

        // Bytes no contact would accept are refused at import…
        const QRegularExpression refused(QStringLiteral("Refused profile media"));
        QTest::ignoreMessage(QtWarningMsg, refused);
        QVERIFY(!a.sync->addLocalMedia(Background, QByteArrayLiteral("not a picture")));
        QTest::ignoreMessage(QtWarningMsg, refused);
        QVERIFY(!a.sync->addLocalMedia(Song, testJpeg())); // a picture is not a song
        QTest::ignoreMessage(QtWarningMsg, refused);
        QVERIFY(!a.sync->addLocalMedia(Background, testJpeg('\x01', maxBackgroundImageBytes + 1)));

        // …and a page naming media not in the local store, or a stored blob
        // in the other slot, is not published.
        const QRegularExpression unusable(QStringLiteral("no usable local blob"));
        QTest::ignoreMessage(QtWarningMsg, unusable);
        QCOMPARE(a.sync->publish(pageWith(QStringLiteral("Missing picture"), testJpeg('\x0E'))), qint64(0));
        const QByteArray song = testSong();
        QVERIFY(a.sync->addLocalMedia(Song, song));
        Profile::Page songAsPicture = pageWith(QStringLiteral("Song as picture"));
        songAsPicture.background = backgroundRef(song);
        songAsPicture.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
        QTest::ignoreMessage(QtWarningMsg, unusable);
        QCOMPARE(a.sync->publish(songAsPicture), qint64(0));

        QCOMPARE(a.sync->publishedRevision(), qint64(0));
        net.settle();
        QCOMPARE(TwoPeerFixture::envelopesTo(a, b.device), 0);
    }
};

QTEST_GUILESS_MAIN(ProfilePageSyncTest)

#include "tst_profilepagesync.moc"
