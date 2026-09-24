#include "media/SongCodec.h"

#include "media/AudioConvert.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace OpenChat {

namespace {

using EncodeResult = Result<QByteArray, SongEncodeError>;

constexpr int songRate = SongContainer::sampleRate;
// 60 ms, Opus's longest single frame: the fewest packets, so the least
// per-packet overhead in a budget where every byte counts.
constexpr int songFrameSamples = 2880;
// The fixed part of the container (SongContainer.h), for the running size
// that lets a rung that cannot fit stop early; the final check is exact.
constexpr qsizetype containerHeaderBytes = 26;
constexpr qsizetype packetLengthBytes = 2;

// Loudness is measured the way ITU-R BS.1770 gates it (400 ms blocks every
// 100 ms, an absolute and a relative gate) but without the K-weighting
// filter: the result only has to put songs at a similar level, not match a
// broadcast meter.
constexpr int loudnessBlockMs = 400;
constexpr int loudnessHopMs = 100;
constexpr int hopsPerBlock = loudnessBlockMs / loudnessHopMs;
constexpr double absoluteGateDb = -70.0;
constexpr double relativeGateDb = -10.0;

constexpr int audibleBlockMs = 10;

[[nodiscard]] double dbToAmplitude(double db)
{
    return std::pow(10.0, db / 20.0);
}

[[nodiscard]] double dbToPower(double db)
{
    return std::pow(10.0, db / 10.0);
}

[[nodiscard]] double powerToDb(double power)
{
    return power > 0.0 ? 10.0 * std::log10(power) : -std::numeric_limits<double>::infinity();
}

[[nodiscard]] qint16 toS16(double value)
{
    const double scaled = std::round(value);
    if (!std::isfinite(scaled))
        return 0;
    return static_cast<qint16>(std::clamp(scaled, -32768.0, 32767.0));
}

// A raised-cosine ramp from 0 (t = 0) to 1 (t = 1): no corner at either end,
// so neither the fade's start nor its end is audible as a click.
[[nodiscard]] double fadeCurve(double t)
{
    return 0.5 - 0.5 * std::cos(std::numbers::pi * std::clamp(t, 0.0, 1.0));
}

struct EncoderDeleter final {
    void operator()(OpusEncoder *encoder) const noexcept { opus_encoder_destroy(encoder); }
};
using EncoderPtr = std::unique_ptr<OpusEncoder, EncoderDeleter>;

// The clip's first `frames` frames as one plane per song channel: mono stays
// mono, everything else becomes left and right.
[[nodiscard]] QVector<QVector<qint16>> songPlanes(const WavAudio &pcm, qsizetype frames)
{
    const int in = pcm.channels;
    const int out = in == 1 ? 1 : 2;
    QVector<QVector<qint16>> planes(out);
    for (QVector<qint16> &plane : planes)
        plane.resize(frames);
    const qint16 *samples = pcm.samples.constData();
    if (in <= 2) {
        for (qsizetype frame = 0; frame < frames; ++frame) {
            for (int channel = 0; channel < out; ++channel)
                planes[channel][frame] = samples[frame * in + channel];
        }
        return planes;
    }
    // A WAV rarely says what its third channel onwards are (a centre, an LFE,
    // surrounds), so they fold into both sides at -3 dB, the way a centre
    // channel folds down, and the sum is scaled so it can never clip.
    constexpr double extraWeight = 1.0 / std::numbers::sqrt2;
    constexpr double scale = 1.0 / (1.0 + extraWeight);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const qint16 *row = samples + frame * in;
        double extra = 0.0;
        for (int channel = 2; channel < in; ++channel)
            extra += row[channel];
        extra = extra / (in - 2) * extraWeight;
        planes[0][frame] = toS16((row[0] + extra) * scale);
        planes[1][frame] = toS16((row[1] + extra) * scale);
    }
    return planes;
}

// Per-100 ms sums of a per-frame power, from which every overlapping 400 ms
// block is four additions; holding sums per hop instead of a prefix per frame
// keeps a 45 s song's measurement to a few kilobytes.
struct PowerHops final {
    std::vector<double> mid; // power of the mid signal (L+R)/2
    std::vector<double> all; // power averaged over the channels
    qsizetype hopFrames = 0;
    qsizetype frames = 0;
};

