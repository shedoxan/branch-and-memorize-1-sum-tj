cmake_minimum_required(VERSION 3.20)

project(kursovaya LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

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

if(WIN32)
    target_link_libraries(kursovaya_lib PUBLIC psapi)
endif()

if(MSVC)
    set(kursovaya_msvc_runtime
        "$<$<CONFIG:Debug>:MultiThreadedDebugDLL>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")

    target_compile_options(kursovaya_lib PRIVATE /W3 /permissive-)
    target_compile_options(kursovaya PRIVATE /W3 /permissive-)
    set_property(TARGET kursovaya_lib PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")
    set_property(TARGET kursovaya PROPERTY
        MSVC_RUNTIME_LIBRARY
        "${kursovaya_msvc_runtime}")

    set_target_properties(kursovaya PROPERTIES
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
endif()
