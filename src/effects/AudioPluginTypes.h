#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace OpenChat {

// Which ABI a plugin file speaks. The two are hosted by different code and
// nothing else in the subsystem branches on this -- a descriptor, a slot and a
// parameter mean the same thing whichever produced them, which is the whole
// point of keeping the format in the identifier rather than in the interface.
enum class AudioPluginFormat {
    Clap,
    Vst3,
};

[[nodiscard]] QString audioPluginFormatName(AudioPluginFormat format);
[[nodiscard]] QString audioPluginFormatExtension(AudioPluginFormat format);

// Names one plugin inside one file on disk.
//
// A bundle is not a plugin: a single .clap or .vst3 routinely publishes dozens
// of them (the LSP bundle installed on a typical Linux box publishes 176), so a
// path alone is ambiguous and a path is what a user hands us. The entry id is
// the plugin's own published identifier -- CLAP's clap_plugin_descriptor::id,
// VST3's class UID as a hex string -- and it is deliberately not an index into
// the factory, because an index silently means a different plugin after the
// vendor adds one.
struct AudioPluginId final {
    AudioPluginFormat format = AudioPluginFormat::Clap;
    // Absolute, symlinks already resolved. The resolved form is what consent is
    // recorded against, so storing the pre-resolution path would let a swapped
    // symlink point an approved entry at a different file.
    QString bundlePath;
    QString entryId;

    [[nodiscard]] bool isValid() const noexcept
    {
        return !bundlePath.isEmpty() && !entryId.isEmpty();
    }

    // "clap:/usr/lib/clap/Foo.clap#com.vendor.foo". Round-trips through
    // fromString; the separators cannot occur in a format name, and a path or
    // entry id containing '#' round-trips because only the LAST '#' splits.
    [[nodiscard]] QString toString() const;
    [[nodiscard]] static AudioPluginId fromString(const QString &text);

    [[nodiscard]] bool operator==(const AudioPluginId &other) const noexcept
    {
        return format == other.format && bundlePath == other.bundlePath
            && entryId == other.entryId;
    }
};

[[nodiscard]] inline size_t qHash(const AudioPluginId &id, size_t seed = 0) noexcept
{
    return qHashMulti(seed, static_cast<int>(id.format), id.bundlePath, id.entryId);
}

// One knob, as the plugin describes itself.
//
// Values here are PLAIN -- in the plugin's own units, between minValue and
// maxValue -- not normalised to [0, 1]. CLAP is natively plain and VST3 is
// natively normalised, so one of the two backends has to convert; doing it in
// the VST3 backend means a settings panel binds to "3.5 dB" rather than to
// "0.62 of whatever this happens to mean", and a stored chain survives a plugin
// update that changes a range.
struct AudioPluginParameter final {
    // The plugin's own handle for this parameter, NOT its index. CLAP hashes
    // them (LSP's "Output gain" is 3137167283) and VST3 assigns them; either
    // way an index is only stable until the vendor inserts a parameter.
    quint32 id = 0;
    QString name;
    QString module; // Group path the plugin filed it under; empty if none.
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    // Whether the plugin only accepts whole numbers here. A UI draws these as a
    // discrete control; passing 1.5 to a stepped parameter is a plugin-defined
    // outcome and rarely the one intended.
    bool stepped = false;
    // Reported but not offered. DPF-based plugins publish "Sample Rate" and
    // "Buffer Size" as read-only hidden parameters, and a strip that showed
    // those would be showing the user our own frame configuration as a knob.
    bool readOnly = false;
    bool hidden = false;

    [[nodiscard]] double clampToRange(double value) const noexcept;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AudioPluginParameter fromJson(const QJsonObject &object);
};

// Everything a probe learned about one plugin, which is everything a settings
// panel can show without loading it again.
//
// Produced by scanning, cached, and persisted. The audio path never consults
// one: by the time a frame is moving, the questions it answers have all been
// settled.
struct AudioPluginDescriptor final {
    AudioPluginId id;
    QString name;
    QString vendor;
    QString version;
    QString description;
    // The plugin's self-declared categories ("audio-effect", "reverb", "Fx").
    // Free-form by specification, so useful for grouping and useless for logic.
    QStringList features;

    // The shape the host has to feed it. Counted across ALL declared ports,
    // sidechains included, because a host that allocates only the main port
    // segfaults the moment a plugin dereferences the sidechain it declared.
    int inputPortCount = 0;
    int outputPortCount = 0;
    int mainInputChannels = 0;
    int mainOutputChannels = 0;

