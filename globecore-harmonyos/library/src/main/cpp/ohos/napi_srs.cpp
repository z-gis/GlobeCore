// napi_srs.cpp —— PROJ 坐标转换 NAPI 导出（对应 Android bridge/srs.cpp 的 JNI 段）。
//
// 计算核心在 wwbridge（bridge_api.h），本文件仅做 napi_value ↔ std 类型薄封装。
// 导出命名统一 native 前缀（鸿蒙单模块 libglobecore，与 GlobeEngine 导出同表命名风格）：
//   nativeGetProjVersion() → number
//   nativeInitProjDataPath(path: string) → void
//   nativeConvert(x, y, srcCrs, tgtCrs) → Float64Array[2] | null（null = 转换失败，对齐 JNI null）
#include <napi/native_api.h>

#include <string>

#include "bridge/bridge_api.h"
#include "napi_util.h"

namespace {

napi_value NativeGetProjVersion(napi_env env, napi_callback_info /*info*/) {
    return wwohos::NapiMakeInt32(env, wwbridge::projVersion());
}

napi_value NativeInitProjDataPath(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return nullptr; // 对齐 JNI null jstring 不处理口径
    wwbridge::initProjDataPath(path);
    return nullptr;
}

napi_value NativeConvert(napi_env env, napi_callback_info info) {
    napi_value args[4];
    wwohos::NapiGetArgs(env, info, 4, args);
    double x = 0, y = 0;
    wwohos::NapiGetDouble(env, args[0], x);
    wwohos::NapiGetDouble(env, args[1], y);
    const std::string src = wwohos::NapiGetString(env, args[2]);
    const std::string tgt = wwohos::NapiGetString(env, args[3]);
    if (src.empty() || tgt.empty()) return wwohos::NapiMakeNull(env);
    double outX = 0.0;
    double outY = 0.0;
    if (!wwbridge::convert(x, y, src, tgt, outX, outY)) return wwohos::NapiMakeNull(env);
    const double buf[2] = {outX, outY};
    return wwohos::NapiMakeFloat64Array(env, buf, 2);
}

} // namespace

namespace wwohos {

napi_value RegisterSrs(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeGetProjVersion", nullptr, NativeGetProjVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeInitProjDataPath", nullptr, NativeInitProjDataPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeConvert", nullptr, NativeConvert, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace wwohos
