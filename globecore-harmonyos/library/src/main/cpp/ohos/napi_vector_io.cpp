// napi_vector_io.cpp —— 矢量要素 IO NAPI 导出（对应 Android bridge/vector_io.cpp 的 JNI 段）。
//
// 核心在 gcbridge（bridge_api.h），JSON 文本作传输格式，与 JNI 版逐字节一致。
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
    gcohos::NapiGetArgs(env, info, 9, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    if (path.empty()) return gcohos::NapiMakeNull(env);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    gcohos::NapiGetDouble(env, args[1], minLon);
    gcohos::NapiGetDouble(env, args[2], minLat);
    gcohos::NapiGetDouble(env, args[3], maxLon);
    gcohos::NapiGetDouble(env, args[4], maxLat);
    bool includeAll = false;
    gcohos::NapiGetBool(env, args[5], includeAll);
    const std::string labelField = gcohos::NapiGetString(env, args[6]);
    bool simplify = false;
    gcohos::NapiGetBool(env, args[7], simplify);
    double simplifyTol = 0;
    gcohos::NapiGetDouble(env, args[8], simplifyTol);
    // 口径同 JNI wrapper：简化关闭则容差归零；四至任一 NaN 视为不过滤
    const double tol = simplify ? simplifyTol : 0.0;
    const bool hasFilter = !std::isnan(minLon) && !std::isnan(minLat)
                           && !std::isnan(maxLon) && !std::isnan(maxLat);

    const std::string json = gcbridge::readVectorFeatures(path, minLon, minLat, maxLon, maxLat,
                                                          hasFilter, includeAll, labelField,
                                                          simplify, tol);
    if (json.empty()) return gcohos::NapiMakeNull(env);
    return gcohos::NapiMakeString(env, json);
}

napi_value NativeQueryVectorFeatures(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    const std::string sql = gcohos::NapiGetString(env, args[1]);
    if (path.empty() || sql.empty()) return gcohos::NapiMakeNull(env);
    const std::string json = gcbridge::queryVectorFeatures(path, sql);
    if (json.empty()) return gcohos::NapiMakeNull(env);
    return gcohos::NapiMakeString(env, json);
}

napi_value NativeCountVectorFeatures(napi_env env, napi_callback_info info) {
    napi_value args[1];
    gcohos::NapiGetArgs(env, info, 1, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    if (path.empty()) return gcohos::NapiMakeInt32(env, -1);
    // 核心已 cap 0x7FFFFFFF（对齐 JNI jint 返回），此处直接窄化
    return gcohos::NapiMakeInt32(env, static_cast<int32_t>(gcbridge::countVectorFeatures(path)));
}

napi_value NativeUpdateFeatureAttributes(napi_env env, napi_callback_info info) {
    napi_value args[4];
    gcohos::NapiGetArgs(env, info, 4, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t featureId = -1;
    gcohos::NapiGetInt64(env, args[1], featureId);
    if (path.empty() || featureId < 0) return gcohos::NapiMakeBool(env, false);
    const std::vector<std::string> keys = gcohos::NapiGetStringArray(env, args[2]);
    const std::vector<std::string> values = gcohos::NapiGetStringArray(env, args[3]);
    const bool ok = gcbridge::updateFeatureAttributes(path, featureId, keys, values);
    return gcohos::NapiMakeBool(env, ok);
}

napi_value NativeGetFeatureAttributes(napi_env env, napi_callback_info info) {
    napi_value args[2];
    gcohos::NapiGetArgs(env, info, 2, args);
    const std::string path = gcohos::NapiGetString(env, args[0]);
    int64_t featureId = -1;
    gcohos::NapiGetInt64(env, args[1], featureId);
    if (path.empty() || featureId < 0) return gcohos::NapiMakeNull(env);
    const std::string json = gcbridge::getFeatureAttributes(path, featureId);
    if (json.empty()) return gcohos::NapiMakeNull(env);
    return gcohos::NapiMakeString(env, json);
}

} // namespace

namespace gcohos {

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

} // namespace gcohos
