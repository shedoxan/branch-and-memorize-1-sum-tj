cmake_minimum_required(VERSION 3.20)

project(kursovaya LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KURSOVAYA_ENABLE_IPO "Enable interprocedural optimization / LTO for release-like builds" OFF)
if(KURSOVAYA_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT kursovaya_ipo_supported OUTPUT kursovaya_ipo_error)
    if(NOT kursovaya_ipo_supported)
        message(FATAL_ERROR "KURSOVAYA_ENABLE_IPO=ON, but IPO/LTO is not supported: ${kursovaya_ipo_error}")
    endif()
endif()

find_package(Threads REQUIRED)
enable_testing()

add_library(kursovaya_lib
    src/core.cpp
    src/dfs_solver.cpp
    src/generator.cpp
    src/threadpool.cpp
)

target_include_directories(kursovaya_lib PUBLIC src)

target_link_libraries(kursovaya_lib PUBLIC Threads::Threads)

add_executable(kursovaya
    src/main.cpp
)

target_link_libraries(kursovaya PRIVATE kursovaya_lib)

add_executable(solver_bench
    src/benchmark.cpp
)

target_link_libraries(solver_bench PRIVATE kursovaya_lib)

add_executable(kursovaya_tests
    tests/test_main.cpp
)

target_link_libraries(kursovaya_tests PRIVATE kursovaya_lib)

add_test(NAME test_core COMMAND kursovaya_tests test_core)
add_test(NAME test_generator COMMAND kursovaya_tests test_generator)
add_test(NAME test_bruteforce_small COMMAND kursovaya_tests test_bruteforce_small)
add_test(NAME test_decomposition_modes COMMAND kursovaya_tests test_decomposition_modes)
add_test(NAME test_memo_backends COMMAND kursovaya_tests test_memo_backends)
add_test(NAME test_reconstruction COMMAND kursovaya_tests test_reconstruction)
add_test(NAME test_final_preset COMMAND kursovaya_tests test_final_preset)

if(WIN32)
    target_link_libraries(kursovaya_lib PUBLIC psapi)
endif()

if(MSVC)
    set(kursovaya_msvc_runtime
        "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")

    target_compile_options(kursovaya_lib PRIVATE /W3 /permissive-)
    target_compile_options(kursovaya PRIVATE /W3 /permissive-)
    target_compile_options(solver_bench PRIVATE /W3 /permissive-)
    target_compile_options(kursovaya_tests PRIVATE /W3 /permissive-)
    target_link_options(kursovaya PRIVATE /STACK:134217728)
    target_link_options(solver_bench PRIVATE /STACK:134217728)
    target_link_options(kursovaya_tests PRIVATE /STACK:134217728)
    set_property(TARGET kursovaya_lib PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")
    set_property(TARGET kursovaya PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")
    set_property(TARGET solver_bench PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")
    set_property(TARGET kursovaya_tests PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")

    set_target_properties(kursovaya PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
    set_target_properties(kursovaya_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
    set_target_properties(solver_bench PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>")
else()
    target_compile_options(kursovaya_lib PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
    target_compile_options(kursovaya PRIVATE
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
    target_compile_options(kursovaya_tests PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
endif()

if(KURSOVAYA_ENABLE_IPO)
    set_property(TARGET kursovaya_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET kursovaya PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET solver_bench PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET kursovaya_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
