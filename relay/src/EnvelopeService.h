#pragma once

#include "PostgresStore.h"
#include "RelayTypes.h"
#include "core/Result.h"
#include "protocol/CiphertextEnvelope.h"
#include "domain/Identifiers.h"

#include <QByteArray>
#include <QList>

namespace OpenChat::Relay {

struct SubmitResult final {
    quint64 serverSequence = 0;
    qint64 acceptedAtMs = 0;
    bool duplicate = false;
};

struct InboxItem final {
    quint64 serverSequence = 0;
    QByteArray envelope;
};

struct FetchResult final {
    QList<InboxItem> items;
    quint64 newWatermark = 0;
};

// Accepts opaque ciphertext envelopes and routes them into per-device inboxes.
// The relay validates structure, size, hash, and the sender's signature WITHOUT
// decrypting MLS content, and never stores anything but the opaque envelope.
class EnvelopeService final
{
public:
    struct Policy final {
        int maxFetch = 512;
        // Cap the cumulative encoded size of one catch-up page. A single
        // envelope is bounded to 1 MiB by the schema, so this keeps a page well
        // under the client's HTTP body limit no matter how large maxFetch is,
        // preventing a ~maxFetch × 1 MiB response allocation.
        qint64 maxResponseBytes = 4 * 1024 * 1024;
    };

    explicit EnvelopeService(PostgresStore &store);
    EnvelopeService(PostgresStore &store, Policy policy);

    // Validates and idempotently stores an envelope for its recipient device.
    // authenticatedDevice is the device resolved from the bearer token; the
    // envelope's sender must equal it.
    [[nodiscard]] Result<SubmitResult, RelayError>
    submit(const AuthenticatedDevice &authenticatedDevice, QByteArrayView envelopeBytes);

    // Every check submit() makes before it writes anything: canonical decode,
    // sender identity, expiry, the sender's Ed25519 signature over the canonical
    // bytes, and that both devices exist and are unrevoked. Stores nothing.
    //
    // The datagram path uses this on its own: an unstored frame still has to be
    // as thoroughly authenticated as a stored one, because the relay is
    // forwarding it to a third party either way.
    [[nodiscard]] Result<CiphertextEnvelopeV1, RelayError>
    validate(const AuthenticatedDevice &authenticatedDevice, QByteArrayView envelopeBytes);

    // Bounded catch-up: envelopes for the device with server_sequence > since,
    // in order, capped at limit (clamped to policy.maxFetch).
    [[nodiscard]] Result<FetchResult, RelayError>
    fetchSince(const DeviceId &deviceId, quint64 since, int limit);

    // Monotonically advances the device's acknowledgement watermark.
    [[nodiscard]] Result<void, RelayError> acknowledge(const DeviceId &deviceId, quint64 watermark);

    [[nodiscard]] quint64 watermarkFor(const DeviceId &deviceId);

private:
    PostgresStore &m_store;
    Policy m_policy;
};

} // namespace OpenChat::Relay
