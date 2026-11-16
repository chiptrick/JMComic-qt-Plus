# C++ UI 层（桌面）——独立于 android 的 c1shell 目标，便于在 Windows 上快速迭代界面。
#
# 复用：
#   * ../reader.cpp        C1-W2/W3 的阅读器（双页 / RTL / 亮度 / 缩放）
#   * res/theme/*.qss      Python 版同一套主题
# 不依赖：sr_qnn / onnxruntime / OpenSSL / JNI —— 那些属于设备侧 c1shell 目标。
cmake_minimum_required(VERSION 3.22)
# 需要 C：内嵌 libwebp 解码端是 C 源码（Qt 6.12 不带 qwebp 插件）
project(JmComicUi VERSION 0.95 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
# 本项目不使用 .ui 文件；关掉 AUTOUIC，否则 ui_*.h 会被当成 uic 产物去找同名 .ui
set(CMAKE_AUTOUIC OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Widgets Svg Network Quick QuickWidgets Concurrent
             ShaderTools)

qt_standard_project_setup()



qt_add_executable(jmcomic_ui
    # 头文件显式列入：AUTOMOC 只处理"目标源文件"及其同名头，
    # ui_pages.h 没有同名 .cpp（页面拆成 browse/library/misc 三个 TU），不列就没人 moc。
    theme.h
    ui_kit.h
    comic_view.h
    cover_cache.h
    page_transition.h
    touch.h
    app_font.h
    ../doh_client.h
    ../net_backend.h
    ../tls/tls_channel.h
    ../quic/quic_http.h
    ui_pages.h
    ui_main_window.h
    section_strip.h
    section_swipe.h
    motion.h
    cache_path.h
    app_paths.h
    data_migrator.h
    ui_prefs.h
    search_history.h
    sr_model_sheet.h
    sr_bench.h
    nas_store.h
    log_export.h
    main_desktop.cpp
    theme.cpp
    ui_kit.cpp
    section_strip.cpp
    section_swipe.cpp
    motion.cpp
    cache_path.cpp
    app_paths.cpp
    data_migrator.cpp
    ui_prefs.cpp
    search_history.cpp
    cover_cache.cpp
    cover_provider.cpp
    comic_view.cpp
    ui_pages_extra.cpp
    ui_pages_cloud.cpp
    ui_pages_nas.cpp
    nas_store.cpp
    log_export.cpp
    page_transition.cpp
    touch.cpp
    touch_menu.cpp
    app_font.cpp
    ui_main_window.cpp
    ui_pages_browse.cpp
    ui_pages_library.cpp
    ui_pages_misc.cpp
    sr_model_sheet.cpp
    sr_bench.cpp
    ../reader.cpp
    ../sr_engine.cpp
    ../sr_engine.h
    ../frame_probe.cpp
    ../jm_webp.cpp
    ../net_client.cpp
    ../node_http.cpp
    ../doh_client.cpp
    ../net_backend.cpp
    ../tls/openssl_tls.cpp
    ../quic/quic_http.cpp
    ../update_checker.cpp
    ../update_checker.h
    ../vk/vk_api.h
    ../vk/vk_device.h
    ../vk/vk_onnx.h
    ../vk/sr_graph.h
    ../vk/vk_api.cpp
    ../vk/vk_device.cpp
    ../vk/vk_onnx.cpp
    ../vk/sr_graph.cpp
    ../vk/sr_vk.h
    ../vk/sr_vk.cpp
    ../jm_account.cpp
    ../jm_account.h
    page_loader.cpp
    download_manager.cpp
    download_manager.h
    proxy_config.cpp
    node_tester.cpp
    node_bench_pool.cpp
    node_bench_pool.h
    proxy_page.cpp
    region_code.cpp
    region_code.h
    ui_log.cpp
    ../jm_api.cpp
    ../jm_crypto.cpp
    session.cpp
    session.h
)

target_include_directories(jmcomic_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/onnxruntime/include
)

# QShader（QtGui 内部头 rhi/qshader.h）：解析 qsb 容器取 SPIR-V。
# Qt 把这个头放在 include/QtGui/6.12.0/QtGui/rhi/ 下，公共 include 路径里没有，
# 需要显式补 QtGui 的版本化私有 include 目录（只用公开的 QShader API）。
target_include_directories(jmcomic_ui PRIVATE
    "${Qt6_DIR}/../../../include/QtGui/6.12.0"
    "${Qt6_DIR}/../../../include/QtGui/6.12.0/QtGui")

# ---------------------------------------------------------------- Vulkan 计算后端
# 头文件：桌面用随仓的 Khronos headers（没有 Vulkan SDK 也能编），Android 用 NDK 自带。
# 入口全部经 loader 动态解析（VK_NO_PROTOTYPES），运行期缺 Vulkan 时只报原因不崩。
if (ANDROID)
    target_compile_definitions(jmcomic_ui PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1 VK_NO_PROTOTYPES=1)
else()
    target_include_directories(jmcomic_ui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/vulkan/include)
    target_compile_definitions(jmcomic_ui PRIVATE VK_NO_PROTOTYPES=1)
endif()
if (ANDROID)
    message(STATUS "UI: Vulkan compute backend enabled (headers from NDK sysroot)")
else()
    message(STATUS "UI: Vulkan compute backend enabled (3rdparty/vulkan/include)")
endif()

# 着色器：构建期用 Qt 的 qsb 编成 .qsb（内含 SPIR-V），打进 :/vkshader/<name>.qsb。
# 不用 qt6_add_shaders：它默认为 Windows 追加 --hlsl 50，而 HLSL 的 cbuffer 打包规则
# 无法表达 int p[16] 这种 push constant（实测 "cannot be expressed with either HLSL
# packing layout"）。这里只要 SPIR-V，直接调 qsb --glsl 450，参数完全可控。
# 每个 kernel 生成两份：<name>.qsb（fp32 存储）与 <name>_fp16.qsb（-D USE_FP16，
# SSBO 存 half、累加 fp32 的 NCNN fp16_packing 模式），运行期按设备能力选择。
get_filename_component(_qt_bin_dir "${Qt6_DIR}/../../../bin" ABSOLUTE)
find_program(JM_QSB NAMES qsb qsb.exe HINTS "${_qt_bin_dir}" REQUIRED)
set(JM_VK_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vkshaders")
file(MAKE_DIRECTORY "${JM_VK_SHADER_DIR}")
set(JM_VK_QSB_FILES "")
foreach(_sh probe pcdump conv convw deconv act eltwise copy d2s pad reduce resize)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/../vk/shaders/${_sh}.comp")
    if (EXISTS "${_src}")
        set(_out "${JM_VK_SHADER_DIR}/${_sh}.qsb")
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND "${JM_QSB}" --glsl 450 -o "${_out}" "${_src}"
            DEPENDS "${_src}"
            COMMENT "qsb ${_sh}.comp -> ${_sh}.qsb"
            VERBATIM)
        list(APPEND JM_VK_QSB_FILES "${_out}")
        set(_out16 "${JM_VK_SHADER_DIR}/${_sh}_fp16.qsb")
        add_custom_command(
            OUTPUT "${_out16}"
            COMMAND "${JM_QSB}" --glsl 450 -D USE_FP16=1 -o "${_out16}" "${_src}"
            DEPENDS "${_src}"
            COMMENT "qsb ${_sh}.comp -> ${_sh}_fp16.qsb"
            VERBATIM)
        list(APPEND JM_VK_QSB_FILES "${_out16}")
    endif()
endforeach()
if (JM_VK_QSB_FILES)
    set_source_files_properties(${JM_VK_QSB_FILES} PROPERTIES GENERATED TRUE)
    qt_add_resources(jmcomic_ui "vkshaders"
        PREFIX "/vkshader"
        BASE "${JM_VK_SHADER_DIR}"
        FILES ${JM_VK_QSB_FILES})
    message(STATUS "UI: Vulkan shaders -> ${JM_VK_SHADER_DIR} (${JM_QSB})")
endif()

# WebP 解码：Qt 6.12 的安装包不带 qwebp 插件，而 JM 的封面/内页全是 webp。
# 复用 C1 壳里的 libwebp 解码端（只编 dec/dsp/utils，无编码器）。
set(JM_WEBP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/libwebp")
file(GLOB JM_WEBP_SOURCES
    "${JM_WEBP_DIR}/src/dec/*.c"
    "${JM_WEBP_DIR}/src/dsp/*.c"
    "${JM_WEBP_DIR}/src/utils/*.c")
add_library(jmwebp STATIC ${JM_WEBP_SOURCES})
target_include_directories(jmwebp PUBLIC "${JM_WEBP_DIR}" "${JM_WEBP_DIR}/src")
if (NOT MSVC)
    target_compile_options(jmwebp PRIVATE -w)
endif()

target_link_libraries(jmcomic_ui PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::Network
    Qt6::Quick Qt6::QuickWidgets Qt6::Concurrent jmwebp)
# 性能测试的进程内存口径（FootprintMb 的 GetProcessMemoryInfo）
if (WIN32)
    target_link_libraries(jmcomic_ui PRIVATE psapi)
endif()

# --- 超分（可选，运行时加载）------------------------------------------------
# 只拷贝 DLL/模型，不做链接依赖：缺它们时 SrEngine 返回"不可用 + 原因"，
# 阅读器其它功能完全不受影响。
#
# 模型不再只拷一个：SrEngine::Catalog() 与 android/sr_qnn/models/models.txt
# 一一对应（39 个 onnx，fp32）。桌面拷到 <exe>/sr_models/；Android 拷到
# <build>/android-build/assets/sr_models/（gradle 的 assets.srcDirs=['assets']），
# 运行时用 assets:/sr_models/... 读进内存后 CreateSessionFromArray。
# _q16 是给 QNN NPU 用的量化模型，CPU EP 不用，不打进包（省 80MB）。
set(JM_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/onnxruntime/win-x64"
    CACHE PATH "onnxruntime.dll 所在目录（Windows）")
set(JM_SR_MODELS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../../python/android/sr_qnn/models"
    CACHE PATH "随包的超分模型目录（*.onnx）")

file(GLOB JM_SR_MODEL_FILES CONFIGURE_DEPENDS "${JM_SR_MODELS_SRC}/*.onnx")
# _q16 / _q16_lowq 是给 QNN NPU 的量化模型，CPU EP 不用；排除所有变体
list(FILTER JM_SR_MODEL_FILES EXCLUDE REGEX "_q16")
if (JM_SR_MODEL_FILES)
    list(SORT JM_SR_MODEL_FILES)
endif()

if (ANDROID AND JM_SR_MODEL_FILES)
    set(_sr_dest "${CMAKE_BINARY_DIR}/android-build/assets/sr_models")
    set(_sr_stamp "${CMAKE_BINARY_DIR}/sr_models.stamp")
    # 一条命令拷全部（cmake -E copy 支持多源 + 目标目录）；stamp 只在模型文件
    # 变化时失效，增量构建不会每次搬 260MB。
    add_custom_command(
        OUTPUT "${_sr_stamp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_sr_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy ${JM_SR_MODEL_FILES} "${_sr_dest}"
        COMMAND ${CMAKE_COMMAND} -E touch "${_sr_stamp}"
        DEPENDS ${JM_SR_MODEL_FILES}
        COMMENT "stage sr_models for ${ANDROID_ABI}")
    add_custom_target(jm_sr_models ALL DEPENDS "${_sr_stamp}")
    add_dependencies(jmcomic_ui jm_sr_models)
    message(STATUS "UI: staging sr_models (${ANDROID_ABI}) -> ${_sr_dest}")
endif()

if(WIN32 AND EXISTS "${JM_ORT_DIR}/onnxruntime.dll")
    add_custom_command(TARGET jmcomic_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${JM_ORT_DIR}/onnxruntime.dll" "$<TARGET_FILE_DIR:jmcomic_ui>/onnxruntime.dll"
        COMMENT "copy onnxruntime.dll next to jmcomic_ui")
endif()
if(WIN32 AND JM_SR_MODEL_FILES)
    add_custom_command(TARGET jmcomic_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:jmcomic_ui>/sr_models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JM_SR_MODEL_FILES}
                "$<TARGET_FILE_DIR:jmcomic_ui>/sr_models"
        COMMENT "copy sr_models next to jmcomic_ui")
endif()

# Intel OpenVINO 插件 EP：默认不打包（整套 runtime ~230MB，且只有 Core Ultra
# 机器用得上）。给出目录时才随包 staging 到 ovep/；SrEngine 运行时按
# JM_OV_EP_DIR / JM_OV_EP_DLL 或 <程序目录>/ovep/ 查找，缺插件就自动回退。
set(JM_OVEP_DIR "" CACHE PATH "OpenVINO EP 插件运行时目录（plugin DLL + OpenVINO/TBB DLL + manifest）")
if(WIN32 AND JM_OVEP_DIR AND EXISTS "${JM_OVEP_DIR}/onnxruntime_providers_openvino_plugin.dll")
    file(GLOB _ovep_runtime CONFIGURE_DEPENDS
         "${JM_OVEP_DIR}/*.dll" "${JM_OVEP_DIR}/*.manifest" "${JM_OVEP_DIR}/cache.json")
    add_custom_command(TARGET jmcomic_ui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:jmcomic_ui>/ovep"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_ovep_runtime}
                "$<TARGET_FILE_DIR:jmcomic_ui>/ovep"
        COMMENT "stage OpenVINO EP plugin runtime into ovep/")
    message(STATUS "UI: packaging OpenVINO EP runtime from ${JM_OVEP_DIR}")
endif()

# 注意：这里不能写 `FILES assets.qrc` —— qt_add_resources 会把 .qrc 当成"普通文件"
# 塞进资源里（二进制里能看到 qrc 的 XML 原文），里面的条目一条都不会注册，
# 表现是 QPixmap(":/ui/assets/...") 永远为空。直接列文件 + BASE 才可靠。
qt_add_resources(jmcomic_ui "ui_assets"
    PREFIX "/ui/assets"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/icon"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/icon/placeholder_avatar.png"
)

# CA bundle（可选）：Android 没有 OpenSSL 认得的默认 CA 文件，而系统目录
# （/apex/.../cacerts）在部分镜像上不可读。构建脚本会把宿主的 CA bundle 拷到
# <build>/generated/cacert.pem，这里把它编进资源，运行时用 :/tls/cacert.pem 加载。
set(JM_CA_BUNDLE_FILE "${CMAKE_BINARY_DIR}/generated/cacert.pem")
if (EXISTS "${JM_CA_BUNDLE_FILE}")
    qt_add_resources(jmcomic_ui "tls_ca"
        PREFIX "/tls"
        BASE "${CMAKE_BINARY_DIR}/generated"
        FILES "${JM_CA_BUNDLE_FILE}")
    message(STATUS "UI: embedding CA bundle (${JM_CA_BUNDLE_FILE})")
endif()

# reader.cpp 同时被 c1shell（没有 UI 层）复用，所以动效层只能是可选依赖：
# 定义这个宏之后 reader.cpp 才会 include motion.h 并启用视口过渡。
target_compile_definitions(jmcomic_ui PRIVATE JM_UI_MOTION=1)

# 换页动画场景（QML）。BASE 让资源路径是 :/qml/PageSwipe.qml，不带源目录前缀。
qt_add_resources(jmcomic_ui "qml_scene"
    PREFIX "/qml"
    BASE qml
    FILES qml/PageSwipe.qml
          qml/PageCards.qml
)

qt_add_resources(jmcomic_ui "themes"
    PREFIX "/theme"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme"
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/dark_teal.qss"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/dark_pink.qss"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/dark_orange.qss"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/light_teal.qss"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/light_pink.qss"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../assets/theme/light_orange.qss"
)

if (MSVC)
    target_compile_options(jmcomic_ui PRIVATE /utf-8)
endif()

if (WIN32)
    set_target_properties(jmcomic_ui PROPERTIES WIN32_EXECUTABLE OFF)
endif()

# ---------------------------------------------------------------- QUIC / HTTP3
# 桌面：OpenSSL 4.x（QUIC-TLS API + ECH）+ ngtcp2 + nghttp3，静态链接 .a，
#       运行时用 OpenSSL 的 DLL。
# Android：需要按 ABI 交叉编译的产物（cpp/shell/quic/tools/build_quic_deps_android.sh）。
#       JM_QUIC_ROOT / JM_OPENSSL41_ROOT 必须指向 **per-ABI** 目录，例如
#         deps/quic-android/arm64-v8a  与  deps/openssl41-android/arm64-v8a
#
# 三态开关，默认 AUTO：
#   AUTO —— 上述两个目录里三件静态库 + OpenSSL 的 libssl 齐全就自动打开。
#           手机端因此默认就带 ECH 与 HTTP/3，不再依赖"记得加 -WithQuic/--with-quic"。
#   ON   —— 强制打开；缺依赖或符号不全直接 FATAL，绝不产出"能编译但功能没开"的包。
#   OFF  —— 关闭；quic_http.cpp 编译成 Available()==false 的桩，行为回到引入 QUIC 之前。
#
# 注意：AUTO 只是"文件在不在"，真正的能力由下面的符号自检决定 —— no-ech 的
# OpenSSL 与没编 ossl 后端的 ngtcp2 都能链接成功，却会在运行期静默降级。
set(JM_WITH_QUIC "AUTO" CACHE STRING "Build HTTP/3 over QUIC: AUTO | ON | OFF")
set_property(CACHE JM_WITH_QUIC PROPERTY STRINGS AUTO ON OFF)
set(JM_QUIC_ROOT "" CACHE PATH "ngtcp2/nghttp3 install prefix (include + lib)")
set(JM_OPENSSL41_ROOT "" CACHE PATH "OpenSSL 4.x root (DLL + import libs on desktop, lib/*.so on Android)")

# 平台相关的库文件名：Android 是无版本号的 .so，桌面是 MinGW 导入库 .dll.a。
if (ANDROID)
    set(JM_OPENSSL41_SHARED "${JM_OPENSSL41_ROOT}/lib/libssl.so")
    set(JM_OPENSSL41_CRYPTO "${JM_OPENSSL41_ROOT}/lib/libcrypto.so")
else()
    set(JM_OPENSSL41_SHARED "${JM_OPENSSL41_ROOT}/lib/libssl.dll.a")
    set(JM_OPENSSL41_CRYPTO "${JM_OPENSSL41_ROOT}/lib/libcrypto.dll.a")
endif()

set(JM_QUIC_DEPS_READY FALSE)
if (JM_QUIC_ROOT AND JM_OPENSSL41_ROOT
    AND EXISTS "${JM_QUIC_ROOT}/lib/libngtcp2.a"
    AND EXISTS "${JM_QUIC_ROOT}/lib/libngtcp2_crypto_ossl.a"
    AND EXISTS "${JM_QUIC_ROOT}/lib/libnghttp3.a"
    AND EXISTS "${JM_OPENSSL41_SHARED}")
    set(JM_QUIC_DEPS_READY TRUE)
endif()

if (JM_WITH_QUIC STREQUAL "AUTO")
    set(_jm_quic "${JM_QUIC_DEPS_READY}")
elseif (JM_WITH_QUIC)
    set(_jm_quic TRUE)
else()
    set(_jm_quic FALSE)
endif()

if (_jm_quic AND NOT JM_QUIC_DEPS_READY)
    message(FATAL_ERROR
        "JM_WITH_QUIC=${JM_WITH_QUIC}，但依赖不完整：\n"
        "  JM_QUIC_ROOT      = ${JM_QUIC_ROOT}\n"
        "  JM_OPENSSL41_ROOT = ${JM_OPENSSL41_ROOT}\n"
        "  需要 libngtcp2.a / libngtcp2_crypto_ossl.a / libnghttp3.a 与 ${JM_OPENSSL41_SHARED}。\n"
        "  Android 先跑 cpp/shell/quic/tools/build_quic_deps_android.sh <abi>，"
        "再把上面两个变量指到 per-ABI 目录。")
endif()

if (NOT _jm_quic)
    if (ANDROID)
        message(WARNING
            "UI: QUIC/HTTP3 与 ECH 未启用（Android ${ANDROID_ABI}）。"
            "APK 里不会有 ngtcp2/nghttp3，OpenSSL 也只能退到 3.x（no-ech），"
            "设置页会显示 \"ECH 开·后端不支持\" / \"HTTP3 开·后端不支持\"。\n"
            "    需要这两项能力时：cpp/shell/quic/tools/build_quic_deps_android.sh ${ANDROID_ABI}，"
            "并让 JM_QUIC_ROOT/JM_OPENSSL41_ROOT 指向 "
            ".../quic-android/${ANDROID_ABI} 与 .../openssl41-android/${ANDROID_ABI}。")
    else()
        message(STATUS "UI: QUIC/HTTP3 disabled (JM_WITH_QUIC=${JM_WITH_QUIC})")
    endif()
endif()

# ---- 依赖自检：符号表比版本号更能说明问题 --------------------------------
if (_jm_quic)
    # NDK 的 llvm-nm 与 clang 包装器同目录；桌面取 PATH 里的 llvm-nm/nm。
    set(JM_NM "")
    get_filename_component(_jm_toolbin "${CMAKE_C_COMPILER}" DIRECTORY)
    foreach(_cand "${_jm_toolbin}/llvm-nm.exe" "${_jm_toolbin}/llvm-nm")
        if (EXISTS "${_cand}")
            set(JM_NM "${_cand}")
            break()
        endif()
    endforeach()
    if (NOT JM_NM)
        find_program(JM_NM NAMES llvm-nm nm)
    endif()

    # mode = -D（共享库动态符号表）或 -g（静态库全部符号）
    function(jm_nm_has out lib mode sym)
        if (JM_NM)
            execute_process(COMMAND "${JM_NM}" ${mode} --defined-only "${lib}"
                            OUTPUT_VARIABLE _out ERROR_QUIET RESULT_VARIABLE _rc)
            if (NOT _rc EQUAL 0)
                set(${out} FALSE PARENT_SCOPE)
                return()
            endif()
            string(FIND "${_out}" "${sym}" _pos)
        else()
            # 没有 nm 时退化为字符串匹配（符号名就在 .dynstr/.strtab 里）
            file(STRINGS "${lib}" _hit REGEX "${sym}")
            if (_hit)
                set(_pos 0)
            else()
                set(_pos -1)
            endif()
        endif()
        if (_pos EQUAL -1)
            set(${out} FALSE PARENT_SCOPE)
        else()
            set(${out} TRUE PARENT_SCOPE)
        endif()
    endfunction()

    set(_jm_missing "")
    foreach(_sym SSL_set1_ech_config_list SSL_ech_set1_server_names
                 SSL_set_quic_tls_cbs SSL_set_quic_tls_early_data_enabled)
        jm_nm_has(_has "${JM_OPENSSL41_SHARED}" -D "${_sym}")
        if (NOT _has)
            list(APPEND _jm_missing "${_sym}")
        endif()
    endforeach()
    jm_nm_has(_has_ossl "${JM_QUIC_ROOT}/lib/libngtcp2_crypto_ossl.a" -g
              ngtcp2_crypto_ossl_configure_client_session)
    if (NOT _has_ossl)
        list(APPEND _jm_missing "ngtcp2_crypto_ossl_configure_client_session")
    endif()
    if (_jm_missing)
        string(REPLACE ";" "\n    " _jm_missing_text "${_jm_missing}")
        message(FATAL_ERROR
            "QUIC/ECH 依赖自检失败，以下符号缺失：\n    ${_jm_missing_text}\n"
            "  OpenSSL: ${JM_OPENSSL41_SHARED}\n"
            "           必须是 >= 3.5 且未带 no-ech / no-quic / no-bulk 的构建。\n"
            "  ngtcp2 : ${JM_QUIC_ROOT}/lib/libngtcp2_crypto_ossl.a\n"
            "           必须是 -DENABLE_OPENSSL=ON（探测到 SSL_set_quic_tls_cbs）的产物。\n"
            "  Android 重新跑：cpp/shell/quic/tools/build_quic_deps_android.sh <abi>")
    endif()

    # 版本下限：符号已能证明能力，这里只把"太旧"提前说清楚。
    file(STRINGS "${JM_OPENSSL41_ROOT}/include/openssl/opensslv.h" _jm_ossl_line
         REGEX "OPENSSL_VERSION_STR")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _jm_ossl_ver "${_jm_ossl_line}")
    file(STRINGS "${JM_QUIC_ROOT}/include/ngtcp2/version.h" _jm_tcp2_line
         REGEX "^#define NGTCP2_VERSION ")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _jm_tcp2_ver "${_jm_tcp2_line}")
    file(STRINGS "${JM_QUIC_ROOT}/include/nghttp3/version.h" _jm_h3_line
         REGEX "^#define NGHTTP3_VERSION ")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" _jm_h3_ver "${_jm_h3_line}")
    if (NOT _jm_ossl_ver OR NOT _jm_tcp2_ver OR NOT _jm_h3_ver)
        message(FATAL_ERROR
            "QUIC 依赖头文件不完整：需要 ${JM_OPENSSL41_ROOT}/include/openssl/opensslv.h、"
            "${JM_QUIC_ROOT}/include/ngtcp2/version.h、${JM_QUIC_ROOT}/include/nghttp3/version.h")
    endif()
    if (_jm_ossl_ver VERSION_LESS "3.5.0")
        message(FATAL_ERROR "OpenSSL ${_jm_ossl_ver} 太旧：SSL_set_quic_tls_cbs 要求 >= 3.5.0")
    endif()
    if (_jm_tcp2_ver VERSION_LESS "1.0.0")
        message(FATAL_ERROR "ngtcp2 ${_jm_tcp2_ver} 太旧：QUIC-TLS 回调接口要求 >= 1.0.0")
    endif()
    if (_jm_h3_ver VERSION_LESS "1.0.0")
        message(FATAL_ERROR "nghttp3 ${_jm_h3_ver} 太旧：HTTP/3 客户端要求 >= 1.0.0")
    endif()
    message(STATUS "UI: QUIC deps OK — OpenSSL ${_jm_ossl_ver}, "
                   "ngtcp2 ${_jm_tcp2_ver}, nghttp3 ${_jm_h3_ver}")
