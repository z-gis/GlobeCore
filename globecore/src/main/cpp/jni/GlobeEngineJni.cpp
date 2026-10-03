// globecore JNI 桥接层：Kotlin 侧只有接口，全部实现位于 native。
// 符号命名对应 Kotlin 类 com.zys.globecore.NativeLib（静态注册，与 app 现有 JNI 风格一致）。
//
// 采用「native 句柄」模型：nativeCreate 返回一个指向 C++ GlobeEngine 实例的地址（jlong），
// 后续调用透传该句柄，避免全局单例、支持多实例。句柄为 0 时直接返回，防止空指针解引用。
#include <jni.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/GlobeEngine.h"
#include "util/Log.h"

using globecore::GlobeEngine;

namespace {

inline GlobeEngine *toGlobeEngine(jlong handle) {
    return reinterpret_cast<GlobeEngine *>(handle);
}

/// 把 #AARRGGBB 整型颜色拆为 [0,1] 浮点 RGBA（对齐 app VectorStyle 的颜色口径）
inline void unpackArgb(jint c, float &r, float &g, float &b, float &a) {
    a = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
    r = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    g = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(c & 0xFF) / 255.0f;
}

/// Android ARGB(0xAARRGGBB) IntArray → RGBA 字节序列（供 native 上传图标纹理）。
/// 空/尺寸不匹配（n != iconW*iconH）时返回空 vector → 点要素回退画屏幕固定圆。
inline std::vector<uint8_t> iconArgbToRgba(JNIEnv *env, jintArray iconArgb, jint iconW, jint iconH) {
    std::vector<uint8_t> out;
    if (iconArgb == nullptr || iconW <= 0 || iconH <= 0) return out;
    const jsize n = env->GetArrayLength(iconArgb);
    if (n != static_cast<jsize>(iconW) * static_cast<jsize>(iconH)) return out;
    jint *px = env->GetIntArrayElements(iconArgb, nullptr);
    if (px == nullptr) return out;
    out.resize(static_cast<size_t>(n) * 4);
    for (jsize i = 0; i < n; ++i) {
        const uint32_t p = static_cast<uint32_t>(px[i]);
        out[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF); // R
        out[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);  // G
        out[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(p & 0xFF);         // B
        out[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>((p >> 24) & 0xFF); // A
    }
    env->ReleaseIntArrayElements(iconArgb, px, JNI_ABORT);
    return out;
}

/// jdoubleArray → std::vector<double>（null/空返回空）
inline std::vector<double> toDoubleVec(JNIEnv *env, jdoubleArray arr) {
    std::vector<double> out;
    if (arr == nullptr) return out;
    const jsize n = env->GetArrayLength(arr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    env->GetDoubleArrayRegion(arr, 0, n, out.data()); // jdouble 即 double，可直接写入
    return out;
}

/// jintArray → std::vector<int>（null/空返回空）
inline std::vector<int> toIntVec(JNIEnv *env, jintArray arr) {
    std::vector<int> out;
    if (arr == nullptr) return out;
    const jsize n = env->GetArrayLength(arr);
    if (n <= 0) return out;
    jint *e = env->GetIntArrayElements(arr, nullptr);
    if (e == nullptr) return out;
    out.resize(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<int>(e[i]);
    env->ReleaseIntArrayElements(arr, e, JNI_ABORT);
    return out;
}

/// jlongArray → std::vector<long long>（null/空返回空）
inline std::vector<long long> toLongVec(JNIEnv *env, jlongArray arr) {
    std::vector<long long> out;
    if (arr == nullptr) return out;
    const jsize n = env->GetArrayLength(arr);
    if (n <= 0) return out;
    jlong *e = env->GetLongArrayElements(arr, nullptr);
    if (e == nullptr) return out;
    out.resize(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) out[static_cast<size_t>(i)] = static_cast<long long>(e[i]);
    env->ReleaseLongArrayElements(arr, e, JNI_ABORT);
    return out;
}

/// String[]（jobjectArray）→ std::vector<std::string>（null 元素转空串；null/空数组返回空）
inline std::vector<std::string> toStringVec(JNIEnv *env, jobjectArray arr) {
    std::vector<std::string> out;
    if (arr == nullptr) return out;
    const jsize n = env->GetArrayLength(arr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    for (jsize i = 0; i < n; ++i) {
        auto s = static_cast<jstring>(env->GetObjectArrayElement(arr, i));
        if (s != nullptr) {
            const char *u = env->GetStringUTFChars(s, nullptr);
            if (u != nullptr) { out[static_cast<size_t>(i)] = u; env->ReleaseStringUTFChars(s, u); }
            env->DeleteLocalRef(s);
        }
    }
    return out;
}

/// 解析单要素样式覆盖串（来自 app 文档层 featureStyles）为 fid → FeatureStyleOverride。
/// 格式：条目以 ';' 分隔，每条目 8 字段逗号分隔：
///   fid,fillArgb,lineArgb,labelFieldName,labelArgb,labelSize,labelOutline,labelOutlineArgb
/// - fillArgb/lineArgb/labelArgb/labelOutlineArgb 为 #AARRGGBB 无符号十进制，'-' 或空 = 该通道未显式设过；
/// - labelFieldName：'-' 或缺省 = 无逐要素标注覆盖（继承整层）；非 '-' = 该要素标注的字段名（空串 = 显式关闭该要素标注）；
/// - labelSize：'-' = 继承，否则浮点；labelOutline：'-' = 继承，否则 1/0。
/// 向后兼容旧 3 字段串（fid,fillArgb,lineArgb）：缺失的标注字段按未设处理。空/非法条目静默跳过。
inline std::unordered_map<long long, globecore::FeatureStyleOverride> parseFeatureOverrides(const char *s) {
    std::unordered_map<long long, globecore::FeatureStyleOverride> out;
    if (s == nullptr) return out;
    auto unpackTo = [](unsigned int c, float (&dst)[4]) {
        dst[3] = static_cast<float>((c >> 24) & 0xFF) / 255.0f; // A
        dst[0] = static_cast<float>((c >> 16) & 0xFF) / 255.0f; // R
        dst[1] = static_cast<float>((c >> 8) & 0xFF) / 255.0f;  // G
        dst[2] = static_cast<float>(c & 0xFF) / 255.0f;         // B
    };
    // 取第 i 个逗号分隔字段（越界返回 nullptr 表示缺省）；'-' 与空串区分由调用方判 token 内容。
    const std::string str(s);
    size_t pos = 0;
    while (pos < str.size()) {
        const size_t semi = str.find(';', pos);
        const std::string entry = str.substr(pos, (semi == std::string::npos) ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? str.size() : semi + 1;
        if (entry.empty()) continue;
        // 切分本条目全部逗号字段
        std::vector<std::string> tk;
        size_t p = 0;
        while (true) {
            const size_t cm = entry.find(',', p);
            tk.push_back(entry.substr(p, (cm == std::string::npos) ? std::string::npos : cm - p));
            if (cm == std::string::npos) break;
            p = cm + 1;
        }
        if (tk.empty()) continue;
        auto get = [&](size_t i, std::string &v) -> bool {  // true=字段存在且非 '-'（即显式设过）
            if (i >= tk.size()) return false;
            const std::string &t = tk[i];
            if (t.empty() || t == "-") return false;
            v = t;
            return true;
        };
        globecore::FeatureStyleOverride ov;
        std::string v;
        if (get(1, v)) { ov.hasFill = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.fill); }
        if (get(2, v)) { ov.hasLine = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.line); }
        // labelFieldName（索引 3）：区分 '-'（缺字段 get 返回 false）与显式关闭（字段存在但为空串）。
        // get() 把空串也当未设，故此处单独判：字段存在且 != '-' 即 hasLabel=true，其值（可能空串）= labelField。
        if (tk.size() > 3 && tk[3] != "-") {
            ov.hasLabel = true;
            ov.labelField = tk[3];  // 空串 = 显式关闭；非空 = 标注字段名（可含非 ASCII，原样传递）
        }
        if (get(4, v)) { ov.hasLabelColor = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.labelColor); }
        if (get(5, v)) { ov.hasLabelSize = true; ov.labelSize = static_cast<float>(strtod(v.c_str(), nullptr)); }
        if (get(6, v)) { ov.hasLabelOutline = true; ov.labelOutlineVal = (v[0] == '1'); }
        if (get(7, v)) { ov.hasLabelOutlineColor = true; unpackTo(static_cast<unsigned int>(strtoull(v.c_str(), nullptr, 10)), ov.labelOutlineColor); }
        if (ov.hasFill || ov.hasLine || ov.hasLabel || ov.hasLabelColor || ov.hasLabelSize ||
            ov.hasLabelOutline || ov.hasLabelOutlineColor)
            out[strtoll(tk[0].c_str(), nullptr, 10)] = ov;
    }
    return out;
}

/// 持有 Kotlin GLSurfaceView 的全局引用与 requestRender 方法号，析构时自动释放全局引用（RAII）。
/// 由 nativeSetRenderCallback 创建、被 GlobeEngine 的重绘回调 lambda 以 shared_ptr 持有；
/// GlobeEngine 销毁时 lambda 随之销毁 → 本结构析构 → DeleteGlobalRef，不泄露。
struct RenderCallbackHolder {
    JavaVM *vm = nullptr;
    jobject viewRef = nullptr;
    jmethodID requestRenderMid = nullptr;
    ~RenderCallbackHolder() {
        if (vm == nullptr || viewRef == nullptr) return;
        JNIEnv *env = nullptr;
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
            env->DeleteGlobalRef(viewRef);
        }
    }
};

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_zys_globecore_NativeLib_nativeCreate(JNIEnv * /*env*/, jobject /*thiz*/) {
    return reinterpret_cast<jlong>(new GlobeEngine());
}

JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeDestroy(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return;
    delete toGlobeEngine(handle);
}

JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSurfaceCreated(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return;
    toGlobeEngine(handle)->surfaceCreated();
}

JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSurfaceChanged(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                         jint width, jint height) {
    if (handle == 0) return;
    toGlobeEngine(handle)->surfaceChanged(width, height);
}

JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeDrawFrame(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return;
    toGlobeEngine(handle)->drawFrame();
}

// 在 GL 线程释放 GL 资源（着色器程序、VBO）。由 Kotlin 侧 queueEvent 调用，
// 保证 glDelete* 在当前上下文所在线程执行；必须在 nativeDestroy 之前调用。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeReleaseGl(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return;
    toGlobeEngine(handle)->releaseGl();
}

// 设置相机完整姿态（对应 参考实现 Camera 唯一通道）：经纬度/高度 + heading/tilt/roll/fieldOfView + altitudeMode（枚举序数 int）。
// 由 Kotlin NativeMapView.setCamera(Camera) 调用；角度用度、高度用米，口径与 geom/Camera.h 一致。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetCamera(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                    jdouble latitudeDeg, jdouble longitudeDeg, jdouble altitudeMeters,
                                                    jdouble headingDeg, jdouble tiltDeg, jdouble rollDeg,
                                                    jdouble fieldOfViewDeg, jint altitudeMode) {
    if (handle == 0) return;
    globecore::Camera cam;
    cam.latitude = latitudeDeg;
    cam.longitude = longitudeDeg;
    cam.altitude = altitudeMeters;
    cam.heading = headingDeg;
    cam.tilt = tiltDeg;
    cam.roll = rollDeg;
    cam.fieldOfView = fieldOfViewDeg;
    // int → 枚举：越界回退 ABSOLUTE（与 geom/Camera.h 默认一致）
    cam.altitudeMode = (altitudeMode >= 0 && altitudeMode <= 3)
                           ? static_cast<globecore::AltitudeMode>(altitudeMode)
                           : globecore::AltitudeMode::ABSOLUTE;
    toGlobeEngine(handle)->setCamera(cam);
}

// 读回相机完整姿态，返回 [latitude, longitude, altitude, heading, tilt, roll, fieldOfView, altitudeMode(序数)]（长度 8）。
// 供 Kotlin NativeMapView.getCamera() 组装 [Camera]；句柄已释放返回 null。
JNIEXPORT jdoubleArray JNICALL
Java_com_zys_globecore_NativeLib_nativeGetCamera(JNIEnv *env, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return nullptr;
    const globecore::Camera cam = toGlobeEngine(handle)->getCamera();
    jdoubleArray arr = env->NewDoubleArray(8);
    if (arr == nullptr) return nullptr;
    const jdouble buf[8] = {
        cam.latitude, cam.longitude, cam.altitude,
        cam.heading, cam.tilt, cam.roll, cam.fieldOfView,
        static_cast<jdouble>(static_cast<int>(cam.altitudeMode))
    };
    env->SetDoubleArrayRegion(arr, 0, 8, buf);
    return arr;
}

// 设置视图模式：0=2D 平面墨卡托正交 / 1=3D 球体透视（口径同 Navigator::ViewMode）。
// 两模式共用相机状态，切换即保持视角中心/高度连续；内部已触发重绘（WHEN_DIRTY 下即时生效）。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetViewMode(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle, jint mode) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setViewMode(mode);
}

