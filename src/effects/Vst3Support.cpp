#include "effects/Vst3Support.h"

#include <pluginterfaces/base/funknownimpl.h>

#include <algorithm>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace OpenChat {

namespace {

// Compares a queried IID against an interface's own. FUID::operator== takes a
// TUID, and the iid members are FUIDs, so this is the direction that compiles
// and the one that reads correctly.
bool matches(const TUID queried, const FUID &known)
{
    return known == FUID::fromTUID(queried);
}

} // namespace

tresult Vst3RefCounted::queryFUnknown(FUnknown *self, const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, FUnknown::iid)) {
        *obj = self;
        addRefImpl();
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

// --- Vst3HostApplication --------------------------------------------------

tresult PLUGIN_API Vst3HostApplication::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, IHostApplication::iid)) {
        *obj = static_cast<IHostApplication *>(this);
        addRef();
        return kResultOk;
    }
    return queryFUnknown(this, iid, obj);
}

tresult PLUGIN_API Vst3HostApplication::getName(String128 name)
{
    // String128 is UTF-16. Plugins occasionally match on the host name to work
    // around DAW-specific bugs; there is nothing to gain from lying about it.
    static const char16_t hostName[] = u"OpenChat";
    std::memcpy(name, hostName, sizeof hostName);
    return kResultOk;
}

tresult PLUGIN_API Vst3HostApplication::createInstance(TUID, TUID, void **obj)
{
    // A plugin asks for this to get a message or attribute-list object so it can
    // talk to its own edit controller. This host connects component and
    // controller directly instead, so there is nothing to hand back. Returning
    // kNotImplemented rather than crashing is the whole contract here.
    if (obj)
        *obj = nullptr;
    return kNotImplemented;
}

// --- Vst3ComponentHandler -------------------------------------------------

tresult PLUGIN_API Vst3ComponentHandler::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, IComponentHandler::iid)) {
        *obj = static_cast<IComponentHandler *>(this);
        addRef();
        return kResultOk;
    }
    return queryFUnknown(this, iid, obj);
}

tresult PLUGIN_API Vst3ComponentHandler::beginEdit(ParamID)
{
    return kResultOk;
}

tresult PLUGIN_API Vst3ComponentHandler::performEdit(ParamID, ParamValue)
{
    // This is the controller telling the host a knob moved in a GUI. There is no
    // GUI, so the only source of parameter changes is the host itself, and
    // echoing them back would be a loop.
    return kResultOk;
}

tresult PLUGIN_API Vst3ComponentHandler::endEdit(ParamID)
{
    return kResultOk;
}

tresult PLUGIN_API Vst3ComponentHandler::restartComponent(int32 flags)
{
    // kLatencyChanged and kParamValuesChanged both arrive here. Acting on either
    // means deactivating and reactivating the component, which cannot happen
    // inside a frame, so it is recorded and honoured between them.
    if (flags != 0)
        m_restartRequested = true;
    return kResultOk;
}

// --- Vst3MemoryStream -----------------------------------------------------

tresult PLUGIN_API Vst3MemoryStream::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, IBStream::iid)) {
        *obj = static_cast<IBStream *>(this);
        addRef();
        return kResultOk;
    }
    return queryFUnknown(this, iid, obj);
}

tresult PLUGIN_API Vst3MemoryStream::read(void *buffer, int32 numBytes, int32 *numBytesRead)
{
    if (!buffer || numBytes < 0)
        return kInvalidArgument;
    const qsizetype available = std::max<qsizetype>(0, m_data.size() - m_position);
    const qsizetype take = std::min<qsizetype>(available, numBytes);
    if (take > 0) {
        std::memcpy(buffer, m_data.constData() + m_position, static_cast<size_t>(take));
        m_position += take;
    }
    if (numBytesRead)
        *numBytesRead = static_cast<int32>(take);
    // Reading zero bytes at the end is end-of-stream, not failure.
    return kResultOk;
}

tresult PLUGIN_API Vst3MemoryStream::write(void *buffer, int32 numBytes, int32 *numBytesWritten)
{
    if (!buffer || numBytes < 0)
        return kInvalidArgument;
    if (m_position > m_data.size())
        m_data.resize(m_position, '\0');
    m_data.replace(m_position, numBytes, static_cast<const char *>(buffer), numBytes);
    m_position += numBytes;
    if (numBytesWritten)
        *numBytesWritten = numBytes;
    return kResultOk;
}

