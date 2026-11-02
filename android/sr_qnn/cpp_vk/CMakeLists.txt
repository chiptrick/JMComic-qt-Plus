# libsr_vk.so：非高通机型的 Vulkan GPU 超分后端
#
# 由 android/tools/build_sr_vk.sh 调用；需要先跑 build_ncnn_android.sh 产出静态库：
#   NCNN_ROOT=<ncnn 源码>  NCNN_BUILD=<构建目录>  （默认 ~/jmcomic-build/ncnn-build-<abi>）
cmake_minimum_required(VERSION 3.22)
project(sr_vk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED NCNN_BUILD)
    message(FATAL_ERROR "需要 -DNCNN_BUILD=<ncnn 构建目录>（跑 android/tools/build_ncnn_android.sh 生成）")
endif()
if(NOT DEFINED NCNN_SRC)
    message(FATAL_ERROR "需要 -DNCNN_SRC=<ncnn 源码目录>（头文件在 <src>/src/ncnn 下）")
endif()
set(NCNN_LIB "${NCNN_BUILD}/src/libncnn.a")

add_library(sr_vk SHARED sr_vk.cpp)

target_include_directories(sr_vk PRIVATE
    "${NCNN_SRC}/src"     # ncnn/net.h、ncnn/gpu.h
    "${NCNN_BUILD}/src"   # 构建期生成的 layer_registry 等头
)

# ncnn(Vulkan) 的静态依赖：ncnn + glslang + SPIRV + 系统 Vulkan / OpenMP / log
find_library(ANDROID_LOG log REQUIRED)
find_library(ANDROID_VULKAN vulkan REQUIRED)

# glslang/ncnn/SPIRV 三个静态库互相引用，必须包在 --start-group 里（否则 ld 只扫一遍就报未定义）
target_link_libraries(sr_vk PRIVATE
    -Wl,--start-group
    "${NCNN_LIB}"
    "${NCNN_BUILD}/glslang/glslang/libglslang.a"
    "${NCNN_BUILD}/glslang/glslang/libglslang-default-resource-limits.a"
    "${NCNN_BUILD}/glslang/glslang/libMachineIndependent.a"
    "${NCNN_BUILD}/glslang/glslang/libGenericCodeGen.a"
    "${NCNN_BUILD}/glslang/glslang/OSDependent/Unix/libOSDependent.a"
    "${NCNN_BUILD}/glslang/OGLCompilersDLL/libOGLCompiler.a"
    "${NCNN_BUILD}/glslang/SPIRV/libSPIRV.a"
    -Wl,--end-group
    "${ANDROID_VULKAN}"
    "${ANDROID_LOG}"
    dl
    m
    atomic
)
# ncnn 用 NCNN_OPENMP=ON 编的，符号是 __kmpc_*（clang 的 libomp），不是 glibc 的 libgomp。
# 用 -fopenmp 让 NDK clang 自己挂 libomp（NDK 里 libomp.a 在 clang/<ver>/lib/linux/<arch>/）。
target_compile_options(sr_vk PRIVATE -fopenmp)
target_link_options(sr_vk PRIVATE -fopenmp)

set_target_properties(sr_vk PROPERTIES
    OUTPUT_NAME "sr_vk"
    POSITION_INDEPENDENT_CODE ON
    LINK_FLAGS "-Wl,-z,max-page-size=16384"      # 与 sr_qnn 一致：16KB 页对齐
)

# 设备端探针：dlopen libsr_vk.so 并真调 srq_init，证明 Vulkan 后端可用
add_executable(sr_vk_probe probe.cpp)
target_link_libraries(sr_vk_probe PRIVATE dl log)
set_target_properties(sr_vk_probe PROPERTIES POSITION_INDEPENDENT_CODE ON)