// 读回当前视图模式（0/1）；句柄已释放返回 0（2D）。
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeLib_nativeGetViewMode(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return 0;
    return static_cast<jint>(toGlobeEngine(handle)->viewMode());
}

// 手势平移（屏幕像素位移），Phase D 由 app 手势识别调用
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativePanBy(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                jdouble dxPx, jdouble dyPx) {
    if (handle == 0) return;
    toGlobeEngine(handle)->panByPixels(dxPx, dyPx);
}

// 手势缩放（以屏幕焦点为锚，factor>1 放大），Phase D 由 app 手势识别调用
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeZoomBy(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                 jdouble factor, jdouble focusXpx, jdouble focusYpx) {
    if (handle == 0) return;
    toGlobeEngine(handle)->zoomBy(factor, focusXpx, focusYpx);
}

// 手势旋转：相机 heading 累加增量（度，顺时针自北为正），仅 3D 通路消费（2D 恒正北，Kotlin 手势侧已门控）
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeRotateHeading(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                        jdouble deltaDeg) {
    if (handle == 0) return;
    toGlobeEngine(handle)->rotateHeading(deltaDeg);
}

// 手势俯仰：相机 tilt 累加增量（度，正=向地平线方向倾视，native 钳 [0,80]），仅 3D 通路消费
//（2D 不消费 tilt，Kotlin 手势侧已门控不调用）
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeRotateTilt(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                     jdouble deltaDeg) {
    if (handle == 0) return;
    toGlobeEngine(handle)->rotateTilt(deltaDeg);
}

