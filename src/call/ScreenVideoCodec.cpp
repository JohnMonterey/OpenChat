#include "call/ScreenVideoCodec.h"

#include "diagnostics/BlackBox.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if OPENCHAT_HAVE_VPX
#    include <vpx/vp8cx.h>
#    include <vpx/vp8dx.h>
#    include <vpx/vpx_decoder.h>
#    include <vpx/vpx_encoder.h>
#endif

namespace OpenChat {

namespace {

constexpr const char *area = "screen share";

// Geometry chosen from the viewer's window is held this long, so dragging an
// edge cannot force a keyframe on every frame.
constexpr qint64 geometryHoldMs = 3000;
// After the picture stops changing it keeps being encoded at full rate this
// long — the codec spends those frames sharpening what a keyframe or fast
// motion left soft — then only once a second, as a heartbeat.
constexpr qint64 refineAfterChangeMs = 3000;
constexpr qint64 stillHeartbeatMs = 1000;
// Frames the decoder may fall behind before it stops trying to catch up and
// asks for a keyframe instead.
constexpr int maxDecodeBacklog = 45;
// A decoder that is always a frame or two behind would otherwise never show
// anything, because only the newest picture is converted; one is shown at
// least this often however far behind it is.
constexpr qint64 maxPictureGapMs = 100;

// libvpx's real-time speed: 8 is its screen-content sweet spot, 9 trades a
// little sharpness for about a third less encoding time. A machine that
// cannot keep up at 8 moves to 9; one that cannot keep up at 9 either is given
// a lower rung (fewer pixels, fewer frames) until it can.
constexpr int normalSpeed = 8;
constexpr int fastestSpeed = 9;
// Encoding time as a share of the time between frames, in per mille.
constexpr int speedUpLoad = 700;
constexpr int slowDownLoad = 300;
constexpr int cpuOverloadLoad = 850;
constexpr int cpuCalmLoad = 350;
constexpr qint64 cpuCheckMs = 2000;
constexpr int cpuCalmChecks = 10;

// A private pool, so a busy global pool (image decoding, QML loaders) can never
// hold up a frame's conversion.
QThreadPool &conversionPool()
{
    static QThreadPool *pool = [] {
        auto *created = new QThreadPool;
        created->setMaxThreadCount(std::clamp(QThread::idealThreadCount() / 2, 1, 4));
        created->setExpiryTimeout(30000);
        return created;
    }();
    return *pool;
}

// Runs fn(begin, end) over [0, count) in stripes, the first on this thread.
template <typename Function>
void inStripes(int count, Function &&fn)
{
    const int stripes = std::clamp(conversionPool().maxThreadCount() + 1, 1, 5);
    if (stripes <= 1 || count < 64) {
        fn(0, count);
        return;
    }
    const int per = (count + stripes - 1) / stripes;
    QSemaphore done;
    int started = 0;
    for (int stripe = 1; stripe < stripes; ++stripe) {
        const int begin = stripe * per;
        const int end = std::min(count, begin + per);
        if (begin >= end)
            break;
        ++started;
        conversionPool().start([&fn, &done, begin, end] {
            fn(begin, end);
            done.release();
        });
    }
    fn(0, std::min(count, per));
    done.acquire(started);
}

inline uint8_t clampByte(int value)
{
    return uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value));
}

// Full-range BT.709 in 16.16 fixed point.
inline uint8_t lumaOf(int r, int g, int b)
{
    return uint8_t((13933 * r + 46871 * g + 4732 * b + 32768) >> 16);
}

} // namespace

void I420Picture::allocate(int newWidth, int newHeight)
{
    width = std::max(2, newWidth & ~1);
    height = std::max(2, newHeight & ~1);
    data.resize(size_t(width) * size_t(height) * 3 / 2);
}

