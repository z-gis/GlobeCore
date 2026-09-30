// napi_globe_engine.cpp —— GlobeEngine 渲染引擎 NAPI 导出（对应 Android jni/GlobeEngineJni.cpp）。
//
// 移植口径：函数名、参数顺序、返回值语义与 NativeLib.kt 的 37 个 external fun 逐一对齐，
// ArkTS 门面（NativeMapView.ets）与 Kotlin 门面同构；差异仅在 GL 宿主：
//   - Android 的 surfaceCreated/Changed/drawFrame/releaseGl 由 Kotlin GLSurfaceView 转发，
//     鸿蒙无对应视图宿主 → 不导出，改由 nativeAttach/nativeDetach + XComponentBridge/EGL 渲染线程管理；
//   - nativeSetRenderCallback(view) 的反射 requestRender → 鸿蒙在 MapRenderHost 构造时
//     内部接线（脏标记唤醒），ArkTS 手势侧只需 nativeRequestRender(handle) 显式刷帧。
//
// 句柄模型同 Android：nativeCreate 返回 GlobeEngine* 地址（number），后续调用透传；0 直接返回。
// attach 的 XComponent 上下文对象经 napi_unwrap 解出 OH_NativeXComponent*（ArkTS 侧
// XComponent.getContext(ContextType.OpenGL, ...) 返回值原样传入）。
#include <napi/native_api.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/GlobeEngine.h"
#include "geom/Camera.h"
#include "layer/VectorLayer.h"
#include "net/HttpClient.h"
#include "util/Log.h"

#include "XComponentBridge.h"
#include "napi_util.h"

using globecore::GlobeEngine;

namespace {

inline GlobeEngine *toGlobeEngine(int64_t handle) {
    return reinterpret_cast<GlobeEngine *>(handle);
}

/// handle → XComponentBridge 注册表（attach 建立、detach/destroy 拆除）；
/// 引擎重绘回调经 MapRenderHost 在 attach 内部接线，此表仅服务 requestRender/detach 反查。
std::mutex g_bridgesMtx;
std::unordered_map<int64_t, wwohos::XComponentBridge *> g_bridges;

// ── 单击拾取：渲染线程→JS 主线程的 TSFN 投递（native 手势确认后回调 ArkTS）──
// SURFACE 型 XComponent 触摸只进 native（ArkTS .onTouch 不触发），故单击事件源在渲染线程，
// 经 threadsafe function 转投 JS 线程触发用户回调（坐标 vp，与手势通路同口径）。
struct TapPack {
    float x;
    float y;
};

struct TapListener {
    napi_threadsafe_function tsfn = nullptr;
};

std::mutex g_tapMtx;
std::unordered_map<int64_t, TapListener> g_tapListeners;

void TapCallJs(napi_env env, napi_value jsCb, void * /*context*/, void *data) {
    std::unique_ptr<TapPack> pack(static_cast<TapPack *>(data)); // 销毁广播时 data 为 null
    if (env == nullptr || jsCb == nullptr || pack == nullptr) return;
    napi_value args[2];
    napi_create_double(env, static_cast<double>(pack->x), &args[0]);
    napi_create_double(env, static_cast<double>(pack->y), &args[1]);
    napi_value thisArg = nullptr;
    napi_get_undefined(env, &thisArg);
    napi_call_function(env, thisArg, jsCb, 2, args, nullptr);
}

/// 把 handle 对应的投递器装进指定桥（调用方须已持 g_bridgesMtx 之外的安全窗口）；
/// 取 g_tapListeners 快照构造 std::function，桥不存在（尚未 attach）则留待 attach 时补装。
std::function<void(float, float)> MakeTapDispatcher(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end() || it->second.tsfn == nullptr) return nullptr;
    napi_threadsafe_function tsfn = it->second.tsfn;
    return [tsfn](float x, float y) {
        auto *pack = new TapPack{x, y};
        if (napi_call_threadsafe_function(tsfn, pack, napi_tsfn_nonblocking) != napi_ok) {
            delete pack; // 队列已销毁/满：丢弃本次单击（宁缺勿悬）
        }
    };
}

void ReleaseTapListener(int64_t handle) {
    std::lock_guard<std::mutex> lock(g_tapMtx);
    auto it = g_tapListeners.find(handle);
    if (it == g_tapListeners.end()) return;
    if (it->second.tsfn != nullptr) {
        napi_release_threadsafe_function(it->second.tsfn, napi_tsfn_release); // finalizer 在 JS 线程跑
    }
    g_tapListeners.erase(it);
}

