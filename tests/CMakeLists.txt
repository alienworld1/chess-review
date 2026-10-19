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