[[nodiscard]] PowerHops measurePower(const std::vector<float> &audio, int channels, qsizetype frames)
{
    PowerHops hops;
    hops.hopFrames = qsizetype(songRate) * loudnessHopMs / 1000;
    hops.frames = frames;
    const qsizetype count = (frames + hops.hopFrames - 1) / hops.hopFrames;
    hops.mid.assign(count, 0.0);
    hops.all.assign(count, 0.0);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        const float *row = audio.data() + frame * channels;
        double sum = 0.0;
        double power = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += row[channel];
            power += double(row[channel]) * row[channel];
        }
        const double mid = sum / channels;
        const qsizetype hop = frame / hops.hopFrames;
        hops.mid[hop] += mid * mid;
        hops.all[hop] += power / channels;
    }
    return hops;
}

// Mean power of each 400 ms block (hops 75% overlapped); one block over the
// whole clip when it is shorter than a block.
[[nodiscard]] std::vector<double> blockPowers(const std::vector<double> &hopSums, const PowerHops &hops)
{
    const qsizetype blockFrames = hops.hopFrames * hopsPerBlock;
    std::vector<double> blocks;
    if (hops.frames < blockFrames) {
        double sum = 0.0;
        for (const double hop : hopSums)
            sum += hop;
        if (hops.frames > 0)
            blocks.push_back(sum / double(hops.frames));
        return blocks;
    }
    const qsizetype fullHops = hops.frames / hops.hopFrames;
    for (qsizetype first = 0; first + hopsPerBlock <= fullHops; ++first) {
        double sum = 0.0;
        for (int hop = 0; hop < hopsPerBlock; ++hop)
            sum += hopSums[first + hop];
        blocks.push_back(sum / double(blockFrames));
    }
    return blocks;
}

// Gated loudness in dBFS (0 dBFS = the power of a full-scale square wave),
// or nullopt when no block passes the absolute gate.
[[nodiscard]] std::optional<double> gatedLoudnessDb(const std::vector<double> &blocks)
{
    const double absoluteGate = dbToPower(absoluteGateDb);
    double sum = 0.0;
    qsizetype count = 0;
    for (const double block : blocks) {
        if (block >= absoluteGate) {
            sum += block;
            ++count;
        }
    }
    if (count == 0)
        return std::nullopt;
    const double gate = std::max(absoluteGate, sum / double(count) * dbToPower(relativeGateDb));
    sum = 0.0;
    count = 0;
    for (const double block : blocks) {
        if (block >= gate) {
            sum += block;
            ++count;
        }
    }
    return powerToDb(sum / double(count));
}

// Encodes the conditioned audio at each rung of `ladder` until one fits
// options.maxBytes.
[[nodiscard]] EncodeResult encodeLadder(const std::vector<float> &audio, int channels, qint64 totalSamples,
                                        const QVector<int> &ladder, const SongEncodeOptions &options,
                                        const std::function<bool()> &isCancelled)
{
    int error = OPUS_OK;
    EncoderPtr encoder(opus_encoder_create(songRate, channels, OPUS_APPLICATION_AUDIO, &error));
    if (error != OPUS_OK || encoder == nullptr)
        return EncodeResult::failure(SongEncodeError::EncoderUnavailable);

    std::vector<float> frame(std::size_t(songFrameSamples) * channels);
    std::array<unsigned char, SongContainer::maxPacketBytes> packet{};
    for (const int bitrate : ladder) {
        opus_encoder_ctl(encoder.get(), OPUS_RESET_STATE);
        opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate));
        // Constrained VBR: the average holds to the bitrate, so the size of a
        // 45 s song is predictable, while hard passages still borrow a little.
        opus_encoder_ctl(encoder.get(), OPUS_SET_VBR(1));
        opus_encoder_ctl(encoder.get(), OPUS_SET_VBR_CONSTRAINT(1));
        opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(10));
        opus_encoder_ctl(encoder.get(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_int32 lookahead = 0;
        opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));

        SongContainer song;
        song.channels = channels;
        song.frameSamples = songFrameSamples;
        // The decoder's output lags the input by the encoder's look-ahead;
        // the player drops that much, so feeding the look-ahead's worth of
        // zeros past the end is what gets the last real samples out.
        song.preSkip = int(lookahead);
        song.totalSamples = totalSamples;
        song.gainQ8 = 0;
        const qint64 packetCount = (totalSamples + song.preSkip + songFrameSamples - 1) / songFrameSamples;
        song.packets.reserve(packetCount);

        qsizetype bytes = containerHeaderBytes;
        bool fits = true;
        for (qint64 index = 0; index < packetCount; ++index) {
            if (isCancelled())
                return EncodeResult::failure(SongEncodeError::Cancelled);
            const qint64 first = index * songFrameSamples;
            const qint64 available = std::clamp<qint64>(totalSamples - first, 0, songFrameSamples);
            std::fill(frame.begin(), frame.end(), 0.0F);
            if (available > 0) {
                std::copy_n(audio.begin() + first * channels, available * channels, frame.begin());
            }
            const opus_int32 written = opus_encode_float(encoder.get(), frame.data(), songFrameSamples,
                                                         packet.data(), opus_int32(packet.size()));
            if (written < 1)
                return EncodeResult::failure(SongEncodeError::EncoderUnavailable);
            bytes += packetLengthBytes + written;
            if (bytes > options.maxBytes) {
                fits = false; // this rung cannot fit: no point finishing it
                break;
            }
            song.packets.push_back(QByteArray(reinterpret_cast<const char *>(packet.data()), written));
        }
        if (!fits)
            continue;
        QByteArray encoded = encodeSongContainer(song);
        if (encoded.isEmpty())
            return EncodeResult::failure(SongEncodeError::EncoderUnavailable);
        if (encoded.size() <= options.maxBytes)
            return EncodeResult::success(std::move(encoded));
    }
    return EncodeResult::failure(SongEncodeError::TooLarge);
}

} // namespace

