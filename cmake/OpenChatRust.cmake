function(add_openchat_rust_library)
    find_program(OPENCHAT_CARGO_EXECUTABLE cargo REQUIRED)

    set(OPENCHAT_MLS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/Cargo.toml")
    set(OPENCHAT_MLS_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")

    # Cross-compiling (a MinGW build of the Windows app from Linux, say) needs
    # cargo told which target to build for; a native build must not be, since
    # `--target` moves the output under a per-target subdirectory. Cargo's
    # triple can be given explicitly through OPENCHAT_RUST_TARGET; otherwise it
    # is derived from the toolchain for the one cross case this project knows.
    set(OPENCHAT_RUST_TARGET "" CACHE STRING
        "Rust target triple to pass to cargo (empty: cargo's host default)")
    set(_openchat_rust_target "${OPENCHAT_RUST_TARGET}")
    if(NOT _openchat_rust_target AND CMAKE_CROSSCOMPILING AND MINGW
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_openchat_rust_target "x86_64-pc-windows-gnu")
    endif()

    set(_openchat_cargo_target_args)
    set(_openchat_cargo_output_dir "${OPENCHAT_MLS_TARGET_DIR}/release")
    if(_openchat_rust_target)
        set(_openchat_cargo_target_args --target "${_openchat_rust_target}")
        set(_openchat_cargo_output_dir
            "${OPENCHAT_MLS_TARGET_DIR}/${_openchat_rust_target}/release")
    endif()

    # MSVC names the static library openchat_mls.lib; every GNU-flavoured
    # toolchain, MinGW included, names it libopenchat_mls.a.
    if(MSVC)
        set(OPENCHAT_MLS_LIBRARY "${_openchat_cargo_output_dir}/openchat_mls.lib")
    else()
        set(OPENCHAT_MLS_LIBRARY "${_openchat_cargo_output_dir}/libopenchat_mls.a")
    endif()

    file(GLOB_RECURSE OPENCHAT_MLS_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/include/*.h"
    )

    add_custom_command(
        OUTPUT "${OPENCHAT_MLS_LIBRARY}"
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${OPENCHAT_MLS_TARGET_DIR}"
            ${OPENCHAT_CARGO_EXECUTABLE} build
                --manifest-path "${OPENCHAT_MLS_MANIFEST}"
                --release
                --locked
                ${_openchat_cargo_target_args}
        DEPENDS
            ${OPENCHAT_MLS_SOURCES}
            "${OPENCHAT_MLS_MANIFEST}"
            "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/Cargo.lock"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building the pinned OpenMLS bridge"
        VERBATIM
    )
    add_custom_target(openchat_mls_rust_build DEPENDS "${OPENCHAT_MLS_LIBRARY}")

    add_library(openchat_mls_rust STATIC IMPORTED GLOBAL)
    set_target_properties(openchat_mls_rust PROPERTIES
        IMPORTED_LOCATION "${OPENCHAT_MLS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/include"
    )
    add_dependencies(openchat_mls_rust openchat_mls_rust_build)

    if(WIN32)
        set_property(TARGET openchat_mls_rust PROPERTY INTERFACE_LINK_LIBRARIES
            "bcrypt;ntdll;userenv;ws2_32;advapi32")
    elseif(APPLE)
        find_library(OPENCHAT_SECURITY_FRAMEWORK Security REQUIRED)
        find_library(OPENCHAT_SYSTEM_CONFIGURATION_FRAMEWORK SystemConfiguration REQUIRED)
        set_property(TARGET openchat_mls_rust PROPERTY INTERFACE_LINK_LIBRARIES
            "${OPENCHAT_SECURITY_FRAMEWORK};${OPENCHAT_SYSTEM_CONFIGURATION_FRAMEWORK}")
    else()
        set_property(TARGET openchat_mls_rust PROPERTY INTERFACE_LINK_LIBRARIES
            "dl;pthread;m;rt;util")
    endif()
endfunction()
