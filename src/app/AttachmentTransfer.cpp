#include "app/AttachmentTransfer.h"
#include "diagnostics/Logging.h"

#include "app/AttachmentFileStore.h"
#include "app/ProfileSession.h"
#include "domain/ProfilePageCodec.h"
#include "network/SyncEngine.h"
#include "security/AttachmentSeal.h"
#include "storage/SqlCipherAttachmentRepository.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <algorithm>
#include <climits>
#include <utility>

namespace OpenChat {

namespace {

constexpr qsizetype sealOverhead = AttachmentLimits::sealOverhead;
// The request back-off doubles at most this often before its cap applies.
constexpr int maxBackoffDoublings = 16;
// Answers remembered for their rate limit; past this many the stale ones go.
constexpr int maxRememberedAnswers = 256;

[[nodiscard]] QByteArray answerKey(const MessageId &messageId, const DeviceId &requester)
{
    return messageId.bytes() + requester.bytes();
}

[[nodiscard]] qsizetype sealedPartSize(const AttachmentDescriptor &descriptor, int index)
{
    return attachmentPartSize(descriptor, index) + sealOverhead;
}

// How far an orphan's file reaches: to the end of the slot of the last part
// it holds.
[[nodiscard]] qint64 orphanExtent(const AttachmentTransferRecord &orphan)
{
    for (int index = int(orphan.present.size()) * 8 - 1; index >= 0; --index) {
        if (orphan.hasPart(index))
            return AttachmentFileStore::partOffset(index + 1);
    }
    return 0;
}

// Every part of `descriptor` read back from `files` and opened: the blob, or
// nothing when one is missing or does not open. On a worker.
[[nodiscard]] std::optional<QByteArray> assemble(const AttachmentFileStore &files, const AttachmentRef &ref,
                                                 const AttachmentDescriptor &descriptor,
                                                 const std::atomic_bool &stopping)
{
    if (descriptor.byteCount < 1 || descriptor.partCount < 1
        || descriptor.partCount != attachmentPartCount(descriptor.byteCount))
        return std::nullopt;
    QByteArray blob;
    blob.reserve(qsizetype(descriptor.byteCount));
    for (int index = 0; index < descriptor.partCount; ++index) {
        if (stopping)
            return std::nullopt;
        const auto body = files.readPart(ref, index, sealedPartSize(descriptor, index));
        if (!body.hasValue())
            return std::nullopt;
        const QByteArray header =
            attachmentFrameHeader(AttachmentFrameType::Part, descriptor.attachmentId, quint32(index));
        const auto part = openAttachmentBody(descriptor.key, header, body.value());
        if (!part || part->size() != attachmentPartSize(descriptor, index))
            return std::nullopt;
        blob += *part;
    }
    if (blob.size() != descriptor.byteCount)
        return std::nullopt;
    return blob;
}

// Runs `apply` on the GUI thread later, if the transfer is still there. From
// a worker: the pointer is only ever read on the GUI thread.
template<typename Apply>
void backOnGui(const QPointer<AttachmentTransfer> &transfer, Apply apply)
{
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [transfer, apply = std::move(apply)]() mutable {
            if (transfer)
                apply(*transfer);
        },
        Qt::QueuedConnection);
}

} // namespace

