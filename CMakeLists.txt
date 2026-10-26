cmake_minimum_required(VERSION 3.24)

project(chess_review VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/ProjectOptions.cmake)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets Sql Svg)
qt_standard_project_setup(REQUIRES 6.12)

add_library(chess_review_chess STATIC src/chess/Position.cpp src/chess/GameTree.cpp)
set_target_properties(chess_review_chess PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(chess_review_chess PUBLIC src)
target_include_directories(chess_review_chess SYSTEM PRIVATE third_party/chess-library)
chess_review_apply_options(chess_review_chess)

add_library(chess_review_pgn STATIC src/pgn/Pgn.cpp)
set_target_properties(chess_review_pgn PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(chess_review_pgn PUBLIC src)
target_link_libraries(chess_review_pgn PUBLIC chess_review_chess)
chess_review_apply_options(chess_review_pgn)

# TOML remains private to the configuration boundary; no parser types escape.
include(FetchContent)
FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tomlplusplus)

add_library(chess_review_review STATIC src/review/Evidence.cpp)
set_target_properties(chess_review_review PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(chess_review_review PUBLIC src)
target_link_libraries(chess_review_review PUBLIC chess_review_chess)
chess_review_apply_options(chess_review_review)

add_library(chess_review_config STATIC src/config/Configuration.cpp)
set_target_properties(chess_review_config PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(chess_review_config PUBLIC src)
target_link_libraries(chess_review_config PUBLIC chess_review_review Qt6::Core PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(chess_review_config PRIVATE TOML_EXCEPTIONS=0 TOML_MAX_NESTED_VALUES=16)
chess_review_apply_options(chess_review_config)

add_library(chess_review_diagnostics STATIC src/diagnostics/Logger.cpp)
target_include_directories(chess_review_diagnostics PUBLIC src)
target_link_libraries(chess_review_diagnostics PUBLIC Qt6::Core)
chess_review_apply_options(chess_review_diagnostics)
target_compile_definitions(chess_review_diagnostics PUBLIC CHESS_REVIEW_VERSION="${PROJECT_VERSION}")

add_library(chess_review_engine STATIC src/engine/EngineProfile.cpp src/engine/EngineSupervisor.cpp
    src/engine/private/EngineProcessWorker.cpp src/engine/private/EngineProcessWorker.h
    src/engine/Analysis.h src/engine/AnalysisCache.cpp src/engine/private/Fingerprint.cpp src/engine/private/SearchProtocol.cpp
    src/engine/private/EngineProcessWorkerSearch.cpp
    src/engine/private/SupervisorCoordinator.cpp src/engine/private/SupervisorCoordinator.h
    src/engine/private/SupervisorScheduling.cpp)
target_include_directories(chess_review_engine PUBLIC src)
target_link_libraries(chess_review_engine PUBLIC Qt6::Core chess_review_chess)
chess_review_apply_options(chess_review_engine)

add_library(chess_review_analysis STATIC src/analysis/ReviewEvidenceAdapter.cpp)
set_target_properties(chess_review_analysis PROPERTIES AUTOMOC OFF AUTOUIC OFF)
target_include_directories(chess_review_analysis PUBLIC src)
target_link_libraries(chess_review_analysis PUBLIC chess_review_engine chess_review_review)
chess_review_apply_options(chess_review_analysis)

add_library(chess_review_persistence STATIC
    src/persistence/private/AnalysisCodec.cpp
    src/persistence/private/AnalysisRepository.cpp
    src/persistence/migrations/005_analysis.cpp
    src/persistence/private/EngineRepository.cpp
    src/persistence/migrations/004_engines.cpp
    src/persistence/GameRepository.h
    src/persistence/private/GameCodec.cpp
    src/persistence/private/GameRepository.cpp
    src/persistence/migrations/002_games.cpp
    src/persistence/migrations/003_game_text.cpp
    src/persistence/Database.h
    src/persistence/Database.cpp
    src/persistence/private/StorageSession.cpp
    src/persistence/private/Migrations.cpp
    src/persistence/private/SchemaHistory.cpp
    src/persistence/private/PrivateFile.cpp
    src/persistence/migrations/001_initial.cpp
)
target_include_directories(chess_review_persistence PUBLIC src)
target_link_libraries(chess_review_persistence PUBLIC Qt6::Core chess_review_pgn chess_review_engine PRIVATE Qt6::Sql chess_review_diagnostics)
chess_review_apply_options(chess_review_persistence)

add_library(chess_review_app_paths STATIC src/app/AppPaths.cpp)
target_include_directories(chess_review_app_paths PUBLIC src)
target_link_libraries(chess_review_app_paths PUBLIC Qt6::Core)
chess_review_apply_options(chess_review_app_paths)

add_library(chess_review_startup STATIC
    src/app/ReviewConfiguration.cpp
    src/app/EngineAnalysisController.cpp
    src/app/EngineSettingsController.cpp
    src/app/AnalysisController.cpp
    src/app/MoveNavigationState.cpp
    src/app/GameImportService.cpp
    src/app/ImportController.cpp
    src/app/GameLibraryController.cpp
    src/import/LocalPgnInput.cpp
    src/app/Diagnostic.cpp
    src/app/StartupCoordinator.cpp
    src/app/NavigationController.cpp
    src/app/UiStateStore.cpp
)
target_link_libraries(chess_review_startup PUBLIC chess_review_persistence chess_review_diagnostics chess_review_config Qt6::Core)
chess_review_apply_options(chess_review_startup)

add_library(chess_review_ui STATIC
    src/ui/EngineText.cpp
    src/ui/settings/EngineSettingsPage.cpp
    src/ui/chess/ChessBoardWidget.cpp
    src/ui/chess/BoardAccessibility.cpp
    src/ui/analysis/EngineAnalysisPanel.cpp
    src/ui/analysis/AnalysisPage.cpp
    src/ui/analysis/MoveNavigation.cpp
    src/ui/analysis/MoveNavigationAccessibility.cpp
    src/ui/shell/MainWindow.cpp
    src/ui/games/GamesPage.cpp
    src/ui/games/PgnTextEdit.cpp
    src/ui/shell/StartupPanel.cpp
    src/ui/shell/DiagnosticsDialog.cpp
)
target_link_libraries(chess_review_ui PUBLIC chess_review_startup Qt6::Widgets PRIVATE Qt6::Svg)
qt_add_resources(chess_review_ui "pieces"
    PREFIX "/pieces"
    BASE "assets/pieces/cburnett"
    FILES
        assets/pieces/cburnett/Chess_plt45.svg
        assets/pieces/cburnett/Chess_pdt45.svg
        assets/pieces/cburnett/Chess_nlt45.svg
        assets/pieces/cburnett/Chess_ndt45.svg
        assets/pieces/cburnett/Chess_blt45.svg
        assets/pieces/cburnett/Chess_bdt45.svg
        assets/pieces/cburnett/Chess_rlt45.svg
        assets/pieces/cburnett/Chess_rdt45.svg
        assets/pieces/cburnett/Chess_qlt45.svg
        assets/pieces/cburnett/Chess_qdt45.svg
        assets/pieces/cburnett/Chess_klt45.svg
        assets/pieces/cburnett/Chess_kdt45.svg
        assets/pieces/cburnett/LICENSE.txt
        assets/pieces/cburnett/provenance.json
)
chess_review_apply_options(chess_review_ui)

qt_add_executable(chess_review_app src/main.cpp)
set_target_properties(chess_review_app PROPERTIES
    OUTPUT_NAME chess-review
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(chess_review_app PRIVATE chess_review_ui chess_review_app_paths)
chess_review_apply_options(chess_review_app)

include(CTest)
if(BUILD_TESTING)
    include(cmake/Testing.cmake)
    add_subdirectory(tests)
    add_subdirectory(src/persistence/tests)
endif()

add_custom_target(run
    COMMAND $<TARGET_FILE:chess_review_app>
    DEPENDS chess_review_app
    USES_TERMINAL
)
