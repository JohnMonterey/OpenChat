# U5 Sync: ProfilePageSync and tst_profilepagesync.

# Publishing, the paced delivery pump, receiving and background requests for
# profile pages, over the ProfileUpdate envelopes of each contact's 1:1
# conversation. Qt Core only, like the rest of openchat_app.
target_sources(openchat_app PRIVATE
    src/app/ProfilePageSync.cpp
)

# Real peers (ProfileSession, SyncEngine, MLS group) trading pages through
# scripted transports. openchat_sqlcipher is named so the outbox inspector
# compiles against SQLCipher's own sqlite3.h, not a system one.
add_executable(tst_profilepagesync
    tests/tst_profilepagesync.cpp
    tests/PageSyncTestSupport.h
)
target_include_directories(tst_profilepagesync PRIVATE src tests)
target_link_libraries(tst_profilepagesync PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_domain
    openchat_sqlcipher
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_profilepagesync COMMAND tst_profilepagesync)
set_tests_properties(tst_profilepagesync PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
