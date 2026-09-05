#include "media/WavFile.h"

#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace OpenChat {

namespace {

constexpr quint16 formatPcm = 0x0001;
constexpr quint16 formatFloat = 0x0003;
constexpr quint16 formatExtensible = 0xFFFE;

// A RIFF chunk header is 4 bytes of id plus a 4-byte little-endian size, and a
// chunk's payload is padded to an even length that the size field excludes.
constexpr qsizetype chunkHeaderBytes = 8;
constexpr qsizetype riffHeaderBytes = 12; // "RIFF" + size + "WAVE"

// Reads a little-endian unsigned value at `offset`, or nullopt when the buffer
// is too short. Every header field goes through this, so a truncated file can
// never be read past its end.
template<typename T>
[[nodiscard]] std::optional<T> readLe(const QByteArray &bytes, qsizetype offset)
{
    if (offset < 0 || bytes.size() - offset < static_cast<qsizetype>(sizeof(T)))
        return std::nullopt;
    return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

[[nodiscard]] bool idAt(const QByteArray &bytes, qsizetype offset, const char (&id)[5])
{
    if (offset < 0 || bytes.size() - offset < 4)
        return false;
    return std::memcmp(bytes.constData() + offset, id, 4) == 0;
}

// Clamps a float sample in [-1, 1] to the S16 range with round-to-nearest.
// Anything outside the nominal range (float WAVs are allowed to overshoot) is
// saturated rather than wrapped, which is what every decoder does.
[[nodiscard]] qint16 floatToS16(double value)
{
    if (!std::isfinite(value))
        return 0;
    const double scaled = std::round(value * 32767.0);
    if (scaled >= 32767.0)
        return 32767;
    if (scaled <= -32768.0)
        return -32768;
    return static_cast<qint16>(scaled);
}

// The parsed fmt chunk, reduced to what the sample decoder needs.
struct WavFormat final {
    quint16 codec = 0;
    quint16 channels = 0;
    quint32 sampleRate = 0;
    quint16 bitsPerSample = 0;
};

// Decodes `data` (the raw data-chunk payload) into interleaved S16.
[[nodiscard]] Result<QVector<qint16>, WavError> decodeSamples(const WavFormat &format,
                                                              QByteArrayView data)
{
    const qsizetype bytesPerSample = format.bitsPerSample / 8;
    if (bytesPerSample <= 0)
        return Result<QVector<qint16>, WavError>::failure(WavError::Unsupported);

    // A data chunk whose length is not a whole number of samples is truncated to
    // the last complete sample rather than rejected: real files (and streams cut
    // mid-write) do this, and the trailing partial sample carries no audio.
    const qsizetype sampleCount = data.size() / bytesPerSample;
    QVector<qint16> out;
    out.reserve(sampleCount);
    const auto *raw = reinterpret_cast<const uchar *>(data.data());

    for (qsizetype i = 0; i < sampleCount; ++i) {
        const uchar *p = raw + i * bytesPerSample;
        switch (format.codec) {
        case formatPcm:
            switch (format.bitsPerSample) {
            case 8:
                // 8-bit WAV is unsigned with a 128 bias, unlike every wider depth.
                out.append(static_cast<qint16>((static_cast<int>(*p) - 128) * 256));
                break;
            case 16:
                out.append(qFromLittleEndian<qint16>(p));
                break;
            case 24: {
                // Sign-extend the 24-bit little-endian value, then take its top 16.
                const qint32 value = (static_cast<qint32>(p[0]) | (static_cast<qint32>(p[1]) << 8)
                                      | (static_cast<qint32>(static_cast<qint8>(p[2])) << 16));
                out.append(static_cast<qint16>(value >> 8));
                break;
            }
            case 32:
                out.append(static_cast<qint16>(qFromLittleEndian<qint32>(p) >> 16));
                break;
            default:
                return Result<QVector<qint16>, WavError>::failure(WavError::Unsupported);
            }
            break;
        case formatFloat:
            // Read the IEEE-754 bit pattern as a little-endian integer first, so
            // the decode is correct on a big-endian host too, then reinterpret.
            if (format.bitsPerSample == 32) {
                const quint32 bits = qFromLittleEndian<quint32>(p);
                float value = 0.0F;
                std::memcpy(&value, &bits, sizeof(value));
                out.append(floatToS16(static_cast<double>(value)));
            } else if (format.bitsPerSample == 64) {
                const quint64 bits = qFromLittleEndian<quint64>(p);
                double value = 0.0;
                std::memcpy(&value, &bits, sizeof(value));
                out.append(floatToS16(value));
            } else {
                return Result<QVector<qint16>, WavError>::failure(WavError::Unsupported);
            }
            break;
        default:
            return Result<QVector<qint16>, WavError>::failure(WavError::Unsupported);
        }
    }
    return Result<QVector<qint16>, WavError>::success(std::move(out));
}

} // namespace

Result<WavAudio, WavError> WavFile::decode(const QByteArray &bytes)
{
    using Failure = Result<WavAudio, WavError>;

    if (bytes.size() < riffHeaderBytes || !idAt(bytes, 0, "RIFF") || !idAt(bytes, 8, "WAVE"))
        return Failure::failure(WavError::NotRiffWave);

    // The RIFF size field is advisory: some writers leave it at 0 or at a
    // streaming placeholder. Walk to the real end of the buffer instead, but
    // never past it.
    std::optional<WavFormat> format;
    QByteArray data;
    bool sawData = false;

    qsizetype cursor = riffHeaderBytes;
    while (cursor + chunkHeaderBytes <= bytes.size()) {
        const std::optional<quint32> declared = readLe<quint32>(bytes, cursor + 4);
        if (!declared)
            return Failure::failure(WavError::MalformedChunk);
        const qsizetype payload = cursor + chunkHeaderBytes;
        const qsizetype available = bytes.size() - payload;
        // A declared size past the end of the file is clamped, not trusted: this
        // is the single most common corruption in real captures and the shape a
        // hostile header would take to provoke an over-large read.
        const qsizetype size =
            std::min(static_cast<qsizetype>(*declared), available < 0 ? qsizetype(0) : available);

        if (idAt(bytes, cursor, "fmt ")) {
            // A fmt chunk is at least 16 bytes; extensible adds cbSize and a
            // 22-byte extension whose first two bytes repeat the real codec tag.
            if (size < 16)
                return Failure::failure(WavError::MalformedChunk);
            WavFormat parsed;
            parsed.codec = readLe<quint16>(bytes, payload + 0).value_or(0);
            parsed.channels = readLe<quint16>(bytes, payload + 2).value_or(0);
            parsed.sampleRate = readLe<quint32>(bytes, payload + 4).value_or(0);
            parsed.bitsPerSample = readLe<quint16>(bytes, payload + 14).value_or(0);
            if (parsed.codec == formatExtensible) {
                if (size < 40)
                    return Failure::failure(WavError::MalformedChunk);
                parsed.codec = readLe<quint16>(bytes, payload + 24).value_or(0);
            }
            format = parsed;
        } else if (idAt(bytes, cursor, "data")) {
            data = bytes.mid(payload, size);
            sawData = true;
        }

        // Advance over the payload plus RIFF's pad byte for odd-length chunks.
        // The +1 guards a zero-size chunk from spinning the loop forever.
        const qsizetype advance = chunkHeaderBytes + size + (size % 2);
        cursor += std::max<qsizetype>(advance, chunkHeaderBytes);
    }

    if (!sawData)
        return Failure::failure(WavError::MissingData);
    if (!format)
        return Failure::failure(WavError::MissingFormat);
    if (format->channels == 0 || format->sampleRate == 0 || format->bitsPerSample == 0)
        return Failure::failure(WavError::Unsupported);
    // A channel count this large is never a real file; it would only serve to
    // make the per-frame arithmetic below overflow or allocate absurdly.
    if (format->channels > 64)
        return Failure::failure(WavError::Unsupported);

    auto samples = decodeSamples(*format, data);
    if (!samples.hasValue())
        return Failure::failure(samples.error());

    WavAudio audio;
    audio.sampleRate = static_cast<int>(format->sampleRate);
    audio.channels = static_cast<int>(format->channels);
    audio.samples = std::move(samples).value();
    // Drop a trailing partial sample frame so frameCount() is exact.
    if (const qsizetype remainder = audio.samples.size() % audio.channels; remainder != 0)
        audio.samples.resize(audio.samples.size() - remainder);
    if (audio.samples.isEmpty())
        return Failure::failure(WavError::Empty);
    return Result<WavAudio, WavError>::success(std::move(audio));
}

Result<WavAudio, WavError> WavFile::readFile(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return Result<WavAudio, WavError>::failure(WavError::NotFound);
    // Check the size before reading so an oversized file is refused rather than
    // pulled into memory and then rejected.
    if (file.size() > maxBytes)
        return Result<WavAudio, WavError>::failure(WavError::TooLarge);
    return decode(file.readAll());
}

Result<QByteArray, WavError> WavFile::encode(const WavAudio &audio)
{
    using Failure = Result<QByteArray, WavError>;
    if (audio.channels <= 0 || audio.sampleRate <= 0)
        return Failure::failure(WavError::Unsupported);

    const qint64 dataBytes = static_cast<qint64>(audio.samples.size()) * 2;
    if (dataBytes > std::numeric_limits<quint32>::max() - 36)
        return Failure::failure(WavError::TooLarge);

    const quint16 channels = static_cast<quint16>(audio.channels);
    const quint32 byteRate = static_cast<quint32>(audio.sampleRate) * channels * 2;
    const quint16 blockAlign = static_cast<quint16>(channels * 2);

    QByteArray out;
    out.reserve(44 + static_cast<qsizetype>(dataBytes));
    const auto appendId = [&out](const char *id) { out.append(id, 4); };
    const auto appendU32 = [&out](quint32 value) {
        uchar buffer[4];
        qToLittleEndian(value, buffer);
        out.append(reinterpret_cast<const char *>(buffer), 4);
    };
    const auto appendU16 = [&out](quint16 value) {
        uchar buffer[2];
        qToLittleEndian(value, buffer);
        out.append(reinterpret_cast<const char *>(buffer), 2);
    };

    appendId("RIFF");
    appendU32(static_cast<quint32>(36 + dataBytes));
    appendId("WAVE");
    appendId("fmt ");
    appendU32(16);
    appendU16(formatPcm);
    appendU16(channels);
    appendU32(static_cast<quint32>(audio.sampleRate));
    appendU32(byteRate);
    appendU16(blockAlign);
    appendU16(16); // bits per sample
    appendId("data");
    appendU32(static_cast<quint32>(dataBytes));

    const qsizetype offset = out.size();
    out.resize(offset + static_cast<qsizetype>(dataBytes));
    auto *cursor = reinterpret_cast<uchar *>(out.data() + offset);
    for (const qint16 sample : audio.samples) {
        qToLittleEndian(sample, cursor);
        cursor += 2;
    }
    return Result<QByteArray, WavError>::success(std::move(out));
}

Result<void, WavError> WavFile::writeFile(const QString &path, const WavAudio &audio)
{
    auto encoded = encode(audio);
    if (!encoded.hasValue())
        return Result<void, WavError>::failure(encoded.error());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return Result<void, WavError>::failure(WavError::WriteFailed);
    const QByteArray &bytes = encoded.value();
    if (file.write(bytes) != bytes.size())
        return Result<void, WavError>::failure(WavError::WriteFailed);
    return Result<void, WavError>::success();
}

} // namespace OpenChat
