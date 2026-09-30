// napi_layer_info.cpp —— 图层信息 NAPI 导出（对应 Android bridge/layer_info.cpp 的 JNI 段）。
//
// 核心在 wwbridge（bridge_api.h）。导出命名对齐 JNI 门面 NativeLayerInfo：
//   nativeGetLayerExtent(path) → Float64Array[4] | null（{minLon,minLat,maxLon,maxLat}）
//   nativeGetVectorFieldNames(path) → string[]（打不开返回空数组，对齐 JNI 空 jobjectArray）
//   nativeGetLayerSrs(path) → string | null（无 SRS/打不开 = null，核心空串转 null）
// GDAL 版本/驱动两枚导出在 napi_gdal_info.cpp（同 JNI 门面，按核心函数分文件）。
#include <napi/native_api.h>

#include <string>
#include <vector>

#include "bridge/bridge_api.h"
#include "napi_util.h"

namespace {

napi_value NativeGetLayerExtent(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return wwohos::NapiMakeNull(env);
    double extent[4] = {0, 0, 0, 0};
    if (!wwbridge::layerExtent(path, extent)) return wwohos::NapiMakeNull(env);
    return wwohos::NapiMakeFloat64Array(env, extent, 4);
}

napi_value NativeGetVectorFieldNames(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return wwohos::NapiMakeStringArray(env, {});
    return wwohos::NapiMakeStringArray(env, wwbridge::vectorFieldNames(path));
}

napi_value NativeGetLayerSrs(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return wwohos::NapiMakeNull(env);
    const std::string srs = wwbridge::layerSrs(path);
    if (srs.empty()) return wwohos::NapiMakeNull(env); // 对齐 JNI 空串 → null
    return wwohos::NapiMakeString(env, srs);
}

} // namespace

namespace wwohos {

napi_value RegisterLayerInfo(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeGetLayerExtent", nullptr, NativeGetLayerExtent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetVectorFieldNames", nullptr, NativeGetVectorFieldNames, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetLayerSrs", nullptr, NativeGetLayerSrs, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace wwohos