AttachmentTransfer::AttachmentTransfer(ProfileSession &session, SyncEngine &engine, RosterFn roster,
                                       Clock clock, Limits limits, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_engine(engine)
    , m_roster(std::move(roster))
    , m_clock(std::move(clock))
    , m_limits(limits)
    , m_files(std::make_unique<AttachmentFileStore>(
          QDir(session.profileDirectory()).filePath(QStringLiteral("attachments"))))
    , m_pool(std::make_unique<QThreadPool>())
    , m_stopping(std::make_shared<std::atomic_bool>(false))
{
    m_pool->setMaxThreadCount(2);
    m_pool->setThreadPriority(QThread::LowPriority);

    connect(&m_engine, &SyncEngine::attachmentFrameReceived, this, &AttachmentTransfer::onFrameReceived);
    connect(&m_engine, &SyncEngine::messageQueued, this, &AttachmentTransfer::onMessageQueued);
    connect(&m_engine, &SyncEngine::messageReceived, this, &AttachmentTransfer::onMessageReceived);
    connect(&m_engine, &SyncEngine::messageStateChanged, this, &AttachmentTransfer::onMessageStateChanged);
    connect(&m_engine, &SyncEngine::linkUp, this, &AttachmentTransfer::onLinkUp);

    m_pumpTimer.setInterval(int(std::clamp<qint64>(m_limits.pumpIntervalMs, 1, INT_MAX)));
    connect(&m_pumpTimer, &QTimer::timeout, this, &AttachmentTransfer::pumpOnce);
    m_recoveryTimer.setInterval(int(std::clamp<qint64>(m_limits.recoveryCheckIntervalMs, 1, INT_MAX)));
    connect(&m_recoveryTimer, &QTimer::timeout, this, &AttachmentTransfer::runRecovery);
    m_collectTimer.setInterval(int(std::clamp<qint64>(m_limits.collectIntervalMs, 1, INT_MAX)));
    connect(&m_collectTimer, &QTimer::timeout, this, &AttachmentTransfer::collectGarbage);

    if (m_engine.isLinkUp())
        m_linkUpSinceMs = now();
    collectGarbage();
    // What a crash left half done: every part came but the blob was never
    // checked. Sending resumes in the pump.
    if (AttachmentRepository *attachments = repository(); attachments != nullptr) {
        if (const auto local = localDevice()) {
            const auto incoming = attachments->incomingActive(*local);
            if (incoming.hasValue()) {
                for (const StoredAttachment &stored : incoming.value()) {
                    const auto transfer = attachments->transfer(stored.ref);
                    if (transfer.hasValue() && transfer.value()
                        && transfer.value()->haveCount >= stored.descriptor.partCount)
                        validate(stored);
                }
            }
        }
    }
    m_recoveryTimer.start();
    m_collectTimer.start();
    wakePump();
}

AttachmentTransfer::~AttachmentTransfer()
{
    // Work under way stops at its next part; nothing it produces lands here.
    m_stopping->store(true);
    m_pool->clear();
    m_pool->waitForDone();
}

AttachmentRepository *AttachmentTransfer::repository() const
{
    return m_session.attachments();
}

std::optional<DeviceId> AttachmentTransfer::localDevice() const
{
    const auto credential = m_session.publicCredential();
    if (!credential.hasValue())
        return std::nullopt;
    return credential.value().deviceId;
}

qint64 AttachmentTransfer::now() const
{
    return m_clock ? m_clock() : QDateTime::currentMSecsSinceEpoch();
}

void AttachmentTransfer::report(const StoredAttachment &stored, AttachmentState state, AttachmentFailure reason,
                                int done)
{
    if (state != AttachmentState::Transferring)
        m_arrivedAtMs.remove(stored.messageId.bytes());
    emit transferChanged(stored.messageId, int(state), int(reason), done, stored.descriptor.partCount);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void AttachmentTransfer::stageOutgoing(const ConversationId &conversation, OutgoingAttachment attachment,
                                       std::function<void(Staged)> done)
{
    const auto fail = [this, done](const QString &message) {
        QMetaObject::invokeMethod(
            this, [done, message] { done(message); }, Qt::QueuedConnection);
    };
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    if (attachments == nullptr || !local) {
        fail(QStringLiteral("Attachments can't be sent right now."));
        return;
    }
    AttachmentDescriptor descriptor = std::move(attachment.descriptor);
    descriptor.attachmentId = AttachmentId::generate();
    descriptor.key = randomAttachmentKey();
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    if (descriptor.key.size() != AttachmentLimits::keyBytes || attachment.blob.size() != descriptor.byteCount
        || !isValidDescriptor(descriptor)) {
        fail(QStringLiteral("This attachment can't be sent."));
        return;
    }
    const AttachmentRef ref{conversation, *local, descriptor.attachmentId};
    // A mark in the transfer bookkeeping, first: should this device stop
    // before the message is queued, the collection finds the bytes it left.
    if (!attachments->recordPartArrived(ref, 0, 0, now()).hasValue()) {
        fail(QStringLiteral("Attachments can't be sent right now."));
        return;
    }
    const QPointer<AttachmentTransfer> self(this);
    const AttachmentFileStore files = *m_files;
    const std::shared_ptr<std::atomic_bool> stopping = m_stopping;
    QByteArray preview = std::move(attachment.preview);
    m_pool->start([self, files, stopping, ref, descriptor, blob = std::move(attachment.blob),
                   preview = std::move(preview), done = std::move(done)] {
        bool sealed = true;
        for (int index = 0; sealed && index < descriptor.partCount; ++index) {
            if (*stopping) {
                sealed = false;
                break;
            }
            const QByteArray frame = sealAttachmentFrame(
                descriptor.key, AttachmentFrameType::Part, descriptor.attachmentId, quint32(index),
                QByteArrayView(blob).mid(qsizetype(index) * AttachmentLimits::partBytes,
                                         attachmentPartSize(descriptor, index)));
            sealed = !frame.isEmpty()
                     && files.writePart(ref, index, QByteArrayView(frame).mid(attachmentFrameHeaderBytes))
                            .hasValue();
        }
        if (!sealed)
            (void)files.remove(ref);
        backOnGui(self, [sealed, ref, descriptor, preview, done](AttachmentTransfer &transfer) {
            if (!sealed) {
                if (AttachmentRepository *attachments = transfer.repository())
                    (void)attachments->deleteTransfer(ref);
                qCWarning(mediaLog) << "An attachment could not be written to the attachment store";
                done(QStringLiteral("There isn't enough space on this computer to send that."));
                return;
            }
            if (!preview.isEmpty())
                transfer.m_stagedPreviews.insert(descriptor.attachmentId.bytes(), preview);
            done(descriptor);
        });
    });
}

void AttachmentTransfer::discardStaged(const ConversationId &conversation, const AttachmentId &attachmentId)
{
    const auto local = localDevice();
    if (!local)
        return;
    const AttachmentRef ref{conversation, *local, attachmentId};
    m_stagedPreviews.remove(attachmentId.bytes());
    (void)m_files->remove(ref);
    if (AttachmentRepository *attachments = repository())
        (void)attachments->deleteTransfer(ref);
}

void AttachmentTransfer::onMessageQueued(const MessageRecord &message)
{
    if (message.kind != ContentKind::Attachment || !message.attachment || message.flow != MessageFlow::Outgoing)
        return;
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    if (attachments == nullptr || !local)
        return;
    // The mark kept its bytes from being collected until now; the message
    // holds them from here on.
    const AttachmentRef ref{message.conversationId, *local, message.attachment->attachmentId};
    (void)attachments->deleteTransfer(ref);
    const QByteArray preview = m_stagedPreviews.take(message.attachment->attachmentId.bytes());
    if (!preview.isEmpty()) {
        const auto stored = attachments->setPreview(message.id, preview);
        if (stored.hasValue() && stored.value())
            emit previewArrived(message.id);
    }
    // The bytes follow once the message is Sent.
}

void AttachmentTransfer::onMessageStateChanged(const MessageId &messageId, DeliveryState state)
{
    if (state == DeliveryState::Sent || state == DeliveryState::Delivered || state == DeliveryState::Read) {
        wakePump();
        return;
    }
    if (state != DeliveryState::Failed)
        return;
    // Its message never went: nobody would know what its bytes are.
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return;
    const auto stored = attachments->descriptorFor(messageId);
    if (stored.hasValue() && stored.value() && stored.value()->flow == MessageFlow::Outgoing
        && stored.value()->state == AttachmentState::Transferring)
        finishOutgoing(*stored.value(), AttachmentState::Cancelled, AttachmentFailure::SendFailed);
}

void AttachmentTransfer::onLinkUp()
{
    m_linkUpSinceMs = now();
    wakePump();
}

bool AttachmentTransfer::cancel(const MessageId &messageId)
{
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return false;
    const auto found = attachments->descriptorFor(messageId);
    if (!found.hasValue() || !found.value())
        return false;
    const StoredAttachment &stored = *found.value();
    if (stored.flow != MessageFlow::Outgoing || stored.state != AttachmentState::Transferring)
        return false;
    const auto changed =
        attachments->setState(messageId, AttachmentState::Cancelled, AttachmentFailure::SenderCancelled);
    if (!changed.hasValue() || !changed.value())
        return false;
    std::erase_if(m_answers, [&](const Answer &answer) { return answer.stored.messageId == messageId; });
    // Whoever has its message is told to stop waiting; nobody else is.
    const QList<DeviceId> recipients = currentRecipients(stored);
    if (!recipients.isEmpty()) {
        const QByteArray frame = sealAttachmentFrame(stored.descriptor.key, AttachmentFrameType::Cancel,
                                                     stored.descriptor.attachmentId, 0, QByteArrayView());
        if (!frame.isEmpty() && m_engine.sendAttachmentFrame(stored.ref.conversationId, recipients, frame))
            ++m_framesSent;
    }
    report(stored, AttachmentState::Cancelled, AttachmentFailure::SenderCancelled, stored.partsSent);
    return true;
}

void AttachmentTransfer::setCallActive(bool active)
{
    if (m_callActive == active)
        return;
    m_callActive = active;
    if (!active)
        wakePump(); // frames held back during the call
}

void AttachmentTransfer::wake()
{
    wakePump();
}

void AttachmentTransfer::wakePump()
{
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
    if (m_pumpWakeQueued)
        return;
    m_pumpWakeQueued = true;
    // Soon, but never on the caller's stack (a signal of the engine's).
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_pumpWakeQueued = false;
            pumpOnce();
        },
        Qt::QueuedConnection);
}

