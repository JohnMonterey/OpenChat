function(add_openchat_rust_library)
    find_program(OPENCHAT_CARGO_EXECUTABLE cargo REQUIRED)

    set(OPENCHAT_MLS_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/rust/openchat-mls/Cargo.toml")
    set(OPENCHAT_MLS_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo")
    if(WIN32)
        set(OPENCHAT_MLS_LIBRARY
            "${OPENCHAT_MLS_TARGET_DIR}/release/openchat_mls.lib")
    else()
        set(OPENCHAT_MLS_LIBRARY
            "${OPENCHAT_MLS_TARGET_DIR}/release/libopenchat_mls.a")
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
