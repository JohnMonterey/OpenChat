#pragma once

// Real peers for profile page tests: each is a ProfileSession with its own
// database, device identity, SyncEngine and MLS state, talking over a
// scripted transport. Linked peers share a real 2-party MLS group and record
// each other as contacts, exactly as a completed contact handshake leaves
// them. Envelopes move only when a test routes them, so a test decides what
// arrives, in which order, and whether the relay acknowledged it.
//
// Every peer's ProfilePageSync (optional: a peer without one is an older
// client) runs on the fixture's manual clock and random source. The default
// limits keep background requests an hour away and the pump fast, so a test
// sees only the traffic it causes unless it opts in.

#include "app/ProfilePageSync.h"
#include "app/ProfileSession.h"
#include "crypto/MlsClient.h"
#include "domain/ChatTypes.h"
#include "domain/Contact.h"
#include "domain/ProfilePage.h"
#include "domain/ProfilePageCodec.h"
#include "domain/SongContainer.h"
#include "network/SyncEngine.h"
#include "protocol/CiphertextEnvelope.h"
#include "security/KeyVault.h"
#include "security/SecureBuffer.h"
#include "storage/SqlCipherChatRepository.h"
#include "storage/SqlCipherContactRepository.h"
#include "storage/SqlCipherProfilePageRepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace PageSyncTest {

// Minimal in-memory KeyVault (no OS keychain in tests). Keys survive a
// session's lock(), so a peer can be reopened as after a restart.
class InMemoryVault final : public OpenChat::KeyVault
{
public:
    using KeyVaultAvailability = OpenChat::KeyVaultAvailability;
    using KeyVaultError = OpenChat::KeyVaultError;
    using SecureBuffer = OpenChat::SecureBuffer;
    template<typename T>
    using Result = OpenChat::Result<T, KeyVaultError>;

    KeyVaultAvailability availability() const override { return KeyVaultAvailability::Available; }
    Result<SecureBuffer> readProfileKey(const OpenChat::ProfileId &) override { return read(m_databaseKey); }
    Result<SecureBuffer> createProfileKey(const OpenChat::ProfileId &) override { return create(m_databaseKey); }
    Result<void> deleteProfileKey(const OpenChat::ProfileId &) override
    {
        m_databaseKey.reset();
        return Result<void>::success();
    }
    Result<SecureBuffer> readDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        return read(m_wrappingKey);
    }
    Result<SecureBuffer> createDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        return create(m_wrappingKey);
    }
    Result<void> deleteDeviceWrappingKey(const OpenChat::ProfileId &) override
    {
        m_wrappingKey.reset();
        return Result<void>::success();
    }

    // The raw SQLCipher key, for tests that inspect the database on disk.
    [[nodiscard]] QByteArray databaseKey() const
    {
        return m_databaseKey ? m_databaseKey->view().toByteArray() : QByteArray();
    }

private:
    static Result<SecureBuffer> read(const std::optional<SecureBuffer> &key)
    {
        if (!key)
            return Result<SecureBuffer>::failure(KeyVaultError::NotFound);
        return Result<SecureBuffer>::success(SecureBuffer::fromBytes(key->view()));
    }
    static Result<SecureBuffer> create(std::optional<SecureBuffer> &key)
    {
        if (key)
            return Result<SecureBuffer>::failure(KeyVaultError::AlreadyExists);
        key = SecureBuffer::random(32);
        return Result<SecureBuffer>::success(SecureBuffer::fromBytes(key->view()));
    }

    std::optional<SecureBuffer> m_databaseKey;
    std::optional<SecureBuffer> m_wrappingKey;
};

// A relay link the test drives: whether it is up, and how many bytes it
// claims are still unsent.
class ScriptedTransport final : public OpenChat::SyncTransport
{
public:
    bool isConnected() const override { return connected; }
    void sendEnvelope(const OpenChat::CiphertextEnvelopeV1 &envelope) override
    {
        sent.append(envelope);
        if (backlogGrowsWithSends)
            unflushedBytes += envelope.ciphertext.size();
    }
    void sendDatagram(const OpenChat::CiphertextEnvelopeV1 &) override {}
    void acknowledge(const OpenChat::EnvelopeId &, quint64) override {}
    qint64 pendingSendBytes() const override { return backlog < 0 ? -1 : backlog + unflushedBytes; }

