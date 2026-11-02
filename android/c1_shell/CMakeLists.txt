# C1-W1：Qt 6.12 C++ 壳工程骨架（壳 + 导航占位；沿用 C0 的直链与 16KB 门禁）
# 验收：lib 门禁 0 未对齐 + Android 17 实机启动；
# C0-③：libsr_qnn.so 以 IMPORTED 方式直链（去 ctypes），随 APK 打包并在设备上验证加载。
#
# 网络：C1 不再起 Python 子进程（原 W6/P4 方案），改由 net_client.cpp 在进程内发请求。
# TLS 交给 Qt 的 OpenSSL 后端，所以要把 libssl/libcrypto 一起打进 APK（见 OPENSSL_DIR）。
cmake_minimum_required(VERSION 3.22.1)
project(C1Shell VERSION 0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_standard_project_setup()

qt_add_executable(c1shell
    main.cpp
    reader.cpp
    settings_page.cpp
    net_client.cpp
)

# CA 证书编进二进制（qrc）。APK 的 assets 也留一份，但 Android 上 assets 路径引擎
# 并非在所有生命周期阶段都可用——资源是确定性最高的那条路。
qt_add_resources(c1shell "ca_bundle"
    PREFIX "/"
    FILES assets/cacert.pem
)

# --- C1：sr_qnn 直链 ---
# 库的位置默认取构建树里 repack 用的同一份（build_wsl.sh 会把源码 rsync 到
# $JM_WORK/src/android/c0_skeleton，../libs/<abi> 即 $JM_WORK/src/android/libs/<abi>），
# 与 wsl_repack_apk 打包 Python 应用用的是同一批 0x4000 对齐产物，避免两处副本漂移。
set(SRQ_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs/${ANDROID_ABI}"
    CACHE PATH "预编译 sr_qnn / onnxruntime 库目录")
set(SRQ_LIB  "${SRQ_LIBS_DIR}/libsr_qnn.so")
set(ORT_LIB  "${SRQ_LIBS_DIR}/libonnxruntime.so")
# 缺库时必须让构建失败，而不是静默产出一个"门禁看起来更绿"的包（11/0/0 少了两个库，
# 表面上比 13/0/0 还干净）——C1 直链验证全靠"链上了"，静默跳过等于把实验做废。
option(SRQ_REQUIRED "缺少 sr_qnn/onnxruntime 库时直接报错（C1 直链验证必须为 ON）" ON)

set(C1_EXTRA_LIBS "")
if(EXISTS "${SRQ_LIB}" AND EXISTS "${ORT_LIB}")
    add_library(sr_qnn SHARED IMPORTED)
    set_target_properties(sr_qnn PROPERTIES IMPORTED_LOCATION "${SRQ_LIB}")
    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION "${ORT_LIB}")
    target_link_libraries(c1shell PRIVATE sr_qnn onnxruntime)
    list(APPEND C1_EXTRA_LIBS "${SRQ_LIB}" "${ORT_LIB}")
    message(STATUS "C1: linking ${SRQ_LIB}")
elseif(SRQ_REQUIRED)
    message(FATAL_ERROR "C1: 缺 ${SRQ_LIBS_DIR} 下的 libsr_qnn.so / libonnxruntime.so。"
                        "直链验证必须真有库；如只想要 v1 骨架请显式 -DSRQ_REQUIRED=OFF。")
else()
    message(WARNING "C1: sr_qnn/onnxruntime 库不在 ${SRQ_LIBS_DIR}，本次构建不含直链验证")
endif()

# --- C1 网络：OpenSSL（Qt 的 TLS 后端在运行时 dlopen libssl/libcrypto） ---
# build_wsl.sh 从现有 p4a dist 复制到 openssl/<abi>/（soname 就是 libssl.so /
# libcrypto.so，p_align=0x4000，与 16KB 门禁口径一致）。
# 缺了会怎样：APK 能装能跑，但所有 HTTPS 直接 SSLErr——比崩溃更难查，所以默认必须存在。
set(OPENSSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/openssl/${ANDROID_ABI}"
    CACHE PATH "随包 OpenSSL 库目录（Qt TLS 后端运行时加载）")
