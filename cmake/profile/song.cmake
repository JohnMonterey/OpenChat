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
