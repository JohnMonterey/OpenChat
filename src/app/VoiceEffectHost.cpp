#include "app/VoiceEffectHost.h"

#include "effects/LocalVoiceEffect.h"
#include "effects/PluginScanner.h"
#include "effects/VoiceEffectChain.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>
#include <utility>

namespace OpenChat {

namespace {

VoiceEffectHost *g_instance = nullptr;

constexpr auto settingsGroup = "VoiceEffectHost";
constexpr auto inventoryKey = "inventory";
constexpr auto consentsKey = "consents";

// Prefix the editor uses for the effects that are ours rather than somebody
// else's. Those are applied through MicrophoneSettings and never reach a
// plugin chain.
constexpr QLatin1String builtinPrefix("builtin:");

QString tr(const char *text)
{
    return QCoreApplication::translate("VoiceEffectHost", text);
}

// The plural forms, which need the count to pick one.
QString trCount(const char *text, int count)
{
    return QCoreApplication::translate("VoiceEffectHost", text, nullptr, count);
}

// The heading the picker files a plugin under. Plugin features are free-form by
// specification -- CLAP suggests a vocabulary and VST3 ships its own -- so this
// recognises the ones that actually appear and otherwise says nothing, which
// makes the picker fall back to grouping by format.
QString categoryFor(const AudioPluginDescriptor &descriptor)
{
    static const QList<QPair<QLatin1String, const char *>> known{
        {QLatin1String("reverb"), QT_TR_NOOP("Space & ambience")},
        {QLatin1String("delay"), QT_TR_NOOP("Space & ambience")},
        {QLatin1String("pitch"), QT_TR_NOOP("Pitch & character")},
        {QLatin1String("distortion"), QT_TR_NOOP("Pitch & character")},
        {QLatin1String("modulation"), QT_TR_NOOP("Radio & modulation")},
        {QLatin1String("filter"), QT_TR_NOOP("Radio & modulation")},
        {QLatin1String("equalizer"), QT_TR_NOOP("Tone & dynamics")},
        {QLatin1String("eq"), QT_TR_NOOP("Tone & dynamics")},
        {QLatin1String("compressor"), QT_TR_NOOP("Tone & dynamics")},
        {QLatin1String("dynamics"), QT_TR_NOOP("Tone & dynamics")},
        {QLatin1String("gate"), QT_TR_NOOP("Tone & dynamics")},
        {QLatin1String("restoration"), QT_TR_NOOP("Cleanup")},
        {QLatin1String("denoise"), QT_TR_NOOP("Cleanup")},
        {QLatin1String("utility"), QT_TR_NOOP("Utility")},
    };

    for (const QString &feature : descriptor.features) {
        const QString lowered = feature.toLower();
        for (const auto &[needle, heading] : known) {
            if (lowered.contains(needle))
                return tr(heading);
        }
    }
    return {};
}

QVariantMap recordFor(const AudioPluginDescriptor &descriptor)
{
    return QVariantMap{
        {QStringLiteral("id"), descriptor.id.toString()},
        {QStringLiteral("name"), descriptor.name},
        // Upper case because this is a label in a list, not the identifier;
        // AudioPluginId keeps the canonical lower-case form.
        {QStringLiteral("format"), audioPluginFormatName(descriptor.id.format).toUpper()},
        {QStringLiteral("category"), categoryFor(descriptor)},
        {QStringLiteral("vendor"), descriptor.vendor},
    };
}

} // namespace

VoiceEffectHost::VoiceEffectHost(QObject *parent)
    : QObject(parent)
    , m_alive(std::make_shared<std::atomic_bool>(true))
{
    if (!g_instance)
        g_instance = this;
    load();
}

VoiceEffectHost::~VoiceEffectHost()
{
    // The scan thread outlives nothing: it is told the host is gone, then
    // joined, so a plugin still being probed cannot deliver into a destroyed
    // object or outlive the process's own teardown.
    m_alive->store(false);
    if (m_scanThread.joinable())
        m_scanThread.join();
    if (g_instance == this)
        g_instance = nullptr;
}

VoiceEffectHost *VoiceEffectHost::instance()
{
    return g_instance;
}

void VoiceEffectHost::scanPlugins()
{
    if (m_scanning)
        return;
    if (m_scanThread.joinable())
        m_scanThread.join(); // the previous scan, already finished

    m_scanning = true;
    m_scanError.clear();
    emit scanChanged();

    m_scanThread = std::thread([this] { runScan(); });
}

void VoiceEffectHost::runScan()
{
    QList<AudioPluginDescriptor> found;
    QStringList failures;

    for (const AudioPluginFormat format : {AudioPluginFormat::Vst3, AudioPluginFormat::Clap}) {
        const QStringList bundles = PluginScanner::findBundles(format);
        for (const QString &bundle : bundles) {
            if (!m_alive->load())
                return; // the host is going away; nothing to deliver into
            PluginError error;
            const QList<AudioPluginDescriptor> described =
                PluginScanner::describe(format, bundle, error);
            if (error && described.isEmpty()) {
                // Named, not silently dropped: a plugin the user can see in
                // their file manager but not in this list needs a reason.
                failures.append(QFileInfo(bundle).fileName());
                continue;
            }
            found.append(described);
        }
    }

    std::sort(found.begin(), found.end(),
              [](const AudioPluginDescriptor &first, const AudioPluginDescriptor &second) {
                  return first.name.compare(second.name, Qt::CaseInsensitive) < 0;
              });

    QString error;
    if (found.isEmpty()) {
        error = failures.isEmpty()
            ? tr("No VST3 or CLAP plugins were found in the usual places on this computer.")
            : tr("No plugins could be read. %1 could not be opened.")
                  .arg(failures.join(QStringLiteral(", ")));
    } else if (!failures.isEmpty()) {
        error = trCount(QT_TRANSLATE_NOOP("VoiceEffectHost",
                                          "%n plugin file(s) could not be read and were skipped."),
                        static_cast<int>(failures.size()));
    }

    // Back to the thread that owns this object before touching anything on it.
    QMetaObject::invokeMethod(
        this, [this, found, error] { finishScan(found, error); }, Qt::QueuedConnection);
}

void VoiceEffectHost::finishScan(QList<AudioPluginDescriptor> found, QString error)
{
    m_inventory = std::move(found);
    m_scanError = std::move(error);
    m_scanning = false;
    m_scanned = true;
    saveInventory();
    publishInventory();
    emit scanChanged();
}

void VoiceEffectHost::publishInventory()
{
    QVariantList records;
    records.reserve(m_inventory.size());
    for (const AudioPluginDescriptor &descriptor : std::as_const(m_inventory))
        records.append(recordFor(descriptor));
    m_plugins = records;
    emit pluginsChanged();
}

const AudioPluginDescriptor *VoiceEffectHost::describedBy(const AudioPluginId &id) const
{
    const auto found = std::find_if(m_inventory.cbegin(), m_inventory.cend(),
                                    [&id](const AudioPluginDescriptor &candidate) {
                                        return candidate.id == id;
                                    });
    return found == m_inventory.cend() ? nullptr : &*found;
}

bool VoiceEffectHost::ensureConsent(const QString &bundlePath, AudioPluginFormat format)
{
    const QString loadable = PluginScanner::loadablePathFor(format, bundlePath);
    if (loadable.isEmpty())
        return false;

    const PluginConsent consent =
        AudioPluginTrust::recordConsent(loadable, QDateTime::currentMSecsSinceEpoch());
    if (!consent.isValid())
        return false; // an untrusted or unreadable path: fails closed

    // Replace rather than append when the file is already known: a plugin the
    // user has just re-picked after a vendor update is consent for the bytes
    // in front of them now, and keeping the old record would leave a hash that
    // no longer matches anything.
    for (PluginConsent &existing : m_consents) {
        if (existing.path == consent.path) {
            existing = consent;
            saveConsents();
            return true;
        }
    }
    m_consents.append(consent);
    saveConsents();
    return true;
}

void VoiceEffectHost::applyPreset(const QVariantMap &preset)
{
    VoiceEffectChainConfig chain;
    chain.enabled = true;
    QStringList missing;

    // Not `slots`: that is a Qt keyword macro, and a variable of that name
    // silently vanishes -- the same reason VoiceEffectChainConfig calls its own
    // list `stages`.
    const QVariantList rack = preset.value(QStringLiteral("slots")).toList();
    for (const QVariant &entry : rack) {
        const QVariantMap slot = entry.toMap();
        const QString effectId = slot.value(QStringLiteral("effectId")).toString();
        if (effectId.isEmpty() || effectId.startsWith(builtinPrefix))
            continue; // an empty slot, or one of ours rather than a plugin
        if (chain.stages.size() >= VoiceEffectChainConfig::maxStages)
            break;

        VoiceEffectStage stage;
        stage.id = AudioPluginId::fromString(effectId);
        if (!stage.id.isValid()) {
            missing.append(slot.value(QStringLiteral("name")).toString());
            continue;
        }
        stage.enabled = slot.value(QStringLiteral("enabled"), true).toBool();
        const double mix = slot.value(QStringLiteral("mix"), 1.0).toDouble();
        stage.mix = std::clamp(mix, 0.0, 1.0);

        // The user put this plugin in a rack and switched it on: that is the
        // act consent records. A file that cannot be trusted is left out of the
        // chain rather than loaded anyway.
        if (stage.enabled && !ensureConsent(stage.id.bundlePath, stage.id.format)) {
            missing.append(slot.value(QStringLiteral("name")).toString());
            continue;
        }
        chain.stages.append(stage);
    }

    m_chain = chain;

    // Build it once, here, so the editor can say what actually loaded instead
    // of implying that everything did. The call builds its own; this one is a
    // rehearsal and is thrown away immediately.
    m_chainError.clear();
    m_activeStageCount = 0;
    if (m_chain.hasWork()) {
        VoiceEffectChain rehearsal;
        (void)rehearsal.build(m_chain, m_consents);
        m_activeStageCount = rehearsal.activeStageCount();
        m_chainError = rehearsal.error();
        if (m_chainError.isEmpty()) {
            for (const VoiceEffectChain::StageStatus &status : rehearsal.stages()) {
                if (status.error) {
                    m_chainError = status.error.message;
                    break;
                }
            }
        }
    }
    if (!missing.isEmpty() && m_chainError.isEmpty()) {
        m_chainError =
            trCount(QT_TRANSLATE_NOOP("VoiceEffectHost",
                                      "%n effect(s) in this preset could not be used."),
                    static_cast<int>(missing.size()));
    }

    emit chainChanged();
    emit factoryChanged();
}

void VoiceEffectHost::clearPreset()
{
    if (!m_chain.hasWork() && m_chainError.isEmpty() && m_activeStageCount == 0)
        return;
    m_chain = VoiceEffectChainConfig{};
    m_chainError.clear();
    m_activeStageCount = 0;
    emit chainChanged();
    emit factoryChanged();
}

VoiceEffectFactory VoiceEffectHost::factory() const
{
    if (!m_chain.hasWork())
        return {};
    return LocalVoiceEffect::factoryFor(m_chain, m_consents);
}

void VoiceEffectHost::load()
{
    QSettings settings;
    settings.beginGroup(QLatin1String(settingsGroup));

    const QJsonArray consents =
        QJsonDocument::fromJson(settings.value(QLatin1String(consentsKey)).toByteArray())
            .array();
    for (const QJsonValue &value : consents) {
        const PluginConsent consent = PluginConsent::fromJson(value.toObject());
        if (consent.isValid())
            m_consents.append(consent);
    }

    const QJsonArray inventory =
        QJsonDocument::fromJson(settings.value(QLatin1String(inventoryKey)).toByteArray())
            .array();
    for (const QJsonValue &value : inventory) {
        const AudioPluginDescriptor descriptor =
            AudioPluginDescriptor::fromJson(value.toObject());
        if (descriptor.isValid())
            m_inventory.append(descriptor);
    }
    // A stored inventory counts as scanned: the user asked for a scan once and
    // should not be asked again on every launch. It is not re-verified here,
    // because every load re-hashes anyway and a stale entry surfaces as a
    // plugin that will not start rather than as one that quietly runs.
    m_scanned = !m_inventory.isEmpty();
    if (m_scanned)
        publishInventory();
}

void VoiceEffectHost::saveInventory() const
{
    QJsonArray array;
    for (const AudioPluginDescriptor &descriptor : m_inventory)
        array.append(descriptor.toJson());
    QSettings settings;
    settings.beginGroup(QLatin1String(settingsGroup));
    settings.setValue(QLatin1String(inventoryKey), QJsonDocument(array).toJson(QJsonDocument::Compact));
}

void VoiceEffectHost::saveConsents() const
{
    QJsonArray array;
    for (const PluginConsent &consent : m_consents)
        array.append(consent.toJson());
    QSettings settings;
    settings.beginGroup(QLatin1String(settingsGroup));
    settings.setValue(QLatin1String(consentsKey), QJsonDocument(array).toJson(QJsonDocument::Compact));
}

} // namespace OpenChat
