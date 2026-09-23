#pragma once

#include "call/CallScreenSession.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>

#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace OpenChat {

// Screen sharing as a real video stream: VP9 in its real-time screen-content
// mode, encoded and decoded off the GUI thread.
//
// The tile encoder (ScreenTileEncoder) re-sends every changed 128-pixel square
// as its own PNG or JPEG. That is ideal for a still desktop and hopeless for
// anything that moves: a scrolled page or a playing video changes every tile
// on every frame, the byte budget covers a handful of them, and a full picture
// takes a second or more to arrive. A video codec encodes motion as motion — a
// scroll is a few motion vectors, not a hundred re-sent squares — so the same
// link carries the whole picture thirty times a second. The tile encoder stays
// as the fallback for a build or a machine where VP9 is unavailable, and for
// receiving from clients that only speak it.

// A 4:2:0 picture in one contiguous buffer, planes back to back with no
// padding, which is the layout vpx_img_wrap() describes. Width and height are
// always even.
struct I420Picture final {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;

    void allocate(int width, int height);
    [[nodiscard]] uint8_t *y() { return data.data(); }
    [[nodiscard]] uint8_t *u() { return data.data() + size_t(width) * size_t(height); }
    [[nodiscard]] uint8_t *v() { return u() + size_t(width / 2) * size_t(height / 2); }
    [[nodiscard]] const uint8_t *y() const { return data.data(); }
    [[nodiscard]] const uint8_t *u() const { return data.data() + size_t(width) * size_t(height); }
    [[nodiscard]] const uint8_t *v() const { return u() + size_t(width / 2) * size_t(height / 2); }
};

// Full-range BT.709, the matrix sRGB desktops are made in. The conversion is
// done here on both ends rather than left to the codec, so no decoder's idea
// of colour ranges can wash a desktop out. Both run across a few threads.
//
// `out` must already be allocated at the output size; the source is scaled to
// it (bilinear) when the sizes differ.
void convertToI420(const ScreenFrameView &source, I420Picture &out);
// Writes Format_RGB32 into `out`, which is (re)allocated to width x height.
void convertI420ToRgb32(const uint8_t *y, int yStride, const uint8_t *u, int uStride,
                        const uint8_t *v, int vStride, int width, int height, QImage &out);

// One rung of the video ladder. Bitrate falls first, then frame rate, then
// resolution, as on the tile ladder.
struct ScreenVideoLevel final {
    int kilobitsPerSecond;
    int targetFps;
    int maxEdge;
};
[[nodiscard]] const std::vector<ScreenVideoLevel> &screenVideoLevels();

struct EncodedScreenFrame final {
    QByteArray data;
    QSize size;
    quint32 number = 0;
    // The media clock when the desktop was captured, for the receiver's delay
    // measurement.
    qint64 captureMs = 0;
    bool keyframe = false;
};

class ScreenVideoEncoder final : public ScreenEncoderControl
{
public:
    // Encoded frames are delivered on `context`'s thread, which must be the
    // thread submit() is called on (the call engine's).
    explicit ScreenVideoEncoder(QObject *context);
    ~ScreenVideoEncoder() override;

    ScreenVideoEncoder(const ScreenVideoEncoder &) = delete;
    ScreenVideoEncoder &operator=(const ScreenVideoEncoder &) = delete;

    // True when this build has VP9 and an encoder can actually be created.
    [[nodiscard]] static bool isAvailable();

    // Takes the frame if it is due: converts it (the only work done on the
    // calling thread) and hands it to the encoder thread. Returns false when it
    // was paced out, when nothing on a long-still screen needs refreshing, or
    // on failure. A frame waiting for a busy encoder is replaced by a newer
    // one, never queued behind it.
    bool submit(const ScreenFrameView &frame, qint64 nowMs);

    // Called on the context's thread for every encoded frame, in order.
    void setOnEncoded(std::function<void(const EncodedScreenFrame &)> callback);

