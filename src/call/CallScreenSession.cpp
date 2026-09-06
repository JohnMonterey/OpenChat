#include "call/CallScreenSession.h"

#include "call/CallMediaPacket.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>

namespace OpenChat {

Q_LOGGING_CATEGORY(lcScreenShare, "openchat.screenshare", QtWarningMsg)

namespace {

// Alpha (or the padding byte) sits in the most significant byte of the native
// word for every 32-bit QImage format on a little-endian host, which is every
// host this application targets. Screen captures routinely leave it as garbage,
// so it is excluded from the change hash: a desktop that has not moved must not
// read as dirty because the compositor felt differently about a byte nobody
// draws.
constexpr quint32 rgbMask = (Q_BYTE_ORDER == Q_LITTLE_ENDIAN) ? 0x00ffffffu : 0xffffff00u;

constexpr quint64 fnvOffset = 1469598103934665603ULL;
constexpr quint64 fnvPrime = 1099511628211ULL;

// A tile with no more distinct colours than this is UI or text, and goes out
// losslessly. Above it the tile is photographic and JPEG is the honest choice.
constexpr int paletteLimit = 16;

// The PNG handler reads "quality" as a zlib level, 0-9, and warns above it.
// Level 3 is a fraction of the default's CPU and within a few percent of its
// size on flat UI content, which is the only content that takes this path.
constexpr int pngCompressionLevel = 3;

// Loss above this costs a rung; below the clean threshold for long enough earns
// one back. The gap between them is what stops the ladder oscillating.
constexpr double lossDegradeThreshold = 0.08;
constexpr double lossCleanThreshold = 0.015;
constexpr int cleanReportsToRecover = 6;

// A share whose changed-tile ratio stays above this is moving content — a game
// or a video — and is better served by more frames than by more detail.
constexpr double motionEnterRatio = 0.35;
constexpr double motionLeaveRatio = 0.15;
constexpr qint64 motionDwellMs = 1000;

// Geometry cannot change more often than this, so a viewer dragging their
// window edge cannot force a full resend on every frame.
constexpr qint64 geometryHoldMs = 3000;

[[nodiscard]] bool isSupportedSourceFormat(QImage::Format format) noexcept
{
    switch (format) {
    case QImage::Format_RGB32:
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
    case QImage::Format_RGBX8888:
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return true;
    default:
        return false;
    }
}

// The opaque twin of a format, with the identical memory layout. Reading a tile
// through it means a capture that left alpha at zero cannot be encoded as a
// fully transparent PNG.
[[nodiscard]] QImage::Format opaqueTwin(QImage::Format format) noexcept
{
    switch (format) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        return QImage::Format_RGB32;
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        return QImage::Format_RGBX8888;
    default:
        return format;
    }
}

[[nodiscard]] int roundUpToPowerOfTwo(int value, int low, int high) noexcept
{
    int result = low;
    while (result < value && result < high)
        result <<= 1;
    return std::clamp(result, low, high);
}

[[nodiscard]] int shiftFor(int powerOfTwo) noexcept
{
    int shift = 0;
    while ((1 << shift) < powerOfTwo)
        ++shift;
    return shift;
}

[[nodiscard]] int divideRoundingUp(int value, int divisor) noexcept
{
    return divisor > 0 ? (value + divisor - 1) / divisor : 0;
}

void appendBigEndian16(QByteArray &out, quint16 value)
{
    char bytes[2];
    qToBigEndian(value, bytes);
    out.append(bytes, 2);
}

void appendBigEndian32(QByteArray &out, quint32 value)
{
    char bytes[4];
    qToBigEndian(value, bytes);
    out.append(bytes, 4);
}

} // namespace

