# libvpx, for screen sharing as a VP9 video stream (src/call/ScreenVideoCodec).
#
# Without it a share falls back to the tile encoder, which re-sends changed
# 128-pixel squares as PNG/JPEG: fine for a still desktop, choppy for anything
# that moves. So a build without libvpx still works but should not be what
# anybody ships, and says so loudly at configure time.
#
# Linux: the distribution's libvpx (pkg-config vpx). Windows (MinGW):
# mingw-w64-libvpx, which tools/windows/rootless-toolchain.sh installs.
# macOS: `brew install libvpx`.
#
# Defines the openchat_vpx interface target and OPENCHAT_HAVE_VPX.

add_library(openchat_vpx INTERFACE)
set(OPENCHAT_HAVE_VPX OFF)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPENCHAT_VPX_PC QUIET IMPORTED_TARGET vpx)
    if(OPENCHAT_VPX_PC_FOUND)
        set(OPENCHAT_HAVE_VPX ON)
        target_link_libraries(openchat_vpx INTERFACE PkgConfig::OPENCHAT_VPX_PC)
        message(STATUS "OpenChat: using libvpx ${OPENCHAT_VPX_PC_VERSION} for screen sharing")
    endif()
endif()

if(NOT OPENCHAT_HAVE_VPX)
    find_path(OPENCHAT_VPX_INCLUDE_DIR vpx/vpx_encoder.h)
    find_library(OPENCHAT_VPX_LIBRARY NAMES vpx libvpx)
    if(OPENCHAT_VPX_INCLUDE_DIR AND OPENCHAT_VPX_LIBRARY)
        set(OPENCHAT_HAVE_VPX ON)
        target_include_directories(openchat_vpx INTERFACE "${OPENCHAT_VPX_INCLUDE_DIR}")
        target_link_libraries(openchat_vpx INTERFACE "${OPENCHAT_VPX_LIBRARY}")
        message(STATUS "OpenChat: using libvpx at ${OPENCHAT_VPX_LIBRARY} for screen sharing")
    endif()
endif()

if(OPENCHAT_HAVE_VPX)
    target_compile_definitions(openchat_vpx INTERFACE OPENCHAT_HAVE_VPX=1)
else()
    target_compile_definitions(openchat_vpx INTERFACE OPENCHAT_HAVE_VPX=0)
    message(WARNING
        "OpenChat: libvpx not found. Screen sharing will fall back to the tile encoder, "
        "which is choppy for anything that moves. Install libvpx (pkg-config 'vpx').")
endif()
