// The interface IDs this host actually uses.
//
// VST3's headers DECLARE each interface's `iid` and leave it undefined; the SDK
// defines them in translation units a plugin project links and a host project
// is expected to provide for itself. Omitting this file does not produce a
// warning or a subtly wrong result -- it produces a link error naming
// `Steinberg::Vst::IComponent::iid` and friends, which is the single most
// common way a first attempt at a VST3 host fails to build.
//
// Only the interfaces this host queries are listed. Adding an interface to the
// backend means adding it here too; the failure is loud, immediate and at link
// time, so it cannot be got wrong silently.
//
// pluginterfaces/base/coreiids.cpp, compiled into openchat_vst3, already
// defines the non-VST base ones (FUnknown, IPluginBase, IPluginFactory,
// IBStream). Repeating any of them here would be a duplicate symbol.

#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>

namespace Steinberg {
namespace Vst {

DEF_CLASS_IID(IComponent)
DEF_CLASS_IID(IAudioProcessor)
DEF_CLASS_IID(IEditController)
DEF_CLASS_IID(IConnectionPoint)
DEF_CLASS_IID(IComponentHandler)
DEF_CLASS_IID(IHostApplication)
DEF_CLASS_IID(IParameterChanges)
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IAttributeList)
DEF_CLASS_IID(IMessage)

} // namespace Vst
} // namespace Steinberg
