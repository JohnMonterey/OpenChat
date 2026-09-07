#include "effects/AudioPluginTypes.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace OpenChat {

namespace {


double finiteOr(double value, double fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

} // namespace

QString audioPluginFormatName(AudioPluginFormat format)
{
    return format == AudioPluginFormat::Vst3 ? QStringLiteral("vst3") : QStringLiteral("clap");
}

QString audioPluginFormatExtension(AudioPluginFormat format)
{
    return format == AudioPluginFormat::Vst3 ? QStringLiteral(".vst3") : QStringLiteral(".clap");
}

QString AudioPluginId::toString() const
{
    if (!isValid())
        return {};
    return audioPluginFormatName(format) + QLatin1Char(':') + bundlePath + QLatin1Char('#')
        + entryId;
}

AudioPluginId AudioPluginId::fromString(const QString &text)
{
    const qsizetype colon = text.indexOf(QLatin1Char(':'));
    // The LAST '#', so a bundle path that contains one still splits correctly.
    // Entry ids are reverse-DNS or hex and never contain '#'; paths can.
    const qsizetype hash = text.lastIndexOf(QLatin1Char('#'));
    if (colon <= 0 || hash <= colon + 1)
        return {};

    AudioPluginId id;
    const QString formatText = text.left(colon);
    if (formatText == QLatin1String("vst3"))
        id.format = AudioPluginFormat::Vst3;
    else if (formatText == QLatin1String("clap"))
        id.format = AudioPluginFormat::Clap;
    else
        return {};

    id.bundlePath = text.mid(colon + 1, hash - colon - 1);
    id.entryId = text.mid(hash + 1);
    return id.isValid() ? id : AudioPluginId{};
}

double AudioPluginParameter::clampToRange(double value) const noexcept
{
    // A plugin is free to publish a descending range, and at least one does.
    // Ordering the bounds here means the clamp holds either way instead of
    // collapsing every value onto one end.
    const double low = std::min(minValue, maxValue);
    const double high = std::max(minValue, maxValue);
    if (!std::isfinite(value))
        return std::clamp(finiteOr(defaultValue, low), low, high);
    return std::clamp(value, low, high);
}

QJsonObject AudioPluginParameter::toJson() const
{
    return QJsonObject{
        {QStringLiteral("id"), static_cast<double>(id)},
        {QStringLiteral("name"), name},
        {QStringLiteral("module"), module},
        {QStringLiteral("min"), minValue},
        {QStringLiteral("max"), maxValue},
        {QStringLiteral("default"), defaultValue},
        {QStringLiteral("stepped"), stepped},
        {QStringLiteral("readOnly"), readOnly},
        {QStringLiteral("hidden"), hidden},
    };
}

AudioPluginParameter AudioPluginParameter::fromJson(const QJsonObject &object)
{
    AudioPluginParameter parameter;
    parameter.id = static_cast<quint32>(object.value(QStringLiteral("id")).toDouble());
    parameter.name = object.value(QStringLiteral("name")).toString();
    parameter.module = object.value(QStringLiteral("module")).toString();
    parameter.minValue = finiteOr(object.value(QStringLiteral("min")).toDouble(0.0), 0.0);
    parameter.maxValue = finiteOr(object.value(QStringLiteral("max")).toDouble(1.0), 1.0);
    parameter.defaultValue =
        finiteOr(object.value(QStringLiteral("default")).toDouble(0.0), 0.0);
    parameter.stepped = object.value(QStringLiteral("stepped")).toBool();
    parameter.readOnly = object.value(QStringLiteral("readOnly")).toBool();
    parameter.hidden = object.value(QStringLiteral("hidden")).toBool();
    return parameter;
}

QJsonObject AudioPluginDescriptor::toJson() const
{
    QJsonArray parameterArray;
    for (const AudioPluginParameter &parameter : parameters)
        parameterArray.append(parameter.toJson());

    return QJsonObject{
        {QStringLiteral("id"), id.toString()},
        {QStringLiteral("name"), name},
        {QStringLiteral("vendor"), vendor},
        {QStringLiteral("version"), version},
        {QStringLiteral("description"), description},
        {QStringLiteral("features"), QJsonArray::fromStringList(features)},
        {QStringLiteral("inputPortCount"), inputPortCount},
        {QStringLiteral("outputPortCount"), outputPortCount},
        {QStringLiteral("mainInputChannels"), mainInputChannels},
        {QStringLiteral("mainOutputChannels"), mainOutputChannels},
        {QStringLiteral("latencySamples"), latencySamples},
        {QStringLiteral("parameters"), parameterArray},
        {QStringLiteral("sha256"), sha256},
        {QStringLiteral("fileSize"), static_cast<double>(fileSize)},
    };
}

AudioPluginDescriptor AudioPluginDescriptor::fromJson(const QJsonObject &object)
{
    AudioPluginDescriptor descriptor;
    descriptor.id = AudioPluginId::fromString(object.value(QStringLiteral("id")).toString());
    descriptor.name = object.value(QStringLiteral("name")).toString();
    descriptor.vendor = object.value(QStringLiteral("vendor")).toString();
    descriptor.version = object.value(QStringLiteral("version")).toString();
    descriptor.description = object.value(QStringLiteral("description")).toString();
    const QJsonArray featureArray = object.value(QStringLiteral("features")).toArray();
    for (const QJsonValue &value : featureArray)
        descriptor.features.append(value.toString());
    descriptor.inputPortCount = object.value(QStringLiteral("inputPortCount")).toInt();
    descriptor.outputPortCount = object.value(QStringLiteral("outputPortCount")).toInt();
    descriptor.mainInputChannels = object.value(QStringLiteral("mainInputChannels")).toInt();
    descriptor.mainOutputChannels = object.value(QStringLiteral("mainOutputChannels")).toInt();
    descriptor.latencySamples = object.value(QStringLiteral("latencySamples")).toInt();
    const QJsonArray parameterArray = object.value(QStringLiteral("parameters")).toArray();
    for (const QJsonValue &value : parameterArray)
        descriptor.parameters.append(AudioPluginParameter::fromJson(value.toObject()));
    descriptor.sha256 = object.value(QStringLiteral("sha256")).toString();
    descriptor.fileSize = static_cast<qint64>(object.value(QStringLiteral("fileSize")).toDouble());
    return descriptor;
}

QJsonObject VoiceEffectStage::toJson() const
{
    QJsonObject parameterObject;
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        // JSON object keys are strings, so the parameter id becomes one. Decimal
        // rather than hex because that is what every other numeric key in the
        // stored payloads uses.
        parameterObject.insert(QString::number(it.key()), it.value());
    }

