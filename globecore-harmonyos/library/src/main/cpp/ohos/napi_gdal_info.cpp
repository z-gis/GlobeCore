// napi_gdal_info.cpp —— GDAL 版本与驱动清单 NAPI 导出（对应 Android bridge/gdal_info.cpp 的 JNI 段）。
//
// 核心在 wwbridge::gdalVersion / vectorDrivers；JNI 版挂在 NativeLayerInfo 门面，
// 鸿蒙统一导出：
//   nativeGetGdalVersion() → string
//   nativeGetVectorDrivers() → string（逐行 "NAME（读/写|只读）"）
#include <napi/native_api.h>

#include <string>

#include "bridge/bridge_api.h"
#include "napi_util.h"

namespace {

napi_value NativeGetGdalVersion(napi_env env, napi_callback_info /*info*/) {
    return wwohos::NapiMakeString(env, wwbridge::gdalVersion());
}

napi_value NativeGetVectorDrivers(napi_env env, napi_callback_info /*info*/) {
    return wwohos::NapiMakeString(env, wwbridge::vectorDrivers());
}

} // namespace

namespace wwohos {

napi_value RegisterGdalInfo(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeGetGdalVersion", nullptr, NativeGetGdalVersion, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetVectorDrivers", nullptr, NativeGetVectorDrivers, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace wwohos
