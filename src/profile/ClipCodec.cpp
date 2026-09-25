#include "profile/ClipCodec.h"

#include <opus.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#if OPENCHAT_HAVE_VPX
#    include <vpx/vp8cx.h>
#    include <vpx/vp8dx.h>
#    include <vpx/vpx_decoder.h>
#    include <vpx/vpx_encoder.h>
#endif

namespace OpenChat {

namespace {

constexpr int sampleRate = ClipContainer::audioSampleRate;
constexpr int packetSamples = ClipContainer::audioFrameSamples;

[[nodiscard]] quint8 clampByte(int value) noexcept
{
    return quint8(std::clamp(value, 0, 255));
}

// BT.601 studio range, both ways: only this codec writes and reads it.
struct I420 final {
    int width = 0, height = 0;
    std::vector<quint8> y, u, v;
};

[[nodiscard]] I420 toI420(const QImage &source)
{
    const QImage image = source.format() == QImage::Format_RGB32 ? source
                                                                 : source.convertToFormat(QImage::Format_RGB32);
    I420 out;
    out.width = image.width();
    out.height = image.height();
    const int cw = (out.width + 1) / 2, ch = (out.height + 1) / 2;
    out.y.resize(size_t(out.width) * size_t(out.height));
    out.u.resize(size_t(cw) * size_t(ch));
    out.v.resize(size_t(cw) * size_t(ch));
    for (int row = 0; row < out.height; ++row) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(row));
        quint8 *y = out.y.data() + size_t(row) * size_t(out.width);
        for (int x = 0; x < out.width; ++x) {
            const int r = qRed(line[x]), g = qGreen(line[x]), b = qBlue(line[x]);
            y[x] = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int cy = 0; cy < ch; ++cy) {
        const int r0 = 2 * cy, r1 = std::min(2 * cy + 1, out.height - 1);
        const auto *a = reinterpret_cast<const QRgb *>(image.constScanLine(r0));
        const auto *b = reinterpret_cast<const QRgb *>(image.constScanLine(r1));
        for (int cx = 0; cx < cw; ++cx) {
            const int x0 = 2 * cx, x1 = std::min(2 * cx + 1, out.width - 1);
            const QRgb p[4] = {a[x0], a[x1], b[x0], b[x1]};
            int r = 0, g = 0, bl = 0;
            for (const QRgb pixel : p) {
                r += qRed(pixel);
                g += qGreen(pixel);
                bl += qBlue(pixel);
            }
            r = (r + 2) / 4;
            g = (g + 2) / 4;
            bl = (bl + 2) / 4;
            out.u[size_t(cy) * size_t(cw) + size_t(cx)] = clampByte(((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128);
            out.v[size_t(cy) * size_t(cw) + size_t(cx)] = clampByte(((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128);
        }
    }
    return out;
}

[[nodiscard]] QImage fromI420(const quint8 *yPlane, int yStride, const quint8 *uPlane, int uStride,
                              const quint8 *vPlane, int vStride, int width, int height)
{
    QImage out(width, height, QImage::Format_RGB32);
    if (out.isNull())
        return out;
    for (int row = 0; row < height; ++row) {
        const quint8 *y = yPlane + qsizetype(row) * yStride;
        const quint8 *u = uPlane + qsizetype(row / 2) * uStride;
        const quint8 *v = vPlane + qsizetype(row / 2) * vStride;
        auto *target = reinterpret_cast<QRgb *>(out.scanLine(row));
        for (int x = 0; x < width; ++x) {
            const int c = 298 * (int(y[x]) - 16);
            const int d = int(u[x / 2]) - 128;
            const int e = int(v[x / 2]) - 128;
            target[x] = qRgb(clampByte((c + 409 * e + 128) >> 8), clampByte((c - 100 * d - 208 * e + 128) >> 8),
                             clampByte((c + 516 * d + 128) >> 8));
        }
    }
    return out;
}

// One output sample frame at 48 kHz from `pcm` (linear interpolation),
// folded into `channels` (1 or 2); past the end of the source, silence.
void sampleAt(const WavAudio &pcm, double position, int channels, float *out)
{
    const qsizetype frames = pcm.frameCount();
    const qsizetype index = qsizetype(position);
    const double t = position - double(index);
    const auto frameValue = [&](qsizetype frame, int channel) -> float {
        if (frame < 0 || frame >= frames)
            return 0.0F;
        return float(pcm.samples.at(frame * pcm.channels + channel)) / 32768.0F;
    };
    for (int c = 0; c < channels; ++c) {
        float sum = 0.0F;
        int count = 0;
        // Mono takes every source channel; stereo takes the even ones left
        // and the odd ones right (a mono source is never folded to stereo).
        for (int source = 0; source < pcm.channels; ++source) {
            if (channels == 2 && source % 2 != c)
                continue;
            const float a = frameValue(index, source);
            const float b = frameValue(index + 1, source);
            sum += a + float(t) * (b - a);
            ++count;
        }
        out[c] = count > 0 ? sum / float(count) : 0.0F;
    }
}

struct OpusEncoderDeleter final {
    void operator()(OpusEncoder *encoder) const noexcept { opus_encoder_destroy(encoder); }
};

} // namespace

bool clipCodecAvailable()
{
    return OPENCHAT_HAVE_VPX != 0;
}

QSize clipFrameSize(QSize source, int longSide)
{
    if (source.isEmpty())
        return {};
    QSize size = source;
    const int longest = std::max(size.width(), size.height());
    const int limit = std::min(longSide, ClipContainer::maxDimension);
    if (longest > limit)
        size = size.scaled(limit, limit, Qt::KeepAspectRatio);
    const auto even = [](int side) {
        return std::clamp(side - side % 2, ClipContainer::minDimension, ClipContainer::maxDimension);
    };
    return {even(size.width()), even(size.height())};
}

std::optional<ClipAudio> encodeClipAudio(const WavAudio &pcm, qint64 durationMs, int bitrate)
{
    if (pcm.channels <= 0 || pcm.sampleRate <= 0 || pcm.frameCount() == 0)
        return ClipAudio{}; // a silent clip
    if (durationMs <= 0)
        return std::nullopt;
    const int channels = pcm.channels == 1 ? 1 : 2;
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, OpusEncoderDeleter> encoder(
        opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, &error));
    if (!encoder || error != OPUS_OK)
        return std::nullopt;
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    if (lookahead < 0 || lookahead > ClipContainer::maxPreSkip)
        return std::nullopt;

    ClipAudio audio;
    audio.channels = channels;
    audio.preSkip = int(lookahead);
    const qint64 samples = durationMs * sampleRate / 1000;
    // The encoder's output lags by its look-ahead: feeding that much more
    // (silence past the end) brings every real sample out.
    const qint64 packets = (samples + audio.preSkip + packetSamples - 1) / packetSamples;
    const double step = double(pcm.sampleRate) / double(sampleRate);
    std::vector<float> frame(size_t(packetSamples) * size_t(channels));
    unsigned char packet[ClipContainer::maxPacketBytes];
    for (qint64 p = 0; p < packets; ++p) {
        for (int i = 0; i < packetSamples; ++i) {
            const qint64 at = p * packetSamples + i;
            float *out = frame.data() + size_t(i) * size_t(channels);
            if (at < samples)
                sampleAt(pcm, double(at) * step, channels, out);
            else
                std::fill(out, out + channels, 0.0F);
        }
        const opus_int32 bytes = opus_encode_float(encoder.get(), frame.data(), packetSamples, packet,
                                                   ClipContainer::maxPacketBytes);
        if (bytes <= 0)
            return std::nullopt;
        audio.packets.push_back(QByteArray(reinterpret_cast<const char *>(packet), bytes));
    }
    return audio;
}

std::optional<QVector<ClipContainer::Frame>>
encodeClipVideo(const QVector<QImage> &frames, int fps, const QVector<int> &kbps, qsizetype maxBytes,
                const std::function<bool()> &cancelled)
{
#if OPENCHAT_HAVE_VPX
    if (frames.isEmpty() || fps < 1 || fps > ClipContainer::maxFps)
        return std::nullopt;
    const int width = frames.first().width();
    const int height = frames.first().height();
    if (width % 2 != 0 || height % 2 != 0 || width < ClipContainer::minDimension
        || height < ClipContainer::minDimension || width > ClipContainer::maxDimension
        || height > ClipContainer::maxDimension)
        return std::nullopt;
    std::vector<I420> pictures;
    pictures.reserve(size_t(frames.size()));
    for (const QImage &frame : frames) {
        if (frame.size() != QSize(width, height))
            return std::nullopt;
        pictures.push_back(toI420(frame));
    }

    for (const int rate : kbps) {
        vpx_codec_enc_cfg_t config;
        if (vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK)
            return std::nullopt;
        config.g_w = unsigned(width);
        config.g_h = unsigned(height);
        config.g_timebase.num = 1;
        config.g_timebase.den = fps;
        config.g_threads = 4;
        config.g_lag_in_frames = 0; // one packet per picture, in order
        config.g_error_resilient = 0;
        config.rc_end_usage = VPX_VBR;
        config.rc_target_bitrate = unsigned(rate);
        config.rc_min_quantizer = 4;
        config.rc_max_quantizer = 60;
        config.rc_dropframe_thresh = 0; // never drop: frame i is shown at i / fps
        config.kf_mode = VPX_KF_DISABLED; // one keyframe, the first
        vpx_codec_ctx_t codec;
        if (vpx_codec_enc_init(&codec, vpx_codec_vp9_cx(), &config, 0) != VPX_CODEC_OK)
            return std::nullopt;
        vpx_codec_control(&codec, VP8E_SET_CPUUSED, 5);
        vpx_codec_control(&codec, VP9E_SET_ROW_MT, 1);
        vpx_codec_control(&codec, VP9E_SET_AQ_MODE, 0);

        QVector<ClipContainer::Frame> encoded;
        qsizetype total = 0;
        bool failed = false;
        const auto collect = [&] {
            vpx_codec_iter_t iterator = nullptr;
            while (const vpx_codec_cx_pkt_t *packet = vpx_codec_get_cx_data(&codec, &iterator)) {
                if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
                    continue;
                const auto *data = static_cast<const char *>(packet->data.frame.buf);
                const qsizetype size = qsizetype(packet->data.frame.sz);
                encoded.push_back({(packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0, QByteArray(data, size)});
                total += size;
            }
        };
        for (size_t i = 0; i < pictures.size() && !failed; ++i) {
            if (cancelled && cancelled()) {
                vpx_codec_destroy(&codec);
                return std::nullopt;
            }
            I420 &picture = pictures[i];
            vpx_image_t image;
            if (!vpx_img_wrap(&image, VPX_IMG_FMT_I420, unsigned(width), unsigned(height), 1, picture.y.data())) {
                failed = true;
                break;
            }
            image.planes[VPX_PLANE_Y] = picture.y.data();
            image.planes[VPX_PLANE_U] = picture.u.data();
            image.planes[VPX_PLANE_V] = picture.v.data();
            image.stride[VPX_PLANE_Y] = width;
            image.stride[VPX_PLANE_U] = (width + 1) / 2;
            image.stride[VPX_PLANE_V] = (width + 1) / 2;
            const vpx_enc_frame_flags_t flags = i == 0 ? VPX_EFLAG_FORCE_KF : 0;
            if (vpx_codec_encode(&codec, &image, vpx_codec_pts_t(i), 1, flags, VPX_DL_GOOD_QUALITY) != VPX_CODEC_OK)
                failed = true;
            else
                collect();
        }
        if (!failed && vpx_codec_encode(&codec, nullptr, 0, 1, 0, VPX_DL_GOOD_QUALITY) == VPX_CODEC_OK)
            collect();
        vpx_codec_destroy(&codec);
        if (failed)
            return std::nullopt;
        if (encoded.size() != frames.size() || !encoded.first().key)
            continue; // libvpx held a picture back: try the next rate
        if (total <= maxBytes)
            return encoded;
    }
    return std::nullopt;
#else
    Q_UNUSED(frames);
    Q_UNUSED(fps);
    Q_UNUSED(kbps);
    Q_UNUSED(maxBytes);
    Q_UNUSED(cancelled);
    return std::nullopt;
#endif
}

QVector<QByteArray> packClip(const QVector<QVector<ClipContainer::Frame>> &video, const QVector<qint64> &durationsMs,
                             QSize size, int fps, const ClipAudio &audio, qsizetype maxBytes)
{
    if (video.isEmpty() || video.size() != durationsMs.size())
        return {};
    // Where the stream's packets are cut: the packets covering the pre-skip
    // and every sample up to each segment's end.
    const auto packetsThrough = [&](qint64 endMs) {
        const qint64 samples = endMs * sampleRate / 1000;
        return (samples + audio.preSkip + packetSamples - 1) / packetSamples;
    };
    QVector<QByteArray> segments;
    qint64 elapsedMs = 0;
    qint64 firstPacket = 0;
    for (qsizetype k = 0; k < video.size(); ++k) {
        ClipContainer clip;
        clip.width = size.width();
        clip.height = size.height();
        clip.fps = fps;
        clip.durationMs = durationsMs.at(k);
        clip.frames = video.at(k);
        elapsedMs += clip.durationMs;
        if (audio.channels > 0) {
            clip.audioChannels = audio.channels;
            clip.audioPreSkip = k == 0 ? audio.preSkip : 0;
            const qint64 lastPacket =
                k + 1 == video.size() ? qint64(audio.packets.size()) : packetsThrough(elapsedMs);
            if (lastPacket > audio.packets.size() || lastPacket < firstPacket)
                return {};
            clip.audioPackets = audio.packets.mid(firstPacket, lastPacket - firstPacket);
            firstPacket = lastPacket;
        }
        QByteArray bytes = encodeClipContainer(clip);
        if (bytes.isEmpty() || bytes.size() > maxBytes)
            return {};
        segments.push_back(std::move(bytes));
    }
    return segments;
}

std::optional<SongContainer> clipSoundtrack(const QVector<ClipContainer> &segments)
{
    return clipSoundtrack(segments, SongContainerLimits{});
}

std::optional<SongContainer> clipSoundtrack(const QVector<ClipContainer> &segments, const SongContainerLimits &limits)
{
    if (segments.isEmpty() || segments.first().audioChannels == 0)
        return std::nullopt;
    SongContainer song;
    song.channels = segments.first().audioChannels;
    song.frameSamples = packetSamples;
    song.preSkip = segments.first().audioPreSkip;
    song.gainQ8 = 0;
    for (qsizetype k = 0; k < segments.size(); ++k) {
        const ClipContainer &segment = segments.at(k);
        if (segment.audioChannels != song.channels || (k > 0 && segment.audioPreSkip != 0))
            return std::nullopt;
        song.totalSamples += segment.audioSamples();
        song.packets += segment.audioPackets;
    }
    const qint64 minimum = (song.totalSamples + song.preSkip + packetSamples - 1) / packetSamples;
    if (song.totalSamples > limits.maxTotalSamples || song.packets.size() < minimum
        || song.packets.size() > minimum + 1)
        return std::nullopt;
    return song;
}

// ---------------------------------------------------------------------------

#if OPENCHAT_HAVE_VPX
namespace {

// Reads a VP9 uncompressed header's bits, most significant first.
class BitReader final
{
public:
    explicit BitReader(QByteArrayView bytes) : m_bytes(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    quint32 read(int bits)
    {
        quint32 value = 0;
        for (int bit = 0; bit < bits; ++bit) {
            if (m_position >= m_bytes.size() * 8) {
                m_ok = false;
                return 0;
            }
            const auto byte = quint8(m_bytes[m_position / 8]);
            value = (value << 1) | ((byte >> (7 - m_position % 8)) & 1);
            ++m_position;
        }
        return value;
    }

private:
    QByteArrayView m_bytes;
    qsizetype m_position = 0;
    bool m_ok = true;
};

// Whether a VP9 frame's header parses (VP9 bitstream spec, 6.2) and every
// size it codes itself fits `maxSide` a side. A frame that takes its size
// from a reference is fine: every reference was such a frame. libvpx sizes
// its buffers from the header before anything checks them against the
// container, so this runs first.
[[nodiscard]] bool vp9FrameFits(QByteArrayView frame, int maxSide)
{
    BitReader bits(frame);
    if (bits.read(2) != 2) // frame_marker
        return false;
    int profile = int(bits.read(1));
    profile |= int(bits.read(1)) << 1;
    if (profile == 3 && bits.read(1) != 0)
        return false;
    if (bits.read(1) == 1) // show_existing_frame: no new picture
        return bits.ok();
    const bool keyFrame = bits.read(1) == 0;
    const bool showFrame = bits.read(1) == 1;
    const bool errorResilient = bits.read(1) == 1;
    const auto syncCode = [&] { return bits.read(24) == 0x498342; };
    const auto colorConfig = [&] {
        if (profile >= 2)
            (void)bits.read(1); // ten_or_twelve_bit
        const quint32 colorSpace = bits.read(3);
        if (colorSpace != 7) { // not RGB
            (void)bits.read(1); // color_range
            if (profile == 1 || profile == 3) {
                (void)bits.read(2); // subsampling_x, subsampling_y
                return bits.read(1) == 0; // reserved_zero
            }
        } else if (profile == 1 || profile == 3) {
            return bits.read(1) == 0; // reserved_zero
        }
        return true;
    };
    const auto sizeFits = [&] {
        const quint32 width = bits.read(16) + 1;
        const quint32 height = bits.read(16) + 1;
        return bits.ok() && width <= quint32(maxSide) && height <= quint32(maxSide);
    };
    if (keyFrame)
        return syncCode() && colorConfig() && sizeFits();
    const bool intraOnly = !showFrame && bits.read(1) == 1;
    if (!errorResilient)
        (void)bits.read(2); // reset_frame_context
    if (intraOnly) {
        if (!syncCode() || (profile > 0 && !colorConfig()))
            return false;
        (void)bits.read(8); // refresh_frame_flags
        return sizeFits();
    }
    (void)bits.read(8);  // refresh_frame_flags
    (void)bits.read(12); // three ref_frame_idx and sign_bias
    for (int reference = 0; reference < 3; ++reference) {
        if (bits.read(1) == 1) // found_ref: that reference's size
            return bits.ok();
    }
    return sizeFits();
}

// The packet as libvpx should decode it, or nothing when any frame in it
// does not fit. Without a superframe index libvpx decodes one frame after
// another until the data ends, and where one ends only the decoder knows;
// such a packet gets an index naming it as one frame, so the frame checked
// here is the only one decoded.
[[nodiscard]] std::optional<QByteArray> vp9PacketToDecode(const QByteArray &packet, int maxSide)
{
    const qsizetype size = packet.size();
    const auto marker = quint8(packet.back());
    if ((marker & 0xE0) == 0xC0) {
        const int frames = (marker & 0x07) + 1;
        const int magnitude = ((marker >> 3) & 0x03) + 1;
        const qsizetype indexBytes = 2 + qsizetype(magnitude) * frames;
        if (size >= indexBytes && quint8(packet[size - indexBytes]) == marker) {
            qsizetype start = 0;
            for (int frame = 0; frame < frames; ++frame) {
                qint64 frameBytes = 0;
                for (int byte = 0; byte < magnitude; ++byte)
                    frameBytes |= qint64(quint8(packet[size - indexBytes + 1 + frame * magnitude + byte])) << (8 * byte);
                if (frameBytes < 1 || frameBytes > size - indexBytes - start
                    || !vp9FrameFits(QByteArrayView(packet).sliced(start, frameBytes), maxSide))
                    return std::nullopt;
                start += frameBytes;
            }
            return packet;
        }
    }
    if (!vp9FrameFits(packet, maxSide) || size > qsizetype(std::numeric_limits<quint32>::max()))
        return std::nullopt;
    QByteArray indexed = packet;
    constexpr quint8 oneFrameOfFourByteSize = 0xC0 | (3 << 3);
    indexed.append(char(oneFrameOfFourByteSize));
    for (int byte = 0; byte < 4; ++byte)
        indexed.append(char((quint32(size) >> (8 * byte)) & 0xFF));
    indexed.append(char(oneFrameOfFourByteSize));
    return indexed;
}

} // namespace
#endif

struct ClipVideoDecoder::State final {
#if OPENCHAT_HAVE_VPX
    vpx_codec_ctx_t codec{};
#endif
    bool valid = false;
};

ClipVideoDecoder::ClipVideoDecoder()
    : m_state(std::make_unique<State>())
{
    reset();
}

ClipVideoDecoder::~ClipVideoDecoder()
{
#if OPENCHAT_HAVE_VPX
    if (m_state->valid)
        vpx_codec_destroy(&m_state->codec);
#endif
}

bool ClipVideoDecoder::isValid() const noexcept
{
    return m_state->valid;
}

void ClipVideoDecoder::reset()
{
#if OPENCHAT_HAVE_VPX
    if (m_state->valid)
        vpx_codec_destroy(&m_state->codec);
    vpx_codec_dec_cfg_t config{};
    config.threads = 2;
    m_state->valid = vpx_codec_dec_init(&m_state->codec, vpx_codec_vp9_dx(), &config, 0) == VPX_CODEC_OK;
#endif
}

QImage ClipVideoDecoder::decode(const ClipContainer::Frame &frame, bool wantPicture)
{
#if OPENCHAT_HAVE_VPX
    if (!m_state->valid || frame.data.isEmpty())
        return {};
    // A frame may code a size far past its container's (8192 px square
    // costs hundreds of MiB inside libvpx), so the header is read first.
    const auto packet = vp9PacketToDecode(frame.data, ClipContainer::maxDimension);
    if (!packet)
        return {};
    if (vpx_codec_decode(&m_state->codec, reinterpret_cast<const uint8_t *>(packet->constData()),
                         unsigned(packet->size()), nullptr, 0)
        != VPX_CODEC_OK)
        return {};
    vpx_codec_iter_t iterator = nullptr;
    vpx_image_t *decoded = nullptr;
    while (vpx_image_t *image = vpx_codec_get_frame(&m_state->codec, &iterator))
        decoded = image;
    if (!wantPicture || decoded == nullptr || decoded->fmt != VPX_IMG_FMT_I420
        || decoded->d_w > unsigned(ClipContainer::maxDimension) || decoded->d_h > unsigned(ClipContainer::maxDimension))
        return {};
    return fromI420(decoded->planes[VPX_PLANE_Y], decoded->stride[VPX_PLANE_Y], decoded->planes[VPX_PLANE_U],
                    decoded->stride[VPX_PLANE_U], decoded->planes[VPX_PLANE_V], decoded->stride[VPX_PLANE_V],
                    int(decoded->d_w), int(decoded->d_h));
#else
    Q_UNUSED(frame);
    Q_UNUSED(wantPicture);
    return {};
#endif
}

} // namespace OpenChat
