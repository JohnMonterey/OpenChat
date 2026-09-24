# U9a C++ integration: app and gallery registration and the --profile preview capture.

# tst_chatcontroller checks the Appearance › Profiles choice (plainProfiles)
# beside the settings list that shows it, and that Low memory mode reaches the
# profile renderer. The cosmetics catalogue AppearanceSettings reads and the
# renderer's singletons come with openchat_profile (linked by controller.cmake).
target_sources(tst_chatcontroller PRIVATE
    src/app/AppearanceSettings.cpp
    src/app/MemorySettings.cpp
)
