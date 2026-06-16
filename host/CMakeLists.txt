cmake_minimum_required(VERSION 3.16)

project(picowal_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(picowal_host
    ../src/picowal_api.c
    ../src/picowal_retail.c
    ../src/picowal_search.c
    ../src/picowal_store_fs.c
    ../src/picowal_store_aztable.c
)

target_include_directories(picowal_host PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../src
)

target_compile_definitions(picowal_host PUBLIC
    PICOWAL_HOST=1
    PICOWAL_NO_DEFAULT_STORE=1
)

if(UNIX)
    target_link_libraries(picowal_host PUBLIC m)
endif()

add_executable(picowal_fs_smoke
    tests/fs_smoke.c
)

target_link_libraries(picowal_fs_smoke PRIVATE picowal_host)

add_executable(picowal_aztable_config_smoke
    tests/aztable_config_smoke.c
)

target_link_libraries(picowal_aztable_config_smoke PRIVATE picowal_host)

add_executable(picowal_aztable_live_smoke
    tests/aztable_live_smoke.c
)

target_link_libraries(picowal_aztable_live_smoke PRIVATE picowal_host)

add_executable(picowal_search_smoke
    tests/search_smoke.c
)

target_link_libraries(picowal_search_smoke PRIVATE picowal_host)

set(PICOWEB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../picoweb" CACHE PATH "Path to picoweb checkout")
if(EXISTS "${PICOWEB_ROOT}/include/picoweb.h")
    add_executable(picowal_retail_smoke
        ../src/picowal_retail_web.c
        tests/retail_smoke.c
    )
    target_include_directories(picowal_retail_smoke PRIVATE "${PICOWEB_ROOT}/include")
    target_link_libraries(picowal_retail_smoke PRIVATE picowal_host)
endif()