/// #AARRGGBB 整型颜色拆为 [0,1] 浮点 RGBA（口径同 JNI unpackArgb）
inline void unpackArgb(int32_t c, float &r, float &g, float &b, float &a) {
    a = static_cast<float>((c >> 24) & 0xFF) / 255.0f;
    r = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    g = static_cast<float>((c >> 8) & 0xFF) / 255.0f;
    b = static_cast<float>(c & 0xFF) / 255.0f;
}

/// 0xAARRGGBB int32 像素 → RGBA 字节（口径同 JNI iconArgbToRgba；尺寸不匹配返回空 → 点回退画圆）
inline std::vector<uint8_t> iconArgbToRgba(const std::vector<int32_t> &argb, int32_t w, int32_t h) {
    std::vector<uint8_t> out;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (argb.empty() || w <= 0 || h <= 0 || argb.size() != n) return out;
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t p = static_cast<uint32_t>(argb[i]);
        out[i * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF); // R
        out[i * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);  // G
        out[i * 4 + 2] = static_cast<uint8_t>(p & 0xFF);         // B
        out[i * 4 + 3] = static_cast<uint8_t>((p >> 24) & 0xFF); // A
    }
    return out;
}

/// double[] → long long 向量（FID 以 double 传输，<2^53 精确；口径同 toLongVec）
inline std::vector<long long> toLongVec(const std::vector<double> &v) {
    std::vector<long long> out;
    out.reserve(v.size());
    for (double d : v) out.push_back(static_cast<long long>(d));
    return out;
}

/// double[] → int 向量（顶点/环计数；口径同 toIntVec）
inline std::vector<int> toIntVec(const std::vector<double> &v) {
    std::vector<int> out;
    out.reserve(v.size());
    for (double d : v) out.push_back(static_cast<int>(d));
    return out;
}

// ── 生命周期 ──

napi_value NativeCreate(napi_env env, napi_callback_info /*info*/) {
    return wwohos::NapiMakeInt64(env, reinterpret_cast<int64_t>(new GlobeEngine()));
}

napi_value NativeDestroy(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    // 安全网：未 detach 的桥先拆除（停渲染线程再删 GlobeEngine，防 EGL 线程悬挂）
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) {
            it->second->shutdown();
            delete it->second;
            g_bridges.erase(it);
        }
    }
    ReleaseTapListener(handle); // join 之后：渲染线程已停，不会再碰 tsfn
    delete toGlobeEngine(handle);
    return nullptr;
}

/// nativeAttach(handle, surfaceId, width, height) → bool：用 surfaceId 直接绑定 surface 并起 EGL 渲染线程
napi_value NativeAttach(napi_env env, napi_callback_info info) {
    napi_value args[4];
    wwohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) {
        return wwohos::NapiMakeBool(env, false);
    }
    int64_t surfaceId = 0;
    wwohos::NapiGetInt64(env, args[1], surfaceId);
    int32_t width = 0;
    int32_t height = 0;
    wwohos::NapiGetInt32(env, args[2], width);
    wwohos::NapiGetInt32(env, args[3], height);
    auto *bridge = wwohos::XComponentBridge::attach(env, toGlobeEngine(handle),
                                                    static_cast<uint64_t>(surfaceId), width, height);
    if (bridge == nullptr) return wwohos::NapiMakeBool(env, false);
    std::lock_guard<std::mutex> lock(g_bridgesMtx);
    auto it = g_bridges.find(handle);
    if (it != g_bridges.end()) { // 重挂：旧桥拆除（ ArkTS 侧应先 detach，此为防御路径）
        it->second->shutdown();
        delete it->second;
    }
    g_bridges[handle] = bridge;
    // attach 前已注册的单击回调补装进桥（桥内部再转 host）
    bridge->setSingleTapCallback(MakeTapDispatcher(handle));
    return wwohos::NapiMakeBool(env, true);
}

/// nativeDetach(handle)：注销 surface 回调 + 停渲染线程（GlobeEngine 数据态保留，可再 attach）
napi_value NativeDetach(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    wwohos::XComponentBridge *bridge = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) {
            bridge = it->second;
            g_bridges.erase(it);
        }
    }
    if (bridge != nullptr) {
        bridge->shutdown();
        delete bridge;
    }
    ReleaseTapListener(handle); // 与桥同生命周期：detach 后重 attach 需重设回调（或由上层重调）
    return nullptr;
}

