# U7 Page QML: tst_profilepageqml.

# The profile page kit (qml/OpenChat/components/Profile*.qml), hosted as
# Main.qml hosts it by tests/qml/ProfilePageHarness.qml over the reference
# mock: the SPEC §3.1 geometry, the painted-area budget, plain text, the
# Contacting actions per relationship and call state, the page's one song
# player, the call strip, the back stack and its history menu, stubs, the
# Plain style switch, the animation policy, borders and focus rings.
# Offscreen on the software scene graph, every warning failing the test.
# OPENCHAT_PROFILE_CAPTURES=<dir> also writes the review PNGs.
# Copy Invite Link runs against a real RelayClient and the fake HTTPS relay
# (tests/relay/RelayTestSupport), so it copies what the relay really minted.
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
