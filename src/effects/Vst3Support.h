#pragma once

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <QByteArray>

#include <array>
#include <atomic>
#include <vector>

namespace OpenChat {

// The small pile of objects a VST3 plugin expects its host to be.
//
// VST3 is a COM-like ABI, which means the host does not merely call the plugin:
// it hands the plugin objects to call BACK, and a plugin is entitled to assume
// they exist. Every class here is one of those, implemented to the minimum a
// plugin needs to run headlessly in a voice call and no further. They are
// deliberately in one header because they are all the same kind of thing --
// scaffolding that exists so that somebody else's code does not dereference
// null.
//
// Reference counting is real but trivial: every one of these outlives the
// plugin that holds it, because the effect owns them all and destroys the
// plugin first. The counts exist because the ABI requires the methods, not
// because anything here is ever actually shared.

// Reference counting for objects that are never actually deleted by it.
//
// These objects are members of the effect, not heap allocations, so a plugin
// over-releasing one must not free it. The count is kept honestly -- it costs
// nothing and a debugger can read it -- but reaching zero simply does nothing.
//
// Deliberately NOT derived from FUnknown. Every interface below already
// inherits FUnknown, so a common base that did too would give each class two
// FUnknown subobjects and make every conversion to it ambiguous. A mixin that
// only counts avoids the diamond entirely.
class Vst3RefCounted
{
public:
    Steinberg::uint32 addRefImpl() noexcept
    {
        return static_cast<Steinberg::uint32>(++m_refCount);
    }
    Steinberg::uint32 releaseImpl() noexcept
    {
        const int count = --m_refCount;
        return static_cast<Steinberg::uint32>(count < 0 ? 0 : count);
    }

protected:
    // Answers a query for FUnknown itself. Takes the most-derived object as an
    // FUnknown* because this class is not one.
    Steinberg::tresult queryFUnknown(Steinberg::FUnknown *self, const Steinberg::TUID iid,
                                     void **obj);

private:
    std::atomic<int> m_refCount{1};
};

// What a plugin is given as its "context" at initialize().
//
// initialize(nullptr) is legal by the letter of the specification and a crash
// in practice: plugins dereference the context to ask for the host's name or to
// create a message object, and enough of them do it without checking that
// passing null is simply not an option. This provides the two interfaces that
// are actually asked for.
class Vst3HostApplication final : public Steinberg::Vst::IHostApplication, private Vst3RefCounted
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return addRefImpl(); }
    Steinberg::uint32 PLUGIN_API release() override { return releaseImpl(); }

    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID iid,
                                                 void **obj) override;
};

// The object an edit controller reports parameter changes through.
//
// A headless host has no automation to record and no UI to redraw, so every
// method here is a no-op -- but the controller calls them during setup and
// during state loading, and a null handler is a null dereference. restartComponent
// is recorded rather than acted on: it means "my latency or parameters moved",
// which cannot be honoured inside a frame.
class Vst3ComponentHandler final : public Steinberg::Vst::IComponentHandler, private Vst3RefCounted
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return addRefImpl(); }
    Steinberg::uint32 PLUGIN_API release() override { return releaseImpl(); }

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override;
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID,
                                              Steinberg::Vst::ParamValue) override;
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override;
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

    [[nodiscard]] bool takeRestartRequested() noexcept
    {
        const bool requested = m_restartRequested;
        m_restartRequested = false;
        return requested;
    }

private:
    bool m_restartRequested = false;
};

// A QByteArray behind the IBStream a plugin reads and writes its state through.
//
// VST3 has no "give me a blob" call; state is always streamed, so a host that
// wants to save a preset has to supply the stream. This is that stream, in
// memory, seekable, and bounded by the size of the array.
class Vst3MemoryStream final : public Steinberg::IBStream, private Vst3RefCounted
{
public:
    Vst3MemoryStream() = default;
    explicit Vst3MemoryStream(QByteArray data)
        : m_data(std::move(data))
    {
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return addRefImpl(); }
    Steinberg::uint32 PLUGIN_API release() override { return releaseImpl(); }

    Steinberg::tresult PLUGIN_API read(void *buffer, Steinberg::int32 numBytes,
                                       Steinberg::int32 *numBytesRead) override;
    Steinberg::tresult PLUGIN_API write(void *buffer, Steinberg::int32 numBytes,
                                        Steinberg::int32 *numBytesWritten) override;
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                       Steinberg::int64 *result) override;
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64 *pos) override;

    [[nodiscard]] const QByteArray &data() const noexcept { return m_data; }
    void rewind() noexcept { m_position = 0; }

private:
    QByteArray m_data;
    qsizetype m_position = 0;
};

// One parameter's worth of changes inside a block, and the list of them.
//
// Both are fixed-capacity and allocate nothing during a frame, for the same
// reason the CLAP event queue does not. outputParameterChanges is separately
// important: it is documented as optional and is not, because plugins written
// against certain frameworks assert on it, so an empty writable list is always
// supplied.
class Vst3ParamQueue final : public Steinberg::Vst::IParamValueQueue, private Vst3RefCounted
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return addRefImpl(); }
    Steinberg::uint32 PLUGIN_API release() override { return releaseImpl(); }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return m_parameterId; }
    Steinberg::int32 PLUGIN_API getPointCount() override
    {
        return static_cast<Steinberg::int32>(m_points.size());
    }
    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32 &sampleOffset,
                                           Steinberg::Vst::ParamValue &value) override;
    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset,
                                           Steinberg::Vst::ParamValue value,
                                           Steinberg::int32 &index) override;

    void configure(Steinberg::Vst::ParamID parameterId, double normalisedValue);
    void clear() noexcept { m_points.clear(); }

private:
    struct Point final {
        Steinberg::int32 offset = 0;
        Steinberg::Vst::ParamValue value = 0.0;
    };

    Steinberg::Vst::ParamID m_parameterId = 0;
    std::vector<Point> m_points;
};

class Vst3ParamChanges final : public Steinberg::Vst::IParameterChanges, private Vst3RefCounted
{
public:

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return addRefImpl(); }
    Steinberg::uint32 PLUGIN_API release() override { return releaseImpl(); }

    Steinberg::int32 PLUGIN_API getParameterCount() override { return m_used; }
    Steinberg::Vst::IParamValueQueue *PLUGIN_API getParameterData(Steinberg::int32 index) override;
    Steinberg::Vst::IParamValueQueue *PLUGIN_API
    addParameterData(const Steinberg::Vst::ParamID &parameterId, Steinberg::int32 &index) override;

    // Host side: queue one plain-normalised value for the head of the next
    // block. Dropped when full rather than allocating mid-frame.
    void queue(Steinberg::Vst::ParamID parameterId, double normalisedValue);
    void clear() noexcept { m_used = 0; }

private:
    static constexpr int capacity = 64;

    // A fixed array, not a vector: these objects carry an atomic refcount and
    // are therefore neither copyable nor movable, which is exactly what a
    // vector's growth needs them to be. Fixing the capacity is also what keeps
    // a parameter change from allocating.
    std::array<Vst3ParamQueue, capacity> m_queues;
    Steinberg::int32 m_used = 0;
};

} // namespace OpenChat