    // Brings the link up and tells the engine, as RelayClient does on every
    // (re)connect: the engine drains its outbox, then emits linkUp().
    void connectLink()
    {
        connected = true;
        if (onConnected)
            onConnected();
    }
    // Everything handed over so far has left the socket.
    void flush() { unflushedBytes = 0; }

    bool connected = true;
    qint64 backlog = 0; // -1: "cannot tell"
    // Counts each envelope handed over as unsent until flush(), like a slow
    // uplink would.
    bool backlogGrowsWithSends = false;
    qint64 unflushedBytes = 0;
    QVector<OpenChat::CiphertextEnvelopeV1> sent;
};

struct ManualClock final {
    qint64 nowMs = 1'790'000'000'000; // September 2026
    void advance(qint64 ms) { nowMs += ms; }
};

// One ProfileUpdate payload a peer's engine decrypted and surfaced.
struct Arrival final {
    OpenChat::ConversationId conversation;
    OpenChat::DeviceId sender;
    QByteArray payload;
};

struct Link final {
    OpenChat::AccountId other;
    OpenChat::ConversationId conversation;
};

struct Peer final {
    explicit Peer(QString peerName)
        : name(std::move(peerName))
    {
    }
    ~Peer()
    {
        // The sync borrows the session and its engine; the engine borrows
        // the transport (declared earlier, so destroyed later).
        sync.reset();
        if (session)
            session->lock();
    }
    Peer(const Peer &) = delete;
    Peer &operator=(const Peer &) = delete;

    [[nodiscard]] OpenChat::SyncEngine &engine() const { return *session->syncEngine(); }
    [[nodiscard]] OpenChat::ProfilePageRepository &pages() const { return *session->profilePages(); }
    [[nodiscard]] OpenChat::SqlCipherContactRepository &contacts() const { return *session->contacts(); }
    [[nodiscard]] std::optional<OpenChat::ConversationId> conversationWith(const Peer &other) const
    {
        for (const Link &link : links) {
            if (link.other == other.account)
                return link.conversation;
        }
        return std::nullopt;
    }

    QString name;
    InMemoryVault vault;
    OpenChat::ProfileId profileId = OpenChat::ProfileId::generate();
    OpenChat::ProfilePaths paths;
    ScriptedTransport transport;
    std::unique_ptr<OpenChat::ProfileSession> session;
    std::unique_ptr<OpenChat::ProfilePageSync> sync;
    // Replaced by the session's own at creation.
    OpenChat::AccountId account = OpenChat::AccountId::generate();
    OpenChat::DeviceId device = OpenChat::DeviceId::generate();
    QVector<Arrival> arrivals;
    QSet<QByteArray> routed; // envelope ids of this peer's sends already delivered
    quint64 inboundSequence = 0;
    QList<Link> links;
};

// Limits for tests: a fast pump, and background requests (whose delay the
// fixture's default random maximises) an hour away, so they stay out of a
// test unless it lowers them.
[[nodiscard]] inline OpenChat::ProfilePageSync::Limits quietLimits()
{
    OpenChat::ProfilePageSync::Limits limits;
    limits.pumpIntervalMs = 5;
    limits.acceptRequestDelayMs = 60LL * 60 * 1000;
    limits.acceptRequestJitterMs = 1;
    limits.startupRequestJitterMs = 60LL * 60 * 1000;
    limits.missingMediaGraceMs = 60LL * 60 * 1000;
    return limits;
}

class TwoPeerFixture final
{
public:
    ManualClock clock;
    // Uniform in [0, bound); the default picks the latest moment.
    std::function<qint64(qint64)> random = [](qint64 bound) { return bound - 1; };
    // Copied into each sync when it starts.
    OpenChat::ProfilePageSync::Limits limits = quietLimits();

    TwoPeerFixture() = default;
    TwoPeerFixture(const TwoPeerFixture &) = delete;
    TwoPeerFixture &operator=(const TwoPeerFixture &) = delete;
    ~TwoPeerFixture()
    {
        // Syncs first: one may still hold a queued call into another's engine.
        for (auto &peer : m_peers)
            peer->sync.reset();
        m_peers.clear();
    }

    // Peers A and B, online, each an Accepted contact of the other. No sync
    // runs yet (startSync).
    [[nodiscard]] bool setUp()
    {
        return m_dir.isValid() && addPeer(QStringLiteral("alice")) && addPeer(QStringLiteral("bob"))
               && link(a(), b()).has_value();
    }

    [[nodiscard]] Peer &a() const { return *m_peers.at(0); }
    [[nodiscard]] Peer &b() const { return *m_peers.at(1); }

