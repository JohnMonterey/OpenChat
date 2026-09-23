#pragma once
#include <QAudioSink>
#include <QElapsedTimer>
#include <QObject>
#include <QIODevice>
#include <QMutex>
#include <memory>

namespace OpenChat {
// One voice, pulled by the audio device. Small buffers keep crossing-to-click
// latency bounded. Silence between events costs no allocations or new sources.
// A retrigger replaces the voice; the outgoing sample keeps sounding through a
// short fading tail, because cutting a decaying sample mid-rattle clicks.
class CaseSampleStream final : public QIODevice {
public:
    CaseSampleStream() { open(QIODevice::ReadOnly | QIODevice::Unbuffered); }
    void trigger(const QByteArray &sample);
    // Width of the fade applied to the voice a retrigger replaces, in bytes.
    // Zero restores a hard cut. Always even: the fade works on 16-bit words.
    void setCrossfadeBytes(qint64 bytes) { m_crossfade = bytes & ~qint64(1); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 65536 + QIODevice::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 maximum) override;
    qint64 writeData(const char *, qint64) override { return -1; }
private:
    QMutex m_mutex;
    QByteArray m_sample, m_tail;
    qint64 m_cursor = 0, m_tailCursor = 0;
    qint64 m_crossfade = 0;
};
class CaseAudio final : public QObject {
public:
    explicit CaseAudio(QObject *parent = nullptr) : QObject(parent) {}
    ~CaseAudio() override { stop(); }
    void start();
    // The popup opening shows the case; returns the sample's length in ms, or
    // 0 when nothing sounded (muted path never reaches here; no device or a
    // missing sample does).
    int display();
    void tick();
    // The reveal; returns how long the sink must stay open to play it out.
    int impact();
    void stop();
private:
    std::unique_ptr<QAudioSink> m_sink;
    CaseSampleStream m_stream;
    QByteArray m_display, m_tick, m_open;
    QElapsedTimer m_lastTick;
};
}
