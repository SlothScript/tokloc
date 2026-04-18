cmake_minimum_required(VERSION 3.10)
project(tokloc)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto")

include(FetchContent)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.0
)
FetchContent_MakeAvailable(cli11)

find_package(Threads REQUIRED)

option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    find_package(doctest REQUIRED)

    add_executable(tests tests/tests.cpp)
    target_compile_definitions(tests PRIVATE TESTING)
    target_link_libraries(tests PRIVATE doctest::doctest)

    # Force warnings off for the test target specifically
    if(MSVC)
        target_compile_options(tests PRIVATE /W0)
    else()
        target_compile_options(tests PRIVATE -w)
    endif()

    enable_testing()
    add_test(NAME tests COMMAND tests "-s")
endif()

add_executable(${PROJECT_NAME} src/tokloc.cpp)

if(NOT MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -pedantic)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE CLI11::CLI11 Threads::Threads)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
