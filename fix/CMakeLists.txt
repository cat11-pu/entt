cmake_minimum_required(VERSION 3.28)

project(iteration_safety LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EnTT is header-only; point the include path at the repo sources.
add_library(entt_headers INTERFACE)
target_include_directories(entt_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

option(ITERATION_SAFETY_USE_SANITIZER "Build the tests with ASan/UBSan" ON)

function(add_sanitized target)
    if(ITERATION_SAFETY_USE_SANITIZER)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

add_executable(iteration_safety_test tests/iteration_safety_test.cpp)
target_link_libraries(iteration_safety_test PRIVATE entt_headers)
add_sanitized(iteration_safety_test)

add_executable(iteration_safety_stress tests/stress_test.cpp)
target_link_libraries(iteration_safety_stress PRIVATE entt_headers)
add_sanitized(iteration_safety_stress)

# Optional release-mode build that mirrors shipping binaries (asserts disabled).
add_executable(iteration_safety_stress_release tests/stress_test.cpp)
target_link_libraries(iteration_safety_stress_release PRIVATE entt_headers)
target_compile_definitions(iteration_safety_stress_release PRIVATE ENTT_DISABLE_ASSERT)
target_compile_options(iteration_safety_stress_release PRIVATE -O2)
