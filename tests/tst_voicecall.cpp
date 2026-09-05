#include <QtTest>

#include "AudioTestSupport.h"
#include "call/CallMediaCrypto.h"
#include "call/CallMediaPacket.h"
#include "call/CallSession.h"
#include "call/CallSignal.h"
#include "call/CallSounds.h"
#include "media/AudioCodec.h"
#include "media/AudioConvert.h"
#include "media/JitterBuffer.h"
#include "media/PcmAudioCodec.h"
#include "media/SpeechLevelMeter.h"
#include "media/ToneSynth.h"
#include "media/WavFile.h"

#include <QDir>
#include <QRandomGenerator>

#include <algorithm>
#include <map>

using namespace OpenChat;
using namespace OpenChat::AudioTest;

namespace {

// How the simulated link mistreats the packets crossing it. Every field maps to
// something a real network does; setting them all to zero gives a perfect link,
// which is the only configuration under which audio can be expected out
// bit-for-bit.
struct Impairment final {
    double lossRate = 0.0;      // fraction of packets dropped outright
    int maxDelayTicks = 0;      // reordering: extra frame intervals a packet may take
    double duplicateRate = 0.0; // fraction delivered twice
    // A relay that holds every Nth packet for a fixed extra time: the shape of
    // periodic jitter, which a fixed buffer never recovers from because the
    // late packet is merely lost — later ones are already in hand, so there is
    // no underrun to re-prime on.
    int spikeEvery = 0;
    int spikeDelayTicks = 0;
    quint32 seed = 0x5eed;
};

// One direction of a call, driven at exactly one frame per tick.
//
// This is the whole test rig: it is the same loop a real call runs — capture a
// frame, send it, and independently pull a frame for playback every interval —
// with the network replaced by a scriptable one. Because playback pulls on the
// tick rather than on arrival, the output is a true recording of what the far
// end's speaker would have produced, gaps and all.
struct LinkResult final {
    QList<AudioFrame> playback;
    int packetsSent = 0;
    int packetsDropped = 0;
    int packetsDelivered = 0;
    // The receiver's buffer statistics halfway through the input and at its
    // end, so a test can tell what happened while the link was being learned
    // from what happened afterwards — and keep both apart from the tail, where
    // every buffer drains and starves because there is nothing left to send.
    JitterBufferStats midpoint;
    JitterBufferStats atEndOfInput;
};

// `clockMs`, when given, is advanced to the receiver's time at each tick so a
// session whose jitter buffer reads it sees arrivals scattered across real
// intervals rather than all at once.
[[nodiscard]] LinkResult pumpOneDirection(CallSession &sender, CallSession &receiver,
                                          const QList<AudioFrame> &input,
                                          const Impairment &impairment, int tailTicks,
                                          qint64 *clockMs = nullptr)
{
    QRandomGenerator random(impairment.seed);
    // Packets waiting to be delivered, keyed by the tick they arrive on.
    QMultiHash<int, QByteArray> inFlight;
    LinkResult result;

    const int ticks = static_cast<int>(input.size()) + tailTicks;
    for (int tick = 0; tick < ticks; ++tick) {
        if (clockMs != nullptr)
            *clockMs = qint64(tick) * CallAudioFormat::frameDurationMs;
        if (tick < input.size()) {
            const QByteArray packet = sender.processCapturedFrame(input.at(tick));
            if (!packet.isEmpty()) {
                ++result.packetsSent;
                const bool dropped =
                    impairment.lossRate > 0.0
                    && random.generateDouble() < impairment.lossRate;
                if (dropped) {
                    ++result.packetsDropped;
                } else {
                    int delay = impairment.maxDelayTicks > 0
                        ? static_cast<int>(random.bounded(impairment.maxDelayTicks + 1))
                        : 0;
                    if (impairment.spikeEvery > 0 && tick % impairment.spikeEvery == impairment.spikeEvery - 1)
                        delay += impairment.spikeDelayTicks;
                    inFlight.insert(tick + delay, packet);
                    if (impairment.duplicateRate > 0.0
                        && random.generateDouble() < impairment.duplicateRate)
                        inFlight.insert(tick + delay + 1, packet);
                }
            }
        }

        const QList<QByteArray> arriving = inFlight.values(tick);
        inFlight.remove(tick);
        for (const QByteArray &packet : arriving) {
            if (receiver.processIncomingPacket(packet) == CallSession::ReceiveResult::Queued)
                ++result.packetsDelivered;
        }

        result.playback.append(receiver.nextPlaybackFrame());
        if (tick == input.size() / 2)
            result.midpoint = receiver.jitterStats();
        if (tick == input.size() - 1)
            result.atEndOfInput = receiver.jitterStats();
    }
    return result;
}

// A caller/callee pair keyed from one shared secret, exactly as a real call is:
// the caller mints the secret, both derive their own send and receive keys from
// it, and neither is told the other's.
struct CallPair final {
    std::unique_ptr<CallSession> caller;
    std::unique_ptr<CallSession> callee;
    CallId callId = CallId::generate();
    QByteArray secret;
};

[[nodiscard]] CallPair makeCallPair(AudioCodecKind codec, JitterBuffer::Config jitter)
{
    CallPair pair;
    pair.secret = generateCallSecret();

    CallSession::Config callerConfig;
    callerConfig.callId = pair.callId;
    callerConfig.direction = CallDirection::Outgoing;
    callerConfig.codec = codec;
    callerConfig.jitter = jitter;
    pair.caller = CallSession::create(callerConfig, pair.secret);

    CallSession::Config calleeConfig = callerConfig;
    calleeConfig.direction = CallDirection::Incoming;
    pair.callee = CallSession::create(calleeConfig, pair.secret);
    return pair;
}

// A perfect link is the only place bit-exactness is a fair expectation, so it
// gets its own well-named configuration: one frame of priming, nothing lost,
// nothing reordered.
[[nodiscard]] JitterBuffer::Config immediateJitterConfig()
{
    JitterBuffer::Config config;
    config.targetDepth = 1;
    return config;
}

// The playout offset a given priming depth introduces: the buffer starts
// releasing frames on the tick its depth first reaches the target, so the first
// (targetDepth - 1) playback slots are silence.
[[nodiscard]] int leadInFrames(const JitterBuffer::Config &config)
{
    return std::max(0, config.targetDepth - 1);
}

// The constant offset at which `playback` reproduces `input` frame for frame, or
// -1 when no such offset exists.
//
// On an impaired link the playout delay is not something the test can predict —
// it depends on how the buffer filled — but it IS constant once playout starts,
// and every frame must line up under it. Discovering the offset and then
// demanding a total match is a stricter assertion than guessing the offset,
// because a stream that drifted by one frame anywhere would match at no offset
// at all.
[[nodiscard]] int findExactAlignment(const QList<AudioFrame> &playback,
                                     const QList<AudioFrame> &input, int maxLead)
{
    for (int lead = 0; lead <= maxLead; ++lead) {
        if (playback.size() < input.size() + lead)
            break;
        bool matches = true;
        for (int i = 0; i < input.size() && matches; ++i)
            matches = playback.at(i + lead) == input.at(i);
        if (matches)
            return lead;
    }
    return -1;
}

[[nodiscard]] QVector<qint16> samplesFrom(const QList<AudioFrame> &frames, int skipFrames,
                                          qsizetype limitSamples)
{
    QVector<qint16> out = AudioConvert::fromFrames(frames.mid(skipFrames));
    if (limitSamples >= 0 && out.size() > limitSamples)
        out.resize(limitSamples);
    return out;
}

// A jitter buffer fed by a scripted network and popped by a scripted clock.
//
// The sender puts packet s on the wire at s × 20 ms of ITS clock; `arrival`
// maps that to the receiver's clock, which is where delay, jitter and clock
// drift are all expressed. The receiver pops once per 20 ms, mid-interval, and
// everything that has arrived by then has been pushed first — the same ordering
// the network thread and the audio thread produce in a live call. Nothing here
// reads real time, so every run is repeatable to the frame.
struct ScriptedReceiver final {
    qint64 nowMs = 0;
    std::unique_ptr<JitterBuffer> buffer;
    quint32 nextToSend = 0;
    int tick = 0;
    // Receiver arrival time → sequence. A std::multimap rather than QMultiMap
    // because packets that land at the same instant must be delivered in the
    // order they were sent, and Qt's hands back the most recently inserted first.
    std::multimap<qint64, quint32> inFlight;

    ScriptedReceiver(JitterBuffer::Config config, bool adaptive)
    {
        if (adaptive)
            config.clock = [this] { return nowMs; };
        buffer = std::make_unique<JitterBuffer>(std::move(config));
    }

    // Puts the next `count` packets on the wire under the given arrival law.
    void send(int count, const std::function<qint64(quint32)> &arrival)
    {
        for (int i = 0; i < count; ++i, ++nextToSend)
            inFlight.emplace(arrival(nextToSend), nextToSend);
    }

    // Runs `ticks` playout intervals and returns what the buffer handed out.
    JitterBuffer::PopResult last; // the most recent pop, for asserting on its payload

    QList<JitterBuffer::PopKind> run(int ticks, bool quiet = true)
    {
        QList<JitterBuffer::PopKind> kinds;
        for (int i = 0; i < ticks; ++i, ++tick) {
            const qint64 popAt =
                qint64(tick) * CallAudioFormat::frameDurationMs + CallAudioFormat::frameDurationMs / 2;
            while (!inFlight.empty() && inFlight.begin()->first <= popAt) {
                const auto first = inFlight.begin();
                nowMs = first->first;
                (void)buffer->push(first->second, QByteArray::number(first->second));
                inFlight.erase(first);
            }
            nowMs = popAt;
            last = buffer->pop(quiet);
            kinds.append(last.kind);
        }
        return kinds;
    }
};

// An arrival law that leaves the receiver persistently deeper than it needs to
// be: the first four packets land together, then everything follows the grid.
// The opening burst reads as jitter, so the target rises to match the depth;
// once those four have aged out of the window the target falls back to the
// floor while the queue is still four deep — the shape a long call takes when
// the sender's clock has crept ahead, but reached in two seconds.
[[nodiscard]] qint64 openingBurst(quint32 sequence)
{
    return sequence < 3 ? 0 : qint64(sequence - 3) * 20;
}

[[nodiscard]] int countOf(const QList<JitterBuffer::PopKind> &kinds, JitterBuffer::PopKind kind)
{
    return static_cast<int>(std::count(kinds.cbegin(), kinds.cend(), kind));
}

// Noise well under the speaking threshold: every frame is unique and non-zero,
// so "which input frame is this?" is answerable by identity, yet the far end's
// level meter reads it as quiet — which is when the buffer is allowed to add or
// remove a frame.
[[nodiscard]] QVector<qint16> quietNoise(int milliseconds, quint32 seed)
{
    QVector<qint16> samples = noise(milliseconds, seed);
    for (qint16 &sample : samples)
        sample = static_cast<qint16>(sample / 64);
    return samples;
}

// Walks `playback` against `input` demanding that every non-silent frame is the
// next input frame not yet accounted for. A frame played out of order, twice,
// or altered fails the walk; frames the receiver dropped are simply skipped.
// Returns the number of input frames played, or -1 if the walk failed.
[[nodiscard]] int inOrderMatches(const QList<AudioFrame> &playback, const QList<AudioFrame> &input)
{
    int nextInput = 0;
    int played = 0;
    for (const AudioFrame &frame : playback) {
        if (!isFullAudioFrame(frame))
            return -1;
        if (frame == silentAudioFrame())
            continue;
        while (nextInput < input.size() && frame != input.at(nextInput))
            ++nextInput;
        if (nextInput >= input.size())
            return -1;
        ++nextInput;
        ++played;
    }
    return played;
}

// A tiny WAV builder, so the malformed-container cases are described by what
// they contain rather than by an opaque blob.
struct WavBuilder final {
    QByteArray riffId = "RIFF";
    QByteArray waveId = "WAVE";
    quint16 codec = 1;
    quint16 channels = 1;
    quint32 sampleRate = 48000;
    quint16 bitsPerSample = 16;
    bool includeFormat = true;
    bool includeData = true;
    QByteArray extraChunkId;   // an unknown chunk placed before the data chunk
    QByteArray extraChunkBody;
    QByteArray data;
    // Written into the data chunk's size field instead of data.size(), to model
    // a writer that lied or a file that was truncated after the header.
    std::optional<quint32> declaredDataSize;

    [[nodiscard]] QByteArray build() const
    {
        const auto u32 = [](quint32 value) {
            QByteArray bytes(4, Qt::Uninitialized);
            qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data()));
            return bytes;
        };
        const auto u16 = [](quint16 value) {
            QByteArray bytes(2, Qt::Uninitialized);
            qToLittleEndian(value, reinterpret_cast<uchar *>(bytes.data()));
            return bytes;
        };

