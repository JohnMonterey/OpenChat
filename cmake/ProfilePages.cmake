# Profile pages, while they are being built. Each unit of the feature edits only
# its own fragment under cmake/profile/, so branches worked on in parallel never
# collide in CMakeLists.txt. The finishing unit folds every fragment into
# CMakeLists.txt at the usual places and deletes this file, cmake/profile/ and
# src/profile/ProfileLibrary.cpp.

# The profile page's native pieces: the painted items, fonts, the contrast
# engine, the media store and song import and playback. ProfileLibrary.cpp only
# holds the library open until its first real source arrives.
add_library(openchat_profile STATIC
    src/profile/ProfileLibrary.cpp
)
target_include_directories(openchat_profile PUBLIC src)
target_link_libraries(openchat_profile
    PUBLIC openchat_domain openchat_media openchat_avatar openchat_cosmetics
           Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Multimedia
    PRIVATE openchat_opus
)

include(cmake/profile/domain.cmake)
include(cmake/profile/engine.cmake)
include(cmake/profile/storage.cmake)
include(cmake/profile/song.cmake)
include(cmake/profile/render.cmake)
include(cmake/profile/sync.cmake)
include(cmake/profile/controller.cmake)
include(cmake/profile/pageqml.cmake)
include(cmake/profile/editor.cmake)
include(cmake/profile/integration.cmake)