void convertToI420(const ScreenFrameView &source, I420Picture &out)
{
    const int outWidth = out.width;
    const int outHeight = out.height;
    if (!source.isValid() || outWidth < 2 || outHeight < 2)
        return;
    const bool same = source.width >= outWidth && source.height >= outHeight
        && std::abs(source.width - outWidth) < 2 && std::abs(source.height - outHeight) < 2;
    // Source position of each output pixel's centre, in 16.16.
    const qint64 stepX = (qint64(source.width) << 16) / outWidth;
    const qint64 stepY = (qint64(source.height) << 16) / outHeight;
    // Where each channel sits in a pixel read as one 32-bit word. The
    // ARGB32 family is defined as such a word; the RGBA8888 family is defined
    // byte by byte, red first, which on a little-endian machine puts red at
    // the bottom of the word.
    const bool byteOrdered = source.format == QImage::Format_RGBX8888
        || source.format == QImage::Format_RGBA8888
        || source.format == QImage::Format_RGBA8888_Premultiplied;
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    const int redShift = byteOrdered ? 0 : 16;
    const int greenShift = 8;
    const int blueShift = byteOrdered ? 16 : 0;
#else
    const int redShift = byteOrdered ? 24 : 16;
    const int greenShift = byteOrdered ? 16 : 8;
    const int blueShift = byteOrdered ? 8 : 0;
#endif

    // Reads one output pixel's colour: direct at 1:1, bilinear otherwise.
    const auto fetch = [&](int x, int y, int &r, int &g, int &b) {
        if (same) {
            const quint32 pixel = reinterpret_cast<const quint32 *>(
                source.bits + qsizetype(y) * source.bytesPerLine)[x];
            r = int((pixel >> redShift) & 0xff);
            g = int((pixel >> greenShift) & 0xff);
            b = int((pixel >> blueShift) & 0xff);
            return;
        }
        const qint64 sx = std::max<qint64>(0, qint64(x) * stepX + stepX / 2 - 32768);
        const qint64 sy = std::max<qint64>(0, qint64(y) * stepY + stepY / 2 - 32768);
        const int x0 = std::min(int(sx >> 16), source.width - 1);
        const int y0 = std::min(int(sy >> 16), source.height - 1);
        const int x1 = std::min(x0 + 1, source.width - 1);
        const int y1 = std::min(y0 + 1, source.height - 1);
        const int fx = int((sx >> 8) & 0xff);
        const int fy = int((sy >> 8) & 0xff);
        const auto *row0 = reinterpret_cast<const quint32 *>(source.bits + qsizetype(y0) * source.bytesPerLine);
        const auto *row1 = reinterpret_cast<const quint32 *>(source.bits + qsizetype(y1) * source.bytesPerLine);
        const quint32 p00 = row0[x0], p01 = row0[x1], p10 = row1[x0], p11 = row1[x1];
        const auto mix = [&](int shift) {
            const int a = int((p00 >> shift) & 0xff), bb = int((p01 >> shift) & 0xff);
            const int c = int((p10 >> shift) & 0xff), d = int((p11 >> shift) & 0xff);
            const int top = a * (256 - fx) + bb * fx;
            const int bottom = c * (256 - fx) + d * fx;
            return (top * (256 - fy) + bottom * fy + 32768) >> 16;
        };
        r = mix(redShift);
        g = mix(greenShift);
        b = mix(blueShift);
    };

    uint8_t *planeY = out.y();
    uint8_t *planeU = out.u();
    uint8_t *planeV = out.v();
    const int chromaWidth = outWidth / 2;
    inStripes(outHeight / 2, [&](int begin, int end) {
        for (int cy = begin; cy < end; ++cy) {
            uint8_t *y0 = planeY + size_t(2 * cy) * size_t(outWidth);
            uint8_t *y1 = y0 + outWidth;
            uint8_t *u = planeU + size_t(cy) * size_t(chromaWidth);
            uint8_t *v = planeV + size_t(cy) * size_t(chromaWidth);
            for (int cx = 0; cx < chromaWidth; ++cx) {
                int r[4], g[4], b[4];
                fetch(2 * cx, 2 * cy, r[0], g[0], b[0]);
                fetch(2 * cx + 1, 2 * cy, r[1], g[1], b[1]);
                fetch(2 * cx, 2 * cy + 1, r[2], g[2], b[2]);
                fetch(2 * cx + 1, 2 * cy + 1, r[3], g[3], b[3]);
                y0[2 * cx] = lumaOf(r[0], g[0], b[0]);
                y0[2 * cx + 1] = lumaOf(r[1], g[1], b[1]);
                y1[2 * cx] = lumaOf(r[2], g[2], b[2]);
                y1[2 * cx + 1] = lumaOf(r[3], g[3], b[3]);
                const int rs = r[0] + r[1] + r[2] + r[3];
                const int gs = g[0] + g[1] + g[2] + g[3];
                const int bs = b[0] + b[1] + b[2] + b[3];
                // The four pixels' sum carries two extra bits, taken back out
                // in the shift.
                u[cx] = clampByte(((-7509 * rs - 25259 * gs + 32768 * bs + (1 << 17)) >> 18) + 128);
                v[cx] = clampByte(((32768 * rs - 29763 * gs - 3005 * bs + (1 << 17)) >> 18) + 128);
            }
        }
    });
}

void convertI420ToRgb32(const uint8_t *y, int yStride, const uint8_t *u, int uStride,
                        const uint8_t *v, int vStride, int width, int height, QImage &out)
{
    if (out.width() != width || out.height() != height || out.format() != QImage::Format_RGB32)
        out = QImage(width, height, QImage::Format_RGB32);
    if (out.isNull())
        return;
    // Taken once, here: QImage::scanLine() may detach, and is not something
    // several threads can call on one image at once.
    uchar *pixels = out.bits();
    const qsizetype stride = out.bytesPerLine();
    const int chromaWidth = (width + 1) / 2;
    inStripes((height + 1) / 2, [&](int begin, int end) {
        for (int cy = begin; cy < end; ++cy) {
            const uint8_t *urow = u + qsizetype(cy) * uStride;
            const uint8_t *vrow = v + qsizetype(cy) * vStride;
            for (int line = 0; line < 2; ++line) {
                const int row = 2 * cy + line;
                if (row >= height)
                    break;
                const uint8_t *yrow = y + qsizetype(row) * yStride;
                auto *target = reinterpret_cast<quint32 *>(pixels + qsizetype(row) * stride);
                for (int cx = 0; cx < chromaWidth; ++cx) {
                    const int cb = int(urow[cx]) - 128;
                    const int cr = int(vrow[cx]) - 128;
                    const int rAdd = (103206 * cr + 32768) >> 16;
                    const int gSub = (12276 * cb + 30679 * cr + 32768) >> 16;
                    const int bAdd = (121609 * cb + 32768) >> 16;
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = 2 * cx + dx;
                        if (x >= width)
                            break;
                        const int luma = yrow[x];
                        target[x] = 0xff000000u | (quint32(clampByte(luma + rAdd)) << 16)
                            | (quint32(clampByte(luma - gSub)) << 8) | quint32(clampByte(luma + bAdd));
                    }
                }
            }
        }
    });
}

