# C1 桌面（宿主）测试：链接真实的 net_client.cpp（与 Android 同一份实现）。
# Qt 需要 Core + Network：Network 既是被测对象的依赖，也用于测试内置的本地 HTTP 服务。
cmake_minimum_required(VERSION 3.22)
project(C1HostTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network)

add_executable(host_test_net_client
    host_test_net_client.cpp
    ../net_client.cpp
    ../net_client.h
)
target_include_directories(host_test_net_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_test_net_client PRIVATE Qt6::Core Qt6::Network)
