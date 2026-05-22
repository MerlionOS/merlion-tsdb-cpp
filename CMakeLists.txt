cmake_minimum_required(VERSION 3.28)
project(merlion_tsdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wunused
    -Woverloaded-virtual -Wnull-dereference
    -Wdouble-promotion -Wformat=2
)

option(MERLION_TSDB_BUILD_TESTS "Build unit tests" ON)
option(MERLION_TSDB_BUILD_BENCH "Build benchmarks" OFF)
option(MERLION_TSDB_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(MERLION_TSDB_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(MERLION_TSDB_ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
if(MERLION_TSDB_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(merlion_tsdb_core STATIC
    src/encoding/bstream.cpp
    src/encoding/varint.cpp
    src/encoding/crc32c.cpp
    src/chunkenc/xor.cpp
    src/wal/page.cpp
    src/wal/segment_writer.cpp
    src/wal/segment_reader.cpp
    src/wal/record.cpp
    src/model/labels.cpp
    src/head/mem_series.cpp
    src/head/series_store.cpp
    src/head/head.cpp
    src/block/meta.cpp
    src/block/chunks.cpp
)
target_include_directories(merlion_tsdb_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(merlion_tsdb_core PRIVATE nlohmann_json::nlohmann_json)

add_executable(merlion_tsdb_smoke src/smoke.cpp)
target_link_libraries(merlion_tsdb_smoke PRIVATE merlion_tsdb_core)

if(MERLION_TSDB_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

if(MERLION_TSDB_BUILD_BENCH)
    add_subdirectory(bench)
endif()
