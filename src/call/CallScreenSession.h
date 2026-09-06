#pragma once

#include "call/CallMediaCrypto.h"
#include "call/ScreenCanvas.h"

#include <QByteArray>
#include <QImage>
#include <QLoggingCategory>
#include <QRect>
#include <QSize>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace OpenChat {

// Off unless the usual QT_LOGGING_RULES turn it on, so a shipped build says
// nothing about a running share.
Q_DECLARE_LOGGING_CATEGORY(lcScreenShare)

// A borrowed, read-only view of one captured desktop frame.
//
// The capture backend hands out frames whose pixels live in the compositor's or
// the decoder's own buffer. Turning one into a QImage would allocate and copy a
// whole desktop every frame; instead the encoder reads the tiles it actually
// needs straight out of the mapped buffer, and this is the handle it does that
// through. It owns nothing and must not outlive the mapping it describes.
struct ScreenFrameView final {
    const uchar *bits = nullptr;
    qsizetype bytesPerLine = 0;
    int width = 0;
    int height = 0;
    // Any 32-bit format. Pixels are compared as opaque words and re-encoded
    // through QImage, so the exact channel order never has to be interpreted.
    // Both the pointer and the stride must be four-byte aligned, which is what
    // lets the whole encoder read rows as words; a capture that is not is
    // refused here rather than read wrongly further down.
    QImage::Format format = QImage::Format_Invalid;

    [[nodiscard]] bool isValid() const noexcept;
    // A view over an existing image. The image must outlive the view.
    [[nodiscard]] static ScreenFrameView fromImage(const QImage &image);
};

// The knobs the whole subsystem is sized by. Defaults are the desktop case:
// 1080p at 30 fps with text that stays readable.
struct ScreenShareTuning final {
    // The longest edge ever encoded, before adaptation. A 4K desktop goes out
    // as 1080p because that is what the far end's window can show; sending
    // 3840x2160 would quadruple every cost for pixels nobody resolves.
    int maxOutputEdge = 1920;
    // Encoded tile edge, in output pixels. Smaller tiles track small changes
    // more tightly but cost 5 bytes of header each and lose compression
    // context; 128 is the balance for UI content at 1080p.
    int tileEdge = 128;
    // Hard ceiling on one datagram.
    //
    // Sized by latency, not by the relay's 1 MiB frame limit. A screen update
    // shares one ordered connection with the call's audio, so a big update is
    // time during which no 20 ms voice frame can be sent behind it. 64 KiB is
    // about 100 ms on a modest uplink and a few milliseconds on a good one;
    // a megabyte would be half a second of silence for a change to a window.
    // Nothing is lost by the cap — a change too large for one update simply
    // continues in the next, which is how the budget works anyway.
    int maxPacketBytes = 64 * 1024;
    // How long a full rolling intra-refresh sweep takes, or 0 for none.
    //
    // Off by default, and deliberately so. Media rides one ordered connection
    // per peer, so a frame is not quietly reordered or thinned — it is either
    // delivered or the link dropped, and the receiver can see exactly that in a
    // gap in the sequence numbers. It asks for a resend when it sees one, which
    // costs nothing while the link is healthy. A blind sweep would instead
    // re-encode a motionless desktop forever: the single most expensive thing
    // this subsystem could possibly do, for content nobody changed.
    int refreshPeriodMs = 0;
    // A share whose desktop is not moving still says so this often, in an
    // eight-byte update carrying no tiles. That is what keeps the far end's
    // staleness timer quiet and its reports — and with them the round-trip
    // time and the viewer's window size — still flowing.
    int heartbeatMs = 1000;
    // How often the receiving end reports back.
    int feedbackIntervalMs = 500;
    // Frame rate while the far end says it is not displaying the share at all.
    // Not zero: the share must come back instantly when the window reopens.
    int idleFps = 2;
};

