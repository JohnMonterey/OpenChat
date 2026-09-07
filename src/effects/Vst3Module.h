#pragma once

#include "effects/AudioPluginInstance.h"
#include "effects/Vst3Support.h"

#include <pluginterfaces/base/ipluginbase.h>

#include <QString>

#include <memory>

namespace OpenChat {

// One opened VST3 bundle.
//
// A .vst3 on Linux is normally a DIRECTORY, not a file: the specification puts
// the real shared object at
//
//     Foo.vst3/Contents/<arch>-linux/Foo.so
//
// where <arch> is uname().machine, so the same bundle carries builds for
// several architectures. It is computed at run time rather than baked in,
// because a host built for one architecture running on another is exactly the
// case the layout exists to serve. A plain .vst3 FILE is also accepted, because
// some plugins ship one and refusing them would be pedantry.
//
// Three exported symbols matter, and only one of them is the one people expect:
//
//     ModuleEntry(void*)  -- Linux-only, called before anything else
//     GetPluginFactory()  -- the factory
//     ModuleExit()        -- Linux-only, called last
//
// Skipping ModuleEntry is the characteristic Linux VST3 host bug: the factory
// pointer comes back fine and the plugin then behaves as though it were never
// initialised, because on Linux that is where its global setup runs.
//
// As with CLAP, opening is executing: dlopen runs the object's constructors
// before it returns. Every caller must have satisfied AudioPluginTrust first.
class Vst3Module final : public AudioPluginModule,
                         public std::enable_shared_from_this<Vst3Module>
{
public:
    ~Vst3Module() override;

    Vst3Module(const Vst3Module &) = delete;
    Vst3Module &operator=(const Vst3Module &) = delete;

    // path is the bundle directory or the bare .so, already resolved and
    // trusted. Null on failure.
    [[nodiscard]] static std::shared_ptr<Vst3Module> open(const QString &path,
                                                          PluginError &error);

    // Given a bundle path, the shared object inside it that should be dlopen'd.
    // Empty when the layout does not contain one for this architecture. Exposed
    // because the trust layer must hash the FILE that will actually be loaded,
    // not the directory that contains it.
    [[nodiscard]] static QString resolveBinaryPath(const QString &bundlePath);

    [[nodiscard]] QList<AudioPluginDescriptor> descriptors() const override;

    [[nodiscard]] std::unique_ptr<AudioPluginInstance>
    createInstance(const QString &entryId, PluginError &error) override;

    [[nodiscard]] Steinberg::IPluginFactory *factory() const noexcept { return m_factory; }
    [[nodiscard]] Vst3HostApplication *hostApplication() noexcept { return &m_hostApplication; }

    struct PrivateTag final {};
    explicit Vst3Module(PrivateTag) {}

private:
    void buildDescriptors();

    void *m_handle = nullptr;
    bool (*m_moduleExit)() = nullptr;
    Steinberg::IPluginFactory *m_factory = nullptr;
    QString m_bundlePath;
    QString m_binaryPath;
    QList<AudioPluginDescriptor> m_descriptors;
    Vst3HostApplication m_hostApplication;
};

} // namespace OpenChat
