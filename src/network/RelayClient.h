#pragma once

#include "core/Result.h"
#include "domain/Identifiers.h"
#include "network/BackoffPolicy.h"
#include "protocol/CanonicalCborCodec.h"
#include "protocol/CiphertextEnvelope.h"

#include <QByteArray>
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
    QUrl authChallenge; // POST -> canonical CBOR { challenge }
    QUrl authComplete;  // POST { credential, challenge, signature } -> tokens
    QUrl authRefresh;   // POST { refresh } -> rotated tokens
    QUrl sync;          // GET ?since=<watermark> -> bounded envelope batch
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
//  - Authorization travels only in request/handshake headers, is fetched per
//    use from the credential callbacks, and is never stored as a member.
//  - At most one serialized token refresh is attempted per auth-failure cycle.
//  - No message plaintext or private key ever enters this object.
class RelayClient final : public QObject
{
    Q_OBJECT

public:
    RelayClient(DeviceId localDeviceId, RelayEndpoints endpoints,
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

    // Closes the live stream and cancels any pending reconnect. Idempotent.
    void disconnect();

    [[nodiscard]] bool isConnected() const noexcept;

signals:
    void authenticated(const RelaySession &session);
    void connected();
    void disconnected();
    void envelopeReceived(const CiphertextEnvelopeV1 &envelope);
    void relayAccepted(const EnvelopeId &envelopeId, quint64 serverSequence);
    // Emitted after a successful refresh so the caller can persist rotated
    // tokens; RelayClient itself does not store them.
    void tokensRotated(const RelaySession &session);
    void authExpired();
    void transportError(RelayTransportError error);
    void catchUpComplete(quint64 newWatermark);

private:
    void completeAuthentication(const QByteArray &deviceCredential, const QByteArray &challenge,
                                const ChallengeSigner &signer, const QByteArray &context);
    void refreshThenRetryFetch(quint64 watermark);
    void deliverCatchUp(const QByteArray &body);

    class Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat
