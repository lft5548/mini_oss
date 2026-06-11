cmake_minimum_required(VERSION 3.16)

project(mini_oss
    VERSION 0.1.0
    DESCRIPTION "Linux C++ high-concurrency object storage service"
    LANGUAGES CXX
)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MINI_OSS_CORE_SOURCES
    src/config.cpp
    src/http.cpp
    src/http_server.cpp
    src/logger.cpp
    src/metadata_store.cpp
    src/metrics.cpp
    src/object_store.cpp
    src/redis_metadata_cache.cpp
    src/router.cpp
    src/thread_pool.cpp
)

add_library(mini_oss_core STATIC
    ${MINI_OSS_CORE_SOURCES}
)

target_include_directories(mini_oss_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(mini_oss_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(mini_oss_core PUBLIC
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    PkgConfig::HIREDIS
)

add_executable(mini_oss
    src/main.cpp
)

target_compile_options(mini_oss PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(mini_oss PRIVATE
    mini_oss_core
)

include(CTest)

if(BUILD_TESTING)
    add_executable(mini_oss_tests
        tests/test_main.cpp
    )

    target_compile_options(mini_oss_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    target_link_libraries(mini_oss_tests PRIVATE
        mini_oss_core
    )

    add_test(NAME mini_oss_unit_tests COMMAND mini_oss_tests)
endif()
