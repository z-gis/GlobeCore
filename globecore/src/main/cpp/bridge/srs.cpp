// PROJ 初始化 / 坐标转换 / 版本 —— 核心实现见 bridge_api.h（双平台共享），
// Android JNI 导出为文末 #ifndef __OHOS__ 薄封装；鸿蒙侧由 napi_srs.cpp 调同一核心。
#include <map>
#include <mutex>
#include <string>

#include "proj/proj.h"

#include "bridge_api.h"
#include "util/Log.h"

// ==================== PROJ 初始化 / 坐标转换 / 版本 ====================

static PJ_CONTEXT *g_proj_ctx = nullptr;

static PJ_CONTEXT *getProjContext() {
    if (!g_proj_ctx) {
        g_proj_ctx = proj_context_create();
    }
    return g_proj_ctx;
}

// ==================== 转换管道缓存 ====================
// 主界面中心/定位坐标文本随导航事件逐帧刷新，每次都 proj_create_crs_to_crs
// （需查 proj.db 构建管道）开销远大于转换本身，故按 "源|目标" 缓存 PJ。
// PJ 常驻不销毁（组合数量有限）；PROJ 上下文与 PJ 非线程安全，转换全程持锁串行。

static std::mutex g_pjMutex;
static std::map<std::string, PJ *> g_pjCache;

/** 持锁调用：取或创建缓存的坐标转换管道（已归一化为 x=经度/东、y=纬度/北 的可视化轴序） */
static PJ *getCachedPipelineLocked(const std::string &src, const std::string &tgt) {
    const std::string key = src + "|" + tgt;
    auto it = g_pjCache.find(key);
    if (it != g_pjCache.end()) {
        return it->second;
    }
    PJ_CONTEXT *ctx = getProjContext();
    PJ *pj = proj_create_crs_to_crs(ctx, src.c_str(), tgt.c_str(), nullptr);
    if (pj != nullptr) {
        PJ *norm = proj_normalize_for_visualization(ctx, pj);
        proj_destroy(pj);
        pj = norm;
    }
    if (pj == nullptr) {
        int err = proj_context_errno(ctx);
        LOGE("proj_create_crs_to_crs failed: %s -> %s, error=%d: %s",
             src.c_str(), tgt.c_str(), err, proj_context_errno_string(ctx, err));
        return nullptr;
    }
    g_pjCache[key] = pj;
    return pj;
}

namespace gcbridge {

int projVersion() {
    const PJ_INFO info = proj_info();
    return info.major * 10000 + info.minor * 100 + info.patch;
}

void initProjDataPath(const std::string &path) {
    PJ_CONTEXT *ctx = getProjContext();
    const char *cPath = path.c_str();
    proj_context_set_search_paths(ctx, 1, &cPath);

    PJ *test = proj_create(ctx, "EPSG:4326");
    if (test) {
        LOGI("proj.db loaded OK from: %s", cPath);
        proj_destroy(test);
    } else {
        LOGE("Failed to load proj.db from: %s, error: %s",
             cPath, proj_context_errno_string(ctx, proj_context_errno(ctx)));
    }
}

bool convert(double x, double y, const std::string &srcCrs, const std::string &tgtCrs,
             double &outX, double &outY) {
    std::lock_guard<std::mutex> lock(g_pjMutex);
    PJ *pj = getCachedPipelineLocked(srcCrs, tgtCrs);
    if (pj == nullptr) return false;
    PJ_COORD out_coord = proj_trans(pj, PJ_FWD, proj_coord(x, y, 0, 0));
    if (proj_errno(pj)) {
        proj_errno_reset(pj);
        return false;
    }
    outX = out_coord.xy.x;
    outY = out_coord.xy.y;
    return true;
}

} // namespace gcbridge

// ── Android JNI 导出薄封装（com.zys.globecore.NativeSrs 门面）──
#if !defined(__OHOS__)
#include <jni.h>

extern "C"
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeSrs_getProjVersion(JNIEnv * /*env*/, jobject /*thiz*/) {
    return static_cast<jint>(gcbridge::projVersion());
}

extern "C"
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeSrs_initProjDataPath(JNIEnv *env, jobject /*thiz*/,
                                                     jstring proj_data_path) {
    const char *path = env->GetStringUTFChars(proj_data_path, nullptr);
    if (path == nullptr) return;
    gcbridge::initProjDataPath(path);
    env->ReleaseStringUTFChars(proj_data_path, path);
}

extern "C"
JNIEXPORT jdoubleArray JNICALL
Java_com_zys_globecore_NativeSrs_convert(
        JNIEnv *env, jobject /*thiz*/,
        jdouble x, jdouble y,
        jstring src_crs, jstring tgt_crs) {

    const char *src = env->GetStringUTFChars(src_crs, nullptr);
    const char *tgt = env->GetStringUTFChars(tgt_crs, nullptr);
    if (src == nullptr || tgt == nullptr) {
        if (src) env->ReleaseStringUTFChars(src_crs, src);
        if (tgt) env->ReleaseStringUTFChars(tgt_crs, tgt);
        return nullptr;
    }
    double outX = 0.0;
    double outY = 0.0;
    const bool ok = gcbridge::convert(x, y, src, tgt, outX, outY);
    env->ReleaseStringUTFChars(src_crs, src);
    env->ReleaseStringUTFChars(tgt_crs, tgt);
    if (!ok) return nullptr;
    jdoubleArray result = env->NewDoubleArray(2);
    if (result == nullptr) return nullptr;
    const jdouble vals[2] = {outX, outY};
    env->SetDoubleArrayRegion(result, 0, 2, vals);
    return result;
}

#endif // !__OHOS__
