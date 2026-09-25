// Chat attachments' bytes between real peers (docs/chat-attachments.md): each
// peer a ProfileSession with its own database, MLS state, SyncEngine and
// AttachmentTransfer, trading over scripted transports that a test routes by
// hand, so it decides what arrives, in which order, and what is lost.

#include "PageSyncTestSupport.h"

#include "app/AttachmentFileStore.h"
#include "app/AttachmentTransfer.h"
#include "domain/Attachment.h"
#include "domain/ProfilePageCodec.h"
#include "security/AttachmentSeal.h"
#include "storage/SqlCipherAttachmentRepository.h"

#include <QBuffer>
#include <QDir>
#include <QSet>
#include <QImage>
#include <QImageWriter>
#include <QRandomGenerator>
#include <QtTest>

#include <climits>
#include <memory>
#include <optional>
#include <vector>

using namespace OpenChat;
using PageSyncTest::Peer;
using PageSyncTest::TwoPeerFixture;

namespace {

// A fast pump and recovery check; everything timed (grace, back-off, the
// orphans' lifetime) runs on the fixture's manual clock.
AttachmentTransferLimits quickLimits()
{
    AttachmentTransferLimits limits;
    limits.pumpIntervalMs = 2;
    limits.recoveryCheckIntervalMs = 5;
    limits.collectIntervalMs = 60LL * 60 * 1000; // a test collects when it wants to
    return limits;
}

struct Change final {
    MessageId messageId;
    int state = 0;
    int reason = 0;
    int done = 0;
    int total = 0;
};

// One peer's transfer, what it reported, and the attachment messages its
// engine surfaced (by id: the sender's and every receiver's are the same).
struct Side final {
    Peer *peer = nullptr;
    std::unique_ptr<AttachmentTransfer> transfer;
    std::vector<Change> changes;
    std::vector<MessageId> received;
    std::vector<MessageId> previews;

    [[nodiscard]] AttachmentRepository &attachments() const { return *peer->session->attachments(); }
};

QByteArray randomBytes(qsizetype size, quint32 seed)
{
    QRandomGenerator generator(seed);
    QByteArray bytes(size, Qt::Uninitialized);
    for (qsizetype index = 0; index < size; index += 4) {
        const quint32 word = generator.generate();
        for (qsizetype byte = 0; byte < 4 && index + byte < size; ++byte)
            bytes[index + byte] = char((word >> (8 * byte)) & 0xFF);
    }
    return bytes;
}

QByteArray jpegOf(const QImage &image, int quality)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    return writer.write(image) ? bytes : QByteArray();
}

// Noise over a gradient: a photo of a few parts that no encoder squeezes.
QImage noisyPicture(int width, int height, quint32 seed)
{
    QRandomGenerator generator(seed);
    QImage image(width, height, QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int noise = int(generator.bounded(96));
            line[x] = qRgb((x * 255 / width + noise) % 256, (y * 255 / height + noise) % 256, (noise * 2) % 256);
        }
    }
    return image;
}

OutgoingAttachment fileAttachment(const QByteArray &bytes, const QString &name = QStringLiteral("notes.bin"))
{
    OutgoingAttachment attachment;
    attachment.descriptor.kind = AttachmentKind::File;
    attachment.descriptor.byteCount = bytes.size();
    attachment.descriptor.sha256 = pageMediaHash(bytes);
    attachment.descriptor.partCount = attachmentPartCount(bytes.size());
    attachment.descriptor.fileName = name;
    attachment.descriptor.mimeType = QStringLiteral("application/octet-stream");
    attachment.blob = bytes;
    return attachment;
}

