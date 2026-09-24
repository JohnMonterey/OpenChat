# U3 Song: SongCodec, SongImport, SongPlayer and tst_profilesong.

# The Opus song codec is plain sample arithmetic plus libopus, like the rest of
# openchat_media, so it stays free of Qt Multimedia and testable from a file.
target_sources(openchat_media PRIVATE
    src/media/SongCodec.cpp
)

# Import (WAV by hand everywhere; other formats through QAudioDecoder) and the
# page's mini player. Both reach Qt Multimedia only on demand: a WAV import
# never loads it and a player loads it on its first play().
target_sources(openchat_profile PRIVATE
    src/profile/SongImport.cpp
    src/profile/SongPlayer.cpp
)

# openchat_opus is named so the test can build a packet libopus itself refuses
# (the premise of the concealment test). Playback runs against a fake output,
# so the suite needs no sound card and makes no sound.
add_executable(tst_profilesong tests/tst_profilesong.cpp)
target_include_directories(tst_profilesong PRIVATE src)
target_link_libraries(tst_profilesong PRIVATE
    openchat_profile openchat_media openchat_domain openchat_opus Qt6::Multimedia Qt6::Test)
add_test(NAME tst_profilesong COMMAND tst_profilesong)
set_tests_properties(tst_profilesong PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