bool AttachmentTransfer::pumpGateOpen() const
{
    // One frame's envelopes at a time: the last one's are all out of the
    // outbox (so a group's fan-out never piles up), and little is waiting
    // in the socket, so texts, receipts and call signals go first.
    if (m_engine.pendingAttachmentFrames() > 0)
        return false;
    const qint64 backlog = m_engine.pendingSendBytes();
    return m_limits.pumpBacklogBytes <= 0 || backlog <= m_limits.pumpBacklogBytes;
}

void AttachmentTransfer::pumpOnce()
{
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    if (attachments == nullptr || !local) {
        m_pumpTimer.stop();
        return;
    }
    if (m_engine.isFailedClosed()) {
        m_pumpTimer.stop();
        if (!m_failedClosedLogged) {
            m_failedClosedLogged = true;
            qCWarning(mediaLog) << "The sync engine has stopped; attachments are not sent";
        }
        return;
    }
    // Nothing goes while the link is down or a call is on; linkUp() and the
    // call's end wake the pump again.
    if (!m_engine.isLinkUp() || m_callActive) {
        m_pumpTimer.stop();
        return;
    }
    if (!m_pumpTimer.isActive())
        m_pumpTimer.start();
    if (!pumpGateOpen())
        return; // the timer looks again
    // Someone waiting on a missing part first.
    if (sendAnswerFrame())
        return;

    const auto active = attachments->outgoingActive(*local);
    if (!active.hasValue())
        return;
    QVector<const StoredAttachment *> sendable;
    for (const StoredAttachment &stored : active.value()) {
        switch (stored.deliveryState) {
        case DeliveryState::Failed:
            finishOutgoing(stored, AttachmentState::Cancelled, AttachmentFailure::SendFailed);
            break;
        case DeliveryState::Sent:
        case DeliveryState::Delivered:
        case DeliveryState::Read:
            sendable.push_back(&stored);
            break;
        case DeliveryState::Draft:
        case DeliveryState::Queued:
        case DeliveryState::Sending:
            break; // its message is not out yet
        }
    }
    if (sendable.isEmpty()) {
        if (m_answers.empty())
            m_pumpTimer.stop();
        return;
    }
    // Round-robin: the one after the last served, so one large file never
    // holds the others back.
    qsizetype start = 0;
    for (qsizetype index = 0; index < sendable.size(); ++index) {
        if (sendable.at(index)->messageId.bytes() == m_lastServed) {
            start = index + 1;
            break;
        }
    }
    for (qsizetype tried = 0; tried < sendable.size(); ++tried) {
        const StoredAttachment &stored = *sendable.at((start + tried) % sendable.size());
        m_lastServed = stored.messageId.bytes();
        if (sendNextFrame(stored) || m_engine.isFailedClosed())
            return;
    }
}