EncodeResult encodeSong(const SongClip &clip, const SongEncodeOptions &options,
                        const std::function<bool()> &cancelled)
{
    const auto isCancelled = [&cancelled] { return cancelled && cancelled(); };
    const WavAudio &pcm = clip.pcm;
    if (pcm.channels <= 0 || pcm.sampleRate <= 0 || pcm.frameCount() == 0)
        return EncodeResult::failure(SongEncodeError::TooShort);

    // The container cannot hold more than SongContainer::maxDurationMs, so a
    // larger option is capped. Cutting the clip here cuts through the song,
    // so its end gets the long fade.
    const qint64 maxDurationMs = std::clamp<qint64>(options.maxDurationMs, 0, SongContainer::maxDurationMs);
    const qint64 maxSourceFrames = qint64(pcm.sampleRate) * maxDurationMs / 1000;
    qsizetype frames = pcm.frameCount();
    bool trimmedEnd = clip.trimmedEnd;
    if (frames > maxSourceFrames) {
        frames = maxSourceFrames;
        trimmedEnd = true;
    }
    if (frames * 1000 < qint64(options.minDurationMs) * pcm.sampleRate || frames == 0)
        return EncodeResult::failure(SongEncodeError::TooShort);

    QVector<QVector<qint16>> planes = songPlanes(pcm, frames);
    for (QVector<qint16> &plane : planes)
        plane = AudioConvert::resampleMono(plane, pcm.sampleRate, songRate);
    const int channels = int(planes.size());
    qint64 total = qint64(SongContainer::maxDurationMs) * songRate / 1000;
    for (const QVector<qint16> &plane : planes)
        total = std::min<qint64>(total, plane.size());
    if (total < 1)
        return EncodeResult::failure(SongEncodeError::TooShort);
    if (isCancelled())
        return EncodeResult::failure(SongEncodeError::Cancelled);

    std::vector<float> audio(std::size_t(total) * channels);
    for (int channel = 0; channel < channels; ++channel) {
        const qint16 *plane = planes[channel].constData();
        for (qint64 frame = 0; frame < total; ++frame)
            audio[std::size_t(frame * channels + channel)] = float(plane[frame]) / 32768.0F;
    }
    planes.clear();

    const PowerHops hops = measurePower(audio, channels, qsizetype(total));
    const std::vector<double> allBlocks = blockPowers(hops.all, hops);
    const double loudest = allBlocks.empty() ? 0.0 : *std::max_element(allBlocks.begin(), allBlocks.end());
    if (loudest < dbToPower(options.silenceThresholdDb))
        return EncodeResult::failure(SongEncodeError::Silent);
    // Measured on the mid signal as specified; a mix whose sides cancel in
    // the middle (antiphase) falls back to the average of its channels.
    std::optional<double> loudness = gatedLoudnessDb(blockPowers(hops.mid, hops));
    if (!loudness)
        loudness = gatedLoudnessDb(allBlocks);
    if (!loudness)
        return EncodeResult::failure(SongEncodeError::Silent);

    float peak = 0.0F;
    for (const float sample : audio)
        peak = std::max(peak, std::abs(sample));
    double gainDb = std::clamp(options.targetLoudnessDb - *loudness, options.minGainDb, options.maxGainDb);
    // The peak ceiling wins over the loudness target: a spiky quiet song is
    // left quieter rather than clipped.
    const double peakDb = 20.0 * std::log10(double(std::max(peak, 1e-9F)));
    if (peakDb + gainDb > options.peakCeilingDb)
        gainDb = options.peakCeilingDb - peakDb;
    const double gain = dbToAmplitude(gainDb);

    const qint64 fadeInFrames =
        std::min<qint64>(total, qint64(clip.trimmedStart ? options.trimFadeMs : options.edgeFadeMs) * songRate / 1000);
    const qint64 fadeOutFrames =
        std::min<qint64>(total, qint64(trimmedEnd ? options.trimFadeMs : options.edgeFadeMs) * songRate / 1000);
    for (qint64 frame = 0; frame < total; ++frame) {
        double frameGain = gain;
        if (frame < fadeInFrames)
            frameGain *= fadeCurve(double(frame) / double(fadeInFrames));
        if (const qint64 fromEnd = total - 1 - frame; fromEnd < fadeOutFrames)
            frameGain *= fadeCurve(double(fromEnd) / double(fadeOutFrames));
        float *row = audio.data() + frame * channels;
        for (int channel = 0; channel < channels; ++channel)
            row[channel] = float(row[channel] * frameGain);
    }
    if (isCancelled())
        return EncodeResult::failure(SongEncodeError::Cancelled);

    const QVector<int> &ladder = channels == 1 ? options.monoBitrates : options.stereoBitrates;
    if (ladder.isEmpty())
        return EncodeResult::failure(SongEncodeError::TooLarge);
    return encodeLadder(audio, channels, total, ladder, options, isCancelled);
}

