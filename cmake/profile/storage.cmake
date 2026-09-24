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
