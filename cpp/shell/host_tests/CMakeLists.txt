# C1 桌面（宿主）测试：链接真实的 net_client.cpp（与 Android 同一份实现）。
# Qt 需要 Core + Network：Network 既是被测对象的依赖，也用于测试内置的本地 HTTP 服务。
cmake_minimum_required(VERSION 3.22)
project(C1HostTests LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Network Gui)

add_executable(host_test_net_client
    host_test_net_client.cpp
    ../net_client.cpp
    ../net_client.h
)
target_include_directories(host_test_net_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_test_net_client PRIVATE Qt6::Core Qt6::Network)

# C3 解密链路：与 Android 同一份 jm_crypto.cpp，向量由 openssl CLI 生成。
add_executable(host_test_jm_crypto
    host_test_jm_crypto.cpp
    ../jm_crypto.cpp
    ../jm_crypto.h
)
target_include_directories(host_test_jm_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_test_jm_crypto PRIVATE Qt6::Core Qt6::Gui)

# C3 真网端到端：同一份 jm_api/net_client/jm_crypto，宿主直接打真实接口。
# 0.90 起 NetClient 还依赖 DoH / 节点直连 / QUIC(桩) / 分流配置，宿主测试必须
# 一起链进来，否则 jm_api.cpp 一编译就跟旧 CMakeLists 的链接列表对不上。
add_executable(host_test_jm_api
    host_test_jm_api.cpp
    ../jm_api.cpp
    ../jm_api.h
    ../jm_crypto.cpp
    ../jm_crypto.h
    ../net_client.cpp
    ../net_client.h
    ../net_backend.cpp
    ../net_backend.h
    ../doh_client.cpp
    ../doh_client.h
    ../node_http.cpp
    ../node_http.h
    ../quic/quic_http.cpp
    ../quic/quic_http.h
    ../tls/openssl_tls.cpp
    ../tls/tls_channel.h
    ../ui/proxy_config.cpp
    ../ui/proxy_config.h
    ../ui/app_paths.cpp
    ../ui/app_paths.h
)
target_include_directories(host_test_jm_api PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../ui)
target_link_libraries(host_test_jm_api PRIVATE Qt6::Core Qt6::Network Qt6::Gui)

# ---- 内嵌 libwebp（Qt 6.12 不带 qwebp 插件，JM 图片全是 webp） ----
set(JM_WEBP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libwebp)
file(GLOB JM_WEBP_SOURCES
    ${JM_WEBP_ROOT}/src/dec/*.c
    ${JM_WEBP_ROOT}/src/dsp/*.c
    ${JM_WEBP_ROOT}/src/utils/*.c
)
list(LENGTH JM_WEBP_SOURCES _n)
message(STATUS "webp sources: ${_n}")
add_library(jmwebp STATIC ${JM_WEBP_SOURCES})
set_target_properties(jmwebp PROPERTIES AUTOMOC OFF)
target_include_directories(jmwebp PUBLIC ${JM_WEBP_ROOT} ${JM_WEBP_ROOT}/src)
target_compile_options(jmwebp PRIVATE -w)

add_executable(host_test_jm_webp
    host_test_jm_webp.cpp
    ../jm_webp.cpp
    ../jm_webp.h
)
target_include_directories(host_test_jm_webp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(host_test_jm_webp PRIVATE Qt6::Core Qt6::Gui jmwebp)