const std::vector<ScreenVideoLevel> &screenVideoLevels()
{
    // Same length as the tile ladder, so a session's rung means the same step
    // on either. The rates are ceilings the codec's rate control aims at while
    // the picture moves; a still screen costs next to nothing on every rung.
    static const std::vector<ScreenVideoLevel> levels = {
        {8000, 30, 2560}, // a 1440p desktop at full resolution, on a good link
        {5000, 30, 1920},
        {3000, 30, 1920}, // the rung a share starts on
        {1500, 20, 1600},
        {700, 15, 1280},
    };
    return levels;
}

#if OPENCHAT_HAVE_VPX

namespace {

[[nodiscard]] int encoderThreads()
{
    return std::clamp(QThread::idealThreadCount() / 2, 1, 4);
}

[[nodiscard]] int decoderThreads()
{
    return std::clamp(QThread::idealThreadCount() / 4, 1, 2);
}

} // namespace

// --- Encoder -----------------------------------------------------------------

struct ScreenVideoEncoder::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool jobReady = false;
    I420Picture pending;
    qint64 pendingCaptureMs = 0;
    quint32 pendingNumber = 0;
    int pendingFps = 30;
    std::atomic<bool> keyframeRequested{true};
    std::atomic<int> bitrateKbps{3000};
    std::function<void(const EncodedScreenFrame &)> onEncoded;
    QPointer<QObject> context;

    // Written by the worker, read by stats().
    std::atomic<int> lastEncodeUs{0};
    std::atomic<quint64> framesEncoded{0};
    std::atomic<quint64> keyframes{0};
    std::atomic<quint64> bytesEncoded{0};
    std::atomic<int> lastFrameBytes{0};
    std::atomic<bool> failed{false};
    // libvpx speed in use, and encoding time as a share of the frame interval
    // (per mille, smoothed, keyframes left out).
    std::atomic<int> speed{normalSpeed};
    std::atomic<int> loadPermille{0};
};

namespace {

// The whole life of one VP9 encoder, on its own thread.
void runEncoder(std::shared_ptr<ScreenVideoEncoder::Shared> shared);

} // namespace

ScreenVideoEncoder::ScreenVideoEncoder(QObject *context)
    : m_context(context)
{
}

ScreenVideoEncoder::~ScreenVideoEncoder()
{
    stopWorker();
}

bool ScreenVideoEncoder::isAvailable()
{
    static const bool available = [] {
        vpx_codec_enc_cfg_t config;
        if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK)
            return false;
        config.g_w = 64;
        config.g_h = 64;
        vpx_codec_ctx_t codec;
        if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK)
            return false;
        vpx_codec_destroy(&codec);
        return true;
    }();
    return available;
}

void ScreenVideoEncoder::setOnEncoded(std::function<void(const EncodedScreenFrame &)> callback)
{
    m_onEncoded = std::move(callback);
    if (m_shared) {
        std::lock_guard lock(m_shared->mutex);
        m_shared->onEncoded = m_onEncoded;
    }
}

void ScreenVideoEncoder::ensureWorker()
{
    if (m_shared)
        return;
    m_shared = std::make_shared<Shared>();
    m_shared->context = m_context;
    m_shared->onEncoded = m_onEncoded;
    m_shared->bitrateKbps.store(bitrateKbps());
    m_worker = std::thread(runEncoder, m_shared);
}

void ScreenVideoEncoder::stopWorker()
{
    if (!m_shared)
        return;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->stop = true;
        m_shared->onEncoded = nullptr;
    }
    m_shared->wake.notify_all();
    // Joined, which waits out at most the frame being encoded. A thread left
    // to finish on its own could still be inside libvpx when the process
    // exits, and on Windows that can hang the exit for good.
    if (m_worker.joinable())
        m_worker.join();
    m_shared.reset();
}

int ScreenVideoEncoder::levelCount() const noexcept
{
    return int(screenVideoLevels().size());
}

int ScreenVideoEncoder::bytesPerSecondAt(int level) const noexcept
{
    return screenVideoLevels().at(size_t(std::clamp(level, 0, levelCount() - 1))).kilobitsPerSecond
        * 1000 / 8;
}

bool ScreenVideoEncoder::hasFailed() const
{
    return m_shared && m_shared->failed.load();
}

int ScreenVideoEncoder::effectiveLevel() const noexcept
{
    return std::clamp(std::max(m_level, m_cpuFloor), 0, levelCount() - 1);
}

int ScreenVideoEncoder::bitrateKbps() const
{
    const int ladder = screenVideoLevels().at(size_t(effectiveLevel())).kilobitsPerSecond;
    return m_linkCapKbps > 0 ? std::max(150, std::min(ladder, m_linkCapKbps)) : ladder;
}

void ScreenVideoEncoder::setLinkCapKbps(int kbps)
{
    m_linkCapKbps = std::max(0, kbps);
    if (m_shared)
        m_shared->bitrateKbps.store(bitrateKbps());
}

void ScreenVideoEncoder::requestFullResend()
{
    if (m_shared)
        m_shared->keyframeRequested.store(true);
}

void ScreenVideoEncoder::setLevel(int level)
{
    m_level = std::clamp(level, 0, levelCount() - 1);
    if (m_shared)
        m_shared->bitrateKbps.store(bitrateKbps());
}

void ScreenVideoEncoder::setRemoteView(QSize largestView, bool anyoneWatching)
{
    m_remoteView = largestView;
    m_anyoneWatching = anyoneWatching;
}

int ScreenVideoEncoder::targetFps() const noexcept
{
    if (!m_anyoneWatching)
        return ScreenShareTuning{}.idleFps;
    return screenVideoLevels().at(size_t(effectiveLevel())).targetFps;
}

