#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>

namespace OpenChat::Relay {

// Coarse outcome codes shared across relay services. The HTTP layer maps these
// to status codes; they never carry content.
enum class RelayError {
    Ok,
    InvalidRequest,
    NotFound,
    Unauthorized,
    Conflict,
    Expired,
    Revoked,
    TokenReuse,
    RateLimited,
    Internal,
};

// Opaque token bundle. Tokens are random strings the client echoes back; the
// relay stores only their SHA-256 hashes.
struct AuthTokens final {
    QByteArray accessToken;
    QByteArray refreshToken;
    qint64 accessExpiresAtMs = 0;
    qint64 refreshExpiresAtMs = 0;
};

// Identity resolved from a valid access token.
struct AuthenticatedDevice final {
    AccountId accountId;
    DeviceId deviceId;
};

} // namespace OpenChat::Relay
