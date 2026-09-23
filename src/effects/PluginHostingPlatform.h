#pragma once

#include <QtGlobal>

// Where third-party voice effect plugins (CLAP, VST3) can be hosted.
//
// A hosted plugin is somebody else's code running inside the process that holds
// the message-history key. What makes that acceptable is AudioPluginTrust: it
// refuses any file that another account could replace, by checking the owner
// and mode of the file and of every directory above it. Those checks are
// written against POSIX ownership and permissions. Windows expresses the same
// question through ACLs, and nothing here answers it yet -- so rather than load
// plugins behind a check that does not exist, hosting is unavailable on Windows:
// AudioPluginTrust::checkPluginPath() refuses every path, which is the gate
// every load goes through, and no library is ever opened.
//
// The built-in voice effects (media/VoiceEffects) are not plugins and are not
// affected.
#ifdef Q_OS_WIN
#define OPENCHAT_PLUGIN_HOSTING 0
#else
#define OPENCHAT_PLUGIN_HOSTING 1
#endif

#if OPENCHAT_PLUGIN_HOSTING

#include <dlfcn.h>

#else

// Stand-ins so the loaders compile unchanged. They are unreachable in practice
// (the trust gate refuses first) and fail closed if they are ever reached:
// nothing is opened and no symbol is ever found.
inline constexpr int RTLD_NOW = 0;
inline constexpr int RTLD_LOCAL = 0;

inline void *dlopen(const char *, int)
{
    return nullptr;
}

inline void *dlsym(void *, const char *)
{
    return nullptr;
}

inline int dlclose(void *)
{
    return 0;
}

inline const char *dlerror()
{
    return "voice effect plugins cannot be hosted on this platform";
}

#endif