std::optional<qint64> findAudibleMs(const WavAudio &pcm, double silenceThresholdDb, qsizetype fromFrame,
                                    qsizetype toFrame)
{
    if (pcm.channels <= 0 || pcm.sampleRate <= 0)
        return std::nullopt;
    const qsizetype frames = pcm.frameCount();
    const qsizetype end = toFrame < 0 ? frames : std::min(toFrame, frames);
    const qsizetype begin = std::clamp<qsizetype>(fromFrame, 0, end);
    const qsizetype block = std::max<qsizetype>(1, qsizetype(pcm.sampleRate) * audibleBlockMs / 1000);
    const double threshold = 32768.0 * dbToAmplitude(silenceThresholdDb);
    const double thresholdPower = threshold * threshold;
    const qint16 *samples = pcm.samples.constData();
    for (qsizetype start = begin; start < end; start += block) {
        const qsizetype count = std::min(block, end - start) * pcm.channels;
        const qint16 *first = samples + start * pcm.channels;
        double sum = 0.0;
        for (qsizetype i = 0; i < count; ++i)
            sum += double(first[i]) * first[i];
        if (sum >= thresholdPower * double(count))
            return qint64(start) * 1000 / pcm.sampleRate;
    }
    return std::nullopt;
}

qint64 firstAudibleMs(const WavAudio &pcm, double silenceThresholdDb)
{
    return findAudibleMs(pcm, silenceThresholdDb).value_or(0);
}

void SongDecoder::OpusDecoderDeleter::operator()(OpusDecoder *decoder) const noexcept
{
    opus_decoder_destroy(decoder);
}