const std::vector<ScreenShareLevel> &screenShareLevels()
{
    // Bitrate falls first, then frame rate, then resolution — the order the
    // brief asks for, and the order that costs a viewer the least: a slightly
    // softer picture is easier to read past than a stuttering or a blurred one.
    // The byte rates are ceilings, not targets: a still desktop sends almost
    // nothing at every rung.
    static const std::vector<ScreenShareLevel> levels = {
        {92, 30, 1, 1'900'000}, // sharp text on a good link; a full packet per frame
        {86, 30, 1, 1'200'000},
        {78, 20, 1, 600'000}, // the rung a share starts on
        {68, 15, 2, 300'000}, // half resolution
        {55, 10, 2, 150'000},
    };
    return levels;
}

bool ScreenFrameView::isValid() const noexcept
{
    return bits != nullptr && width > 0 && height > 0 && isSupportedSourceFormat(format)
        && bytesPerLine >= qsizetype(width) * 4 && bytesPerLine % 4 == 0
        && reinterpret_cast<quintptr>(bits) % 4 == 0;
}

ScreenFrameView ScreenFrameView::fromImage(const QImage &image)
{
    ScreenFrameView view;
    if (image.isNull() || !isSupportedSourceFormat(image.format()))
        return view;
    view.bits = image.constBits();
    view.bytesPerLine = image.bytesPerLine();
    view.width = image.width();
    view.height = image.height();
    view.format = image.format();
    return view;
}

// --- ScreenTileEncoder -------------------------------------------------------

ScreenTileEncoder::ScreenTileEncoder(ScreenShareTuning tuning)
    : m_tuning(tuning)
{
    m_tileEdge = roundUpToPowerOfTwo(m_tuning.tileEdge, 1 << minTileShift, 1 << maxTileShift);
    m_tuning.tileEdge = m_tileEdge;
    m_tuning.maxPacketBytes = std::clamp(m_tuning.maxPacketBytes, 16 * 1024, 256 * 1024);
    m_tuning.maxOutputEdge = std::clamp(m_tuning.maxOutputEdge, 320, maxCanvasEdge);
    m_level = std::clamp(m_level, 0, int(screenShareLevels().size()) - 1);
}

int ScreenTileEncoder::targetFps() const noexcept
{
    if (!m_anyoneWatching)
        return m_tuning.idleFps;
    const ScreenShareLevel &level = screenShareLevels().at(size_t(m_level));
    // Moving content buys frames with detail, never with bandwidth: the rung's
    // byte ceiling is unchanged, so each frame simply gets a smaller share.
    return m_motionMode ? std::min(60, level.targetFps * 2) : level.targetFps;
}

void ScreenTileEncoder::setLevel(int level)
{
    const int clamped = std::clamp(level, 0, int(screenShareLevels().size()) - 1);
    if (clamped == m_level)
        return;
    m_level = clamped;
    if (m_level > 1)
        m_motionMode = false; // frames are the first luxury to go
}

void ScreenTileEncoder::setRemoteView(QSize largestView, bool anyoneWatching)
{
    m_remoteView = largestView;
    m_anyoneWatching = anyoneWatching;
}

void ScreenTileEncoder::requestFullResend()
{
    if (m_tileDirty.empty())
        return;
    std::fill(m_tileDirty.begin(), m_tileDirty.end(), quint8(1));
    m_dirtyCount = int(m_tileDirty.size());
    qCDebug(lcScreenShare) << "full resend requested";
}

void ScreenTileEncoder::reconfigure(int sourceWidth, int sourceHeight, qint64 nowMs)
{
    const ScreenShareLevel &level = screenShareLevels().at(size_t(m_level));

    // What the far end can actually resolve caps what is worth encoding, with
    // headroom for its display scaling. A share in a 300 px panel does not
    // deserve 1080p.
    int allowedEdge = m_tuning.maxOutputEdge;
    if (m_anyoneWatching && !m_remoteView.isEmpty()) {
        const int viewEdge = std::max(m_remoteView.width(), m_remoteView.height());
        allowedEdge = std::clamp(viewEdge * 2, 640, m_tuning.maxOutputEdge);
    }
    const int longEdge = std::max(sourceWidth, sourceHeight);
    const int fitDivisor = std::max(1, divideRoundingUp(longEdge, allowedEdge));
    const int wanted = fitDivisor * std::max(1, level.divisor);

    const bool sizeMoved = m_sourceSize != QSize(sourceWidth, sourceHeight);
    const bool divisorMoved = wanted != m_divisor;
    if (!sizeMoved && !divisorMoved && !m_tileHash.empty())
        return;
    // The source's own size changing is not negotiable — a monitor was swapped
    // or a window resized — but a divisor chosen from the far end's window is,
    // and it is held down so dragging an edge cannot force a resend per frame.
    if (!sizeMoved && divisorMoved && !m_tileHash.empty()
        && nowMs - m_lastGeometryMs < geometryHoldMs)
        return;

    m_sourceSize = QSize(sourceWidth, sourceHeight);
    m_lastGeometryMs = nowMs;
    m_divisor = wanted;
    m_sourceTileEdge = m_tileEdge * m_divisor;
    const int outputWidth = divideRoundingUp(sourceWidth, m_divisor);
    const int outputHeight = divideRoundingUp(sourceHeight, m_divisor);
    m_columns = divideRoundingUp(outputWidth, m_tileEdge);
    m_rows = divideRoundingUp(outputHeight, m_tileEdge);

    const size_t tiles = size_t(m_columns) * size_t(m_rows);
    m_tileHash.assign(tiles, 0);
    m_tileDirty.assign(tiles, 1); // a new geometry is entirely new to the peer
    m_dirtyCount = int(tiles);
    m_sendCursor = 0;
    m_refreshCursor = 0;
    ++m_generation;
    m_stats.outputSize = QSize(outputWidth, outputHeight);
    m_stats.totalTiles = int(tiles);
    qCDebug(lcScreenShare) << "sender geometry" << m_sourceSize << "->" << m_stats.outputSize
                           << "divisor" << m_divisor << "tiles" << m_columns << "x" << m_rows
                           << "generation" << m_generation;
}

int ScreenTileEncoder::markDirtyTiles(const ScreenFrameView &frame)
{
    const int step = std::max(1, m_divisor);
    // When downscaling, only the pixels that survive the scale are hashed, so
    // the cost stays proportional to what is sent rather than to the desktop.
    // The starting offset rotates, so over `divisor` frames every pixel has
    // still been looked at and no change can hide in the gaps.
    const int phase = step > 1 ? (m_samplePhase % step) : 0;
    m_samplePhase = (m_samplePhase + 1) % step;

    int changed = 0;
    for (int row = 0; row < m_rows; ++row) {
        const int firstY = row * m_sourceTileEdge;
        const int lastY = std::min(frame.height, firstY + m_sourceTileEdge);
        for (int column = 0; column < m_columns; ++column) {
            const int firstX = column * m_sourceTileEdge;
            const int lastX = std::min(frame.width, firstX + m_sourceTileEdge);
            quint64 hash = fnvOffset;
            for (int y = firstY + phase; y < lastY; y += step) {
                const auto *pixels = reinterpret_cast<const quint32 *>(
                    frame.bits + qsizetype(y) * frame.bytesPerLine);
                for (int x = firstX + phase; x < lastX; x += step) {
                    hash ^= quint64(pixels[x] & rgbMask);
                    hash *= fnvPrime;
                }
            }
            const int index = row * m_columns + column;
            if (m_tileHash[size_t(index)] == hash)
                continue;
            m_tileHash[size_t(index)] = hash;
            ++changed;
            if (m_tileDirty[size_t(index)] == 0) {
                m_tileDirty[size_t(index)] = 1;
                ++m_dirtyCount;
            }
        }
    }
    return changed;
}

QImage ScreenTileEncoder::stageTile(const ScreenFrameView &frame, int column, int row)
{
    const int outputWidth = m_stats.outputSize.width();
    const int outputHeight = m_stats.outputSize.height();
    const int tileX = column * m_tileEdge;
    const int tileY = row * m_tileEdge;
    const int tileWidth = std::min(m_tileEdge, outputWidth - tileX);
    const int tileHeight = std::min(m_tileEdge, outputHeight - tileY);
    if (tileWidth <= 0 || tileHeight <= 0)
        return {};

    const int sourceX = column * m_sourceTileEdge;
    const int sourceY = row * m_sourceTileEdge;
    const int sourceWidth = std::min(m_sourceTileEdge, frame.width - sourceX);
    const int sourceHeight = std::min(m_sourceTileEdge, frame.height - sourceY);
    if (sourceWidth <= 0 || sourceHeight <= 0)
        return {};

    if (m_divisor == 1) {
        // Nothing to resample, so there is nothing to copy either: the encoder
        // reads this tile straight out of the capture's buffer, through the
        // opaque twin of its format so a capture that leaves alpha at zero
        // cannot be encoded as transparent.
        return QImage(frame.bits + qsizetype(sourceY) * frame.bytesPerLine
                          + qsizetype(sourceX) * 4,
                      tileWidth, tileHeight, frame.bytesPerLine, opaqueTwin(frame.format));
    }

    if (m_tileScratch.width() != m_tileEdge || m_tileScratch.height() != m_tileEdge
        || m_tileScratch.format() != frame.format) {
        // Allocated once per geometry, then written over for the life of the
        // share, and only on the downscaling path. This is the only full-tile
        // buffer the sender ever holds.
        m_tileScratch = QImage(m_tileEdge, m_tileEdge, frame.format);
    }
    {
        // Borrowed, never copied: the region of the captured desktop this tile
        // covers, addressed in place through the capture's own stride, and
        // resampled directly into the scratch.
        const QImage source(frame.bits + qsizetype(sourceY) * frame.bytesPerLine
                                + qsizetype(sourceX) * 4,
                            sourceWidth, sourceHeight, frame.bytesPerLine, frame.format);
        QPainter painter(&m_tileScratch);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(QRect(0, 0, tileWidth, tileHeight), source,
                          QRect(0, 0, sourceWidth, sourceHeight));
    }
    return QImage(m_tileScratch.bits(), tileWidth, tileHeight, m_tileScratch.bytesPerLine(),
                  opaqueTwin(m_tileScratch.format()));
}

bool ScreenTileEncoder::encodeTile(const QImage &tile, QByteArray &out, TileEncoding &encoding)
{
    const int width = tile.width();
    const int height = tile.height();
    if (width <= 0 || height <= 0)
        return false;
    // A tile has to fit in a packet with room for the headers around it, and in
    // the sixteen bits its length is written in.
    const int tileCeiling =
        std::min({maxTileBytes, m_tuning.maxPacketBytes - updateHeaderBytes - tileHeaderBytes,
                  int(std::numeric_limits<quint16>::max())});

    // One pass answers both questions the choice of encoding turns on: is the
    // tile a single colour, and does it have few enough colours to be UI rather
    // than a photograph. It stops as soon as neither answer can still change.
    const quint32 first = *reinterpret_cast<const quint32 *>(tile.constScanLine(0)) & rgbMask;
    bool solid = true;
    quint32 palette[paletteLimit];
    palette[0] = first;
    int paletteCount = 1;
    bool paletteOverflowed = false;
    for (int y = 0; y < height; ++y) {
        const auto *pixels = reinterpret_cast<const quint32 *>(tile.constScanLine(y));
        for (int x = 0; x < width; ++x) {
            const quint32 pixel = pixels[x] & rgbMask;
            if (pixel != first)
                solid = false;
            if (!paletteOverflowed) {
                int slot = 0;
                while (slot < paletteCount && palette[slot] != pixel)
                    ++slot;
                if (slot == paletteCount) {
                    if (paletteCount == paletteLimit)
                        paletteOverflowed = true;
                    else
                        palette[paletteCount++] = pixel;
                }
            }
            if (paletteOverflowed && !solid)
                break;
        }
        if (paletteOverflowed && !solid)
            break;
    }

    out.resize(0);
    if (solid) {
        encoding = TileEncoding::Solid;
        const QRgb colour = tile.pixel(0, 0);
        out.append(char(qRed(colour)));
        out.append(char(qGreen(colour)));
        out.append(char(qBlue(colour)));
        return true;
    }

    const bool lossless = !paletteOverflowed;
    {
        QBuffer buffer(&out);
        if (!buffer.open(QIODevice::WriteOnly))
            return false;
        QImageWriter writer(&buffer,
                            lossless ? QByteArrayLiteral("png") : QByteArrayLiteral("jpeg"));
        writer.setQuality(lossless ? pngCompressionLevel
                                   : screenShareLevels().at(size_t(m_level)).jpegQuality);
        if (!writer.write(tile))
            return false;
    }
    encoding = lossless ? TileEncoding::Png : TileEncoding::Jpeg;

    if (out.size() > tileCeiling) {
        // Only reachable for a pathologically noisy tile. Re-encode it hard
        // rather than dropping the region or bursting the packet.
        out.resize(0);
        QBuffer retry(&out);
        if (!retry.open(QIODevice::WriteOnly))
            return false;
        QImageWriter fallback(&retry, QByteArrayLiteral("jpeg"));
        fallback.setQuality(20);
        if (!fallback.write(tile))
            return false;
        retry.close();
        encoding = TileEncoding::Jpeg;
        if (out.size() > tileCeiling)
            return false;
    }
    return true;
}

QByteArrayView ScreenTileEncoder::buildUpdate(const ScreenFrameView &frame, qint64 nowMs)
{
    if (!frame.isValid())
        return {};

    const int fps = std::max(1, targetFps());
    // The send rate is ours, not the display's: a 240 Hz monitor offers frames
    // at 240 Hz and this is where all but the ones we asked for are dropped.
    const qint64 minimumGapMs = std::max<qint64>(1, 1000 / fps - 2);
    if (m_lastFrameMs != 0 && nowMs - m_lastFrameMs < minimumGapMs) {
        ++m_stats.framesSkipped;
        return {};
    }

    reconfigure(frame.width, frame.height, nowMs);
    if (m_columns <= 0 || m_rows <= 0)
        return {};

    QElapsedTimer encodeClock;
    encodeClock.start();

    const int totalTiles = m_columns * m_rows;
    const int changed = markDirtyTiles(frame);

    // The optional rolling intra-refresh, off unless a deployment asks for it.
    // Loss is normally repaired by the receiver noticing a gap in the sequence
    // and asking for the picture again; this is the blind fallback for a
    // transport that could lose a frame without leaving one.
    if (m_lastRefreshMs == 0)
        m_lastRefreshMs = nowMs;
    const qint64 sinceRefresh = nowMs - m_lastRefreshMs;
    if (sinceRefresh > 0 && m_tuning.refreshPeriodMs > 0) {
        const int quota = int(qint64(totalTiles) * sinceRefresh / m_tuning.refreshPeriodMs);
        if (quota > 0) {
            for (int i = 0; i < quota && i < totalTiles; ++i) {
                const int index = (m_refreshCursor + i) % totalTiles;
                if (m_tileDirty[size_t(index)] == 0) {
                    m_tileDirty[size_t(index)] = 1;
                    ++m_dirtyCount;
                }
            }
            m_refreshCursor = (m_refreshCursor + quota) % totalTiles;
            m_lastRefreshMs = nowMs;
        }
    }

    // Motion detection runs on what actually moved, not on what is queued for
    // refresh, so a still desktop never looks like a game.
    const double changeRatio = totalTiles > 0 ? double(changed) / totalTiles : 0.0;
    if (changeRatio >= motionEnterRatio) {
        if (m_motionSinceMs == 0)
            m_motionSinceMs = nowMs;
        else if (!m_motionMode && nowMs - m_motionSinceMs >= motionDwellMs && m_level <= 1)
            m_motionMode = true;
    } else if (changeRatio <= motionLeaveRatio) {
        m_motionSinceMs = 0;
        m_motionMode = false;
    }

    m_stats.dirtyTiles = m_dirtyCount;
    m_stats.totalTiles = totalTiles;
    m_stats.level = m_level;
    m_stats.targetFps = fps;
    m_stats.jpegQuality = screenShareLevels().at(size_t(m_level)).jpegQuality;
    m_stats.motionMode = m_motionMode;
    const bool heartbeatDue = m_tuning.heartbeatMs > 0
        && (m_lastSentMs == 0 || nowMs - m_lastSentMs >= m_tuning.heartbeatMs);
    if (m_dirtyCount == 0) {
        ++m_stats.framesIdle;
        m_lastFrameMs = nowMs;
        if (!heartbeatDue)
            return {};
        // Nothing moved, so this update carries no tiles at all: it exists to
        // say the share is still running and to keep the reports coming back.
        m_payload.resize(0);
        appendBigEndian16(m_payload, quint16(m_stats.outputSize.width()));
        appendBigEndian16(m_payload, quint16(m_stats.outputSize.height()));
        m_payload.append(char(shiftFor(m_tileEdge)));
        m_payload.append(char(m_generation));
        appendBigEndian16(m_payload, 0);
        m_lastSentMs = nowMs;
        m_stats.lastPayloadBytes = int(m_payload.size());
        m_stats.lastTileCount = 0;
        return QByteArrayView(m_payload);
    }

    const ScreenShareLevel &level = screenShareLevels().at(size_t(m_level));
    const int perFrameCeiling = m_tuning.maxPacketBytes - updateHeaderBytes - 64;
    const int budget = std::clamp(level.bytesPerSecond / fps, 8 * 1024, perFrameCeiling);

    m_payload.resize(0);
    m_payload.reserve(std::min(budget + 4096, m_tuning.maxPacketBytes));
    appendBigEndian16(m_payload, quint16(m_stats.outputSize.width()));
    appendBigEndian16(m_payload, quint16(m_stats.outputSize.height()));
    m_payload.append(char(shiftFor(m_tileEdge)));
    m_payload.append(char(m_generation));
    appendBigEndian16(m_payload, 0); // patched with the tile count below

    int sent = 0;
    int spent = 0;
    int lastIndexSent = -1;
    // The cursor rotates between frames so that a budget too small for every
    // dirty tile starves no region: whatever was skipped last time is reached
    // first this time. Nothing is ever queued — a tile that changes three times
    // before it can be sent is simply sent once, in its newest state.
    for (int offset = 0; offset < totalTiles; ++offset) {
        const int index = (m_sendCursor + offset) % totalTiles;
        if (m_tileDirty[size_t(index)] == 0)
            continue;
        if (spent >= budget)
            break;
        const QImage tile = stageTile(frame, index % m_columns, index / m_columns);
        if (tile.isNull()) {
            m_tileDirty[size_t(index)] = 0;
            --m_dirtyCount;
            continue;
        }
        TileEncoding encoding = TileEncoding::Jpeg;
        if (!encodeTile(tile, m_tileBuffer, encoding)) {
            // The same pixels would fail the same way next frame, so retrying
            // would stall every tile behind this one for as long as the content
            // sat there. It is cleared instead and marked again the moment it
            // changes — which is the only thing that could change the answer.
            qCWarning(lcScreenShare) << "tile" << index << "could not be encoded; skipping";
            m_tileDirty[size_t(index)] = 0;
            --m_dirtyCount;
            continue;
        }
        if (spent > 0 && spent + tileHeaderBytes + int(m_tileBuffer.size()) > budget)
            break;
        appendBigEndian16(m_payload, quint16(index));
        m_payload.append(char(quint8(encoding)));
        appendBigEndian16(m_payload, quint16(m_tileBuffer.size()));
        m_payload.append(m_tileBuffer);
        spent += tileHeaderBytes + int(m_tileBuffer.size());
        m_tileDirty[size_t(index)] = 0;
        --m_dirtyCount;
        lastIndexSent = index;
        ++sent;
        if (sent == std::numeric_limits<quint16>::max())
            break;
    }
    if (lastIndexSent >= 0)
        m_sendCursor = (lastIndexSent + 1) % totalTiles;

    m_lastFrameMs = nowMs;
    if (sent == 0)
        return {};
    qToBigEndian(quint16(sent), m_payload.data() + 6);
    m_lastSentMs = nowMs;

    ++m_stats.framesSent;
    m_stats.tilesSent += quint64(sent);
    m_stats.bytesSent += quint64(m_payload.size());
    m_stats.lastPayloadBytes = int(m_payload.size());
    m_stats.lastTileCount = sent;
    m_stats.lastEncodeUs = int(encodeClock.nsecsElapsed() / 1000);
    m_stats.dirtyTiles = m_dirtyCount;
    return QByteArrayView(m_payload);
}

void ScreenTileEncoder::reset()
{
    // Every sending buffer goes back to the allocator the moment sharing stops.
    // Nothing here is kept warm for a share that may never happen again.
    m_sourceSize = QSize();
    m_columns = 0;
    m_rows = 0;
    m_dirtyCount = 0;
    m_sendCursor = 0;
    m_refreshCursor = 0;
    m_samplePhase = 0;
    m_lastFrameMs = 0;
    m_lastSentMs = 0;
    m_lastRefreshMs = 0;
    m_lastGeometryMs = 0;
    m_motionMode = false;
    m_motionSinceMs = 0;
    m_anyoneWatching = true;
    m_remoteView = QSize();
    std::vector<quint64>().swap(m_tileHash);
    std::vector<quint8>().swap(m_tileDirty);
    m_tileScratch = QImage();
    m_tileBuffer = QByteArray();
    m_payload = QByteArray();
    m_stats.outputSize = QSize();
    m_stats.dirtyTiles = 0;
    m_stats.totalTiles = 0;
}

// --- CallScreenSession -------------------------------------------------------

CallScreenSession::CallScreenSession(CallId id, ScreenShareTuning tuning,
                                     std::shared_ptr<ScreenTileEncoder> encoder,
                                     CallMediaKeys frameSend, CallMediaKeys frameReceive,
                                     CallMediaKeys feedbackSend, CallMediaKeys feedbackReceive)
    : m_id(id)
    , m_tuning(tuning)
    , m_encoder(std::move(encoder))
    , m_frameSealer(std::move(frameSend))
    , m_frameOpener(std::move(frameReceive))
    , m_feedbackSealer(std::move(feedbackSend))
    , m_feedbackOpener(std::move(feedbackReceive))
{
    m_desiredLevel = m_encoder ? m_encoder->level() : 2;
}

std::unique_ptr<CallScreenSession> CallScreenSession::create(
    const CallId &id, CallDirection direction, QByteArrayView secret,
    std::shared_ptr<ScreenTileEncoder> encoder, ScreenShareTuning tuning)
{
    const auto frames = CallMediaKeySchedule::deriveScreen(secret, id);
    const auto reports = CallMediaKeySchedule::deriveScreenFeedback(secret, id);
    if (!frames || !reports || !encoder)
        return {};
    return std::unique_ptr<CallScreenSession>(new CallScreenSession(
        id, tuning, std::move(encoder), frames->sendKeys(direction),
        frames->receiveKeys(direction), reports->sendKeys(direction),
        reports->receiveKeys(direction)));
}

QByteArray CallScreenSession::sealUpdate(QByteArrayView payload, qint64 nowMs)
{
    if (payload.isEmpty())
        return {};
    // Exhaustion must never wrap the nonce, even on an implausibly long share.
    if (m_frameSequence > std::numeric_limits<quint32>::max())
        return {};
    CallMediaPacket packet;
    packet.version = wireVersion;
    packet.callId = m_id;
    packet.flags = flagContent;
    packet.sequence = quint32(m_frameSequence++);
    packet.sealed = m_frameSealer.seal(packet.sequence, payload, packet.header());
    if (packet.sealed.isEmpty())
        return {};
    QByteArray encoded = packet.encode();
    m_sentAt[packet.sequence % rttRingSize] = {packet.sequence, nowMs};
    m_windowBytesSent += quint64(encoded.size());
    return encoded;
}

QByteArray CallScreenSession::encodeStop()
{
    if (m_frameSequence > std::numeric_limits<quint32>::max())
        return {};
    CallMediaPacket packet;
    packet.version = wireVersion;
    packet.callId = m_id;
    packet.flags = 0; // no content: the share is over
    packet.sequence = quint32(m_frameSequence++);
    packet.sealed = m_frameSealer.seal(packet.sequence, QByteArray(), packet.header());
    return packet.sealed.isEmpty() ? QByteArray() : packet.encode();
}

void CallScreenSession::resetSendState()
{
    m_desiredLevel = 2;
    m_cleanReports = 0;
    m_remoteView = QSize();
    m_remoteViewHidden = false;
    m_lastAckedSequence.reset();
    m_windowBytesSent = 0;
    m_lossRatio = 0.0;
    m_rttMs = -1;
    m_sentAt.fill({0, 0});
}

void CallScreenSession::setViewSize(QSize size)
{
    m_viewSize = size.isEmpty() ? QSize() : size;
}

void CallScreenSession::resetReceiver()
{
    m_canvas.reset();
    m_lastAppliedSequence.reset();
    m_currentGeneration.reset();
    m_receivedTileEdge = 0;
    m_tilesSeenCount = 0;
    std::vector<quint8>().swap(m_tileSeen);
    m_decodeScratch = QImage();
    m_resyncNeeded = true;
    m_receiving = false;
    m_windowFramesApplied = 0;
    m_windowBytesReceived = 0;
}

std::optional<CallScreenSession::Update> CallScreenSession::decode(QByteArrayView packet,
                                                                   qint64 nowMs)
{
    constexpr auto headerSize = CallMediaPacket::headerBytes;
    if (packet.size() < headerSize + CallMediaSealer::tagBytes)
        return std::nullopt;
    const auto *data = reinterpret_cast<const uchar *>(packet.data());
    if (data[0] != wireVersion
        || packet.sliced(2, CallId::byteCount) != QByteArrayView(m_id.bytes()))
        return std::nullopt;
    const quint8 flags = data[1];
    if ((flags & ~(flagContent | flagFeedback)) != 0)
        return std::nullopt;
    const quint32 sequence = qFromBigEndian<quint32>(data + 18);
    const QByteArrayView sealed = packet.sliced(headerSize);
    const QByteArrayView associated = packet.first(headerSize);

    if ((flags & flagFeedback) != 0) {
        if ((flags & flagContent) != 0)
            return std::nullopt;
        const auto payload = m_feedbackOpener.open(sequence, sealed, associated);
        if (!payload)
            return std::nullopt;
        applyFeedback(*payload, nowMs);
        Update update;
        update.kind = Update::Kind::Feedback;
        return update;
    }

    // Frames arrive over a single ordered connection, so anything not newer is
    // a duplicate or a replay and can never improve the picture.
    if (m_lastAppliedSequence && sequence <= *m_lastAppliedSequence) {
        ++m_framesRejected;
        return std::nullopt;
    }
    const auto payload = m_frameOpener.open(sequence, sealed, associated);
    if (!payload) {
        ++m_framesRejected;
        return std::nullopt;
    }
    if (m_lastAppliedSequence && sequence > *m_lastAppliedSequence + 1) {
        // A gap in an ordered stream is a genuine loss, not a reordering. Ask
        // for everything again rather than living with squares of stale desktop
        // until they happen to change.
        m_resyncNeeded = true;
        qCDebug(lcScreenShare) << "sequence gap" << *m_lastAppliedSequence << "->" << sequence;
    }
    m_highestSequenceSeen = std::max(m_highestSequenceSeen, sequence);

    if ((flags & flagContent) == 0) {
        if (!payload->isEmpty())
            return std::nullopt;
        resetReceiver();
        m_lastAppliedSequence = sequence;
        Update update;
        update.kind = Update::Kind::Stopped;
        return update;
    }
    m_windowBytesReceived += quint32(packet.size());
    return decodeFrame(sequence, *payload);
}

std::optional<CallScreenSession::Update> CallScreenSession::decodeFrame(quint32 sequence,
                                                                       QByteArrayView payload)
{
    if (payload.size() < ScreenTileEncoder::updateHeaderBytes) {
        ++m_framesRejected;
        return std::nullopt;
    }
    const auto *header = reinterpret_cast<const uchar *>(payload.data());
    const int width = qFromBigEndian<quint16>(header);
    const int height = qFromBigEndian<quint16>(header + 2);
    const int tileShift = header[4];
    const quint8 generation = header[5];
    const int tileCount = qFromBigEndian<quint16>(header + 6);

    // Everything the sender declares is checked before a byte is allocated for
    // it, authenticated peer or not.
    if (width <= 0 || height <= 0 || width > ScreenTileEncoder::maxCanvasEdge
        || height > ScreenTileEncoder::maxCanvasEdge
        || tileShift < ScreenTileEncoder::minTileShift
        || tileShift > ScreenTileEncoder::maxTileShift) {
        ++m_framesRejected;
        return std::nullopt;
    }
    const int tileEdge = 1 << tileShift;
    const int columns = divideRoundingUp(width, tileEdge);
    const int rows = divideRoundingUp(height, tileEdge);
    if (columns <= 0 || rows <= 0 || tileCount > columns * rows) {
        ++m_framesRejected;
        return std::nullopt;
    }

    Update update;
    update.kind = Update::Kind::Frame;
    const bool geometryMoved = !m_canvas || m_canvas->size() != QSize(width, height)
        || m_receivedTileEdge != tileEdge || !m_currentGeneration
        || *m_currentGeneration != generation;
    if (geometryMoved) {
        // A new geometry gets a NEW canvas rather than a resized one, so any
        // view still holding the previous surface keeps valid pixels until it
        // is handed this one.
        auto canvas = std::make_shared<ScreenCanvas>(QSize(width, height));
        if (canvas->isEmpty()) {
            ++m_framesRejected;
            return std::nullopt;
        }
        m_canvas = std::move(canvas);
        m_receivedTileEdge = tileEdge;
        m_currentGeneration = generation;
        m_tileSeen.assign(size_t(columns) * size_t(rows), 0);
        m_tilesSeenCount = 0;
        m_decodeScratch = QImage();
        update.canvasReplaced = true;
        qCDebug(lcScreenShare) << "receiver canvas" << m_canvas->size() << "tiles" << columns << "x"
                               << rows << "generation" << generation;
    }

    m_canvas->beginUpdate();
    qsizetype cursor = ScreenTileEncoder::updateHeaderBytes;
    int applied = 0;
    for (int i = 0; i < tileCount; ++i) {
        if (cursor + ScreenTileEncoder::tileHeaderBytes > payload.size())
            break;
        const auto *entry = reinterpret_cast<const uchar *>(payload.data()) + cursor;
        const int index = qFromBigEndian<quint16>(entry);
        const auto encoding = ScreenTileEncoder::TileEncoding(entry[2]);
        const int length = qFromBigEndian<quint16>(entry + 3);
        cursor += ScreenTileEncoder::tileHeaderBytes;
        if (cursor + length > payload.size())
            break;
        const QByteArrayView body = payload.sliced(cursor, length);
        cursor += length;
        if (index >= columns * rows)
            continue;

        const int column = index % columns;
        const int row = index / columns;
        const int x = column * tileEdge;
        const int y = row * tileEdge;
        const int tileWidth = std::min(tileEdge, width - x);
        const int tileHeight = std::min(tileEdge, height - y);
        if (tileWidth <= 0 || tileHeight <= 0)
            continue;

        if (encoding == ScreenTileEncoder::TileEncoding::Solid) {
            if (length != 3)
                continue;
            const auto *rgb = reinterpret_cast<const uchar *>(body.data());
            const QRgb colour = qRgb(rgb[0], rgb[1], rgb[2]);
            for (int line = 0; line < tileHeight; ++line) {
                auto *pixels = reinterpret_cast<QRgb *>(m_canvas->scanLine(y + line)) + x;
                std::fill(pixels, pixels + tileWidth, colour);
            }
        } else if (encoding == ScreenTileEncoder::TileEncoding::Png
                   || encoding == ScreenTileEncoder::TileEncoding::Jpeg) {
            // Wraps the packet's own bytes: a tile is decoded without first
            // being copied out of the datagram.
            QByteArray bytes = QByteArray::fromRawData(body.data(), body.size());
            QBuffer buffer(&bytes);
            if (!buffer.open(QIODevice::ReadOnly))
                continue;
            QImageReader reader(&buffer,
                                encoding == ScreenTileEncoder::TileEncoding::Png
                                    ? QByteArrayLiteral("png")
                                    : QByteArrayLiteral("jpeg"));
            // The declared size must match the slot before any pixels are
            // allocated: an authenticated peer is still not allowed to hand us
            // a decompression bomb.
            if (reader.size() != QSize(tileWidth, tileHeight))
                continue;
            const bool fullTile = tileWidth == tileEdge && tileHeight == tileEdge;
            QImage edgeTile;
            QImage *target = &edgeTile;
            if (fullTile) {
                if (m_decodeScratch.size() != QSize(tileEdge, tileEdge)
                    || m_decodeScratch.format() != QImage::Format_RGB32) {
                    m_decodeScratch = QImage(tileEdge, tileEdge, QImage::Format_RGB32);
                }
                target = &m_decodeScratch;
            }
            if (!reader.read(target) || target->isNull())
                continue;
            if (target->format() != QImage::Format_RGB32)
                *target = target->convertToFormat(QImage::Format_RGB32);
            for (int line = 0; line < tileHeight; ++line) {
                std::memcpy(m_canvas->scanLine(y + line) + qsizetype(x) * 4,
                            target->constScanLine(line), size_t(tileWidth) * 4);
            }
        } else {
            continue;
        }

        m_canvas->addDirty(QRect(x, y, tileWidth, tileHeight));
        if (m_tileSeen[size_t(index)] == 0) {
            m_tileSeen[size_t(index)] = 1;
            ++m_tilesSeenCount;
        }
        ++applied;
    }

    m_canvas->endUpdate(m_tilesSeenCount >= int(m_tileSeen.size()));
    m_lastAppliedSequence = sequence;
    m_receiving = true;
    ++m_windowFramesApplied;
    ++m_framesReceived;
    m_tilesApplied += quint64(applied);
    m_bytesReceived += quint64(payload.size());
    update.canvas = m_canvas;
    update.dirty = m_canvas->dirtyRect();
    return update;
}

QByteArray CallScreenSession::encodeFeedback(qint64 nowMs)
{
    if (!m_receiving)
        return {};
    if (m_lastReportMs != 0 && nowMs - m_lastReportMs < m_tuning.feedbackIntervalMs)
        return {};
    if (m_feedbackSequence > std::numeric_limits<quint32>::max())
        return {};
    const qint64 interval = m_lastReportMs == 0
        ? m_tuning.feedbackIntervalMs
        : std::clamp<qint64>(nowMs - m_lastReportMs, 1, 65535);

    QByteArray payload;
    payload.reserve(feedbackBytes);
    appendBigEndian32(payload, m_highestSequenceSeen);
    appendBigEndian32(payload, m_windowFramesApplied);
    appendBigEndian32(payload, m_windowBytesReceived);
    appendBigEndian16(payload, quint16(interval));
    appendBigEndian16(payload,
                      quint16(std::clamp(m_viewSize.width(), 0, ScreenTileEncoder::maxCanvasEdge)));
    appendBigEndian16(payload,
                      quint16(std::clamp(m_viewSize.height(), 0, ScreenTileEncoder::maxCanvasEdge)));
    const bool askingForResend = m_resyncNeeded;
    payload.append(char(askingForResend ? 0x01 : 0x00));
    payload.append(char(0));

    CallMediaPacket packet;
    packet.version = wireVersion;
    packet.callId = m_id;
    packet.flags = flagFeedback;
    packet.sequence = quint32(m_feedbackSequence++);
    packet.sealed = m_feedbackSealer.seal(packet.sequence, payload, packet.header());
    if (packet.sealed.isEmpty())
        return {};

    m_lastReportMs = nowMs;
    m_windowFramesApplied = 0;
    m_windowBytesReceived = 0;
    // Asked once. If the resend that answers it is lost too, the gap that
    // creates will raise the request again; repeating it every half second
    // while the answer is still in flight would only restart it.
    if (askingForResend)
        m_resyncNeeded = false;
    return packet.encode();
}

void CallScreenSession::applyFeedback(QByteArrayView payload, qint64 nowMs)
{
    if (payload.size() < feedbackBytes)
        return;
    const auto *data = reinterpret_cast<const uchar *>(payload.data());
    const quint32 highest = qFromBigEndian<quint32>(data);
    const quint32 framesApplied = qFromBigEndian<quint32>(data + 4);
    const int interval = qFromBigEndian<quint16>(data + 12);
    const int viewWidth = qFromBigEndian<quint16>(data + 14);
    const int viewHeight = qFromBigEndian<quint16>(data + 16);
    const quint8 flags = data[18];

    m_remoteView = QSize(viewWidth, viewHeight);
    m_remoteViewHidden = viewWidth <= 0 || viewHeight <= 0;

    const auto &slot = m_sentAt[highest % rttRingSize];
    if (slot.first == highest && slot.second > 0)
        m_rttMs = int(std::clamp<qint64>(nowMs - slot.second, 0, 60'000));

    if ((flags & 0x01) != 0 && m_encoder) {
        // The far end lost its canvas — it reconnected, or the view was rebuilt.
        // Everything it has is stale, so everything goes again.
        m_encoder->requestFullResend();
    }

    // Loss is measured over the range the far end itself covered — the
    // sequences between its previous acknowledgement and this one — not against
    // everything that has been sent. Those two differ by whatever is still on
    // the wire, and on a slow-moving share that in-flight frame can be most of
    // the window: measuring it as loss would walk a perfectly healthy link all
    // the way down the ladder.
    bool measured = false;
    double lossRatio = 0.0;
    if (m_lastAckedSequence && highest > *m_lastAckedSequence) {
        const double expected = double(highest - *m_lastAckedSequence);
        const double delivered = std::min<double>(framesApplied, expected);
        lossRatio = 1.0 - delivered / expected;
        measured = true;
    }
    if (!m_lastAckedSequence || highest > *m_lastAckedSequence)
        m_lastAckedSequence = highest;
    // "Starved" means the encoder actually wanted the whole ceiling: climbing
    // back up is only earned when the link was being asked for something.
    const ScreenShareLevel &level = screenShareLevels().at(size_t(m_desiredLevel));
    const qint64 window = std::max<qint64>(interval, 1);
    const double sentPerSecond = double(m_windowBytesSent) * 1000.0 / double(window);
    const bool starved = sentPerSecond > level.bytesPerSecond * 0.8;

    m_windowBytesSent = 0;
    // A report that covers no new frames says nothing about the link, so it
    // moves nothing.
    if (!measured)
        return;
    m_lossRatio = lossRatio;
    adapt(lossRatio, starved);
}

void CallScreenSession::adapt(double lossRatio, bool starved)
{
    const int lastLevel = int(screenShareLevels().size()) - 1;
    if (lossRatio > lossDegradeThreshold) {
        m_cleanReports = 0;
        if (m_desiredLevel < lastLevel) {
            ++m_desiredLevel;
            qCDebug(lcScreenShare) << "quality down to level" << m_desiredLevel << "loss"
                                   << lossRatio;
        }
        return;
    }
    if (lossRatio > lossCleanThreshold) {
        m_cleanReports = 0;
        return;
    }
    // A still desktop reporting no loss proves nothing about capacity, so only
    // a link that was actually being pushed earns its way back up.
    if (!starved && m_desiredLevel > 0)
        return;
    if (++m_cleanReports >= cleanReportsToRecover && m_desiredLevel > 0) {
        --m_desiredLevel;
        m_cleanReports = 0;
        qCDebug(lcScreenShare) << "quality up to level" << m_desiredLevel;
    }
}

ScreenShareStats CallScreenSession::stats() const
{
    ScreenShareStats merged = m_encoder ? m_encoder->stats() : ScreenShareStats{};
    merged.lossRatio = m_lossRatio;
    merged.rttMs = m_rttMs;
    merged.remoteViewSize = m_remoteView;
    merged.framesReceived = m_framesReceived;
    merged.tilesApplied = m_tilesApplied;
    merged.bytesReceived = m_bytesReceived;
    merged.framesRejected = m_framesRejected;
    return merged;
}

} // namespace OpenChat