endif()

if (_jm_quic)
    target_compile_definitions(jmcomic_ui PRIVATE
        JM_WITH_QUIC=1
        NGTCP2_STATICLIB=1
        NGHTTP3_STATICLIB=1)

    target_include_directories(jmcomic_ui PRIVATE
        "${JM_QUIC_ROOT}/include"
        "${JM_OPENSSL41_ROOT}/include")

    set(JM_QUIC_STATIC_LIBS
        "${JM_QUIC_ROOT}/lib/libngtcp2.a"
        "${JM_QUIC_ROOT}/lib/libngtcp2_crypto_ossl.a"
        "${JM_QUIC_ROOT}/lib/libnghttp3.a")

    if (ANDROID)
        # Android 用 bionic 的 log 库；没有 ws2_32。OpenSSL 是 .so，必须随 APK 打包。
        target_link_libraries(jmcomic_ui PRIVATE
            ${JM_QUIC_STATIC_LIBS}
            "${JM_OPENSSL41_SHARED}"
            "${JM_OPENSSL41_CRYPTO}"
            log)

        # staging 必须与 QUIC 开关一致：Qt 的 TLS 插件在 Android 上 dlopen
        # libssl_3.so / libcrypto_3.so，QUIC 开启时这两个别名也要指向 4.x。
        # 否则 3.x 的 libssl_3 会把 DT_NEEDED libcrypto.so 绑到 4.x 上，加载期
        # 直接缺符号（实测 cannot locate "X509_NAME_free"）。两套 OpenSSL 不能
        # 同包，因此无版本号的 libssl.so/libcrypto.so 只保留 4.x 这一套。
        # 直接 ninja（不经 build_android.ps1/.sh）也由此保证正确。
        set(_jm_stage "${CMAKE_CURRENT_BINARY_DIR}/staging")
        file(MAKE_DIRECTORY "${_jm_stage}")
        file(COPY_FILE "${JM_OPENSSL41_SHARED}" "${_jm_stage}/libssl_3.so")
        file(COPY_FILE "${JM_OPENSSL41_CRYPTO}" "${_jm_stage}/libcrypto_3.so")
        file(REMOVE "${_jm_stage}/libssl.so" "${_jm_stage}/libcrypto.so")
        get_filename_component(_jm_qt_kit "${Qt6_DIR}/../../.." ABSOLUTE)
        set(_jm_tls_plugin
            "${_jm_qt_kit}/plugins/tls/libplugins_tls_qopensslbackend_${ANDROID_ABI}.so")
        if (NOT EXISTS "${_jm_tls_plugin}")
            message(FATAL_ERROR "Qt TLS plugin not found: ${_jm_tls_plugin}")
        endif()
        # 覆盖脚本传进来的列表：内容一致（脚本在 QUIC 开启时同样从 4.x stage），
        # 但能把旧的非 QUIC 构建缓存里的 3.x staging 路径清干净。
        set(UI_ANDROID_EXTRA_LIBS
            "${_jm_stage}/libssl_3.so;${_jm_stage}/libcrypto_3.so;${_jm_tls_plugin};${JM_OPENSSL41_SHARED};${JM_OPENSSL41_CRYPTO}"
            CACHE STRING "androiddeployqt extra .so files (semicolon separated)" FORCE)
        if (NOT EXISTS "${CMAKE_BINARY_DIR}/generated/cacert.pem")
            message(WARNING
                "Android QUIC: <build>/generated/cacert.pem 不存在 —— QUIC 握手会因 "
                "verify=20（无信任锚）失败。构建脚本应先把宿主 CA bundle 拷到那里。")
        endif()
        message(STATUS "UI: HTTP/3 over QUIC + ECH enabled for Android ${ANDROID_ABI} (${JM_QUIC_ROOT})")
    else()
        target_link_libraries(jmcomic_ui PRIVATE
            ${JM_QUIC_STATIC_LIBS}
            "${JM_OPENSSL41_SHARED}"
            "${JM_OPENSSL41_CRYPTO}"
            ws2_32)
        message(STATUS "UI: HTTP/3 over QUIC + ECH enabled (${JM_QUIC_ROOT})")

        # 运行时依赖：OpenSSL 4.x 的两个 DLL 必须与 exe 同目录
        add_custom_command(TARGET jmcomic_ui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${JM_OPENSSL41_ROOT}/bin/libssl-4-x64.dll"
                "${JM_OPENSSL41_ROOT}/bin/libcrypto-4-x64.dll"
                "$<TARGET_FILE_DIR:jmcomic_ui>"
            COMMENT "Copying OpenSSL 4.x runtime DLLs")
    endif()