SongDecoder::SongDecoder(SongContainer song)
    : m_song(std::move(song))
{
    // decodeSongContainer() checked the container's own rules, but a
    // SongContainer can be built by hand too, so the ones the decode loop
    // relies on are checked again.
    const bool shapeOk = (m_song.channels == 1 || m_song.channels == 2)
        && (m_song.frameSamples == 960 || m_song.frameSamples == 1920 || m_song.frameSamples == 2880)
        && m_song.preSkip >= 0 && m_song.preSkip <= SongContainer::maxPreSkip && m_song.totalSamples >= 1
        && m_song.totalSamples <= SongContainer::maxTotalSamples && !m_song.packets.isEmpty();
    if (!shapeOk)
        return;

    // Every packet's duration comes from its TOC byte(s) alone, before any of
    // it is decoded: a packet claiming another length than the container
    // declares would shift everything after it, so the song is refused.
    m_packetSamples.reserve(m_song.packets.size());
    qint64 available = 0;
    for (qsizetype index = 0; index < m_song.packets.size(); ++index) {
        const QByteArray &packet = m_song.packets.at(index);
        if (packet.isEmpty() || packet.size() > SongContainer::maxPacketBytes)
            return;
        const int samples = opus_packet_get_nb_samples(reinterpret_cast<const unsigned char *>(packet.constData()),
                                                       opus_int32(packet.size()), songRate);
        const bool last = index + 1 == m_song.packets.size();
        if (samples <= 0 || (!last && samples != m_song.frameSamples) || (last && samples > m_song.frameSamples))
            return;
        m_packetSamples.push_back(samples);
        available += samples;
    }
    if (available < m_song.preSkip + m_song.totalSamples)
        return;

    int error = OPUS_OK;
    m_decoder.reset(opus_decoder_create(songRate, m_song.channels, &error));
    if (error != OPUS_OK || m_decoder == nullptr) {
        m_decoder.reset();
        return;
    }
    m_buffer.assign(std::size_t(maxPacketSamples) * m_song.channels, 0.0F);
    // The container clamps gainQ8 to attenuation; clamp again for a
    // hand-built one so the decoder never amplifies.
    m_gain = float(std::pow(10.0, std::min(m_song.gainQ8, 0) / 256.0 / 20.0));
    m_skip = m_song.preSkip;
    m_valid = true;
}

SongDecoder::~SongDecoder() = default;

bool SongDecoder::isValid() const noexcept
{
    return m_valid;
}

int SongDecoder::channels() const noexcept
{
    return m_song.channels;
}

qint64 SongDecoder::totalSamples() const noexcept
{
    return m_song.totalSamples;
}

qint64 SongDecoder::position() const noexcept
{
    return m_position;
}

bool SongDecoder::atEnd() const noexcept
{
    return !m_valid || m_position >= m_song.totalSamples || m_nextPacket >= m_song.packets.size();
}

QVector<qint16> SongDecoder::next()
{
    const int channels = m_song.channels;
    while (!atEnd()) {
        const qsizetype index = m_nextPacket++;
        const QByteArray &packet = m_song.packets.at(index);
        const int expected = m_packetSamples[std::size_t(index)];
        int decoded = opus_decode_float(m_decoder.get(), reinterpret_cast<const unsigned char *>(packet.constData()),
                                        opus_int32(packet.size()), m_buffer.data(), maxPacketSamples, 0);
        if (decoded != expected) {
            // A packet whose TOC was fine but whose body is not (or that
            // decodes to another length) is concealed like a lost one: the
            // song keeps its length and its timing, and never stops.
            decoded = opus_decode_float(m_decoder.get(), nullptr, 0, m_buffer.data(), expected, 0);
            if (decoded != expected) {
                std::fill_n(m_buffer.begin(), std::size_t(expected) * channels, 0.0F);
                decoded = expected;
            }
        }
        const qint64 skipped = std::min<qint64>(m_skip, decoded);
        m_skip -= skipped;
        const qint64 count = std::min<qint64>(decoded - skipped, m_song.totalSamples - m_position);
        if (count <= 0)
            continue;
        QVector<qint16> out(count * channels);
        const float *source = m_buffer.data() + skipped * channels;
        for (qsizetype i = 0; i < out.size(); ++i)
            out[i] = toS16(double(source[i]) * m_gain * 32768.0);
        m_position += count;
        return out;
    }
    return {};
}

void SongDecoder::seekToSample(qint64 sample)
{
    if (!m_valid)
        return;
    sample = std::clamp<qint64>(sample, 0, m_song.totalSamples);
    // In decoder-output samples: every packet but the last holds exactly
    // frameSamples (checked above), so the packet index is a division.
    const qint64 target = m_song.preSkip + sample;
    const qsizetype packet = qsizetype(std::min<qint64>(target / m_song.frameSamples, m_song.packets.size() - 1));
    const qsizetype first = std::max<qsizetype>(0, packet - 1);
    opus_decoder_ctl(m_decoder.get(), OPUS_RESET_STATE);
    m_nextPacket = first;
    m_skip = target - qint64(first) * m_song.frameSamples;
    m_position = sample;
}

} // namespace OpenChat
