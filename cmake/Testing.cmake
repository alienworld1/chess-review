include_guard(GLOBAL)

find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
include(FetchContent)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CATCH_BUILD_EXTRA_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
    URL https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v3.8.1
    URL_HASH SHA256=18b3f70ac80fccc340d8c6ff0f339b2ae64944782f8d2fca2bd705cf47cadb79
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(Catch2)
# Build Catch2 only when a test target actually links it.
set_property(DIRECTORY "${catch2_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
set_target_properties(Catch2 Catch2WithMain PROPERTIES AUTOMOC OFF AUTOUIC OFF)
include("${catch2_SOURCE_DIR}/extras/Catch.cmake")

function(chess_review_add_qt_test target name)
    cmake_parse_arguments(TEST "WIDGETS;INTEGRATION" "" "SOURCES;LIBRARIES" ${ARGN})
    add_executable(${target} ${TEST_SOURCES})
    target_link_libraries(${target} PRIVATE ${TEST_LIBRARIES} Qt6::Test)
    chess_review_apply_options(${target})
    add_test(NAME ${name} COMMAND ${target})
    set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS "qt;${name}")
    if(TEST_INTEGRATION)
        set_property(TEST ${name} APPEND PROPERTY LABELS integration)
    endif()
    if(TEST_WIDGETS)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endfunction()
