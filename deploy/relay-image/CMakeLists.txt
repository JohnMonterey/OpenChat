cmake_minimum_required(VERSION 3.21)

# Deployment-only build definition for the OpenChat relay container image.
#
# The repository's top-level CMakeLists builds the whole Qt Quick client
# (Quick, QuickControls2, Svg, DBus, QtKeychain, SqlCipher and the Rust MLS
# library), none of which the relay needs. Configuring that project inside a
# container image would pull in an enormous, mostly irrelevant toolchain.
#
# This trimmed project configures ONLY the relay executable and the two
# libraries it links against (openchat_domain, openchat_protocol). The library
# and target definitions mirror the top-level CMakeLists and relay/CMakeLists.txt
# exactly, so the relay subdirectory builds unmodified via add_subdirectory().

project(openchat_relay_image LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Network HttpServer WebSockets Sql Test)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

qt_standard_project_setup()
enable_testing()

# Mirrors the openchat_domain target in the top-level CMakeLists.txt.
add_library(openchat_domain STATIC
    src/domain/Identifiers.cpp
)
target_include_directories(openchat_domain PUBLIC src)
target_link_libraries(openchat_domain PUBLIC Qt6::Core)

# Mirrors the openchat_protocol target in the top-level CMakeLists.txt.
add_library(openchat_protocol STATIC
    src/protocol/CiphertextEnvelope.cpp
    src/protocol/CanonicalCborCodec.cpp
)
target_include_directories(openchat_protocol PUBLIC src)
target_link_libraries(openchat_protocol PUBLIC openchat_domain Qt6::Core)

# Builds openchat_relay_core, openchat_relay_server and the openchat-relay
# executable (and the tst_relayservices target, which is not compiled unless
# explicitly requested). The migrations are compiled into the binary here.
add_subdirectory(relay)