    // A fresh profile with networking started, known to nobody.
    [[nodiscard]] Peer *addPeer(const QString &name)
    {
        using namespace OpenChat;
        auto peer = std::make_unique<Peer>(name);
        peer->paths = ProfilePaths::forProfile(m_dir.path(), peer->profileId);
        auto created = ProfileSession::create(peer->profileId, peer->vault, peer->paths);
        if (!created.hasValue())
            return nullptr;
        peer->session = std::move(created).value();
        const auto account = peer->session->accountId();
        const auto credential = peer->session->publicCredential();
        if (!account.hasValue() || !credential.hasValue()
            || !peer->session->startNetworking(peer->transport).hasValue())
            return nullptr;
        peer->account = account.value();
        peer->device = credential.value().deviceId;
        watch(*peer);
        m_peers.push_back(std::move(peer));
        return m_peers.back().get();
    }

    // A new 2-party MLS group (x creates it, y joins from the Welcome), both
    // states persisted, and each side's contact row for the other bound to
    // it with the other's device: Accepted, or PendingOutgoing when that side
    // has not accepted.
    std::optional<OpenChat::ConversationId> link(Peer &x, Peer &y, bool xAccepts = true, bool yAccepts = true)
    {
        const auto conversation = makeMlsGroup(x, y);
        if (!conversation || !recordContact(x, y, *conversation, xAccepts)
            || !recordContact(y, x, *conversation, yAccepts)
            || !upsertConversation(x, *conversation, OpenChat::ConversationKind::Direct)
            || !upsertConversation(y, *conversation, OpenChat::ConversationKind::Direct))
            return std::nullopt;
        x.links.push_back({y.account, *conversation});
        y.links.push_back({x.account, *conversation});
        return conversation;
    }

    // A group conversation of x and y: an MLS group no contact row names.
    std::optional<OpenChat::ConversationId> makeGroup(Peer &x, Peer &y)
    {
        const auto conversation = makeMlsGroup(x, y);
        if (!conversation || !upsertConversation(x, *conversation, OpenChat::ConversationKind::Group)
            || !upsertConversation(y, *conversation, OpenChat::ConversationKind::Group))
            return std::nullopt;
        return conversation;
    }

    // A group conversation of every one of `members` (at least two): the
    // first creates the MLS group and adds the others with one Welcome, and
    // each stores it as a Group conversation. Nobody's contacts change.
    std::optional<OpenChat::ConversationId> makeGroupOf(const QList<Peer *> &members)
    {
        using namespace OpenChat;
        if (members.size() < 2)
            return std::nullopt;
        const ConversationId conversation = ConversationId::generate();
        QList<QByteArray> keyPackages;
        for (qsizetype index = 1; index < members.size(); ++index) {
            auto keyPackage = members.at(index)->session->mls()->generateKeyPackage();
            if (!keyPackage.hasValue() || !members.at(index)->session->persistMlsState().hasValue())
                return std::nullopt;
            keyPackages.append(keyPackage.value());
        }
        Peer &creator = *members.first();
        if (!creator.session->mls()->createGroup(conversation).hasValue())
            return std::nullopt;
        auto added = creator.session->mls()->addMembers(conversation, keyPackages);
        if (!added.hasValue() || !creator.session->persistMlsState().hasValue())
            return std::nullopt;
        for (qsizetype index = 1; index < members.size(); ++index) {
            Peer &joiner = *members.at(index);
            if (!joiner.session->mls()->joinGroup(conversation, added.value().welcome).hasValue()
                || !joiner.session->persistMlsState().hasValue())
                return std::nullopt;
        }
        for (Peer *member : members) {
            if (!upsertConversation(*member, conversation, ConversationKind::Group))
                return std::nullopt;
        }
        return conversation;
    }

    // x accepts y (their row was PendingOutgoing).
    [[nodiscard]] bool acceptContact(Peer &x, const Peer &y)
    {
        const auto conversation = x.conversationWith(y);
        return conversation && x.contacts().markAccepted(y.account, *conversation, clock.nowMs).hasValue();
    }

    OpenChat::ProfilePageSync &startSync(Peer &peer)
    {
        peer.sync = std::make_unique<OpenChat::ProfilePageSync>(
            *peer.session, peer.engine(), [this] { return clock.nowMs; }, random, limits);
        return *peer.sync;
    }

    void stopSync(Peer &peer) { peer.sync.reset(); }