QList<DeviceId> AttachmentTransfer::currentRecipients(const StoredAttachment &stored) const
{
    // Those the message went to who are still there: somebody who joined
    // since never gets the bytes of what was sent before them.
    const QList<DeviceId> roster = m_roster ? m_roster(stored.ref.conversationId) : QList<DeviceId>();
    QList<DeviceId> recipients;
    for (const DeviceId &device : stored.recipients) {
        if (roster.contains(device) && !recipients.contains(device))
            recipients.append(device);
    }
    return recipients;
}

QByteArray AttachmentTransfer::partFrame(const StoredAttachment &stored, int index) const
{
    const AttachmentDescriptor &descriptor = stored.descriptor;
    if (index < 0 || index >= descriptor.partCount)
        return {};
    const auto body = m_files->readPart(stored.ref, index, sealedPartSize(descriptor, index));
    if (!body.hasValue())
        return {};
    return attachmentFrameHeader(AttachmentFrameType::Part, descriptor.attachmentId, quint32(index))
           + body.value();
}

bool AttachmentTransfer::sendNextFrame(const StoredAttachment &stored)
{
    AttachmentRepository *attachments = repository();
    const AttachmentDescriptor &descriptor = stored.descriptor;
    const QList<DeviceId> recipients = currentRecipients(stored);
    if (recipients.isEmpty()) {
        // Everyone it went to left (or the contact's device changed).
        finishOutgoing(stored, AttachmentState::Cancelled, AttachmentFailure::SendFailed);
        return false;
    }
    const ConversationId &conversation = stored.ref.conversationId;
    // The preview first, so the bubble shows something at once.
    if (descriptor.hasPreview && !stored.previewSent) {
        const auto preview = attachments->preview(stored.messageId);
        if (preview.hasValue() && previewIsAcceptable(preview.value())) {
            const QByteArray frame = sealAttachmentFrame(descriptor.key, AttachmentFrameType::Preview,
                                                         descriptor.attachmentId, 0, preview.value());
            if (frame.isEmpty() || !m_engine.sendAttachmentFrame(conversation, recipients, frame))
                return false;
            ++m_framesSent;
            (void)attachments->recordFrameSent(stored.messageId, stored.partsSent, true);
            return true;
        }
        // Lost before its message was queued: the parts go without it.
        (void)attachments->recordFrameSent(stored.messageId, stored.partsSent, true);
    }
    if (stored.partsSent >= descriptor.partCount) {
        finishOutgoing(stored, AttachmentState::Complete, AttachmentFailure::None);
        return false;
    }
    const int index = stored.partsSent;
    const QByteArray frame = partFrame(stored, index);
    if (frame.isEmpty()) {
        qCWarning(mediaLog) << "An attachment's stored bytes are gone; it cannot be sent";
        finishOutgoing(stored, AttachmentState::Failed, AttachmentFailure::SendFailed);
        return false;
    }
    if (!m_engine.sendAttachmentFrame(conversation, recipients, frame))
        return false;
    ++m_framesSent;
    const int sent = index + 1;
    if (!attachments->recordFrameSent(stored.messageId, sent, true).hasValue())
        qCWarning(mediaLog) << "An attachment's progress could not be recorded";
    if (sent >= descriptor.partCount)
        finishOutgoing(stored, AttachmentState::Complete, AttachmentFailure::None);
    else
        report(stored, AttachmentState::Transferring, AttachmentFailure::None, sent);
    return true;
}

bool AttachmentTransfer::sendAnswerFrame()
{
    while (!m_answers.empty()) {
        Answer &answer = m_answers.front();
        const ConversationId &conversation = answer.stored.ref.conversationId;
        const QList<DeviceId> roster = m_roster ? m_roster(conversation) : QList<DeviceId>();
        if (!roster.contains(answer.requester)) {
            m_answers.pop_front();
            continue;
        }
        const AttachmentDescriptor &descriptor = answer.stored.descriptor;
        if (answer.cancel) {
            const QByteArray frame = sealAttachmentFrame(descriptor.key, AttachmentFrameType::Cancel,
                                                         descriptor.attachmentId, 0, QByteArrayView());
            const bool sent =
                !frame.isEmpty() && m_engine.sendAttachmentFrame(conversation, {answer.requester}, frame);
            m_answers.pop_front(); // best effort: another request brings another answer
            if (sent)
                ++m_framesSent;
            return sent;
        }
        if (answer.parts.empty()) {
            m_answers.pop_front();
            continue;
        }
        const QByteArray frame = partFrame(answer.stored, answer.parts.front());
        if (frame.isEmpty()) {
            m_answers.pop_front();
            continue;
        }
        if (!m_engine.sendAttachmentFrame(conversation, {answer.requester}, frame))
            return false;
        ++m_framesSent;
        answer.parts.pop_front();
        if (answer.parts.empty())
            m_answers.pop_front();
        return true;
    }
    return false;
}

