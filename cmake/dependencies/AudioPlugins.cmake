# The two plug-in ABIs the voice effects subsystem can host.
#
# Both are vendored under third_party/ rather than found on the system, which is
# the opposite of what Opus and SQLCipher do, and deliberately so. Those are
# libraries: any conforming copy computes the same answer, so taking the
# distribution's is strictly better than shipping our own. These are *ABI
# descriptions* -- struct layouts, vtable orders and interface IDs that every
# plugin on the user's disk was already compiled against. A host that picks up
# a differently-versioned copy from the build machine does not fail to
# configure; it compiles cleanly and then reads a plugin's fields at the wrong
# offsets at run time. Pinning the exact headers is what makes the contract
# knowable, so there is no system-copy branch here on purpose.
#
# Neither ABI costs a runtime dependency: nothing here links against anything a
# user must install, and a build with no plugins on the machine still builds
# every line of this code.

# CLAP -- MIT, and genuinely header-only. There is no library, no init, and no
# code to compile: the whole ABI is data layouts plus one exported symbol the
# plugin defines. This is why the CLAP backend is a few hundred lines and the
# VST3 one is not.
add_library(openchat_clap INTERFACE)
target_include_directories(openchat_clap SYSTEM INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/clap/include")

# VST 3 -- MIT since 3.8, and header-only apart from the interface IDs.
#
# DECLARE_CLASS_IID in the headers only *declares* each interface's iid; the
# definitions live in these four translation units. Without them every
# FUnknown::iid reference is an undefined symbol at link time, which is the
# single most common way a first attempt at a VST3 host fails to build. They
# are compiled here once rather than pulled in per consumer so that the
# definitions exist exactly once in the link.
#
# SYSTEM on the include directory is not cosmetic: this project builds with
# -Wall -Wextra and the SDK headers do not, so without it third-party warnings
# would drown our own.
add_library(openchat_vst3 STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/vst3/pluginterfaces/base/funknown.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/vst3/pluginterfaces/base/coreiids.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/vst3/pluginterfaces/base/conststringtable.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/vst3/pluginterfaces/base/ustring.cpp"
)
target_include_directories(openchat_vst3 SYSTEM PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/vst3")
set_target_properties(openchat_vst3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The SDK's own sources are not ours to keep warning-clean.
if(NOT MSVC)
    target_compile_options(openchat_vst3 PRIVATE -w)
endif()