    // Locks the profile and unlocks it again with networking on the same
    // transport, as a restart does. The sync is not restarted.
    [[nodiscard]] bool reopen(Peer &peer)
    {
        using namespace OpenChat;
        peer.sync.reset();
        peer.session->lock();
        peer.session.reset();
        auto unlocked = ProfileSession::unlock(peer.profileId, peer.vault, peer.paths);
        if (!unlocked.hasValue())
            return false;
        peer.session = std::move(unlocked).value();
        if (!peer.session->startNetworking(peer.transport).hasValue())
            return false;
        watch(peer);
        return true;
    }

    // Indexes into from.transport.sent of the envelopes addressed to `to`
    // that have not been delivered yet, in the order they were sent.
    [[nodiscard]] QVector<qsizetype> pendingTo(const Peer &from, const Peer &to) const
    {
        QVector<qsizetype> pending;
        for (qsizetype index = 0; index < from.transport.sent.size(); ++index) {
            const auto &envelope = from.transport.sent.at(index);
            if (envelope.recipientDeviceId == to.device && !from.routed.contains(envelope.envelopeId.bytes()))
                pending.push_back(index);
        }
        return pending;
    }

    // The relay takes one envelope from `from` (acknowledging it to its
    // engine) and hands it to `to`'s engine.
    void deliver(Peer &from, Peer &to, qsizetype index)
    {
        const OpenChat::CiphertextEnvelopeV1 envelope = from.transport.sent.at(index);
        if (from.routed.contains(envelope.envelopeId.bytes()))
            return;
        from.routed.insert(envelope.envelopeId.bytes());
        const quint64 sequence = ++to.inboundSequence;
        if (from.transport.onRelayAccepted)
            from.transport.onRelayAccepted(envelope.envelopeId, sequence);
        to.engine().handleEnvelope(envelope, sequence);
    }

    // The relay takes one envelope from `from` (acknowledging it to its
    // engine) but holds it back: the test hands it on later, or never (a
    // loss). Returns the envelope and the sequence it would arrive under.
    std::pair<OpenChat::CiphertextEnvelopeV1, quint64> acceptOnly(Peer &from, Peer &to, qsizetype index)
    {
        const OpenChat::CiphertextEnvelopeV1 envelope = from.transport.sent.at(index);
        from.routed.insert(envelope.envelopeId.bytes());
        const quint64 sequence = ++to.inboundSequence;
        if (from.transport.onRelayAccepted)
            from.transport.onRelayAccepted(envelope.envelopeId, sequence);
        return {envelope, sequence};
    }

    // Delivers every pending envelope from `from` to `to`; returns how many.
    int route(Peer &from, Peer &to)
    {
        int delivered = 0;
        for (const qsizetype index : pendingTo(from, to)) {
            deliver(from, to, index);
            ++delivered;
        }
        return delivered;
    }

    int routeAll()
    {
        int delivered = 0;
        for (auto &from : m_peers) {
            for (auto &to : m_peers) {
                if (from != to)
                    delivered += route(*from, *to);
            }
        }
        return delivered;
    }

    // Runs the event loop and the relay until nothing has moved for quietMs.
    void settle(int quietMs = 40)
    {
        QElapsedTimer total;
        total.start();
        QElapsedTimer quiet;
        quiet.start();
        while (total.elapsed() < 15'000) {
            QTest::qWait(2);
            if (routeAll() > 0)
                quiet.restart();
            else if (quiet.elapsed() >= quietMs)
                return;
        }
    }

