cmake_minimum_required(VERSION 3.20)

project(branch_and_memorize_sum_tj LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BM_SOLVER_ENABLE_IPO "Enable interprocedural optimization / LTO for release-like builds" OFF)
if(BM_SOLVER_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bm_solver_ipo_supported OUTPUT bm_solver_ipo_error)
    if(NOT bm_solver_ipo_supported)
        message(FATAL_ERROR "BM_SOLVER_ENABLE_IPO=ON, but IPO/LTO is not supported: ${bm_solver_ipo_error}")
    endif()
endif()

find_package(Threads REQUIRED)
enable_testing()

add_library(bm_solver_lib
    src/core.cpp
    src/dfs_solver.cpp
    src/generator.cpp
    src/threadpool.cpp
)

target_include_directories(bm_solver_lib PUBLIC src)

target_link_libraries(bm_solver_lib PUBLIC Threads::Threads)

add_executable(bm_solver
    src/main.cpp
)

target_link_libraries(bm_solver PRIVATE bm_solver_lib)

add_executable(solver_bench
    src/benchmark.cpp
)

target_link_libraries(solver_bench PRIVATE bm_solver_lib)

add_executable(bm_solver_tests
    tests/test_main.cpp
)

target_link_libraries(bm_solver_tests PRIVATE bm_solver_lib)

add_test(NAME test_core COMMAND bm_solver_tests test_core)
add_test(NAME test_generator COMMAND bm_solver_tests test_generator)
add_test(NAME test_bruteforce_small COMMAND bm_solver_tests test_bruteforce_small)
add_test(NAME test_decomposition_modes COMMAND bm_solver_tests test_decomposition_modes)
add_test(NAME test_memo_backends COMMAND bm_solver_tests test_memo_backends)
add_test(NAME test_reconstruction COMMAND bm_solver_tests test_reconstruction)
add_test(NAME test_final_preset COMMAND bm_solver_tests test_final_preset)

if(WIN32)
    target_link_libraries(bm_solver_lib PUBLIC psapi)
endif()

if(MSVC)
    set(bm_solver_msvc_runtime
        "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")

    target_compile_options(bm_solver_lib PRIVATE /W3 /permissive-)
    target_compile_options(bm_solver PRIVATE /W3 /permissive-)
    target_compile_options(solver_bench PRIVATE /W3 /permissive-)
    target_compile_options(bm_solver_tests PRIVATE /W3 /permissive-)
    target_link_options(bm_solver PRIVATE /STACK:134217728)
    target_link_options(solver_bench PRIVATE /STACK:134217728)
    target_link_options(bm_solver_tests PRIVATE /STACK:134217728)
    set_property(TARGET bm_solver_lib PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${bm_solver_msvc_runtime}")
    set_property(TARGET bm_solver PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${bm_solver_msvc_runtime}")
    set_property(TARGET solver_bench PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${bm_solver_msvc_runtime}")
    set_property(TARGET bm_solver_tests PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${bm_solver_msvc_runtime}")

    set_target_properties(bm_solver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
    set_target_properties(bm_solver_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
    set_target_properties(solver_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
else()
    target_compile_options(bm_solver_lib PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
    target_compile_options(bm_solver PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
    target_compile_options(solver_bench PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
    target_compile_options(bm_solver_tests PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
endif()

if(BM_SOLVER_ENABLE_IPO)
    set_property(TARGET bm_solver_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET bm_solver PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET solver_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET bm_solver_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
