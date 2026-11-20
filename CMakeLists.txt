
cmake_minimum_required(VERSION 3.20)

project(LoadTester LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(loadtester src/main.cpp)

target_link_libraries(loadtester
    PRIVATE
    CURL::libcurl
    Threads::Threads
    nlohmann_json::nlohmann_json
)

target_compile_options(loadtester PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
