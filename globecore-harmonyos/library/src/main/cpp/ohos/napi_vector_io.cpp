// napi_vector_io.cpp —— 矢量要素 IO NAPI 导出（对应 Android bridge/vector_io.cpp 的 JNI 段）。
//
// 核心在 wwbridge（bridge_api.h），JSON 文本作传输格式，与 JNI 版逐字节一致。
// 导出命名对齐 JNI 门面 NativeVector（加 native 前缀与全模块风格统一）：
//   nativeReadVectorFeatures(path, minLon, minLat, maxLon, maxLat,
//       includeAllFields, labelField, simplifyGeometry, simplifyTolerance) → string | null
//   nativeQueryVectorFeatures(path, sql) → string | null
//   nativeCountVectorFeatures(path) → number
//   nativeUpdateFeatureAttributes(path, featureId, keys[], values[]) → boolean
//   nativeGetFeatureAttributes(path, featureId) → string | null
// featureId 在 ArkTS 侧以 number（<2^53）传输，对齐 JNI jlong 语义。
#include <napi/native_api.h>

#include <cmath>
#include <string>
#include <vector>

#include "bridge/bridge_api.h"
#include "napi_util.h"

namespace {

napi_value NativeReadVectorFeatures(napi_env env, napi_callback_info info) {
    napi_value args[9];
    wwohos::NapiGetArgs(env, info, 9, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return wwohos::NapiMakeNull(env);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    wwohos::NapiGetDouble(env, args[1], minLon);
    wwohos::NapiGetDouble(env, args[2], minLat);
    wwohos::NapiGetDouble(env, args[3], maxLon);
    wwohos::NapiGetDouble(env, args[4], maxLat);
    bool includeAll = false;
    wwohos::NapiGetBool(env, args[5], includeAll);
    const std::string labelField = wwohos::NapiGetString(env, args[6]);
    bool simplify = false;
    wwohos::NapiGetBool(env, args[7], simplify);
    double simplifyTol = 0;
    wwohos::NapiGetDouble(env, args[8], simplifyTol);
    // 口径同 JNI wrapper：简化关闭则容差归零；四至任一 NaN 视为不过滤
    const double tol = simplify ? simplifyTol : 0.0;
    const bool hasFilter = !std::isnan(minLon) && !std::isnan(minLat)
                           && !std::isnan(maxLon) && !std::isnan(maxLat);

    const std::string json = wwbridge::readVectorFeatures(path, minLon, minLat, maxLon, maxLat,
                                                          hasFilter, includeAll, labelField,
                                                          simplify, tol);
    if (json.empty()) return wwohos::NapiMakeNull(env);
    return wwohos::NapiMakeString(env, json);
}

napi_value NativeQueryVectorFeatures(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    const std::string sql = wwohos::NapiGetString(env, args[1]);
    if (path.empty() || sql.empty()) return wwohos::NapiMakeNull(env);
    const std::string json = wwbridge::queryVectorFeatures(path, sql);
    if (json.empty()) return wwohos::NapiMakeNull(env);
    return wwohos::NapiMakeString(env, json);
}

napi_value NativeCountVectorFeatures(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    if (path.empty()) return wwohos::NapiMakeInt32(env, -1);
    // 核心已 cap 0x7FFFFFFF（对齐 JNI jint 返回），此处直接窄化
    return wwohos::NapiMakeInt32(env, static_cast<int32_t>(wwbridge::countVectorFeatures(path)));
}

napi_value NativeUpdateFeatureAttributes(napi_env env, napi_callback_info info) {
    napi_value args[4];
    wwohos::NapiGetArgs(env, info, 4, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    int64_t featureId = -1;
    wwohos::NapiGetInt64(env, args[1], featureId);
    if (path.empty() || featureId < 0) return wwohos::NapiMakeBool(env, false);
    const std::vector<std::string> keys = wwohos::NapiGetStringArray(env, args[2]);
    const std::vector<std::string> values = wwohos::NapiGetStringArray(env, args[3]);
    const bool ok = wwbridge::updateFeatureAttributes(path, featureId, keys, values);
    return wwohos::NapiMakeBool(env, ok);
}

napi_value NativeGetFeatureAttributes(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    const std::string path = wwohos::NapiGetString(env, args[0]);
    int64_t featureId = -1;
    wwohos::NapiGetInt64(env, args[1], featureId);
    if (path.empty() || featureId < 0) return wwohos::NapiMakeNull(env);
    const std::string json = wwbridge::getFeatureAttributes(path, featureId);
    if (json.empty()) return wwohos::NapiMakeNull(env);
    return wwohos::NapiMakeString(env, json);
}

} // namespace

namespace wwohos {

napi_value RegisterVectorIo(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeReadVectorFeatures", nullptr, NativeReadVectorFeatures, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeQueryVectorFeatures", nullptr, NativeQueryVectorFeatures, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeCountVectorFeatures", nullptr, NativeCountVectorFeatures, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateFeatureAttributes", nullptr, NativeUpdateFeatureAttributes, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetFeatureAttributes", nullptr, NativeGetFeatureAttributes, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace wwohos
