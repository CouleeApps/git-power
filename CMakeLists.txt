cmake_minimum_required(VERSION 3.19)
project(git-power)

set(CMAKE_CXX_STANDARD 14)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
find_package(libgit2 REQUIRED)


add_executable(git-power main.cpp)

target_include_directories(git-power PRIVATE ${LIBGIT2_INCLUDE_DIR})
target_link_libraries(git-power PRIVATE ${LIBGIT2_LIBRARIES})

install(TARGETS git-power)