// 添加一个瓦片图源图层（可多次调用叠加：底图在前、注记 overlay 在后），native 按 <cacheDir>/<z>/<x>_<y>.tile 读写
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeAddTileLayer(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                       jstring cacheDir, jstring urlTemplate, jint maxLevel,
                                                       jboolean overlay) {
    if (handle == 0 || cacheDir == nullptr) return;
    const char *dirUtf = env->GetStringUTFChars(cacheDir, nullptr);
    if (dirUtf == nullptr) return;
    std::string url;
    if (urlTemplate != nullptr) {
        const char *urlUtf = env->GetStringUTFChars(urlTemplate, nullptr);
        if (urlUtf != nullptr) {
            url = urlUtf;
            env->ReleaseStringUTFChars(urlTemplate, urlUtf);
        }
    }
    toGlobeEngine(handle)->addTileLayer(std::string(dirUtf), url, maxLevel, overlay == JNI_TRUE);
    env->ReleaseStringUTFChars(cacheDir, dirUtf);
}

// 设置指定图层可见性（index 为 addTileLayer 加入次序，0 起）：隐藏层不绘制、不取瓦片（如注记显隐开关）
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetLayerVisible(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                          jint index, jboolean visible) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setLayerVisible(index, visible == JNI_TRUE);
}

