# Opus for voice calls -- guaranteed, not optional.
#
# Opus is what makes a call practical on a real link: 24 kbit/s instead of the
# 768 kbit/s raw 48 kHz PCM costs. A build without it would still *work* -- the
# pipeline is codec-agnostic and the lossless PCM codec is always present -- but
# it would flood a domestic uplink and the relay with 40 kB/s per direction, and
# nothing in the UI would show why calls sound terrible. That is a failure mode a
# packager should never be able to reach by accident, so Opus follows the same
# rule as SQLCipher: use the system copy when there is one, build it from source
# when there is not, and fail to configure if neither is possible.
#
# Defines the openchat_opus interface target, which carries whatever include and
# link settings the chosen copy needs. `#include <opus.h>` is the form that works
# for every source: opus.pc puts ${includedir}/opus on the path, the plain search
# below adds that directory itself, and the bundled build exposes its include/.

include(FetchContent)

option(OPENCHAT_BUNDLED_OPUS
       "Build Opus from source even when a system libopus is present" OFF)

add_library(openchat_opus INTERFACE)

set(_openchat_opus_found FALSE)

if(NOT OPENCHAT_BUNDLED_OPUS)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENCHAT_OPUS_PC QUIET IMPORTED_TARGET opus)
        if(OPENCHAT_OPUS_PC_FOUND)
            set(_openchat_opus_found TRUE)
            target_link_libraries(openchat_opus INTERFACE PkgConfig::OPENCHAT_OPUS_PC)
            message(STATUS "OpenChat: using system Opus ${OPENCHAT_OPUS_PC_VERSION}")
        endif()
    endif()

    if(NOT _openchat_opus_found)
        # Platforms without pkg-config (or with Opus installed by hand) still get
        # the system copy when it is genuinely present.
        find_path(OPENCHAT_OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus)
        find_library(OPENCHAT_OPUS_LIBRARY NAMES opus)
        if(OPENCHAT_OPUS_INCLUDE_DIR AND OPENCHAT_OPUS_LIBRARY)
            set(_openchat_opus_found TRUE)
            target_include_directories(openchat_opus INTERFACE "${OPENCHAT_OPUS_INCLUDE_DIR}")
            target_link_libraries(openchat_opus INTERFACE "${OPENCHAT_OPUS_LIBRARY}")
            message(STATUS "OpenChat: using system Opus at ${OPENCHAT_OPUS_LIBRARY}")
        endif()
    endif()
endif()

if(NOT _openchat_opus_found)
    message(STATUS "OpenChat: no system Opus; building it from source")
    FetchContent_Declare(
        opus_source
        URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
        URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Only the static library is wanted: no tools, no demo programs, and none of
    # the install rules, which would otherwise put a second libopus into this
    # project's own install tree.
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    # Opus turns its own test suite on whenever BUILD_TESTING is set in the
    # enclosing scope. This project does not set it today, but a future
    # include(CTest) would, and would silently add Opus's tests to ours; shadow
    # it for the duration of the subdirectory so that can never happen.
    set(_openchat_saved_build_testing "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)
    FetchContent_MakeAvailable(opus_source)
    set(BUILD_TESTING "${_openchat_saved_build_testing}")
    unset(_openchat_saved_build_testing)

    if(NOT TARGET opus)
        message(FATAL_ERROR
            "OpenChat: the bundled Opus build did not produce a library target. "
            "Install libopus (with headers) or check the network for the download.")
    endif()
    target_link_libraries(openchat_opus INTERFACE opus)
    set(_openchat_opus_found TRUE)
endif()

if(NOT _openchat_opus_found)
    message(FATAL_ERROR "OpenChat: Opus is required for voice calls and could not be provided.")
endif()
unset(_openchat_opus_found)
