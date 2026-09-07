#pragma once

#include "effects/AudioPluginInstance.h"
#include "effects/ClapHostContext.h"

#include <QString>

#include <memory>

struct clap_plugin_entry;
struct clap_plugin_factory;

namespace OpenChat {

// One dlopen'd .clap file.
//
// A .clap is an ordinary ELF shared object with a different extension; the
// whole ABI is one exported symbol, `clap_entry`, which is a DATA symbol and
// not a function -- dlsym returns a pointer to the struct itself, and treating
// it as a function pointer is the single most common way a first CLAP host
// crashes.
//
// Opening one is not an inspection, it is an execution: dlopen runs the
// object's DT_INIT_ARRAY constructors before it returns, before dlsym, before
// any validation a host could possibly write. There is therefore no way to
// "check whether this file is a valid plugin" that is not already arbitrary
// code execution, and every caller of open() must have satisfied
// AudioPluginTrust first. Nothing in this class re-checks that, because by the
// time this class runs it is far too late.
// Always held by shared_ptr: an instance keeps its module alive, because
// unloading the code a running plugin lives in is not survivable.
class ClapModule final : public AudioPluginModule,
                         public std::enable_shared_from_this<ClapModule>
{
public:
    ~ClapModule() override;

    ClapModule(const ClapModule &) = delete;
    ClapModule &operator=(const ClapModule &) = delete;

    // path must already be resolved and trusted. Null on failure.
    [[nodiscard]] static std::shared_ptr<ClapModule> open(const QString &path,
                                                          PluginError &error);

    [[nodiscard]] QList<AudioPluginDescriptor> descriptors() const override;

    [[nodiscard]] std::unique_ptr<AudioPluginInstance>
    createInstance(const QString &entryId, PluginError &error) override;

public:
    // Public only so make_shared can reach it; open() is the way in.
    struct PrivateTag final {};
    explicit ClapModule(PrivateTag) {}

private:

    // Instantiates every plugin the factory publishes just far enough to read
    // its ports, latency and parameters, then destroys it again. This is the
    // expensive half of a scan and it happens once, at open, because the
    // alternative is doing it lazily inside whatever asks a question first.
    void buildDescriptors();

    void *m_handle = nullptr;
    const clap_plugin_entry *m_entry = nullptr;
    const clap_plugin_factory *m_factory = nullptr;
    QString m_path;
    QList<AudioPluginDescriptor> m_descriptors;
    // One host per module, shared by the instances it creates. A plugin holds
    // the pointer for its whole life, so this must outlive every instance --
    // which it does, because an instance keeps its module alive.
    std::unique_ptr<ClapHostContext> m_host;
};

} // namespace OpenChat
