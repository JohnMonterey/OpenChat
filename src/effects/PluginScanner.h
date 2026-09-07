#pragma once

#include "effects/AudioPluginInstance.h"
#include "effects/AudioPluginTypes.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <memory>

namespace OpenChat {

// Finding plugin files, and opening one once the user has chosen it.
//
// LISTING IS NOT LOADING, and the separation is the entire security design of
// this subsystem. findBundles() reads directory entries and returns paths; it
// executes nothing. Only describe() and openModule() dlopen anything, and both
// require the caller to have satisfied AudioPluginTrust first.
//
// There is deliberately no scanAndLoadEverything(), and there will not be one.
// Auto-loading whatever is in the search paths means anything that can drop a
// file into ~/.clap gets it executed inside the process holding the message
// history key on next launch, with nobody having decided anything. That is the
// whole mass-compromise vector, and the only defence against it is that a
// plugin reaches a call solely because a user named it.
namespace PluginScanner {

// Where plugins conventionally live, most specific first. The user's own
// directory precedes the system ones so a personally-installed copy wins.
//
// CLAP's specification names $HOME/.clap, /usr/lib/clap and $CLAP_PATH.
// /usr/local/lib/clap is convention rather than specification and is scanned
// anyway, because that is where a locally-built plugin actually lands.
//
// VST3 has no VST3_PATH: it is not in the specification and the SDK never reads
// one, so a host that invented support for it would be honouring a variable no
// plugin installer sets.
[[nodiscard]] QStringList searchPaths(AudioPluginFormat format);

// Every plugin file under the search paths, or under `roots` when given.
// Recursive, because both formats permit vendor subdirectories.
//
// Returns paths and nothing else. A path here has not been opened, validated,
// or established to be a plugin at all -- it is a filename with the right
// extension.
[[nodiscard]] QStringList findBundles(AudioPluginFormat format,
                                      const QStringList &roots = {});

// The file that will actually be dlopen'd for a bundle path. For CLAP that is
// the bundle itself; for VST3 it is the shared object inside the bundle
// directory. Consent must be recorded against THIS path, because it is the one
// whose bytes get executed.
[[nodiscard]] QString loadablePathFor(AudioPluginFormat format, const QString &bundlePath);

// Guesses the format from the file extension. A caller that already knows
// should say so rather than asking.
[[nodiscard]] bool formatOf(const QString &path, AudioPluginFormat &format);

// Opens a plugin file. EXECUTES IT -- see the class comment.
[[nodiscard]] std::shared_ptr<AudioPluginModule> openModule(AudioPluginFormat format,
                                                            const QString &bundlePath,
                                                            PluginError &error);

// Opens a bundle, reads what it publishes, and closes it again.
//
// This is what a settings panel calls to fill a list. It still runs the
// plugin's code, so it is still gated on trust; the difference from a load is
// only that nothing is kept afterwards.
[[nodiscard]] QList<AudioPluginDescriptor> describe(AudioPluginFormat format,
                                                    const QString &bundlePath,
                                                    PluginError &error);

} // namespace PluginScanner

} // namespace OpenChat
