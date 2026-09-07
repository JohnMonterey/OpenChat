#include <QtTest>

#include "effects/AudioPluginTrust.h"
#include "effects/AudioPluginTypes.h"
#include "effects/LocalVoiceEffect.h"
#include "effects/PluginScanner.h"
#include "effects/SampleBridge.h"
#include "effects/VoiceEffectChain.h"
#include "media/AudioTypes.h"

#include <QTemporaryDir>

#include <cmath>

using namespace OpenChat;

namespace {

// The fixture bundle this project builds beside the test. Its absence is a
// build failure, not a reason to skip: the whole point of building our own
// plugin is that "a real plugin loaded and changed the audio" is asserted
// everywhere, not only on machines that happen to have a DAW installed.
QString fixturePath()
{
    return QStringLiteral(OPENCHAT_TEST_PLUGIN_DIR) + QStringLiteral("/oc_test_plugins.clap");
}

AudioPluginId fixtureId(const QString &entry)
{
    AudioPluginId id;
    id.format = AudioPluginFormat::Clap;
    id.bundlePath = fixturePath();
    id.entryId = entry;
    return id;
}

// Consent for the fixture, as the user granting it would produce.
QList<PluginConsent> fixtureConsent()
{
    const PluginConsent consent = AudioPluginTrust::recordConsent(fixturePath(), 1);
    return consent.isValid() ? QList<PluginConsent>{consent} : QList<PluginConsent>{};
}

AudioFrame toneFrame(double amplitude, double frequency = 440.0, int frameIndex = 0)
{
    AudioFrame frame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        const double t =
            double(frameIndex * CallAudioFormat::samplesPerFrame + i) / CallAudioFormat::sampleRate;
        const double value = amplitude * std::sin(2.0 * M_PI * frequency * t);
        const qint16 sample = static_cast<qint16>(std::lround(value * 32767.0));
        qToLittleEndian<qint16>(sample, frame.data() + i * CallAudioFormat::bytesPerSample);
    }
    return frame;
}

double frameRms(const AudioFrame &frame)
{
    double sum = 0.0;
    for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
        const double sample =
            qFromLittleEndian<qint16>(frame.constData() + i * CallAudioFormat::bytesPerSample);
        sum += sample * sample;
    }
    return std::sqrt(sum / CallAudioFormat::samplesPerFrame) / 32768.0;
}

VoiceEffectChainConfig chainOf(const QList<AudioPluginId> &ids)
{
    VoiceEffectChainConfig config;
    config.enabled = true;
    for (const AudioPluginId &id : ids) {
        VoiceEffectStage stage;
        stage.id = id;
        stage.enabled = true;
        config.stages.append(stage);
    }
    return config;
}

} // namespace

class VoiceEffectsTest final : public QObject
{
    Q_OBJECT

private slots:

    // --- the sample bridge -------------------------------------------------