void ScreenVideoEncoder::updateCpuFloor(qint64 nowMs)
{
    if (!m_shared || nowMs - m_lastCpuCheckMs < cpuCheckMs)
        return;
    m_lastCpuCheckMs = nowMs;
    const int load = m_shared->loadPermille.load();
    if (load > cpuOverloadLoad && m_shared->speed.load() >= fastestSpeed) {
        // Already at libvpx's fastest and still behind: frames would queue up
        // behind the encoder and the share would stutter however good the
        // link. A lower rung has fewer pixels and fewer frames to encode.
        m_calmCpuChecks = 0;
        const int floor = std::min(levelCount() - 1, effectiveLevel() + 1);
        if (floor != m_cpuFloor) {
            m_cpuFloor = floor;
            BlackBox::record(area, QStringLiteral("encoder busy %1% of each frame at speed %2; "
                                                  "held at rung %3 or lower")
                                       .arg(load / 10)
                                       .arg(m_shared->speed.load())
                                       .arg(m_cpuFloor));
        }
    } else if (m_cpuFloor > 0 && load < cpuCalmLoad) {
        // Plenty of room for a while: try one rung higher again.
        if (++m_calmCpuChecks >= cpuCalmChecks) {
            --m_cpuFloor;
            m_calmCpuChecks = 0;
        }
    } else {
        m_calmCpuChecks = 0;
    }
}

QSize ScreenVideoEncoder::outputSizeFor(QSize source, qint64 nowMs)
{
    int allowedEdge = screenVideoLevels().at(size_t(effectiveLevel())).maxEdge;
    // What the far end can resolve, with room for its display scaling.
    if (m_anyoneWatching && !m_remoteView.isEmpty()) {
        const int viewEdge = std::max(m_remoteView.width(), m_remoteView.height());
        allowedEdge = std::clamp(viewEdge * 2, 640, allowedEdge);
    }
    const int longEdge = std::max(source.width(), source.height());
    const double scale = longEdge > allowedEdge ? double(allowedEdge) / longEdge : 1.0;
    const QSize wanted(std::max(2, int(std::lround(source.width() * scale)) & ~1),
                       std::max(2, int(std::lround(source.height() * scale)) & ~1));
    if (wanted == m_outputSize)
        return m_outputSize;
    // The source changing size is not negotiable; a size chosen from the
    // viewer's window is, and is held down.
    const bool sourceMoved = source != m_sourceSize;
    if (!sourceMoved && !m_outputSize.isEmpty() && nowMs - m_lastGeometryMs < geometryHoldMs)
        return m_outputSize;
    m_sourceSize = source;
    m_outputSize = wanted;
    m_lastGeometryMs = nowMs;
    return m_outputSize;
}

