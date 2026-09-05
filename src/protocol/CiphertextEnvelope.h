#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QMap>

namespace OpenChat {

enum class EnvelopeMessageKind : quint8 {
    MlsPrivateMessage = 0,
    MlsHandshake = 1,
    Receipt = 2,
    AttachmentControl = 3,
    // A contact-request acceptance: an MLS application message (encrypted
    // under the freshly joined group's ratchet) whose delivery proves to the
    // requester that the peer joined the 2-party group. Carries no user
    // content; the engine consumes it as a control receive and surfaces it as
    // contactAcceptReceived rather than as a visible message.
    ContactAccept = 4,
    // Voice-call control: an MLS application message carrying an offer, ringing
    // notice, answer or hangup. Reliable and ratcheted like any other control
    // message, and consumed by the call engine rather than shown as a row.
    CallSignal = 5,
    // One 20 ms slice of call audio, sealed under the call's own media key
    // rather than the group ratchet. Unlike every other kind this one travels
    // the datagram path: it is never stored, never retried, and dropped when the
    // recipient is offline, because a late voice frame is worse than none.
    CallMedia = 6,
    // A profile update: the sender's chosen presence, status text and (JPEG)
    // profile picture, as an MLS application message under the conversation's
    // ratchet. Durable and retried like a call signal, consumed as a control
    // receive, and never a visible row. The relay sees only ciphertext, so a
    // picture or status never leaves the two devices in the clear.
    ProfileUpdate = 7,
    // A group chat invitation: the ciphertext IS an MLS Welcome for a group
    // with more than two members, shipped verbatim (it is already HPKE-sealed
    // to the recipient's KeyPackage). Unlike MlsHandshake it is never stashed
    // as a contact request: a device joins it straight away, but only when the
    // sender is an already-accepted contact and the Welcome's membership names
    // the sender's device.
    GroupWelcome = 8,
    // Group chat control: an MLS application message carrying an encoded
    // GroupUpdateMessage (the roster and title, a rename, or a member leaving).
    // Durable and retried, consumed as a control receive, never a visible row.
    GroupControl = 9,
    // A raw MLS Commit (a member added or removed) for the members already in
    // the group. Processed by the ratchet to advance the epoch; carries no
    // application plaintext and is never a visible row.
    MlsCommit = 10,
};

// The highest EnvelopeMessageKind the codec accepts on decode. Kept next to the
// enum so a new kind cannot be added without widening the wire bound.
inline constexpr quint8 maxEnvelopeMessageKind =
    static_cast<quint8>(EnvelopeMessageKind::MlsCommit);

struct CiphertextEnvelopeV1 final {
    quint8 version = 1;
    EnvelopeId envelopeId;
    AccountId senderAccountId;
    DeviceId senderDeviceId;
    DeviceId recipientDeviceId;
    ConversationId conversationId;
    EnvelopeMessageKind messageKind = EnvelopeMessageKind::MlsPrivateMessage;
    qint64 createdAtMs = 0;
    qint64 expiresAtMs = 0;
    EnvelopeId idempotencyKey;
    QByteArray ciphertext;
    QByteArray ciphertextSha256;
    QByteArray senderSignature;
    QMap<quint64, QByteArray> noncriticalExtensions;

    friend bool operator==(const CiphertextEnvelopeV1 &,
                           const CiphertextEnvelopeV1 &) = default;
};

} // namespace OpenChat
