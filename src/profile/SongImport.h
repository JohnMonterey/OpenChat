#pragma once

#include "media/SongCodec.h"

#include <QByteArray>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThread>
#include <QThreadPool>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>

namespace OpenChat {

enum class SongImportError {
    FileMissing,        // gone, not a file, or unreadable
    FileTooLarge,       // over SongImportLimits::maxFileBytes, refused before reading
    UnsupportedFormat,  // neither WAV nor anything the decoder here can read
    DecoderUnavailable, // not WAV, and this computer has no Qt Multimedia decoder
    DecodeFailed,       // a readable format, but the file is broken or holds no audio
    Silent,
    TooShort,
    EncodeFailed,
    TimedOut,           // a decoder pass took longer than SongImportLimits::timeoutMs
};

// One short sentence for the song editor.
[[nodiscard]] QString songImportErrorText(SongImportError error);

struct SongImportLimits final {
    qint64 maxFileBytes = 64LL * 1024 * 1024;
    qint64 maxSourceMs = 15LL * 60 * 1000; // only the first 15 minutes can be picked from
    int timeoutMs = 30'000;
    int peakBuckets = 120;
    // A whole file's WAV is read into memory only up to this (a 5-minute
    // 96 kHz 24-bit stereo recording is about 170 MB); a denser one goes
    // through the decoder, which streams it.
    qint64 maxWavReadBytes = 192LL * 1024 * 1024;
};

// What the song editor shows once a file is picked: its card, its waveform
// and where the 45 s window starts.
struct SongSourceInfo final {
    QString fileName;
    QString formatLabel; // "WAV · 4:26 · 44.1 kHz stereo"
    qint64 durationMs = 0;
    int sampleRate = 0;
    int channels = 0;
    QString title, artist;              // from tags, sanitised to 60; title falls back to the base name
    QVector<quint8> peaks;              // peakBuckets values 0..255: |max| of the mid signal
    qint64 defaultWindowStartMs = 0;    // the first audible moment, pulled back so a full window fits

    friend bool operator==(const SongSourceInfo &, const SongSourceInfo &) = default;
};

// A whole file as one song (encodeWhole): a chat's audio attachment.
struct EncodedSong final {
    QByteArray container; // within the options' limits
    qint64 durationMs = 0;
    // The waveform of what was encoded: |max| of the mid signal per bar,
    // lifted (by at most 24 dB) so the loudest bar is 255.
    QVector<quint8> peaks;
    bool trimmed = false; // the source ran on past SongEncodeOptions::maxDurationMs

    friend bool operator==(const EncodedSong &, const EncodedSong &) = default;
};

// Imports a profile song in two passes (SPEC §14.11: pick a file, then drag
// a 45 s window over its waveform):
//   analyse()      streams the whole file once and keeps only its peaks and
//                  tags;
//   encodeWindow() decodes the chosen 45 s again and encodes it off the GUI
//                  thread.
// Any call cancels the one before it, whose result is then never emitted.
//
// A RIFF/WAVE file (by its magic, not its name) is read by WavFile on every
// platform and never loads Qt Multimedia. Anything else goes through
// QAudioDecoder, which needs a Multimedia backend: Linux's FFmpeg reads MP3,
// OGG, FLAC and M4A; Windows' Media Foundation MP3, M4A and WMA.
//
// A chat's audio attachment takes a third way, encodeWhole(): no analysis
// and no window, just the file from its start, up to the options' length, in
// one pass (a WAV is read no further than that needs).
class SongImporter final : public QObject
{
    Q_OBJECT
public:
    explicit SongImporter(SongImportLimits limits = {}, QObject *parent = nullptr);
    ~SongImporter() override; // cancels and waits for the worker
    // Nothing is running on its worker (after cancel(), a step that does not
    // stop half way may still be finishing, and deleting it waits for that).
    [[nodiscard]] bool isIdle() const;