        QByteArray body = waveId;
        if (includeFormat) {
            QByteArray fmt = u16(codec) + u16(channels) + u32(sampleRate)
                + u32(sampleRate * channels * bitsPerSample / 8)
                + u16(static_cast<quint16>(channels * bitsPerSample / 8)) + u16(bitsPerSample);
            if (codec == 0xFFFE) {
                // WAVE_FORMAT_EXTENSIBLE: cbSize, valid bits, channel mask, then a
                // GUID whose first two bytes repeat the real codec tag.
                fmt += u16(22) + u16(bitsPerSample) + u32(channels == 1 ? 0x4 : 0x3) + u16(1)
                    + QByteArray(14, '\0');
            }
            body += QByteArray("fmt ") + u32(static_cast<quint32>(fmt.size())) + fmt;
            if (fmt.size() % 2 != 0)
                body += QByteArray(1, '\0');
        }
        if (!extraChunkId.isEmpty()) {
            body += extraChunkId + u32(static_cast<quint32>(extraChunkBody.size()))
                + extraChunkBody;
            if (extraChunkBody.size() % 2 != 0)
                body += QByteArray(1, '\0'); // RIFF pads odd chunks to even length
        }
        if (includeData) {
            body += QByteArray("data")
                + u32(declaredDataSize.value_or(static_cast<quint32>(data.size()))) + data;
        }
        return riffId + u32(static_cast<quint32>(body.size())) + body;
    }
};

} // namespace

class VoiceCallTest final : public QObject
{
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------- WAV in

    void readsARealAudioFileFromThisMachine()
    {
        const std::optional<LoadedWav> loaded = loadFirstSystemWav();
        if (!loaded)
            QSKIP("no readable WAV file found on this machine");
        qInfo().noquote() << "using system audio file:" << loaded->path
                          << loaded->audio.sampleRate << "Hz"
                          << loaded->audio.channels << "ch"
                          << loaded->audio.durationMs() << "ms";
        QVERIFY(loaded->audio.sampleRate > 0);
        QVERIFY(loaded->audio.channels > 0);
        QVERIFY(loaded->audio.frameCount() > 0);
        // Interleaved sample count must be a whole number of sample frames, or
        // every downstream channel calculation is off by a partial frame.
        QCOMPARE(loaded->audio.samples.size() % loaded->audio.channels, 0);
        QVERIFY(rmsLevel(loaded->audio.samples) > 0.0); // not a file of silence
    }

