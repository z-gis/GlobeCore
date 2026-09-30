// napi_util.h —— NAPI 参数编解码公共工具（鸿蒙桥接层专用，header-only inline）。
//
// 类型映射口径（ArkTS ↔ C++，对齐 Android JNI 版 GlobeEngineJni.cpp 的数组语义）：
//   jlong 句柄        → number（int64，ArkTS 侧 < 2^53 安全）
//   jdoubleArray      → Float64Array（getCamera[8] / pick[2] / screenToGeo[2] / lonlat 摊平）
//   jintArray         → Int32Array（ARGB 像素、顶点计数、环计数）
//   jlongArray fids   → Float64Array（FID < 2^53 精确表示，免 BigInt 使用负担）
//   jobjectArray 字符串→ string[]（Array<string>）
//   null/undefined 参 → 可空语义与 JNI nullptr 一致
#ifndef GLOBECORE_OHOS_NAPI_UTIL_H
#define GLOBECORE_OHOS_NAPI_UTIL_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <napi/native_api.h>

namespace wwohos {

// ── 参数读取 ──

inline bool NapiGetDouble(napi_env env, napi_value v, double &out) {
    return napi_get_value_double(env, v, &out) == napi_ok;
}

inline bool NapiGetInt32(napi_env env, napi_value v, int32_t &out) {
    return napi_get_value_int32(env, v, &out) == napi_ok;
}

inline bool NapiGetInt64(napi_env env, napi_value v, int64_t &out) {
    return napi_get_value_int64(env, v, &out) == napi_ok;
}

inline bool NapiGetBool(napi_env env, napi_value v, bool &out) {
    return napi_get_value_bool(env, v, &out) == napi_ok;
}

/// string 参数（null/undefined 视为空串，对齐 JNI 侧 null→"" 口径）
inline std::string NapiGetString(napi_env env, napi_value v) {
    napi_valuetype type = napi_undefined;
    if (v == nullptr || napi_typeof(env, v, &type) != napi_ok || type != napi_string) return "";
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return "";
    std::vector<char> buf(len + 1);
    size_t written = 0;
    if (napi_get_value_string_utf8(env, v, buf.data(), buf.size(), &written) != napi_ok) return "";
    return std::string(buf.data(), written);
}

/// string[] 参数（null/非数组返回空；元素非 string 记空串，对齐 toStringVec 口径）
inline std::vector<std::string> NapiGetStringArray(napi_env env, napi_value v) {
    std::vector<std::string> out;
    bool isArray = false;
    if (v == nullptr || napi_is_array(env, v, &isArray) != napi_ok || !isArray) return out;
    uint32_t n = 0;
    napi_get_array_length(env, v, &n);
    out.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        napi_value item = nullptr;
        napi_get_element(env, v, i, &item);
        out.push_back(NapiGetString(env, item));
    }
    return out;
}

/// TypedArray 读取：Float64Array → vector<double>；Int32Array → 经 double 提升。
/// 传入 kind 期望的 napi_typedarray_type；不匹配/nullptr/长度为 0 返回空（对齐 null 数组语义）。
inline std::vector<double> NapiGetFloat64Array(napi_env env, napi_value v) {
    std::vector<double> out;
    if (v == nullptr) return out;
    napi_typedarray_type type = napi_biguint64_array;
    size_t length = 0;
    void *data = nullptr;
    napi_value buf = nullptr;
    size_t offset = 0;
    if (napi_get_typedarray_info(env, v, &type, &length, &data, &buf, &offset) != napi_ok) return out;
    if (type == napi_float64_array && data != nullptr && length > 0) {
        const double *p = static_cast<const double *>(data);
        out.assign(p, p + length);
    } else if (type == napi_int32_array && data != nullptr && length > 0) {
        const int32_t *p = static_cast<const int32_t *>(data);
        out.reserve(length);
        for (size_t i = 0; i < length; ++i) out.push_back(static_cast<double>(p[i]));
    }
    return out;
}

/// TypedArray 读取：Int32Array（ARGB 像素/计数数组）。类型不符返回空。
inline std::vector<int32_t> NapiGetInt32Array(napi_env env, napi_value v) {
    std::vector<int32_t> out;
    if (v == nullptr) return out;
    napi_typedarray_type type = napi_biguint64_array;
    size_t length = 0;
    void *data = nullptr;
    napi_value buf = nullptr;
    size_t offset = 0;
    if (napi_get_typedarray_info(env, v, &type, &length, &data, &buf, &offset) != napi_ok) return out;
    if (type == napi_int32_array && data != nullptr && length > 0) {
        const int32_t *p = static_cast<const int32_t *>(data);
        out.assign(p, p + length);
    }
    return out;
}

// ── 返回值构造 ──

inline napi_value NapiMakeDouble(napi_env env, double v) {
    napi_value out = nullptr;
    napi_create_double(env, v, &out);
    return out;
}

inline napi_value NapiMakeInt32(napi_env env, int32_t v) {
    napi_value out = nullptr;
    napi_create_int32(env, v, &out);
    return out;
}

inline napi_value NapiMakeInt64(napi_env env, int64_t v) {
    napi_value out = nullptr;
    napi_create_int64(env, v, &out); // ArkTS number 收 < 2^53 的句柄值
    return out;
}

inline napi_value NapiMakeBool(napi_env env, bool v) {
    napi_value out = nullptr;
    napi_get_boolean(env, v, &out);
    return out;
}

inline napi_value NapiMakeString(napi_env env, const std::string &s) {
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), s.size(), &out);
    return out;
}

inline napi_value NapiMakeNull(napi_env env) {
    napi_value out = nullptr;
    napi_get_null(env, &out);
    return out;
}

/// double[] → Float64Array（数据经 ArrayBuffer 拷贝一次；size==0 返回空数组对象）
inline napi_value NapiMakeFloat64Array(napi_env env, const double *data, size_t size) {
    void *bufData = nullptr;
    napi_value arraybuffer = nullptr;
    if (napi_create_arraybuffer(env, size * sizeof(double), &bufData, &arraybuffer) != napi_ok) {
        return NapiMakeNull(env);
    }
    if (size > 0 && data != nullptr) {
        memcpy(bufData, data, size * sizeof(double));
    }
    napi_value out = nullptr;
    napi_create_typedarray(env, napi_float64_array, size, arraybuffer, 0, &out);
    return out;
}

inline napi_value NapiMakeFloat64Array(napi_env env, const std::vector<double> &v) {
    return NapiMakeFloat64Array(env, v.data(), v.size());
}

/// string[] → Array<string>
inline napi_value NapiMakeStringArray(napi_env env, const std::vector<std::string> &v) {
    napi_value out = nullptr;
    napi_create_array_with_length(env, v.size(), &out);
    for (size_t i = 0; i < v.size(); ++i) {
        napi_set_element(env, out, static_cast<uint32_t>(i), NapiMakeString(env, v[i]));
    }
    return out;
}

/// 参数批量取出：args 预先 sized，超出实际 argc 的元素保持 nullptr
inline void NapiGetArgs(napi_env env, napi_callback_info info, size_t argc, napi_value *args) {
    size_t actual = argc;
    napi_get_cb_info(env, info, &actual, args, nullptr, nullptr);
    for (size_t i = actual; i < argc; ++i) args[i] = nullptr;
}

} // namespace wwohos

#endif // GLOBECORE_OHOS_NAPI_UTIL_H