// 添加一个本地栅格图层（tif/img 等）：native 用内建 GDAL 重投影按全球墨卡托瓦片四至即时生成瓦片，
// 绘制在底图之上、矢量之下。返回新图层在 layers_ 的索引（0 起，供 nativeSetLayerVisible）；句柄/路径无效或打不开栅格返回 -1。
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeLib_nativeAddRasterLayer(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                         jstring cacheDir, jstring path) {
    if (handle == 0 || cacheDir == nullptr || path == nullptr) return -1;
    const char *dirUtf = env->GetStringUTFChars(cacheDir, nullptr);
    if (dirUtf == nullptr) return -1;
    const char *pathUtf = env->GetStringUTFChars(path, nullptr);
    if (pathUtf == nullptr) {
        env->ReleaseStringUTFChars(cacheDir, dirUtf);
        return -1;
    }
    const jint index = toGlobeEngine(handle)->addRasterLayer(std::string(dirUtf), std::string(pathUtf));
    env->ReleaseStringUTFChars(path, pathUtf);
    env->ReleaseStringUTFChars(cacheDir, dirUtf);
    return index;
}

// 添加一个矢量图层（native 直接读文件：GDAL/OGR 读取 + 重投影 + earcut 三角剖分，异步加载）。
// 颜色为 #AARRGGBB 整型，线宽为像素、点半径为 dp。labelField 非空时逐要素取该字段值为标注文本
// （labelSize 为字号缩放、1.0=基准）。返回新图层索引（0 起）；句柄/路径无效返回 -1。
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeLib_nativeAddVectorLayer(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                         jstring path,
                                                         jint fillColor, jint outlineColor, jfloat outlineWidth,
                                                         jint lineColor, jfloat lineWidth,
                                                         jint pointColor, jfloat pointRadiusDp,
                                                         jboolean hasFillColor, jboolean hasLineColor,
                                                         jstring labelField, jint labelColor, jfloat labelSize,
                                                         jboolean labelOutline, jint labelOutlineColor,
                                                         jstring featureStyleOverride,
                                                         jintArray iconArgb, jint iconW, jint iconH,
                                                         jboolean hasExtent, jdouble minLon, jdouble minLat,
                                                         jdouble maxLon, jdouble maxLat, jint maxFeatures) {
    if (handle == 0 || path == nullptr) return -1;
    const char *pathUtf = env->GetStringUTFChars(path, nullptr);
    if (pathUtf == nullptr) return -1;

    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = outlineWidth;
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = lineWidth;
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = pointRadiusDp;
    // 逐要素源文件配色门控：整层显式设了该通道则不被 KML 原色覆盖（图层管理改样式即时生效）。
    style.hasFillColor = (hasFillColor == JNI_TRUE);
    style.hasLineColor = (hasLineColor == JNI_TRUE);
    // 标注样式：labelField 为空则整层不标注（对齐主界面门控）
    if (labelField != nullptr) {
        const char *lfUtf = env->GetStringUTFChars(labelField, nullptr);
        if (lfUtf != nullptr) {
            style.labelField = lfUtf;
            env->ReleaseStringUTFChars(labelField, lfUtf);
        }
    }
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = labelSize;
    style.labelOutline = (labelOutline == JNI_TRUE);
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB, style.labelOutlineA);
    // 单要素样式覆盖：解析下传串入 style.featureOverrides（建几何时逐要素按 fid 命中覆盖）。
    if (featureStyleOverride != nullptr) {
        const char *ovUtf = env->GetStringUTFChars(featureStyleOverride, nullptr);
        style.featureOverrides = parseFeatureOverrides(ovUtf);
        if (ovUtf != nullptr) env->ReleaseStringUTFChars(featureStyleOverride, ovUtf);
    }

    // 点要素图标：Android ARGB(0xAARRGGBB) IntArray → RGBA 字节（供 native 上传纹理、billboard 渲染）。
    // 尺寸不匹配或无图标时 iconRgba 留空 → 点要素回退画屏幕固定圆。
    std::vector<uint8_t> iconRgba;
    if (iconArgb != nullptr && iconW > 0 && iconH > 0) {
        const jsize n = env->GetArrayLength(iconArgb);
        if (n == static_cast<jsize>(iconW) * static_cast<jsize>(iconH)) {
            jint *px = env->GetIntArrayElements(iconArgb, nullptr);
            if (px != nullptr) {
                iconRgba.resize(static_cast<size_t>(n) * 4);
                for (jsize i = 0; i < n; ++i) {
                    const uint32_t p = static_cast<uint32_t>(px[i]);
                    iconRgba[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF); // R
                    iconRgba[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);  // G
                    iconRgba[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(p & 0xFF);         // B
                    iconRgba[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>((p >> 24) & 0xFF); // A
                }
                env->ReleaseIntArrayElements(iconArgb, px, JNI_ABORT);
            }
        }
    }

    const jint index = toGlobeEngine(handle)->addVectorLayer(std::string(pathUtf), style,
                                                             std::move(iconRgba), iconW, iconH,
                                                             hasExtent == JNI_TRUE, minLon, minLat,
                                                             maxLon, maxLat, maxFeatures);
    env->ReleaseStringUTFChars(path, pathUtf);
    return index;
}

// 按新屏幕范围重载指定矢量层（Swap-on-ready，无空窗）：index 为 nativeAddVectorLayer 加入次序。
// hasExtent=JNI_FALSE 走整文件全量；maxFeatures≤0 取硬上限。越界/非文件层忽略。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeUpdateVectorExtent(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                             jint index, jboolean hasExtent,
                                                             jdouble minLon, jdouble minLat,
                                                             jdouble maxLon, jdouble maxLat, jint maxFeatures) {
    if (handle == 0) return;
    toGlobeEngine(handle)->updateVectorExtent(index, hasExtent == JNI_TRUE, minLon, minLat,
                                              maxLon, maxLat, maxFeatures);
}