/// nativeSetTapCallback(handle, cb)：注册/更新（cb=null 反注册）单击拾取回调。
/// 事件源：native 手势识别确认的单击（短按未移动且未触发长按），坐标 vp。
napi_value NativeSetTapCallback(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;

    ReleaseTapListener(handle); // 替换语义：先拆旧投递器

    napi_valuetype vt = napi_undefined;
    if (args[1] != nullptr) napi_typeof(env, args[1], &vt);
    if (vt != napi_function) return nullptr; // 传 null/undefined = 反注册

    TapListener reg;
    napi_value asyncName = nullptr;
    napi_create_string_utf8(env, "globecore-tap", NAPI_AUTO_LENGTH, &asyncName);
    // JS 回调函数直接交给 tsfn（napi 内部持持久引用，CallJs 的 jsCb 参数即此函数）；
    // 队列上限 16：单击是低频事件，积压只出现在快速连点，满了丢弃新击而非无限排队。
    // 注意：OHOS 签名的 context 在 call_js_cb 之前，与 Node 参数序相反。
    const napi_status st = napi_create_threadsafe_function(
        env, args[1], nullptr, asyncName, 16 /*max_queue_size*/, 1 /*initial_thread_count*/,
        nullptr /*thread_finalize_data*/, nullptr /*thread_finalize_cb*/, nullptr /*context*/,
        TapCallJs, &reg.tsfn);
    if (st != napi_ok) {
        LOGE("nativeSetTapCallback: create tsfn failed: %d", st);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(g_tapMtx);
        g_tapListeners[handle] = reg;
    }
    // 装进当前桥（若已 attach）；未 attach 则 NativeAttach 末尾补装
    {
        std::lock_guard<std::mutex> lock(g_bridgesMtx);
        auto it = g_bridges.find(handle);
        if (it != g_bridges.end()) it->second->setSingleTapCallback(MakeTapDispatcher(handle));
    }
    return nullptr;
}

/// 手势/属性变更后显式刷帧（对应 Kotlin view.requestRender()；surface 未就绪时忽略）
napi_value NativeRequestRender(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    std::lock_guard<std::mutex> lock(g_bridgesMtx);
    auto it = g_bridges.find(handle);
    if (it != g_bridges.end()) it->second->requestRender();
    return nullptr;
}

// ── 相机 / 视图模式 / 手势 ──