    void analyse(const QString &path);
    // The window is clamped so it fits the source; `encoded` reports where it
    // really starts.
    void encodeWindow(const QString &path, qint64 startMs);
    // Encodes the file from its start, up to options.maxDurationMs, as one
    // song within options.limits, with a waveform of `peakBuckets` bars
    // taken from the same PCM; `encodedWhole` reports it. Progress runs
    // through the decode and then the encode.
    void encodeWhole(const QString &path, const SongEncodeOptions &options, int peakBuckets);
    void cancel();
    [[nodiscard]] bool busy() const;
    // The worker's thread priority (a chat import runs low, beside the UI).
    void setWorkerPriority(QThread::Priority priority);

    // Creates a QAudioDecoder, which loads the Multimedia backend: call it only
    // once the user has picked a file that is not WAV (Low memory mode
    // promises Multimedia stays unloaded until something needs it).
    [[nodiscard]] static bool canDecodeCompressed();
    // Sends WAV files through QAudioDecoder too, so its path can be tested
    // with a file the test writes itself.
    void setForceDecoderForTesting(bool force);
    // Runs on the worker just before each encode that starts after this call
    // (a test holds an encode "under way" with it, or makes it throw).
    void setEncodeHookForTesting(std::function<void()> hook);
    // Returns once the worker has finished everything it was given; whatever
    // that produced is posted to this object, not yet emitted.
    void waitForIdleForTesting();

signals:
    void analysed(const OpenChat::SongSourceInfo &info);
    void encoded(const QByteArray &container, qint64 durationMs, qint64 windowStartMs);
    void encodedWhole(const OpenChat::EncodedSong &song);
    void failed(OpenChat::SongImportError error, const QString &message);
    void progressChanged(qreal progress);

private:
    class DecodeRun; // one QAudioDecoder pass over a non-WAV file

    [[nodiscard]] quint64 begin();
    [[nodiscard]] bool isCurrent(quint64 generation) const;
    void postFailure(quint64 generation, SongImportError error);
    void deliverFailure(quint64 generation, SongImportError error);
    void deliverAnalysis(quint64 generation, const QString &path, const SongSourceInfo &info);
    void deliverEncoded(quint64 generation, const QByteArray &container, qint64 durationMs, qint64 windowStartMs);
    void deliverWhole(quint64 generation, const EncodedSong &song);
    void postProgress(quint64 generation, qreal progress);
    void startEncode(quint64 generation, SongClip clip, qint64 windowStartMs);
    void startWholeEncode(quint64 generation, SongClip clip, bool trimmed);
    // Runs `job` on the worker unless a newer call came first. A job that
    // throws fails with `error` instead of taking the process down.
    void runOnWorker(quint64 generation, SongImportError error, std::function<void()> job);
    // On the worker: encodes the clip and posts the outcome.
    void encodeAndPost(quint64 generation, const SongClip &clip, qint64 windowStartMs,
                       const std::function<void()> &hook);
    // On the worker: encodes a whole file's clip and posts the outcome;
    // progress continues from `progressFrom`.
    void encodeWholeAndPost(quint64 generation, const SongClip &clip, bool trimmed, const SongEncodeOptions &options,
                            int peakBuckets, qreal progressFrom, const std::function<void()> &hook);
    void startDecodeRun(quint64 generation, const QString &path, bool analyse, qint64 startMs, bool retry = false);
    void startWholeDecodeRun(quint64 generation, const QString &path);
    void endDecodeRun();
    [[nodiscard]] std::optional<SongImportError> checkFile(const QString &path) const;

    SongImportLimits m_limits;
    // What the current encodeWhole() asked for.
    SongEncodeOptions m_wholeOptions;
    int m_wholePeakBuckets = 0;
    bool m_forceDecoder = false;
    std::function<void()> m_encodeHook; // copied into each job as it starts
    bool m_busy = false;
    std::atomic<quint64> m_generation{0};
    // Durations of analysed files, so a compressed window can be placed
    // without decoding the file to its end first.
    QHash<QString, qint64> m_knownDurations;
    DecodeRun *m_run = nullptr;
    // One worker: a newer job waits the few milliseconds the older one needs
    // to notice it was cancelled, instead of both holding a song's PCM.
    QThreadPool m_pool;
};

} // namespace OpenChat

Q_DECLARE_METATYPE(OpenChat::SongSourceInfo)
Q_DECLARE_METATYPE(OpenChat::EncodedSong)
