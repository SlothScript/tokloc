cmake_minimum_required(VERSION 3.10)
project(tokloc)

# Allow older third-party CMakeLists.txt files to configure under newer CMake.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/onqtam/doctest.git
    GIT_TAG v2.4.9
)
FetchContent_Declare(
    yyjson
    GIT_REPOSITORY https://github.com/ibireme/yyjson.git
    GIT_TAG 0.9.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cli11 doctest yyjson)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

option(BUILD_TESTS "Build tests" OFF)

if(BUILD_TESTS)
    add_executable(tests tests/tests.cpp)
    target_compile_definitions(tests PRIVATE TESTING)
    target_link_libraries(tests PRIVATE CLI11::CLI11 doctest::doctest CURL::libcurl yyjson)

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

target_link_libraries(${PROJECT_NAME} PRIVATE CLI11::CLI11 Threads::Threads CURL::libcurl yyjson)

include(GNUInstallDirs)
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
