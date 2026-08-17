#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "network/BackoffPolicy.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QSslConfiguration>
#include <QString>
#include <QUrl>

#include <chrono>
#include <functional>
#include <memory>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class QWebSocket;
QT_END_NAMESPACE

namespace OpenChat {

// The single WebSocket subprotocol OpenChat speaks. The relay MUST echo exactly
// this token; a missing or different negotiated protocol is a hard failure.
inline constexpr char relaySubprotocol[] = "openchat.ciphertext.v1";

// Explicit endpoint configuration. Production requires https/wss for every URL;
// isSecure() gates that and the client refuses to operate insecurely.
struct RelayEndpoints final {
    QUrl accounts;      // POST { account_id, device_id, handle, signing_key, credential } (unauth)
    QUrl authChallenge; // POST { account_id, device_id } -> canonical CBOR { challenge }
    QUrl authComplete;  // POST { account_id, device_id, challenge, signature, context } -> tokens
    QUrl authRefresh;   // POST { refresh } -> rotated tokens
    QUrl sync;          // GET ?since=<watermark> -> bounded envelope batch
    QUrl keyPackages;   // POST { key_package } -> 200 (authenticated bearer token)
    QUrl directory;     // GET ?handle=<h> -> CBOR { account_id, devices } (authenticated)
    QUrl invites;       // POST { ttl_ms? } -> CBOR { token, expires_at_ms } (authenticated)
    QUrl invitesRedeem; // POST { token } -> CBOR { account_id, devices } (authenticated)
    QUrl live;          // wss:// live envelope stream

    [[nodiscard]] bool isSecure() const;
};

// Strict bounds applied to every inbound byte sequence before allocation/parse.
struct RelayLimits final {
    quint64 maxIncomingFrameBytes = maxEnvelopeBytes;   // per WebSocket frame
    quint64 maxIncomingMessageBytes = maxEnvelopeBytes; // reassembled message
    qint64 maxHttpBodyBytes = 8 * 1024 * 1024;          // catch-up batch ceiling
    qint64 readBufferBytes = maxEnvelopeBytes;          // per-reply in-flight cap
    int maxCatchUpEnvelopes = 512;                      // items per catch-up call
    int maxRedirects = 0;                               // relay never redirects
    std::chrono::milliseconds transferTimeout{std::chrono::seconds{20}};
    std::chrono::milliseconds connectTimeout{std::chrono::seconds{20}};
};

// Token material is supplied on demand through callbacks and never retained by
// the RelayClient beyond the lifetime of a single request/handshake. The caller
// owns durable, wiped storage; RelayClient only borrows a copy to set a header.
struct RelayCredentials final {
    std::function<QByteArray()> accessToken;  // current short-lived access token
    std::function<QByteArray()> refreshToken; // current rotating refresh token
};

// Produces an Ed25519 signature over the server challenge (and bound context).
// Owned by the caller so no device private key ever enters RelayClient.
using ChallengeSigner =
    std::function<QByteArray(QByteArrayView challenge, QByteArrayView context)>;

enum class RelayCallError {
    NotConnected,
    NotConfigured,
    InsecureEndpoint,
    EncodeFailure,
    Busy,
};

// Typed transport failures surfaced through transportError(). Every value is
// terminal for the current connection; the reconnect policy decides retries.
enum class RelayTransportError {
    Tls,               // sslErrors / handshake verification failure
    ProtocolMismatch,  // negotiated subprotocol missing or wrong
    TextFrameRejected, // a text frame arrived; binary-only protocol
    FrameTooLarge,     // frame/message exceeded the configured bound
    MalformedEnvelope, // canonical CBOR decode rejected the frame
    WrongRecipient,    // envelope not addressed to this device
    InvalidControlFrame,
    ConnectTimeout,
    Network,           // generic socket error
    HttpStatus,        // non-2xx HTTP response
    BodyTooLarge,      // HTTP body exceeded the configured bound
    AuthRejected,      // credentials rejected and refresh exhausted
    InsecureEndpoint,  // a configured endpoint was not https/wss
};

// Outcome of an account-registration attempt, reported through
// accountRegistrationFailed(). HandleUnavailable maps the relay's 409 Conflict
// (the handle or account is already taken); InvalidRequest maps a 400; Transport
// covers TLS/network failures and any other non-2xx status.
enum class RelayRegistrationError {
    HandleUnavailable,
    InvalidRequest,
    Transport,
};

// One device returned by a directory lookup: its stable device id plus the
// Ed25519 signing public key other members verify against. Only ever populated
// from a response that passed defensive validation (fixed field sizes, bounded
// device count).
struct RelayDirectoryDevice final {
    DeviceId deviceId;
    QByteArray signingKey;
};

// A resolved account and its active devices, produced by resolveHandle() and
// redeemInvite(). Like the envelope payloads it is intentionally not
// default-constructible (AccountId has no default), so it travels through its
// signals by const reference and is observed from a slot rather than QSignalSpy.
struct RelayDirectoryEntry final {
    AccountId accountId;
    QList<RelayDirectoryDevice> devices;
};

// Typed discovery failures surfaced through handleResolutionFailed() and
// inviteRedemptionFailed(). NotFound maps the relay's 404 (an unknown handle, or
// an invalid/expired/consumed invite); Malformed is a response that decoded but
// violated the directory shape or its defensive bounds; Transport covers
// TLS/network failures, an oversize body, and any other non-2xx status.
enum class RelayDirectoryError {
    NotFound,
    Malformed,
    Transport,
};

// Result of a device authentication exchange (Task 8 obtains it; later phases
// persist the tokens through their own wiped storage).
struct RelaySession final {
    QByteArray accessToken;
    QByteArray refreshToken;
    qint64 accessExpiresAtMs = 0;
};

// Authenticated HTTPS/WSS relay transport.
//
// Security posture (verified in tests):
//  - TLS uses VerifyPeer + SecureProtocols starting from the system default
//    configuration, so system trust roots are kept. sslErrors is terminal and
//    ignoreSslErrors() is never called.
//  - The live WebSocket negotiates exactly relaySubprotocol; any mismatch or a
//    missing negotiated protocol closes the connection without delivering data.
//  - Incoming frames/messages, HTTP bodies, read buffers, redirects, and
//    timeouts are all bounded before allocation or parsing.
//  - Only bounded canonical-CBOR envelopes addressed to this device are ever
//    surfaced through envelopeReceived; text frames, oversized frames, malformed
//    CBOR, wrong recipients, and invalid control frames are dropped as terminal
//    transport errors and never invoke the envelope callback.
//  - Authorization travels only in request/handshake headers and is fetched per
//    use from the credential callbacks. setTokens() may install the current
//    session tokens as that callback source; when it does they are held only in
//    memory, never logged, and never written to disk.
//  - At most one serialized token refresh is attempted per auth-failure cycle.
//  - No message plaintext or private key ever enters this object.
class RelayClient final : public QObject
{
    Q_OBJECT

public:
    RelayClient(DeviceId localDeviceId, AccountId localAccountId, RelayEndpoints endpoints,
                RelayCredentials credentials, RelayLimits limits = {},
                BackoffPolicy backoff = BackoffPolicy{}, QObject *parent = nullptr);
    ~RelayClient() override;

