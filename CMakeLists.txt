cmake_minimum_required(VERSION 3.24)

project(chess_review VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/ProjectOptions.cmake)

find_package(Qt6 6.12 REQUIRED COMPONENTS Widgets Sql)
qt_standard_project_setup(REQUIRES 6.12)

add_library(chess_review_diagnostics STATIC src/diagnostics/Logger.cpp)
target_include_directories(chess_review_diagnostics PUBLIC src)
target_link_libraries(chess_review_diagnostics PUBLIC Qt6::Core)
chess_review_apply_options(chess_review_diagnostics)
target_compile_definitions(chess_review_diagnostics PUBLIC CHESS_REVIEW_VERSION="${PROJECT_VERSION}")

add_library(chess_review_persistence STATIC
    src/persistence/Database.h
    src/persistence/Database.cpp
    src/persistence/private/StorageSession.cpp
    src/persistence/private/Migrations.cpp
    src/persistence/private/SchemaHistory.cpp
    src/persistence/private/PrivateFile.cpp
    src/persistence/migrations/001_initial.cpp
)
target_include_directories(chess_review_persistence PUBLIC src)
target_link_libraries(chess_review_persistence PUBLIC Qt6::Core PRIVATE Qt6::Sql chess_review_diagnostics)
chess_review_apply_options(chess_review_persistence)

add_library(chess_review_app_paths STATIC src/app/AppPaths.cpp)
target_include_directories(chess_review_app_paths PUBLIC src)
target_link_libraries(chess_review_app_paths PUBLIC Qt6::Core)
chess_review_apply_options(chess_review_app_paths)

add_library(chess_review_startup STATIC
    src/app/Diagnostic.cpp
    src/app/StartupCoordinator.cpp
    src/app/NavigationController.cpp
    src/app/UiStateStore.cpp
)
target_link_libraries(chess_review_startup PUBLIC chess_review_persistence chess_review_diagnostics Qt6::Core)
chess_review_apply_options(chess_review_startup)

add_library(chess_review_ui STATIC
    src/ui/shell/MainWindow.cpp
    src/ui/shell/StartupPanel.cpp
    src/ui/shell/DiagnosticsDialog.cpp
)
target_link_libraries(chess_review_ui PUBLIC chess_review_startup Qt6::Widgets)
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
