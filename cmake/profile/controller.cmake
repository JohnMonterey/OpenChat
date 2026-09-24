# U6 Controller: ProfileController and friends, profile_qml_test_sources/libs and tst_profilecontroller.

# The QML-facing profile controller, its page objects and resolved style, the
# editor's undo history and the reference mock's pages. ChatController owns
# the controller, so they are compiled beside ChatController.cpp in every
# target that compiles it, each also linking the renderer library.
set(profile_controller_sources
    src/controllers/ProfileController.cpp
    src/controllers/ProfilePageObject.cpp
    src/controllers/ProfileReferencePages.cpp
    src/controllers/ProfileEditHistory.cpp
)
foreach(profile_target IN ITEMS OpenChat tst_e2e tst_chatcontroller tst_qmlload openchat-profile-gallery)
    target_sources(${profile_target} PRIVATE ${profile_controller_sources})
    target_link_libraries(${profile_target} PRIVATE openchat_profile)
endforeach()

# What a suite built on tests/ProfileQmlHarness.h compiles and links: the
# controllers and models the chat window's QML binds to, the native types the
# harness registers (as tst_qmlload's list does) and the profile controller.
# tst_profilecontroller, tst_profilepageqml and tst_profileeditor use both.
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

# The controller's navigation, resolution, page objects, editing, undo,
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
