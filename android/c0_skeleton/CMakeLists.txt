# C0-②' + C0-③：Qt 6.12 C++ 空壳 —— 判定实验（§0A 更新 17）
# 验收：lib 门禁 0 未对齐 + bundle 门禁 N/A（无 Python）+ Android 17 实机启动；
# C0-③：libsr_qnn.so 以 IMPORTED 方式直链（去 ctypes），随 APK 打包并在设备上验证加载。
cmake_minimum_required(VERSION 3.22.1)
project(C0Skeleton VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(c0skeleton
    main.cpp
)

# --- C0-③：sr_qnn 直链 ---
# 库的位置默认取构建树里 repack 用的同一份（build_wsl.sh 会把源码 rsync 到
# $JM_WORK/src/android/c0_skeleton，../libs/<abi> 即 $JM_WORK/src/android/libs/<abi>），
# 与 wsl_repack_apk 打包 Python 应用用的是同一批 0x4000 对齐产物，避免两处副本漂移。
set(SRQ_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libs/${ANDROID_ABI}"
    CACHE PATH "预编译 sr_qnn / onnxruntime 库目录")
set(SRQ_LIB  "${SRQ_LIBS_DIR}/libsr_qnn.so")
set(ORT_LIB  "${SRQ_LIBS_DIR}/libonnxruntime.so")

if(EXISTS "${SRQ_LIB}" AND EXISTS "${ORT_LIB}")
    add_library(sr_qnn SHARED IMPORTED)
    set_target_properties(sr_qnn PROPERTIES IMPORTED_LOCATION "${SRQ_LIB}")
    add_library(onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime PROPERTIES IMPORTED_LOCATION "${ORT_LIB}")
    target_link_libraries(c0skeleton PRIVATE sr_qnn onnxruntime)
    # 让 Qt 的 CMake 部署把这两个非 Qt 依赖也打进 APK（部署器会跟踪 IMPORTED 依赖，
    # 但 EXTRA_LIBS 是显式兜底，避免版本差异漏包）
    set_target_properties(c0skeleton PROPERTIES
        QT_ANDROID_EXTRA_LIBS "${SRQ_LIB};${ORT_LIB}")
    message(STATUS "C0-③: linking ${SRQ_LIB}")
else()
    message(WARNING "C0-③: sr_qnn/onnxruntime 库不在 ${SRQ_LIBS_DIR}，本次构建不含直链验证")
endif()

set_target_properties(c0skeleton PROPERTIES
    QT_ANDROID_PACKAGE_NAME "org.jmcomic.c0skeleton"
    QT_ANDROID_MIN_SDK_VERSION 34
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_VERSION_CODE 1
    QT_ANDROID_VERSION_NAME "0.1"
)

target_link_libraries(c0skeleton PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    log
)