    void wavRoundTripsThroughTheWriter()
    {
        WavAudio original;
        original.sampleRate = 44100;
        original.channels = 2;
        original.samples = noise(120, 7);
        // Make the sample count a whole number of stereo frames.
        original.samples.resize(original.samples.size() / 2 * 2);

        auto encoded = WavFile::encode(original);
        QVERIFY(encoded.hasValue());
        auto decoded = WavFile::decode(encoded.value());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value().sampleRate, original.sampleRate);
        QCOMPARE(decoded.value().channels, original.channels);
        QCOMPARE(decoded.value().samples, original.samples);
    }

    void wavDecodesEverySupportedSampleEncoding_data()
    {
        QTest::addColumn<quint16>("codec");
        QTest::addColumn<quint16>("bits");
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QVector<qint16>>("expected");

        // 8-bit PCM is unsigned with a 128 bias; every wider depth is signed.
        QTest::newRow("u8") << quint16(1) << quint16(8)
                            << QByteArray::fromHex("80ff0040")
                            << QVector<qint16>{0, 32512, -32768, -16384};
        QTest::newRow("s16") << quint16(1) << quint16(16)
                             << QByteArray::fromHex("0000ff7f0080")
                             << QVector<qint16>{0, 32767, -32768};
        QTest::newRow("s24") << quint16(1) << quint16(24)
                             << QByteArray::fromHex("000000ffff7f000080")
                             << QVector<qint16>{0, 32767, -32768};
        QTest::newRow("s32") << quint16(1) << quint16(32)
                             << QByteArray::fromHex("00000000ffffff7f00000080")
                             << QVector<qint16>{0, 32767, -32768};
        // 1.0f, -1.0f, 0.0f
        QTest::newRow("f32") << quint16(3) << quint16(32)
                             << QByteArray::fromHex("0000803f000080bf00000000")
                             << QVector<qint16>{32767, -32767, 0};
        QTest::newRow("extensible-s16")
            << quint16(0xFFFE) << quint16(16) << QByteArray::fromHex("0100feff")
            << QVector<qint16>{1, -2};
    }

    void wavDecodesEverySupportedSampleEncoding()
    {
        QFETCH(quint16, codec);
        QFETCH(quint16, bits);
        QFETCH(QByteArray, data);
        QFETCH(QVector<qint16>, expected);

        WavBuilder builder;
        builder.codec = codec;
        builder.bitsPerSample = bits;
        builder.data = data;
        auto decoded = WavFile::decode(builder.build());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value().samples, expected);
    }

    void wavRejectsMalformedContainers_data()
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<int>("expectedError");

        const auto error = [](WavError value) { return static_cast<int>(value); };

        QTest::newRow("empty") << QByteArray() << error(WavError::NotRiffWave);
        QTest::newRow("truncated-riff-header")
            << QByteArray("RIFF\x10\x00\x00") << error(WavError::NotRiffWave);
        {
            WavBuilder builder;
            builder.riffId = "RIFX"; // a valid RIFF variant, but big-endian
            builder.data = QByteArray(8, '\1');
            QTest::newRow("wrong-container") << builder.build() << error(WavError::NotRiffWave);
        }
        {
            WavBuilder builder;
            builder.waveId = "AVI ";
            builder.data = QByteArray(8, '\1');
            QTest::newRow("not-wave") << builder.build() << error(WavError::NotRiffWave);
        }
        {
            WavBuilder builder;
            builder.includeFormat = false;
            builder.data = QByteArray(8, '\1');
            QTest::newRow("no-format-chunk") << builder.build() << error(WavError::MissingFormat);
        }
        {
            WavBuilder builder;
            builder.includeData = false;
            QTest::newRow("no-data-chunk") << builder.build() << error(WavError::MissingData);
        }
        {
            WavBuilder builder;
            builder.channels = 0;
            builder.data = QByteArray(8, '\1');
            QTest::newRow("zero-channels") << builder.build() << error(WavError::Unsupported);
        }
        {
            WavBuilder builder;
            builder.channels = 4096; // absurd: would blow up per-frame arithmetic
            builder.data = QByteArray(64, '\1');
            QTest::newRow("absurd-channel-count")
                << builder.build() << error(WavError::Unsupported);
        }
        {
            WavBuilder builder;
            builder.sampleRate = 0;
            builder.data = QByteArray(8, '\1');
            QTest::newRow("zero-sample-rate") << builder.build() << error(WavError::Unsupported);
        }
        {
            WavBuilder builder;
            builder.bitsPerSample = 12; // a real depth, but not one this reader decodes
            builder.data = QByteArray(8, '\1');
            QTest::newRow("unsupported-depth") << builder.build() << error(WavError::Unsupported);
        }
        {
            WavBuilder builder;
            builder.data = QByteArray(); // well formed, but carries no audio
            QTest::newRow("empty-data-chunk") << builder.build() << error(WavError::Empty);
        }
        {
            // A fmt chunk shorter than the 16 bytes the format needs.
            QByteArray bytes = QByteArray("RIFF") + QByteArray::fromHex("14000000")
                + QByteArray("WAVE") + QByteArray("fmt ") + QByteArray::fromHex("08000000")
                + QByteArray(8, '\0');
            QTest::newRow("short-format-chunk")
                << bytes << error(WavError::MalformedChunk);
        }
        {
            // Declares WAVE_FORMAT_EXTENSIBLE but stops before the extension that
            // carries the real codec tag.
            WavBuilder builder;
            builder.data = QByteArray(4, '\1');
            QByteArray bytes = builder.build();
            const int codecOffset = 20; // RIFF(12) + "fmt "(4) + size(4)
            bytes[codecOffset] = char(0xFE);
            bytes[codecOffset + 1] = char(0xFF);
            QTest::newRow("truncated-extensible")
                << bytes << error(WavError::MalformedChunk);
        }
    }

    void wavRejectsMalformedContainers()
    {
        QFETCH(QByteArray, bytes);
        QFETCH(int, expectedError);
        auto decoded = WavFile::decode(bytes);
        QVERIFY2(!decoded.hasValue(), "a malformed container was accepted");
        QCOMPARE(static_cast<int>(decoded.error()), expectedError);
    }

    void wavClampsALyingDataSizeInsteadOfTrustingIt()
    {
        // The single most common real-world corruption: a size field written
        // before the audio was, then never updated. Reading it as gospel would
        // mean a 4 GiB allocation from a 40-byte file.
        WavBuilder builder;
        builder.data = QByteArray::fromHex("0100feff");
        builder.declaredDataSize = 0xFFFFFFF0U;
        auto decoded = WavFile::decode(builder.build());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value().samples, (QVector<qint16>{1, -2}));
    }

    void wavWalksPastUnknownAndOddLengthChunks()
    {
        // Metadata chunks before the audio are normal (LIST/INFO is everywhere),
        // and an odd-length one exercises RIFF's word-alignment pad rule: get the
        // padding wrong and the data chunk is never found.
        WavBuilder builder;
        builder.extraChunkId = "LIST";
        builder.extraChunkBody = QByteArray("INFOIART\x05\x00\x00\x00" "Adam\0", 17);
        builder.data = QByteArray::fromHex("0100feff0300");
        auto decoded = WavFile::decode(builder.build());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value().samples, (QVector<qint16>{1, -2, 3}));
    }

    void wavDropsATrailingPartialSampleFrame()
    {
        // Stereo data cut mid-frame: the last orphaned sample has no partner and
        // must not shift every subsequent channel by one.
        WavBuilder builder;
        builder.channels = 2;
        builder.data = QByteArray::fromHex("0100020003000400" "0500");
        auto decoded = WavFile::decode(builder.build());
        QVERIFY(decoded.hasValue());
        QCOMPARE(decoded.value().channels, 2);
        QCOMPARE(decoded.value().samples, (QVector<qint16>{1, 2, 3, 4}));
        QCOMPARE(decoded.value().frameCount(), 2);
    }

    // --------------------------------------------------------- conditioning

    void callFormatConversionIsExactWhenNothingNeedsChanging()
    {
        // A source already at 48 kHz mono must survive conditioning untouched.
        // If it does not, no later stage can be compared bit-for-bit.
        WavAudio audio;
        audio.sampleRate = CallAudioFormat::sampleRate;
        audio.channels = 1;
        audio.samples = syntheticSpeech(200);
        QCOMPARE(AudioConvert::toCallFormat(audio), audio.samples);
    }

    void downmixAveragesChannelsWithoutOverflowing()
    {
        // Both channels at opposite full scale must average to silence, not wrap.
        const QVector<qint16> interleaved{32767, -32768, 32767, 32767, -32768, -32768};
        const QVector<qint16> mono = AudioConvert::downmixToMono(interleaved, 2);
        QCOMPARE(mono.size(), 3);
        // The S16 range is asymmetric, so +full and -full average to half an LSB
        // rather than exactly zero; what matters is that it stays at silence and
        // does not wrap to full scale.
        QVERIFY(std::abs(mono.at(0)) <= 1);
        QCOMPARE(mono.at(1), qint16(32767));
        QCOMPARE(mono.at(2), qint16(-32768));
    }

    void resamplingPreservesToneAndDuration()
    {
        const int sourceRate = 8000;
        const int milliseconds = 250;
        QVector<qint16> source;
        source.reserve(sourceRate * milliseconds / 1000);
        for (int i = 0; i < sourceRate * milliseconds / 1000; ++i) {
            const double t = static_cast<double>(i) / sourceRate;
            source.append(static_cast<qint16>(0.6 * std::sin(2.0 * M_PI * 440.0 * t) * 32767.0));
        }

        const QVector<qint16> resampled =
            AudioConvert::resampleMono(source, sourceRate, CallAudioFormat::sampleRate);
        // Duration must be preserved to within a sample: a rate conversion that
        // drifts turns a call into a slowly desynchronising one.
        const qsizetype expected = source.size() * CallAudioFormat::sampleRate / sourceRate;
        QVERIFY(std::abs(resampled.size() - expected) <= 1);

        // And the tone must still be a 440 Hz tone: count zero crossings and
        // check the implied frequency, which no amount of interpolation error
        // can fake.
        int crossings = 0;
        for (qsizetype i = 1; i < resampled.size(); ++i) {
            if ((resampled.at(i - 1) < 0) != (resampled.at(i) < 0))
                ++crossings;
        }
        const double seconds = static_cast<double>(resampled.size()) / CallAudioFormat::sampleRate;
        const double frequency = crossings / (2.0 * seconds);
        QVERIFY2(std::abs(frequency - 440.0) < 5.0,
                 qPrintable(QStringLiteral("resampled tone measured %1 Hz").arg(frequency)));
    }

    void framingIsReversibleAndPadsOnlyTheTail()
    {
        QVector<qint16> samples = syntheticSpeech(100);
        samples.resize(samples.size() - 17); // deliberately not a whole frame

        const QList<AudioFrame> frames = AudioConvert::toFrames(samples);
        for (const AudioFrame &frame : frames)
            QVERIFY(isFullAudioFrame(frame));

        const QVector<qint16> restored = AudioConvert::fromFrames(frames);
        QVERIFY(restored.size() >= samples.size());
        QCOMPARE(restored.first(samples.size()), samples);
        // Everything past the original audio must be silence, not stale memory.
        for (qsizetype i = samples.size(); i < restored.size(); ++i)
            QCOMPARE(restored.at(i), qint16(0));

        // Asking for no padding drops the partial frame entirely.
        const QList<AudioFrame> exact = AudioConvert::toFrames(samples, /*padTail=*/false);
        QCOMPARE(exact.size(), samples.size() / CallAudioFormat::samplesPerFrame);
    }

    // --------------------------------------------------------------- codecs

    void pcmCodecIsExactlyLossless()
    {
        PcmAudioCodec codec;
        for (const AudioFrame &frame : AudioConvert::toFrames(noise(200, 11))) {
            const QByteArray payload = codec.encode(frame);
            QCOMPARE(payload.size(), CallAudioFormat::bytesPerFrame);
            QCOMPARE(codec.decode(payload), frame);
        }
    }

    void codecsRejectFramesOfTheWrongLength()
    {
        for (const AudioCodecKind kind : {AudioCodecKind::Pcm, AudioCodecKind::Opus}) {
            std::unique_ptr<AudioCodec> codec = makeAudioCodec(kind);
            QVERIFY(codec);
            QVERIFY(codec->encode(AudioFrame()).isEmpty());
            QVERIFY(codec->encode(AudioFrame(CallAudioFormat::bytesPerFrame - 2, '\0')).isEmpty());
            QVERIFY(codec->encode(AudioFrame(CallAudioFormat::bytesPerFrame + 2, '\0')).isEmpty());
            QVERIFY(codec->decode(QByteArray()).isEmpty());
        }
    }

    void everyBuildOffersOpus()
    {
        // Raw PCM is 768 kbit/s: unusable over a domestic uplink and punishing
        // for the relay. A build that lacked Opus used to fall back to it
        // silently and produce a client that appeared to work while flooding
        // the network. Opus is now provided by the build itself, so a call may
        // only ever use PCM when someone asks for it by name.
        QVERIFY(isAudioCodecAvailable(AudioCodecKind::Opus));
        QCOMPARE(static_cast<int>(preferredAudioCodec()), static_cast<int>(AudioCodecKind::Opus));
        QVERIFY(makeAudioCodec(AudioCodecKind::Opus) != nullptr);
    }

    void opusRoundTripStaysFaithfulToTheSource()
    {
        std::unique_ptr<AudioCodec> encoder = makeAudioCodec(AudioCodecKind::Opus);
        std::unique_ptr<AudioCodec> decoder = makeAudioCodec(AudioCodecKind::Opus);
        QVERIFY(encoder && decoder);

        const QVector<qint16> source = syntheticSpeech(1500);
        QList<AudioFrame> output;
        qint64 compressedBytes = 0;
        for (const AudioFrame &frame : AudioConvert::toFrames(source)) {
            const QByteArray payload = encoder->encode(frame);
            QVERIFY(!payload.isEmpty());
            compressedBytes += payload.size();
            output.append(decoder->decode(payload));
        }

        const QVector<qint16> decoded = AudioConvert::fromFrames(output);
        // Opus delays its output; measure fidelity at the alignment that delay
        // implies rather than pretending there is none.
        const qsizetype lag = bestAlignment(source, decoded, 960);
        const double snr = snrDb(source, decoded, lag);
        qInfo().noquote() << "opus lag" << lag << "samples, SNR" << snr << "dB, ratio"
                          << (double(source.size() * 2) / double(compressedBytes));
        QVERIFY2(snr > 6.0, qPrintable(QStringLiteral("opus SNR only %1 dB").arg(snr)));
        // The whole reason Opus is here: it must actually be much smaller.
        QVERIFY(compressedBytes * 8 < source.size() * 2);
    }

    // -------------------------------------------------------- media crypto

    void mediaKeysAreDistinctPerDirectionAndPerCall()
    {
        const QByteArray secret = generateCallSecret();
        const CallId first = CallId::generate();
        const CallId second = CallId::generate();

        const auto a = CallMediaKeySchedule::derive(secret, first);
        const auto b = CallMediaKeySchedule::derive(secret, first);
        const auto c = CallMediaKeySchedule::derive(secret, second);
        QVERIFY(a && b && c);

        // Deterministic: both ends of a call must land on the same keys.
        QCOMPARE(a->fromCaller.key, b->fromCaller.key);
        QCOMPARE(a->fromCaller.salt, b->fromCaller.salt);
        // The two directions must never share key material, or the two streams
        // would reuse nonces against the same key.
        QVERIFY(a->fromCaller.key != a->fromCallee.key);
        QVERIFY(a->fromCaller.salt != a->fromCallee.salt);
        // The same secret bound to a different call id gives unrelated keys, so
        // a replayed offer cannot resurrect an old call's key stream.
        QVERIFY(a->fromCaller.key != c->fromCaller.key);

        // Each end's send key is the other end's receive key, and vice versa.
        QCOMPARE(a->sendKeys(CallDirection::Outgoing).key,
                 a->receiveKeys(CallDirection::Incoming).key);
        QCOMPARE(a->sendKeys(CallDirection::Incoming).key,
                 a->receiveKeys(CallDirection::Outgoing).key);
    }

    void shortOrAbsentSecretsCannotKeyACall()
    {
        const CallId callId = CallId::generate();
        QVERIFY(!CallMediaKeySchedule::derive(QByteArray(), callId));
        QVERIFY(!CallMediaKeySchedule::derive(QByteArray(31, 'k'), callId));
        QVERIFY(!CallMediaKeySchedule::derive(QByteArray(33, 'k'), callId));
        QVERIFY(CallMediaKeySchedule::derive(QByteArray(32, 'k'), callId));

        // And a session refuses to exist without them, rather than falling back
        // to sending audio unprotected.
        CallSession::Config config;
        config.callId = callId;
        QVERIFY(CallSession::create(config, QByteArray(16, 'k')) == nullptr);
    }

    void tamperingWithAnyAuthenticatedFieldIsDetected()
    {
        const auto schedule = CallMediaKeySchedule::derive(generateCallSecret(),
                                                           CallId::generate());
        QVERIFY(schedule);
        CallMediaSealer sealer(schedule->fromCaller);
        CallMediaOpener opener(schedule->fromCaller);

        const QByteArray plaintext = "twenty milliseconds of audio";
        const QByteArray header(CallMediaPacket::headerBytes, '\x11');
        const QByteArray sealed = sealer.seal(7, plaintext, header);
        QVERIFY(!sealed.isEmpty());

        // The honest case first, so the rejections below mean something.
        QCOMPARE(opener.open(7, sealed, header).value_or(QByteArray()), plaintext);

        CallMediaOpener fresh(schedule->fromCaller);
        // A different sequence: the nonce changes, so the tag cannot verify.
        QVERIFY(!fresh.open(8, sealed, header));
        // A single altered header byte: the header is the associated data.
        QByteArray alteredHeader = header;
        alteredHeader[3] = char(0x12);
        QVERIFY(!fresh.open(7, sealed, alteredHeader));
        // A single altered ciphertext byte.
        QByteArray alteredSealed = sealed;
        alteredSealed[0] = char(alteredSealed.at(0) ^ 0x01);
        QVERIFY(!fresh.open(7, alteredSealed, header));
        // A truncated tag.
        QVERIFY(!fresh.open(7, sealed.left(sealed.size() - 1), header));
        // The other direction's key must not open this direction's traffic.
        CallMediaOpener wrongDirection(schedule->fromCallee);
        QVERIFY(!wrongDirection.open(7, sealed, header));
        // Every one of those was refused, and none of them advanced the window.
        QCOMPARE(fresh.acceptedCount(), quint64(0));
        QVERIFY(fresh.open(7, sealed, header).has_value());
    }

    void replayIsRefusedWhileReorderingIsNot()
    {
        const auto schedule = CallMediaKeySchedule::derive(generateCallSecret(),
                                                           CallId::generate());
        QVERIFY(schedule);
        CallMediaSealer sealer(schedule->fromCaller);
        CallMediaOpener opener(schedule->fromCaller);
        const QByteArray header(CallMediaPacket::headerBytes, '\x22');

        QList<QByteArray> sealedFrames;
        for (quint32 sequence = 0; sequence < 80; ++sequence)
            sealedFrames.append(sealer.seal(sequence, QByteArray("frame"), header));

        // Out of order, but all inside the window: every one is accepted.
        for (const quint32 sequence : {quint32(3), quint32(1), quint32(2), quint32(0)})
            QVERIFY(opener.open(sequence, sealedFrames.at(sequence), header).has_value());
        QCOMPARE(opener.acceptedCount(), quint64(4));

        // Each of those again: all refused as replays, none as forgeries.
        for (const quint32 sequence : {quint32(0), quint32(1), quint32(2), quint32(3)})
            QVERIFY(!opener.open(sequence, sealedFrames.at(sequence), header));
        QCOMPARE(opener.replayCount(), quint64(4));

        // A jump far ahead slides the window past the old sequences, which then
        // become unprovable rather than merely unseen — still refused.
        QVERIFY(opener.open(75, sealedFrames.at(75), header).has_value());
        QVERIFY(!opener.open(5, sealedFrames.at(5), header));
        // But a sequence still inside the window after the jump is accepted.
        QVERIFY(opener.open(70, sealedFrames.at(70), header).has_value());
    }

    void aForgedHighSequenceCannotLockOutRealAudio()
    {
        // Without this property an attacker who cannot decrypt anything could
        // still silence a call by injecting one packet claiming a huge sequence.
        const auto schedule = CallMediaKeySchedule::derive(generateCallSecret(),
                                                           CallId::generate());
        QVERIFY(schedule);
        CallMediaSealer sealer(schedule->fromCaller);
        CallMediaOpener opener(schedule->fromCaller);
        const QByteArray header(CallMediaPacket::headerBytes, '\x33');

        QVERIFY(!opener.open(1'000'000, QByteArray(32, '\xAB'), header));
        // The genuine stream still plays.
        for (quint32 sequence = 0; sequence < 5; ++sequence)
            QVERIFY(opener.open(sequence, sealer.seal(sequence, QByteArray("x"), header), header)
                        .has_value());
    }

    void mediaPacketsRejectMalformedFraming_data()
    {
        QTest::addColumn<QByteArray>("bytes");

        QTest::newRow("empty") << QByteArray();
        QTest::newRow("header-only") << QByteArray(CallMediaPacket::headerBytes, '\1');
        QTest::newRow("shorter-than-header")
            << QByteArray(CallMediaPacket::headerBytes - 1, '\1');
        QTest::newRow("oversized") << QByteArray(CallMediaPacket::maxBytes + 1, '\1');
        {
            QByteArray bytes(CallMediaPacket::headerBytes + 4, '\1');
            bytes[0] = char(99); // an unknown version
            QTest::newRow("unknown-version") << bytes;
        }
        {
            // An all-zero call id is not a valid identifier.
            QByteArray bytes(CallMediaPacket::headerBytes + 4, '\0');
            bytes[0] = char(CallMediaPacket::currentVersion);
            QTest::newRow("null-call-id") << bytes;
        }
    }

    void mediaPacketsRejectMalformedFraming()
    {
        QFETCH(QByteArray, bytes);
        QVERIFY(!CallMediaPacket::decode(bytes).has_value());
    }

    void mediaPacketHeaderRoundTrips()
    {
        CallMediaPacket packet;
        packet.callId = CallId::generate();
        packet.sequence = 0xDEADBEEF;
        packet.flags = CallMediaPacket::flagMuted;
        packet.sealed = QByteArray(64, '\x5A');

        const auto decoded = CallMediaPacket::decode(packet.encode());
        QVERIFY(decoded);
        QCOMPARE(decoded->callId, packet.callId);
        QCOMPARE(decoded->sequence, packet.sequence);
        QCOMPARE(decoded->sealed, packet.sealed);
        QVERIFY(decoded->isMuted());
        QCOMPARE(decoded->header(), packet.header());
    }

    // -------------------------------------------------------- jitter buffer

    void jitterBufferPrimesBeforeItPlays()
    {
        JitterBuffer::Config config;
        config.targetDepth = 3;
        JitterBuffer buffer(config);

        QCOMPARE(buffer.push(0, "a"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.pop().kind, JitterBuffer::PopKind::Starved);
        QCOMPARE(buffer.push(1, "b"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.pop().kind, JitterBuffer::PopKind::Starved);
        QCOMPARE(buffer.push(2, "c"), JitterBuffer::PushResult::Accepted);

        // Depth reached the target, so playout starts — in order, from the first.
        for (const char *expected : {"a", "b", "c"}) {
            const JitterBuffer::PopResult result = buffer.pop();
            QCOMPARE(result.kind, JitterBuffer::PopKind::Frame);
            QCOMPARE(result.payload, QByteArray(expected));
        }
        // Drained: back to priming rather than flapping between play and starve.
        QCOMPARE(buffer.pop().kind, JitterBuffer::PopKind::Starved);
        QVERIFY(buffer.isPriming());
    }

    void jitterBufferAdmitsAnEarlyPacketThatArrivesSecond()
    {
        // The first packet to ARRIVE is not necessarily the first packet SENT.
        // Nothing has played yet when it happens, so the playout start can still
        // be moved back to the earlier one — otherwise a call whose opening two
        // packets crossed on the wire loses its first frame every time.
        JitterBuffer::Config config;
        config.targetDepth = 3;
        JitterBuffer buffer(config);

        QCOMPARE(buffer.push(11, "b"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.push(10, "a"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.push(12, "c"), JitterBuffer::PushResult::Accepted);
        for (const char *expected : {"a", "b", "c"})
            QCOMPARE(buffer.pop().payload, QByteArray(expected));
        QCOMPARE(buffer.stats().late, quint64(0));
    }

    void aLateStragglerNeverRewindsPlayout()
    {
        // Once a frame has been rendered the playout cursor is authoritative.
        // An underrun re-primes the buffer, but a straggler arriving during that
        // re-prime must NOT drag the cursor back over audio already played —
        // doing so would replay those slots as concealed gaps.
        JitterBuffer::Config config;
        config.targetDepth = 1;
        JitterBuffer buffer(config);

        for (quint32 sequence = 0; sequence < 4; ++sequence) {
            QCOMPARE(buffer.push(sequence, QByteArray::number(sequence)),
                     JitterBuffer::PushResult::Accepted);
            QCOMPARE(buffer.pop().payload, QByteArray::number(sequence));
        }
        // Drained: the buffer re-primes.
        QCOMPARE(buffer.pop().kind, JitterBuffer::PopKind::Starved);
        QVERIFY(buffer.isPriming());

        // A straggler for a slot that has already been heard is late, full stop.
        QCOMPARE(buffer.push(1, "stale"), JitterBuffer::PushResult::Late);
        QCOMPARE(buffer.push(4, "next"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.pop().payload, QByteArray("next"));
        QCOMPARE(buffer.stats().lost, quint64(0));
    }

    void jitterBufferReordersWithinItsWindow()
    {
        JitterBuffer::Config config;
        config.targetDepth = 4;
        JitterBuffer buffer(config);

        // Arrive badly out of order, but all before playout needs them.
        for (const auto &[sequence, payload] :
             QList<QPair<quint32, QByteArray>>{{3, "d"}, {0, "a"}, {2, "c"}, {1, "b"}})
            QCOMPARE(buffer.push(sequence, payload), JitterBuffer::PushResult::Accepted);

        for (const char *expected : {"a", "b", "c", "d"}) {
            const JitterBuffer::PopResult result = buffer.pop();
            QCOMPARE(result.kind, JitterBuffer::PopKind::Frame);
            QCOMPARE(result.payload, QByteArray(expected));
        }
    }

    void jitterBufferClassifiesEveryKindOfBadArrival()
    {
        JitterBuffer::Config config;
        config.targetDepth = 1;
        config.maxDepth = 8;
        config.resetDistance = 64;
        JitterBuffer buffer(config);

        QCOMPARE(buffer.push(10, "a"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.push(10, "a"), JitterBuffer::PushResult::Duplicate);
        // Too far ahead to address in a buffer this size.
        QCOMPARE(buffer.push(30, "far"), JitterBuffer::PushResult::Overflow);
        // Far enough away in either direction to be a different stream.
        QCOMPARE(buffer.push(500, "restart"), JitterBuffer::PushResult::Reset);

        QCOMPARE(buffer.pop().payload, QByteArray("restart"));
        // Now behind playout: late, not a duplicate and not a reset.
        QCOMPARE(buffer.push(500, "again"), JitterBuffer::PushResult::Late);

        const JitterBufferStats &stats = buffer.stats();
        QCOMPARE(stats.duplicates, quint64(1));
        QCOMPARE(stats.overflows, quint64(1));
        QCOMPARE(stats.resets, quint64(1));
        QCOMPARE(stats.late, quint64(1));
    }

    void jitterBufferReportsAGapRatherThanStalling()
    {
        JitterBuffer::Config config;
        config.targetDepth = 2;
        JitterBuffer buffer(config);

        QVERIFY(buffer.push(0, "a") == JitterBuffer::PushResult::Accepted);
        // Sequence 1 never arrives.
        QVERIFY(buffer.push(2, "c") == JitterBuffer::PushResult::Accepted);

        QCOMPARE(buffer.pop().payload, QByteArray("a"));
        const JitterBuffer::PopResult gap = buffer.pop();
        QCOMPARE(gap.kind, JitterBuffer::PopKind::Lost);
        QCOMPARE(gap.sequence, quint32(1));
        QCOMPARE(buffer.pop().payload, QByteArray("c"));
        QCOMPARE(buffer.stats().lost, quint64(1));
    }

    void jitterBufferSurvivesSequenceWraparound()
    {
        // A call long enough to wrap a 32-bit counter must not suddenly treat
        // every packet as ancient. This is pure arithmetic, so it is testable
        // without waiting 2.7 years.
        JitterBuffer::Config config;
        config.targetDepth = 1;
        JitterBuffer buffer(config);

        const quint32 start = 0xFFFFFFFDU;
        for (quint32 i = 0; i < 6; ++i)
            QCOMPARE(buffer.push(start + i, QByteArray::number(i)),
                     JitterBuffer::PushResult::Accepted);
        for (quint32 i = 0; i < 6; ++i) {
            const JitterBuffer::PopResult result = buffer.pop();
            QCOMPARE(result.kind, JitterBuffer::PopKind::Frame);
            QCOMPARE(result.payload, QByteArray::number(i));
        }
        QCOMPARE(buffer.stats().resets, quint64(0));
        QCOMPARE(buffer.stats().late, quint64(0));
    }

    void jitterBufferDepthIsBounded()
    {
        JitterBuffer::Config config;
        config.targetDepth = 2;
        config.maxDepth = 10;
        JitterBuffer buffer(config);
        // A sender racing far ahead of playout must not grow the queue without
        // limit; the excess is refused, not buffered.
        for (quint32 sequence = 0; sequence < 200; ++sequence)
            (void)buffer.push(sequence, QByteArray(4, 'x'));
        QVERIFY(buffer.depth() <= config.maxDepth);
        QVERIFY(buffer.stats().peakDepth <= config.maxDepth);
        QVERIFY(buffer.stats().overflows > 0);
    }

    // ------------------------------------------- adaptive playout

    void aBufferWithoutAClockHoldsItsDepthFixed()
    {
        // The exact-audio tests depend on a buffer that never reshapes time, so
        // "no clock" must mean "no controller" — not a controller running on a
        // guess.
        JitterBuffer buffer(JitterBuffer::Config{});
        QVERIFY(!buffer.isAdaptive());
        for (quint32 sequence = 0; sequence < 400; ++sequence) {
            (void)buffer.push(sequence, "x");
            // Every fifth interval a second frame lands: a sender running 20%
            // fast. A fixed buffer must simply fill up.
            if (sequence % 5 == 0)
                (void)buffer.push(++sequence, "y");
            (void)buffer.pop();
        }
        QCOMPARE(buffer.targetDepth(), 4);
        QCOMPARE(buffer.stats().inserted, quint64(0));
        QCOMPARE(buffer.stats().dropped, quint64(0));
        QVERIFY(buffer.depth() > 40);
    }

    void jitterDeepensTheCushionUntilNothingArrivesLate()
    {
        // A relay hop that holds every seventh packet 100 ms is well past what
        // an 80 ms cushion covers: fixed, each of those packets misses its slot
        // and is heard as a gap. Adapting, the buffer sees the spread and
        // deepens itself, after which the same packets are merely reordered.
        const auto spiky = [](quint32 sequence) -> qint64 {
            return qint64(sequence) * 20 + (sequence % 7 == 6 ? 100 : 0);
        };
        ScriptedReceiver fixed(JitterBuffer::Config{}, /*adaptive=*/false);
        fixed.send(200, spiky);
        (void)fixed.run(200);

        ScriptedReceiver adaptive(JitterBuffer::Config{}, /*adaptive=*/true);
        adaptive.send(200, spiky);
        const QList<JitterBuffer::PopKind> opening = adaptive.run(100);
        const QList<JitterBuffer::PopKind> settled = adaptive.run(100);

        QVERIFY(fixed.buffer->stats().lost >= 20);
        QCOMPARE(adaptive.buffer->targetDepth(), 6); // 100 ms is five frames, plus the one that plays
        QVERIFY(adaptive.buffer->stats().inserted >= 2);
        // It took one spike to learn from; after that, none is lost.
        QVERIFY(countOf(opening, JitterBuffer::PopKind::Lost) <= 1);
        QCOMPARE(countOf(settled, JitterBuffer::PopKind::Lost), 0);
        QCOMPARE(countOf(settled, JitterBuffer::PopKind::Starved), 0);
        QVERIFY(adaptive.buffer->stats().lost < fixed.buffer->stats().lost);
    }

    void aSettledLinkGivesTheLatencyBack()
    {
        // Latency bought against jitter is not kept once the jitter is gone: a
        // few seconds of clean arrivals and the target falls back to the floor,
        // with the surplus frames dropped one at a time in the quiet.
        const auto spiky = [](quint32 sequence) -> qint64 {
            return qint64(sequence) * 20 + (sequence % 7 == 6 ? 100 : 0);
        };
        const auto clean = [](quint32 sequence) -> qint64 { return qint64(sequence) * 20; };
        ScriptedReceiver receiver(JitterBuffer::Config{}, /*adaptive=*/true);
        receiver.send(200, spiky);
        receiver.send(400, clean);
        (void)receiver.run(200);
        QCOMPARE(receiver.buffer->targetDepth(), 6);
        const quint64 droppedBefore = receiver.buffer->stats().dropped;

        const QList<JitterBuffer::PopKind> afterwards = receiver.run(400);
        QCOMPARE(receiver.buffer->targetDepth(), 4);
        QVERIFY(receiver.buffer->stats().dropped >= droppedBefore + 2);
        // Shedding the depth cost no audio the ear could miss: no gap, no
        // starvation, and the queue is back at the floor.
        QCOMPARE(countOf(afterwards, JitterBuffer::PopKind::Lost), 0);
        QCOMPARE(countOf(afterwards, JitterBuffer::PopKind::Starved), 0);
        QVERIFY(receiver.buffer->depth() <= 5);
    }

    void aFastSenderIsTrimmedAFrameAtATime()
    {
        // The sender's sound card runs 2% fast (exaggerated from the real tens
        // of ppm so the effect shows in seconds rather than minutes): 51 frames
        // arrive for every 50 played. Fixed, the queue fills a frame every
        // second until it hits the ceiling. Compensated, one frame is dropped
        // each time the running depth sits a whole frame above target.
        const auto fast = [](quint32 sequence) -> qint64 { return qint64(sequence) * 196 / 10; };
        ScriptedReceiver fixed(JitterBuffer::Config{}, /*adaptive=*/false);
        fixed.send(2100, fast);
        (void)fixed.run(2000);
        QVERIFY(fixed.buffer->depth() >= 40);

        ScriptedReceiver adaptive(JitterBuffer::Config{}, /*adaptive=*/true);
        adaptive.send(2100, fast);
        const QList<JitterBuffer::PopKind> kinds = adaptive.run(2000);
        // 40 extra frames arrived over the run; all but the couple the average
        // has not caught up with yet have been shed.
        QVERIFY2(adaptive.buffer->stats().dropped >= 36 && adaptive.buffer->stats().dropped <= 41,
                 qPrintable(QStringLiteral("dropped %1").arg(adaptive.buffer->stats().dropped)));
        QVERIFY(adaptive.buffer->depth() <= 7);
        QVERIFY(adaptive.buffer->stats().peakDepth <= 8);
        QCOMPARE(adaptive.buffer->stats().overflows, quint64(0));
        QCOMPARE(adaptive.buffer->targetDepth(), 4); // drift is not jitter
        // Only the priming pops starved, and nothing was ever lost or invented.
        QCOMPARE(countOf(kinds, JitterBuffer::PopKind::Starved), 3);
        QCOMPARE(countOf(kinds, JitterBuffer::PopKind::Lost), 0);
        QCOMPARE(countOf(kinds, JitterBuffer::PopKind::Inserted), 0);
    }

    void aSlowSenderIsPaddedInsteadOfStarving()
    {
        // The mirror image: the sender runs 2% slow, so the queue drains a frame
        // a second. Fixed, it starves within seconds and re-primes — 80 ms of
        // silence every time, over and over. Compensated, one concealment frame
        // is slipped in whenever the depth sits a frame under target, and
        // playout never stops.
        const auto slow = [](quint32 sequence) -> qint64 { return qint64(sequence) * 204 / 10; };
        ScriptedReceiver fixed(JitterBuffer::Config{}, /*adaptive=*/false);
        fixed.send(2000, slow);
        const QList<JitterBuffer::PopKind> fixedKinds = fixed.run(2000);
        QVERIFY(countOf(fixedKinds, JitterBuffer::PopKind::Starved) > 20);

        ScriptedReceiver adaptive(JitterBuffer::Config{}, /*adaptive=*/true);
        adaptive.send(2000, slow);
        const QList<JitterBuffer::PopKind> kinds = adaptive.run(2000);
        QVERIFY2(adaptive.buffer->stats().inserted >= 36 && adaptive.buffer->stats().inserted <= 41,
                 qPrintable(QStringLiteral("inserted %1").arg(adaptive.buffer->stats().inserted)));
        QCOMPARE(countOf(kinds, JitterBuffer::PopKind::Starved), 3);
        QCOMPARE(countOf(kinds, JitterBuffer::PopKind::Lost), 0);
        QCOMPARE(adaptive.buffer->stats().dropped, quint64(0));
        QCOMPARE(adaptive.buffer->targetDepth(), 4);
    }

    void correctionsWaitForAQuietMomentButNotForever()
    {
        // A frame added or removed mid-word is a click; the same frame in a
        // pause is nothing. So a correction that is owed waits for the caller
        // to report quiet — but only for so long, because music has no pauses
        // and a buffer left to drift ends up starving, which is worse.
        JitterBuffer::Config config;
        config.correctionPatience = 30;
        // One packet arrives 65 ms late — still inside its slot, so nothing is
        // lost, but the spread now says the cushion should be a frame deeper.
        const auto oneStraggler = [](quint32 sequence) -> qint64 {
            return qint64(sequence) * 20 + (sequence == 10 ? 65 : 0);
        };

        // The straggler lands during the fourteenth interval, so that is the pop
        // at which the deeper target is first owed and the patience starts.
        ScriptedReceiver loud(config, /*adaptive=*/true);
        loud.send(200, oneStraggler);
        (void)loud.run(13, /*quiet=*/false);
        QCOMPARE(loud.buffer->targetDepth(), 4);
        (void)loud.run(1, /*quiet=*/false);
        QCOMPARE(loud.buffer->targetDepth(), 5);
        QCOMPARE(loud.buffer->stats().lost, quint64(0));
        QCOMPARE(loud.buffer->stats().inserted, quint64(0));
        // Owed, but the far end is talking: 28 more intervals go by untouched...
        const QList<JitterBuffer::PopKind> talking = loud.run(28, /*quiet=*/false);
        QCOMPARE(countOf(talking, JitterBuffer::PopKind::Inserted), 0);
        // ...and on the thirtieth the patience is spent and it happens anyway.
        QCOMPARE(loud.run(1, /*quiet=*/false).first(), JitterBuffer::PopKind::Inserted);

        ScriptedReceiver quiet(config, /*adaptive=*/true);
        quiet.send(200, oneStraggler);
        (void)quiet.run(15, /*quiet=*/false);
        QCOMPARE(quiet.buffer->targetDepth(), 5);
        QCOMPARE(quiet.buffer->stats().inserted, quint64(0));
        // The moment the far end falls quiet, the owed frame goes in.
        QCOMPARE(quiet.run(1, /*quiet=*/true).first(), JitterBuffer::PopKind::Inserted);
        QCOMPARE(quiet.buffer->stats().inserted, quint64(1));
    }

    void anInsertHappensAtOnceWhenTheAlternativeIsStarving()
    {
        // Waiting for quiet is a luxury the buffer cannot afford with one frame
        // left: starving costs a whole re-prime of silence, against one frame
        // of concealment now.
        JitterBuffer::Config config;
        config.targetDepth = 2;
        qint64 nowMs = 0;
        config.clock = [&nowMs] { return nowMs; };
        JitterBuffer buffer(config);

        // A clean start, then packets begin arriving 60 ms late — the target
        // grows to four while the queue holds two.
        for (quint32 sequence = 0; sequence < 4; ++sequence) {
            nowMs = qint64(sequence) * 20;
            QCOMPARE(buffer.push(sequence, QByteArray::number(sequence)),
                     JitterBuffer::PushResult::Accepted);
            (void)buffer.pop(/*quiet=*/false);
        }
        nowMs = 4 * 20 + 60;
        QCOMPARE(buffer.push(4, "4"), JitterBuffer::PushResult::Accepted);
        QCOMPARE(buffer.targetDepth(), 4);
        QCOMPARE(buffer.depth(), 2);
        // Two frames queued and the far end talking: the owed insert waits, and
        // the next frame plays as normal...
        QCOMPARE(buffer.pop(/*quiet=*/false).payload, QByteArray("3"));
        // ...but with one frame left and nothing new arrived, waiting any longer
        // would mean starving on the pop after. Insert now, loud or not — and
        // again, because two frames of depth were owed and each insert repays
        // exactly one.
        QCOMPARE(buffer.depth(), 1);
        QCOMPARE(buffer.pop(/*quiet=*/false).kind, JitterBuffer::PopKind::Inserted);
        QCOMPARE(buffer.pop(/*quiet=*/false).kind, JitterBuffer::PopKind::Inserted);
        // The frame itself is still there and still next.
        const JitterBuffer::PopResult next = buffer.pop(/*quiet=*/false);
        QCOMPARE(next.kind, JitterBuffer::PopKind::Frame);
        QCOMPARE(next.payload, QByteArray("4"));
    }

    void aHoleIsSkippedRatherThanConcealedWhenTheQueueIsTooDeep()
    {
        // When the buffer owes a drop and the next slot is empty anyway, the
        // cheapest correction is to skip the hole: one frame less latency and
        // one frame less concealment, and nothing that was ever audio is lost.
        JitterBuffer::Config config;
        config.targetDepth = 2;
        ScriptedReceiver receiver(config, /*adaptive=*/true);
        receiver.send(110, openingBurst);
        ++receiver.nextToSend; // sequence 110 is never sent
        receiver.send(89, openingBurst);

        // By the time the hole comes up the burst has aged out and the queue has
        // been two frames too deep for a while, but the far end has been talking
        // throughout, so the correction is still owed.
        const QList<JitterBuffer::PopKind> before = receiver.run(110, /*quiet=*/false);
        QCOMPARE(receiver.buffer->targetDepth(), 2);
        QCOMPARE(receiver.buffer->stats().dropped, quint64(0));
        QCOMPARE(countOf(before, JitterBuffer::PopKind::Lost), 0);

        // The hole is the correction: skipped, not concealed, and the frame
        // after it plays in its place.
        const QList<JitterBuffer::PopKind> atTheHole = receiver.run(1, /*quiet=*/false);
        QCOMPARE(atTheHole.first(), JitterBuffer::PopKind::Frame);
        QCOMPARE(receiver.last.payload, QByteArray::number(111));
        QVERIFY(receiver.last.skipped.isEmpty()); // nothing was removed that a decoder needs
        QCOMPARE(receiver.buffer->stats().dropped, quint64(1));
        QCOMPARE(receiver.buffer->stats().lost, quint64(0));
    }

    void aDroppedFrameIsHandedBackForTheDecoder()
    {
        // Opus predicts each frame from the previous one, so a frame the buffer
        // removes must still pass through the decoder or the frame after it
        // decodes against the wrong history. The buffer therefore returns what
        // it skipped alongside what it played.
        JitterBuffer::Config config;
        config.targetDepth = 2;
        ScriptedReceiver receiver(config, /*adaptive=*/true);
        receiver.send(200, openingBurst);

        // Quiet throughout, so the drop comes the moment it is owed — which is
        // when the burst ages out of the window, whichever pop that is.
        int pops = 0;
        while (receiver.buffer->stats().dropped == 0 && pops < 150) {
            (void)receiver.run(1, /*quiet=*/true);
            ++pops;
        }
        QVERIFY2(receiver.buffer->stats().dropped == 1, "no frame was ever dropped");
        // Sequence s plays on pop s until the drop; on that pop the frame that
        // was due is handed back and the one after it plays.
        QCOMPARE(receiver.last.kind, JitterBuffer::PopKind::Frame);
        QCOMPARE(receiver.last.skipped, QByteArray::number(pops - 1));
        QCOMPARE(receiver.last.payload, QByteArray::number(pops));
        QCOMPARE(receiver.buffer->stats().lost, quint64(0));
    }

    // ------------------------------------------- the call, end to end

    void audioArrivesIdenticallyOnTheOtherEnd()
    {
        // The headline property: what one end captures is exactly what the other
        // end plays. Run over the lossless codec on a perfect link, so any
        // difference at all is a real defect and not a codec's rounding.
        const JitterBuffer::Config jitter = immediateJitterConfig();
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QVector<qint16> source = syntheticSpeech(2000);
        const QList<AudioFrame> input = AudioConvert::toFrames(source);
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, Impairment{}, /*tailTicks=*/2);

        QCOMPARE(result.packetsSent, static_cast<int>(input.size()));
        QCOMPARE(result.packetsDelivered, static_cast<int>(input.size()));

        const int lead = leadInFrames(jitter);
        QCOMPARE(lead, 0);
        for (int i = 0; i < input.size(); ++i) {
            QVERIFY2(result.playback.at(i + lead) == input.at(i),
                     qPrintable(QStringLiteral("frame %1 differs").arg(i)));
        }
        // Nothing was concealed or invented along the way.
        QCOMPARE(pair.callee->stats().framesConcealed, quint64(0));
        QCOMPARE(pair.callee->replayCount(), quint64(0));
    }

    void audioFromAFileOnThisMachineArrivesIdentically()
    {
        // The same property, but sourced from a real audio file rather than a
        // synthesised one: an arbitrary rate, an arbitrary channel count, and a
        // spectrum nobody designed to be easy.
        const std::optional<LoadedWav> loaded = loadFirstSystemWav();
        if (!loaded)
            QSKIP("no readable WAV file found on this machine");

        const QVector<qint16> conditioned = AudioConvert::toCallFormat(loaded->audio);
        QVERIFY2(!conditioned.isEmpty(), "the file conditioned to nothing");
        // Conditioning must preserve the audio, not merely produce samples.
        QVERIFY(rmsLevel(conditioned) > 0.0);

        const JitterBuffer::Config jitter = immediateJitterConfig();
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> input = AudioConvert::toFrames(conditioned);
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, Impairment{}, /*tailTicks=*/2);

        const QVector<qint16> heard =
            samplesFrom(result.playback, leadInFrames(jitter), conditioned.size());
        QCOMPARE(heard.size(), conditioned.size());
        QVERIFY2(heard == conditioned,
                 qPrintable(QStringLiteral("audio from %1 did not survive the call")
                                .arg(loaded->path)));
    }

    void bothDirectionsCarryTheirOwnAudioAtOnce()
    {
        // A call is not two independent one-way streams: each end must hear the
        // other and never itself, which is exactly what a direction mix-up in
        // the key schedule would break.
        const JitterBuffer::Config jitter = immediateJitterConfig();
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> fromCaller = AudioConvert::toFrames(tone(400, 440.0, 0.5));
        const QList<AudioFrame> fromCallee = AudioConvert::toFrames(noise(400, 23));
        QCOMPARE(fromCaller.size(), fromCallee.size());

        QList<AudioFrame> heardByCallee;
        QList<AudioFrame> heardByCaller;
        for (int tick = 0; tick < fromCaller.size(); ++tick) {
            const QByteArray up = pair.caller->processCapturedFrame(fromCaller.at(tick));
            const QByteArray down = pair.callee->processCapturedFrame(fromCallee.at(tick));
            QCOMPARE(pair.callee->processIncomingPacket(up), CallSession::ReceiveResult::Queued);
            QCOMPARE(pair.caller->processIncomingPacket(down), CallSession::ReceiveResult::Queued);
            heardByCallee.append(pair.callee->nextPlaybackFrame());
            heardByCaller.append(pair.caller->nextPlaybackFrame());
        }

        QCOMPARE(heardByCallee, fromCaller);
        QCOMPARE(heardByCaller, fromCallee);
    }

    void aReorderingLinkStillDeliversTheAudioIntact()
    {
        // Reordering inside the buffer's window is recoverable, and recovering
        // it must be exact — not merely "close enough".
        JitterBuffer::Config jitter;
        jitter.targetDepth = 5;
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        Impairment impairment;
        impairment.maxDelayTicks = 3; // well inside a 5-frame prime
        const QList<AudioFrame> input = AudioConvert::toFrames(syntheticSpeech(1500));
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, impairment, /*tailTicks=*/8);

        // Every frame arrived, just not in order, so the far end must reproduce
        // all of them exactly under one constant playout delay.
        const int lead = findExactAlignment(result.playback, input,
                                            jitter.targetDepth + impairment.maxDelayTicks + 2);
        QVERIFY2(lead >= 0, "no playout offset reproduced the reordered audio exactly");
        QCOMPARE(pair.callee->stats().framesConcealed, quint64(0));
        QCOMPARE(pair.callee->jitterStats().accepted, quint64(input.size()));
        QCOMPARE(pair.callee->jitterStats().late, quint64(0));
    }

    void duplicatedPacketsChangeNothing()
    {
        // A relay that redelivers, or a retransmission that races the original,
        // must not double up the audio or advance the playout clock twice.
        JitterBuffer::Config jitter;
        jitter.targetDepth = 4;
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        Impairment impairment;
        impairment.duplicateRate = 1.0; // every single packet arrives twice
        const QList<AudioFrame> input = AudioConvert::toFrames(syntheticSpeech(800));
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, impairment, /*tailTicks=*/8);

        const int lead = findExactAlignment(result.playback, input, jitter.targetDepth + 2);
        QVERIFY2(lead >= 0, "duplicated delivery changed the audio");
        // Every duplicate was refused by the replay window before it ever
        // reached the buffer, so none of them were queued.
        QCOMPARE(pair.callee->replayCount(), quint64(input.size()));
        QCOMPARE(pair.callee->jitterStats().duplicates, quint64(0));
    }

    void everyPacketThatSurvivesTheLinkIsPlayedExactlyAndInOrder()
    {
        // Loss must cost the frames that were lost and nothing else: the rest of
        // the audio must still come out intact, in the right order, without
        // being corrupted, duplicated or silently resequenced around the hole.
        //
        // The source is noise so that every frame is unique and non-silent,
        // which makes "was this frame played?" answerable by identity — and
        // makes a concealment (silence, for the lossless codec) impossible to
        // confuse with real audio.
        JitterBuffer::Config jitter;
        jitter.targetDepth = 4;
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> input = AudioConvert::toFrames(noise(2000, 0xBEEF));
        Impairment impairment;
        impairment.lossRate = 0.05;
        impairment.seed = 0xC0FFEE;
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, impairment, /*tailTicks=*/8);
        QVERIFY(result.packetsDropped > 0);

        // Walk the playback against the input: every non-silent frame must be
        // the next input frame not yet accounted for. A frame played out of
        // order, twice, or altered would break the walk.
        int nextInput = 0;
        int played = 0;
        for (const AudioFrame &frame : result.playback) {
            QVERIFY(isFullAudioFrame(frame)); // the speaker is never starved
            if (frame == silentAudioFrame())
                continue; // a concealed gap or the pre-roll before playout began
            QVERIFY2(nextInput < input.size(), "more audio was played than was ever sent");
            // Skip the frames this playback frame proves were dropped.
            while (nextInput < input.size() && frame != input.at(nextInput))
                ++nextInput;
            QVERIFY2(nextInput < input.size(),
                     "a played frame does not match anything that was sent");
            ++nextInput;
            ++played;
        }
        QCOMPARE(played, static_cast<int>(input.size()) - result.packetsDropped);
        // Playback ran for every tick regardless of what the link did.
        QCOMPARE(result.playback.size(), input.size() + 8);
    }

    void aJitteryLinkDeepensTheCallInsteadOfStuttering()
    {
        // A relay hop that holds every seventh packet 100 ms, against an 80 ms
        // cushion. The fixed buffer never recovers: each held packet misses its
        // slot and is lost, but the frames behind it are already in hand, so
        // there is no underrun to re-prime on and nothing ever changes — one
        // frame in seven is a gap for the whole call. Given a clock, the
        // session's buffer sees the spread, deepens itself, and from then on the
        // same packets are merely early enough.
        Impairment impairment;
        impairment.spikeEvery = 7;
        impairment.spikeDelayTicks = 5; // 100 ms
        const QList<AudioFrame> input = AudioConvert::toFrames(syntheticSpeech(6000));
        const auto secondHalfGaps = [](const LinkResult &result) {
            return (result.atEndOfInput.lost + result.atEndOfInput.late)
                - (result.midpoint.lost + result.midpoint.late);
        };

        CallPair fixed = makeCallPair(AudioCodecKind::Pcm, JitterBuffer::Config{});
        QVERIFY(fixed.caller && fixed.callee);
        const LinkResult fixedResult =
            pumpOneDirection(*fixed.caller, *fixed.callee, input, impairment, /*tailTicks=*/12);
        QVERIFY2(secondHalfGaps(fixedResult) >= 18, "the fixed buffer stopped losing frames on its own");

        qint64 nowMs = 0;
        JitterBuffer::Config adaptiveConfig;
        adaptiveConfig.clock = [&nowMs] { return nowMs; };
        CallPair adaptive = makeCallPair(AudioCodecKind::Pcm, adaptiveConfig);
        QVERIFY(adaptive.caller && adaptive.callee);
        const LinkResult result = pumpOneDirection(*adaptive.caller, *adaptive.callee, input,
                                                   impairment, /*tailTicks=*/12, &nowMs);
        const JitterBufferStats stats = result.atEndOfInput;
        QCOMPARE(stats.targetDepth, 6); // 100 ms is five frames, plus the one that plays
        QVERIFY(stats.inserted >= 2);
        // It took a spike or two to learn from; in the second half nothing is
        // lost, nothing is late, and playout never stops.
        QCOMPARE(secondHalfGaps(result), quint64(0));
        QCOMPARE(stats.starved - result.midpoint.starved, quint64(0));
        QVERIFY(stats.lost + stats.late <= 3);
        // Everything that reached the far end was played once, in order and
        // unaltered; the deepening only moved it later. The pauses in the speech
        // are silent frames, which the walk cannot tell from concealment, so
        // they are the only ones it does not count.
        const int silentInputs =
            static_cast<int>(std::count(input.cbegin(), input.cend(), silentAudioFrame()));
        QCOMPARE(inOrderMatches(result.playback, input),
                 static_cast<int>(input.size()) - silentInputs - static_cast<int>(stats.lost));
        QCOMPARE(adaptive.callee->jitterStats().played + adaptive.callee->jitterStats().lost,
                 quint64(input.size()));
    }

    void aCallFromAFastSenderIsTrimmedWithoutCorruptingTheAudio()
    {
        // The full pipeline under clock drift: the sender produces an extra
        // frame every second, and the receiver has to lose one frame a second to
        // keep up — the right frames, in the right places, and nothing else.
        const int ticks = 3000;
        qint64 nowMs = 0;
        JitterBuffer::Config jitter;
        jitter.clock = [&nowMs] { return nowMs; };
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> input = AudioConvert::toFrames(quietNoise((ticks + ticks / 50 + 1) * 20, 0xD21F));
        QList<AudioFrame> playback;
        int sent = 0;
        for (int tick = 0; tick < ticks; ++tick) {
            nowMs = qint64(tick) * CallAudioFormat::frameDurationMs;
            const int frames = tick % 50 == 49 ? 2 : 1;
            for (int i = 0; i < frames; ++i) {
                const QByteArray packet = pair.caller->processCapturedFrame(input.at(sent++));
                QCOMPARE(pair.callee->processIncomingPacket(packet), CallSession::ReceiveResult::Queued);
            }
            playback.append(pair.callee->nextPlaybackFrame());
        }

        const JitterBufferStats stats = pair.callee->jitterStats();
        QVERIFY2(stats.dropped >= 55 && stats.dropped <= 61,
                 qPrintable(QStringLiteral("dropped %1").arg(stats.dropped)));
        QCOMPARE(stats.overflows, quint64(0));
        QVERIFY(stats.peakDepth <= 8);
        QCOMPARE(pair.callee->stats().framesConcealed, quint64(0));
        // The playback is the input with exactly the dropped frames missing.
        const int played = inOrderMatches(playback, input);
        QVERIFY2(played >= 0, "playback was not the input in order");
        QCOMPARE(played, static_cast<int>(stats.played));
        QCOMPARE(played + static_cast<int>(stats.dropped) + pair.callee->jitterStats().accepted
                     - stats.played - stats.dropped,
                 quint64(sent));
    }

    void aCallFromASlowSenderIsPaddedRatherThanReprimed()
    {
        // A sender that falls a frame behind every second would, with a fixed
        // buffer, starve the receiver every few seconds and cost 80 ms of dead
        // air each time. With compensation the shortfall is made up one
        // concealed frame at a time and playout never stops.
        const int ticks = 3000;
        const QList<AudioFrame> input = AudioConvert::toFrames(quietNoise(ticks * 20, 0x510));

        const auto run = [&](const JitterBuffer::Config &jitter, qint64 *clock) {
            CallPair pair = makeCallPair(AudioCodecKind::Pcm, jitter);
            QList<AudioFrame> playback;
            int sent = 0;
            for (int tick = 0; tick < ticks; ++tick) {
                if (clock != nullptr)
                    *clock = qint64(tick) * CallAudioFormat::frameDurationMs;
                if (tick % 50 != 49) {
                    const QByteArray packet = pair.caller->processCapturedFrame(input.at(sent++));
                    (void)pair.callee->processIncomingPacket(packet);
                }
                playback.append(pair.callee->nextPlaybackFrame());
            }
            return std::pair{std::move(pair), std::move(playback)};
        };

        auto [fixed, fixedPlayback] = run(JitterBuffer::Config{}, nullptr);
        QVERIFY(fixed.callee->jitterStats().starved > 40);

        qint64 nowMs = 0;
        JitterBuffer::Config jitter;
        jitter.clock = [&nowMs] { return nowMs; };
        auto [adaptive, playback] = run(jitter, &nowMs);
        const JitterBufferStats stats = adaptive.callee->jitterStats();
        QCOMPARE(stats.starved, quint64(3)); // priming only
        QVERIFY2(stats.inserted >= 55 && stats.inserted <= 61,
                 qPrintable(QStringLiteral("inserted %1").arg(stats.inserted)));
        QCOMPARE(stats.dropped, quint64(0));
        QCOMPARE(stats.lost, quint64(0));
        // Nothing real was lost: every frame sent and popped is in the output,
        // in order, with only silence between.
        QCOMPARE(inOrderMatches(playback, input), static_cast<int>(stats.played));
    }

    void aTotallyBrokenLinkPlaysSilenceRatherThanNothing()
    {
        // Every packet lost is the degenerate case, and it must still produce a
        // steady stream of full frames — a playback device starved of frames
        // clicks and stutters instead of going quiet.
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.caller && pair.callee);

        Impairment impairment;
        impairment.lossRate = 1.0;
        const QList<AudioFrame> input = AudioConvert::toFrames(syntheticSpeech(400));
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, impairment, /*tailTicks=*/4);

        QCOMPARE(result.packetsDelivered, 0);
        QCOMPARE(result.playback.size(), input.size() + 4);
        for (const AudioFrame &frame : result.playback)
            QCOMPARE(frame, silentAudioFrame());
        QCOMPARE(pair.callee->stats().framesSilent, quint64(result.playback.size()));
    }

    void mutingRemovesTheAudioRatherThanHidingIt()
    {
        // A mute that only set a flag would still put recognisable audio on the
        // wire. Check the far end hears exact silence AND that the packets keep
        // flowing, so the call's clock and the peer's view of it stay intact.
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.caller && pair.callee);
        pair.caller->setMuted(true);

        const QList<AudioFrame> input = AudioConvert::toFrames(tone(400, 440.0, 0.9));
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, Impairment{}, /*tailTicks=*/1);

        QCOMPARE(result.packetsSent, static_cast<int>(input.size()));
        for (const AudioFrame &frame : result.playback)
            QCOMPARE(frame, silentAudioFrame());
        // The local level meter agrees: a muted speaker does not read as talking.
        QVERIFY(!pair.caller->isLocalSpeaking());
        QCOMPARE(pair.caller->localLevel(), 0.0);

        // Unmuting restores real audio on the very next frame.
        pair.caller->setMuted(false);
        const QByteArray packet = pair.caller->processCapturedFrame(input.at(0));
        QCOMPARE(pair.callee->processIncomingPacket(packet), CallSession::ReceiveResult::Queued);
        QCOMPARE(pair.callee->nextPlaybackFrame(), input.at(0));
    }

    void mediaFromAnotherCallIsRefused()
    {
        // Two calls in a row, or a stale packet from a call that just ended,
        // must not be spliced into the live one.
        CallPair first = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        CallPair second = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(first.caller && second.callee);

        const AudioFrame frame = AudioConvert::toFrames(tone(20, 440.0, 0.5)).first();
        const QByteArray strayPacket = first.caller->processCapturedFrame(frame);
        QCOMPARE(second.callee->processIncomingPacket(strayPacket),
                 CallSession::ReceiveResult::WrongCall);
        QCOMPARE(second.callee->nextPlaybackFrame(), silentAudioFrame());
    }

    void aTamperedPacketNeverReachesPlayback()
    {
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.caller && pair.callee);

        const AudioFrame frame = AudioConvert::toFrames(tone(20, 660.0, 0.5)).first();
        QByteArray packet = pair.caller->processCapturedFrame(frame);
        QVERIFY(packet.size() > CallMediaPacket::headerBytes);

        // Flip one bit of the ciphertext.
        QByteArray corrupted = packet;
        corrupted[CallMediaPacket::headerBytes] =
            char(corrupted.at(CallMediaPacket::headerBytes) ^ 0x01);
        QCOMPARE(pair.callee->processIncomingPacket(corrupted),
                 CallSession::ReceiveResult::Unauthentic);

        // Flip one bit of the (authenticated, plaintext) header instead.
        QByteArray renumbered = packet;
        renumbered[21] = char(renumbered.at(21) ^ 0x01); // low byte of the sequence
        QCOMPARE(pair.callee->processIncomingPacket(renumbered),
                 CallSession::ReceiveResult::Unauthentic);

        // Nothing got through, and playback is silence rather than garbage.
        QCOMPARE(pair.callee->nextPlaybackFrame(), silentAudioFrame());
        QCOMPARE(pair.callee->stats().packetsRejected, quint64(2));
        // The genuine packet still plays afterwards.
        QCOMPARE(pair.callee->processIncomingPacket(packet), CallSession::ReceiveResult::Queued);
        QCOMPARE(pair.callee->nextPlaybackFrame(), frame);
    }

    void garbageOnTheMediaPathIsIgnored()
    {
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.callee);
        for (const QByteArray &junk : {QByteArray(), QByteArray("hello"),
                                       QByteArray(CallMediaPacket::maxBytes * 2, '\xFF')}) {
            QCOMPARE(pair.callee->processIncomingPacket(junk),
                     CallSession::ReceiveResult::Malformed);
        }
        QCOMPARE(pair.callee->nextPlaybackFrame(), silentAudioFrame());
    }

    void aWrongLengthCaptureFrameIsNotSent()
    {
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.caller);
        QVERIFY(pair.caller->processCapturedFrame(AudioFrame()).isEmpty());
        QVERIFY(pair.caller->processCapturedFrame(AudioFrame(100, '\0')).isEmpty());
        QCOMPARE(pair.caller->stats().framesSent, quint64(0));
        // And the sequence numbering did not advance for a frame never sent.
        const AudioFrame good = silentAudioFrame();
        const QByteArray packet = pair.caller->processCapturedFrame(good);
        const auto decoded = CallMediaPacket::decode(packet);
        QVERIFY(decoded);
        QCOMPARE(decoded->sequence, quint32(0));
    }

    void opusCallsCarryRecognisableAudioEndToEnd()
    {
        // Opus is lossy, so the promise is different in kind: not "the same
        // bytes" but "the same sound". Measure it as such.
        CallPair pair = makeCallPair(AudioCodecKind::Opus, immediateJitterConfig());
        QVERIFY(pair.caller && pair.callee);

        const QVector<qint16> source = syntheticSpeech(2000);
        const QList<AudioFrame> input = AudioConvert::toFrames(source);
        const LinkResult result =
            pumpOneDirection(*pair.caller, *pair.callee, input, Impairment{}, /*tailTicks=*/2);

        const QVector<qint16> heard = samplesFrom(result.playback, 0, -1);
        const qsizetype lag = bestAlignment(source, heard, 960);
        const double snr = snrDb(source, heard, lag);
        qInfo().noquote() << "end-to-end opus SNR" << snr << "dB at lag" << lag;
        QVERIFY2(snr > 6.0, qPrintable(QStringLiteral("opus call SNR only %1 dB").arg(snr)));
        // Loudness must survive too: a codec that quietly halved the level would
        // still score well on SNR relative to its own output.
        QVERIFY(std::abs(rmsLevel(heard) - rmsLevel(source)) < 0.05);
        QCOMPARE(result.playback.size(), input.size() + 2);
    }

    void opusConcealsLossInsteadOfDroppingToSilence()
    {
        // The point of asking the codec to conceal is that a single lost frame
        // should be barely noticeable rather than a hole in the audio.
        // A cushion deep enough that one drop is a gap in the middle of the
        // stream rather than an underrun at the end of it.
        JitterBuffer::Config jitter;
        jitter.targetDepth = 3;
        CallPair pair = makeCallPair(AudioCodecKind::Opus, jitter);
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> input = AudioConvert::toFrames(tone(600, 440.0, 0.6));
        QList<AudioFrame> heard;
        for (int i = 0; i < input.size(); ++i) {
            const QByteArray packet = pair.caller->processCapturedFrame(input.at(i));
            // Drop one frame from the middle of a steady tone.
            if (i != 15)
                (void)pair.callee->processIncomingPacket(packet);
            heard.append(pair.callee->nextPlaybackFrame());
        }
        QCOMPARE(pair.callee->stats().framesConcealed, quint64(1));
        QCOMPARE(pair.callee->stats().framesSilent, quint64(jitter.targetDepth - 1));

        // Once playout has begun, no frame is silent: the gap was filled with
        // extrapolated tone rather than with a hole in the audio.
        for (int i = jitter.targetDepth - 1; i < heard.size(); ++i) {
            QVERIFY2(AudioConvert::frameRms(heard.at(i)) > 0.05,
                     qPrintable(QStringLiteral("playback frame %1 dropped to silence").arg(i)));
        }
    }

    // -------------------------------------------------- speaking indicator

    void theSpeakingIndicatorFollowsTheVoice()
    {
        SpeechLevelMeter meter;
        QVERIFY(!meter.isSpeaking());

        // Silence never triggers it, however long it lasts.
        for (int i = 0; i < 50; ++i)
            meter.update(silentAudioFrame());
        QVERIFY(!meter.isSpeaking());
        QCOMPARE(meter.level(), 0.0);

        // Ordinary speech does, quickly.
        const QList<AudioFrame> speech = AudioConvert::toFrames(tone(200, 300.0, 0.35));
        meter.update(speech.first());
        QVERIFY(meter.isSpeaking());
        QVERIFY(meter.level() > 0.1);

        // A short gap between words does NOT clear it — that is the whole point
        // of the hangover, and without it the indicator strobes mid-sentence.
        for (int i = 0; i < 8; ++i)
            meter.updateSilent();
        QVERIFY(meter.isSpeaking());

        // A real pause does clear it.
        for (int i = 0; i < 40; ++i)
            meter.updateSilent();
        QVERIFY(!meter.isSpeaking());
        QVERIFY(meter.level() < 0.02);
    }

    void bothEndsSeeWhoIsTalking()
    {
        // The property the green ring depends on: while one side sends audio,
        // the other side's remote meter reports speaking, and the silent side
        // does not claim to be talking.
        CallPair pair = makeCallPair(AudioCodecKind::Pcm, immediateJitterConfig());
        QVERIFY(pair.caller && pair.callee);

        const QList<AudioFrame> speech = AudioConvert::toFrames(tone(300, 220.0, 0.4));
        for (const AudioFrame &frame : speech) {
            const QByteArray packet = pair.caller->processCapturedFrame(frame);
            (void)pair.callee->processIncomingPacket(packet);
            (void)pair.callee->nextPlaybackFrame();
            // The callee is silent throughout.
            (void)pair.caller->processIncomingPacket(
                pair.callee->processCapturedFrame(silentAudioFrame()));
            (void)pair.caller->nextPlaybackFrame();
        }

        QVERIFY(pair.caller->isLocalSpeaking());   // I am talking
        QVERIFY(pair.callee->isRemoteSpeaking());  // and they can tell
        QVERIFY(!pair.callee->isLocalSpeaking());  // they are not
        QVERIFY(!pair.caller->isRemoteSpeaking()); // and I can tell
        QVERIFY(pair.callee->remoteLevel() > pair.caller->remoteLevel());
    }

    // ------------------------------------------------------------ signalling

    void callSignalsRoundTrip()
    {
        const CallId callId = CallId::generate();
        const QByteArray secret = generateCallSecret();
        QCOMPARE(secret.size(), callSecretBytes);

        const QList<CallSignalMessage> messages{
            CallSignalMessage::offer(callId, secret, AudioCodecKind::Opus),
            CallSignalMessage::ringing(callId),
            CallSignalMessage::answer(callId, true, AudioCodecKind::Pcm),
            CallSignalMessage::answer(callId, false, AudioCodecKind::Pcm),
            CallSignalMessage::hangup(callId, CallEndReason::Busy),
        };
        for (const CallSignalMessage &message : messages) {
            const std::optional<CallSignalMessage> decoded =
                decodeCallSignal(encodeCallSignal(message));
            QVERIFY(decoded);
            QCOMPARE(static_cast<int>(decoded->type), static_cast<int>(message.type));
            QCOMPARE(decoded->callId, message.callId);
            QCOMPARE(decoded->accepted, message.accepted);
            QCOMPARE(static_cast<int>(decoded->reason), static_cast<int>(message.reason));
            if (message.type == CallSignalType::Offer)
                QCOMPARE(decoded->secret, message.secret);
        }
    }

    void malformedCallSignalsAreRejected()
    {
        const CallId callId = CallId::generate();
        QVERIFY(!decodeCallSignal(QByteArray()));
        QVERIFY(!decodeCallSignal(QByteArray("not cbor at all")));
        QVERIFY(!decodeCallSignal(QByteArray(2000, '\xFF')));

        // Right shape, wrong arity.
        QCborArray tooShort;
        tooShort.append(0);
        tooShort.append(callId.bytes());
        QVERIFY(!decodeCallSignal(tooShort.toCborValue().toCbor()));

        // A signal type this build does not know.
        QCborArray unknownType;
        unknownType.append(99);
        unknownType.append(callId.bytes());
        unknownType.append(QByteArray());
        unknownType.append(0);
        unknownType.append(false);
        unknownType.append(0);
        QVERIFY(!decodeCallSignal(unknownType.toCborValue().toCbor()));

        // An offer whose secret is too short to key a call.
        CallSignalMessage shortSecret =
            CallSignalMessage::offer(callId, QByteArray(8, 'k'), AudioCodecKind::Pcm);
        QVERIFY(!decodeCallSignal(encodeCallSignal(shortSecret)));

        // A null call id.
        QCborArray nullId;
        nullId.append(0);
        nullId.append(QByteArray(CallId::byteCount, '\0'));
        nullId.append(QByteArray(callSecretBytes, 'k'));
        nullId.append(0);
        nullId.append(false);
        nullId.append(0);
        QVERIFY(!decodeCallSignal(nullId.toCborValue().toCbor()));
    }

    void callSecretsAreFreshEveryTime()
    {
        QSet<QByteArray> seen;
        for (int i = 0; i < 32; ++i) {
            const QByteArray secret = generateCallSecret();
            QCOMPARE(secret.size(), callSecretBytes);
            QVERIFY2(!seen.contains(secret), "generateCallSecret repeated a value");
            seen.insert(secret);
        }
    }

    // ------------------------------------------------------------- call sounds

    void everyCallSoundExistsAndIsAudible()
    {
        // A sound that renders to nothing, or to something inaudibly quiet, is a
        // sound the user will report as broken. Check each one carries real
        // signal and sits in the headroom left for speech to play over it.
        for (const CallSound sound : CallSoundBoard::allSounds()) {
            const QVector<qint16> &samples = CallSoundBoard::samplesFor(sound);
            const QString name = CallSoundBoard::nameFor(sound);
            QVERIFY2(!samples.isEmpty(), qPrintable(name + QStringLiteral(" rendered nothing")));
            const double peak = peakLevel(samples);
            QVERIFY2(peak > 0.05,
                     qPrintable(QStringLiteral("%1 peaks at only %2").arg(name).arg(peak)));
            // Interface sounds are mixed over the far end's voice, so they must
            // leave room rather than dominate.
            QVERIFY2(peak < 0.6,
                     qPrintable(QStringLiteral("%1 peaks at %2, too loud to mix under speech")
                                    .arg(name)
                                    .arg(peak)));
        }
    }

    void callSoundsAreFreeOfClicks()
    {
        // A tone cut off at a non-zero sample is a step, and a step is an
        // audible click at both ends and at every loop point. Two checks: the
        // waveform starts and ends at silence, and no single sample-to-sample
        // step is large enough to be one.
        for (const CallSound sound : CallSoundBoard::allSounds()) {
            const QVector<qint16> &samples = CallSoundBoard::samplesFor(sound);
            const QString name = CallSoundBoard::nameFor(sound);
            QVERIFY(samples.size() > 2);
            QVERIFY2(std::abs(samples.first()) < 64,
                     qPrintable(name + QStringLiteral(" starts on a step")));
            QVERIFY2(std::abs(samples.last()) < 64,
                     qPrintable(name + QStringLiteral(" ends on a step")));

            qint32 largestStep = 0;
            for (qsizetype i = 1; i < samples.size(); ++i) {
                largestStep = std::max<qint32>(
                    largestStep, std::abs(static_cast<qint32>(samples.at(i))
                                          - static_cast<qint32>(samples.at(i - 1))));
            }
            // The steepest legitimate slope here is a ~1.6 kHz partial at these
            // amplitudes; anything far above that is a discontinuity.
            QVERIFY2(largestStep < 3000,
                     qPrintable(QStringLiteral("%1 has a %2-count jump between samples")
                                    .arg(name)
                                    .arg(largestStep)));
        }
    }

    void theTwoRingingSoundsAreTellableApart()
    {
        // Ringback ("their phone is ringing") and the incoming ring ("answer
        // me") mean opposite things and must never be confused. They differ in
        // both cadence and pitch, so compare both.
        const QVector<qint16> &ringback = CallSoundBoard::samplesFor(CallSound::Ringback);
        const QVector<qint16> &incoming = CallSoundBoard::samplesFor(CallSound::IncomingRing);

        // Different loop lengths give them different rhythms.
        QVERIFY(std::abs(ringback.size() - incoming.size())
                > CallAudioFormat::samplesForMs(300));

        // Both loops must both ring AND rest: a ring with no gap is a drone, and
        // one with no tone is nothing at all.
        for (const CallSound sound : {CallSound::Ringback, CallSound::IncomingRing}) {
            const QList<AudioFrame> frames =
                AudioConvert::toFrames(CallSoundBoard::samplesFor(sound));
            int loud = 0;
            int quiet = 0;
            for (const AudioFrame &frame : frames) {
                if (AudioConvert::frameRms(frame) > 0.02)
                    ++loud;
                else
                    ++quiet;
            }
            QVERIFY2(loud > 2, qPrintable(CallSoundBoard::nameFor(sound)
                                          + QStringLiteral(" never actually sounds")));
            QVERIFY2(quiet > 2, qPrintable(CallSoundBoard::nameFor(sound)
                                           + QStringLiteral(" never rests")));
        }

        // And they occupy different registers: the incoming ring sits higher, so
        // it cuts through a room the ringback would not.
        QVERIFY(rmsLevel(incoming) > 0.0 && rmsLevel(ringback) > 0.0);
    }

    void connectedAndEndedAreAMatchedPairInOppositeDirections()
    {
        // Answered rises, finished falls. Measured by comparing the dominant
        // pitch of each half via zero-crossing density, which needs no FFT and
        // cannot be fooled by amplitude.
        const auto halfPitch = [](const QVector<qint16> &samples, bool secondHalf) {
            const qsizetype from = secondHalf ? samples.size() / 2 : 0;
            const qsizetype to = secondHalf ? samples.size() : samples.size() / 2;
            int crossings = 0;
            for (qsizetype i = from + 1; i < to; ++i) {
                if ((samples.at(i - 1) < 0) != (samples.at(i) < 0))
                    ++crossings;
            }
            const double seconds = static_cast<double>(to - from) / CallAudioFormat::sampleRate;
            return seconds > 0.0 ? crossings / (2.0 * seconds) : 0.0;
        };

        const QVector<qint16> &connected = CallSoundBoard::samplesFor(CallSound::Connected);
        const QVector<qint16> &ended = CallSoundBoard::samplesFor(CallSound::Ended);
        QVERIFY2(halfPitch(connected, true) > halfPitch(connected, false),
                 "the pick-up sound does not rise");
        QVERIFY2(halfPitch(ended, true) < halfPitch(ended, false),
                 "the hang-up sound does not fall");

        // Mute is the low one and unmute the high one, for the same reason.
        const QVector<qint16> &muted = CallSoundBoard::samplesFor(CallSound::Muted);
        const QVector<qint16> &unmuted = CallSoundBoard::samplesFor(CallSound::Unmuted);
        QVERIFY2(halfPitch(unmuted, false) > halfPitch(muted, false),
                 "unmute is not higher than mute");

        // Both are short: they fire while somebody may be mid-sentence.
        QVERIFY(CallAudioFormat::msForSamples(muted.size()) <= 200);
        QVERIFY(CallAudioFormat::msForSamples(unmuted.size()) <= 200);
    }

    void aLoopingSoundRepeatsWithoutEndingOrDrifting()
    {
        CallSoundBoard board;
        QVERIFY(board.isIdle());
        board.startLoop(CallSound::Ringback);
        QVERIFY(!board.isIdle());
        QCOMPARE(board.loopingSound().value_or(CallSound::Ended), CallSound::Ringback);

        // Run well past the end of the source material: a loop that stopped, or
        // that ran out and fell silent, would show up as a long quiet tail.
        const qsizetype sourceFrames =
            CallSoundBoard::samplesFor(CallSound::Ringback).size()
            / CallAudioFormat::samplesPerFrame;
        const int totalFrames = static_cast<int>(sourceFrames * 3);
        int loudFrames = 0;
        for (int i = 0; i < totalFrames; ++i) {
            AudioFrame frame = silentAudioFrame();
            board.mixInto(frame);
            QVERIFY(isFullAudioFrame(frame));
            if (AudioConvert::frameRms(frame) > 0.02)
                ++loudFrames;
        }
        // Three cycles of a ring-and-rest pattern: it must have sounded in
        // roughly the same proportion each time round rather than once.
        QVERIFY2(loudFrames > totalFrames / 8,
                 qPrintable(QStringLiteral("the loop sounded in only %1 of %2 frames")
                                .arg(loudFrames)
                                .arg(totalFrames)));
        QVERIFY(!board.isIdle());

        board.stopLoop();
        QVERIFY(board.isIdle());
        AudioFrame afterStop = silentAudioFrame();
        board.mixInto(afterStop);
        QCOMPARE(afterStop, silentAudioFrame());
    }

    void aOneShotSoundPlaysExactlyOnce()
    {
        CallSoundBoard board;
        board.playOnce(CallSound::Connected);
        QVERIFY(!board.isIdle());
        QVERIFY(board.remainingOneShotMs() > 0);

        const qsizetype length = CallSoundBoard::samplesFor(CallSound::Connected).size();
        const int frames = static_cast<int>(length / CallAudioFormat::samplesPerFrame) + 2;
        QVector<qint16> rendered;
        for (int i = 0; i < frames; ++i) {
            AudioFrame frame = silentAudioFrame();
            board.mixInto(frame);
            rendered.append(AudioConvert::samplesOf(frame));
        }
        // It played, and then it stopped on its own.
        QVERIFY(peakLevel(rendered) > 0.05);
        QVERIFY(board.isIdle());
        QCOMPARE(board.remainingOneShotMs(), 0);

        AudioFrame afterwards = silentAudioFrame();
        board.mixInto(afterwards);
        QCOMPARE(afterwards, silentAudioFrame());

        // Asking for the same sound again while it is still running restarts it
        // rather than layering a second copy over the first.
        board.playOnce(CallSound::Ringback);
        board.playOnce(CallSound::Ringback);
        AudioFrame single = silentAudioFrame();
        board.mixInto(single);
        CallSoundBoard reference;
        reference.playOnce(CallSound::Ringback);
        AudioFrame expected = silentAudioFrame();
        reference.mixInto(expected);
        QCOMPARE(single, expected);
    }

    void soundsMixOverTheCallWithoutReplacingIt()
    {
        // The tones share the call's own output stream, so they must sit ON the
        // far end's voice rather than in place of it.
        const AudioFrame speech = AudioConvert::toFrames(tone(20, 300.0, 0.30)).first();

        CallSoundBoard board;
        AudioFrame withoutSound = speech;
        board.mixInto(withoutSound);
        QCOMPARE(withoutSound, speech); // nothing playing: untouched

        board.playOnce(CallSound::Muted);
        AudioFrame mixed = speech;
        board.mixInto(mixed);
        QVERIFY(mixed != speech);
        // The speech is still in there: the mix is additive, and removing the
        // blip again leaves something close to the original.
        QVERIFY(AudioConvert::frameRms(mixed) > AudioConvert::frameRms(speech) * 0.7);
        QVERIFY(AudioConvert::framePeak(mixed) <= 1.0);

        // Mixing into a frame of the wrong length leaves it alone rather than
        // writing past it.
        AudioFrame short_ = AudioFrame(100, '\0');
        board.mixInto(short_);
        QCOMPARE(short_.size(), 100);
    }

    void loudCallAudioMixedWithASoundSaturatesRatherThanWraps()
    {
        // Full-scale speech plus a tone must dull, not tear. Wrapping would turn
        // a loud moment into a burst of noise, which is far worse than a clip.
        CallSoundBoard board;
        board.startLoop(CallSound::IncomingRing);
        const QList<AudioFrame> loud = AudioConvert::toFrames(tone(400, 250.0, 1.0));
        for (const AudioFrame &input : loud) {
            AudioFrame frame = input;
            board.mixInto(frame);
            const QVector<qint16> before = AudioConvert::samplesOf(input);
            const QVector<qint16> after = AudioConvert::samplesOf(frame);
            for (qsizetype i = 0; i < before.size(); ++i) {
                // Saturation only ever moves a sample toward the rail it was
                // already heading for; a wrap would flip its sign.
                if (before.at(i) > 16000)
                    QVERIFY2(after.at(i) > 0, "a loud positive sample wrapped negative");
                if (before.at(i) < -16000)
                    QVERIFY2(after.at(i) < 0, "a loud negative sample wrapped positive");
            }
        }
    }

    void toneSynthesisIsDeterministicAndBounded()
    {
        const ToneSynth::Segment segment{{ToneSynth::Partial{440.0, 1.0},
                                          ToneSynth::Partial{880.0, 0.5}},
                                         /*durationMs=*/100,
                                         /*gain=*/1.0};
        const QVector<qint16> first = ToneSynth::renderSegment(segment);
        const QVector<qint16> second = ToneSynth::renderSegment(segment);
        QCOMPARE(first, second);
        QCOMPARE(first.size(), CallAudioFormat::samplesForMs(100));
        // Summed partials are normalised, so adding harmonics enriches the tone
        // instead of pushing it into the rails.
        QVERIFY(peakLevel(first) <= 1.0);

        // A silent segment is silence of exactly the right length.
        const QVector<qint16> silence = ToneSynth::renderSegment({{}, /*durationMs=*/40});
        QCOMPARE(silence.size(), CallAudioFormat::samplesForMs(40));
        QCOMPARE(peakLevel(silence), 0.0);

        // A segment shorter than its own envelope still opens and closes at
        // silence rather than jumping part-way up a ramp.
        const QVector<qint16> tiny =
            ToneSynth::renderSegment({{ToneSynth::Partial{1000.0, 1.0}},
                                      /*durationMs=*/5,
                                      /*gain=*/1.0,
                                      /*attackMs=*/50,
                                      /*releaseMs=*/50});
        QVERIFY(!tiny.isEmpty());
        QVERIFY(std::abs(tiny.first()) < 64);
        QVERIFY(std::abs(tiny.last()) < 64);

        // Zero and negative durations produce nothing at all.
        QVERIFY(ToneSynth::renderSegment({{ToneSynth::Partial{440.0, 1.0}}, 0}).isEmpty());
        QVERIFY(ToneSynth::renderSegment({{ToneSynth::Partial{440.0, 1.0}}, -20}).isEmpty());
    }

    void theRenderedSoundFilesMatchWhatTheAppPlays()
    {
        // assets/sounds holds a WAV of every call sound, rendered from this same
        // code by openchat-render-call-sounds. They exist so the sounds can be
        // listened to and handed around, which only means anything if they are
        // still what the application actually plays — so check, rather than
        // trusting whoever last edited a tone to have re-rendered.
        const QString directory =
            QStringLiteral(OPENCHAT_SOURCE_DIR) + QStringLiteral("/assets/sounds");
        for (const CallSound sound : CallSoundBoard::allSounds()) {
            const QString name = CallSoundBoard::nameFor(sound);
            const QString path = directory + QLatin1Char('/') + name + QStringLiteral(".wav");
            auto read = WavFile::readFile(path);
            QVERIFY2(read.hasValue(),
                     qPrintable(QStringLiteral("%1 is missing; run "
                                               "openchat-render-call-sounds assets/sounds")
                                    .arg(path)));
            QCOMPARE(read.value().sampleRate, CallAudioFormat::sampleRate);
            QCOMPARE(read.value().channels, CallAudioFormat::channels);
            QVERIFY2(read.value().samples == CallSoundBoard::samplesFor(sound),
                     qPrintable(QStringLiteral("%1.wav no longer matches the generated sound; "
                                               "run openchat-render-call-sounds assets/sounds")
                                    .arg(name)));
        }
    }
};

QTEST_MAIN(VoiceCallTest)
#include "tst_voicecall.moc"
