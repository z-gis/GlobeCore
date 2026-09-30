// GDAL 版本信息与驱动清单 —— 核心实现见 bridge_api.h（双平台共享），
// Android JNI 导出为文末 #ifndef __OHOS__ 薄封装；鸿蒙侧由 napi_gdal_info.cpp 调同一核心。
#include <string>

#include "gdal/gdal_priv.h"

#include "bridge_api.h"

// ==================== GDAL 版本信息与驱动清单（NativeLayerInfo 门面） ====================

namespace wwbridge {

std::string gdalVersion() {
    GDALAllRegister();
    const char *chVersion = GDALVersionInfo("VERSION_NUM");
    return chVersion != nullptr ? chVersion : "";
}

std::string vectorDrivers() {
    GDALAllRegister();

    // 枚举全部矢量驱动，标注是否支持创建（写入）
    GDALDriverManager *mgr = GetGDALDriverManager();
    std::string result;
    const int count = mgr->GetDriverCount();
    for (int i = 0; i < count; i++) {
        GDALDriver *driver = mgr->GetDriver(i);
        if (driver->GetMetadataItem(GDAL_DCAP_VECTOR) == nullptr) continue;
        const bool canCreate = driver->GetMetadataItem(GDAL_DCAP_CREATE) != nullptr;
        if (!result.empty()) result += "\n";
        result += driver->GetDescription();
        result += canCreate ? "（读/写）" : "（只读）";
    }
    return result;
}

} // namespace wwbridge

// ── Android JNI 导出薄封装 ──
#if !defined(__OHOS__)
#include <jni.h>

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeLayerInfo_getGdalVersion(JNIEnv *env, jobject /*thiz*/) {
    const std::string v = wwbridge::gdalVersion();
    return env->NewStringUTF(v.c_str());
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_zys_globecore_NativeLayerInfo_getVectorDrivers(JNIEnv *env, jobject /*thiz*/) {
    const std::string d = wwbridge::vectorDrivers();
    return env->NewStringUTF(d.c_str());
}

#endif // !__OHOS__
