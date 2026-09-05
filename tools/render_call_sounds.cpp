// Renders OpenChat's call sounds to WAV files.
//
// The sounds are synthesised in code rather than shipped as assets, so this is
// how you get hold of them as audio: to listen to, to hand to someone else, or
// to check a change before it ships. Run it again after editing
// src/call/CallSounds.cpp and the files update.
//
//   openchat-render-call-sounds [output-directory]
//
// Writes one 48 kHz mono 16-bit WAV per sound, plus a single preview track that
// plays all of them in order so the set can be judged as a whole.

#include "call/CallSounds.h"
#include "media/AudioTypes.h"
#include "media/WavFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

#include <cmath>

namespace {

[[nodiscard]] OpenChat::WavAudio wrap(QVector<qint16> samples)
{
    OpenChat::WavAudio audio;
    audio.sampleRate = OpenChat::CallAudioFormat::sampleRate;
    audio.channels = OpenChat::CallAudioFormat::channels;
    audio.samples = std::move(samples);
    return audio;
}

// The looping sounds are one cycle long, which is the truth of what loops but is
// over quickly. The preview repeats them so their cadence is audible.
[[nodiscard]] int previewRepeatsFor(OpenChat::CallSound sound)
{
    switch (sound) {
    case OpenChat::CallSound::Ringback:
    case OpenChat::CallSound::IncomingRing:
        return 3;
    default:
        return 1;
    }
}

[[nodiscard]] double peakOf(const QVector<qint16> &samples)
{
    double peak = 0.0;
    for (const qint16 sample : samples)
        peak = std::max(peak, std::abs(sample / 32768.0));
    return peak;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    QTextStream out(stdout);

    const QString directory = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral("call-sounds");
    if (!QDir().mkpath(directory)) {
        QTextStream(stderr) << "cannot create " << directory << "\n";
        return 1;
    }
    const QString absolute = QDir(directory).absolutePath();

    QVector<qint16> preview;
    const QVector<qint16> gap(OpenChat::CallAudioFormat::samplesForMs(600), 0);

    for (const OpenChat::CallSound sound : OpenChat::CallSoundBoard::allSounds()) {
        const QString name = OpenChat::CallSoundBoard::nameFor(sound);
        const QVector<qint16> &samples = OpenChat::CallSoundBoard::samplesFor(sound);
        const QString path = absolute + QLatin1Char('/') + name + QStringLiteral(".wav");

        const auto written = OpenChat::WavFile::writeFile(path, wrap(samples));
        if (!written.hasValue()) {
            QTextStream(stderr) << "failed to write " << path << "\n";
            return 1;
        }

        for (int repeat = 0; repeat < previewRepeatsFor(sound); ++repeat)
            preview += samples;
        preview += gap;

        out << QStringLiteral("%1  %2 s  peak %3\n")
                   .arg(name, -16)
                   .arg(OpenChat::CallAudioFormat::msForSamples(samples.size()) / 1000.0, 5, 'f', 2)
                   .arg(peakOf(samples), 4, 'f', 2);
    }

    const QString previewPath = absolute + QStringLiteral("/all-call-sounds.wav");
    if (!OpenChat::WavFile::writeFile(previewPath, wrap(preview)).hasValue()) {
        QTextStream(stderr) << "failed to write " << previewPath << "\n";
        return 1;
    }

    out << "\nwritten to " << absolute << "\n";
    return 0;
}
