#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>

#include <cstdint>
#include <optional>

namespace OpenChat {

// The wire shape of one 20 ms media frame.
//
//   0       version (1)
//   1       flags
//   2..17   call id
//   18..21  sequence, big-endian
//   22..    AES-256-GCM ciphertext || 16-byte tag
//
// The 22-byte header is the AEAD's associated data, so the call id, sequence and
// flags are all authenticated even though they travel in the clear: a relay can
// see which call a packet belongs to (it has to, to route it) but cannot move a
// packet to another call, renumber it, or flip its flags without the tag failing.
struct CallMediaPacket final {
    static constexpr quint8 currentVersion = 1;
    static constexpr qsizetype headerBytes = 22;
    // A 20 ms PCM frame plus header and tag is the largest packet this pipeline
    // ever produces; anything bigger is refused before it is decrypted.
    static constexpr qsizetype maxBytes = headerBytes + 1920 + 16;

    // Set when the sender is muted. The frame is still sent (and still carries
    // real silence), so the far end keeps a steady clock and an accurate view of
    // whether the peer is talking rather than inferring it from a gap.
    static constexpr quint8 flagMuted = 0x01;

    quint8 version = currentVersion;
    quint8 flags = 0;
    CallId callId = CallId::generate();
    quint32 sequence = 0;
    QByteArray sealed;

    [[nodiscard]] bool isMuted() const noexcept { return (flags & flagMuted) != 0; }

    // The authenticated header bytes, which are also what seal/open take as
    // associated data.
    [[nodiscard]] QByteArray header() const;
    [[nodiscard]] QByteArray encode() const;

    // Rejects a short, over-long, or wrong-version packet outright. A packet
    // that decodes here has NOT been authenticated yet — that is the opener's job.
    [[nodiscard]] static std::optional<CallMediaPacket> decode(QByteArrayView bytes);
};

} // namespace OpenChat
