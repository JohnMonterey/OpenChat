cmake_minimum_required(VERSION 3.21)

project(OpenChat VERSION 0.2.9 LANGUAGES C CXX)

# The macOS notification backend is Objective-C++ (UNUserNotificationCenter has
# no C interface). Enabled here rather than beside the target, because a
# language must be enabled in the top-level scope before anything uses it.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui HttpServer Multimedia Network Quick QuickControls2 Sql Svg Test WebSockets)
# The notification-area icon drives the platform's tray directly (see
# src/app/TrayIcon.h), which is Qt Gui's private platform interface. That ties
# a build to the Qt it was built against, which every build here already is:
# the Windows package ships its own Qt, and the Linux build is rebuilt with
# the system's.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

include(cmake/dependencies/AudioPlugins.cmake)
include(cmake/dependencies/Opus.cmake)
include(cmake/dependencies/Vpx.cmake)
include(cmake/dependencies/QtKeychain.cmake)
include(cmake/dependencies/SqlCipher.cmake)
include(cmake/OpenChatRust.cmake)
add_openchat_rust_library()

qt_standard_project_setup(REQUIRES 6.5)
add_library(openchat_case STATIC
    src/case/DailyCaseService.cpp
    src/case/DailyCaseController.cpp
    src/case/CaseAudio.cpp
)
target_include_directories(openchat_case PUBLIC src)
# CaseAudio decodes its bundled crate samples with the defensive WavFile reader.
# The claim draws its reward from the cosmetics catalogue's rarity tiers.
target_link_libraries(openchat_case PUBLIC Qt6::Core Qt6::Multimedia
    PRIVATE openchat_media openchat_cosmetics)

