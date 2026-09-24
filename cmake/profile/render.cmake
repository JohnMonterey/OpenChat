# U4 Render: the profile painted items, fonts, media store, readability and tst_profilerender.

# The page's native pieces: the backdrop and its motifs, the background
# picture layer, the styled name, the ambient sprites, the preset miniatures
# and mood faces, the contrast engine, the decoded-picture store, the
# background import pipeline, the bundled fonts, the render policy and the
# shared animation ticker.
target_sources(openchat_profile PRIVATE
    src/profile/ProfileRenderPolicy.cpp
    src/profile/ProfileTicker.cpp
    src/profile/ProfileMotifs.cpp
    src/profile/ProfileReadability.cpp
    src/profile/ProfileMediaStore.cpp
)
