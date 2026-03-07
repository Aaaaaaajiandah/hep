cmake_minimum_required(VERSION 3.20)
project(hep C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(ZLIB REQUIRED)

add_compile_options(-Wall -Wextra -O2)

add_executable(hep
    src/main.c
    src/core/sha1.c
    src/core/zlib_utils.c
    src/core/odb.c
    src/core/blob.c
    src/core/tree.c
    src/core/commit.c
    src/repo.c
    src/index.c
    src/refs.c
    src/util.c
)

target_include_directories(bro PRIVATE src src/core)
target_link_libraries(bro PRIVATE ZLIB::ZLIB)
