chess_review_add_qt_test(diagnostics_tests diagnostics INTEGRATION
    SOURCES qt/DiagnosticsTest.cpp
    LIBRARIES chess_review_diagnostics)

chess_review_add_qt_test(startup_tests startup INTEGRATION WIDGETS
    SOURCES qt/StartupTest.cpp
    LIBRARIES chess_review_ui chess_review_app_paths)

chess_review_add_qt_test(app_paths_tests app_paths
    SOURCES qt/AppPathsTest.cpp
    LIBRARIES chess_review_app_paths)

chess_review_add_qt_test(shell_tests shell INTEGRATION WIDGETS
    SOURCES qt/ShellTest.cpp
    LIBRARIES chess_review_ui)

# Exercise the production composition root and QApplication::exec(), with a test-only UI driver.
qt_add_executable(shell_exit_probe
    "${PROJECT_SOURCE_DIR}/src/main.cpp"
    qt/ShellExitProbe.cpp
)
target_link_libraries(shell_exit_probe PRIVATE chess_review_ui chess_review_app_paths)
chess_review_apply_options(shell_exit_probe)
add_dependencies(shell_tests shell_exit_probe)
target_compile_definitions(shell_tests PRIVATE
    CHESS_REVIEW_EXIT_PROBE="$<TARGET_FILE:shell_exit_probe>")

add_executable(chess_tests unit/PositionTest.cpp)
set_target_properties(chess_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(chess_tests PRIVATE chess_review_chess Catch2::Catch2WithMain)
chess_review_apply_options(chess_tests)
catch_discover_tests(chess_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)
