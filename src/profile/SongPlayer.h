#pragma once

#include "domain/SongContainer.h"
#include "media/SongCodec.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace OpenChat {

// The song bytes a player may play, by key (the song's SHA-256 in hex). The
// profile controller puts the song of the page on screen (and of the draft)
// here, so 224 KB never travels through QML: the page's SongPlayer is given
// only the key. At most `capacity` songs are held, the least recently put
// evicted; a player keeps its own copy of what it loaded, so eviction never
// interrupts one. GUI thread only, like the players it notifies.
class SongLibrary final
{
public:
    static constexpr int capacity = 3;

    static SongLibrary &instance();

    // Players already waiting on `key` (their page arrived before its song
    // did) load the bytes at once.
    void put(const QString &key, const QByteArray &container);
    [[nodiscard]] QByteArray get(const QString &key) const;
    void release(const QString &key);
    void clear();

private:
    SongLibrary() = default;

    struct Entry final {
        QString key;
        QByteArray container;
    };
    std::vector<Entry> m_entries; // least recently put first
};

// Pull-mode PCM for an audio sink: decodes one Opus packet at a time (never
// the whole song), resamples linearly when the sink does not run at 48 kHz,
// and maps the song's channels onto the sink's.
//
// On the way out every song passes a loudness guard, because a contact's song
// is hostile input: after gainQ8, a short-term RMS ceiling of -14 dBFS (400 ms
// window, 50 ms attack, 500 ms release) and a -1 dBFS peak limit, so a 0 dBFS
// square wave plays at an ordinary level. Then the fixed playback gain and
// the fades.
//
// The sink may pull from its own thread, so every member is guarded.
class SongStream final : public QIODevice
{
public:
    // 0.8 × 0.7 headroom, the level of the other UI sounds: OpenChat has no
    // output volume setting and the song has no volume control of its own.
    static constexpr double playbackGain = 0.56;
    static constexpr double rmsCeilingDb = -14.0;
    static constexpr double peakCeilingDb = -1.0;
    static constexpr int rmsWindowMs = 400;
    static constexpr int attackMs = 50;
    static constexpr int releaseMs = 500;
    // A resume or a seek lands mid-waveform; this short ramp keeps it from clicking.
    static constexpr int resumeFadeInMs = 5;

    SongStream(SongContainer song, QAudioFormat sinkFormat, qint64 startSample = 0, QObject *parent = nullptr);
    ~SongStream() override;

    [[nodiscard]] bool isValid() const;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

    // The song position (48 kHz frames) of the next frame the sink will get.
    [[nodiscard]] qint64 framesPlayed() const;
    // The last frame has gone out, or a fade-out has reached silence.
    [[nodiscard]] bool finished() const;
    [[nodiscard]] bool fadedOut() const;
    // Ramps the output to silence over `ms`; the stream then ends.
    void startFadeOut(int ms);
    void seekToSample(qint64 sample);

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    void refillLocked();
    void guardFrameLocked(float *frame);
    [[nodiscard]] qsizetype queuedFramesLocked() const;
    [[nodiscard]] bool finishedLocked() const;
    void writeFrameLocked(char *out, const float *song, float gain) const;

    mutable QMutex m_mutex;
    SongDecoder m_decoder;
    QAudioFormat m_format;
    int m_songChannels = 1;
    int m_sinkChannels = 0;
    int m_sinkRate = 0;
    int m_bytesPerSample = 0;
    double m_step = 1.0; // song frames per sink frame

    // Guarded song frames at 48 kHz, interleaved, waiting to be resampled.
    std::vector<float> m_queue;
    qint64 m_queueBase = 0; // song frame of m_queue's first frame
    double m_readPos = 0.0; // read position in m_queue, in song frames
    qsizetype m_realFrames = 0; // queued frames that are song, not the end pad
    bool m_flushed = false;     // the decoder is done and the end pad is queued

    // Loudness guard state.
    std::vector<float> m_powerRing;
    qsizetype m_ringIndex = 0;
    double m_powerSum = 0.0;
    float m_rmsGain = 1.0F;
    float m_peakGain = 1.0F;

    // Output ramps, counted in sink frames.
    qint64 m_fadeInTotal = 0;
    qint64 m_fadeInDone = 0;
    qint64 m_fadeOutTotal = 0;
    qint64 m_fadeOutLeft = -1; // -1: no fade-out running
    bool m_fadedOut = false;
};