// 文件矢量层是否仍有未完成的加载/上传（后台读建几何或 GL 大块上传中）：供宿主轮询显示「加载中」提示。
// 纯查询不触发重绘；句柄无效返回 JNI_FALSE。
JNIEXPORT jboolean JNICALL
Java_com_zys_globecore_NativeLib_nativeHasVectorLoading(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return JNI_FALSE;
    return toGlobeEngine(handle)->hasVectorLoading() ? JNI_TRUE : JNI_FALSE;
}

// 设置指定矢量图层可见性（index 为 addVectorLayer 加入次序，0 起）：越界忽略
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetVectorLayerVisible(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                                jint index, jboolean visible) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setVectorLayerVisible(index, visible == JNI_TRUE);
}

// 设置文件矢量层级别可见性下限（文档 LayerInfo.effectiveMinDisplayLevel 传入，按显示级别；≤0 不限）：
// 相机显示级别 < minLevel 时整层隐藏且不上传/不可拾取。越界/叠加层忽略；变更触发一帧重绘。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetVectorMinLevel(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                            jint index, jint minLevel) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setVectorMinLevel(index, minLevel);
}

// 当前相机显示级别（Navigator::displayLevel()，与 app 界面级别文本同口径）：供宿主发出屏幕范围重载前
// 判定级别达标，级别不足跳过无谓加载。句柄无效返回 0（级别最低值，不致隐藏）。
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeLib_nativeGetCameraZoomLevel(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return 0;
    return toGlobeEngine(handle)->currentZoomLevel();
}

