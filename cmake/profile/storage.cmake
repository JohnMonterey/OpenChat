# U2 Storage: migration 016, the profile page repository and tst_profilepagestore.

target_sources(openchat_storage PRIVATE
    src/storage/SqlCipherProfilePageRepository.cpp
)
# A resource list of its own so this branch never edits CMakeLists.txt; the
# path is the one migrate() reads, :/openchat/016_profile_pages.sql. Folded
# into "openchat_migrations" when the feature is finished.
qt_add_resources(openchat_storage "openchat_migrations_profiles"
    PREFIX "/openchat"
    BASE "src/storage/migrations"
    FILES
        src/storage/migrations/016_profile_pages.sql
)

# openchat_sqlcipher is named so the test compiles against SQLCipher's own
# sqlite3.h (it opens raw connections to build old schemas and to write rows
# that bypass the repository), not a system one.
add_executable(tst_profilepagestore tests/tst_profilepagestore.cpp)
target_include_directories(tst_profilepagestore PRIVATE src)
target_link_libraries(tst_profilepagestore
    PRIVATE openchat_app openchat_storage openchat_sqlcipher Qt6::Core Qt6::Test
)
add_test(NAME tst_profilepagestore COMMAND tst_profilepagestore)