quint64 ScreenVideoEncoder::sampleHash(const ScreenFrameView &frame) const
{
    // A sparse grid — every eighth row, every fourth pixel along it — is enough
    // to tell a still screen from a changing one at a thirtieth of the cost of
    // reading it all; a change too small to touch the grid is caught by the
    // once-a-second heartbeat.
    quint64 hash = 1469598103934665603ULL;
    for (int y = 0; y < frame.height; y += 8) {
        const auto *row = reinterpret_cast<const quint32 *>(frame.bits + qsizetype(y) * frame.bytesPerLine);
        for (int x = (y / 8) % 4; x < frame.width; x += 4) {
            hash ^= row[x] & 0x00ffffffu;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

bool ScreenVideoEncoder::submit(const ScreenFrameView &frame, qint64 nowMs)
{
    if (!frame.isValid())
        return false;
    ensureWorker();
    if (m_shared->failed.load())
        return false;
    updateCpuFloor(nowMs);
    const int fps = std::max(1, targetFps());
    if (m_lastSubmitMs != 0 && nowMs - m_lastSubmitMs < std::max<qint64>(1, 1000 / fps - 2)) {
        ++m_framesSkipped;
        return false;
    }
    const QSize output = outputSizeFor(QSize(frame.width, frame.height), nowMs);
    const quint64 hash = sampleHash(frame);
    const bool changed = hash != m_lastHash || output != QSize(m_fill.width, m_fill.height);
    if (changed) {
        m_lastHash = hash;
        m_stillFrames = 0;
    } else {
        ++m_stillFrames;
    }
    const bool refining = qint64(m_stillFrames) * 1000 / fps < refineAfterChangeMs;
    if (!changed && !refining && !m_shared->keyframeRequested.load()
        && nowMs - m_lastSubmitMs < stillHeartbeatMs) {
        ++m_framesIdle;
        return false;
    }

    {
        BlackBox::Activity activity(area, "converting a frame for the video encoder");
        if (m_fill.width != output.width() || m_fill.height != output.height())
            m_fill.allocate(output.width(), output.height());
        convertToI420(frame, m_fill);
    }
    m_lastSubmitMs = nowMs;
    {
        std::lock_guard lock(m_shared->mutex);
        // A frame still waiting for a busy encoder is replaced, not queued:
        // what goes out is always the newest picture.
        std::swap(m_shared->pending, m_fill);
        m_shared->pendingCaptureMs = nowMs;
        m_shared->pendingNumber = m_nextNumber++;
        m_shared->pendingFps = fps;
        m_shared->jobReady = true;
    }
    m_shared->bitrateKbps.store(bitrateKbps());
    m_shared->wake.notify_one();
    return true;
}

ScreenShareStats ScreenVideoEncoder::stats() const
{
    ScreenShareStats stats;
    stats.video = true;
    stats.outputSize = m_outputSize;
    stats.level = effectiveLevel();
    stats.cpuLimited = m_cpuFloor > m_level;
    stats.targetFps = targetFps();
    stats.bitrateKbps = bitrateKbps();
    stats.framesSkipped = m_framesSkipped;
    stats.framesIdle = m_framesIdle;
    if (m_shared) {
        stats.framesSent = m_shared->framesEncoded.load();
        stats.bytesSent = m_shared->bytesEncoded.load();
        stats.lastEncodeUs = m_shared->lastEncodeUs.load();
        stats.lastPayloadBytes = m_shared->lastFrameBytes.load();
        stats.keyframes = m_shared->keyframes.load();
        stats.encoderSpeed = m_shared->speed.load();
        stats.encoderLoadPercent = m_shared->loadPermille.load() / 10;
    }
    return stats;
}

void ScreenVideoEncoder::reset()
{
    stopWorker();
    m_sourceSize = QSize();
    m_outputSize = QSize();
    m_lastGeometryMs = 0;
    m_lastSubmitMs = 0;
    m_lastHash = 0;
    m_stillFrames = 0;
    m_remoteView = QSize();
    m_anyoneWatching = true;
    m_linkCapKbps = 0;
    m_cpuFloor = 0;
    m_calmCpuChecks = 0;
    m_lastCpuCheckMs = 0;
    m_fill = I420Picture();
}

namespace {

void configureEncoder(vpx_codec_ctx_t &codec, int width, int speed)
{
    vpx_codec_control(&codec, VP8E_SET_CPUUSED, speed);
    vpx_codec_control(&codec, VP9E_SET_TUNE_CONTENT, VP9E_CONTENT_SCREEN);
    vpx_codec_control(&codec, VP9E_SET_ROW_MT, 1);
    // Tile columns let the threads split each frame: 2^n columns, each at
    // least 256 pixels wide.
    int columns = 0;
    while (columns < 2 && (width >> (columns + 1)) >= 256)
        ++columns;
    vpx_codec_control(&codec, VP9E_SET_TILE_COLUMNS, columns);
    vpx_codec_control(&codec, VP9E_SET_AQ_MODE, 3);
    vpx_codec_control(&codec, VP8E_SET_STATIC_THRESHOLD, 1);
    // A keyframe of a dense desktop is huge; capped at three frames' worth it
    // arrives in well under a second, a little soft, and the frames after it
    // sharpen it.
    vpx_codec_control(&codec, VP8E_SET_MAX_INTRA_BITRATE_PCT, 300);
    vpx_codec_control(&codec, VP9E_SET_COLOR_SPACE, VPX_CS_BT_709);
    vpx_codec_control(&codec, VP9E_SET_COLOR_RANGE, VPX_CR_FULL_RANGE);
}

void fillConfig(vpx_codec_enc_cfg_t &config, int width, int height, int kbps)
{
    config.g_w = unsigned(width);
    config.g_h = unsigned(height);
    config.g_timebase = {1, 1000};
    config.g_threads = unsigned(encoderThreads());
    // Nothing held back for look-ahead: every frame goes out as soon as it is
    // encoded.
    config.g_lag_in_frames = 0;
    // The transport is one ordered connection: frames are not lost, so no
    // resilience bits are spent. A broken stream is repaired by a keyframe.
    config.g_error_resilient = 0;
    config.rc_end_usage = VPX_CBR;
    config.rc_target_bitrate = unsigned(kbps);
    config.rc_min_quantizer = 10;
    config.rc_max_quantizer = 52;
    config.rc_undershoot_pct = 50;
    config.rc_overshoot_pct = 50;
    config.rc_buf_initial_sz = 500;
    config.rc_buf_optimal_sz = 600;
    config.rc_buf_sz = 1000;
    config.rc_dropframe_thresh = 0;
    // Keyframes only when asked for: at the start, on a new size, or when a
    // viewer lost the stream.
    config.kf_mode = VPX_KF_DISABLED;
}

void runEncoder(std::shared_ptr<ScreenVideoEncoder::Shared> shared)
{
    BlackBox::nameThread("screen encoder");
    vpx_codec_ctx_t codec{};
    vpx_codec_enc_cfg_t config{};
    bool open = false;
    int openWidth = 0;
    int openHeight = 0;
    int openKbps = 0;
    I420Picture working;
    int speed = normalSpeed;
    // Delta frames encoded since the speed last changed, so one slow frame
    // cannot flip it back and forth.
    int framesAtSpeed = 0;
    double load = 0.0;
    for (;;) {
        qint64 captureMs = 0;
        quint32 number = 0;
        int fps = 30;
        {
            std::unique_lock lock(shared->mutex);
            shared->wake.wait(lock, [&] { return shared->stop || shared->jobReady; });
            if (shared->stop)
                break;
            std::swap(working, shared->pending);
            captureMs = shared->pendingCaptureMs;
            number = shared->pendingNumber;
            fps = shared->pendingFps;
            shared->jobReady = false;
        }
        BlackBox::Activity activity(area, "encoding a frame with VP9");
        const int kbps = shared->bitrateKbps.load();
        bool forceKeyframe = shared->keyframeRequested.exchange(false);
        if (!open || working.width != openWidth || working.height != openHeight) {
            if (open)
                vpx_codec_destroy(&codec);
            open = false;
            if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK) {
                shared->failed.store(true);
                break;
            }
            fillConfig(config, working.width, working.height, kbps);
            if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK) {
                BlackBox::record(area, "the VP9 encoder could not be created");
                shared->failed.store(true);
                break;
            }
            configureEncoder(codec, working.width, speed);
            open = true;
            openWidth = working.width;
            openHeight = working.height;
            openKbps = kbps;
            forceKeyframe = true;
            BlackBox::record(area, QStringLiteral("VP9 encoder open at %1x%2, %3 kbit/s, %4 threads, speed %5")
                                       .arg(openWidth)
                                       .arg(openHeight)
                                       .arg(kbps)
                                       .arg(encoderThreads())
                                       .arg(speed));
        } else if (kbps != openKbps) {
            config.rc_target_bitrate = unsigned(kbps);
            if (vpx_codec_enc_config_set(&codec, &config) == VPX_CODEC_OK)
                openKbps = kbps;
        }

        vpx_image_t image;
        if (vpx_img_wrap(&image, VPX_IMG_FMT_I420, unsigned(working.width),
                         unsigned(working.height), 1, working.data.data())
            == nullptr) {
            continue;
        }
        QElapsedTimer clock;
        clock.start();
        const vpx_codec_err_t result = vpx_codec_encode(
            &codec, &image, vpx_codec_pts_t(captureMs), 1000 / std::max(1, fps),
            forceKeyframe ? VPX_EFLAG_FORCE_KF : 0, VPX_DL_REALTIME);
        if (result != VPX_CODEC_OK) {
            // Next frame starts a fresh stream rather than feeding a broken one.
            shared->keyframeRequested.store(true);
            continue;
        }
        EncodedScreenFrame frame;
        frame.size = QSize(working.width, working.height);
        frame.number = number;
        frame.captureMs = captureMs;
        vpx_codec_iter_t iterator = nullptr;
        while (const vpx_codec_cx_pkt_t *packet = vpx_codec_get_cx_data(&codec, &iterator)) {
            if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
                continue;
            frame.data.append(static_cast<const char *>(packet->data.frame.buf),
                              qsizetype(packet->data.frame.sz));
            frame.keyframe = frame.keyframe || (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
        }
        const qint64 encodeUs = clock.nsecsElapsed() / 1000;
        shared->lastEncodeUs.store(int(encodeUs));
        if (frame.data.isEmpty())
            continue;
        if (!frame.keyframe) {
            // Keyframes are left out: they are rare, always slow, and say
            // nothing about whether the steady stream keeps up.
            const double share = double(encodeUs) / (1000000.0 / std::max(1, fps));
            load = framesAtSpeed == 0 ? share : load * 0.9 + share * 0.1;
            ++framesAtSpeed;
            shared->loadPermille.store(int(std::lround(load * 1000.0)));
            const int wanted = speed < fastestSpeed && framesAtSpeed >= 15 && load * 1000.0 > speedUpLoad
                ? fastestSpeed
                : (speed > normalSpeed && framesAtSpeed >= 150 && load * 1000.0 < slowDownLoad
                       ? normalSpeed
                       : speed);
            if (wanted != speed
                && vpx_codec_control(&codec, VP8E_SET_CPUUSED, wanted) == VPX_CODEC_OK) {
                BlackBox::record(area, QStringLiteral("VP9 speed %1 -> %2 (encoding took %3% of each frame)")
                                           .arg(speed)
                                           .arg(wanted)
                                           .arg(int(load * 100.0)));
                speed = wanted;
                framesAtSpeed = 0;
                shared->speed.store(speed);
            }
        }
        shared->framesEncoded.fetch_add(1);
        shared->bytesEncoded.fetch_add(quint64(frame.data.size()));
        shared->lastFrameBytes.store(int(frame.data.size()));
        if (frame.keyframe)
            shared->keyframes.fetch_add(1);

        // Posted with the lock held and only while the share is running: the
        // owner sets `stop` under this lock before the context can go away, so
        // the context is certainly alive for as long as the post takes.
        std::lock_guard lock(shared->mutex);
        if (shared->stop)
            break;
        QObject *context = shared->context.data();
        if (context == nullptr)
            continue;
        std::weak_ptr<ScreenVideoEncoder::Shared> weak = shared;
        QMetaObject::invokeMethod(
            context,
            [weak, frame = std::move(frame)] {
                const auto alive = weak.lock();
                if (!alive)
                    return;
                std::function<void(const EncodedScreenFrame &)> callback;
                {
                    std::lock_guard lock(alive->mutex);
                    if (alive->stop)
                        return;
                    callback = alive->onEncoded;
                }
                if (callback)
                    callback(frame);
            },
            Qt::QueuedConnection);
    }
    if (open)
        vpx_codec_destroy(&codec);
}

} // namespace

// --- Decoder -----------------------------------------------------------------

struct ScreenVideoDecoder::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::deque<std::pair<QByteArray, bool>> queue;
    bool waitingForKeyframe = true;
    std::function<void(QImage)> onPicture;
    std::function<void()> onKeyframeNeeded;
    QPointer<QObject> context;
    // The newest decoded picture not yet taken by the context's thread, and
    // whether a delivery for it is already on its way there.
    QImage latest;
    bool deliveryPosted = false;
};

namespace {

// Must be called with shared->mutex held, which is what keeps the context
// alive: the owner sets `stop` under that lock before it can go away.
void postKeyframeNeeded(const std::shared_ptr<ScreenVideoDecoder::Shared> &shared)
{
    if (shared->stop)
        return;
    QObject *context = shared->context.data();
    if (context == nullptr)
        return;
    std::weak_ptr<ScreenVideoDecoder::Shared> weak = shared;
    QMetaObject::invokeMethod(
        context,
        [weak] {
            const auto alive = weak.lock();
            if (!alive)
                return;
            std::function<void()> callback;
            {
                std::lock_guard lock(alive->mutex);
                if (alive->stop)
                    return;
                callback = alive->onKeyframeNeeded;
            }
            if (callback)
                callback();
        },
        Qt::QueuedConnection);
}

void runDecoder(std::shared_ptr<ScreenVideoDecoder::Shared> shared)
{
    BlackBox::nameThread("screen decoder");
    vpx_codec_ctx_t codec{};
    vpx_codec_dec_cfg_t config{};
    config.threads = unsigned(decoderThreads());
    if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), &config, 0) != VPX_CODEC_OK) {
        BlackBox::record(area, "the VP9 decoder could not be created");
        return;
    }
    // Decoded pictures are recycled once nobody else holds them, so a steady
    // stream does not allocate a desktop-sized image per frame.
    std::vector<QImage> pool;
    QElapsedTimer sincePicture;
    sincePicture.start();
    for (;;) {
        QByteArray compressed;
        {
            std::unique_lock lock(shared->mutex);
            shared->wake.wait(lock, [&] { return shared->stop || !shared->queue.empty(); });
            if (shared->stop)
                break;
            auto next = std::move(shared->queue.front());
            shared->queue.pop_front();
            compressed = std::move(next.first);
        }
        BlackBox::Activity activity(area, "decoding a VP9 frame");
        if (vpx_codec_decode(&codec, reinterpret_cast<const uint8_t *>(compressed.constData()),
                             unsigned(compressed.size()), nullptr, 0)
            != VPX_CODEC_OK) {
            BlackBox::record(area, "a VP9 frame could not be decoded; waiting for a keyframe");
            // A fresh decoder, so nothing of the broken stream is carried
            // into the next one.
            vpx_codec_destroy(&codec);
            if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), &config, 0) != VPX_CODEC_OK) {
                BlackBox::record(area, "the VP9 decoder could not be created again");
                return;
            }
            std::lock_guard lock(shared->mutex);
            // Whatever depends on the broken frame goes; a keyframe already
            // waiting behind it starts the stream again without asking.
            while (!shared->queue.empty() && !shared->queue.front().second)
                shared->queue.pop_front();
            if (shared->queue.empty()) {
                shared->waitingForKeyframe = true;
                postKeyframeNeeded(shared);
            }
            continue;
        }
        vpx_codec_iter_t iterator = nullptr;
        vpx_image_t *decoded = nullptr;
        while (vpx_image_t *image = vpx_codec_get_frame(&codec, &iterator))
            decoded = image;
        if (decoded == nullptr || decoded->fmt != VPX_IMG_FMT_I420)
            continue;
        const int width = int(decoded->d_w);
        const int height = int(decoded->d_h);
        // Only the newest picture is ever shown, so while more frames are
        // already waiting this one is decoded (the stream needs it) but not
        // converted — unless nothing has been shown for a while, which is
        // what a decoder that stays a frame behind would otherwise look like.
        {
            std::lock_guard lock(shared->mutex);
            if (!shared->queue.empty() && sincePicture.elapsed() < maxPictureGapMs)
                continue;
        }
        sincePicture.restart();
        // Written through the pool's own reference: a second reference taken
        // first would make the write copy the whole picture.
        QImage *target = nullptr;
        for (QImage &candidate : pool) {
            if (candidate.isDetached() && candidate.width() == width && candidate.height() == height) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                                      [&](const QImage &old) {
                                          return old.width() != width || old.height() != height;
                                      }),
                       pool.end());
            if (pool.size() >= 4)
                pool.erase(pool.begin());
            pool.emplace_back();
            target = &pool.back();
        }
        convertI420ToRgb32(decoded->planes[VPX_PLANE_Y], decoded->stride[VPX_PLANE_Y],
                           decoded->planes[VPX_PLANE_U], decoded->stride[VPX_PLANE_U],
                           decoded->planes[VPX_PLANE_V], decoded->stride[VPX_PLANE_V], width,
                           height, *target);
        if (target->isNull())
            continue;
        QImage picture = *target;

        // Posted under the lock, for the same reason as postKeyframeNeeded().
        std::lock_guard lock(shared->mutex);
        if (shared->stop)
            break;
        QObject *context = shared->context.data();
        if (context == nullptr)
            continue;
        shared->latest = std::move(picture);
        if (shared->deliveryPosted)
            continue;
        shared->deliveryPosted = true;
        std::weak_ptr<ScreenVideoDecoder::Shared> weak = shared;
        QMetaObject::invokeMethod(
            context,
            [weak] {
                const auto alive = weak.lock();
                if (!alive)
                    return;
                QImage newest;
                std::function<void(QImage)> callback;
                {
                    std::lock_guard lock(alive->mutex);
                    alive->deliveryPosted = false;
                    if (alive->stop)
                        return;
                    newest = std::move(alive->latest);
                    alive->latest = QImage();
                    callback = alive->onPicture;
                }
                if (callback && !newest.isNull())
                    callback(std::move(newest));
            },
            Qt::QueuedConnection);
    }
    vpx_codec_destroy(&codec);
}

} // namespace