endif()

# ---------------------------------------------------------------- Android 打包
# 桌面端只用 Windows 套件；移动端用 Qt android_<abi> 交叉编译同一个目标。
# 与 c1shell 的差别：这个壳不含 sr_qnn / JNI，只需要 Qt 库 + TLS 后端就能装。
if (ANDROID)
    # HTTPS：Qt 的 TLS 后端是插件，androiddeployqt 不会因为链了 Qt6::Network 就带上；
    # 同时 QNetworkAccessManager 运行期 dlopen libssl/libcrypto。构建脚本把三件事
    # 一起塞进 UI_ANDROID_EXTRA_LIBS（插件的 .so + libssl/libcrypto 的 _3 别名副本）。
    set(UI_ANDROID_EXTRA_LIBS "" CACHE STRING "androiddeployqt 额外打包的 .so（分号分隔）")

    # versionCode 必须单调递增：回退会 INSTALL_FAILED_VERSION_DOWNGRADE，逼用户
    # 卸载重装（= 清空 /data/data/<pkg>，登录态一起蒸发）。由 project VERSION 推导
    # （0.96 -> 196，0.100 -> 1100），不要再手写数字。
    math(EXPR JM_ANDROID_VERSION_CODE
         "${PROJECT_VERSION_MAJOR} * 100 + ${PROJECT_VERSION_MINOR}")
    set_target_properties(jmcomic_ui PROPERTIES
        QT_ANDROID_PACKAGE_NAME "org.jmcomic.ui"
        QT_ANDROID_MIN_SDK_VERSION 34
        QT_ANDROID_TARGET_SDK_VERSION 34
        QT_ANDROID_VERSION_CODE ${JM_ANDROID_VERSION_CODE}
        QT_ANDROID_VERSION_NAME "${PROJECT_VERSION}"
    )
    if (UI_ANDROID_EXTRA_LIBS)
        set_target_properties(jmcomic_ui PROPERTIES
            QT_ANDROID_EXTRA_LIBS "${UI_ANDROID_EXTRA_LIBS}")
        message(STATUS "UI: packaging extra libs ${UI_ANDROID_EXTRA_LIBS}")
    endif()

    # ONNX Runtime（超分推理）：随包进 lib/<abi>/。SrEngine 用 QLibrary("libonnxruntime.so")
    # 裸名加载，靠 nativeLibraryDir 的 DT_RUNPATH 命中；不做链接依赖。
    set(JM_ORT_ANDROID_DIR "" CACHE PATH "Android libonnxruntime.so 所在目录")
    if (EXISTS "${JM_ORT_ANDROID_DIR}/libonnxruntime.so")
        set_property(TARGET jmcomic_ui APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
            "${JM_ORT_ANDROID_DIR}/libonnxruntime.so")
        message(STATUS "UI: packaging onnxruntime ${JM_ORT_ANDROID_DIR}/libonnxruntime.so")
    else()
        message(WARNING
            "UI: 没有 Android libonnxruntime.so（-DJM_ORT_ANDROID_DIR=...），"
            "这一版 APK 的超分不可用。")
    endif()

    # 手机上没有窗口管理器给你拖拽/最大化：标题栏里的最小化/最大化按钮在
    # Android 上没有意义，首次上机先保持原样跑通，再决定是否按平台隐藏。
endif()
