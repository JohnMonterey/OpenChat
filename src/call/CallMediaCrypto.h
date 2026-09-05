#pragma once

#include "call/CallTypes.h"
#include "core/Result.h"
#include "domain/Identifiers.h"

#include <QByteArray>

#include <cstdint>
#include <optional>

namespace OpenChat {

// Per-direction media keying for one call.
//
// The MLS group already protects the call's signalling, but running 50 media
// frames a second through the group ratchet would be both slow and wasteful, so
// media gets its own key schedule: the caller draws one 32-byte secret, ships it
// inside the MLS-encrypted offer, and both ends expand it with HKDF-SHA256 into
// a separate key and nonce salt per direction. Two directions therefore never
// share a (key, nonce) pair — the failure that breaks AES-GCM outright — and the
// keys live only as long as the call.
struct CallMediaKeys final {
    QByteArray key;  // 32 bytes, AES-256-GCM
    QByteArray salt; // 12 bytes, XORed with the frame sequence to form the nonce

    [[nodiscard]] bool isValid() const noexcept;
};

// Both directions' material, derived together so a caller cannot accidentally
// key its sender and receiver from the same label.
struct CallMediaKeySchedule final {
    CallMediaKeys fromCaller; // the offering side's send key
    CallMediaKeys fromCallee; // the answering side's send key

    // Expands `secret` (exactly callSecretBytes) bound to `callId`, so the same
    // secret replayed under a different call id yields unrelated keys. Returns
    // nullopt if the secret is the wrong length or the KDF fails.
    [[nodiscard]] static std::optional<CallMediaKeySchedule> derive(QByteArrayView secret,
                                                                    const CallId &callId);
    [[nodiscard]] static std::optional<CallMediaKeySchedule> deriveVideo(QByteArrayView secret,
                                                                       const CallId &callId);

    // The pair this end uses, given which side of the call it is on.
    [[nodiscard]] const CallMediaKeys &sendKeys(CallDirection direction) const noexcept;
    [[nodiscard]] const CallMediaKeys &receiveKeys(CallDirection direction) const noexcept;
};

// The secret one PAIR of group-call members key their media from. A group
// call has one call secret but many media paths, and two paths must never
// share a (key, nonce) pair; expanding the call secret with both device ids
// (ordered, so both ends derive the same value) gives every pair its own
// secret, from which the usual per-direction schedule is then derived. Empty
// if the secret is the wrong length or the KDF fails.
[[nodiscard]] QByteArray deriveGroupPairSecret(QByteArrayView callSecret, const CallId &callId,
                                               const DeviceId &first, const DeviceId &second);

// AES-256-GCM sealing of one media frame, with the frame's sequence number as
// both the nonce input and additional authenticated data.
class CallMediaSealer final
{
public:
    static constexpr int tagBytes = 16;
    static constexpr int nonceBytes = 12;

    explicit CallMediaSealer(CallMediaKeys keys);

    // Returns ciphertext||tag, or empty on failure (including an unkeyed sealer).
    [[nodiscard]] QByteArray seal(quint32 sequence, QByteArrayView plaintext,
                                  QByteArrayView associatedData) const;

private:
    CallMediaKeys m_keys;
};

// The opening half, with the replay protection a sealer does not need.
//
// A media path is deliberately unreliable, so the opener must accept frames out
// of order — but "out of order" and "replayed" look identical without state.
// A 64-slot sliding window over the highest sequence accepted so far
// distinguishes them: anything inside the window is accepted once and only once,
// anything below it is refused outright.
class CallMediaOpener final
{
public:
    static constexpr int windowSize = 64;

    explicit CallMediaOpener(CallMediaKeys keys);

    // Returns the plaintext, or nullopt when the frame fails authentication, is
    // a replay, or has fallen out of the replay window. A rejected frame leaves
    // the window untouched, so a forged sequence cannot advance it.
    [[nodiscard]] std::optional<QByteArray> open(quint32 sequence, QByteArrayView sealed,
                                                 QByteArrayView associatedData);

    [[nodiscard]] quint64 acceptedCount() const noexcept { return m_accepted; }
    [[nodiscard]] quint64 rejectedCount() const noexcept { return m_rejected; }
    [[nodiscard]] quint64 replayCount() const noexcept { return m_replays; }

private:
    // True if `sequence` is new; does not mutate. Split from the commit so a
    // frame that fails authentication never touches the window.
    [[nodiscard]] bool isFresh(quint32 sequence) const noexcept;
    void commit(quint32 sequence) noexcept;

    CallMediaKeys m_keys;
    quint32 m_highest = 0;
    quint64 m_window = 0; // bit i set == (m_highest - i) already accepted
    bool m_seen = false;
    quint64 m_accepted = 0;
    quint64 m_rejected = 0;
    quint64 m_replays = 0;
};

} // namespace OpenChat
