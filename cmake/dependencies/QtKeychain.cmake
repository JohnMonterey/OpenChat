include(FetchContent)

set(BUILD_WITH_QT6 ON CACHE BOOL "Build qtkeychain with Qt 6" FORCE)
set(BUILD_TEST_APPLICATION OFF CACHE BOOL "Build qtkeychain test application" FORCE)
set(BUILD_QTQUICK_DEMO OFF CACHE BOOL "Build qtkeychain Qt Quick demo" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "Build qtkeychain translations" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)
set(LIBSECRET_SUPPORT OFF CACHE BOOL "Use KWallet DBus backend on Unix" FORCE)

# qtkeychain includes CTest internally. Keep its upstream integration tests out of
# OpenChat's default build while preserving this project's BUILD_TESTING value.
set(_openchat_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)

FetchContent_Declare(
    qtkeychain
    URL https://github.com/frankosterfeld/qtkeychain/archive/refs/tags/0.16.0.tar.gz
    URL_HASH SHA256=3be26ec4ae30eecf0c2ff7572ba83799791b157c76e15a05ef35f23dc25e4054
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(qtkeychain)

set(BUILD_TESTING "${_openchat_build_testing}")
unset(_openchat_build_testing)