void AttachmentTransfer::finishOutgoing(const StoredAttachment &stored, AttachmentState state,
                                        AttachmentFailure reason)
{
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return;
    const auto changed = attachments->setState(stored.messageId, state, reason);
    if (!changed.hasValue() || !changed.value())
        return;
    if (state != AttachmentState::Complete) {
        std::erase_if(m_answers,
                      [&](const Answer &answer) { return answer.stored.messageId == stored.messageId; });
    }
    report(stored, state, reason,
           state == AttachmentState::Complete ? stored.descriptor.partCount : stored.partsSent);
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

void AttachmentTransfer::onFrameReceived(const ConversationId &conversation, const DeviceId &sender,
                                         const QByteArray &frame)
{
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    const auto split = splitAttachmentFrame(frame);
    if (attachments == nullptr || !local || !split || sender == *local)
        return;
    // Nothing is kept for a conversation that is gone or was left.
    const auto live = attachments->conversationIsLive(conversation);
    if (!live.hasValue() || !live.value())
        return;
    const AttachmentFrameHeader &header = split->first;
    switch (header.type) {
    case AttachmentFrameType::Part:
        receivePart({conversation, sender, header.attachmentId}, int(header.index), frame);
        return;
    case AttachmentFrameType::Preview:
        receivePreview({conversation, sender, header.attachmentId}, frame);
        return;
    case AttachmentFrameType::Request:
        // About an attachment this device sent.
        receiveRequest({conversation, *local, header.attachmentId}, sender, frame);
        return;
    case AttachmentFrameType::Cancel:
        receiveCancel({conversation, sender, header.attachmentId}, frame);
        return;
    }
}

bool AttachmentTransfer::storageAllows(qint64 bytes)
{
    const qint64 free = m_files->freeBytes();
    if (free >= 0 && free - bytes < m_limits.minFreeDiskBytes)
        return false;
    AttachmentRepository *attachments = repository();
    const auto received = attachments ? attachments->receivedBytes() : Result<qint64, RepositoryError>::success(0);
    return received.hasValue() && received.value() + bytes <= m_limits.maxReceivedBytes;
}

void AttachmentTransfer::receivePart(const AttachmentRef &ref, int index, const QByteArray &frame)
{
    AttachmentRepository *attachments = repository();
    const QByteArrayView body = QByteArrayView(frame).mid(attachmentFrameHeaderBytes);
    const auto found = attachments->descriptorByRef(ref);
    if (!found.hasValue())
        return;
    const qint64 nowMs = now();
    if (!found.value()) {
        // Before its message (the relay can reorder): kept sealed, from
        // someone in the chat, within a budget per sender, and checked once
        // the message says what it is. A part lands at its fixed offset in
        // the file, so the budget counts how far each file reaches rather
        // than the bytes it holds: one high part would otherwise cost a
        // whole file's worth of disk for a few counted bytes.
        if (body.size() > AttachmentLimits::partBytes + sealOverhead)
            return;
        if (!m_roster || !m_roster(ref.conversationId).contains(ref.senderDeviceId))
            return;
        const auto orphans = attachments->orphans(LLONG_MAX);
        if (!orphans.hasValue())
            return;
        qint64 charged = 0;
        qint64 heldExtent = 0;
        int files = 0;
        bool known = false;
        for (const AttachmentTransferRecord &orphan : orphans.value()) {
            if (orphan.ref.conversationId != ref.conversationId
                || orphan.ref.senderDeviceId != ref.senderDeviceId || orphan.haveCount == 0)
                continue;
            if (orphan.ref == ref) {
                if (orphan.hasPart(index))
                    return;
                heldExtent = orphanExtent(orphan);
                known = true;
            }
            charged += orphanExtent(orphan);
            ++files;
        }
        const qint64 extent = std::max(heldExtent, AttachmentFileStore::partOffset(index + 1));
        if ((!known && files >= m_limits.orphanFilesPerSender)
            || charged - heldExtent + extent > m_limits.orphanBytesPerSender
            || !storageAllows(extent - heldExtent))
            return;
        if (m_files->writePart(ref, index, body).hasValue())
            (void)attachments->recordPartArrived(ref, index, body.size(), nowMs);
        return;
    }
    const StoredAttachment &stored = *found.value();
    const AttachmentDescriptor &descriptor = stored.descriptor;
    if (stored.flow != MessageFlow::Incoming || stored.state != AttachmentState::Transferring
        || index >= descriptor.partCount || body.size() != sealedPartSize(descriptor, index))
        return;
    // Only a part that opens with the key is stored: a forged or damaged one
    // never takes space, and never makes the whole transfer fail.
    if (!openAttachmentFrame(descriptor.key, frame))
        return;
    const auto held = attachments->transfer(ref);
    if (held.hasValue() && held.value() && held.value()->hasPart(index))
        return;
    if (!storageAllows(body.size()) || !m_files->writePart(ref, index, body).hasValue()) {
        failIncoming(stored, AttachmentFailure::NoSpace);
        return;
    }
    const auto arrival = attachments->recordPartArrived(ref, index, body.size(), nowMs);
    if (!arrival.hasValue() || !arrival.value().changed)
        return;
    report(stored, AttachmentState::Transferring, AttachmentFailure::None, arrival.value().haveCount);
    if (arrival.value().haveCount >= descriptor.partCount)
        validate(stored);
}

void AttachmentTransfer::receivePreview(const AttachmentRef &ref, const QByteArray &frame)
{
    AttachmentRepository *attachments = repository();
    const QByteArrayView body = QByteArrayView(frame).mid(attachmentFrameHeaderBytes);
    const auto found = attachments->descriptorByRef(ref);
    if (!found.hasValue())
        return;
    if (!found.value()) {
        // Kept sealed until its message comes, a few per sender at most.
        if (body.size() > AttachmentLimits::maxPreviewBytes + AttachmentLimits::controlSealOverhead)
            return;
        const auto orphans = attachments->orphans(LLONG_MAX);
        if (!orphans.hasValue())
            return;
        const auto previews = std::count_if(orphans.value().cbegin(), orphans.value().cend(),
                                            [&](const AttachmentTransferRecord &orphan) {
                                                return orphan.ref.conversationId == ref.conversationId
                                                       && orphan.ref.senderDeviceId == ref.senderDeviceId
                                                       && !orphan.sealedPreview.isEmpty();
                                            });
        if (previews < m_limits.orphanPreviewsPerSender)
            (void)attachments->setSealedPreview(ref, body, now());
        return;
    }
    const StoredAttachment &stored = *found.value();
    if (stored.flow != MessageFlow::Incoming || !stored.descriptor.hasPreview)
        return;
    const auto jpeg = openAttachmentFrame(stored.descriptor.key, frame);
    if (!jpeg || !previewIsAcceptable(*jpeg))
        return;
    const auto existing = attachments->preview(stored.messageId);
    if (!existing.hasValue() || !existing.value().isEmpty())
        return; // already shown
    const auto set = attachments->setPreview(stored.messageId, *jpeg);
    if (set.hasValue() && set.value())
        emit previewArrived(stored.messageId);
}

void AttachmentTransfer::receiveRequest(const AttachmentRef &ref, const DeviceId &requester,
                                        const QByteArray &frame)
{
    AttachmentRepository *attachments = repository();
    const auto found = attachments->descriptorByRef(ref);
    if (!found.hasValue() || !found.value() || found.value()->flow != MessageFlow::Outgoing)
        return;
    const StoredAttachment &stored = *found.value();
    const auto bitmap = openAttachmentFrame(stored.descriptor.key, frame);
    if (!bitmap || bitmap->size() != (stored.descriptor.partCount + 7) / 8)
        return;
    // Only someone the message went to who is still there, and not more
    // than once in a while.
    const QList<DeviceId> roster = m_roster ? m_roster(ref.conversationId) : QList<DeviceId>();
    if (!stored.recipients.contains(requester) || !roster.contains(requester))
        return;
    const qint64 nowMs = now();
    const QByteArray key = answerKey(stored.messageId, requester);
    if (const auto last = m_lastAnswerMs.constFind(key);
        last != m_lastAnswerMs.cend() && nowMs - *last < m_limits.answerIntervalMs)
        return;
    if (m_lastAnswerMs.size() >= maxRememberedAnswers) {
        m_lastAnswerMs.removeIf([&](QHash<QByteArray, qint64>::iterator entry) {
            return nowMs - entry.value() >= m_limits.answerIntervalMs;
        });
    }
    m_lastAnswerMs.insert(key, nowMs);
    std::erase_if(m_answers, [&](const Answer &answer) {
        return answer.stored.messageId == stored.messageId && answer.requester == requester;
    });
    Answer answer{stored, requester, {}, false};
    if (stored.state == AttachmentState::Failed || stored.state == AttachmentState::Cancelled) {
        // It stopped: say so again, so they stop asking.
        answer.cancel = true;
    } else {
        // Only parts already handed out once: the pump sends the rest anyway.
        for (int index = 0; index < std::min(stored.partsSent, stored.descriptor.partCount); ++index) {
            if ((quint8(bitmap->at(index / 8)) >> (index % 8)) & 1)
                answer.parts.push_back(index);
        }
        if (answer.parts.empty())
            return;
    }
    m_answers.push_back(std::move(answer));
    wakePump();
}

void AttachmentTransfer::receiveCancel(const AttachmentRef &ref, const QByteArray &frame)
{
    AttachmentRepository *attachments = repository();
    const auto found = attachments->descriptorByRef(ref);
    if (!found.hasValue() || !found.value())
        return;
    const StoredAttachment &stored = *found.value();
    if (stored.flow != MessageFlow::Incoming || stored.state != AttachmentState::Transferring
        || !openAttachmentFrame(stored.descriptor.key, frame))
        return;
    const auto changed =
        attachments->setState(stored.messageId, AttachmentState::Cancelled, AttachmentFailure::SenderCancelled);
    if (!changed.hasValue() || !changed.value())
        return;
    (void)m_files->remove(ref);
    (void)attachments->deleteTransfer(ref);
    report(stored, AttachmentState::Cancelled, AttachmentFailure::SenderCancelled, 0);
}

void AttachmentTransfer::failIncoming(const StoredAttachment &stored, AttachmentFailure reason)
{
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return;
    // The bytes first: should this stop half way, nothing is left unnamed.
    (void)m_files->remove(stored.ref);
    const auto failed = attachments->failIncoming(stored.messageId, reason);
    if (failed.hasValue() && failed.value())
        report(stored, AttachmentState::Failed, reason, 0);
}

void AttachmentTransfer::onMessageReceived(const MessageRecord &message)
{
    if (message.kind != ContentKind::Attachment || !message.attachment || message.flow != MessageFlow::Incoming)
        return;
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return;
    // The stored row, not the message: a clash with an earlier attachment id
    // leaves the message without one.
    const auto found = attachments->descriptorFor(message.id);
    if (!found.hasValue() || !found.value())
        return;
    const StoredAttachment &stored = *found.value();
    if (stored.flow != MessageFlow::Incoming || stored.state != AttachmentState::Transferring)
        return;
    // Its bytes are due from now on: a request waits the grace from here.
    m_arrivedAtMs.insert(message.id.bytes(), now());
    const auto transfer = attachments->transfer(stored.ref);
    if (transfer.hasValue() && transfer.value())
        adoptOrphans(stored, *transfer.value());
}

void AttachmentTransfer::adoptOrphans(const StoredAttachment &stored, const AttachmentTransferRecord &transfer)
{
    const QByteArray id = stored.messageId.bytes();
    if (m_validating.contains(id))
        return;
    m_validating.insert(id);
    const QPointer<AttachmentTransfer> self(this);
    const AttachmentFileStore files = *m_files;
    const std::shared_ptr<std::atomic_bool> stopping = m_stopping;
    m_pool->start([self, files, stopping, stored, transfer] {
        const AttachmentDescriptor &descriptor = stored.descriptor;
        QVector<int> bad;
        for (int index = 0; index < AttachmentLimits::maxParts; ++index) {
            if (*stopping)
                return;
            if (!transfer.hasPart(index))
                continue;
            if (index >= descriptor.partCount) {
                bad.append(index);
                continue;
            }
            const auto body = files.readPart(stored.ref, index, sealedPartSize(descriptor, index));
            const QByteArray header =
                attachmentFrameHeader(AttachmentFrameType::Part, descriptor.attachmentId, quint32(index));
            if (!body.hasValue() || !openAttachmentBody(descriptor.key, header, body.value()))
                bad.append(index);
        }
        QByteArray preview;
        if (!transfer.sealedPreview.isEmpty() && descriptor.hasPreview) {
            const auto opened = openAttachmentBody(
                descriptor.key, attachmentFrameHeader(AttachmentFrameType::Preview, descriptor.attachmentId, 0),
                transfer.sealedPreview);
            if (opened && previewIsAcceptable(*opened))
                preview = *opened;
        }
        backOnGui(self, [stored, bad, preview, hadSealedPreview = !transfer.sealedPreview.isEmpty()](
                            AttachmentTransfer &owner) {
            owner.m_validating.remove(stored.messageId.bytes());
            AttachmentRepository *attachments = owner.repository();
            if (attachments == nullptr)
                return;
            const qint64 nowMs = owner.now();
            for (const int index : bad) {
                const qsizetype bytes = index < stored.descriptor.partCount
                                            ? sealedPartSize(stored.descriptor, index)
                                            : AttachmentLimits::partBytes + sealOverhead;
                (void)attachments->clearPart(stored.ref, index, bytes, nowMs);
            }
            if (hadSealedPreview)
                (void)attachments->setSealedPreview(stored.ref, QByteArrayView(), nowMs);
            if (!preview.isEmpty()) {
                const auto set = attachments->setPreview(stored.messageId, preview);
                if (set.hasValue() && set.value())
                    emit owner.previewArrived(stored.messageId);
            }
            const auto current = attachments->descriptorFor(stored.messageId);
            const auto transfer = attachments->transfer(stored.ref);
            if (!current.hasValue() || !current.value()
                || current.value()->state != AttachmentState::Transferring || !transfer.hasValue()
                || !transfer.value())
                return;
            owner.report(stored, AttachmentState::Transferring, AttachmentFailure::None,
                         transfer.value()->haveCount);
            if (transfer.value()->haveCount >= stored.descriptor.partCount)
                owner.validate(*current.value());
        });
    });
}

void AttachmentTransfer::validate(const StoredAttachment &stored)
{
    const QByteArray id = stored.messageId.bytes();
    if (m_validating.contains(id))
        return;
    m_validating.insert(id);
    const QPointer<AttachmentTransfer> self(this);
    const AttachmentFileStore files = *m_files;
    const std::shared_ptr<std::atomic_bool> stopping = m_stopping;
    m_pool->start([self, files, stopping, stored] {
        const auto blob = assemble(files, stored.ref, stored.descriptor, *stopping);
        if (*stopping)
            return;
        // The blob itself never leaves this worker: only the verdict does.
        const bool valid = blob && attachmentBlobIsValid(stored.descriptor, *blob);
        backOnGui(self, [stored, valid](AttachmentTransfer &owner) {
            owner.m_validating.remove(stored.messageId.bytes());
            AttachmentRepository *attachments = owner.repository();
            if (attachments == nullptr)
                return;
            // It may have been cancelled meanwhile.
            const auto current = attachments->descriptorFor(stored.messageId);
            if (!current.hasValue() || !current.value()
                || current.value()->state != AttachmentState::Transferring)
                return;
            if (!valid) {
                qCWarning(mediaLog) << "A received attachment did not match its description";
                owner.failIncoming(stored, AttachmentFailure::Invalid);
                return;
            }
            const auto changed =
                attachments->setState(stored.messageId, AttachmentState::Complete, AttachmentFailure::None);
            if (changed.hasValue() && changed.value())
                owner.report(stored, AttachmentState::Complete, AttachmentFailure::None,
                             stored.descriptor.partCount);
        });
    });
}

// ---------------------------------------------------------------------------
// Media for the interface
// ---------------------------------------------------------------------------

void AttachmentTransfer::loadBlob(const MessageId &messageId, std::function<void(const QByteArray &)> done)
{
    const auto answerEmpty = [this, done] {
        QMetaObject::invokeMethod(this, [done] { done({}); }, Qt::QueuedConnection);
    };
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr) {
        answerEmpty();
        return;
    }
    const auto found = attachments->descriptorFor(messageId);
    if (!found.hasValue() || !found.value()) {
        answerEmpty();
        return;
    }
    const StoredAttachment stored = *found.value();
    // A received attachment only once it is complete and checked; one sent
    // from here was sealed whole before its message went.
    if (stored.flow == MessageFlow::Incoming && stored.state != AttachmentState::Complete) {
        answerEmpty();
        return;
    }
    const QPointer<AttachmentTransfer> self(this);
    const AttachmentFileStore files = *m_files;
    const std::shared_ptr<std::atomic_bool> stopping = m_stopping;
    m_pool->start([self, files, stopping, stored, done = std::move(done)] {
        auto blob = assemble(files, stored.ref, stored.descriptor, *stopping);
        if (blob && pageMediaHash(*blob) != stored.descriptor.sha256)
            blob.reset();
        backOnGui(self, [done, blob = blob.value_or(QByteArray())](AttachmentTransfer &) { done(blob); });
    });
}