napi_value NativeSetCamera(napi_env env, napi_callback_info info) {
    napi_value args[9];
    wwohos::NapiGetArgs(env, info, 9, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double lat = 0, lon = 0, alt = 0, heading = 0, tilt = 0, roll = 0, fov = 0;
    int32_t altMode = 0;
    wwohos::NapiGetDouble(env, args[1], lat);
    wwohos::NapiGetDouble(env, args[2], lon);
    wwohos::NapiGetDouble(env, args[3], alt);
    wwohos::NapiGetDouble(env, args[4], heading);
    wwohos::NapiGetDouble(env, args[5], tilt);
    wwohos::NapiGetDouble(env, args[6], roll);
    wwohos::NapiGetDouble(env, args[7], fov);
    wwohos::NapiGetInt32(env, args[8], altMode);
    globecore::Camera cam;
    cam.latitude = lat;
    cam.longitude = lon;
    cam.altitude = alt;
    cam.heading = heading;
    cam.tilt = tilt;
    cam.roll = roll;
    cam.fieldOfView = fov;
    // int → 枚举：越界回退 ABSOLUTE（与 geom/Camera.h 默认一致，同 JNI 版）
    cam.altitudeMode = (altMode >= 0 && altMode <= 3)
                           ? static_cast<globecore::AltitudeMode>(altMode)
                           : globecore::AltitudeMode::ABSOLUTE;
    toGlobeEngine(handle)->setCamera(cam);
    return nullptr;
}

napi_value NativeGetCamera(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeNull(env);
    const globecore::Camera cam = toGlobeEngine(handle)->getCamera();
    const double buf[8] = {
        cam.latitude, cam.longitude, cam.altitude,
        cam.heading, cam.tilt, cam.roll, cam.fieldOfView,
        static_cast<double>(static_cast<int>(cam.altitudeMode)),
    };
    return wwohos::NapiMakeFloat64Array(env, buf, 8);
}

napi_value NativeSetViewMode(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t mode = 0;
    wwohos::NapiGetInt32(env, args[1], mode);
    toGlobeEngine(handle)->setViewMode(mode);
    return nullptr;
}

napi_value NativeGetViewMode(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeInt32(env, 0);
    return wwohos::NapiMakeInt32(env, toGlobeEngine(handle)->viewMode());
}

napi_value NativePanBy(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double dx = 0, dy = 0;
    wwohos::NapiGetDouble(env, args[1], dx);
    wwohos::NapiGetDouble(env, args[2], dy);
    toGlobeEngine(handle)->panByPixels(dx, dy);
    return nullptr;
}

napi_value NativeZoomBy(napi_env env, napi_callback_info info) {
    napi_value args[4];
    wwohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double factor = 1, fx = 0, fy = 0;
    wwohos::NapiGetDouble(env, args[1], factor);
    wwohos::NapiGetDouble(env, args[2], fx);
    wwohos::NapiGetDouble(env, args[3], fy);
    toGlobeEngine(handle)->zoomBy(factor, fx, fy);
    return nullptr;
}

napi_value NativeRotateHeading(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double delta = 0;
    wwohos::NapiGetDouble(env, args[1], delta);
    toGlobeEngine(handle)->rotateHeading(delta);
    return nullptr;
}

napi_value NativeRotateTilt(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double delta = 0;
    wwohos::NapiGetDouble(env, args[1], delta);
    toGlobeEngine(handle)->rotateTilt(delta);
    return nullptr;
}

// ── 瓦片 / 栅格图层 ──

napi_value NativeAddTileLayer(napi_env env, napi_callback_info info) {
    napi_value args[5];
    wwohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    const std::string cacheDir = wwohos::NapiGetString(env, args[1]);
    if (cacheDir.empty()) return nullptr;
    const std::string url = wwohos::NapiGetString(env, args[2]);
    int32_t maxLevel = 0;
    wwohos::NapiGetInt32(env, args[3], maxLevel);
    bool overlay = false;
    wwohos::NapiGetBool(env, args[4], overlay);
    toGlobeEngine(handle)->addTileLayer(cacheDir, url, maxLevel, overlay);
    return nullptr;
}

napi_value NativeSetLayerVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool visible = false;
    wwohos::NapiGetInt32(env, args[1], index);
    wwohos::NapiGetBool(env, args[2], visible);
    toGlobeEngine(handle)->setLayerVisible(index, visible);
    return nullptr;
}

napi_value NativeAddRasterLayer(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeInt32(env, -1);
    const std::string cacheDir = wwohos::NapiGetString(env, args[1]);
    const std::string path = wwohos::NapiGetString(env, args[2]);
    if (cacheDir.empty() || path.empty()) return wwohos::NapiMakeInt32(env, -1);
    return wwohos::NapiMakeInt32(env, toGlobeEngine(handle)->addRasterLayer(cacheDir, path));
}

// ── 矢量图层 ──

