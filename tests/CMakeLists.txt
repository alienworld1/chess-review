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

add_executable(chess_tests unit/PositionTest.cpp unit/GameTreeTest.cpp)
set_target_properties(chess_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(chess_tests PRIVATE chess_review_chess Catch2::Catch2WithMain)
chess_review_apply_options(chess_tests)
catch_discover_tests(chess_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)

# Chess/PGN allocation interception stays in this executable, outside ordinary tests.
add_executable(game_tree_allocation_tests
    unit/GameTreeAllocationTest.cpp support/AllocationFailure.cpp)
target_include_directories(game_tree_allocation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(game_tree_allocation_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(game_tree_allocation_tests PRIVATE chess_review_pgn Catch2::Catch2WithMain)
chess_review_apply_options(game_tree_allocation_tests)
catch_discover_tests(game_tree_allocation_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)

add_executable(pgn_tests unit/PgnTest.cpp)
set_target_properties(pgn_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(pgn_tests PRIVATE chess_review_pgn Catch2::Catch2WithMain)
chess_review_apply_options(pgn_tests)
catch_discover_tests(pgn_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)

chess_review_add_qt_test(analysis_tests analysis INTEGRATION WIDGETS
    SOURCES qt/AnalysisTest.cpp
    LIBRARIES chess_review_ui)

chess_review_add_qt_test(move_navigation_tests move_navigation INTEGRATION WIDGETS
    SOURCES qt/MoveNavigationTest.cpp
    LIBRARIES chess_review_ui)
# Includes 512 asynchronous edits and layout cases; allow sanitizer overhead.
set_tests_properties(move_navigation PROPERTIES TIMEOUT 60)

chess_review_add_qt_test(move_navigation_state_tests move_navigation_state
    SOURCES qt/MoveNavigationStateTest.cpp
    LIBRARIES chess_review_startup)

chess_review_add_qt_test(move_navigation_adversarial_tests move_navigation_adversarial INTEGRATION WIDGETS
    SOURCES qt/MoveNavigationAdversarialTest.cpp
    LIBRARIES chess_review_ui)

chess_review_add_qt_test(import_tests import INTEGRATION WIDGETS
    SOURCES qt/ImportTest.cpp
    LIBRARIES chess_review_ui)

chess_review_add_qt_test(game_opening_tests game_opening INTEGRATION WIDGETS
    SOURCES qt/GameOpeningTest.cpp
    LIBRARIES chess_review_ui)
add_dependencies(game_opening_tests shell_exit_probe)
target_compile_definitions(game_opening_tests PRIVATE
    CHESS_REVIEW_EXIT_PROBE="$<TARGET_FILE:shell_exit_probe>")

chess_review_add_qt_test(engine_fingerprint_tests engine_fingerprint
    SOURCES qt/EngineFingerprintTest.cpp LIBRARIES chess_review_engine)

chess_review_add_qt_test(search_protocol_tests search_protocol
    SOURCES qt/SearchProtocolTest.cpp LIBRARIES chess_review_engine)

# Unix pipe transport; Windows support is intentionally not implemented yet.
if(UNIX)
    add_executable(fake_uci_engine
        fake-uci-engine/main.cpp
        fake-uci-engine/Scenario.cpp
        fake-uci-engine/PipeIo.cpp)
    set_target_properties(fake_uci_engine PROPERTIES
        OUTPUT_NAME fake-uci-engine AUTOMOC OFF AUTOUIC OFF)
    target_link_libraries(fake_uci_engine PRIVATE Qt6::Core)
    chess_review_apply_options(fake_uci_engine)

    chess_review_add_qt_test(fake_uci_tests fake_uci INTEGRATION
        SOURCES qt/FakeUciTest.cpp
        LIBRARIES Qt6::Core)
    add_dependencies(fake_uci_tests fake_uci_engine)
    target_compile_definitions(fake_uci_tests PRIVATE
        FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>"
        FAKE_UCI_SCENARIOS="${CMAKE_CURRENT_SOURCE_DIR}/fake-uci-engine/scenarios")

    chess_review_add_qt_test(engine_validation_tests engine_validation INTEGRATION
        SOURCES qt/EngineValidationTest.cpp LIBRARIES chess_review_engine)
    add_dependencies(engine_validation_tests fake_uci_engine)
    target_compile_definitions(engine_validation_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(engine_runtime_tests engine_runtime INTEGRATION
        SOURCES qt/EngineRuntimeTest.cpp LIBRARIES chess_review_engine)
    add_dependencies(engine_runtime_tests fake_uci_engine)
    target_compile_definitions(engine_runtime_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(engine_process_worker_tests engine_process_worker INTEGRATION
        SOURCES qt/EngineProcessWorkerTest.cpp LIBRARIES chess_review_engine)
    # Multiple real terminate/kill cases need headroom under ASan/UBSan.
    set_tests_properties(engine_process_worker PROPERTIES TIMEOUT 60)
    add_dependencies(engine_process_worker_tests fake_uci_engine)
    target_compile_definitions(engine_process_worker_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(engine_scheduling_tests engine_scheduling INTEGRATION
        SOURCES qt/EngineSchedulingTest.cpp LIBRARIES chess_review_engine)
    # Includes actual 65,536-entry admission/tombstone boundary checks.
    set_tests_properties(engine_scheduling PROPERTIES TIMEOUT 60)
    add_dependencies(engine_scheduling_tests fake_uci_engine)
    target_compile_definitions(engine_scheduling_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(engine_analysis_tests engine_analysis INTEGRATION
        SOURCES qt/EngineAnalysisTest.cpp LIBRARIES chess_review_engine)
    # Real-process timeout cases plus long-history stress exceed 30 seconds under sanitizers.
    set_tests_properties(engine_analysis PROPERTIES TIMEOUT 60)
    add_dependencies(engine_analysis_tests fake_uci_engine)
    target_compile_definitions(engine_analysis_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(live_analysis_tests live_analysis INTEGRATION WIDGETS
        SOURCES qt/LiveAnalysisTest.cpp LIBRARIES chess_review_ui)
    set_tests_properties(live_analysis PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen:configfile=${PROJECT_SOURCE_DIR}/tests/support/desktop-screen.json")
    add_dependencies(live_analysis_tests fake_uci_engine)
    target_compile_definitions(live_analysis_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    chess_review_add_qt_test(engine_settings_tests engine_settings INTEGRATION WIDGETS
        SOURCES qt/EngineSettingsTest.cpp LIBRARIES chess_review_ui)
    add_dependencies(engine_settings_tests fake_uci_engine)
    target_compile_definitions(engine_settings_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")

    qt_add_executable(engine_exit_probe "${PROJECT_SOURCE_DIR}/src/main.cpp" qt/EngineExitProbe.cpp)
    target_include_directories(engine_exit_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
    target_link_libraries(engine_exit_probe PRIVATE chess_review_ui chess_review_app_paths)
    chess_review_apply_options(engine_exit_probe)
    chess_review_add_qt_test(engine_exit_tests engine_exit INTEGRATION
        SOURCES qt/EngineExitTest.cpp LIBRARIES chess_review_engine)
    add_dependencies(engine_exit_tests engine_exit_probe fake_uci_engine)
    target_compile_definitions(engine_exit_tests PRIVATE
        FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>"
        ENGINE_EXIT_PROBE="$<TARGET_FILE:engine_exit_probe>")
endif()

add_executable(review_evidence_tests unit/ReviewEvidenceTest.cpp)
set_target_properties(review_evidence_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(review_evidence_tests PRIVATE chess_review_review Catch2::Catch2WithMain)
chess_review_apply_options(review_evidence_tests)
catch_discover_tests(review_evidence_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)

chess_review_add_qt_test(review_configuration_tests review_configuration INTEGRATION WIDGETS
    SOURCES qt/ReviewConfigurationTest.cpp LIBRARIES chess_review_ui chess_review_analysis)
target_compile_definitions(review_configuration_tests PRIVATE
    CHESS_REVIEW_CONFIG_EXAMPLE="${PROJECT_SOURCE_DIR}/docs/config.example.toml")

if(UNIX)
    chess_review_add_qt_test(quick_review_tests quick_review INTEGRATION WIDGETS
        SOURCES qt/QuickReviewTest.cpp LIBRARIES chess_review_ui Qt6::Sql)
    # Adaptive long-game fixtures perform two stronger passes; allow sanitizer overhead.
    set_tests_properties(quick_review PROPERTIES TIMEOUT 240 ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen:configfile=${PROJECT_SOURCE_DIR}/tests/support/desktop-screen.json")
    add_dependencies(quick_review_tests fake_uci_engine)
    target_compile_definitions(quick_review_tests PRIVATE FAKE_UCI_ENGINE="$<TARGET_FILE:fake_uci_engine>")
endif()

add_executable(review_classification_tests unit/ReviewClassificationTest.cpp unit/ReviewSummaryTest.cpp unit/RetryTest.cpp)
set_target_properties(review_classification_tests PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_link_libraries(review_classification_tests PRIVATE chess_review_review Catch2::Catch2WithMain)
chess_review_apply_options(review_classification_tests)
catch_discover_tests(review_classification_tests ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 30)

chess_review_add_qt_test(review_navigation_tests review_navigation INTEGRATION WIDGETS
    SOURCES qt/ReviewNavigationTest.cpp LIBRARIES chess_review_ui)
