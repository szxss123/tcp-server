cmake_minimum_required(VERSION 3.16)
project(tcp_server)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(tcp_server
    src/main.cpp
    src/TcpServer.cpp
    src/ThreadPool.cpp
)

target_include_directories(tcp_server PRIVATE include)
target_link_libraries(tcp_server PRIVATE Threads::Threads)