napi_value NativeAddVectorLayer(napi_env env, napi_callback_info info) {
    napi_value args[23];
    wwohos::NapiGetArgs(env, info, 23, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeInt32(env, -1);
    const std::string path = wwohos::NapiGetString(env, args[1]);
    if (path.empty()) return wwohos::NapiMakeInt32(env, -1);

    int32_t fillColor = 0, outlineColor = 0, lineColor = 0, pointColor = 0;
    int32_t labelColor = 0, labelOutlineColor = 0;
    double outlineWidth = 0, lineWidth = 0, pointRadiusDp = 0, labelSize = 0;
    bool labelOutline = false;
    wwohos::NapiGetInt32(env, args[2], fillColor);
    wwohos::NapiGetInt32(env, args[3], outlineColor);
    wwohos::NapiGetDouble(env, args[4], outlineWidth);
    wwohos::NapiGetInt32(env, args[5], lineColor);
    wwohos::NapiGetDouble(env, args[6], lineWidth);
    wwohos::NapiGetInt32(env, args[7], pointColor);
    wwohos::NapiGetDouble(env, args[8], pointRadiusDp);
    const std::string labelField = wwohos::NapiGetString(env, args[9]);
    wwohos::NapiGetInt32(env, args[10], labelColor);
    wwohos::NapiGetDouble(env, args[11], labelSize);
    wwohos::NapiGetBool(env, args[12], labelOutline);
    wwohos::NapiGetInt32(env, args[13], labelOutlineColor);
    const std::vector<int32_t> iconArgb = wwohos::NapiGetInt32Array(env, args[14]);
    int32_t iconW = 0, iconH = 0;
    wwohos::NapiGetInt32(env, args[15], iconW);
    wwohos::NapiGetInt32(env, args[16], iconH);
    bool hasExtent = false;
    wwohos::NapiGetBool(env, args[17], hasExtent);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    wwohos::NapiGetDouble(env, args[18], minLon);
    wwohos::NapiGetDouble(env, args[19], minLat);
    wwohos::NapiGetDouble(env, args[20], maxLon);
    wwohos::NapiGetDouble(env, args[21], maxLat);
    // maxFeatures 为第 23 个参数（NativeLib.kt 末位，缺省 0 = 引擎默认上限）
    int32_t maxFeatures = 0;
    if (args[22] != nullptr) wwohos::NapiGetInt32(env, args[22], maxFeatures);

    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = static_cast<float>(outlineWidth);
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = static_cast<float>(lineWidth);
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = static_cast<float>(pointRadiusDp);
    style.labelField = labelField; // 空则整层不标注（对齐主界面门控）
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = static_cast<float>(labelSize);
    style.labelOutline = labelOutline;
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB,
               style.labelOutlineA);

    std::vector<uint8_t> iconRgba = iconArgbToRgba(iconArgb, iconW, iconH);
    const int index = toGlobeEngine(handle)->addVectorLayer(
            path, style, std::move(iconRgba), iconW, iconH,
            hasExtent, minLon, minLat, maxLon, maxLat, maxFeatures);
    return wwohos::NapiMakeInt32(env, index);
}

