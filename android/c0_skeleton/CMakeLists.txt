# C0-②'：Qt 6.12 C++ 空壳 —— 判定实验（§0A 更新 17）
# 验收：lib 门禁 0 未对齐 + bundle 门禁 N/A（无 Python）+ Android 17 实机启动。
cmake_minimum_required(VERSION 3.22.1)
project(C0Skeleton VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(c0skeleton
    main.cpp
)

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
)
