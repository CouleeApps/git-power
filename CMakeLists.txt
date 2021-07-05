cmake_minimum_required(VERSION 3.10)
project(git-power)

set(CMAKE_CXX_STANDARD 14)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(libgit2 REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(git-power main.cpp)

target_include_directories(git-power PRIVATE ${LIBGIT2_INCLUDE_DIR})
target_include_directories(git-power PRIVATE ${OPENSSL_INCLUDE_DIR})
target_link_libraries(git-power PRIVATE ${LIBGIT2_LIBRARIES})
target_link_libraries(git-power PRIVATE ${OPENSSL_CRYPTO_LIBRARIES})

if (UNIX AND NOT APPLE)
    # Ubuntu is a snowflake
    target_link_libraries(git-power PRIVATE pthread atomic)
endif ()

install(TARGETS git-power)