// One rung of the quality ladder. Adaptation walks down it on loss and back up
// on sustained clean reports, in the order the brief asks for: bitrate first,
// then frame rate, then resolution.
struct ScreenShareLevel final {
    int jpegQuality;
    int targetFps;
    // Source pixels per encoded pixel, on top of the maxOutputEdge cap.
    int divisor;
    int bytesPerSecond;
};

// The ladder, exposed so the tests can assert the ordering the brief demands:
// bitrate falls before frame rate, and frame rate before resolution.
[[nodiscard]] const std::vector<ScreenShareLevel> &screenShareLevels();

// What a development build wants to see about a running share. Cheap to keep
// (a handful of counters updated per frame) and never logged unless asked for.
struct ScreenShareStats final {
    // Sending half, from the encoder.
    QSize outputSize;
    int level = 0;
    int targetFps = 0;
    int jpegQuality = 0;
    bool motionMode = false;
    quint64 framesSent = 0;
    quint64 framesSkipped = 0; // paced out: capture offered a frame too early
    quint64 framesIdle = 0;    // nothing had changed, so nothing was sent
    quint64 bytesSent = 0;
    quint64 tilesSent = 0;
    int lastPayloadBytes = 0;
    int lastTileCount = 0;
    int lastEncodeUs = 0;
    int dirtyTiles = 0;
    int totalTiles = 0;

    // Link and receiving half, from one peer's session.
    double lossRatio = 0.0;
    int rttMs = -1; // -1 until a report closes the loop
    QSize remoteViewSize;
    quint64 framesReceived = 0;
    quint64 tilesApplied = 0;
    quint64 bytesReceived = 0;
    quint64 framesRejected = 0;
};

// The picture half of screen sharing: everything about turning a desktop into
// bytes, and nothing about who those bytes are for.
//
// Screen content is not camera content: it is mostly static, it is full of flat
// colour and sharp text, and re-encoding an entire 1080p desktop 30 times a
// second to send back the same pixels would be indefensible on a link, a CPU
// and a battery. So the frame is cut into tiles, each tile is hashed, and only
// the tiles that actually changed are encoded — as PNG when the tile is flat UI
// or text (lossless, so glyphs stay crisp), as JPEG when it is photographic,
// and as three bytes when it is one solid colour. A rolling intra-refresh
// resends every tile on a slow sweep so a lost datagram heals on its own.
//
// It is a separate object from the per-peer session for the group case: a mesh
// call has one screen and several peers, so the desktop is hashed once and the
// tiles encoded once, and only the cheap AES seal is repeated per peer.
class ScreenTileEncoder final
{
public:
    static constexpr int updateHeaderBytes = 8;
    static constexpr int tileHeaderBytes = 5;
    static constexpr int maxCanvasEdge = 8192;
    static constexpr int minTileShift = 5; // 32 px
    static constexpr int maxTileShift = 9; // 512 px
    static constexpr int maxTileBytes = 512 * 1024;

    enum class TileEncoding : quint8 {
        Solid = 0, // three bytes of RGB
        Png = 1,   // flat UI and text: lossless, so glyphs stay sharp
        Jpeg = 2,  // photographic content, where lossless would be ruinous
    };

    explicit ScreenTileEncoder(ScreenShareTuning tuning = {});

    // The next update, or an empty view when the frame was paced out, when
    // nothing changed, or on failure. The returned bytes belong to the encoder
    // and stay valid until the next call — they are sealed, not stored.
    [[nodiscard]] QByteArrayView buildUpdate(const ScreenFrameView &frame, qint64 nowMs);

    // Every tile goes again. Used when a peer says it lost its canvas.
    void requestFullResend();
    // The rung to encode at. In a mesh this is the worst peer's rung, because
    // one payload serves all of them.
    void setLevel(int level);
    [[nodiscard]] int level() const noexcept { return m_level; }
    // The largest the share is being displayed anywhere, and whether anyone is
    // displaying it at all. Caps the resolution worth encoding.
    void setRemoteView(QSize largestView, bool anyoneWatching);
    [[nodiscard]] int targetFps() const noexcept;
    [[nodiscard]] bool remoteViewHidden() const noexcept { return !m_anyoneWatching; }