// 移除一个矢量图层（墓碑：置 dead + visible=false，GL 线程下一帧回收其 VBO/纹理/几何）：
// 文件矢量层与叠加层均适用，越界忽略。用于样式变更时就地换层，避免宿主整界面 recreate 造成闪动。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeRemoveVectorLayer(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                            jint index) {
    if (handle == 0) return;
    toGlobeEngine(handle)->removeVectorLayer(index);
}

// ── 动态叠加层（运行时内存几何：测量/拍照标识/轨迹/样地等）──
// 几何以摊平数组传入（经纬度对 double[]），native 组装 VectorReadResult → setData 同步建几何 → GL 上传。

// 新建动态叠加层：颜色/线宽/点半径/标注样式同 nativeAddVectorLayer，但无 labelField（标注文本由
// updateOverlay* 逐要素传入）。iconArgb/iconW/iconH 为点要素图标（可选）。返回索引（0 起）；句柄无效返回 -1。
JNIEXPORT jint JNICALL
Java_com_zys_globecore_NativeLib_nativeAddOverlayLayer(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                          jint fillColor, jint outlineColor, jfloat outlineWidth,
                                                          jint lineColor, jfloat lineWidth,
                                                          jint pointColor, jfloat pointRadiusDp,
                                                          jint labelColor, jfloat labelSize,
                                                          jboolean labelOutline, jint labelOutlineColor,
                                                          jintArray iconArgb, jint iconW, jint iconH) {
    if (handle == 0) return -1;
    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = outlineWidth;
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = lineWidth;
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = pointRadiusDp;
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = labelSize;
    style.labelOutline = (labelOutline == JNI_TRUE);
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB, style.labelOutlineA);
    std::vector<uint8_t> iconRgba = iconArgbToRgba(env, iconArgb, iconW, iconH);
    return toGlobeEngine(handle)->addOverlayLayer(style, std::move(iconRgba), iconW, iconH);
}

// 更新叠加点要素（覆盖式）：lonlat=[lon0,lat0,...]；fids/labels 与点一一对应（可空）。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeUpdateOverlayPoints(JNIEnv *env, jobject /*thiz*/, jlong handle, jint index,
                                                              jdoubleArray lonlat, jlongArray fids,
                                                              jobjectArray labels) {
    if (handle == 0) return;
    toGlobeEngine(handle)->updateOverlayPoints(index, toDoubleVec(env, lonlat), toLongVec(env, fids),
                                               toStringVec(env, labels));
}

// 更新叠加线要素（覆盖式）：lonlat 为全部线顶点摊平；vertexCounts[f] 为第 f 条线顶点数；fids/labels 与线对应。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeUpdateOverlayLines(JNIEnv *env, jobject /*thiz*/, jlong handle, jint index,
                                                             jdoubleArray lonlat, jintArray vertexCounts,
                                                             jlongArray fids, jobjectArray labels) {
    if (handle == 0) return;
    toGlobeEngine(handle)->updateOverlayLines(index, toDoubleVec(env, lonlat), toIntVec(env, vertexCounts),
                                              toLongVec(env, fids), toStringVec(env, labels));
}

// 更新叠加面要素（覆盖式）：lonlat 为全部环顶点摊平；ringVertexCounts 为每环顶点数（跨要素摊平）；
// ringsPerFeature[f] 为第 f 个面环数（首环外环、余为洞）；fids/labels 与面对应。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeUpdateOverlayPolygons(JNIEnv *env, jobject /*thiz*/, jlong handle, jint index,
                                                                jdoubleArray lonlat, jintArray ringVertexCounts,
                                                                jintArray ringsPerFeature, jlongArray fids,
                                                                jobjectArray labels) {
    if (handle == 0) return;
    toGlobeEngine(handle)->updateOverlayPolygons(index, toDoubleVec(env, lonlat), toIntVec(env, ringVertexCounts),
                                                 toIntVec(env, ringsPerFeature), toLongVec(env, fids),
                                                 toStringVec(env, labels));
}

// 移除叠加层（墓碑：置 dead + visible=false，GL 线程下一帧回收其资源）：仅对叠加层生效，越界忽略。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeRemoveOverlayLayer(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                             jint index) {
    if (handle == 0) return;
    toGlobeEngine(handle)->removeOverlayLayer(index);
}

