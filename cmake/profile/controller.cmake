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
