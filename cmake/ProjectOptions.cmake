include_guard(GLOBAL)

option(CHESS_REVIEW_WARNINGS_AS_ERRORS "Treat project compiler warnings as errors" OFF)
option(CHESS_REVIEW_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(CHESS_REVIEW_SANITIZERS AND
   (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    message(FATAL_ERROR "CHESS_REVIEW_SANITIZERS requires GCC or Clang on Linux")
endif()

function(chess_review_apply_options target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
        if(CHESS_REVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CHESS_REVIEW_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()
    if(CHESS_REVIEW_SANITIZERS)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()