// 设置叠加层「不参与拾取」标志（仅对叠加层生效，越界/非叠加层忽略）：true 时层仍正常绘制，
// 但 pickVector 整层跳过。选中高亮/查询高亮等瞬态视觉层经此退出拾取竞争，命中直接落到源层。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetOverlayNoPick(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                           jint index, jboolean noPick) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setOverlayNoPick(index, noPick == JNI_TRUE);
}

// 移除全部叠加层（墓碑所有 isOverlay 层）：文件矢量层不受影响。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeClearOverlayLayers(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle) {
    if (handle == 0) return;
    toGlobeEngine(handle)->clearOverlayLayers();
}

// 拾取矢量要素：屏幕点命中检测，命中返回 [layerIndex, fid]（jlong[2]）；未命中返回 null
JNIEXPORT jlongArray JNICALL
Java_com_zys_globecore_NativeLib_nativePickVector(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                     jdouble sxPx, jdouble syPx) {
    if (handle == 0) return nullptr;
    int layerIndex = -1;
    long long fid = -1;
    if (!toGlobeEngine(handle)->pickVector(sxPx, syPx, layerIndex, fid)) return nullptr;
    jlongArray arr = env->NewLongArray(2);
    if (arr == nullptr) return nullptr;
    jlong buf[2] = {static_cast<jlong>(layerIndex), static_cast<jlong>(fid)};
    env->SetLongArrayRegion(arr, 0, 2, buf);
    return arr;
}

// 屏幕点 → 地理经纬度（WGS84 度）：命中返回 [lon, lat]（jdouble[2]）；视口未就绪返回 null。供采集交互把地图单击转成加点坐标。
JNIEXPORT jdoubleArray JNICALL
Java_com_zys_globecore_NativeLib_nativeScreenToGeo(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                      jdouble sxPx, jdouble syPx) {
    if (handle == 0) return nullptr;
    double lon = 0.0, lat = 0.0;
    if (!toGlobeEngine(handle)->screenToGeo(sxPx, syPx, lon, lat)) return nullptr;
    jdoubleArray arr = env->NewDoubleArray(2);
    if (arr == nullptr) return nullptr;
    jdouble buf[2] = {lon, lat};
    env->SetDoubleArrayRegion(arr, 0, 2, buf);
    return arr;
}

// 取指定矢量层某 FID 要素的经纬度几何：命中返回 double[4][] = { {type}, ringCounts, ringsPerFeature, lonlat }
// （type：0=点,1=线,2=面；lonlat 摊平 [lon,lat,...]）；未命中或句柄已释放返回 null。
// 供宿主点击选中后画高亮叠加层（对齐主界面选中态高亮）。仅读 CPU 几何、不涉 GL，可在主线程调用。
JNIEXPORT jobjectArray JNICALL
Java_com_zys_globecore_NativeLib_nativeFeatureGeometry(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                          jint layerIndex, jlong fid) {
    if (handle == 0) return nullptr;
    int type = -1;
    std::vector<double> lonlat;
    std::vector<int> ringCounts, ringsPerFeature;
    if (!toGlobeEngine(handle)->featureGeometry(layerIndex, static_cast<long long>(fid), type,
                                                lonlat, ringCounts, ringsPerFeature)) {
        return nullptr;
    }
    jclass dblArrCls = env->FindClass("[D");
    if (dblArrCls == nullptr) return nullptr;
    jobjectArray result = env->NewObjectArray(4, dblArrCls, nullptr);
    if (result == nullptr) { env->DeleteLocalRef(dblArrCls); return nullptr; }
    // 逐槽填 double[]（int 值以 double 传递，Kotlin 侧 toInt 还原；小整数 double 精确表示）
    auto setSlot = [&](int slot, const std::vector<double> &v) {
        jdoubleArray a = env->NewDoubleArray(static_cast<jsize>(v.size()));
        if (a == nullptr) return;
        if (!v.empty()) env->SetDoubleArrayRegion(a, 0, static_cast<jsize>(v.size()), v.data());
        env->SetObjectArrayElement(result, slot, a);
        env->DeleteLocalRef(a);
    };
    setSlot(0, {static_cast<double>(type)});
    setSlot(1, std::vector<double>(ringCounts.begin(), ringCounts.end()));
    setSlot(2, std::vector<double>(ringsPerFeature.begin(), ringsPerFeature.end()));
    setSlot(3, lonlat);
    env->DeleteLocalRef(dblArrCls);
    return result;
}

