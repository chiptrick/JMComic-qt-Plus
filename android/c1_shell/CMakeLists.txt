# C1-W1：Qt 6.12 C++ 壳工程骨架（壳 + 导航占位；沿用 C0 的直链与 16KB 门禁）
# 验收：lib 门禁 0 未对齐 + bundle 门禁 N/A（无 Python）+ Android 17 实机启动；
# C0-③：libsr_qnn.so 以 IMPORTED 方式直链（去 ctypes），随 APK 打包并在设备上验证加载。
cmake_minimum_required(VERSION 3.22.1)
project(C1Shell VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

qt_add_executable(c1shell
    main.cpp
    reader.cpp
    settings_page.cpp
    python_bridge.cpp
    pyruntime.cpp
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

# --- C1-W6：Python 运行时（可选；缺了就是 W6 未完成，不会静默假装绿） ---
# 由 build_wsl.sh 从现有 p4a dist 里拷进来：libpythonbin.so + libpython3.14.so +
# sqlite/ffi/ssl/crypto + libpybundle.so(tar.gz 容器)。缺任何一个都不打包。
set(PY_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pylibs/${ANDROID_ABI}"
    CACHE PATH "Python 运行时库目录（由 build_wsl.sh 填充）")
set(PY_RUNTIME_LIBS
    "${PY_RUNTIME_DIR}/libpythonbin.so"
    "${PY_RUNTIME_DIR}/libpython3.14.so"
    "${PY_RUNTIME_DIR}/libsqlite3.so"
    "${PY_RUNTIME_DIR}/libffi.so"
    "${PY_RUNTIME_DIR}/libcrypto.so"
    "${PY_RUNTIME_DIR}/libssl.so")
set(PY_RUNTIME_OK ON)
foreach(lib IN LISTS PY_RUNTIME_LIBS)
    if(NOT EXISTS "${lib}")
        set(PY_RUNTIME_OK OFF)
        message(STATUS "C1-W6: Python runtime lib missing: ${lib}")
    endif()
endforeach()
if(PY_RUNTIME_OK AND EXISTS "${PY_RUNTIME_DIR}/libpybundle_fixed.so")
    # Python 运行时统一走 assets/（build_wsl.sh 塞进去），运行时复制到私有目录再启动；
    # 不进 QT_ANDROID_EXTRA_LIBS：extractNativeLibs=false 时 .so 不落盘、没法 exec。
    target_compile_definitions(c1shell PRIVATE C1_HAVE_PY_RUNTIME=1)
    message(STATUS "C1-W6: Python runtime staged in ${PY_RUNTIME_DIR}")
else()
    message(WARNING "C1-W6: Python runtime incomplete; APK will not contain Python subprocess runtime")
endif()

if(C1_EXTRA_LIBS)
    set_target_properties(c1shell PROPERTIES
        QT_ANDROID_EXTRA_LIBS "${C1_EXTRA_LIBS}")
endif()

set_target_properties(c1shell PROPERTIES
    QT_ANDROID_PACKAGE_NAME "org.jmcomic.c1shell"
    QT_ANDROID_MIN_SDK_VERSION 34
    QT_ANDROID_TARGET_SDK_VERSION 34
    QT_ANDROID_VERSION_CODE 1
    QT_ANDROID_VERSION_NAME "0.1-c1"
)

target_link_libraries(c1shell PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    log
)
