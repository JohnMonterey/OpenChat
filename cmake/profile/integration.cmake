# U9a C++ integration: app and gallery registration and the --profile preview capture.

# tst_chatcontroller checks the Appearance › Profiles choice (plainProfiles)
# beside the settings list that shows it. AppearanceSettings reads the
# cosmetics catalogue, which openchat_profile (linked by controller.cmake)
# brings in.
target_sources(tst_chatcontroller PRIVATE src/app/AppearanceSettings.cpp)