option(C1_TLS_REQUIRED "缺少 libssl/libcrypto 时直接报错（HTTPS 依赖）" ON)

if(EXISTS "${OPENSSL_DIR}/libssl.so" AND EXISTS "${OPENSSL_DIR}/libcrypto.so")
    list(APPEND C1_EXTRA_LIBS
        "${OPENSSL_DIR}/libssl.so" "${OPENSSL_DIR}/libcrypto.so")
    # Qt 的 OSSL 后端在 Android 上按 libssl_3.so / libcrypto_3.so 命名找库（它自家
    # android_openssl 包的约定）；只给 libssl.so 会报 "Failed to load libssl/libcrypto."
    foreach(_alias libssl_3 libcrypto_3)
        if(EXISTS "${OPENSSL_DIR}/${_alias}.so")
            list(APPEND C1_EXTRA_LIBS "${OPENSSL_DIR}/${_alias}.so")
        endif()
    endforeach()
    message(STATUS "C1: packaging OpenSSL from ${OPENSSL_DIR}")
elseif(C1_TLS_REQUIRED)
    message(FATAL_ERROR "C1: 缺 ${OPENSSL_DIR} 下的 libssl.so / libcrypto.so。"
                        "没有它们 HTTPS 会全部失败；确有需要可 -DC1_TLS_REQUIRED=OFF。")
else()
    message(WARNING "C1: 未打包 OpenSSL，HTTPS 请求会在运行时报 SSLErr")
endif()

# Qt 的 TLS 后端是插件：androiddeployqt 不会因为 target_link_libraries(Qt6::Network) 就
# 把它塞进 APK（实测包内 plugins/ 为空 → 运行期 "TLS initialization failed"）。
# Android 上 Qt 从 lib/<abi>/ 按 libplugins_tls_<name>_<abi>.so 的名字找插件，所以直接把
# 插件的 .so 加进随包库即可。
if(ANDROID)
    # 优先用构建脚本显式传入的路径（它知道 $QT_ANDROID）；没传就按 Qt6::Core 的安装位置推。
    set(C1_TLS_PLUGIN "" CACHE FILEPATH "Qt TLS 后端插件路径（libplugins_tls_qopensslbackend_<abi>.so）")
    if(NOT C1_TLS_PLUGIN)
        get_target_property(_qtcore_loc Qt6::Core IMPORTED_LOCATION)
        if(_qtcore_loc)
            get_filename_component(_qtlib_dir "${_qtcore_loc}" DIRECTORY)
            get_filename_component(_qt_prefix "${_qtlib_dir}" DIRECTORY)
            set(C1_TLS_PLUGIN
                "${_qt_prefix}/plugins/tls/libplugins_tls_qopensslbackend_${ANDROID_ABI}.so")
        endif()
    endif()
    if(C1_TLS_PLUGIN AND EXISTS "${C1_TLS_PLUGIN}")
        list(APPEND C1_EXTRA_LIBS "${C1_TLS_PLUGIN}")
        message(STATUS "C1: packaging TLS backend ${C1_TLS_PLUGIN}")
    else()
        message(FATAL_ERROR "C1: 找不到 Qt TLS 后端插件（C1_TLS_PLUGIN='${C1_TLS_PLUGIN}'）；"
                            "没有它 HTTPS 一定失败（QNetworkAccessManager 报 TLS initialization failed）")
    endif()
endif()

if(C1_EXTRA_LIBS)
    set_target_properties(c1shell PROPERTIES
        QT_ANDROID_EXTRA_LIBS "${C1_EXTRA_LIBS}")
endif()

set_target_properties(c1shell PROPERTIES
    QT_ANDROID_PACKAGE_NAME "org.jmcomic.c1shell"
    QT_ANDROID_MIN_SDK_VERSION 34
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_VERSION_CODE 2
    QT_ANDROID_VERSION_NAME "0.2-c1"
)

target_link_libraries(c1shell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    log
)