    // Drops every buffer and forgets the geometry. Called the moment sharing
    // stops, so nothing desktop-sized is held for a share that is over.
    void reset();

    [[nodiscard]] const ScreenShareStats &stats() const noexcept { return m_stats; }

private:
    void reconfigure(int sourceWidth, int sourceHeight, qint64 nowMs);
    int markDirtyTiles(const ScreenFrameView &frame);
    [[nodiscard]] QImage stageTile(const ScreenFrameView &frame, int column, int row);
    [[nodiscard]] bool encodeTile(const QImage &tile, QByteArray &out, TileEncoding &encoding);

    ScreenShareTuning m_tuning;
    QSize m_sourceSize;
    int m_divisor = 1;
    int m_tileEdge = 128;
    int m_sourceTileEdge = 128;
    int m_columns = 0;
    int m_rows = 0;
    quint8 m_generation = 0;
    std::vector<quint64> m_tileHash;
    std::vector<quint8> m_tileDirty;
    int m_dirtyCount = 0;
    int m_sendCursor = 0;    // rotates so no region starves under a tight budget
    int m_refreshCursor = 0; // the rolling intra-refresh sweep
    int m_samplePhase = 0;   // rotates the hash sampling grid when downscaling
    qint64 m_lastFrameMs = 0;
    qint64 m_lastSentMs = 0;
    qint64 m_lastRefreshMs = 0;
    qint64 m_lastGeometryMs = 0;
    QImage m_tileScratch;
    QByteArray m_tileBuffer;
    QByteArray m_payload;
    int m_level = 2; // start conservative; clean reports earn the way up
    bool m_motionMode = false;
    qint64 m_motionSinceMs = 0;
    QSize m_remoteView;
    bool m_anyoneWatching = true;
    ScreenShareStats m_stats;
};

// One end of the screen-share path to ONE peer, in both directions.
//
// It owns the keys, the sequence numbers, the reconstruction of what arrives,
// and the congestion loop: the receiving end reports back along the same path —
// how much arrived, how big its window is — and that report is what moves this
// peer's rung on the quality ladder.
//
// Wire version 3, so the audio (1) and camera (2) parsers on either side are
// untouched and an older client simply ignores the packets.
class CallScreenSession final
{
public:
    static constexpr quint8 wireVersion = 3;
    // Payload present. Absent means "the share stopped".
    static constexpr quint8 flagContent = 0x01;
    // A receiver-to-sender report rather than picture data. Sealed under its
    // own key schedule, so it can never collide with the peer's own share.
    static constexpr quint8 flagFeedback = 0x02;
    static constexpr int feedbackBytes = 20;

    // What arrived. `canvas` is the surface the share now lives in; it changes
    // identity only when the sender's geometry changes.
    struct Update final {
        enum class Kind { Frame, Stopped, Feedback };
        Kind kind = Kind::Frame;
        ScreenCanvasPtr canvas;
        QRect dirty;
        bool canvasReplaced = false;
    };

    // `encoder` is shared with every other peer in the same call, so one
    // desktop is hashed and encoded once however many people are watching.
    [[nodiscard]] static std::unique_ptr<CallScreenSession>
    create(const CallId &id, CallDirection direction, QByteArrayView secret,
           std::shared_ptr<ScreenTileEncoder> encoder, ScreenShareTuning tuning = {});

    // --- Sending -----------------------------------------------------------