QByteArray AttachmentTransfer::previewFor(const MessageId &messageId)
{
    AttachmentRepository *attachments = repository();
    if (attachments == nullptr)
        return {};
    const auto preview = attachments->preview(messageId);
    return preview.hasValue() ? preview.value() : QByteArray();
}

// ---------------------------------------------------------------------------
// Storage and recovery
// ---------------------------------------------------------------------------

void AttachmentTransfer::collectGarbage()
{
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    if (attachments == nullptr || !local)
        return;
    int collected = 0;
    // Frames whose message never came, and bytes staged here for a message
    // that was never queued.
    const auto orphans = attachments->orphans(now() - m_limits.orphanTtlMs);
    if (orphans.hasValue()) {
        for (const AttachmentTransferRecord &orphan : orphans.value()) {
            if (m_files->remove(orphan.ref).hasValue() && attachments->deleteTransfer(orphan.ref).hasValue())
                ++collected;
        }
    }
    // Received attachments that failed or were cancelled. What this device
    // sent is kept whatever became of it: it can be sent again.
    const auto abandoned = attachments->abandonedTransfers();
    if (abandoned.hasValue()) {
        for (const AttachmentRef &ref : abandoned.value()) {
            if (ref.senderDeviceId == *local)
                continue;
            if (m_files->remove(ref).hasValue() && attachments->deleteTransfer(ref).hasValue())
                ++collected;
        }
    }
    if (collected > 0)
        qCDebug(mediaLog) << "Collected" << collected << "attachment transfers";
}

