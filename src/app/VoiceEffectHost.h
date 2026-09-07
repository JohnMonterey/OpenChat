#pragma once

#include "effects/AudioPluginTrust.h"
#include "effects/AudioPluginTypes.h"
#include "effects/VoiceEffect.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <thread>

namespace OpenChat {

// What the Custom Vocal FX editor talks to: the scan that finds plugins on this
// machine, the consent that lets one be loaded, and the chain a call runs.
//
// The UI owns the preset documents (see docs/vocal-fx-ui.md) and this owns
// everything that touches a file. The split matters because the editor is
// allowed to be optimistic and this is not: a slot referring to a plugin that
// has since been uninstalled is an ordinary thing for a stored document to
// contain, and the answer is an inactive stage and a sentence saying so, never
// a UI claiming a chain is processing audio that is not.
//
// SCANNING IS NOT LOADING FOR A CALL, but it is still running somebody's code.
// PluginScanner::describe dlopens each bundle to ask what it publishes, so a
// scan is gated on the path checks (nobody else can rewrite the file) even
// though it is not gated on consent — there is nothing for a user to consent to
// until they have been shown a list. Consent is recorded later, against the
// bytes, at the moment a plugin is put into a chain: that is the point where
// the user has actually named it.
class VoiceEffectHost final : public QObject
{
    Q_OBJECT
    // True only while an explicitly requested scan is running. Nothing here
    // ever starts one on its own.
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanChanged)
    // True once an inventory exists, from this session or a previous one.
    Q_PROPERTY(bool scanned READ hasScanned NOTIFY scanChanged)
    // Empty on success; otherwise one sentence about why the scan found
    // nothing usable.
    Q_PROPERTY(QString scanError READ scanError NOTIFY scanChanged)
    // The inventory as the picker consumes it: {id, name, format, category}.
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    // What the applied chain is actually doing. Empty while healthy; otherwise
    // why a stage is not playing, so the editor can say so rather than imply
    // the chain is live.
    Q_PROPERTY(QString chainError READ chainError NOTIFY chainChanged)
    Q_PROPERTY(int activeStageCount READ activeStageCount NOTIFY chainChanged)

public:
    explicit VoiceEffectHost(QObject *parent = nullptr);
    ~VoiceEffectHost() override;

    // The process-wide instance, or nullptr before one exists. Same contract as
    // MicrophoneSettings: main() makes it, the QML singleton hands out that one.
    [[nodiscard]] static VoiceEffectHost *instance();

    [[nodiscard]] bool isScanning() const { return m_scanning; }
    [[nodiscard]] bool hasScanned() const { return m_scanned; }
    [[nodiscard]] QString scanError() const { return m_scanError; }
    [[nodiscard]] QVariantList plugins() const { return m_plugins; }
    [[nodiscard]] QString chainError() const { return m_chainError; }
    [[nodiscard]] int activeStageCount() const { return m_activeStageCount; }

    // Walks the conventional plugin directories for both formats and asks each
    // bundle what it publishes. Runs off the GUI thread — a scan opens every
    // plugin on the machine, which on a box with a DAW installed is seconds,
    // not milliseconds.
    Q_INVOKABLE void scanPlugins();

    // Applies one preset document: {id, name, slots}, each slot
    // {effectId, name, enabled, mix}. An empty effectId is an unused slot,
    // "builtin:<id>" is handled by MicrophoneSettings and skipped here, and
    // anything else is a plugin id from the inventory.
    //
    // Recording consent here, rather than at scan time, is what keeps "a plugin
    // runs because the user named it" true: this is the call that happens
    // because somebody put a plugin in a rack and switched it on.
    Q_INVOKABLE void applyPreset(const QVariantMap &preset);

    // Drops the custom chain, for switching back to a built-in single effect.
    Q_INVOKABLE void clearPreset();

    // The chain as a call should build it. Empty when no preset is applied, so
    // an engine handed this sends the microphone through unchanged.
    [[nodiscard]] VoiceEffectFactory factory() const;
    [[nodiscard]] const VoiceEffectChainConfig &chain() const noexcept { return m_chain; }

signals:
    void scanChanged();
    void pluginsChanged();
    void chainChanged();
    // The applied chain changed: whoever owns a CallEngine should hand it
    // factory() again.
    void factoryChanged();

private:
    // Called on the scanning thread; hands its result back through the event
    // loop, because everything below touches Qt objects owned by this thread.
    void runScan();
    void finishScan(QList<AudioPluginDescriptor> found, QString error);
    void publishInventory();
    void load();
    void saveInventory() const;
    void saveConsents() const;
    // Consent for one bundle, recorded if this is the first time it is used.
    [[nodiscard]] bool ensureConsent(const QString &bundlePath, AudioPluginFormat format);
    [[nodiscard]] const AudioPluginDescriptor *describedBy(const AudioPluginId &id) const;

    QList<AudioPluginDescriptor> m_inventory;
    QList<PluginConsent> m_consents;
    QVariantList m_plugins;
    VoiceEffectChainConfig m_chain;

    QString m_scanError;
    QString m_chainError;
    int m_activeStageCount = 0;
    bool m_scanning = false;
    bool m_scanned = false;

    std::thread m_scanThread;
    // Set when this object is going away, so a scan still running does not
    // deliver into a destroyed host.
    std::shared_ptr<std::atomic_bool> m_alive;
};

} // namespace OpenChat