ScreenVideoDecoder::ScreenVideoDecoder(QObject *context)
    : m_context(context)
{
}

ScreenVideoDecoder::~ScreenVideoDecoder()
{
    stopWorker();
}

void ScreenVideoDecoder::ensureWorker()
{
    if (m_shared)
        return;
    m_shared = std::make_shared<Shared>();
    m_shared->context = m_context;
    m_shared->onPicture = m_onPicture;
    m_shared->onKeyframeNeeded = m_onKeyframeNeeded;
    m_worker = std::thread(runDecoder, m_shared);
}

void ScreenVideoDecoder::stopWorker()
{
    if (!m_shared)
        return;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->stop = true;
        m_shared->queue.clear();
        m_shared->onPicture = nullptr;
        m_shared->onKeyframeNeeded = nullptr;
    }
    m_shared->wake.notify_all();
    // Joined, as the encoder's is: at most the frame being decoded.
    if (m_worker.joinable())
        m_worker.join();
    m_shared.reset();
}

void ScreenVideoDecoder::setOnPicture(std::function<void(QImage)> callback)
{
    m_onPicture = std::move(callback);
    if (m_shared) {
        std::lock_guard lock(m_shared->mutex);
        m_shared->onPicture = m_onPicture;
    }
}

void ScreenVideoDecoder::setOnKeyframeNeeded(std::function<void()> callback)
{
    m_onKeyframeNeeded = std::move(callback);
    if (m_shared) {
        std::lock_guard lock(m_shared->mutex);
        m_shared->onKeyframeNeeded = m_onKeyframeNeeded;
    }
}

