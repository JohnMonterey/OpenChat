# U9a C++ integration: app and gallery registration and the --profile preview capture.

# tst_chatcontroller checks the Appearance › Profiles choice (plainProfiles)
# beside the settings list that shows it, and that Low memory mode reaches the
# profile renderer. The cosmetics catalogue AppearanceSettings reads and the
# renderer's singletons come with openchat_profile (linked by controller.cmake).
target_sources(tst_chatcontroller PRIVATE
    src/app/AppearanceSettings.cpp
    src/app/MemorySettings.cpp
)

# The --profile preview, captured: Jessica's page (Scene Queen) over the chat
# window at the default size. OpenChat exits non-zero unless the page really is
# on screen (the profile open, the profilePage item in the window, her own
# custom page), so the capture cannot pass on the chat window underneath. It
# stays disabled until Main.qml hosts ProfilePage (U9b), which drops the
# DISABLED property when it folds this fragment into CMakeLists.txt.
add_openchat_capture_test(capture_profile 860 680 "--profile;scene-queen")
set_tests_properties(capture_profile PROPERTIES DISABLED TRUE)
