cmake_minimum_required(VERSION 3.14)
project(socket_serverProgram)

set(CMAKE_CXX_STANDARD 14)

add_executable(HelloSocket main.cpp)

find_package(Threads REQUIRED)
add_executable(server server.cpp)
target_link_libraries(server Threads::Threads)

add_executable(client client.cpp)
target_link_libraries(client Threads::Threads)

add_executable(alloc Alloctor.cpp)