OutgoingAttachment photoAttachment(int width = 1000, int height = 750)
{
    const QImage picture = noisyPicture(width, height, 7);
    OutgoingAttachment attachment;
    attachment.blob = jpegOf(picture, 80);
    // The preview is small and smooth, as the importer makes it.
    attachment.preview = jpegOf(picture.scaled(160, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                                    .scaled(320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation),
                                50);
    AttachmentDescriptor &descriptor = attachment.descriptor;
    descriptor.kind = AttachmentKind::Image;
    descriptor.byteCount = attachment.blob.size();
    descriptor.sha256 = pageMediaHash(attachment.blob);
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.fileName = QStringLiteral("ferry.jpg");
    descriptor.mimeType = QStringLiteral("image/jpeg");
    descriptor.width = width;
    descriptor.height = height;
    descriptor.hasPreview = previewIsAcceptable(attachment.preview);
    return attachment;
}

// The frame an AttachmentControl envelope carries, split; nothing for any
// other envelope.
std::optional<AttachmentFrameHeader> frameOf(const CiphertextEnvelopeV1 &envelope)
{
    if (envelope.messageKind != EnvelopeMessageKind::AttachmentControl)
        return std::nullopt;
    const auto split = splitAttachmentFrame(envelope.ciphertext);
    if (!split)
        return std::nullopt;
    return split->first;
}

// Frames of `type` the peer's engine handed to its link, each envelope once
// (an envelope the engine sends again after a restart is the same frame).
int framesFrom(const Peer &peer, AttachmentFrameType type, std::optional<DeviceId> to = std::nullopt)
{
    QSet<QByteArray> envelopes;
    for (const CiphertextEnvelopeV1 &envelope : peer.transport.sent) {
        const auto header = frameOf(envelope);
        if (header && header->type == type && (!to || envelope.recipientDeviceId == *to))
            envelopes.insert(envelope.envelopeId.bytes());
    }
    return int(envelopes.size());
}

class Fixture final
{
public:
    TwoPeerFixture fx;
    ConversationId direct = ConversationId::generate();
    // Every member's device, by conversation: each side's roster is this
    // minus itself.
    QHash<QByteArray, QList<DeviceId>> members;
    std::vector<std::unique_ptr<Side>> sides;

    Fixture() = default;
    ~Fixture()
    {
        // Before the fixture's sessions: each borrows its peer's.
        for (auto &side : sides)
            side->transfer.reset();
    }

    [[nodiscard]] bool setUp()
    {
        if (!fx.setUp())
            return false;
        const auto conversation = fx.a().conversationWith(fx.b());
        if (!conversation)
            return false;
        direct = *conversation;
        members.insert(direct.bytes(), {fx.a().device, fx.b().device});
        return true;
    }

    Side &start(Peer &peer, AttachmentTransferLimits limits = quickLimits())
    {
        Side *side = nullptr;
        for (auto &existing : sides) {
            if (existing->peer == &peer)
                side = existing.get();
        }
        if (side == nullptr) {
            sides.push_back(std::make_unique<Side>());
            side = sides.back().get();
            side->peer = &peer;
        }
        side->transfer = std::make_unique<AttachmentTransfer>(
            *peer.session, peer.engine(),
            [this, device = peer.device](const ConversationId &conversation) {
                QList<DeviceId> roster = members.value(conversation.bytes());
                roster.removeAll(device);
                return roster;
            },
            [this] { return fx.clock.nowMs; }, limits);
        AttachmentTransfer *transfer = side->transfer.get();
        QObject::connect(transfer, &AttachmentTransfer::transferChanged, transfer,
                         [side](const MessageId &id, int state, int reason, int done, int total) {
                             side->changes.push_back({id, state, reason, done, total});
                         });
        QObject::connect(transfer, &AttachmentTransfer::previewArrived, transfer,
                         [side](const MessageId &id) { side->previews.push_back(id); });
        QObject::connect(&peer.engine(), &SyncEngine::messageReceived, transfer,
                         [side](const MessageRecord &message) {
                             if (message.kind == ContentKind::Attachment)
                                 side->received.push_back(message.id);
                         });
        return *side;
    }

    // Seals `attachment` on `side` and queues its message: the message id.
    std::optional<MessageId> send(Side &side, const ConversationId &conversation,
                                  const QList<DeviceId> &recipients, bool group,
                                  const OutgoingAttachment &attachment, const QString &caption = QString())
    {
        std::optional<AttachmentTransfer::Staged> staged;
        side.transfer->stageOutgoing(conversation, attachment,
                                     [&staged](AttachmentTransfer::Staged result) { staged = std::move(result); });
        if (!QTest::qWaitFor([&] { return staged.has_value(); }, 20'000))
            return std::nullopt;
        const auto *descriptor = std::get_if<AttachmentDescriptor>(&*staged);
        if (descriptor == nullptr)
            return std::nullopt;
        std::optional<MessageId> queued;
        const auto connection = QObject::connect(
            &side.peer->engine(), &SyncEngine::messageQueued,
            [&queued](const MessageRecord &message) { queued = message.id; });
        const AttachmentSendRefusal refusal =
            side.peer->engine().enqueueAttachment(conversation, recipients, group, caption, *descriptor);
        QObject::disconnect(connection);
        if (refusal != AttachmentSendRefusal::None)
            return std::nullopt;
        return queued;
    }

    [[nodiscard]] static std::optional<StoredAttachment> stored(const Side &side, const MessageId &id)
    {
        const auto found = side.attachments().descriptorFor(id);
        if (!found.hasValue() || !found.value())
            return std::nullopt;
        return *found.value();
    }

    [[nodiscard]] static std::optional<AttachmentState> stateOf(const Side &side, const MessageId &id)
    {
        const auto found = stored(side, id);
        return found ? std::optional<AttachmentState>(found->state) : std::nullopt;
    }

    [[nodiscard]] static int heldParts(const Side &side, const AttachmentRef &ref)
    {
        const auto transfer = side.attachments().transfer(ref);
        return transfer.hasValue() && transfer.value() ? transfer.value()->haveCount : 0;
    }

    [[nodiscard]] static QByteArray loadBlob(Side &side, const MessageId &id)
    {
        std::optional<QByteArray> blob;
        side.transfer->loadBlob(id, [&blob](const QByteArray &bytes) { blob = bytes; });
        if (!QTest::qWaitFor([&] { return blob.has_value(); }, 20'000))
            return {};
        return *blob;
    }

    [[nodiscard]] static AttachmentFileStore filesOf(const Side &side)
    {
        return AttachmentFileStore(
            QDir(side.peer->session->profileDirectory()).filePath(QStringLiteral("attachments")));
    }

    // Runs the event loop and the relay until `done`.
    [[nodiscard]] bool settleUntil(const std::function<bool()> &done, int timeoutMs = 60'000)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < timeoutMs) {
            if (done())
                return true;
            QTest::qWait(1);
            fx.routeAll();
        }
        return done();
    }

    // Runs the event loop without routing anything for `ms`.
    static void idle(int ms) { QTest::qWait(ms); }

    [[nodiscard]] bool anyFailedClosed() const
    {
        for (const auto &side : sides) {
            if (side->peer->engine().isFailedClosed())
                return true;
        }
        return false;
    }

    // Hands `to` an AttachmentControl envelope from `from` carrying `frame`,
    // as the relay would.
    void deliverFrame(Peer &from, Peer &to, const ConversationId &conversation, const QByteArray &frame)
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const CiphertextEnvelopeV1 envelope{1,
                                            EnvelopeId::generate(),
                                            from.account,
                                            from.device,
                                            to.device,
                                            conversation,
                                            EnvelopeMessageKind::AttachmentControl,
                                            now,
                                            now + 3'600'000,
                                            EnvelopeId::generate(),
                                            frame,
                                            QCryptographicHash::hash(frame, QCryptographicHash::Sha256),
                                            QByteArray(64, '\x03')};
        to.engine().handleEnvelope(envelope, ++to.inboundSequence);
    }

    // The index in from's sent envelopes of its first MLS message to `to`.
    [[nodiscard]] static qsizetype messageIndex(const Peer &from, const Peer &to)
    {
        for (qsizetype index = 0; index < from.transport.sent.size(); ++index) {
            const auto &envelope = from.transport.sent.at(index);
            if (envelope.messageKind == EnvelopeMessageKind::MlsPrivateMessage
                && envelope.recipientDeviceId == to.device && !from.routed.contains(envelope.envelopeId.bytes()))
                return index;
        }
        return -1;
    }
};

} // namespace

class AttachmentTransferTest final : public QObject
{
    Q_OBJECT

private slots:
    void aPhotoArrivesWholeWithItsPreviewFirst()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const OutgoingAttachment picture = photoAttachment();
        QVERIFY(picture.descriptor.hasPreview);
        QVERIFY(picture.descriptor.partCount >= 2);

        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, picture, QStringLiteral("Look"));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));

        QCOMPARE(Fixture::stateOf(alice, *id), AttachmentState::Complete);
        QCOMPARE(Fixture::loadBlob(bob, *id), picture.blob);
        QCOMPARE(bob.attachments().preview(*id).value(), picture.preview);
        QVERIFY(std::find(bob.previews.cbegin(), bob.previews.cend(), *id) != bob.previews.cend());
        // Its message is the one both sides know it by.
        QVERIFY(std::find(bob.received.cbegin(), bob.received.cend(), *id) != bob.received.cend());
        // The preview went before any part, and every part exactly once.
        std::optional<AttachmentFrameType> first;
        for (const auto &envelope : f.fx.a().transport.sent) {
            if (const auto header = frameOf(envelope); header && !first)
                first = header->type;
        }
        QCOMPARE(first, AttachmentFrameType::Preview);
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Part), picture.descriptor.partCount);
        // Progress was reported part by part, ending complete.
        QVERIFY(!bob.changes.empty());
        QCOMPARE(bob.changes.back().state, int(AttachmentState::Complete));
        QCOMPARE(bob.changes.back().done, picture.descriptor.partCount);
        // The sender reads its own photo back too.
        QCOMPARE(Fixture::loadBlob(alice, *id), picture.blob);
        QVERIFY(!f.anyFailedClosed());
    }

    void aSixteenMegabyteFileArrivesByteForByte()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(AttachmentLimits::maxFileBytes, 16);
        const OutgoingAttachment file = fileAttachment(bytes, QStringLiteral("archive.zip"));
        QCOMPARE(file.descriptor.partCount, AttachmentLimits::maxParts);

        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, file);
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }, 120'000));
        QCOMPARE(Fixture::loadBlob(bob, *id), bytes);
        // What travelled and what is kept is sealed: the plaintext is nowhere
        // in either peer's file.
        const AttachmentRef ref = Fixture::stored(bob, *id)->ref;
        const auto sealed = Fixture::filesOf(bob).readPart(ref, 0, AttachmentLimits::partBytes + 16);
        QVERIFY(sealed.hasValue());
        QVERIFY(!sealed.value().contains(bytes.left(64)));
        QVERIFY(!f.anyFailedClosed());
    }

    void aGroupSendKeepsTheOutboxToOneFrameAndTextsGoFirst()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Peer *carol = f.fx.addPeer(QStringLiteral("carol"));
        Peer *dave = f.fx.addPeer(QStringLiteral("dave"));
        QVERIFY(carol && dave);
        const auto group = f.fx.makeGroupOf({&f.fx.a(), &f.fx.b(), carol, dave});
        QVERIFY(group);
        const QList<DeviceId> others{f.fx.b().device, carol->device, dave->device};
        f.members.insert(group->bytes(), {f.fx.a().device, f.fx.b().device, carol->device, dave->device});
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        Side &carolSide = f.start(*carol);
        Side &daveSide = f.start(*dave);
        // A slow uplink: what alice hands over stays unsent until flushed.
        f.fx.a().transport.backlogGrowsWithSends = true;
        const QByteArray bytes = randomBytes(6 * AttachmentLimits::partBytes - 1000, 3);
        const auto id = f.send(alice, *group, others, true, fileAttachment(bytes));
        QVERIFY(id);

        qsizetype textMark = -1;
        bool textQueued = false;
        const bool done = f.settleUntil([&] {
            // Never more than one frame's envelopes in the outbox.
            if (f.fx.a().engine().pendingAttachmentFrames() > others.size())
                return true;
            // A text written mid-transfer, while a frame is still in the outbox.
            if (!textQueued && framesFrom(f.fx.a(), AttachmentFrameType::Part) >= 2
                && f.fx.a().engine().pendingAttachmentFrames() > 0) {
                textQueued = true;
                textMark = f.fx.a().transport.sent.size();
                f.fx.a().engine().enqueueGroupText(*group, others, QStringLiteral("Mid-transfer"));
            }
            // The socket writes everything out; the engine hears the link is free.
            if (f.fx.a().transport.unflushedBytes > 0) {
                f.fx.a().transport.flush();
                f.fx.a().transport.connectLink();
            }
            return Fixture::stateOf(bob, *id) == AttachmentState::Complete
                   && Fixture::stateOf(carolSide, *id) == AttachmentState::Complete
                   && Fixture::stateOf(daveSide, *id) == AttachmentState::Complete;
        });
        QVERIFY(done);
        QVERIFY(f.fx.a().engine().pendingAttachmentFrames() <= others.size());
        QVERIFY(textQueued);
        // The text's three envelopes left before any frame envelope queued after it.
        int textsSeen = 0;
        for (qsizetype index = textMark; index < f.fx.a().transport.sent.size(); ++index) {
            const auto &envelope = f.fx.a().transport.sent.at(index);
            if (envelope.messageKind == EnvelopeMessageKind::MlsPrivateMessage) {
                ++textsSeen;
            } else if (envelope.messageKind == EnvelopeMessageKind::AttachmentControl) {
                QCOMPARE(textsSeen, int(others.size()));
                break;
            }
        }
        QCOMPARE(textsSeen, int(others.size()));
        QCOMPARE(Fixture::loadBlob(carolSide, *id), bytes);
        QVERIFY(!f.anyFailedClosed());
    }

    void aLinkThatDropsPausesTheTransferAndItResumes()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(5 * AttachmentLimits::partBytes, 5);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, fileAttachment(bytes));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return framesFrom(f.fx.a(), AttachmentFrameType::Part) >= 2; }));

        f.fx.a().transport.connected = false;
        const int sentWhileUp = framesFrom(f.fx.a(), AttachmentFrameType::Part);
        Fixture::idle(60);
        f.fx.routeAll();
        Fixture::idle(60);
        // At most the frame already handed to the engine went while down.
        QVERIFY(framesFrom(f.fx.a(), AttachmentFrameType::Part) <= sentWhileUp + 1);
        QCOMPARE(Fixture::stateOf(alice, *id), AttachmentState::Transferring);

        f.fx.a().transport.connectLink();
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QCOMPARE(Fixture::loadBlob(bob, *id), bytes);
        QVERIFY(!f.anyFailedClosed());
    }

    void aCallHoldsFramesBackUntilItEnds()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        alice.transfer->setCallActive(true);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, photoAttachment());
        QVERIFY(id);
        // The message itself goes; its bytes wait.
        QVERIFY(f.settleUntil([&] { return !bob.received.empty(); }));
        Fixture::idle(80);
        f.fx.routeAll();
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Preview), 0);
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Part), 0);

        alice.transfer->setCallActive(false);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QVERIFY(!f.anyFailedClosed());
    }

    void aRestartResumesWhereTheTransferStopped()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(6 * AttachmentLimits::partBytes, 6);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, fileAttachment(bytes));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return Fixture::heldParts(bob, Fixture::stored(alice, *id)->ref) >= 2; }));

        // Both restart mid-transfer.
        alice.transfer.reset();
        bob.transfer.reset();
        QVERIFY(f.fx.reopen(f.fx.a()));
        QVERIFY(f.fx.reopen(f.fx.b()));
        f.start(f.fx.a());
        f.start(f.fx.b());
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QCOMPARE(Fixture::stateOf(alice, *id), AttachmentState::Complete);
        QCOMPARE(Fixture::loadBlob(bob, *id), bytes);
        // No part went twice.
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Part), 6);
        QVERIFY(!f.anyFailedClosed());
    }

    void framesThatOvertakeTheirMessageAreKeptAndAdopted()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const OutgoingAttachment picture = photoAttachment();
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, picture);
        QVERIFY(id);
        // The relay takes the message but hands it on last.
        const qsizetype messageAt = Fixture::messageIndex(f.fx.a(), f.fx.b());
        QVERIFY(messageAt >= 0);
        const auto [message, sequence] = f.fx.acceptOnly(f.fx.a(), f.fx.b(), messageAt);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(alice, *id) == AttachmentState::Complete; }));
        f.fx.routeAll();

        // Everything is kept, sealed, before bob knows what it is.
        const AttachmentRef ref = Fixture::stored(alice, *id)->ref;
        QVERIFY(!Fixture::stored(bob, *id));
        QCOMPARE(Fixture::heldParts(bob, ref), picture.descriptor.partCount);
        const auto transfer = bob.attachments().transfer(ref);
        QVERIFY(transfer.hasValue() && transfer.value());
        QVERIFY(!transfer.value()->sealedPreview.isEmpty());

        f.fx.b().engine().handleEnvelope(message, sequence);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QCOMPARE(Fixture::loadBlob(bob, *id), picture.blob);
        QCOMPARE(bob.attachments().preview(*id).value(), picture.preview);
        QVERIFY(std::find(bob.previews.cbegin(), bob.previews.cend(), *id) != bob.previews.cend());
        QVERIFY(!f.anyFailedClosed());
    }

    void aLostPartIsAskedForAndSentAgain()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(4 * AttachmentLimits::partBytes - 7, 8);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, fileAttachment(bytes));
        QVERIFY(id);
        // Part 1 is taken by the relay and never arrives.
        bool lost = false;
        QElapsedTimer elapsed;
        elapsed.start();
        while (Fixture::stateOf(alice, *id) != AttachmentState::Complete && elapsed.elapsed() < 60'000) {
            QTest::qWait(1);
            for (const qsizetype index : f.fx.pendingTo(f.fx.a(), f.fx.b())) {
                const auto header = frameOf(f.fx.a().transport.sent.at(index));
                if (header && header->type == AttachmentFrameType::Part && header->index == 1) {
                    (void)f.fx.acceptOnly(f.fx.a(), f.fx.b(), index);
                    lost = true;
                }
            }
            f.fx.routeAll();
        }
        f.fx.routeAll();
        const AttachmentRef ref = Fixture::stored(alice, *id)->ref;
        QVERIFY(lost);
        QCOMPARE(Fixture::heldParts(bob, ref), 3);
        Fixture::idle(30);
        QCOMPARE(framesFrom(f.fx.b(), AttachmentFrameType::Request), 0); // too soon to ask

        // Two quiet minutes later bob asks, and alice sends part 1 again.
        f.fx.clock.advance(AttachmentTransferLimits{}.requestGraceMs + 1'000);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QCOMPARE(framesFrom(f.fx.b(), AttachmentFrameType::Request), 1);
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Part), 5);
        QCOMPARE(Fixture::loadBlob(bob, *id), bytes);
        // Asking again at once is not answered again: the rate limit holds.
        QVERIFY(!f.anyFailedClosed());
    }

    void aDamagedPartIsRefusedAndABlobThatDoesNotMatchFails()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(2 * AttachmentLimits::partBytes, 9);
        // Alice's own frames wait (as in a call) while forged ones arrive.
        alice.transfer->setCallActive(true);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, fileAttachment(bytes));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return !bob.received.empty(); }));
        const StoredAttachment sent = *Fixture::stored(alice, *id);
        const AttachmentRef ref = sent.ref;
        // A part sealed with the right key and changed on the way, and one
        // from someone without the key.
        const QByteArray first = bytes.left(AttachmentLimits::partBytes);
        QByteArray changed =
            sealAttachmentFrame(sent.descriptor.key, AttachmentFrameType::Part, ref.attachmentId, 0, first);
        changed[changed.size() - 40] = char(changed.at(changed.size() - 40) ^ 0x5A);
        const QByteArray forged =
            sealAttachmentFrame(QByteArray(32, 'x'), AttachmentFrameType::Part, ref.attachmentId, 0, first);
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct, changed);
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct, forged);
        QCOMPARE(Fixture::heldParts(bob, ref), 0);
        QVERIFY(bob.changes.empty());
        QCOMPARE(Fixture::stateOf(bob, *id), AttachmentState::Transferring);

        alice.transfer->setCallActive(false);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Complete; }));
        QCOMPARE(Fixture::loadBlob(bob, *id), bytes);

        // A sender whose bytes do not match what its message promised: the
        // receiver assembles them, finds out, and keeps nothing.
        OutgoingAttachment lying = fileAttachment(randomBytes(AttachmentLimits::partBytes + 5, 10));
        lying.descriptor.sha256 = pageMediaHash(QByteArray("something else"));
        QTest::ignoreMessage(QtWarningMsg, "A received attachment did not match its description");
        const auto lie = f.send(alice, f.direct, {f.fx.b().device}, false, lying);
        QVERIFY(lie);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *lie) == AttachmentState::Failed; }));
        const auto failed = Fixture::stored(bob, *lie);
        QCOMPARE(failed->reason, AttachmentFailure::Invalid);
        QVERIFY(!Fixture::filesOf(bob).contains(failed->ref));
        QCOMPARE(Fixture::heldParts(bob, failed->ref), 0);
        QVERIFY(Fixture::loadBlob(bob, *lie).isEmpty());
        QVERIFY(!f.anyFailedClosed());
    }

    void aMemberAddedMidTransferGetsNoFrames()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Peer *carol = f.fx.addPeer(QStringLiteral("carol"));
        Peer *dave = f.fx.addPeer(QStringLiteral("dave"));
        QVERIFY(carol && dave);
        const auto group = f.fx.makeGroupOf({&f.fx.a(), &f.fx.b(), carol});
        QVERIFY(group);
        f.members.insert(group->bytes(), {f.fx.a().device, f.fx.b().device, carol->device});
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        Side &carolSide = f.start(*carol);
        const QByteArray bytes = randomBytes(4 * AttachmentLimits::partBytes, 11);
        const auto id = f.send(alice, *group, {f.fx.b().device, carol->device}, true, fileAttachment(bytes));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return framesFrom(f.fx.a(), AttachmentFrameType::Part) >= 1; }));

        // Dave joins the group while the file is on its way.
        f.members[group->bytes()].append(dave->device);
        alice.transfer->wake();
        QVERIFY(f.settleUntil([&] {
            return Fixture::stateOf(bob, *id) == AttachmentState::Complete
                   && Fixture::stateOf(carolSide, *id) == AttachmentState::Complete;
        }));
        for (const auto &envelope : f.fx.a().transport.sent)
            QVERIFY(!(envelope.recipientDeviceId == dave->device
                      && envelope.messageKind == EnvelopeMessageKind::AttachmentControl));
        QVERIFY(!f.anyFailedClosed());
    }

    void cancellingStopsTheTransferForEveryone()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray bytes = randomBytes(6 * AttachmentLimits::partBytes, 12);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false, fileAttachment(bytes));
        QVERIFY(id);
        const AttachmentRef ref = Fixture::stored(alice, *id)->ref;
        QVERIFY(f.settleUntil([&] { return Fixture::heldParts(bob, ref) >= 2; }));

        QVERIFY(alice.transfer->cancel(*id));
        QVERIFY(!alice.transfer->cancel(*id)); // only once
        const int partsAtCancel = framesFrom(f.fx.a(), AttachmentFrameType::Part);
        QCOMPARE(Fixture::stateOf(alice, *id), AttachmentState::Cancelled);
        QCOMPARE(alice.changes.back().state, int(AttachmentState::Cancelled));
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Cancelled; }));
        QCOMPARE(Fixture::stored(bob, *id)->reason, AttachmentFailure::SenderCancelled);
        QVERIFY(!Fixture::filesOf(bob).contains(ref));
        Fixture::idle(50);
        f.fx.routeAll();
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Part), partsAtCancel);
        QCOMPARE(framesFrom(f.fx.a(), AttachmentFrameType::Cancel), 1);
        // What was sent stays with the sender: it can be sent again.
        QCOMPARE(Fixture::loadBlob(alice, *id), bytes);
        QVERIFY(!f.anyFailedClosed());
    }

    void aFullDiskRefusesWhatArrives()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        AttachmentTransferLimits full = quickLimits();
        full.minFreeDiskBytes = LLONG_MAX / 4; // no disk is that empty
        Side &bob = f.start(f.fx.b(), full);
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false,
                               fileAttachment(randomBytes(3 * AttachmentLimits::partBytes, 13)));
        QVERIFY(id);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *id) == AttachmentState::Failed; }));
        const auto refused = Fixture::stored(bob, *id);
        QCOMPARE(refused->reason, AttachmentFailure::NoSpace);
        QVERIFY(!Fixture::filesOf(bob).contains(refused->ref));
        QCOMPARE(bob.changes.back().state, int(AttachmentState::Failed));
        QCOMPARE(bob.changes.back().reason, int(AttachmentFailure::NoSpace));

        // And a budget of received bytes the same way.
        AttachmentTransferLimits small = quickLimits();
        small.maxReceivedBytes = 1'000;
        bob.transfer.reset();
        f.start(f.fx.b(), small);
        const auto next = f.send(alice, f.direct, {f.fx.b().device}, false,
                                 fileAttachment(randomBytes(AttachmentLimits::partBytes, 14)));
        QVERIFY(next);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(bob, *next) == AttachmentState::Failed; }));
        QCOMPARE(Fixture::stored(bob, *next)->reason, AttachmentFailure::NoSpace);
        QVERIFY(!f.anyFailedClosed());
    }

    void whatNeverGetsAMessageIsCollected()
    {
        Fixture f;
        QVERIFY(f.setUp());
        Side &alice = f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const auto id = f.send(alice, f.direct, {f.fx.b().device}, false,
                               fileAttachment(randomBytes(2 * AttachmentLimits::partBytes, 15)));
        QVERIFY(id);
        // The message is lost on the way; its frames are not.
        const qsizetype messageAt = Fixture::messageIndex(f.fx.a(), f.fx.b());
        QVERIFY(messageAt >= 0);
        (void)f.fx.acceptOnly(f.fx.a(), f.fx.b(), messageAt);
        QVERIFY(f.settleUntil([&] { return Fixture::stateOf(alice, *id) == AttachmentState::Complete; }));
        f.fx.routeAll();
        const AttachmentRef ref = Fixture::stored(alice, *id)->ref;
        QCOMPARE(Fixture::heldParts(bob, ref), 2);
        QVERIFY(Fixture::filesOf(bob).contains(ref));

        // Not yet: the message may still come.
        bob.transfer->collectGarbage();
        QCOMPARE(Fixture::heldParts(bob, ref), 2);
        f.fx.clock.advance(AttachmentTransferLimits{}.orphanTtlMs + 60'000);
        bob.transfer->collectGarbage();
        QCOMPARE(Fixture::heldParts(bob, ref), 0);
        QVERIFY(!Fixture::filesOf(bob).contains(ref));

        // A send sealed but never queued (a refusal, a crash) leaves nothing
        // behind either, and the sender's own finished file stays.
        std::optional<AttachmentTransfer::Staged> staged;
        alice.transfer->stageOutgoing(f.direct, fileAttachment(randomBytes(1'000, 16)),
                                      [&staged](AttachmentTransfer::Staged result) { staged = result; });
        QVERIFY(QTest::qWaitFor([&] { return staged.has_value(); }));
        const auto *sealed = std::get_if<AttachmentDescriptor>(&*staged);
        QVERIFY(sealed);
        const AttachmentRef unsent{f.direct, f.fx.a().device, sealed->attachmentId};
        QVERIFY(Fixture::filesOf(alice).contains(unsent));
        f.fx.clock.advance(AttachmentTransferLimits{}.orphanTtlMs + 60'000);
        alice.transfer->collectGarbage();
        QVERIFY(!Fixture::filesOf(alice).contains(unsent));
        QVERIFY(Fixture::filesOf(alice).contains(ref));
        QVERIFY(!f.anyFailedClosed());
    }

    void framesAheadOfTheirMessageCostOnlyWhatTheBudgetAllows()
    {
        Fixture f;
        QVERIFY(f.setUp());
        f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const QByteArray key(32, 'k');
        const AttachmentFileStore files = Fixture::filesOf(bob);

        // The last part a file may have, with next to nothing in it: it
        // reaches almost 17 MB into its file, and that is what it costs.
        const quint32 last = quint32(AttachmentLimits::maxParts - 1);
        QList<AttachmentRef> kept;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const AttachmentId id = AttachmentId::generate();
            f.deliverFrame(f.fx.a(), f.fx.b(), f.direct,
                           sealAttachmentFrame(key, AttachmentFrameType::Part, id, last, QByteArray()));
            const AttachmentRef ref{f.direct, f.fx.a().device, id};
            if (Fixture::heldParts(bob, ref) == 1)
                kept.append(ref);
        }
        // 32 MiB holds one such file, not two.
        QCOMPARE(kept.size(), 1);
        QVERIFY(files.contains(kept.first()));
        // Low parts still fit beside it, but no more than a few files' worth.
        int lowKept = 0;
        for (int attempt = 0; attempt < 40; ++attempt) {
            const AttachmentId id = AttachmentId::generate();
            f.deliverFrame(f.fx.a(), f.fx.b(), f.direct,
                           sealAttachmentFrame(key, AttachmentFrameType::Part, id, 0, QByteArray(10, 'p')));
            lowKept += Fixture::heldParts(bob, {f.direct, f.fx.a().device, id});
        }
        QCOMPARE(lowKept, AttachmentTransferLimits{}.orphanFilesPerSender - 1);

        // Nothing is kept from someone who is not in the chat.
        f.members.insert(f.direct.bytes(), {f.fx.b().device});
        const AttachmentId fromStranger = AttachmentId::generate();
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct,
                       sealAttachmentFrame(key, AttachmentFrameType::Part, fromStranger, 0, QByteArray(10, 'p')));
        QCOMPARE(Fixture::heldParts(bob, {f.direct, f.fx.a().device, fromStranger}), 0);
        QVERIFY(!f.anyFailedClosed());
    }

    void hostileFramesNeverStopTheEngine()
    {
        Fixture f;
        QVERIFY(f.setUp());
        f.start(f.fx.a());
        Side &bob = f.start(f.fx.b());
        const AttachmentId unknown = AttachmentId::generate();
        const QByteArray key(32, 'k');
        // Malformed; a request and a cancel about nothing bob sent or holds; a
        // part for a conversation bob is not in; a part too large to be one.
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct, QByteArray(40, '\x01'));
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct,
                       sealAttachmentFrame(key, AttachmentFrameType::Request, unknown, 0, QByteArray(1, '\xFF')));
        f.deliverFrame(f.fx.a(), f.fx.b(), f.direct,
                       sealAttachmentFrame(key, AttachmentFrameType::Cancel, unknown, 0, QByteArrayView()));
        const ConversationId stranger = ConversationId::generate();
        f.deliverFrame(f.fx.a(), f.fx.b(), stranger,
                       sealAttachmentFrame(key, AttachmentFrameType::Part, unknown, 0, QByteArray(100, 'p')));
        Fixture::idle(20);
        QCOMPARE(Fixture::heldParts(bob, {stranger, f.fx.a().device, unknown}), 0);
        QVERIFY(bob.changes.empty());
        QVERIFY(!f.fx.b().engine().isFailedClosed());
        QVERIFY(!f.anyFailedClosed());
    }
};

QTEST_GUILESS_MAIN(AttachmentTransferTest)

#include "tst_attachmenttransfer.moc"