bool ScreenVideoDecoder::submit(QByteArray frame, bool keyframe)
{
    ensureWorker();
    {
        std::lock_guard lock(m_shared->mutex);
        if (m_shared->waitingForKeyframe && !keyframe)
            return false;
        if (int(m_shared->queue.size()) >= maxDecodeBacklog) {
            // Too far behind to catch up by decoding: start again from a
            // keyframe instead — this one, if it is one.
            m_shared->queue.clear();
            if (!keyframe) {
                m_shared->waitingForKeyframe = true;
                BlackBox::record(area, "the VP9 decoder fell too far behind; asking for a keyframe");
                postKeyframeNeeded(m_shared);
                return false;
            }
        }
        // A keyframe anchors the stream the moment it is queued: the frames
        // that follow it belong to it, decoded or not yet.
        if (keyframe)
            m_shared->waitingForKeyframe = false;
        m_shared->queue.emplace_back(std::move(frame), keyframe);
    }
    m_shared->wake.notify_one();
    return true;
}

void ScreenVideoDecoder::reset()
{
    // The next frame starts a fresh decoder, waiting for a keyframe.
    stopWorker();
}

#else // no VP9 in this build: the tile encoder carries every share

struct ScreenVideoEncoder::Shared {
};
struct ScreenVideoDecoder::Shared {
};