    // Runs the event loop without routing anything until `done`.
    [[nodiscard]] static bool waitFor(const std::function<bool()> &done, int timeoutMs = 5'000)
    {
        return QTest::qWaitFor(done, timeoutMs);
    }

    // Sends a payload straight through from's engine on its conversation with
    // `to`, as a client with its own ideas (a newer, older or hostile one)
    // would.
    [[nodiscard]] bool sendRaw(Peer &from, const Peer &to, const QByteArray &payload)
    {
        const auto conversation = from.conversationWith(to);
        if (!conversation)
            return false;
        from.engine().sendProfileUpdate(*conversation, to.device, payload);
        return !from.engine().isFailedClosed();
    }

    // The payloads of `kind` that reached `receiver` from `sender`, in order.
    [[nodiscard]] static QVector<QByteArray> payloadsFrom(const Peer &receiver, const Peer &sender,
                                                          OpenChat::ProfilePayloadKind kind)
    {
        QVector<QByteArray> payloads;
        for (const Arrival &arrival : receiver.arrivals) {
            if (arrival.sender == sender.device && OpenChat::classifyProfilePayload(arrival.payload) == kind)
                payloads.push_back(arrival.payload);
        }
        return payloads;
    }

    // Every ProfileUpdate envelope `from` handed to its link addressed to `to`.
    [[nodiscard]] static int envelopesTo(const Peer &from, const OpenChat::DeviceId &to)
    {
        int count = 0;
        for (const auto &envelope : from.transport.sent) {
            if (envelope.messageKind == OpenChat::EnvelopeMessageKind::ProfileUpdate
                && envelope.recipientDeviceId == to)
                ++count;
        }
        return count;
    }

private:
    std::optional<OpenChat::ConversationId> makeMlsGroup(Peer &x, Peer &y)
    {
        using namespace OpenChat;
        const ConversationId conversation = ConversationId::generate();
        auto keyPackage = y.session->mls()->generateKeyPackage();
        if (!keyPackage.hasValue() || !y.session->persistMlsState().hasValue()
            || !x.session->mls()->createGroup(conversation).hasValue())
            return std::nullopt;
        auto added = x.session->mls()->addMembers(conversation, {keyPackage.value()});
        if (!added.hasValue() || !x.session->persistMlsState().hasValue()
            || !y.session->mls()->joinGroup(conversation, added.value().welcome).hasValue()
            || !y.session->persistMlsState().hasValue())
            return std::nullopt;
        return conversation;
    }

    [[nodiscard]] bool recordContact(Peer &owner, const Peer &other, const OpenChat::ConversationId &conversation,
                                     bool accepted)
    {
        using namespace OpenChat;
        ContactRecord record{other.account, other.name,   QString(),   ContactState::PendingOutgoing,
                             conversation,  clock.nowMs, clock.nowMs};
        record.peerDeviceId = other.device;
        if (!owner.contacts().recordOutgoingRequest(record).hasValue())
            return false;
        return !accepted || owner.contacts().markAccepted(other.account, conversation, clock.nowMs).hasValue();
    }

    [[nodiscard]] bool upsertConversation(Peer &owner, const OpenChat::ConversationId &conversation,
                                          OpenChat::ConversationKind kind)
    {
        return owner.session->chats()
            ->upsertConversation(OpenChat::ConversationRecord{conversation, conversation.bytes(), QString(), kind,
                                                              clock.nowMs})
            .hasValue();
    }

    // Records every ProfileUpdate payload the peer's engine surfaces.
    static void watch(Peer &peer)
    {
        Peer *target = &peer;
        QObject::connect(&peer.engine(), &OpenChat::SyncEngine::profileUpdateReceived, &peer.engine(),
                         [target](const OpenChat::ConversationId &conversation,
                                  const OpenChat::DeviceId &sender, const QByteArray &payload) {
                             target->arrivals.push_back({conversation, sender, payload});
                         });
    }

    // Declared first: every peer's database lives in it.
    QTemporaryDir m_dir;
    std::vector<std::unique_ptr<Peer>> m_peers;
};

// --- Media blobs that pass the arrival checks (ProfilePageCodec.h).

// A JPEG as far as the marker walk is concerned: SOI, APP0, DQT, SOF2, DHT,
// one scan with stuffed bytes and a restart marker, EOI. Pixels are never
// decoded on arrival, so none are needed. `fill` makes blobs distinct; with
// `targetBytes` the scan data is padded to that exact size.
[[nodiscard]] inline QByteArray testJpeg(char fill = '\x5A', qsizetype targetBytes = 0)
{
    const auto segment = [](quint8 marker, int payloadBytes) {
        QByteArray bytes;
        bytes.append(char(0xFF));
        bytes.append(char(marker));
        bytes.append(char(((payloadBytes + 2) >> 8) & 0xFF));
        bytes.append(char((payloadBytes + 2) & 0xFF));
        bytes.append(QByteArray(payloadBytes, '\x11'));
        return bytes;
    };
    QByteArray jpeg("\xFF\xD8", 2);
    jpeg += segment(0xE0, 14);
    jpeg += segment(0xDB, 65);
    jpeg += segment(0xC2, 15);
    jpeg += segment(0xC4, 28);
    jpeg += segment(0xDA, 10);
    jpeg += QByteArray::fromHex("1234ff0056ffd07890ff00ab");
    const qsizetype padding = targetBytes > 0 ? targetBytes - jpeg.size() - 2 : 64;
    jpeg += QByteArray(std::max<qsizetype>(padding, 0), fill);
    jpeg += QByteArray("\xFF\xD9", 2);
    return jpeg;
}

// A well-formed song container of `packets` 60 ms mono packets of
// `packetBytes` bytes each.
[[nodiscard]] inline QByteArray testSong(char fill = '\x42', int packets = 10, int packetBytes = 100)
{
    OpenChat::SongContainer song;
    song.channels = 1;
    song.frameSamples = 2880;
    song.preSkip = 312;
    song.totalSamples = qint64(packets) * 2880 - 312;
    for (int packet = 0; packet < packets; ++packet)
        song.packets.push_back(QByteArray(packetBytes, fill));
    return OpenChat::encodeSongContainer(song);
}

// Exactly maxBackgroundImageBytes.
[[nodiscard]] inline QByteArray largestJpeg(char fill = '\x5A')
{
    return testJpeg(fill, OpenChat::maxBackgroundImageBytes);
}

// 180 packets of 1272/1273 bytes: a container of exactly maxSongBytes.
[[nodiscard]] inline QByteArray largestSong(char fill = '\x42')
{
    OpenChat::SongContainer song;
    song.channels = 1;
    song.frameSamples = 2880;
    song.preSkip = 312;
    song.totalSamples = 180 * 2880 - 312;
    for (int packet = 0; packet < 180; ++packet)
        song.packets.push_back(QByteArray(packet < 150 ? 1272 : 1273, fill));
    return OpenChat::encodeSongContainer(song);
}

[[nodiscard]] inline OpenChat::Profile::MediaRef backgroundRef(const QByteArray &jpeg)
{
    OpenChat::Profile::MediaRef ref;
    ref.sha256 = OpenChat::pageMediaHash(jpeg);
    ref.bytes = quint32(jpeg.size());
    ref.width = 640;
    ref.height = 480;
    return ref;
}

[[nodiscard]] inline OpenChat::Profile::MediaRef songRef(const QByteArray &song)
{
    OpenChat::Profile::MediaRef ref;
    ref.sha256 = OpenChat::pageMediaHash(song);
    ref.bytes = quint32(song.size());
    const auto container = OpenChat::decodeSongContainer(song);
    ref.durationMs = container ? quint32(container->durationMs()) : 0;
    return ref;
}

// A page with `headline`, naming the given blobs (none when empty).
[[nodiscard]] inline OpenChat::Profile::Page pageWith(const QString &headline, const QByteArray &background = {},
                                                      const QByteArray &song = {})
{
    using namespace OpenChat;
    Profile::Page page = Profile::defaultPage();
    page.content.headline = headline;
    if (!background.isEmpty()) {
        page.background = backgroundRef(background);
        page.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
    }
    if (!song.isEmpty())
        page.song = songRef(song);
    return page;
}

// Imports the blobs as the editor would, then publishes. Returns the revision.
[[nodiscard]] inline qint64 publishPage(OpenChat::ProfilePageSync &sync, const QString &headline,
                                        const QByteArray &background = {}, const QByteArray &song = {})
{
    using namespace OpenChat;
    if (!background.isEmpty() && !sync.addLocalMedia(Profile::MediaKind::BackgroundImageMedia, background))
        return 0;
    if (!song.isEmpty() && !sync.addLocalMedia(Profile::MediaKind::SongMedia, song))
        return 0;
    return sync.publish(pageWith(headline, background, song));
}

// A core as another client would send it: `revision` as given.
[[nodiscard]] inline QByteArray rawCore(qint64 revision, const QString &headline,
                                        const OpenChat::Profile::MediaRef &background = {},
                                        const OpenChat::Profile::MediaRef &song = {})
{
    using namespace OpenChat;
    Profile::Page page = Profile::defaultPage();
    page.revision = revision;
    page.publishedAtMs = revision;
    page.content.headline = headline;
    page.background = background;
    if (background.isSet())
        page.theme.backgroundKind = Profile::BackgroundKind::ImageBackground;
    page.song = song;
    return encodePageCore(page);
}

[[nodiscard]] inline QByteArray rawMedia(OpenChat::Profile::MediaKind kind, const QByteArray &data)
{
    return OpenChat::encodePageMedia({kind, OpenChat::pageMediaHash(data), data});
}

[[nodiscard]] inline QByteArray rawRequest(std::optional<qint64> haveRevision = std::nullopt,
                                           const QVector<QByteArray> &wantMedia = {})
{
    return OpenChat::encodePageRequest({haveRevision, wantMedia});
}

} // namespace PageSyncTest