napi_value NativeUpdateVectorExtent(napi_env env, napi_callback_info info) {
    napi_value args[8];
    wwohos::NapiGetArgs(env, info, 8, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1, maxFeatures = 0;
    bool hasExtent = false;
    wwohos::NapiGetInt32(env, args[1], index);
    wwohos::NapiGetBool(env, args[2], hasExtent);
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    wwohos::NapiGetDouble(env, args[3], minLon);
    wwohos::NapiGetDouble(env, args[4], minLat);
    wwohos::NapiGetDouble(env, args[5], maxLon);
    wwohos::NapiGetDouble(env, args[6], maxLat);
    wwohos::NapiGetInt32(env, args[7], maxFeatures);
    toGlobeEngine(handle)->updateVectorExtent(index, hasExtent, minLon, minLat, maxLon, maxLat,
                                              maxFeatures);
    return nullptr;
}

napi_value NativeHasVectorLoading(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeBool(env, false);
    return wwohos::NapiMakeBool(env, toGlobeEngine(handle)->hasVectorLoading());
}

napi_value NativeSetVectorLayerVisible(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool visible = false;
    wwohos::NapiGetInt32(env, args[1], index);
    wwohos::NapiGetBool(env, args[2], visible);
    toGlobeEngine(handle)->setVectorLayerVisible(index, visible);
    return nullptr;
}

napi_value NativeSetVectorMinLevel(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1, minLevel = 0;
    wwohos::NapiGetInt32(env, args[1], index);
    wwohos::NapiGetInt32(env, args[2], minLevel);
    toGlobeEngine(handle)->setVectorMinLevel(index, minLevel);
    return nullptr;
}

napi_value NativeGetCameraZoomLevel(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeInt32(env, 0);
    return wwohos::NapiMakeInt32(env, toGlobeEngine(handle)->currentZoomLevel());
}

napi_value NativeRemoveVectorLayer(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    wwohos::NapiGetInt32(env, args[1], index);
    toGlobeEngine(handle)->removeVectorLayer(index);
    return nullptr;
}

// ── 动态叠加层 ──

napi_value NativeAddOverlayLayer(napi_env env, napi_callback_info info) {
    napi_value args[15];
    wwohos::NapiGetArgs(env, info, 15, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeInt32(env, -1);
    int32_t fillColor = 0, outlineColor = 0, lineColor = 0, pointColor = 0;
    int32_t labelColor = 0, labelOutlineColor = 0;
    double outlineWidth = 0, lineWidth = 0, pointRadiusDp = 0, labelSize = 0;
    bool labelOutline = false;
    wwohos::NapiGetInt32(env, args[1], fillColor);
    wwohos::NapiGetInt32(env, args[2], outlineColor);
    wwohos::NapiGetDouble(env, args[3], outlineWidth);
    wwohos::NapiGetInt32(env, args[4], lineColor);
    wwohos::NapiGetDouble(env, args[5], lineWidth);
    wwohos::NapiGetInt32(env, args[6], pointColor);
    wwohos::NapiGetDouble(env, args[7], pointRadiusDp);
    wwohos::NapiGetInt32(env, args[8], labelColor);
    wwohos::NapiGetDouble(env, args[9], labelSize);
    wwohos::NapiGetBool(env, args[10], labelOutline);
    wwohos::NapiGetInt32(env, args[11], labelOutlineColor);
    const std::vector<int32_t> iconArgb = wwohos::NapiGetInt32Array(env, args[12]);
    int32_t iconW = 0, iconH = 0;
    wwohos::NapiGetInt32(env, args[13], iconW);
    wwohos::NapiGetInt32(env, args[14], iconH);

    globecore::VectorStyle style;
    unpackArgb(fillColor, style.fillR, style.fillG, style.fillB, style.fillA);
    unpackArgb(outlineColor, style.outlineR, style.outlineG, style.outlineB, style.outlineA);
    style.outlineWidth = static_cast<float>(outlineWidth);
    unpackArgb(lineColor, style.lineR, style.lineG, style.lineB, style.lineA);
    style.lineWidth = static_cast<float>(lineWidth);
    unpackArgb(pointColor, style.pointR, style.pointG, style.pointB, style.pointA);
    style.pointRadiusDp = static_cast<float>(pointRadiusDp);
    unpackArgb(labelColor, style.labelR, style.labelG, style.labelB, style.labelA);
    style.labelSize = static_cast<float>(labelSize);
    style.labelOutline = labelOutline;
    unpackArgb(labelOutlineColor, style.labelOutlineR, style.labelOutlineG, style.labelOutlineB,
               style.labelOutlineA);
    std::vector<uint8_t> iconRgba = iconArgbToRgba(iconArgb, iconW, iconH);
    return wwohos::NapiMakeInt32(env,
                                 toGlobeEngine(handle)->addOverlayLayer(style, std::move(iconRgba), iconW, iconH));
}

napi_value NativeUpdateOverlayPoints(napi_env env, napi_callback_info info) {
    napi_value args[5];
    wwohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    wwohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = wwohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> fids = wwohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<std::string> labels = wwohos::NapiGetStringArray(env, args[4]);
    toGlobeEngine(handle)->updateOverlayPoints(index, lonlat, toLongVec(fids), labels);
    return nullptr;
}

napi_value NativeUpdateOverlayLines(napi_env env, napi_callback_info info) {
    napi_value args[6];
    wwohos::NapiGetArgs(env, info, 6, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    wwohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = wwohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> vertexCounts = wwohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<double> fids = wwohos::NapiGetFloat64Array(env, args[4]);
    const std::vector<std::string> labels = wwohos::NapiGetStringArray(env, args[5]);
    toGlobeEngine(handle)->updateOverlayLines(index, lonlat, toIntVec(vertexCounts), toLongVec(fids),
                                              labels);
    return nullptr;
}

napi_value NativeUpdateOverlayPolygons(napi_env env, napi_callback_info info) {
    napi_value args[7];
    wwohos::NapiGetArgs(env, info, 7, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    wwohos::NapiGetInt32(env, args[1], index);
    const std::vector<double> lonlat = wwohos::NapiGetFloat64Array(env, args[2]);
    const std::vector<double> ringVertexCounts = wwohos::NapiGetFloat64Array(env, args[3]);
    const std::vector<double> ringsPerFeature = wwohos::NapiGetFloat64Array(env, args[4]);
    const std::vector<double> fids = wwohos::NapiGetFloat64Array(env, args[5]);
    const std::vector<std::string> labels = wwohos::NapiGetStringArray(env, args[6]);
    toGlobeEngine(handle)->updateOverlayPolygons(index, lonlat, toIntVec(ringVertexCounts),
                                                 toIntVec(ringsPerFeature), toLongVec(fids), labels);
    return nullptr;
}

napi_value NativeRemoveOverlayLayer(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    wwohos::NapiGetInt32(env, args[1], index);
    toGlobeEngine(handle)->removeOverlayLayer(index);
    return nullptr;
}

napi_value NativeSetOverlayNoPick(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    int32_t index = -1;
    bool noPick = false;
    wwohos::NapiGetInt32(env, args[1], index);
    wwohos::NapiGetBool(env, args[2], noPick);
    toGlobeEngine(handle)->setOverlayNoPick(index, noPick);
    return nullptr;
}

napi_value NativeClearOverlayLayers(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->clearOverlayLayers();
    return nullptr;
}

// ── 拾取 / 反算 / 要素几何 ──

napi_value NativePickVector(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeNull(env);
    double sx = 0, sy = 0;
    wwohos::NapiGetDouble(env, args[1], sx);
    wwohos::NapiGetDouble(env, args[2], sy);
    int layerIndex = -1;
    long long fid = -1;
    if (!toGlobeEngine(handle)->pickVector(sx, sy, layerIndex, fid)) return wwohos::NapiMakeNull(env);
    const double buf[2] = {static_cast<double>(layerIndex), static_cast<double>(fid)};
    return wwohos::NapiMakeFloat64Array(env, buf, 2);
}

napi_value NativeScreenToGeo(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeNull(env);
    double sx = 0, sy = 0;
    wwohos::NapiGetDouble(env, args[1], sx);
    wwohos::NapiGetDouble(env, args[2], sy);
    double lon = 0, lat = 0;
    if (!toGlobeEngine(handle)->screenToGeo(sx, sy, lon, lat)) return wwohos::NapiMakeNull(env);
    const double buf[2] = {lon, lat};
    return wwohos::NapiMakeFloat64Array(env, buf, 2);
}

/// 命中返回 Array<Float64Array> [ [type], ringCounts, ringsPerFeature, lonlat ]（口径同 JNI double[4][]）
napi_value NativeFeatureGeometry(napi_env env, napi_callback_info info) {
    napi_value args[3];
    wwohos::NapiGetArgs(env, info, 3, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return wwohos::NapiMakeNull(env);
    int32_t layerIndex = -1;
    double fidD = 0;
    wwohos::NapiGetInt32(env, args[1], layerIndex);
    wwohos::NapiGetDouble(env, args[2], fidD);
    int type = -1;
    std::vector<double> lonlat;
    std::vector<int> ringCounts, ringsPerFeature;
    if (!toGlobeEngine(handle)->featureGeometry(layerIndex, static_cast<long long>(fidD), type,
                                                lonlat, ringCounts, ringsPerFeature)) {
        return wwohos::NapiMakeNull(env);
    }
    std::vector<double> typeVec = {static_cast<double>(type)};
    std::vector<double> ringCountsD(ringCounts.begin(), ringCounts.end());
    std::vector<double> ringsPerFeatureD(ringsPerFeature.begin(), ringsPerFeature.end());
    napi_value result = nullptr;
    napi_create_array_with_length(env, 4, &result);
    napi_set_element(env, result, 0, wwohos::NapiMakeFloat64Array(env, typeVec));
    napi_set_element(env, result, 1, wwohos::NapiMakeFloat64Array(env, ringCountsD));
    napi_set_element(env, result, 2, wwohos::NapiMakeFloat64Array(env, ringsPerFeatureD));
    napi_set_element(env, result, 3, wwohos::NapiMakeFloat64Array(env, lonlat));
    return result;
}

// ── 显示 / 标记 / 字体 ──

napi_value NativeSetDisplayDensity(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double density = 1;
    wwohos::NapiGetDouble(env, args[1], density);
    toGlobeEngine(handle)->setDisplayDensity(density);
    return nullptr;
}

napi_value NativeSetLocationMarker(napi_env env, napi_callback_info info) {
    napi_value args[5];
    wwohos::NapiGetArgs(env, info, 5, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    double lon = 0, lat = 0, heading = -1;
    bool visible = false;
    wwohos::NapiGetDouble(env, args[1], lon);
    wwohos::NapiGetDouble(env, args[2], lat);
    wwohos::NapiGetBool(env, args[3], visible);
    wwohos::NapiGetDouble(env, args[4], heading);
    toGlobeEngine(handle)->setLocationMarker(lon, lat, visible, heading);
    return nullptr;
}

napi_value NativeSetLocationMarkerIcon(napi_env env, napi_callback_info info) {
    napi_value args[4];
    wwohos::NapiGetArgs(env, info, 4, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    const std::vector<int32_t> argb = wwohos::NapiGetInt32Array(env, args[1]);
    int32_t w = 0, h = 0;
    wwohos::NapiGetInt32(env, args[2], w);
    wwohos::NapiGetInt32(env, args[3], h);
    std::vector<uint8_t> rgba = iconArgbToRgba(argb, w, h);
    toGlobeEngine(handle)->setLocationMarkerIcon(std::move(rgba), w, h);
    return nullptr;
}

napi_value NativeSetFontPath(napi_env env, napi_callback_info info) {
    napi_value args[2];
    wwohos::NapiGetArgs(env, info, 2, args);
    int64_t handle = 0;
    if (!wwohos::NapiGetInt64(env, args[0], handle) || handle == 0) return nullptr;
    toGlobeEngine(handle)->setFontPath(wwohos::NapiGetString(env, args[1]));
    return nullptr;
}

/// nativeSetCaBundle(path)：设置 HTTPS 证书校验用的 CA 证书包（cacert.pem）绝对路径（全局、无句柄）。
/// 对应引擎 HttpClient::setCaBundle。OHOS 无 Android 的 /system/etc/security/cacerts，随包内置 CA 后
/// 由 ArkTS 启动时拷入沙箱并传入，使瓦片 HTTPS 首握手即校验通过，免逐块“校验失败→降级重试”的双程时延。
/// 传空串回退引擎默认行为（Android 系统证书目录）。
napi_value NativeSetCaBundle(napi_env env, napi_callback_info info) {
    napi_value args[1];
    wwohos::NapiGetArgs(env, info, 1, args);
    globecore::HttpClient::setCaBundle(wwohos::NapiGetString(env, args[0]));
    return nullptr;
}

} // namespace

namespace wwohos {

/// 由 napi_init.cpp 统一注册：挂全部 GlobeEngine 导出到 exports（模块名 libglobecore）
napi_value RegisterGlobeEngine(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"nativeCreate", nullptr, NativeCreate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeDestroy", nullptr, NativeDestroy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAttach", nullptr, NativeAttach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeDetach", nullptr, NativeDetach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRequestRender", nullptr, NativeRequestRender, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetCamera", nullptr, NativeSetCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetCamera", nullptr, NativeGetCamera, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetViewMode", nullptr, NativeSetViewMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetViewMode", nullptr, NativeGetViewMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePanBy", nullptr, NativePanBy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeZoomBy", nullptr, NativeZoomBy, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRotateHeading", nullptr, NativeRotateHeading, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRotateTilt", nullptr, NativeRotateTilt, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddTileLayer", nullptr, NativeAddTileLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLayerVisible", nullptr, NativeSetLayerVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddRasterLayer", nullptr, NativeAddRasterLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddVectorLayer", nullptr, NativeAddVectorLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateVectorExtent", nullptr, NativeUpdateVectorExtent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeHasVectorLoading", nullptr, NativeHasVectorLoading, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetVectorLayerVisible", nullptr, NativeSetVectorLayerVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetVectorMinLevel", nullptr, NativeSetVectorMinLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeGetCameraZoomLevel", nullptr, NativeGetCameraZoomLevel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRemoveVectorLayer", nullptr, NativeRemoveVectorLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeAddOverlayLayer", nullptr, NativeAddOverlayLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayPoints", nullptr, NativeUpdateOverlayPoints, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayLines", nullptr, NativeUpdateOverlayLines, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeUpdateOverlayPolygons", nullptr, NativeUpdateOverlayPolygons, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeRemoveOverlayLayer", nullptr, NativeRemoveOverlayLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetOverlayNoPick", nullptr, NativeSetOverlayNoPick, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeClearOverlayLayers", nullptr, NativeClearOverlayLayers, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativePickVector", nullptr, NativePickVector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetTapCallback", nullptr, NativeSetTapCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeScreenToGeo", nullptr, NativeScreenToGeo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeFeatureGeometry", nullptr, NativeFeatureGeometry, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetDisplayDensity", nullptr, NativeSetDisplayDensity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLocationMarker", nullptr, NativeSetLocationMarker, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetLocationMarkerIcon", nullptr, NativeSetLocationMarkerIcon, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetFontPath", nullptr, NativeSetFontPath, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"nativeSetCaBundle", nullptr, NativeSetCaBundle, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}

} // namespace wwohos
