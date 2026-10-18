cmake_minimum_required(VERSION 3.24)

project(chess_review VERSION 0.1.0 LANGUAGES CXX)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(chess_review_app src/main.cpp)
set_target_properties(chess_review_app PROPERTIES
    OUTPUT_NAME chess-review
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_link_libraries(chess_review_app PRIVATE Qt6::Widgets)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(chess_review_app PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    )
elseif(MSVC)
    target_compile_options(chess_review_app PRIVATE /W4 /permissive-)
endif()

add_custom_target(run
    COMMAND $<TARGET_FILE:chess_review_app>
    DEPENDS chess_review_app
    USES_TERMINAL
)
