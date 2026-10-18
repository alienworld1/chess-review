cmake_minimum_required(VERSION 3.24)

project(chess_review VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Sql)
qt_standard_project_setup(REQUIRES 6.8)

add_library(chess_review_diagnostics STATIC src/diagnostics/Logger.cpp)
target_include_directories(chess_review_diagnostics PUBLIC src)
target_link_libraries(chess_review_diagnostics PUBLIC Qt6::Core)
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

add_library(chess_review_app_paths STATIC src/app/AppPaths.cpp)
target_include_directories(chess_review_app_paths PUBLIC src)
target_link_libraries(chess_review_app_paths PUBLIC Qt6::Core)

add_library(chess_review_startup STATIC
    src/app/Diagnostic.cpp
    src/app/StartupCoordinator.cpp
    src/ui/StartupWindow.cpp
)
target_link_libraries(chess_review_startup PUBLIC chess_review_persistence chess_review_diagnostics Qt6::Widgets)

qt_add_executable(chess_review_app src/main.cpp)
set_target_properties(chess_review_app PROPERTIES
    OUTPUT_NAME chess-review
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(chess_review_app PRIVATE chess_review_startup chess_review_app_paths)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_executable(diagnostics_tests tests/qt/DiagnosticsTest.cpp)
    target_link_libraries(diagnostics_tests PRIVATE chess_review_diagnostics Qt6::Test)
    add_test(NAME diagnostics COMMAND diagnostics_tests)

    add_executable(persistence_tests src/persistence/tests/DatabaseTest.cpp)
    target_link_libraries(persistence_tests PRIVATE chess_review_persistence Qt6::Sql Qt6::Test)
    add_test(NAME persistence COMMAND persistence_tests)

    add_executable(startup_tests tests/qt/StartupTest.cpp)
    target_link_libraries(startup_tests PRIVATE chess_review_startup chess_review_app_paths Qt6::Test)
    add_test(NAME startup COMMAND startup_tests)
    set_tests_properties(startup PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(app_paths_tests tests/qt/AppPathsTest.cpp)
    target_link_libraries(app_paths_tests PRIVATE chess_review_app_paths Qt6::Test)
    add_test(NAME app_paths COMMAND app_paths_tests)
    set_tests_properties(diagnostics persistence startup app_paths PROPERTIES TIMEOUT 30)
endif()

foreach(target diagnostics_tests chess_review_diagnostics chess_review_app chess_review_app_paths chess_review_startup chess_review_persistence persistence_tests startup_tests app_paths_tests)
    if(TARGET ${target})
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            )
        elseif(MSVC)
            target_compile_options(${target} PRIVATE /W4 /permissive-)
        endif()
    endif()
endforeach()

add_custom_target(run
    COMMAND $<TARGET_FILE:chess_review_app>
    DEPENDS chess_review_app
    USES_TERMINAL
)