# Collectible cosmetics: chat-bubble skins, avatar frames, presence beads, name
# flair and profile scenes. All QPainter artwork, so it renders on the software scene
# graph (which runs no shaders) exactly as on the hardware one.
add_library(openchat_cosmetics STATIC
    src/cosmetics/CosmeticCatalog.cpp
    src/cosmetics/PeerCosmetics.cpp
    src/cosmetics/CosmeticPaint.cpp
    src/cosmetics/CosmeticTypes.cpp
    src/cosmetics/AvatarFrameItem.cpp
    src/cosmetics/FramePainters.cpp
    src/cosmetics/BeadArtItem.cpp
    src/cosmetics/NameFlairItem.cpp
    src/cosmetics/ProfileSceneItem.cpp
    src/cosmetics/ProceduralNoise.cpp
    src/cosmetics/BubbleSkins.cpp
)
target_include_directories(openchat_cosmetics PUBLIC src)
target_link_libraries(openchat_cosmetics PUBLIC openchat_domain Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
    qt_policy(SET QTP0004 NEW)
endif()

# Profile pages' native pieces: the backdrop and its motifs, the background
# picture layer, the styled name, the ambient sprites, the preset miniatures
# and mood faces, the contrast engine, the decoded-picture store, the
# background import pipeline, the bundled fonts, the render policy, the shared
# animation ticker, song import and the page's mini player, and the QML
# registration. QPainter artwork like the cosmetics; Qt Multimedia is reached
# only on demand (a WAV import never loads it, a player on its first play()).
add_library(openchat_profile STATIC
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
    src/profile/SongImport.cpp
    src/profile/SongPlayer.cpp
)
target_include_directories(openchat_profile PUBLIC src)
target_link_libraries(openchat_profile
    PUBLIC openchat_domain openchat_media openchat_avatar openchat_cosmetics
           Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Multimedia
    PRIVATE openchat_opus
)
# The eight bundled content faces (docs/profile-pages.md), with their
# licences. Registered lazily by ProfileFonts::ensureRegistered(), never at
# start-up.
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

# The QML-facing profile controller, its page objects and resolved style, the
# editor's undo history and the reference mock's pages. ChatController owns
# the controller, so these are compiled beside ChatController.cpp in every
# target that compiles it, each also linking openchat_profile.
set(profile_controller_sources
    src/controllers/ProfileController.cpp
    src/controllers/ProfilePageObject.cpp
    src/controllers/ProfileReferencePages.cpp
    src/controllers/ProfileEditHistory.cpp
)

set_source_files_properties(qml/OpenChat/Theme.qml qml/OpenChat/VocalFx.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_executable(OpenChat
    src/main.cpp
    src/controllers/ChatController.cpp
    ${profile_controller_sources}
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/CallController.cpp
    src/controllers/CrashReportController.cpp
    src/controllers/VoiceDebugController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
    src/render/CallVideoItem.cpp
    src/render/TrayOrb.cpp
    src/app/AppearanceSettings.cpp
    src/app/CloseToTray.cpp
    src/app/TrayIcon.cpp
    src/app/MemorySettings.cpp
    src/app/MicrophoneSettings.cpp
    src/app/VoiceEffectHost.cpp
    src/app/ComposerEditing.cpp
    src/app/TextLineSpacing.cpp
    src/app/TransportSettings.cpp
)

# A GUI program on Windows, so launching it opens no console window. Run from
# a Command Prompt it borrows that prompt's console for what it prints (see
# attachParentConsole in main.cpp).
set_target_properties(OpenChat PROPERTIES WIN32_EXECUTABLE TRUE)

if(APPLE)
    configure_file(cmake/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist @ONLY)
    # Declare usage strings to Qt's permission-plugin finalizer and embed them
    # in the existing standalone executable as well as any future app bundle.
    set_target_properties(OpenChat PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist)
    target_link_options(OpenChat PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist")
    set_property(TARGET OpenChat APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist)
endif()

qt_add_qml_module(OpenChat
    URI OpenChat
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/OpenChat
    QML_FILES
        qml/OpenChat/Main.qml
        qml/OpenChat/Onboarding.qml
        qml/OpenChat/CrashNotice.qml
        qml/OpenChat/VoiceDebugWindow.qml
        qml/OpenChat/Theme.qml
        qml/OpenChat/VocalFx.qml
        qml/OpenChat/components/VocalFxPanel.qml
        qml/OpenChat/components/VocalFxPicker.qml
        qml/OpenChat/components/VoiceEffectsMenu.qml
        qml/OpenChat/components/PresenceBead.qml
        qml/OpenChat/components/Avatar.qml
        qml/OpenChat/components/AeroTextField.qml
        qml/OpenChat/components/AeroButton.qml
        qml/OpenChat/components/AeroSwitch.qml
        qml/OpenChat/components/AeroSlider.qml
        qml/OpenChat/components/AeroMenu.qml
        qml/OpenChat/components/AeroMenuItem.qml
        qml/OpenChat/components/AeroMenuSetting.qml
        qml/OpenChat/components/MicrophoneContextMenu.qml
        qml/OpenChat/components/MicrophoneSettingsPanel.qml
        qml/OpenChat/components/LowMemoryPanel.qml
        qml/OpenChat/components/ConnectionSettingsPanel.qml
        qml/OpenChat/components/SettingsPage.qml
        qml/OpenChat/components/ContactSidebar.qml
        qml/OpenChat/components/ContactCategory.qml
        qml/OpenChat/components/ContactRow.qml
        qml/OpenChat/components/ContactStatusBubble.qml
        qml/OpenChat/components/AddContactDialog.qml
        qml/OpenChat/components/SafetyNumberDialog.qml
        qml/OpenChat/components/RequestRow.qml
        qml/OpenChat/components/ConversationHeader.qml
        qml/OpenChat/components/GroupMemberPicker.qml
        qml/OpenChat/components/CallParticipant.qml
        qml/OpenChat/components/CallActionButton.qml
        qml/OpenChat/components/CallHeader.qml
        qml/OpenChat/components/CallStrip.qml
        qml/OpenChat/components/ScreenShareStage.qml
        qml/OpenChat/components/ScreenSharePicker.qml
        qml/OpenChat/components/MediaIconButton.qml
        qml/OpenChat/components/MediaZoomOverlay.qml
        qml/OpenChat/components/MessageActionButton.qml
        qml/OpenChat/components/MessageDelegate.qml
        qml/OpenChat/components/MessageHistory.qml
        qml/OpenChat/components/Composer.qml
        qml/OpenChat/components/DailyCaseButton.qml
        qml/OpenChat/components/DailyCaseModal.qml
        qml/OpenChat/components/CaseReel.qml
        qml/OpenChat/components/CaseTile.qml
        qml/OpenChat/components/CosmeticPreview.qml
        qml/OpenChat/components/CosmeticPicker.qml
        qml/OpenChat/components/ProfilePage.qml
        qml/OpenChat/components/ProfileTopBar.qml
        qml/OpenChat/components/ProfileBackChip.qml
        qml/OpenChat/components/ProfilePageView.qml
        qml/OpenChat/components/ProfilePreviewDecor.qml
        qml/OpenChat/components/ProfileBox.qml
        qml/OpenChat/components/ProfileStrip.qml
        qml/OpenChat/components/ProfileBoxBorder.qml
        qml/OpenChat/components/ProfileIdentityModule.qml
        qml/OpenChat/components/ProfileContactingModule.qml
        qml/OpenChat/components/ProfileActionCell.qml
        qml/OpenChat/components/ProfileHandleModule.qml
        qml/OpenChat/components/ProfileSongModule.qml
        qml/OpenChat/components/ProfileInfoTable.qml
        qml/OpenChat/components/ProfileInterestsModule.qml
        qml/OpenChat/components/ProfileDetailsModule.qml
        qml/OpenChat/components/ProfileBannerModule.qml
        qml/OpenChat/components/ProfileBlurbsModule.qml
        qml/OpenChat/components/ProfileFriendSpaceModule.qml
        qml/OpenChat/components/ProfileFriendTile.qml
        qml/OpenChat/components/ProfileMonogramTile.qml
        qml/OpenChat/components/ProfileNoPageModule.qml
        qml/OpenChat/components/ProfileStubCard.qml
        qml/OpenChat/components/ProfileRequestButton.qml
        qml/OpenChat/components/ProfileConfirmPopup.qml
        qml/OpenChat/components/ProfileAvatarAffordance.qml
        qml/OpenChat/components/ProfileBadge.qml
        qml/OpenChat/components/ProfileGlyph.qml
        qml/OpenChat/components/ProfileTip.qml
        qml/OpenChat/components/ProfileChipButton.qml
        qml/OpenChat/components/ProfileDefaultButton.qml
        qml/OpenChat/components/ProfileToast.qml
        qml/OpenChat/components/ProfileSpinner.qml
        qml/OpenChat/components/ProfileFocusRing.qml
        qml/OpenChat/components/ProfileEditor.qml
        qml/OpenChat/components/ProfileEditorBar.qml
        qml/OpenChat/components/ProfileEditorRail.qml
        qml/OpenChat/components/ProfileEditorPanel.qml
        qml/OpenChat/components/ProfilePreviewFrame.qml
        qml/OpenChat/components/ProfileThemesTab.qml
        qml/OpenChat/components/ProfileBackgroundTab.qml
        qml/OpenChat/components/ProfileBoxesTab.qml
        qml/OpenChat/components/ProfileTextTab.qml
        qml/OpenChat/components/ProfileNameTab.qml
        qml/OpenChat/components/ProfileAboutTab.qml
        qml/OpenChat/components/ProfileFriendsTab.qml
        qml/OpenChat/components/ProfileSongTab.qml
        qml/OpenChat/components/ProfileLayoutTab.qml
        qml/OpenChat/components/ProfileColorWell.qml
        qml/OpenChat/components/ProfileColorPicker.qml
        qml/OpenChat/components/ProfileSegmented.qml
        qml/OpenChat/components/ProfileTextArea.qml
        qml/OpenChat/components/ProfileFieldLabel.qml
        qml/OpenChat/components/ProfileFold.qml
        qml/OpenChat/components/ProfileTileGrid.qml
        qml/OpenChat/components/ProfileWaveform.qml
        qml/OpenChat/components/ProfileReadabilityNotice.qml
        qml/OpenChat/components/ProfileLeaveDialog.qml
        qml/OpenChat/components/ProfileDiscardPopover.qml
    # The logo and the placeholder picture ship as 256 px copies of the
    # full-size artwork beside them: neither is ever drawn larger, and the
    # window icon stays decoded for as long as the process runs.
    RESOURCES
        assets/Jessica.png
        assets/alex.png
        assets/michael.png
        assets/ryan.png
        assets/userpfp_none-256.png
        assets/icons/video-call.svg
        assets/icons/video-call-dark.svg
        assets/icons/phone-call.svg
        assets/icons/phone-call-dark.svg
        assets/icons/settings.svg
        assets/icons/settings-dark.svg
        assets/icons/chevron-down.svg
        assets/icons/chevron-down-dark.svg
        assets/icons/openchat-256.png
        assets/sounds/crate-display.wav
        assets/sounds/crate-item-scroll.wav
        assets/sounds/crate-open.wav
)

target_include_directories(OpenChat PRIVATE src)
target_link_libraries(OpenChat PRIVATE
    openchat_case
    openchat_case_relay
    openchat_cosmetics
    openchat_profile
    openchat_app
    openchat_avatar
    openchat_call
    openchat_crash
    openchat_notify
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
)

if(UNIX AND NOT APPLE)
    # The desktop entry is how a freedesktop desktop learns OpenChat's name and
    # icon. Notifications carry its basename in the "desktop-entry" hint, which
    # is what puts the application's icon on the notification and its name in
    # the notification settings. Nothing at build or run time depends on this
    # being installed; without it the desktop falls back to the plain name.
    include(GNUInstallDirs)
    install(TARGETS OpenChat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES deploy/openchat.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES assets/icons/openchat-256.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME openchat.png)
endif()

# Posts one real desktop notification through the application's own code. Not
# part of the application: it is how the platform backends are checked by hand
# on a desktop session, which no automated test can do.
add_executable(openchat-notify-check tools/notify_check.cpp)
target_include_directories(openchat-notify-check PRIVATE src)
target_link_libraries(openchat-notify-check PRIVATE
    openchat_notify
    openchat_avatar
    Qt6::Core
    Qt6::Gui
)

# Renders the generated call sounds to WAV. Not part of the application: it is
# how the sounds are auditioned and handed over, since they exist as code rather
# than as bundled audio files.
add_executable(openchat-render-call-sounds tools/render_call_sounds.cpp)
target_include_directories(openchat-render-call-sounds PRIVATE src)
target_link_libraries(openchat-render-call-sounds PRIVATE
    openchat_call
    openchat_media
    Qt6::Core
)

# What a screen-share viewer actually sees, tile encoder against VP9, on
# synthetic desktops. Not part of the application: it is how a change to either
# encoder is measured rather than guessed at.
add_executable(openchat-screen-bench tools/screen_share_bench.cpp)
target_include_directories(openchat-screen-bench PRIVATE src)
target_link_libraries(openchat-screen-bench PRIVATE openchat_call Qt6::Core Qt6::Gui)

# Runs one side of a real call against a real relay, so the two-machine question
# can be answered by actually doing it rather than by inference.
add_executable(openchat-call-check tools/call_check.cpp)
target_include_directories(openchat-call-check PRIVATE src)
target_link_libraries(openchat-call-check PRIVATE
    openchat_app
    openchat_call
    openchat_media
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
)

enable_testing()
add_executable(tst_dailycase tests/tst_dailycase.cpp)
target_link_libraries(tst_dailycase PRIVATE openchat_case Qt6::Test)
add_test(NAME tst_dailycase COMMAND tst_dailycase)

add_executable(tst_voiceeffects tests/tst_voiceeffects.cpp src/media/VoiceEffects.cpp)
target_include_directories(tst_voiceeffects PRIVATE src)
set_target_properties(tst_voiceeffects PROPERTIES AUTOMOC OFF)
add_test(NAME tst_voiceeffects COMMAND tst_voiceeffects)

add_executable(tst_appmetadata tests/tst_appmetadata.cpp)
target_include_directories(tst_appmetadata PRIVATE src)
target_link_libraries(tst_appmetadata PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_appmetadata COMMAND tst_appmetadata)

add_library(openchat_domain STATIC
    src/diagnostics/Logging.cpp
    src/diagnostics/BlackBox.cpp
    src/domain/Identifiers.cpp
    src/domain/Handle.cpp
    src/domain/CosmeticRules.cpp
    src/domain/ProfileUpdate.cpp
    src/domain/ProfilePage.cpp
    src/domain/ProfilePageCodec.cpp
    src/domain/SongContainer.cpp
    src/domain/GroupUpdate.cpp
    src/domain/MessageContent.cpp
)
target_include_directories(openchat_domain PUBLIC src)
target_link_libraries(openchat_domain PUBLIC Qt6::Core)

# Profile pictures: the file -> bounded JPEG pipeline a user's chosen picture
# goes through before it is stored or sent, and the in-process registry the
# avatar artwork draws received pictures from. Needs Qt Gui for QImage, which
# is why it is not part of openchat_domain.
add_library(openchat_avatar STATIC
    src/render/ProfileImage.cpp
    src/render/AvatarStore.cpp
    src/render/AvatarPainter.cpp
)
target_include_directories(openchat_avatar PUBLIC src)
target_link_libraries(openchat_avatar PUBLIC openchat_domain Qt6::Core Qt6::Gui)

# Renders every bubble skin through the real MessageDelegate to a contact sheet
# (light and dark PNGs under cosmetics-shots/). Not part of the application: it
# is how the skins are reviewed. Runs headless on the offscreen platform.
add_executable(openchat-cosmetics-gallery
    tools/cosmetics_gallery.cpp
    src/render/BubbleBackground.cpp
    src/app/AppearanceSettings.cpp
    src/app/TextLineSpacing.cpp
)
target_include_directories(openchat-cosmetics-gallery PRIVATE src)
target_compile_definitions(openchat-cosmetics-gallery PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(openchat-cosmetics-gallery PRIVATE
    openchat_cosmetics
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
)

# Desktop notifications. The policy (what is worth announcing, and what it says)
# is platform-neutral and tested on its own; exactly one backend is compiled in,
# chosen by the platform this binary targets. On the freedesktop desktops that
# backend is a D-Bus client, so the same code serves Wayland and X11.
set(openchat_notify_sources
    src/notify/NotificationService.cpp
    src/notify/NotificationBackendFactory.cpp
    # Listed so AUTOMOC processes the QObject headers that have no source file
    # of their own: the abstract backend and the do-nothing one.
    src/notify/NotificationBackend.h
    src/notify/NullNotifier.h
)
if(APPLE)
    list(APPEND openchat_notify_sources
        src/notify/MacNotifier.mm
        src/notify/MacNotifier.h)
elseif(WIN32)
    list(APPEND openchat_notify_sources
        src/notify/WindowsNotifier.cpp
        src/notify/WindowsNotifier.h)
elseif(UNIX)
    list(APPEND openchat_notify_sources
        src/notify/FreedesktopNotifier.cpp
        src/notify/FreedesktopNotifier.h)
endif()

add_library(openchat_notify STATIC ${openchat_notify_sources})
target_include_directories(openchat_notify PUBLIC src)
target_link_libraries(openchat_notify PUBLIC openchat_avatar Qt6::Core Qt6::Gui)
if(APPLE)
    set_source_files_properties(src/notify/MacNotifier.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(openchat_notify PRIVATE
        "-framework Foundation"
        "-framework UserNotifications"
    )
elseif(WIN32)
    # shell32/ole32/propsys register the Start Menu shortcut that gives an
    # unpackaged application the identity Windows attributes its toasts to.
    # shlwapi: propvarutil.h's inline InitPropVariantFromString calls SHStrDupW.
    target_link_libraries(openchat_notify PRIVATE shell32 ole32 oleaut32 propsys shlwapi uuid)
    # C++/WinRT is what draws the sender's picture on a real toast. It ships
    # with the Windows SDK and with recent mingw-w64, but not with every
    # toolchain, so it is detected rather than assumed: the source picks the
    # same branch through __has_include, and this keeps the libraries it needs
    # in step with that choice.
    include(CheckIncludeFileCXX)
    check_include_file_cxx("winrt/Windows.UI.Notifications.h" OPENCHAT_HAVE_CPPWINRT)
    if(OPENCHAT_HAVE_CPPWINRT)
        target_link_libraries(openchat_notify PRIVATE runtimeobject windowsapp)
    else()
        message(STATUS
            "OpenChat: no C++/WinRT; notifications fall back to shell balloons.")
    endif()
elseif(UNIX)
    target_link_libraries(openchat_notify PUBLIC Qt6::DBus)
endif()

# Crash reporting: fatal-signal and structured-exception handlers, reports
# written without allocating, the freeze detector, and the post-mortem of a
# session that died without any handler running. See src/diagnostics/CrashReporter.h.
add_library(openchat_crash STATIC
    src/diagnostics/CrashReporter.cpp
    src/diagnostics/CrashWriter.cpp
)
if(WIN32)
    target_sources(openchat_crash PRIVATE src/diagnostics/CrashHandlerWin.cpp)
    target_link_libraries(openchat_crash PRIVATE psapi)
else()
    target_sources(openchat_crash PRIVATE src/diagnostics/CrashHandlerPosix.cpp)
    target_link_libraries(openchat_crash PRIVATE ${CMAKE_DL_LIBS})
endif()
target_include_directories(openchat_crash PUBLIC src)
target_link_libraries(openchat_crash PUBLIC openchat_domain Qt6::Core Qt6::Gui)
# Which build a report came from. The revision is taken at configure time; the
# link timestamp every Windows report lists per module is the exact match.
find_package(Git QUIET)
set(OPENCHAT_BUILD_ID "unknown revision")
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE openchat_git_describe OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE openchat_git_result)
    if(openchat_git_result EQUAL 0 AND openchat_git_describe)
        set(OPENCHAT_BUILD_ID "${openchat_git_describe}")
    endif()
endif()
set_source_files_properties(src/diagnostics/CrashReporter.cpp PROPERTIES COMPILE_DEFINITIONS
    "OPENCHAT_VERSION=\"${PROJECT_VERSION}\";OPENCHAT_BUILD_ID=\"${OPENCHAT_BUILD_ID}\"")

add_library(openchat_protocol STATIC
    src/protocol/CiphertextEnvelope.cpp
    src/protocol/CanonicalCborCodec.cpp
    src/protocol/UdpMediaFrame.cpp
)
target_include_directories(openchat_protocol PUBLIC src)
target_link_libraries(openchat_protocol PUBLIC openchat_domain Qt6::Core)

add_library(openchat_network STATIC
    src/network/BackoffPolicy.cpp
    src/network/RelayClient.cpp
    src/network/MlsTransactionCoordinator.cpp
    src/network/SyncEngine.cpp
    src/network/MlsSyncSession.cpp
    src/network/RelayTransport.cpp
)
target_include_directories(openchat_network PUBLIC src)
target_link_libraries(openchat_network
    PUBLIC openchat_protocol openchat_domain Qt6::Core Qt6::Network Qt6::WebSockets
)

# The relay as the authority for cases and cosmetics, for the signed-in app.
# Kept out of openchat_case so the case and QML tests need no network stack.
add_library(openchat_case_relay STATIC
    src/case/RelayCaseService.cpp
)
target_include_directories(openchat_case_relay PUBLIC src)
target_link_libraries(openchat_case_relay PUBLIC openchat_case openchat_network Qt6::Core)

add_library(openchat_crypto STATIC
    src/crypto/MlsClient.cpp
)
target_include_directories(openchat_crypto PUBLIC src)
target_link_libraries(openchat_crypto
    PUBLIC openchat_domain Qt6::Core
    PRIVATE openchat_mls_rust
)

# Format-agnostic audio: file decoding, rate/channel conditioning, the codec
# surface (PCM and Opus), the adaptive jitter buffer, and the level meter.
# Deliberately free of Qt Multimedia and of any call concept, so it is testable
# with nothing but a file on disk.
add_library(openchat_media STATIC
    src/media/ToneSynth.cpp
    src/media/WavFile.cpp
    src/media/AudioConvert.cpp
    src/media/AudioCodec.cpp
    src/media/OpusAudioCodec.cpp
    src/media/JitterBuffer.cpp
    src/media/SpeechLevelMeter.cpp
    src/media/MicrophoneProcessor.cpp
    src/media/VoiceEffects.cpp
    src/media/SongCodec.cpp
)
target_include_directories(openchat_media PUBLIC src)
target_link_libraries(openchat_media
    PUBLIC openchat_domain Qt6::Core
    PRIVATE openchat_opus
)

# Third-party audio plugin hosting for voice effects: the two plugin ABIs, the
# trust layer that gates loading one, and the chain a call runs them in.
#
# Its own library, and separate from openchat_media specifically BECAUSE of what
# it does. openchat_media is deliberately free of anything but arithmetic on
# samples; this vendors third-party ABI headers and calls dlopen on files the
# application did not build. Keeping that in one target with one set of
# dependencies is what makes "which code can execute a plugin" a question with a
# short answer.
#
# Links openchat_domain rather than openchat_media even though it is audio code:
# everything it needs from there is the header-only CallAudioFormat in
# src/media/AudioTypes.h, and openchat_domain already puts src/ on the include
# path. Depending on openchat_media would drag in Opus, and with it a network
# fetch at configure time on machines without a system libopus -- for a header.
add_library(openchat_effects STATIC
    src/effects/AudioPluginTypes.cpp
    src/effects/AudioPluginTrust.cpp
    src/effects/ClapHostContext.cpp
    src/effects/ClapModule.cpp
    src/effects/ClapEffect.cpp
    src/effects/Vst3InterfaceIds.cpp
    src/effects/Vst3Support.cpp
    src/effects/Vst3Module.cpp
    src/effects/Vst3Effect.cpp
    src/effects/PluginScanner.cpp
    src/effects/VoiceEffectChain.cpp
    src/effects/LocalVoiceEffect.cpp
)
target_include_directories(openchat_effects PUBLIC src)
target_link_libraries(openchat_effects
    PUBLIC openchat_domain Qt6::Core
    PRIVATE openchat_clap openchat_vst3 ${CMAKE_DL_LIBS}
)

add_library(openchat_security STATIC
    src/security/SecureBuffer.cpp
    src/security/QtKeychainVault.cpp
    src/security/DeviceIdentity.cpp
    src/security/RecoveryCode.cpp
    src/security/PasswordKey.cpp
    src/security/SafetyNumber.cpp
)

add_library(openchat_storage STATIC
    src/storage/SqlCipherDatabase.cpp
    src/storage/SqlCipherChatRepository.cpp
    src/storage/SqlCipherContactRepository.cpp
    src/storage/SqlCipherOutboxRepository.cpp
    src/storage/SqlCipherSyncRepository.cpp
    src/storage/SqlCipherSyncStore.cpp
    src/storage/SqlCipherProfilePageRepository.cpp
    src/storage/CapturingMlsStateStore.cpp
)
target_include_directories(openchat_storage PUBLIC src)
target_link_libraries(openchat_storage
    PUBLIC openchat_security Qt6::Core
    PRIVATE OpenSSL::Crypto openchat_sqlcipher
)
qt_add_resources(openchat_storage "openchat_migrations"
    PREFIX "/openchat"
    BASE "src/storage/migrations"
    FILES
        src/storage/migrations/001_initial.sql
        src/storage/migrations/002_indexes.sql
        src/storage/migrations/003_domain_alignment.sql
        src/storage/migrations/004_profile_identity.sql
        src/storage/migrations/005_outbox_control.sql
        src/storage/migrations/006_account_identity.sql
        src/storage/migrations/007_contacts.sql
        src/storage/migrations/008_pending_handshakes.sql
        src/storage/migrations/009_contact_verification.sql
        src/storage/migrations/010_profile_display_name.sql
        src/storage/migrations/011_contact_device.sql
        src/storage/migrations/012_profiles.sql
        src/storage/migrations/013_group_chats.sql
        src/storage/migrations/014_chat_read_state.sql
        src/storage/migrations/015_message_edits_and_replies.sql
        src/storage/migrations/016_profile_pages.sql
)
target_include_directories(openchat_security PUBLIC src)
target_link_libraries(openchat_security
    PUBLIC openchat_domain Qt6::Core
    PRIVATE OpenSSL::Crypto qt6keychain
)

# The voice call itself: signalling codec, per-call media keys, the media
# pipeline, the state machine, and the Qt Multimedia device adapters. Everything
# except QtAudioIo is device- and network-free.
add_library(openchat_call STATIC
    src/call/CallTypes.cpp
    src/call/CallSounds.cpp
    src/call/CallSignal.cpp
    src/call/CallMediaCrypto.cpp
    src/call/CallMediaPacket.cpp
    src/call/CallSession.cpp
    src/call/CallEngine.cpp
    src/call/SyncCallTransport.cpp
    src/call/QtAudioIo.cpp
    src/call/CallVideoSession.cpp
    src/call/CallScreenSession.cpp
    src/call/ScreenVideoCodec.cpp
    src/call/ScreenAudio.cpp
    src/call/ScreenAudioCapture.cpp
    src/call/QtVideoCapture.cpp
    src/call/QtScreenCapture.cpp
    src/call/VideoFrameCopy.cpp
    src/call/UdpCallMediaPath.cpp
)
target_include_directories(openchat_call PUBLIC src)
target_link_libraries(openchat_call
    PUBLIC openchat_media openchat_network openchat_domain openchat_effects
           Qt6::Core Qt6::Gui Qt6::Multimedia
    PRIVATE OpenSSL::Crypto openchat_vpx openchat_opus
)

# A screen share's sound: what the computer plays, minus OpenChat's own
# output. See src/call/ScreenAudioCapture.h. Windows uses WASAPI process
# loopback; Linux the PulseAudio client library (PipeWire serves it too);
# anything else shares screens without sound.
if(WIN32)
    target_sources(openchat_call PRIVATE src/call/ScreenAudioCaptureWin.cpp)
    target_link_libraries(openchat_call PRIVATE ole32 uuid)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(OPENCHAT_PULSE_PC QUIET IMPORTED_TARGET libpulse)
    endif()
    if(OPENCHAT_PULSE_PC_FOUND AND NOT APPLE)
        target_sources(openchat_call PRIVATE src/call/ScreenAudioCapturePulse.cpp)
        target_link_libraries(openchat_call PRIVATE PkgConfig::OPENCHAT_PULSE_PC)
        message(STATUS "OpenChat: screen shares carry sound through libpulse ${OPENCHAT_PULSE_PC_VERSION}")
    else()
        target_sources(openchat_call PRIVATE src/call/ScreenAudioCaptureNone.cpp)
        if(NOT APPLE)
            message(WARNING "OpenChat: libpulse not found; screen shares will have no sound. "
                            "Install the PulseAudio client library (pkg-config 'libpulse').")
        endif()
    endif()
endif()

# Screen capture through the operating system's own API where Qt's cannot be
# relied on: the Windows Qt ships no FFmpeg backend (so no QScreenCapture at
# all) and Homebrew's macOS Qt switches it off. See src/call/NativeScreenCapture.h.
# Turning this off falls back to Qt Multimedia everywhere, which is how Linux
# always captures.
option(OPENCHAT_NATIVE_SCREEN_CAPTURE
    "Capture screens with DXGI/Windows.Graphics.Capture and ScreenCaptureKit" ON)
if(OPENCHAT_NATIVE_SCREEN_CAPTURE AND WIN32)
    target_sources(openchat_call PRIVATE src/call/NativeScreenCaptureWin.cpp)
    target_link_libraries(openchat_call PRIVATE d3d11 dxgi dwmapi gdi32 user32)
    # Windows.Graphics.Capture, for single windows, is reached through
    # C++/WinRT; without it only whole screens can be shared.
    include(CheckIncludeFileCXX)
    check_include_file_cxx("winrt/Windows.Graphics.Capture.h" OPENCHAT_HAVE_CPPWINRT_CAPTURE)
    if(OPENCHAT_HAVE_CPPWINRT_CAPTURE)
        target_link_libraries(openchat_call PRIVATE runtimeobject windowsapp)
    else()
        message(STATUS "OpenChat: no C++/WinRT; single windows cannot be screen-shared.")
    endif()
elseif(OPENCHAT_NATIVE_SCREEN_CAPTURE AND APPLE)
    target_sources(openchat_call PRIVATE src/call/NativeScreenCaptureMac.mm)
    set_source_files_properties(src/call/NativeScreenCaptureMac.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    # Weak, so the application still starts on macOS older than 12.3; the code
    # checks @available before touching ScreenCaptureKit and uses Qt's path
    # there instead.
    target_link_libraries(openchat_call PRIVATE
        "-weak_framework ScreenCaptureKit"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        "-framework AppKit")
else()
    target_sources(openchat_call PRIVATE src/call/NativeScreenCaptureNone.cpp)
endif()
qt_add_resources(openchat_call "openchat_call_sounds"
    PREFIX "/openchat/sounds"
    BASE "assets/sounds"
    FILES
        assets/sounds/ringback.wav
        assets/sounds/incoming-ring.wav
        assets/sounds/connected.wav
        assets/sounds/ended.wav
        assets/sounds/muted.wav
        assets/sounds/unmuted.wav
)

add_library(openchat_app STATIC
    src/app/ProfileSession.cpp
    src/app/AccountBootstrap.cpp
    src/app/LocalDataReset.cpp
    src/app/AddContactService.cpp
    src/app/ContactRequestService.cpp
    src/app/GroupService.cpp
    src/app/DeviceLink.cpp
    src/app/KeyPackageSupply.cpp
    src/app/ProfilePageSync.cpp
)
target_include_directories(openchat_app PUBLIC src)
target_link_libraries(openchat_app
    PUBLIC openchat_network openchat_crypto openchat_storage openchat_security Qt6::Core
)

add_executable(tst_domain tests/tst_domain.cpp)
target_include_directories(tst_domain PRIVATE src)
target_link_libraries(tst_domain PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_domain COMMAND tst_domain)

# The profile page model, its wire codec and the song container.
add_executable(tst_profilepage tests/tst_profilepage.cpp)
target_include_directories(tst_profilepage PRIVATE src)
target_link_libraries(tst_profilepage PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_profilepage COMMAND tst_profilepage)

add_executable(tst_groupupdate tests/tst_groupupdate.cpp)
target_include_directories(tst_groupupdate PRIVATE src)
target_link_libraries(tst_groupupdate PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_groupupdate COMMAND tst_groupupdate)

add_executable(tst_messagecontent tests/tst_messagecontent.cpp)
target_include_directories(tst_messagecontent PRIVATE src)
target_link_libraries(tst_messagecontent PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_messagecontent COMMAND tst_messagecontent)

add_executable(tst_keyvault tests/tst_keyvault.cpp)
target_include_directories(tst_keyvault PRIVATE src tests)
target_link_libraries(tst_keyvault PRIVATE openchat_security Qt6::Core Qt6::Test)
add_test(NAME tst_keyvault COMMAND tst_keyvault)

add_executable(tst_safetynumber tests/tst_safetynumber.cpp)
target_include_directories(tst_safetynumber PRIVATE src)
target_link_libraries(tst_safetynumber PRIVATE openchat_security Qt6::Core Qt6::Test)
add_test(NAME tst_safetynumber COMMAND tst_safetynumber)

add_executable(tst_sqlcipherstorage tests/tst_sqlcipherstorage.cpp)
target_include_directories(tst_sqlcipherstorage PRIVATE src)
target_link_libraries(tst_sqlcipherstorage PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_sqlcipherstorage COMMAND tst_sqlcipherstorage)

add_executable(tst_repositories tests/tst_repositories.cpp)
target_include_directories(tst_repositories PRIVATE src)
target_link_libraries(tst_repositories PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_repositories COMMAND tst_repositories)

add_executable(tst_syncstore tests/tst_syncstore.cpp)
target_include_directories(tst_syncstore PRIVATE src)
target_link_libraries(tst_syncstore PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_syncstore COMMAND tst_syncstore)

# Settled control envelopes leave the outbox (SqlCipherSyncStore), checked on
# disk through a second connection, and through a real SyncEngine.
add_executable(tst_outboxretention tests/tst_outboxretention.cpp)
target_include_directories(tst_outboxretention PRIVATE src)
target_link_libraries(tst_outboxretention PRIVATE
    openchat_storage openchat_network openchat_protocol openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_outboxretention COMMAND tst_outboxretention)

# Migration 016 and the profile page repository. openchat_sqlcipher is named so
# the test compiles against SQLCipher's own sqlite3.h (it opens raw connections
# to build old schemas and to write rows that bypass the repository).
add_executable(tst_profilepagestore tests/tst_profilepagestore.cpp)
target_include_directories(tst_profilepagestore PRIVATE src)
target_link_libraries(tst_profilepagestore
    PRIVATE openchat_app openchat_storage openchat_sqlcipher Qt6::Core Qt6::Test
)
add_test(NAME tst_profilepagestore COMMAND tst_profilepagestore)

add_executable(tst_contactrepository tests/tst_contactrepository.cpp)
target_include_directories(tst_contactrepository PRIVATE src)
target_link_libraries(tst_contactrepository PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_contactrepository COMMAND tst_contactrepository)

add_executable(tst_envelopecodec tests/tst_envelopecodec.cpp)
target_include_directories(tst_envelopecodec PRIVATE src)
target_compile_definitions(tst_envelopecodec PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_envelopecodec PRIVATE openchat_protocol Qt6::Core Qt6::Test)
add_test(NAME tst_envelopecodec COMMAND tst_envelopecodec)

add_executable(tst_udpmediaframe tests/tst_udpmediaframe.cpp)
target_include_directories(tst_udpmediaframe PRIVATE src)
target_link_libraries(tst_udpmediaframe PRIVATE openchat_protocol Qt6::Core Qt6::Test)
add_test(NAME tst_udpmediaframe COMMAND tst_udpmediaframe)

add_executable(tst_mlsbridge tests/tst_mlsbridge.cpp)
target_include_directories(tst_mlsbridge PRIVATE src)
target_link_libraries(tst_mlsbridge PRIVATE openchat_crypto Qt6::Core Qt6::Test)
add_test(NAME tst_mlsbridge COMMAND tst_mlsbridge)

add_subdirectory(relay)

add_executable(tst_relayclient
    tests/tst_relayclient.cpp
    tests/relay/RelayTestSupport.cpp
)
target_include_directories(tst_relayclient PRIVATE src tests relay/src)
target_link_libraries(tst_relayclient PRIVATE
    openchat_network
    openchat_protocol
    openchat_domain
    openchat_security
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_relayclient COMMAND tst_relayclient)
set_tests_properties(tst_relayclient PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_udpmediaservice tests/tst_udpmediaservice.cpp)
target_include_directories(tst_udpmediaservice PRIVATE src tests relay/src)
target_link_libraries(tst_udpmediaservice PRIVATE
    openchat_relay_server
    openchat_relay_core
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_udpmediaservice COMMAND tst_udpmediaservice)

add_executable(tst_deviceidentity tests/tst_deviceidentity.cpp)
target_include_directories(tst_deviceidentity PRIVATE src relay/src)
target_link_libraries(tst_deviceidentity PRIVATE
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_core
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_deviceidentity COMMAND tst_deviceidentity)

add_executable(tst_mlscoordinator tests/tst_mlscoordinator.cpp)
target_include_directories(tst_mlscoordinator PRIVATE src)
target_link_libraries(tst_mlscoordinator PRIVATE openchat_network openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_mlscoordinator COMMAND tst_mlscoordinator)

add_executable(tst_syncengine tests/tst_syncengine.cpp)
target_include_directories(tst_syncengine PRIVATE src)
target_link_libraries(tst_syncengine PRIVATE
    openchat_network openchat_protocol openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_syncengine COMMAND tst_syncengine)

# Profile pages between real peers (ProfileSession, SyncEngine, MLS group)
# trading through scripted transports: publishing, the paced delivery pump,
# receiving and background requests. openchat_sqlcipher is named so the outbox
# inspector compiles against SQLCipher's own sqlite3.h, not a system one.
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

add_executable(tst_syncadapters tests/tst_syncadapters.cpp)
target_include_directories(tst_syncadapters PRIVATE src)
target_link_libraries(tst_syncadapters PRIVATE
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test)
add_test(NAME tst_syncadapters COMMAND tst_syncadapters)
set_tests_properties(tst_syncadapters PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_profilesession tests/tst_profilesession.cpp)
target_include_directories(tst_profilesession PRIVATE src)
target_link_libraries(tst_profilesession PRIVATE openchat_app Qt6::Core Qt6::Test)
add_test(NAME tst_profilesession COMMAND tst_profilesession)

# Erasing a profile from before usernames and passwords (and everything cached
# beside it), and telling such a profile apart without unlocking it.
add_executable(tst_localdatareset tests/tst_localdatareset.cpp)
target_include_directories(tst_localdatareset PRIVATE src)
target_link_libraries(tst_localdatareset PRIVATE openchat_app Qt6::Core Qt6::Test)
add_test(NAME tst_localdatareset COMMAND tst_localdatareset)

# The canonical form of a username, and the local password stretch whose output
# is the only thing the relay ever receives (pinned by a known-answer vector).
add_executable(tst_accountcredentials tests/tst_accountcredentials.cpp)
target_include_directories(tst_accountcredentials PRIVATE src)
target_link_libraries(tst_accountcredentials PRIVATE openchat_security openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_accountcredentials COMMAND tst_accountcredentials)

# A device the relay refuses (retired by a login elsewhere, or unknown) must be
# reported and re-checked slowly, never retried in a tight loop.
add_executable(tst_devicelink
    tests/tst_devicelink.cpp
    tests/relay/RelayTestSupport.cpp
)
target_include_directories(tst_devicelink PRIVATE src tests relay/src)
target_link_libraries(tst_devicelink PRIVATE
    openchat_app
    openchat_network
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_devicelink COMMAND tst_devicelink)

# Live end-to-end account bootstrap against the Compose relay stack over TLS.
# Labelled "integration": it QSKIPs cleanly when the stack is not reachable, so a
# normal ctest run is unaffected; bring the stack up to make it actually run.
add_executable(tst_accountbootstrap tests/tst_accountbootstrap.cpp)
target_include_directories(tst_accountbootstrap PRIVATE src)
target_compile_definitions(tst_accountbootstrap PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_accountbootstrap PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_accountbootstrap COMMAND tst_accountbootstrap)
set_tests_properties(tst_accountbootstrap PROPERTIES
    LABELS integration
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Real-wiring integration test for the add-contact handshake SEND path: a live
# ProfileSession (SQLCipher + real MlsClient) plus a second MlsClient standing in
# for the peer, driven through AddContactService by emitting the relay's signals.
add_executable(tst_addcontactservice tests/tst_addcontactservice.cpp
    tests/relay/RelayTestSupport.cpp)
target_include_directories(tst_addcontactservice PRIVATE src tests relay/src)
target_link_libraries(tst_addcontactservice PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_addcontactservice COMMAND tst_addcontactservice)
set_tests_properties(tst_addcontactservice PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Real-wiring integration test for the add-contact handshake RECEIVE path: a live
# ProfileSession (SQLCipher + real MlsClient) plus a second MlsClient standing in
# for the sender, driving ContactRequestService's accept/decline/block/reconcile
# through the SyncEngine over a fake ciphertext transport.
add_executable(tst_contactrequestservice tests/tst_contactrequestservice.cpp)
target_include_directories(tst_contactrequestservice PRIVATE src tests relay/src)
target_link_libraries(tst_contactrequestservice PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_contactrequestservice COMMAND tst_contactrequestservice)
set_tests_properties(tst_contactrequestservice PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Two-client end-to-end integration test over REAL TLS + REAL in-process relay +
# REAL PostgreSQL. Two bootstrapped ProfileSessions exchange a contact handshake
# and messages through an in-process TLS-terminating proxy in front of a live
# RelayServer. Labelled "integration": it QSKIPs cleanly when Postgres is not
# reachable, so a normal ctest run is unaffected.
add_executable(tst_e2e
    tests/tst_e2e.cpp
    tests/relay/RelayTestSupport.cpp
    src/controllers/ChatController.cpp
    ${profile_controller_sources}
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
)
target_include_directories(tst_e2e PRIVATE src tests relay/src)
target_link_libraries(tst_e2e PRIVATE
    openchat_case_relay
    openchat_cosmetics
    openchat_profile
    openchat_app
    openchat_avatar
    openchat_call
    openchat_media
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_server
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_e2e COMMAND tst_e2e)
set_tests_properties(tst_e2e PROPERTIES
    LABELS integration
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OPENCHAT_TEST_PG_PORT=55432;OPENCHAT_TEST_PG_HOST=127.0.0.1;OPENCHAT_TEST_PG_USER=ocrelay;OPENCHAT_TEST_PG_ADMINDB=postgres"
)

add_executable(tst_contactcontroller
    tests/tst_contactcontroller.cpp
    src/controllers/ContactController.cpp
    src/models/RequestListModel.cpp
)
target_include_directories(tst_contactcontroller PRIVATE src tests relay/src)
target_link_libraries(tst_contactcontroller PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_contactcontroller COMMAND tst_contactcontroller)
set_tests_properties(tst_contactcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The notification policy, checked against a recording backend so no desktop
# notification service is needed to run it.
add_executable(tst_notifications tests/tst_notifications.cpp)
target_include_directories(tst_notifications PRIVATE src)
target_link_libraries(tst_notifications PRIVATE
    openchat_notify
    openchat_avatar
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME tst_notifications COMMAND tst_notifications)
set_tests_properties(tst_notifications PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(UNIX AND NOT APPLE)
    # The freedesktop backend against a notification daemon of the test's own,
    # on a private session bus. Skips itself where dbus-daemon is absent.
    add_executable(tst_freedesktopnotifier tests/tst_freedesktopnotifier.cpp)
    target_include_directories(tst_freedesktopnotifier PRIVATE src)
    target_link_libraries(tst_freedesktopnotifier PRIVATE
        openchat_notify
        openchat_avatar
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Test
    )
    add_test(NAME tst_freedesktopnotifier COMMAND tst_freedesktopnotifier)
    set_tests_properties(tst_freedesktopnotifier PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(tst_models
    tests/tst_models.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
)
target_include_directories(tst_models PRIVATE src)
target_link_libraries(tst_models PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME tst_models COMMAND tst_models)

# Also the Appearance › Profiles choice (plainProfiles) beside the settings list
# that shows it, and that Low memory mode reaches the profile renderer.
add_executable(tst_chatcontroller
    tests/tst_chatcontroller.cpp
    src/controllers/ChatController.cpp
    ${profile_controller_sources}
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/app/AppearanceSettings.cpp
    src/app/MemorySettings.cpp
)
target_include_directories(tst_chatcontroller PRIVATE src tests)
target_link_libraries(tst_chatcontroller PRIVATE
    openchat_cosmetics
    openchat_profile
    openchat_call
    openchat_avatar
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_chatcontroller COMMAND tst_chatcontroller)
set_tests_properties(tst_chatcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_onboardingcontroller
    tests/tst_onboardingcontroller.cpp
    src/controllers/OnboardingController.cpp
)
target_include_directories(tst_onboardingcontroller PRIVATE src)
target_link_libraries(tst_onboardingcontroller PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_onboardingcontroller COMMAND tst_onboardingcontroller)

add_executable(tst_videocall tests/tst_videocall.cpp)
target_link_libraries(tst_videocall PRIVATE openchat_call Qt6::Test)
add_test(NAME tst_videocall COMMAND tst_videocall)

add_executable(tst_crashreport tests/tst_crashreport.cpp)
target_include_directories(tst_crashreport PRIVATE src)
target_compile_definitions(tst_crashreport PRIVATE
    OPENCHAT_EXECUTABLE="$<TARGET_FILE:OpenChat>")
target_link_libraries(tst_crashreport PRIVATE openchat_crash openchat_domain Qt6::Core Qt6::Test)
# The end-to-end cases crash the real application on purpose.
add_dependencies(tst_crashreport OpenChat)
add_test(NAME tst_crashreport COMMAND tst_crashreport)

add_executable(tst_screenvideo tests/tst_screenvideo.cpp)
target_link_libraries(tst_screenvideo PRIVATE openchat_call Qt6::Gui Qt6::Test)
add_test(NAME tst_screenvideo COMMAND tst_screenvideo)

add_executable(tst_screenaudio tests/tst_screenaudio.cpp)
target_link_libraries(tst_screenaudio PRIVATE openchat_call Qt6::Test)
if(OPENCHAT_PULSE_PC_FOUND AND NOT APPLE AND NOT WIN32)
    pkg_check_modules(OPENCHAT_PULSE_SIMPLE_PC QUIET IMPORTED_TARGET libpulse-simple)
    if(OPENCHAT_PULSE_SIMPLE_PC_FOUND)
        target_compile_definitions(tst_screenaudio PRIVATE OPENCHAT_TEST_PULSE=1)
        target_link_libraries(tst_screenaudio PRIVATE PkgConfig::OPENCHAT_PULSE_SIMPLE_PC
                                                      PkgConfig::OPENCHAT_PULSE_PC)
    endif()
endif()
add_test(NAME tst_screenaudio COMMAND tst_screenaudio)

add_executable(tst_screenshare tests/tst_screenshare.cpp)
target_link_libraries(tst_screenshare PRIVATE openchat_call Qt6::Gui Qt6::Test)
add_test(NAME tst_screenshare COMMAND tst_screenshare)

# The voice-call test: pushes a real audio file (and synthesised references)
# through the whole pipeline and checks what comes out the far end, plus the
# malformed-input, loss, reordering, duplication, replay and tampering edges
# that a real link produces. Runs headless -- no audio device is opened.
add_executable(tst_voicecall tests/tst_voicecall.cpp)
target_include_directories(tst_voicecall PRIVATE src tests)
# So the test can check the WAVs in assets/sounds still match what
# CallSoundBoard actually loads.
target_compile_definitions(tst_voicecall PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_voicecall PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_voicecall COMMAND tst_voicecall)

# The call state machine over a scripted transport and scripted audio devices:
# offers, answers, declines, hangups, timeouts, glare and busy handling.
add_executable(tst_callengine tests/tst_callengine.cpp)
target_include_directories(tst_callengine PRIVATE src tests)
target_link_libraries(tst_callengine PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_callengine COMMAND tst_callengine)

# Real 1:1 call verifying UDP Active state, audio roundtrips, and transparent fallback to WS
add_executable(tst_calludp
    tests/tst_calludp.cpp
    src/app/TransportSettings.cpp
)
target_include_directories(tst_calludp PRIVATE src tests relay/src)
target_link_libraries(tst_calludp PRIVATE
    openchat_call
    openchat_media
    openchat_network
    openchat_protocol
    openchat_domain
    openchat_relay_server
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_calludp COMMAND tst_calludp)

# The seam the other UDP tests leave open: a real RelayClient asking for a media
# token over its live socket, a real UdpMediaService minting and binding it, and
# a real UdpCallMediaPath reaching Active with nothing hand-injected.
add_executable(tst_udpmediae2e
    tests/tst_udpmediae2e.cpp
    tests/relay/RelayTestSupport.cpp
    src/app/TransportSettings.cpp
)
target_include_directories(tst_udpmediae2e PRIVATE src tests relay/src)
target_link_libraries(tst_udpmediae2e PRIVATE
    openchat_call
    openchat_media
    openchat_network
    openchat_protocol
    openchat_domain
    openchat_security
    openchat_relay_server
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_udpmediae2e COMMAND tst_udpmediae2e)
set_tests_properties(tst_udpmediae2e PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Group calls over a mesh of scripted endpoints: everyone rings, joins one by
# one, hears the others mixed, leaves without ending it for the rest, and the
# decline/busy/timeout/glare edges.
add_executable(tst_groupcall tests/tst_groupcall.cpp)
target_include_directories(tst_groupcall PRIVATE src tests)
target_link_libraries(tst_groupcall PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_groupcall COMMAND tst_groupcall)

# The profile-update codec, the picture pipeline and the avatar registry:
# round trips, every rejected shape, and the size/dimension bounds a chosen
# file must respect before it is decoded in full.
add_executable(tst_profile tests/tst_profile.cpp)
target_include_directories(tst_profile PRIVATE src)
target_link_libraries(tst_profile PRIVATE openchat_avatar openchat_domain Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME tst_profile COMMAND tst_profile)
set_tests_properties(tst_profile PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_bubblebackground
    tests/tst_bubblebackground.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
)
target_include_directories(tst_bubblebackground PRIVATE src)
target_link_libraries(tst_bubblebackground PRIVATE openchat_avatar openchat_cosmetics Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_test(NAME tst_bubblebackground COMMAND tst_bubblebackground)

add_executable(tst_cosmetics tests/tst_cosmetics.cpp)
target_link_libraries(tst_cosmetics PRIVATE openchat_cosmetics Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_test(NAME tst_cosmetics COMMAND tst_cosmetics)
set_tests_properties(tst_cosmetics PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The profile renderer: motifs, backdrop, picture layer, media store, the
# background pipeline, the contrast engine against the measured preset table,
# the name art, ambient, ticker, fonts, thumbnails and QML registration.
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

# The profile song: codec, import and the page's player. openchat_opus is named
# so the test can build a packet libopus itself refuses (the premise of the
# concealment test). Playback runs against a fake output, so the suite needs no
# sound card and makes no sound.
add_executable(tst_profilesong tests/tst_profilesong.cpp)
target_include_directories(tst_profilesong PRIVATE src)
target_link_libraries(tst_profilesong PRIVATE
    openchat_profile openchat_media openchat_domain openchat_opus Qt6::Multimedia Qt6::Test)
add_test(NAME tst_profilesong COMMAND tst_profilesong)
set_tests_properties(tst_profilesong PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_qmlload
    tests/tst_qmlload.cpp
    tests/ProfileQmlHarness.h
    src/controllers/ChatController.cpp
    ${profile_controller_sources}
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/CallController.cpp
    src/controllers/VoiceDebugController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
    src/render/CallVideoItem.cpp
    src/render/TrayOrb.cpp
    src/app/AppearanceSettings.cpp
    src/app/CloseToTray.cpp
    src/app/TrayIcon.cpp
    src/app/MemorySettings.cpp
    src/app/MicrophoneSettings.cpp
    src/app/VoiceEffectHost.cpp
    src/app/ComposerEditing.cpp
    src/app/TextLineSpacing.cpp
    src/controllers/CrashReportController.cpp
    src/app/TransportSettings.cpp
)
target_include_directories(tst_qmlload PRIVATE src)
target_compile_definitions(tst_qmlload PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_qmlload PRIVATE
    openchat_case
    openchat_cosmetics
    openchat_profile
    openchat_app
    openchat_avatar
    openchat_call
    openchat_crash
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_qmlload COMMAND tst_qmlload)
set_tests_properties(tst_qmlload PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)

# What a suite built on tests/ProfileQmlHarness.h compiles and links: the
# controllers and models the chat window's QML binds to, the native types the
# harness registers (as tst_qmlload's list does) and the profile controller.
set(profile_qml_test_sources
    tests/ProfileQmlHarness.h
    src/controllers/ChatController.cpp
    src/controllers/CallController.cpp
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/VoiceDebugController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/AvatarArtwork.cpp
    src/render/BubbleBackground.cpp
    src/render/CallVideoItem.cpp
    src/app/AppearanceSettings.cpp
    src/app/MemorySettings.cpp
    src/app/MicrophoneSettings.cpp
    src/app/VoiceEffectHost.cpp
    src/app/ComposerEditing.cpp
    src/app/TextLineSpacing.cpp
    src/app/TransportSettings.cpp
    ${profile_controller_sources}
)
set(profile_qml_test_libs
    openchat_profile
    openchat_case
    openchat_cosmetics
    openchat_app
    openchat_avatar
    openchat_call
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)

# The profile controller: navigation, resolution, page objects, editing, undo,
# imports and catalogues on the reference mock, and publishing, receiving,
# drafts and call deferral over two real peers (tests/PageSyncTestSupport.h).
# openchat_sqlcipher is named so the peers' storage headers compile against
# SQLCipher's own sqlite3.h.
add_executable(tst_profilecontroller
    tests/tst_profilecontroller.cpp
    tests/PageSyncTestSupport.h
    ${profile_qml_test_sources}
)
target_include_directories(tst_profilecontroller PRIVATE src tests)
target_compile_definitions(tst_profilecontroller PRIVATE OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_profilecontroller PRIVATE ${profile_qml_test_libs} openchat_sqlcipher)
add_test(NAME tst_profilecontroller COMMAND tst_profilecontroller)
set_tests_properties(tst_profilecontroller PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# The profile page kit (qml/OpenChat/components/Profile*.qml), hosted as Main
# hosts it by tests/qml/ProfilePageHarness.qml over the reference mock: the
# page geometry, the painted-area budget, plain text, the Contacting actions
# per relationship and call state, the page's one song player, the call strip,
# the back stack and its history menu, stubs, the Plain style switch, the
# animation policy, borders and focus rings, every warning failing the test.
# Copy Invite Link runs against a real RelayClient and the fake HTTPS relay
# (tests/relay/RelayTestSupport), so it copies what the relay really minted.
# OPENCHAT_PROFILE_CAPTURES=<dir> also writes the review PNGs.
add_executable(tst_profilepageqml
    tests/tst_profilepageqml.cpp
    tests/relay/RelayTestSupport.cpp
    ${profile_qml_test_sources}
)
target_include_directories(tst_profilepageqml PRIVATE src tests)
target_compile_definitions(tst_profilepageqml PRIVATE OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_profilepageqml PRIVATE ${profile_qml_test_libs} OpenSSL::Crypto)
add_test(NAME tst_profilepageqml COMMAND tst_profilepageqml)
set_tests_properties(tst_profilepageqml PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# The owner's editor (qml/OpenChat/components/Profile{Editor,EditorBar,...}.qml)
# hosted by tests/qml/ProfileEditorHarness.qml in the real ProfilePage over
# the reference mock: the rail, the nine tabs and their controls, the colour
# picker, Save / Discard / undo and the leave dialog.
add_executable(tst_profileeditor
    tests/tst_profileeditor.cpp
    ${profile_qml_test_sources}
)
target_include_directories(tst_profileeditor PRIVATE src tests)
target_compile_definitions(tst_profileeditor PRIVATE OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(tst_profileeditor PRIVATE ${profile_qml_test_libs})
add_test(NAME tst_profileeditor COMMAND tst_profileeditor)
set_tests_properties(tst_profileeditor PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

# Renders every profile cosmetic through the real QML components into labelled
# PNG sheets (light and dark, 2x) under cosmetics-shots/. A design tool run by
# hand, not part of the application; it loads the QML from the source tree.
add_executable(openchat-profile-gallery
    tools/profile_gallery/main.cpp
    src/controllers/ChatController.cpp
    ${profile_controller_sources}
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
    src/render/CallVideoItem.cpp
    src/app/AppearanceSettings.cpp
    src/app/MicrophoneSettings.cpp
    src/app/VoiceEffectHost.cpp
    src/app/ComposerEditing.cpp
    src/app/TextLineSpacing.cpp
    src/app/TransportSettings.cpp
)
# The bundled contact photos, at the paths the avatar painter looks them up,
# so frames are judged against real faces.
qt_add_resources(openchat-profile-gallery gallery_photos
    PREFIX /qt/qml/OpenChat
    FILES
        assets/Jessica.png
        assets/alex.png
        assets/michael.png
        assets/ryan.png
        assets/userpfp_none-256.png
)
target_include_directories(openchat-profile-gallery PRIVATE src)
target_compile_definitions(openchat-profile-gallery PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(openchat-profile-gallery PRIVATE
    openchat_case
    openchat_cosmetics
    openchat_profile
    openchat_app
    openchat_avatar
    openchat_call
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
)

function(add_openchat_capture_test test_name capture_width capture_height)
    add_test(
        NAME ${test_name}
        COMMAND ${CMAKE_COMMAND}
            -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
            -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${test_name}.png
            -DCAPTURE_WIDTH=${capture_width}
            -DCAPTURE_HEIGHT=${capture_height}
            "-DCAPTURE_EXTRA_ARG=${ARGV3}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
    )
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 5)
endfunction()

add_openchat_capture_test(capture_smoke 860 680)
add_openchat_capture_test(capture_minimum 720 560)
add_openchat_capture_test(capture_large 1024 768)
add_openchat_capture_test(capture_video 900 680 --call-video)
add_openchat_capture_test(capture_video_minimum 720 560 --call-video)
add_openchat_capture_test(capture_screen 1100 780 --call-screen)
add_openchat_capture_test(capture_screen_minimum 720 560 --call-screen)
add_openchat_capture_test(capture_screen_picker 900 700 --call-picker)
# A share and a camera enlarged over the window, and the call filling it:
# several flags, quoted into one list so the runner passes them all through.
add_openchat_capture_test(capture_screen_zoom 1100 780 "--call-screen;--call-zoom")
add_openchat_capture_test(capture_video_zoom 900 680 "--call-video;--call-zoom")
add_openchat_capture_test(capture_screen_fullscreen 1100 780 "--call-screen;--call-fullscreen")
add_openchat_capture_test(capture_video_fullscreen 720 560 "--call-video;--call-fullscreen")
# Jessica's profile (the Scene Queen preset) over the chat window. OpenChat
# exits non-zero unless the page really is on screen (the profile open, the
# profilePage item visible in the window, her own custom page), so the capture
# cannot pass on the chat window underneath.
add_openchat_capture_test(capture_profile 860 680 "--profile;scene-queen")

# The add-contact preview sub-mode: renders the dialog over a populated mock
# Requests list. Uses the same verify runner with the extra --add-contact argument.
add_test(
    NAME capture_add_contact
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_add_contact.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--add-contact
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_add_contact PROPERTIES TIMEOUT 5)

# The contact-verification preview sub-mode: renders the safety-number dialog over
# the mock chat window. Uses the same verify runner with the extra --verify argument.
add_test(
    NAME capture_verify
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_verify.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--verify
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_verify PROPERTIES TIMEOUT 5)

# The in-call preview sub-mode: renders the call surface in place of the
# conversation header, mid-call, with the far end talking so the green speaking
# ring is in the capture.
add_test(
    NAME capture_call
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_call.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--call
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_call PROPERTIES TIMEOUT 5)