    QJsonObject object{
        {QStringLiteral("id"), id.toString()},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("mix"), mix},
        {QStringLiteral("parameters"), parameterObject},
    };
    if (!state.isEmpty()) {
        object.insert(QStringLiteral("state"), QString::fromLatin1(state.toBase64()));
        object.insert(QStringLiteral("stateVersion"), stateVersion);
    }
    return object;
}

VoiceEffectStage VoiceEffectStage::fromJson(const QJsonObject &object)
{
    VoiceEffectStage stage;
    stage.id = AudioPluginId::fromString(object.value(QStringLiteral("id")).toString());
    stage.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    // Clamped rather than rejected: a mix outside the range is a stored
    // document somebody hand-edited, and the nearest legal value is what they
    // were reaching for.
    const double storedMix = object.value(QStringLiteral("mix")).toDouble(1.0);
    stage.mix = std::isfinite(storedMix) ? std::clamp(storedMix, 0.0, 1.0) : 1.0;
    stage.stateVersion = object.value(QStringLiteral("stateVersion")).toString();

    const QByteArray encoded = object.value(QStringLiteral("state")).toString().toLatin1();
    if (!encoded.isEmpty()) {
        // Strict decoding: a corrupted blob must arrive as "no state" rather
        // than as a shorter blob the plugin would then be handed.
        const QByteArray::FromBase64Result decoded =
            QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (decoded)
            stage.state = *decoded;
        else
            stage.stateVersion.clear();
    }

    const QJsonObject parameterObject = object.value(QStringLiteral("parameters")).toObject();
    for (auto it = parameterObject.constBegin(); it != parameterObject.constEnd(); ++it) {
        bool ok = false;
        const quint32 parameterId = it.key().toUInt(&ok);
        if (!ok)
            continue;
        const double value = it.value().toDouble();
        if (std::isfinite(value))
            stage.parameters.insert(parameterId, value);
    }
    return stage;
}

bool VoiceEffectChainConfig::hasWork() const noexcept
{
    if (!enabled)
        return false;
    return std::any_of(stages.constBegin(), stages.constEnd(),
                       [](const VoiceEffectStage &stage) {
                           return stage.enabled && stage.id.isValid();
                       });
}

QByteArray VoiceEffectChainConfig::toJson() const
{
    QJsonArray stageArray;
    for (const VoiceEffectStage &stage : stages)
        stageArray.append(stage.toJson());
    const QJsonObject object{
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("stages"), stageArray},
    };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

VoiceEffectChainConfig VoiceEffectChainConfig::fromJson(const QByteArray &json)
{
    VoiceEffectChainConfig config;
    if (json.isEmpty())
        return config;

    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (!document.isObject())
        return config;

    const QJsonObject object = document.object();
    config.enabled = object.value(QStringLiteral("enabled")).toBool();
    const QJsonArray stageArray = object.value(QStringLiteral("stages")).toArray();
    for (const QJsonValue &value : stageArray) {
        VoiceEffectStage stage = VoiceEffectStage::fromJson(value.toObject());
        // A stage whose id will not parse names no plugin, so it can never be
        // loaded and would only ever occupy a row. A stage whose FILE is merely
        // missing is kept, the way MicrophoneSettings keeps a stored device id:
        // an unplugged thing coming back should just work again.
        if (!stage.id.isValid())
            continue;
        if (config.stages.size() >= maxStages)
            break;
        config.stages.append(stage);
    }
    return config;
}

PluginError makePluginError(PluginErrorCode code, const QString &message)
{
    return PluginError{code, message};
}

} // namespace OpenChat