    // The most the link itself has shown it can carry, in kbit/s; 0 lifts the
    // cap. Applied on top of the ladder's own rate.
    void setLinkCapKbps(int kbps);
    [[nodiscard]] int bitrateKbps() const;
    // True once the encoder could not be created or run on this machine; the
    // engine then carries the share on the tile encoder instead.
    [[nodiscard]] bool hasFailed() const;

    void requestFullResend() override;
    // The rung the link allows. The encoder may run lower still when this
    // machine cannot encode that rung fast enough (stats().cpuLimited).
    void setLevel(int level) override;
    [[nodiscard]] int level() const noexcept override { return m_level; }
    [[nodiscard]] int levelCount() const noexcept override;
    [[nodiscard]] int bytesPerSecondAt(int level) const noexcept override;
    void setRemoteView(QSize largestView, bool anyoneWatching) override;
    [[nodiscard]] int targetFps() const noexcept override;
    [[nodiscard]] bool remoteViewHidden() const noexcept override { return !m_anyoneWatching; }
    [[nodiscard]] ScreenShareStats stats() const override;
    void reset() override;

    // What the encoder thread shares with this object. Public only so the
    // thread's function in the .cpp can name it.
    struct Shared;

private:
    void ensureWorker();
    void stopWorker();
    [[nodiscard]] int effectiveLevel() const noexcept;
    void updateCpuFloor(qint64 nowMs);
    [[nodiscard]] QSize outputSizeFor(QSize source, qint64 nowMs);
    [[nodiscard]] quint64 sampleHash(const ScreenFrameView &frame) const;

    QObject *m_context;
    std::shared_ptr<Shared> m_shared;
    std::thread m_worker;
    std::function<void(const EncodedScreenFrame &)> m_onEncoded;
    int m_level = 2;
    // The lowest rung this machine has shown it can encode in time.
    int m_cpuFloor = 0;
    int m_calmCpuChecks = 0;
    qint64 m_lastCpuCheckMs = 0;
    int m_linkCapKbps = 0;
    QSize m_remoteView;
    bool m_anyoneWatching = true;
    QSize m_sourceSize;
    QSize m_outputSize;
    qint64 m_lastGeometryMs = 0;
    qint64 m_lastSubmitMs = 0;
    quint64 m_lastHash = 0;
    int m_stillFrames = 0;
    quint32 m_nextNumber = 0;
    I420Picture m_fill;
    quint64 m_framesSkipped = 0;
    quint64 m_framesIdle = 0;
};

class ScreenVideoDecoder final
{
public:
    // Pictures are delivered on `context`'s thread.
    explicit ScreenVideoDecoder(QObject *context);
    ~ScreenVideoDecoder();

    ScreenVideoDecoder(const ScreenVideoDecoder &) = delete;
    ScreenVideoDecoder &operator=(const ScreenVideoDecoder &) = delete;

    // One complete compressed frame, in the order it was encoded. Everything
    // is decoded (a video stream cannot skip a frame and stay intact), but only
    // the newest picture is ever handed on: a view that falls behind shows the
    // present, not a backlog. Returns false when the frame was discarded
    // because the stream is broken and waiting for a keyframe.
    bool submit(QByteArray frame, bool keyframe);
    // Newest decoded picture, Format_RGB32, on the context's thread.
    void setOnPicture(std::function<void(QImage)> callback);
    // The stream broke (a gap, a decode error, a backlog too long to catch
    // up): nothing more is decoded until a keyframe arrives.
    void setOnKeyframeNeeded(std::function<void()> callback);
    // Forget the stream; the next frame must be a keyframe.
    void reset();

    // What the decoder thread shares with this object; see the encoder's.
    struct Shared;

private:
    void ensureWorker();
    void stopWorker();

    QObject *m_context;
    std::shared_ptr<Shared> m_shared;
    std::thread m_worker;
    std::function<void(QImage)> m_onPicture;
    std::function<void()> m_onKeyframeNeeded;
};

} // namespace OpenChat