tresult PLUGIN_API Vst3MemoryStream::seek(int64 pos, int32 mode, int64 *result)
{
    qsizetype target = 0;
    switch (mode) {
    case kIBSeekSet:
        target = static_cast<qsizetype>(pos);
        break;
    case kIBSeekCur:
        target = m_position + static_cast<qsizetype>(pos);
        break;
    case kIBSeekEnd:
        target = m_data.size() + static_cast<qsizetype>(pos);
        break;
    default:
        return kInvalidArgument;
    }
    if (target < 0)
        return kInvalidArgument;
    m_position = target;
    if (result)
        *result = static_cast<int64>(m_position);
    return kResultOk;
}

tresult PLUGIN_API Vst3MemoryStream::tell(int64 *pos)
{
    if (!pos)
        return kInvalidArgument;
    *pos = static_cast<int64>(m_position);
    return kResultOk;
}

// --- Vst3ParamQueue -------------------------------------------------------

tresult PLUGIN_API Vst3ParamQueue::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, IParamValueQueue::iid)) {
        *obj = static_cast<IParamValueQueue *>(this);
        addRef();
        return kResultOk;
    }
    return queryFUnknown(this, iid, obj);
}

tresult PLUGIN_API Vst3ParamQueue::getPoint(int32 index, int32 &sampleOffset, ParamValue &value)
{
    if (index < 0 || index >= static_cast<int32>(m_points.size()))
        return kInvalidArgument;
    sampleOffset = m_points[static_cast<size_t>(index)].offset;
    value = m_points[static_cast<size_t>(index)].value;
    return kResultOk;
}

tresult PLUGIN_API Vst3ParamQueue::addPoint(int32 sampleOffset, ParamValue value, int32 &index)
{
    // Only ever reached when a plugin writes to the OUTPUT changes list. There
    // is nowhere for those to go, so they are accepted and dropped -- refusing
    // would make some plugins treat their own automation output as broken.
    (void)sampleOffset;
    (void)value;
    index = 0;
    return kResultOk;
}

void Vst3ParamQueue::configure(ParamID parameterId, double normalisedValue)
{
    m_parameterId = parameterId;
    m_points.clear();
    // Offset 0: at the head of the block, the same position the CLAP backend
    // uses, so a parameter change lands identically in both.
    m_points.push_back(Point{0, normalisedValue});
}

// --- Vst3ParamChanges -----------------------------------------------------

tresult PLUGIN_API Vst3ParamChanges::queryInterface(const TUID iid, void **obj)
{
    if (!obj)
        return kInvalidArgument;
    if (matches(iid, IParameterChanges::iid)) {
        *obj = static_cast<IParameterChanges *>(this);
        addRef();
        return kResultOk;
    }
    return queryFUnknown(this, iid, obj);
}

IParamValueQueue *PLUGIN_API Vst3ParamChanges::getParameterData(int32 index)
{
    if (index < 0 || index >= m_used)
        return nullptr;
    return &m_queues[static_cast<size_t>(index)];
}

IParamValueQueue *PLUGIN_API Vst3ParamChanges::addParameterData(const ParamID &parameterId,
                                                                int32 &index)
{
    // The plugin's side of the OUTPUT list. Hand back a real queue so a plugin
    // that writes automation out has somewhere legal to write it.
    for (int32 i = 0; i < m_used; ++i) {
        if (m_queues[static_cast<size_t>(i)].getParameterId() == parameterId) {
            index = i;
            return &m_queues[static_cast<size_t>(i)];
        }
    }
    if (m_used >= capacity)
        return nullptr;
    const int32 slot = m_used++;
    m_queues[static_cast<size_t>(slot)].configure(parameterId, 0.0);
    m_queues[static_cast<size_t>(slot)].clear();
    index = slot;
    return &m_queues[static_cast<size_t>(slot)];
}

void Vst3ParamChanges::queue(ParamID parameterId, double normalisedValue)
{
    for (int32 i = 0; i < m_used; ++i) {
        if (m_queues[static_cast<size_t>(i)].getParameterId() == parameterId) {
            m_queues[static_cast<size_t>(i)].configure(parameterId, normalisedValue);
            return;
        }
    }
    if (m_used >= capacity)
        return; // Sixty-four knob movements in one 20 ms block is not a human.
    m_queues[static_cast<size_t>(m_used++)].configure(parameterId, normalisedValue);
}

} // namespace OpenChat