    RelayClient(const RelayClient &) = delete;
    RelayClient &operator=(const RelayClient &) = delete;

    // Injects an alternate TLS configuration. Intended for tests that add a
    // private development CA on top of the system roots. The configuration must
    // keep VerifyPeer; the client re-asserts VerifyPeer + SecureProtocols
    // regardless so a test cannot accidentally weaken verification.
    void setTlsConfiguration(const QSslConfiguration &configuration);

    // Installs the session tokens obtained from authenticated()/tokensRotated()
    // as the source the credential callbacks read from, so a subsequent
    // authenticated request (publishKeyPackage, fetchSince, connectLive) carries
    // the fresh access token and the refresh path has a refresh token to present.
    // The tokens are held only in memory for the life of the session, are never
    // logged or written to disk, and only ever leave through a request/handshake
    // Authorization header. Overwrites any previously installed tokens.
    void setTokens(const QByteArray &accessToken, const QByteArray &refreshToken);

    // Performs the two-step device challenge/response over HTTPS and, on
    // success, emits authenticated() with a fresh RelaySession.
    void authenticateDevice(const QByteArray &deviceCredential,
                            const ChallengeSigner &signer,
                            const QByteArray &context = {});

    // Opens the live WSS stream and requests resumption from the durable
    // watermark. Reconnect is automatic with full-jitter backoff until
    // disconnect() is called.
    void connectLive(quint64 resumeWatermark);

    // Encodes and sends one ciphertext envelope as a binary frame. Returns an
    // error synchronously when not connected or when encoding fails; relay
    // acceptance arrives asynchronously through relayAccepted().
    [[nodiscard]] Result<void, RelayCallError> sendEnvelope(const CiphertextEnvelopeV1 &envelope);

    // Sends a plaintext-free acknowledgement control frame (envelope id +
    // advanced watermark) over the live stream.
    [[nodiscard]] Result<void, RelayCallError> acknowledge(const EnvelopeId &envelopeId,
                                                           quint64 watermark);

    // Bounded HTTPS catch-up from the watermark. Delivers each decoded envelope
    // through envelopeReceived and finishes with catchUpComplete().
    void fetchSince(quint64 watermark);