ScreenVideoEncoder::ScreenVideoEncoder(QObject *context)
    : m_context(context)
{
}
ScreenVideoEncoder::~ScreenVideoEncoder() = default;
bool ScreenVideoEncoder::isAvailable()
{
    return false;
}
void ScreenVideoEncoder::setOnEncoded(std::function<void(const EncodedScreenFrame &)> callback)
{
    m_onEncoded = std::move(callback);
}
void ScreenVideoEncoder::ensureWorker() {}
void ScreenVideoEncoder::stopWorker() {}
int ScreenVideoEncoder::levelCount() const noexcept
{
    return int(screenVideoLevels().size());
}
int ScreenVideoEncoder::bytesPerSecondAt(int level) const noexcept
{
    return screenVideoLevels().at(size_t(std::clamp(level, 0, levelCount() - 1))).kilobitsPerSecond
        * 125;
}
int ScreenVideoEncoder::bitrateKbps() const
{
    return 0;
}
bool ScreenVideoEncoder::hasFailed() const
{
    return true;
}
void ScreenVideoEncoder::setLinkCapKbps(int) {}
void ScreenVideoEncoder::requestFullResend() {}
void ScreenVideoEncoder::setLevel(int level)
{
    m_level = std::clamp(level, 0, levelCount() - 1);
}
int ScreenVideoEncoder::effectiveLevel() const noexcept
{
    return m_level;
}
void ScreenVideoEncoder::updateCpuFloor(qint64) {}
void ScreenVideoEncoder::setRemoteView(QSize largestView, bool anyoneWatching)
{
    m_remoteView = largestView;
    m_anyoneWatching = anyoneWatching;
}
int ScreenVideoEncoder::targetFps() const noexcept
{
    return screenVideoLevels().at(size_t(m_level)).targetFps;
}
QSize ScreenVideoEncoder::outputSizeFor(QSize source, qint64)
{
    return source;
}
quint64 ScreenVideoEncoder::sampleHash(const ScreenFrameView &) const
{
    return 0;
}
bool ScreenVideoEncoder::submit(const ScreenFrameView &, qint64)
{
    return false;
}
ScreenShareStats ScreenVideoEncoder::stats() const
{
    return {};
}
void ScreenVideoEncoder::reset() {}

ScreenVideoDecoder::ScreenVideoDecoder(QObject *context)
    : m_context(context)
{
}
ScreenVideoDecoder::~ScreenVideoDecoder() = default;
void ScreenVideoDecoder::ensureWorker() {}
void ScreenVideoDecoder::stopWorker() {}
bool ScreenVideoDecoder::submit(QByteArray, bool)
{
    return false;
}
void ScreenVideoDecoder::setOnPicture(std::function<void(QImage)> callback)
{
    m_onPicture = std::move(callback);
}
void ScreenVideoDecoder::setOnKeyframeNeeded(std::function<void()> callback)
{
    m_onKeyframeNeeded = std::move(callback);
}
void ScreenVideoDecoder::reset() {}

#endif // OPENCHAT_HAVE_VPX

} // namespace OpenChat