    // The claim MicrophoneProcessor makes about unity gain being bit-identical
    // has to survive a chain that does nothing, so the conversion out of the
    // frame and back into it must be exactly lossless. It is only lossless with
    // a symmetrical 32768 scale; the widespread 32767-on-the-way-back
    // convention loses one LSB on every sample, which this sweep would catch.
    void theSampleBridgeRoundTripsEveryInt16Exactly()
    {
        AudioFrame frame(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
        float floats[CallAudioFormat::samplesPerFrame];
        AudioFrame back(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);

        int worstError = 0;
        for (int base = -32768; base <= 32767; base += CallAudioFormat::samplesPerFrame) {
            for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
                const int value = std::min(base + i, 32767);
                qToLittleEndian<qint16>(static_cast<qint16>(value),
                                        frame.data() + i * CallAudioFormat::bytesPerSample);
            }
            SampleBridge::frameToFloat(frame, floats);
            SampleBridge::floatToFrame(floats, back.data());
            for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i) {
                const int before = qFromLittleEndian<qint16>(
                    frame.constData() + i * CallAudioFormat::bytesPerSample);
                const int after = qFromLittleEndian<qint16>(
                    back.constData() + i * CallAudioFormat::bytesPerSample);
                worstError = std::max(worstError, std::abs(before - after));
            }
        }
        QCOMPARE(worstError, 0);
    }

    // NaN must become silence and not the clamp's low bound. std::clamp is
    // comparison-based and every comparison against NaN is false, so a naive
    // clamp maps NaN to -32768: a full-scale click on every affected sample.
    void theSampleBridgeMapsNonFiniteSamplesToSilenceNotToFullScale()
    {
        float floats[CallAudioFormat::samplesPerFrame];
        for (int i = 0; i < CallAudioFormat::samplesPerFrame; ++i)
            floats[i] = std::numeric_limits<float>::quiet_NaN();
        floats[0] = std::numeric_limits<float>::infinity();
        floats[1] = -std::numeric_limits<float>::infinity();
        floats[2] = 2.0f;  // Legitimately over full scale: a saturator's peak.
        floats[3] = -2.0f;

        AudioFrame out(CallAudioFormat::bytesPerFrame, Qt::Uninitialized);
        SampleBridge::floatToFrame(floats, out.data());

        auto sampleAt = [&](int index) {
            return qFromLittleEndian<qint16>(out.constData()
                                             + index * CallAudioFormat::bytesPerSample);
        };
        QCOMPARE(sampleAt(0), qint16(32767));
        QCOMPARE(sampleAt(1), qint16(-32768));
        QCOMPARE(sampleAt(2), qint16(32767));
        QCOMPARE(sampleAt(3), qint16(-32768));
        for (int i = 4; i < CallAudioFormat::samplesPerFrame; ++i)
            QCOMPARE(sampleAt(i), qint16(0));

        QVERIFY(!SampleBridge::isFinite(floats));
    }

    // --- identifiers and persistence ---------------------------------------

    void pluginIdentifiersRoundTripThroughText()
    {
        const AudioPluginId id = fixtureId(QStringLiteral("org.openchat.test.gain"));
        const AudioPluginId parsed = AudioPluginId::fromString(id.toString());
        QCOMPARE(parsed.format, id.format);
        QCOMPARE(parsed.bundlePath, id.bundlePath);
        QCOMPARE(parsed.entryId, id.entryId);

        // A path containing the separator still round-trips, because only the
        // last '#' splits.
        AudioPluginId awkward;
        awkward.format = AudioPluginFormat::Vst3;
        awkward.bundlePath = QStringLiteral("/home/a#b/Plug.vst3");
        awkward.entryId = QStringLiteral("DEADBEEF");
        const AudioPluginId awkwardBack = AudioPluginId::fromString(awkward.toString());
        QCOMPARE(awkwardBack.bundlePath, awkward.bundlePath);
        QCOMPARE(awkwardBack.entryId, awkward.entryId);

        QVERIFY(!AudioPluginId::fromString(QStringLiteral("nonsense")).isValid());
        QVERIFY(!AudioPluginId::fromString(QStringLiteral("aiff:/x#y")).isValid());
    }

    void aStoredChainSurvivesBeingWrittenAndReadBack()
    {
        VoiceEffectChainConfig config = chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});
        config.stages[0].parameters.insert(0x9A11, 2.5);
        config.stages[0].state = QByteArray("\x01\x02\x03", 3);
        config.stages[0].stateVersion = QStringLiteral("1.0.0");

        const VoiceEffectChainConfig back = VoiceEffectChainConfig::fromJson(config.toJson());
        QCOMPARE(back.enabled, true);
        QCOMPARE(back.stages.size(), 1);
        QCOMPARE(back.stages[0].id.entryId, QStringLiteral("org.openchat.test.gain"));
        QCOMPARE(back.stages[0].parameters.value(0x9A11), 2.5);
        QCOMPARE(back.stages[0].state, QByteArray("\x01\x02\x03", 3));
    }

    // A corrupted state blob has to arrive as "no state" rather than as a
    // shorter blob, because a truncated blob is what gets handed to a plugin's
    // load() and that is a crash rather than a wrong setting.
    void aCorruptedStateBlobIsDiscardedRatherThanTruncated()
    {
        const QByteArray json =
            R"({"enabled":true,"stages":[{"id":"clap:/x.clap#y","enabled":true,)"
            R"("state":"!!!not base64!!!","stateVersion":"1"}]})";
        const VoiceEffectChainConfig config = VoiceEffectChainConfig::fromJson(json);
        QCOMPARE(config.stages.size(), 1);
        QVERIFY(config.stages[0].state.isEmpty());
        QVERIFY(config.stages[0].stateVersion.isEmpty());
    }

    // --- the trust layer ---------------------------------------------------

    void aPluginInADirectoryOthersCanWriteIsRefused()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString pluginPath = directory.filePath(QStringLiteral("plugin.clap"));
        QFile file(pluginPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not really a plugin");
        file.close();

        // The file itself is fine to begin with.
        const QString resolved = AudioPluginTrust::resolve(pluginPath);
        QVERIFY(!resolved.isEmpty());
        QVERIFY(!AudioPluginTrust::checkPluginPath(resolved));

        // Making the CONTAINING DIRECTORY world-writable is enough to refuse
        // it, even though the file's own permissions never changed. Replacing a
        // file is a property of its directory, and a host that only checked the
        // file would approve bytes anybody could swap.
        QVERIFY(QFile::setPermissions(directory.path(),
                                      QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                          | QFileDevice::ExeOwner | QFileDevice::ReadOther
                                          | QFileDevice::WriteOther | QFileDevice::ExeOther));
        const PluginError error = AudioPluginTrust::checkPluginPath(resolved);
        QCOMPARE(error.code, PluginErrorCode::UntrustedPath);
        QVERIFY(!error.message.isEmpty());
    }

    void aPluginWhoseBytesChangedNeedsFreshConsent()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString pluginPath = directory.filePath(QStringLiteral("plugin.clap"));
        {
            QFile file(pluginPath);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("version one");
        }

        const PluginConsent consent = AudioPluginTrust::recordConsent(pluginPath, 1234);
        QVERIFY(consent.isValid());
        QCOMPARE(consent.sha256.size(), 64);
        QVERIFY(!AudioPluginTrust::verifyAgainstConsent(pluginPath, {consent}));

        // The vendor's own auto-update is exactly this event, and it must
        // surface rather than being re-hashed away.
        {
            QFile file(pluginPath);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write("version two, subtly different");
        }
        const PluginError error = AudioPluginTrust::verifyAgainstConsent(pluginPath, {consent});
        QCOMPARE(error.code, PluginErrorCode::HashMismatch);
    }

    void aPluginNobodyApprovedIsNotLoaded()
    {
        const PluginError error =
            AudioPluginTrust::verifyAgainstConsent(fixturePath(), /*consents=*/{});
        QCOMPARE(error.code, PluginErrorCode::NotApproved);
    }

    // --- a real plugin, really loaded --------------------------------------

    // Everything below this point runs somebody's compiled shared object. If
    // the host were a stub these would fail rather than pass vacuously.

    void theFixtureBundlePublishesEveryPluginItDeclares()
    {
        PluginError error;
        const QList<AudioPluginDescriptor> descriptors =
            PluginScanner::describe(AudioPluginFormat::Clap, fixturePath(), error);
        QVERIFY2(!error, qPrintable(error.message));
        QCOMPARE(descriptors.size(), 4);

        const AudioPluginDescriptor &gain = descriptors.first();
        QCOMPARE(gain.id.entryId, QStringLiteral("org.openchat.test.gain"));
        QCOMPARE(gain.name, QStringLiteral("OpenChat Test Gain"));
        QCOMPARE(gain.vendor, QStringLiteral("OpenChat"));
        QVERIFY(gain.isNativeMono());
        QCOMPARE(gain.parameters.size(), 1);
        QCOMPARE(gain.parameters.first().name, QStringLiteral("Gain"));
        QCOMPARE(gain.parameters.first().maxValue, 4.0);
        // Hashed as it was read, so consent means these bytes.
        QCOMPARE(gain.sha256.size(), 64);
        QVERIFY(gain.fileSize > 0);

        // The stereo variant reports a non-zero latency, so a host that
        // hardcoded zero rather than reading the extension is caught.
        const auto stereo = std::find_if(
            descriptors.constBegin(), descriptors.constEnd(),
            [](const AudioPluginDescriptor &d) {
                return d.id.entryId == QStringLiteral("org.openchat.test.stereo");
            });
        QVERIFY(stereo != descriptors.constEnd());
        QCOMPARE(stereo->mainInputChannels, 2);
        QCOMPARE(stereo->mainOutputChannels, 2);
        QCOMPARE(stereo->latencySamples, 64);

        // The sidechain variant declares two input ports, and the host must
        // allocate both.
        const auto sidechain = std::find_if(
            descriptors.constBegin(), descriptors.constEnd(),
            [](const AudioPluginDescriptor &d) {
                return d.id.entryId == QStringLiteral("org.openchat.test.sidechain");
            });
        QVERIFY(sidechain != descriptors.constEnd());
        QCOMPARE(sidechain->inputPortCount, 2);
        QCOMPARE(sidechain->mainInputChannels, 1);
    }

    // The headline assertion: a plugin loaded and the samples changed, by
    // exactly the factor the plugin was told to apply.
    void aRealPluginChangesTheAudioByExactlyWhatItWasAsked()
    {
        VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});
        config.stages[0].parameters.insert(0x9A11, 0.5);

        VoiceEffectChain chain;
        QVERIFY2(chain.build(config, fixtureConsent()), qPrintable(chain.error()));
        QCOMPARE(chain.activeStageCount(), 1);
        QVERIFY(chain.isRunning());

        const AudioFrame in = toneFrame(0.4);
        AudioFrame out;
        chain.process(in, out);

        QCOMPARE(out.size(), CallAudioFormat::bytesPerFrame);
        QVERIFY(out != in); // It really did something.
        // Half the amplitude, within the rounding of one 16-bit step.
        QVERIFY2(std::abs(frameRms(out) - frameRms(in) * 0.5) < 0.001,
                 qPrintable(QStringLiteral("in=%1 out=%2")
                                .arg(frameRms(in))
                                .arg(frameRms(out))));
    }

    // Two stages in series, so the chain is a chain and not just a slot.
    void stagesApplyInOrderAndCompound()
    {
        VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain")),
                     fixtureId(QStringLiteral("org.openchat.test.gain"))});
        config.stages[0].parameters.insert(0x9A11, 0.5);
        config.stages[1].parameters.insert(0x9A11, 0.5);

        VoiceEffectChain chain;
        QVERIFY2(chain.build(config, fixtureConsent()), qPrintable(chain.error()));
        QCOMPARE(chain.activeStageCount(), 2);

        const AudioFrame in = toneFrame(0.8);
        AudioFrame out;
        chain.process(in, out);
        QVERIFY2(std::abs(frameRms(out) - frameRms(in) * 0.25) < 0.001,
                 qPrintable(QStringLiteral("in=%1 out=%2")
                                .arg(frameRms(in))
                                .arg(frameRms(out))));
    }

    // A stereo plugin fed a mono call. The fixture inverts its right channel,
    // so a host that averaged the two output channels would produce SILENCE.
    // Getting the original signal back is what proves channel 0 is taken.
    void aStereoPluginIsFedMonoAndReadBackFromChannelZero()
    {
        const VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.stereo"))});

        VoiceEffectChain chain;
        QVERIFY2(chain.build(config, fixtureConsent()), qPrintable(chain.error()));
        QCOMPARE(chain.latencySamples(), 64);

        const AudioFrame in = toneFrame(0.4);
        AudioFrame out;
        chain.process(in, out);

        QVERIFY2(frameRms(out) > 0.01,
                 "averaging the channels would have cancelled the signal to silence");
        QVERIFY(std::abs(frameRms(out) - frameRms(in)) < 0.001);
    }

    // The fixture dereferences its second declared input port. A host that
    // allocated only the main one reads past the end of its own array here, so
    // this test passing at all is the assertion.
    void aPluginWithASidechainPortGetsOneAllocated()
    {
        const VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.sidechain"))});

        VoiceEffectChain chain;
        QVERIFY2(chain.build(config, fixtureConsent()), qPrintable(chain.error()));

        const AudioFrame in = toneFrame(0.4);
        AudioFrame out;
        for (int frame = 0; frame < 8; ++frame)
            chain.process(in, out);

        // The sidechain is silent, so the sum is the main input unchanged.
        QVERIFY(std::abs(frameRms(out) - frameRms(in)) < 0.001);
        QVERIFY(chain.isRunning());
    }

    // NaN propagates: once a filter's state is poisoned every later sample is
    // too. Clamping it forever would hide a dead plugin behind plausible
    // silence while the far end heard nothing, so the chain switches off and
    // says why.
    void aPluginThatPoisonsTheSignalIsSwitchedOffAndTheCallGoesOnDry()
    {
        const VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.poison"))});

        VoiceEffectChain chain;
        QVERIFY2(chain.build(config, fixtureConsent()), qPrintable(chain.error()));
        QVERIFY(chain.isRunning());

        const AudioFrame in = toneFrame(0.4);
        AudioFrame out;
        chain.process(in, out);

        QVERIFY(!chain.isRunning());
        QCOMPARE(chain.missedFrames(), quint64(1));
        QVERIFY(!chain.error().isEmpty());
        // The frame the user was speaking is delivered dry rather than as NaN.
        QCOMPARE(out, in);

        // And it stays dry afterwards rather than trying again every frame.
        chain.process(in, out);
        QCOMPARE(out, in);
    }

    void aPluginThatWasNeverApprovedIsSkippedAndTheOthersStillLoad()
    {
        VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});

        VoiceEffectChain chain;
        // No consent at all: the chain must refuse to load anything.
        QVERIFY(!chain.build(config, /*consents=*/{}));
        QCOMPARE(chain.activeStageCount(), 0);
        QCOMPARE(chain.stages().size(), 1);
        QCOMPARE(chain.stages().first().error.code, PluginErrorCode::NotApproved);
        QVERIFY(!chain.error().isEmpty());

        // And with consent, the same config works -- so the refusal was the
        // trust check and not a broken config.
        QVERIFY(chain.build(config, fixtureConsent()));
        QCOMPARE(chain.activeStageCount(), 1);
    }

    void aMissingPluginLeavesTheRestOfTheChainRunning()
    {
        AudioPluginId missing = fixtureId(QStringLiteral("org.openchat.test.gain"));
        missing.bundlePath = QStringLiteral("/nonexistent/nowhere.clap");

        const VoiceEffectChainConfig config =
            chainOf({missing, fixtureId(QStringLiteral("org.openchat.test.gain"))});

        VoiceEffectChain chain;
        QVERIFY(chain.build(config, fixtureConsent()));
        QCOMPARE(chain.activeStageCount(), 1);
        QCOMPARE(chain.stages().size(), 2);
        QVERIFY(chain.stages().at(0).error.isError());
        QVERIFY(!chain.stages().at(1).error.isError());
    }

    void aPluginStateBlobRoundTripsThroughTheHost()
    {
        PluginError error;
        auto module = PluginScanner::openModule(AudioPluginFormat::Clap, fixturePath(), error);
        QVERIFY2(module != nullptr, qPrintable(error.message));

        auto instance =
            module->createInstance(QStringLiteral("org.openchat.test.gain"), error);
        QVERIFY2(instance != nullptr, qPrintable(error.message));

        instance->setParameter(0x9A11, 3.0);
        // The parameter is queued for the next frame, so it has to be processed
        // before the plugin's own state reflects it.
        float samples[CallAudioFormat::samplesPerFrame] = {};
        instance->processMono(samples);

        const QByteArray state = instance->saveState();
        QCOMPARE(state.size(), qsizetype(sizeof(double)));

        auto other = module->createInstance(QStringLiteral("org.openchat.test.gain"), error);
        QVERIFY(other != nullptr);
        QVERIFY(other->loadState(state));
        QCOMPARE(other->parameterValue(0x9A11), 3.0);
        QCOMPARE(instance->parameterText(0x9A11, 3.0), QStringLiteral("3.00 x"));
    }

    // --- the effect a call actually gets -----------------------------------

    void anEffectWithNothingConfiguredPassesFramesThroughUntouched()
    {
        LocalVoiceEffect effect(VoiceEffectChainConfig{}, {});
        QVERIFY(!effect.prepare()); // Nothing to do is not an error.
        QVERIFY(!effect.status().running);
        QVERIFY(effect.status().error.isEmpty());

        const AudioFrame in = toneFrame(0.4);
        QCOMPARE(effect.process(in), in);
    }

    void aConfiguredEffectReportsWhatItIsDoing()
    {
        VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});
        config.stages[0].parameters.insert(0x9A11, 2.0);

        LocalVoiceEffect effect(config, fixtureConsent());
        QVERIFY2(effect.prepare(), qPrintable(effect.status().error));

        const VoiceEffectStatus status = effect.status();
        QVERIFY(status.running);
        QCOMPARE(status.activeSlots, 1);
        QCOMPARE(status.missedFrames, quint64(0));
        QVERIFY(status.error.isEmpty());

        const AudioFrame in = toneFrame(0.2);
        const AudioFrame out = effect.process(in);
        QVERIFY2(std::abs(frameRms(out) - frameRms(in) * 2.0) < 0.002,
                 qPrintable(QStringLiteral("in=%1 out=%2")
                                .arg(frameRms(in))
                                .arg(frameRms(out))));
    }

    // A frame that is not a full frame is passed through rather than rejected,
    // for the same reason MicrophoneProcessor does it: nothing downstream would
    // accept it anyway, and a chain is not the place to discover that.
    void aFrameOfTheWrongLengthPassesStraightThrough()
    {
        const VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});
        LocalVoiceEffect effect(config, fixtureConsent());
        QVERIFY(effect.prepare());

        const AudioFrame stub(17, '\x7f');
        QCOMPARE(effect.process(stub), stub);
    }

    void theFactoryBuildsAnIndependentEffectPerCall()
    {
        const VoiceEffectChainConfig config =
            chainOf({fixtureId(QStringLiteral("org.openchat.test.gain"))});
        const VoiceEffectFactory factory =
            LocalVoiceEffect::factoryFor(config, fixtureConsent());
        QVERIFY(static_cast<bool>(factory));

        const std::unique_ptr<VoiceEffect> first = factory();
        const std::unique_ptr<VoiceEffect> second = factory();
        QVERIFY(first != nullptr);
        QVERIFY(second != nullptr);
        QVERIFY(first.get() != second.get());
    }

    // --- discovery ---------------------------------------------------------

    void discoveryFindsFilesWithoutOpeningThem()
    {
        const QFileInfo fixture(fixturePath());
        const QStringList found =
            PluginScanner::findBundles(AudioPluginFormat::Clap, {fixture.absolutePath()});
        QVERIFY(found.contains(fixture.absoluteFilePath()));

        AudioPluginFormat format = AudioPluginFormat::Vst3;
        QVERIFY(PluginScanner::formatOf(fixturePath(), format));
        QCOMPARE(format, AudioPluginFormat::Clap);
        QVERIFY(PluginScanner::formatOf(QStringLiteral("/x/Foo.vst3"), format));
        QCOMPARE(format, AudioPluginFormat::Vst3);
        QVERIFY(!PluginScanner::formatOf(QStringLiteral("/x/Foo.dll"), format));
    }

    void searchPathsNeverIncludeADirectoryThatIsNotThere()
    {
        for (const AudioPluginFormat format :
             {AudioPluginFormat::Clap, AudioPluginFormat::Vst3}) {
            for (const QString &path : PluginScanner::searchPaths(format))
                QVERIFY2(QFileInfo(path).isDir(), qPrintable(path));
        }
    }
};

QTEST_MAIN(VoiceEffectsTest)

#include "tst_voiceeffects.moc"