    // Registers a new account+device over HTTPS. This is the unauthenticated
    // bootstrap call, so it carries no bearer token even when one is available.
    // Emits accountRegistered() on a 2xx; a taken handle (relay 409) emits
    // accountRegistrationFailed(HandleUnavailable), a 400 InvalidRequest, and any
    // other failure Transport.
    void registerAccount(const AccountId &account, const DeviceId &device, const QString &handle,
                         const QByteArray &signingKey, const QByteArray &credential);

    // Publishes one MLS KeyPackage for this device to the authenticated HTTPS
    // endpoint (bearer access token attached). Emits keyPackagePublished() on a
    // 2xx; a rejected token drives a single serialized refresh-and-retry and then
    // authExpired(); any other failure emits keyPackagePublishFailed().
    void publishKeyPackage(const QByteArray &keyPackage);

    // Resolves a handle to an account and its active devices over the
    // authenticated HTTPS directory endpoint (bearer access token attached). The
    // handle is URL-encoded into a `handle` query item. Emits handleResolved()
    // with a defensively validated entry on a 2xx; a rejected token drives a
    // single serialized refresh-and-retry and then authExpired(); an unknown
    // handle (relay 404) emits handleResolutionFailed(NotFound); a response that
    // violates the directory shape or its bounds emits Malformed; any other
    // failure emits Transport.
    void resolveHandle(const QString &handle);

    // Mints a one-time invite over the authenticated HTTPS endpoint. When
    // ttlMs > 0 it is sent as ttl_ms; otherwise the body is empty and the relay
    // applies its default TTL. Emits inviteCreated(token, expiresAtMs) on a 2xx;
    // a rejected token drives the single refresh-and-retry then authExpired();
    // any other failure emits inviteCreationFailed().
    void createInvite(qint64 ttlMs = 0);

    // Redeems a one-time invite over the authenticated HTTPS endpoint and, on a
    // 2xx, emits inviteRedeemed() with the inviter's defensively validated entry.
    // A rejected token drives the single refresh-and-retry then authExpired(); an
    // invalid/expired/consumed token (relay 404) emits
    // inviteRedemptionFailed(NotFound); a malformed body emits Malformed; any
    // other failure Transport.
    void redeemInvite(const QByteArray &token);

    // Closes the live stream and cancels any pending reconnect. Idempotent.
    void disconnect();

    [[nodiscard]] bool isConnected() const noexcept;

signals:
    void authenticated(const RelaySession &session);
    void connected();
    void disconnected();
    // Delivered ciphertext plus its per-recipient relay sequence, which the
    // durable receive path uses as the inbox watermark.
    void envelopeReceived(const CiphertextEnvelopeV1 &envelope, quint64 serverSequence);
    void relayAccepted(const EnvelopeId &envelopeId, quint64 serverSequence);
    // Emitted after a successful refresh so the caller can persist rotated
    // tokens; RelayClient itself does not store them.
    void tokensRotated(const RelaySession &session);
    void authExpired();
    void transportError(RelayTransportError error);
    void catchUpComplete(quint64 newWatermark);
    // Account bootstrap: registration succeeded / failed with a typed reason.
    void accountRegistered();
    void accountRegistrationFailed(RelayRegistrationError error);
    // KeyPackage publish succeeded / failed (non-auth failure; a rejected token
    // surfaces through authExpired() after the single refresh-and-retry).
    void keyPackagePublished();
    void keyPackagePublishFailed();
    // Directory lookup: a defensively validated entry, or a typed failure.
    void handleResolved(const RelayDirectoryEntry &entry);
    void handleResolutionFailed(RelayDirectoryError error);
    // One-time invite creation: the plaintext token plus advisory expiry, or a
    // non-auth failure (a rejected token surfaces through authExpired()).
    void inviteCreated(const QByteArray &token, qint64 expiresAtMs);
    void inviteCreationFailed();
    // One-time invite redemption: the inviter's entry, or a typed failure.
    void inviteRedeemed(const RelayDirectoryEntry &entry);
    void inviteRedemptionFailed(RelayDirectoryError error);

private:
    void completeAuthentication(const QByteArray &challenge, const ChallengeSigner &signer,
                                const QByteArray &context);
    void refreshThenRetryFetch(quint64 watermark);
    void refreshThenRetryPublish(const QByteArray &keyPackage);
    // Runs one serialized token refresh and, on success, invokes `retry` exactly
    // once with the rotated credentials in place. Used by the authenticated
    // discovery calls; the fetch/publish paths keep their own typed helpers.
    void refreshThenRetry(std::function<void()> retry);
    void deliverCatchUp(const QByteArray &body);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
