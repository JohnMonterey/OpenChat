include(FetchContent)
find_package(Threads REQUIRED)

FetchContent_Declare(
    sqlcipher_source
    URL https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v4.17.0.tar.gz
    URL_HASH SHA256=79c0e164b9c059e7487bf8f29272f601cca5f3312cc267461f81e349962a5058
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlcipher_source)

set(_sqlcipher_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/sqlcipher-generated")
set(_sqlcipher_c "${_sqlcipher_generated_dir}/sqlite3.c")
set(_sqlcipher_h "${_sqlcipher_generated_dir}/sqlite3.h")
file(MAKE_DIRECTORY "${_sqlcipher_generated_dir}")

if(MSVC)
    find_program(OPENCHAT_NMAKE_EXECUTABLE nmake REQUIRED)
    add_custom_command(
        OUTPUT "${_sqlcipher_c}" "${_sqlcipher_h}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sqlcipher_generated_dir}"
        COMMAND "${OPENCHAT_NMAKE_EXECUTABLE}"
                /f "${sqlcipher_source_SOURCE_DIR}/Makefile.msc"
                TOP="${sqlcipher_source_SOURCE_DIR}"
                sqlite3.c
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${sqlcipher_source_SOURCE_DIR}/sqlite3.c" "${_sqlcipher_c}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${sqlcipher_source_SOURCE_DIR}/sqlite3.h" "${_sqlcipher_h}"
        WORKING_DIRECTORY "${sqlcipher_source_SOURCE_DIR}"
        VERBATIM
    )
else()
    find_program(OPENCHAT_MAKE_EXECUTABLE make REQUIRED)
    add_custom_command(
        OUTPUT "${_sqlcipher_c}" "${_sqlcipher_h}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_sqlcipher_generated_dir}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "CFLAGS=-O2 -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL"
                "${sqlcipher_source_SOURCE_DIR}/configure"
                --disable-shared
                --enable-static
                --disable-tcl
                --disable-load-extension
                --disable-math
                --with-tempstore=yes
        COMMAND "${OPENCHAT_MAKE_EXECUTABLE}" sqlite3.c sqlite3.h
        WORKING_DIRECTORY "${_sqlcipher_generated_dir}"
        VERBATIM
    )
endif()

add_library(openchat_sqlcipher STATIC "${_sqlcipher_c}")
# GCC 16 on Linux x86-64 folds splitmix64's 64-bit seed constants into
# 32-bit TLS relocations against xoshiro_s, causing link overflows. PIC with
# initial-exec TLS avoids those relocations while retaining optimization.
# Initial-exec is safe here: this static library is linked into executables.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 16
   AND CMAKE_C_COMPILER_VERSION VERSION_LESS 17
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set_target_properties(openchat_sqlcipher PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(openchat_sqlcipher PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:-ftls-model=initial-exec>"
    )
endif()
target_include_directories(openchat_sqlcipher PUBLIC "${_sqlcipher_generated_dir}")
target_compile_definitions(openchat_sqlcipher
    PUBLIC SQLITE_HAS_CODEC
    PRIVATE
    SQLCIPHER_CRYPTO_OPENSSL
    SQLITE_THREADSAFE=2
    SQLITE_TEMP_STORE=2
    SQLITE_EXTRA_INIT=sqlcipher_extra_init
    SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
    SQLITE_OMIT_LOAD_EXTENSION
)
target_link_libraries(openchat_sqlcipher
    PUBLIC OpenSSL::Crypto Threads::Threads ${CMAKE_DL_LIBS}
)
