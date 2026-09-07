#pragma once

#include "effects/AudioPluginTypes.h"
#include "media/AudioTypes.h"

#include <QByteArray>
#include <QList>
#include <QString>

#include <memory>

namespace OpenChat {

// One loaded, activated plugin, reduced to what a voice chain asks of it.
//
// This is the seam between the two plugin ABIs and everything above them. CLAP
// and VST3 disagree about almost everything -- how a plugin is found in a file,
// how buses are negotiated, whether parameters are plain or normalised, how
// state is streamed -- and every one of those disagreements is settled below
// this interface. Above it there is one kind of effect.
//
// The audio call is processMono() and it takes a bare float pointer rather than
// an AudioFrame on purpose: a chain runs N of these back to back over the same
// scratch buffer, and going through QByteArray at each step would allocate once
// per plugin per frame, fifty times a second.
class AudioPluginInstance
{
public:
    virtual ~AudioPluginInstance() = default;

    // Exactly CallAudioFormat::samplesPerFrame samples, processed in place.
    //
    // noexcept because there is nothing sensible to do with an exception here
    // and a chain must not unwind through a plugin's stack frame; a plugin that
    // fails reports it by leaving the buffer alone.
    virtual void processMono(float *samples) noexcept = 0;

    // Clears filters, delay lines and reverb tails without reloading anything.
    virtual void reset() noexcept = 0;

    [[nodiscard]] virtual const AudioPluginDescriptor &descriptor() const noexcept = 0;
    [[nodiscard]] virtual int latencySamples() const noexcept = 0;

    // Plain values, in the parameter's own units. Queued for the head of the
    // next frame rather than applied immediately, because a parameter change is
    // an event in both ABIs and applying one mid-block is how a plugin gets a
    // discontinuity it did not ask for.
    virtual void setParameter(quint32 parameterId, double value) = 0;
    [[nodiscard]] virtual double parameterValue(quint32 parameterId) const = 0;
    // What the plugin would print for this value -- "-6.0 dB", "440 Hz". The
    // one thing a settings strip genuinely cannot work out for itself.
    [[nodiscard]] virtual QString parameterText(quint32 parameterId, double value) const = 0;

    // The plugin's own opaque blob. Empty when it publishes no state
    // extension, which is not an error: the parameter snapshot is the fallback
    // and covers everything a user actually set.
    [[nodiscard]] virtual QByteArray saveState() const = 0;
    virtual bool loadState(const QByteArray &state) = 0;
};

// One opened plugin file, and the plugins inside it.
//
// Kept separate from the instance because a bundle is not a plugin: one file
// routinely publishes dozens, and opening it is the expensive, dangerous half
// (dlopen runs the file's constructors) while creating an instance from an
// already-open module is cheap.
class AudioPluginModule
{
public:
    virtual ~AudioPluginModule() = default;

    // Every plugin the file publishes. Populated at open; introspecting one
    // costs an instantiate-and-destroy, so this is where the cost lives rather
    // than in a later scan.
    [[nodiscard]] virtual QList<AudioPluginDescriptor> descriptors() const = 0;

    // Instantiates and activates one plugin for 48 kHz mono 960-sample frames.
    // Null on failure, with error set to the reason.
    [[nodiscard]] virtual std::unique_ptr<AudioPluginInstance>
    createInstance(const QString &entryId, PluginError &error) = 0;
};

} // namespace OpenChat
