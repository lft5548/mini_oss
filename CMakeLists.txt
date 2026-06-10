cmake_minimum_required(VERSION 3.16)

project(mini_oss
    VERSION 0.1.0
    DESCRIPTION "Linux C++ high-concurrency object storage service"
    LANGUAGES CXX
)

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(mini_oss
    src/main.cpp
    src/http.cpp
    src/http_server.cpp
    src/metadata_store.cpp
    src/object_store.cpp
    src/router.cpp
)

target_include_directories(mini_oss PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(mini_oss PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_libraries(mini_oss PRIVATE
    OpenSSL::Crypto
    SQLite::SQLite3
)
