# U8 Editor QML: tst_profileeditor.

# The owner's editor (qml/OpenChat/components/Profile{Editor,EditorBar,...}.qml)
# hosted by tests/qml/ProfileEditorHarness.qml over the reference mock: the
# rail, the nine tabs and their controls, the colour picker, Save / Discard /
# undo and the leave dialog. The QML is read from the source tree, so the
# suite needs no rebuild after a QML change.
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
