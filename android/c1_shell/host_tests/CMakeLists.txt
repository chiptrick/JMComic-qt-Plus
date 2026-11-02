# C1 P4-4：桌面（宿主）闭环测试 —— 与 Android 共用同一份 python_bridge.cpp。
# Qt 只用到 Core + Network（Network 用于测试内置的本地 HTTP 服务）。
cmake_minimum_required(VERSION 3.22)
project(C1HostTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)

add_executable(host_test_python_bridge
    host_test_python_bridge.cpp
    ../python_bridge.cpp
    ../python_bridge.h
)
target_include_directories(host_test_python_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_test_python_bridge PRIVATE Qt6::Core Qt6::Network)