// Where a SongPlayer's audio goes. The system output opens a QAudioSink on
// the default device; tests install a fake with setOutputFactoryForTesting so
// they need no sound card and make no sound.
class SongOutput : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    [[nodiscard]] virtual QAudioFormat format() const = 0;
    // How far ahead of the speaker the device pulls: a fade is heard only
    // after what was pulled before it, so teardown waits this much longer.
    [[nodiscard]] virtual int bufferMs() const = 0;
    virtual bool start(QIODevice *stream) = 0; // pull mode
    virtual void stop() = 0;                    // at once

signals:
    void failed(const QString &message); // the device stopped mid-song
};

// Opens an output for a song with `channels` channels, or returns null and
// sets `error` to a short sentence.
using SongOutputFactory = std::function<std::unique_ptr<SongOutput>(int channels, QString &error)>;

// The profile page's mini player (SPEC §5.4). It never starts by itself:
// play() is the only way sound begins, and nothing in here calls it. Only one
// player sounds in the process: play() fades out whichever was playing.
//
// `active` false (the page popped or covered, the window hidden or minimised)
// fades out over 150 ms and rewinds; `suspended` true (a call rings or
// starts) fades out and keeps the position, and refuses play() until the
// call is over. Qt Multimedia is first touched by play(), so a page that is
// only looked at never loads it.
class SongPlayer : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString songKey READ songKey WRITE setSongKey NOTIFY sourceChanged)
    Q_PROPERTY(bool valid READ valid NOTIFY sourceChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY sourceChanged)
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY stateChanged)
    Q_PROPERTY(bool suspended READ suspended WRITE setSuspended NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    static constexpr int fadeOutMs = 150;
    static constexpr int positionIntervalMs = 100;
    static constexpr int seekStepMs = 5'000; // ← and → on the orb

    explicit SongPlayer(QObject *parent = nullptr);
    ~SongPlayer() override; // stops the sink at once

    [[nodiscard]] QString songKey() const;
    // The same key does nothing (a page refresh never interrupts the song);
    // a new key stops, rewinds and loads it from SongLibrary.
    void setSongKey(const QString &key);
    [[nodiscard]] bool valid() const;
    [[nodiscard]] qint64 durationMs() const;
    [[nodiscard]] qint64 positionMs() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] bool suspended() const;
    void setSuspended(bool suspended);
    [[nodiscard]] bool active() const;
    void setActive(bool active);
    [[nodiscard]] QString error() const;

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void seekBy(qint64 deltaMs);

    // The system's default output, as play() opens it; a null device is
    // "No audio output". Public so a test can drive the real path with one.
    [[nodiscard]] static std::unique_ptr<SongOutput> openSystemOutput(const QAudioDevice &device, int channels,
                                                                      QString &error);
    // Replaces how every player opens its output; an empty factory restores
    // the system output.
    static void setOutputFactoryForTesting(SongOutputFactory factory);
    // An output as play() opens one (the test factory when one is set), for
    // other page sounds: a panel video's.
    [[nodiscard]] static std::unique_ptr<SongOutput> openOutput(int channels, QString &error);
    // Another page sound takes over: the sounding song pauses, and `stop`
    // runs (once) when a song starts playing, so two never sound together.
    // Returns a token for releaseSound().
    static quint64 takeOverSound(std::function<void()> stop);
    // Forgets the stop a sound registered (it ended by itself).
    static void releaseSound(quint64 token);

signals:
    void sourceChanged();
    void positionChanged();
    void stateChanged();

private:
    friend class SongLibrary;

    // A stream and the output pulling it. The output is declared last so it
    // is destroyed (and stops pulling) before the stream it reads.
    struct Voice final {
        std::unique_ptr<SongStream> stream;
        std::unique_ptr<SongOutput> output;
        explicit operator bool() const noexcept { return output != nullptr; }
    };

    static void songArrived(const QString &key);
    void load();
    void unload();
    // Fades the playing voice out (or lets it play out its buffer when
    // fadeMs is 0) and tears it down afterwards; flips the state at once.
    void retireVoice(int fadeMs);
    void killRetiring();
    void updatePosition();
    void setPositionMs(qint64 positionMs);
    void setError(const QString &error);
    void outputFailed(const SongOutput *output, const QString &message);

    QString m_key;
    std::optional<SongContainer> m_song; // loaded and validated
    bool m_loaded = false;               // the library had bytes for m_key
    qint64 m_durationMs = 0;
    qint64 m_positionMs = 0;
    qint64 m_voiceStartMs = 0; // where the playing voice started (a seek moves it)
    bool m_playing = false;
    bool m_suspended = false;
    bool m_active = true;
    QString m_error;
    Voice m_voice;
    Voice m_retiring;
    QTimer m_positionTimer;
    QTimer m_retireTimer;
};

} // namespace OpenChat
