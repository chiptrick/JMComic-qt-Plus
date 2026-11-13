// ONNX Runtime 错误码枚举。
//
// 官方发行包里 onnxruntime_c_api.h 会 include 这个文件，但 Android 侧的
// 头文件目录只带了 5 个主干头（缺这一个）。这里按 ORT 的公开 ABI 补齐：
// 只影响 OrtErrorCode 的取值，调用方只用 GetErrorMessage()（返回字符串），
// 不做错误码分支，因此与 DLL 的版本差异不影响行为。
#pragma once

typedef enum OrtErrorCode {
    ORT_OK = 0,
    ORT_FAIL = 1,
    ORT_INVALID_ARGUMENT = 2,
    ORT_NO_SUCHFILE = 3,
    ORT_NO_MODEL = 4,
    ORT_ENGINE_ERROR = 5,
    ORT_RUNTIME_EXCEPTION = 6,
    ORT_INVALID_PROTOBUF = 7,
    ORT_MODEL_LOADED = 8,
    ORT_NOT_IMPLEMENTED = 9,
    ORT_INVALID_GRAPH = 10,
    ORT_EP_FAIL = 11,
    ORT_MODEL_LOAD_CANCELED = 12,
    ORT_MODEL_REQUIRES_COMPILATION = 13,
    ORT_NOT_FOUND = 14,
} OrtErrorCode;