    // Seals an update the shared encoder has already built. Empty in, empty out.
    [[nodiscard]] QByteArray sealUpdate(QByteArrayView payload, qint64 nowMs);
    // Tells this peer the share is over, so its view closes at once rather than
    // waiting for the staleness timer.
    [[nodiscard]] QByteArray encodeStop();
    // What this peer's own reports say it can take, and how much of the picture
    // it can show. The engine folds these across peers for the shared encoder.
    [[nodiscard]] int desiredLevel() const noexcept { return m_desiredLevel; }
    [[nodiscard]] QSize remoteViewSize() const noexcept { return m_remoteView; }
    [[nodiscard]] bool remoteViewHidden() const noexcept { return m_remoteViewHidden; }
    // Forgets what this peer was told, so a restarted share begins cleanly.
    void resetSendState();

    // --- Receiving ---------------------------------------------------------

    [[nodiscard]] std::optional<Update> decode(QByteArrayView packet, qint64 nowMs);
    // The periodic report for the sender, or empty when one is not due. Emitted
    // only while a share is actually being received.
    [[nodiscard]] QByteArray encodeFeedback(qint64 nowMs);
    // How large the share is being displayed here, which caps what the sender
    // bothers to encode. An empty size means it is not on screen at all.
    void setViewSize(QSize size);
    // Forgets the received canvas and asks the next report for a full resend.
    void resetReceiver();
    [[nodiscard]] bool isReceiving() const noexcept { return m_receiving; }
    [[nodiscard]] const ScreenCanvasPtr &canvas() const noexcept { return m_canvas; }

    // The encoder's counters merged with this peer's link and receive counters.
    [[nodiscard]] ScreenShareStats stats() const;

private:
    CallScreenSession(CallId id, ScreenShareTuning tuning,
                      std::shared_ptr<ScreenTileEncoder> encoder, CallMediaKeys frameSend,
                      CallMediaKeys frameReceive, CallMediaKeys feedbackSend,
                      CallMediaKeys feedbackReceive);

    void applyFeedback(QByteArrayView payload, qint64 nowMs);
    void adapt(double lossRatio, bool starved);
    [[nodiscard]] std::optional<Update> decodeFrame(quint32 sequence, QByteArrayView payload);

    CallId m_id;
    ScreenShareTuning m_tuning;
    std::shared_ptr<ScreenTileEncoder> m_encoder;
    CallMediaSealer m_frameSealer;
    CallMediaOpener m_frameOpener;
    CallMediaSealer m_feedbackSealer;
    CallMediaOpener m_feedbackOpener;

    // --- sending state, per peer ---
    quint64 m_frameSequence = 0;
    int m_desiredLevel = 2;
    int m_cleanReports = 0;
    QSize m_remoteView;
    bool m_remoteViewHidden = false;
    quint64 m_windowBytesSent = 0;
    // The highest sequence the far end has acknowledged. Loss is measured
    // between two acknowledgements rather than against what was sent, because
    // what was sent includes frames still on the wire.
    std::optional<quint32> m_lastAckedSequence;
    double m_lossRatio = 0.0;
    int m_rttMs = -1;
    // Enough recent (sequence, sent-at) pairs to close the round trip against
    // whichever frame the far end last reported.
    static constexpr int rttRingSize = 64;
    std::array<std::pair<quint32, qint64>, rttRingSize> m_sentAt{};

    // --- receiving state, per peer ---
    ScreenCanvasPtr m_canvas;
    std::optional<quint32> m_lastAppliedSequence;
    std::optional<quint8> m_currentGeneration;
    int m_receivedTileEdge = 0;
    std::vector<quint8> m_tileSeen;
    int m_tilesSeenCount = 0;
    QImage m_decodeScratch;
    QSize m_viewSize;
    bool m_resyncNeeded = true;
    quint64 m_feedbackSequence = 0;
    qint64 m_lastReportMs = 0;
    quint32 m_windowFramesApplied = 0;
    quint32 m_windowBytesReceived = 0;
    quint32 m_highestSequenceSeen = 0;
    bool m_receiving = false;
    quint64 m_framesReceived = 0;
    quint64 m_tilesApplied = 0;
    quint64 m_bytesReceived = 0;
    quint64 m_framesRejected = 0;
};

} // namespace OpenChat