// 取指定矢量层某 FID 要素的当前实际渲染色：返回 [fillArgb, lineArgb]（#AARRGGBB 打包 Int，含符号位），
// 未命中/几何未就绪返回 null。供宿主点击要素弹层色块「所见即所得」回显（尤其 KML 逐要素原色）。
JNIEXPORT jintArray JNICALL
Java_com_zys_globecore_NativeLib_nativeFeatureRenderColor(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                             jint index, jlong fid) {
    if (handle == 0) return nullptr;
    unsigned int fill = 0, line = 0;
    if (!toGlobeEngine(handle)->featureRenderColor(index, static_cast<long long>(fid), fill, line))
        return nullptr;
    jintArray arr = env->NewIntArray(2);
    if (arr == nullptr) return nullptr;
    const jint buf[2] = {static_cast<jint>(fill), static_cast<jint>(line)};
    env->SetIntArrayRegion(arr, 0, 2, buf);
    return arr;
}

// 设置屏幕密度（displayMetrics.density），作为 LOD 细分判据的 densityFactor（对齐 参考实现 setupViewport）
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetDisplayDensity(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                            jdouble density) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setDisplayDensity(density);
}

// 设置定位标记（蓝点）的地理坐标、可见性与移动方位角（headingDeg<0 不画箭头）：屏幕固定尺寸叠加在瓦片之上
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetLocationMarker(JNIEnv * /*env*/, jobject /*thiz*/, jlong handle,
                                                            jdouble lonDeg, jdouble latDeg, jboolean visible,
                                                            jdouble headingDeg) {
    if (handle == 0) return;
    toGlobeEngine(handle)->setLocationMarker(lonDeg, latDeg, visible == JNI_TRUE, headingDeg);
}

// 设置定位标记罗盘图标（ARGB IntArray → RGBA 像素）：有图标时 drawLocationMarker 画纹理四边形替代蓝点
// （对齐原主界面 LocationModel ic_compass 方式）。复用 iconArgbToRgba 转换口径。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetLocationMarkerIcon(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                                 jintArray iconArgb, jint iconW, jint iconH) {
    if (handle == 0) return;
    std::vector<uint8_t> rgba = iconArgbToRgba(env, iconArgb, iconW, iconH);
    toGlobeEngine(handle)->setLocationMarkerIcon(std::move(rgba), iconW, iconH);
}

// 设置矢量标注字体文件路径（path 为空则 native 自动探测系统 CJK 字体）：加载字形图集，供标注文本渲染。
// 宜在 UI 线程调用（FontAtlas::load 含一次文件 IO，纯 CPU、线程安全，不涉 GL 资源）。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetFontPath(JNIEnv *env, jobject /*thiz*/, jlong handle, jstring path) {
    if (handle == 0) return;
    std::string p;
    if (path != nullptr) {
        const char *u = env->GetStringUTFChars(path, nullptr);
        if (u != nullptr) {
            p = u;
            env->ReleaseStringUTFChars(path, u);
        }
    }
    toGlobeEngine(handle)->setFontPath(p);
}

// 注册「需重绘」回调：RENDERMODE_WHEN_DIRTY 下，native 在瓦片异步加载/解码完成、需再画一帧时
// 回调 Kotlin GLSurfaceView.requestRender（线程安全，可从 GL 线程调）。view 传 NativeMapView 实例。
JNIEXPORT void JNICALL
Java_com_zys_globecore_NativeLib_nativeSetRenderCallback(JNIEnv *env, jobject /*thiz*/, jlong handle,
                                                            jobject view) {
    if (handle == 0 || view == nullptr) return;
    auto holder = std::make_shared<RenderCallbackHolder>();
    env->GetJavaVM(&holder->vm);
    holder->viewRef = env->NewGlobalRef(view);
    jclass cls = env->GetObjectClass(view);
    if (cls != nullptr) {
        holder->requestRenderMid = env->GetMethodID(cls, "requestRender", "()V");
        env->DeleteLocalRef(cls);
    }
    if (holder->requestRenderMid == nullptr) return; // 无 requestRender 方法：holder 随 shared_ptr 销毁，自动释放全局引用
    toGlobeEngine(handle)->setRenderCallback([holder]() {
        if (holder->vm == nullptr || holder->viewRef == nullptr || holder->requestRenderMid == nullptr) return;
        JNIEnv *env = nullptr;
        // drawFrame 经 nativeDrawFrame 进入，GL 线程已附加到 JVM，GetEnv 直接取得 env
        if (holder->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) return;
        env->CallVoidMethod(holder->viewRef, holder->requestRenderMid);
        if (env->ExceptionCheck()) env->ExceptionClear();
    });
}

} // extern "C"
