# OpenChat relay: a separate ciphertext-only server executable plus a service
# library and its tests. The relay stores and routes opaque envelopes; it never
# holds message plaintext, keys, or recovery material.

add_library(openchat_relay_core STATIC
    src/RelayCrypto.cpp
    src/PostgresStore.cpp
    src/AuthService.cpp
    src/KeyPackageService.cpp
    src/EnvelopeService.cpp
    src/DirectoryService.cpp
)
target_include_directories(openchat_relay_core PUBLIC src)
target_link_libraries(openchat_relay_core
    PUBLIC openchat_protocol openchat_domain Qt6::Core Qt6::Sql
    PRIVATE OpenSSL::Crypto
)
qt_add_resources(openchat_relay_core "openchat_relay_migrations"
    PREFIX "/relay"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/migrations"
    FILES
        migrations/001_accounts_devices.sql
        migrations/002_tokens_keypackages.sql
        migrations/003_inboxes_attachments.sql
        migrations/004_invites.sql
)

add_library(openchat_relay_server STATIC
    src/RelayServer.cpp
)
target_include_directories(openchat_relay_server PUBLIC src)
target_link_libraries(openchat_relay_server
    PUBLIC openchat_relay_core Qt6::Core Qt6::Network Qt6::HttpServer Qt6::WebSockets
)

add_executable(openchat-relay src/main.cpp)
target_link_libraries(openchat-relay PRIVATE
    openchat_relay_server
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::HttpServer
    Qt6::WebSockets
    Qt6::Sql
)

add_executable(tst_relayservices tests/tst_relayservices.cpp)
target_include_directories(tst_relayservices PRIVATE src)
target_link_libraries(tst_relayservices PRIVATE
    openchat_relay_core
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Sql
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_relayservices COMMAND tst_relayservices)
