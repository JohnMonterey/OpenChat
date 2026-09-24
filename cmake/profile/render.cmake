# U4 Render: the profile painted items, fonts, media store, readability and tst_profilerender.

# The page's native pieces: the backdrop and its motifs, the background
# picture layer, the styled name, the ambient sprites, the preset miniatures
# and mood faces, the contrast engine, the decoded-picture store, the
# background import pipeline, the bundled fonts, the render policy, the shared
# animation ticker and the QML registration.
target_sources(openchat_profile PRIVATE
    src/profile/ProfileRenderPolicy.cpp
    src/profile/ProfileTicker.cpp
    src/profile/ProfileMotifs.cpp
    src/profile/ProfileReadability.cpp
    src/profile/ProfileMediaStore.cpp
    src/profile/ProfileBackgroundImage.cpp
    src/profile/ProfileFonts.cpp
    src/profile/ProfileBackdropItem.cpp
    src/profile/ProfileImageLayerItem.cpp
    src/profile/ProfileNameTextItem.cpp
    src/profile/ProfileAmbientItem.cpp
    src/profile/ProfilePresetThumbItem.cpp
    src/profile/ProfileMoodFaceItem.cpp
    src/profile/ProfileQmlTypes.cpp
)

# The eight bundled content faces (SPEC §11), with their licences. Registered
# lazily by ProfileFonts::ensureRegistered(), never at start-up.
qt_add_resources(openchat_profile "openchat_profile_fonts"
    PREFIX "/openchat/profile-fonts"
    BASE "assets/fonts"
    FILES
        assets/fonts/README.md
        assets/fonts/fredoka/FredokaMedium.ttf
        assets/fonts/fredoka/FredokaSemiBold.ttf
        assets/fonts/fredoka/OFL.txt
        assets/fonts/pacifico/Pacifico-Regular.ttf
        assets/fonts/pacifico/OFL.txt
        assets/fonts/courierprime/CourierPrime-Regular.ttf
        assets/fonts/courierprime/CourierPrime-Bold.ttf
        assets/fonts/courierprime/OFL.txt
        assets/fonts/pressstart2p/PressStart2P-Regular.ttf
        assets/fonts/pressstart2p/OFL.txt
        assets/fonts/unifrakturmaguntia/UnifrakturMaguntia-Book.ttf
        assets/fonts/unifrakturmaguntia/OFL.txt
        assets/fonts/orbitron/OpenChatFuture-SemiBold.ttf
        assets/fonts/orbitron/OpenChatFuture-ExtraBold.ttf
        assets/fonts/orbitron/OFL.txt
        assets/fonts/playfairdisplay/OpenChatSerif-Regular.ttf
        assets/fonts/playfairdisplay/OpenChatSerif-Bold.ttf
        assets/fonts/playfairdisplay/OFL.txt
        assets/fonts/permanentmarker/PermanentMarker-Regular.ttf
        assets/fonts/permanentmarker/LICENSE.txt
)

# The renderer's tests: motifs, backdrop, picture layer, media store, the
# background pipeline, the contrast engine against SPEC §10's measured table,
# the name art, ambient, ticker, fonts, thumbnails and QML registration.
# Offscreen and on the software scene graph, like the app's QML suites.
# OPENCHAT_PROFILE_CAPTURES=<dir> also writes review PNGs.
add_executable(tst_profilerender tests/tst_profilerender.cpp)
target_include_directories(tst_profilerender PRIVATE src)
target_link_libraries(tst_profilerender PRIVATE
    openchat_profile openchat_cosmetics openchat_avatar openchat_domain
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Test)
target_compile_definitions(tst_profilerender PRIVATE OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME tst_profilerender COMMAND tst_profilerender)
set_tests_properties(tst_profilerender PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