void AttachmentTransfer::runRecovery()
{
    if (!m_engine.isLinkUp()) {
        m_linkUpSinceMs = 0;
        return;
    }
    const qint64 nowMs = now();
    if (m_linkUpSinceMs == 0)
        m_linkUpSinceMs = nowMs;
    AttachmentRepository *attachments = repository();
    const auto local = localDevice();
    if (attachments == nullptr || !local || m_engine.isFailedClosed())
        return;
    const auto incoming = attachments->incomingActive(*local);
    if (!incoming.hasValue())
        return;
    for (const StoredAttachment &stored : incoming.value()) {
        const AttachmentDescriptor &descriptor = stored.descriptor;
        const ConversationId &conversation = stored.ref.conversationId;
        if (m_validating.contains(stored.messageId.bytes()))
            continue;
        // Only a sender still in the conversation is asked.
        if (!m_roster || !m_roster(conversation).contains(stored.ref.senderDeviceId))
            continue;
        const auto found = attachments->transfer(stored.ref);
        if (!found.hasValue())
            continue;
        const std::optional<AttachmentTransferRecord> &transfer = found.value();
        const int asked = transfer ? transfer->requestsSent : 0;
        if (asked >= m_limits.maxRequests)
            continue;
        // No progress for a while, with the link up long enough for the
        // relay's backlog to have come in.
        qint64 lastProgress = std::max(m_arrivedAtMs.value(stored.messageId.bytes(), 0), m_linkUpSinceMs);
        if (transfer)
            lastProgress = std::max({lastProgress, transfer->firstSeenMs, transfer->updatedAtMs});
        if (nowMs - lastProgress < m_limits.requestGraceMs)
            continue;
        const qint64 backoff = std::min(m_limits.requestGraceMs << std::min(asked, maxBackoffDoublings),
                                        m_limits.requestMaxIntervalMs);
        if (transfer && transfer->lastRequestMs > 0 && nowMs - transfer->lastRequestMs < backoff)
            continue;
        QByteArray missing((descriptor.partCount + 7) / 8, '\0');
        for (int index = 0; index < descriptor.partCount; ++index) {
            if (!transfer || !transfer->hasPart(index))
                missing[index / 8] = char(quint8(missing.at(index / 8)) | (1 << (index % 8)));
        }
        const QByteArray frame = sealAttachmentFrame(descriptor.key, AttachmentFrameType::Request,
                                                     descriptor.attachmentId, 0, missing);
        if (!frame.isEmpty() && m_engine.sendAttachmentFrame(conversation, {stored.ref.senderDeviceId}, frame)) {
            ++m_framesSent;
            (void)attachments->recordRequest(stored.ref, nowMs);
        }
    }
}

} // namespace OpenChat
