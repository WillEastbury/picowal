cmake_minimum_required(VERSION 3.16)

project(picowal_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(picowal_host
    ../src/picowal_api.c
    ../src/picowal_store_fs.c
)

target_include_directories(picowal_host PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_definitions(picowal_host PUBLIC
    PICOWAL_HOST=1
    PICOWAL_NO_DEFAULT_STORE=1
)

add_executable(picowal_fs_smoke
    tests/fs_smoke.c
)

target_link_libraries(picowal_fs_smoke PRIVATE picowal_host)