    // Reported by the plugin at activation, added straight to mouth-to-ear
    // delay, never compensated -- see docs/voice-effects.md. Zero for most
    // effects; ZaMaximX2 reports 480 samples (10 ms) on this pipeline.
    int latencySamples = 0;

    QList<AudioPluginParameter> parameters;

    // What the file looked like when it was probed. A later load re-hashes and
    // refuses on mismatch, so an entry whose bytes changed since the user
    // approved them cannot quietly load.
    QString sha256;
    qint64 fileSize = 0;

    [[nodiscard]] bool isValid() const noexcept { return id.isValid(); }
    // True when the plugin takes and returns exactly one channel on its main
    // ports, so no duplication or channel-picking happens at all.
    [[nodiscard]] bool isNativeMono() const noexcept
    {
        return mainInputChannels == 1 && mainOutputChannels == 1;
    }

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AudioPluginDescriptor fromJson(const QJsonObject &object);
};

// One position in the chain: which plugin, whether it is switched on, and
// whatever the user has changed about it.
//
// A rack of these would conventionally be called slots, and is not, because
// `slots` is a Qt keyword macro and a member of that name silently vanishes.
//
// State and parameters are kept side by side rather than one being derived from
// the other. The opaque state blob is the plugin's own and captures things no
// parameter exposes, but it is version-specific and handing a stale one to
// load() is a crash rather than a wrong setting -- so the parameter snapshot is
// the fallback when the blob is refused, and the reason both are stored.
struct VoiceEffectStage final {
    AudioPluginId id;
    bool enabled = true;
    // How much of this stage's output survives, 0 dry to 1 wet, blended
    // against what went into it.
    //
    // Per stage rather than once for the chain, because the useful setting
    // differs per effect in the same rack: a gate is meaningless at anything
    // but fully wet, while the reverb after it is unusable anywhere near it.
    double mix = 1.0;
    QByteArray state;
    QString stateVersion;
    QHash<quint32, double> parameters;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static VoiceEffectStage fromJson(const QJsonObject &object);
};

// The whole outgoing effect, in order.
//
// Series, not parallel: a chain is what a voice goes through, and every effect
// people ask for on a call (gate, then pitch, then a little reverb) is a
// sequence. Parallel routing needs a mixer, which is exactly the thing this is
// not.
struct VoiceEffectChainConfig final {
    // Eight is not a technical limit, it is a latency one. Each stage adds its
    // own reported delay to a conversation that has no budget to spare, and a
    // user who has stacked eight has already left the range where a call is
    // pleasant.
    static constexpr int maxStages = 8;

    bool enabled = false;
    QList<VoiceEffectStage> stages;

    [[nodiscard]] bool hasWork() const noexcept;
    [[nodiscard]] QByteArray toJson() const;
    [[nodiscard]] static VoiceEffectChainConfig fromJson(const QByteArray &json);
};

// Why a plugin is not playing. Each maps to one sentence a settings panel can
// show without the user reading a log, which is why they are this specific:
// "it did not work" is not something anybody can act on.
enum class PluginErrorCode {
    None,
    // The path itself was refused before anything was opened -- not absolute,
    // not a regular file, or somewhere any other user could rewrite it.
    UntrustedPath,
    // The bytes changed since the user approved them. Deliberately not
    // recoverable without a fresh decision; a vendor's own auto-update is
    // exactly the event this exists to surface.
    HashMismatch,
    // The user has never approved this file.
    NotApproved,
    // dlopen failed, or the file is not a plugin of the format claimed.
    BadBundle,
    // A shared library the plugin needs is not installed.
    DependencyMissing,
    // The bundle loaded but publishes no plugin with this entry id.
    EntryNotFound,
    // The plugin refused to initialise or activate.
    ActivationFailed,
    // Its port layout cannot be adapted to a mono voice call.
    ChannelLayoutUnsupported,
    // It died or hung while being probed, and will not be probed again.
    ProbeCrashed,
    ProbeTimedOut,
    // It produced non-finite samples and was switched off mid-call.
    ProducedNonFinite,
    // It could not deliver frames fast enough and was switched off.
    TooSlow,
};

struct PluginError final {
    PluginErrorCode code = PluginErrorCode::None;
    // Already translated, already safe to put in front of a user, and never
    // containing anything the plugin chose -- a plugin's own strings are
    // attacker-controlled text and do not belong in a security-relevant
    // sentence.
    QString message;

    [[nodiscard]] bool isError() const noexcept { return code != PluginErrorCode::None; }
    explicit operator bool() const noexcept { return isError(); }
};

[[nodiscard]] PluginError makePluginError(PluginErrorCode code, const QString &message);

} // namespace OpenChat
