# U3 Song: SongCodec, SongImport, SongPlayer and tst_profilesong.

# The Opus song codec is plain sample arithmetic plus libopus, like the rest of
# openchat_media, so it stays free of Qt Multimedia and testable from a file.
target_sources(openchat_media PRIVATE
    src/media/SongCodec.cpp
)
