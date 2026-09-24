# U1 Domain: ProfilePage, ProfilePageCodec, SongContainer and tst_profilepage.

# The page model, its wire codec and the song container. Qt Core only, like the
# rest of openchat_domain: storage, sync and the GUI-less tests all use them.
target_sources(openchat_domain PRIVATE
    src/domain/ProfilePage.cpp
    src/domain/ProfilePageCodec.cpp
    src/domain/SongContainer.cpp
)

add_executable(tst_profilepage tests/tst_profilepage.cpp)
target_include_directories(tst_profilepage PRIVATE src)
target_link_libraries(tst_profilepage PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_profilepage COMMAND tst_profilepage)
