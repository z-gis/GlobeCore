// XComponentBridge 实现。见 XComponentBridge.h 顶部时序说明。
#include "XComponentBridge.h"

#include <cinttypes>
#include <chrono>
#include <cmath>
#include <utility>

#include <native_window/external_window.h> // OH_NativeWindow_CreateNativeWindowFromSurfaceId / _DestroyNativeWindow

#include "core/GlobeEngine.h"
#include "util/Log.h"

namespace gcohos {

// ── 组件 → 桥实例注册表（静态回调只带 component 指针，据此反查）──
std::mutex XComponentBridge::s_registryMtx;
std::unordered_map<OH_NativeXComponent *, XComponentBridge *> XComponentBridge::s_byComponent;

XComponentBridge::XComponentBridge(OH_NativeXComponent *component, globecore::GlobeEngine *engine)
    : component_(component), engine_(engine) {
    callbacks_.OnSurfaceCreated = onSurfaceCreatedCB;
    callbacks_.OnSurfaceChanged = onSurfaceChangedCB;
    callbacks_.OnSurfaceDestroyed = onSurfaceDestroyedCB;
    // 必须注册 DispatchTouchEvent：本 API 上 OH_NativeXComponent_RegisterCallback 不接受空成员
    // （置空会返回 -2 导致 attach 失败）；且 SURFACE 型 XComponent 的触摸只经此 native 回调投递，
    // ArkTS .onTouch 不触发，故手势识别直接在本回调里做（见 dispatchTouchEventCB）。
    callbacks_.DispatchTouchEvent = dispatchTouchEventCB;
}

XComponentBridge::~XComponentBridge() {
    shutdown();
}

XComponentBridge *XComponentBridge::attach(napi_env env, globecore::GlobeEngine *engine,
                                           uint64_t surfaceId, int32_t width, int32_t height) {
    if (engine == nullptr) return nullptr;
    // API 12+：onLoad 的 context 不承载组件指针；官方取法是从 exports 的 OH_NATIVE_XCOMPONENT_OBJ unwrap。
    OH_NativeXComponent *component = ResolveNativeXComponent(env);
    if (component == nullptr) {
        LOGE("XComponentBridge::attach: ResolveNativeXComponent null (libraryname must be 'globecore')");
        return nullptr;
    }
    // 关键：SURFACE 型 XComponent 在 onLoad 前 surface 往往已创建，RegisterCallback 会错过 OnSurfaceCreated。
    // 故不依赖回调，直接用 surfaceId 造 OHNativeWindow，attach 当下即建窗口 + 建渲染线程出首帧。
    OHNativeWindow *window = nullptr;
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window) != 0 || window == nullptr) {
        LOGE("XComponentBridge::attach: CreateNativeWindowFromSurfaceId(%" PRIu64 ") failed", surfaceId);
        return nullptr;
    }

    auto *bridge = new XComponentBridge(component, engine);
    bridge->window_ = window;
    bridge->ownsWindow_ = true; // 本桥拥有该窗口，shutdown 时销毁
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        s_byComponent[component] = bridge; // 静态回调只带 component，经注册表反查实例
    }
    // 注册回调：接管后续 OnSurfaceChanged / OnSurfaceDestroyed（迟到的 OnSurfaceCreated 由 host_ 去重）
    const int32_t rc = OH_NativeXComponent_RegisterCallback(component, &bridge->callbacks_);
    if (rc != 0) {
        LOGE("OH_NativeXComponent_RegisterCallback failed: %d", rc);
        {
            std::lock_guard<std::mutex> lock(s_registryMtx);
            s_byComponent.erase(component);
        }
        delete bridge; // ~bridge → shutdown() 销毁 ownsWindow_ 的 window
        return nullptr;
    }
    // 立即起 EGL 渲染线程并下发尺寸（首帧不等 surface 回调）
    bridge->host_ = std::make_unique<MapRenderHost>(engine, window);
    // attach 前已注册的单击回调补装进 host（NAPI setTapCallback 与 XComponent onLoad 无固定先后）
    bridge->host_->setSingleTapCallback(bridge->tapCb_);
    // 长按到点回调：渲染线程触发后转投本桥 handleLongPress（功能待定，当前仅置长按标志
    // 抑制随后的单击/惯性）。bridge 拥有 host_（shutdown 先于析构），捕获 this 安全。
    bridge->host_->setLongPressCallback([bridge](float x, float y) {
        bridge->handleLongPress(x, y);
    });
    int32_t w = width;
    int32_t h = height;
    if (w <= 0 || h <= 0) {
        uint64_t sw = 0;
        uint64_t sh = 0;
        if (OH_NativeXComponent_GetXComponentSize(component, window, &sw, &sh) ==
                OH_NATIVEXCOMPONENT_RESULT_SUCCESS && sw > 0 && sh > 0) {
            w = static_cast<int32_t>(sw);
            h = static_cast<int32_t>(sh);
        }
    }
    if (w > 0 && h > 0) {
        bridge->host_->setSurfaceSize(w, h);
    }
    LOGI("XComponentBridge attached: comp=%p surfaceId=%" PRIu64 " %dx%d",
         static_cast<void *>(component), surfaceId, w, h);
    return bridge;
}

void XComponentBridge::shutdown() {
    if (component_ == nullptr) return; // 已 shutdown（幂等）
    // OHOS NDK 未提供 surface 回调的反注册接口（仅 RegisterCallback）；
    // 此处依赖 host_.reset() join 渲染线程 + 注册表 erase 完成拆除。
    host_.reset(); // join 渲染线程：GL 线程内 releaseGl + EGL 销毁
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        s_byComponent.erase(component_);
    }
    // 仅销毁本桥经 CreateNativeWindowFromSurfaceId 拥有的窗口（回调下发的窗口不归本桥所有）
    if (ownsWindow_ && window_ != nullptr) {
        OH_NativeWindow_DestroyNativeWindow(window_);
    }
    window_ = nullptr;
    ownsWindow_ = false;
    component_ = nullptr;
    engine_ = nullptr;
}

void XComponentBridge::requestRender() {
    if (host_) host_->requestRender();
}

void XComponentBridge::onSurfaceCreatedCB(OH_NativeXComponent *component, void *window) {
    XComponentBridge *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        auto it = s_byComponent.find(component);
        if (it != s_byComponent.end()) self = it->second;
    }
    if (self == nullptr || window == nullptr) return;
    if (self->host_) return; // 重复创建回调（理论不发生）：忽略，避免双渲染线程
    // surface 就绪：起 EGL 渲染线程绑定该窗口（GlobeEngine 仍存活，GL 资源在其上重建）
    self->host_ = std::make_unique<MapRenderHost>(self->engine_, static_cast<OHNativeWindow *>(window));
    // 宽高在 OnSurfaceChanged 中下发；部分设备不回调 changed，这里主动取一次初值
    uint64_t w = 0;
    uint64_t h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) ==
            OH_NATIVEXCOMPONENT_RESULT_SUCCESS &&
        w > 0 && h > 0) {
        self->host_->setSurfaceSize(static_cast<int32_t>(w), static_cast<int32_t>(h));
    }
    LOGI("XComponent surface created: %" PRIu64 "x%" PRIu64, w, h);
}

void XComponentBridge::onSurfaceChangedCB(OH_NativeXComponent *component, void *window) {
    XComponentBridge *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        auto it = s_byComponent.find(component);
        if (it != s_byComponent.end()) self = it->second;
    }
    if (self == nullptr || self->host_ == nullptr || window == nullptr) return;
    uint64_t w = 0;
    uint64_t h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) !=
        OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    self->host_->setSurfaceSize(static_cast<int32_t>(w), static_cast<int32_t>(h));
}

void XComponentBridge::onSurfaceDestroyedCB(OH_NativeXComponent *component, void *window) {
    (void)window;
    XComponentBridge *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        auto it = s_byComponent.find(component);
        if (it != s_byComponent.end()) self = it->second;
    }
    if (self == nullptr) return;
    // 窗口即将失效：立即停渲染线程（内部完成 releaseGl + EGL 销毁），GlobeEngine 数据态保留待重挂
    self->host_.reset();
    LOGI("XComponent surface destroyed");
}

void XComponentBridge::dispatchTouchEventCB(OH_NativeXComponent *component, void *window) {
    // SURFACE 型 XComponent 的触摸只经此 native 回调投递（ArkTS .onTouch 不触发），故手势在此识别。
    XComponentBridge *self = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_registryMtx);
        auto it = s_byComponent.find(component);
        if (it != s_byComponent.end()) self = it->second;
    }
    if (self == nullptr || window == nullptr) return;
    OH_NativeXComponent_TouchEvent ev;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    self->handleTouch(ev);
}

namespace {
constexpr float kTouchSlop = 8.0f;        // vp：越过此位移才判定拖动
// 长按判定时长：单指按下保持（未越过 touchSlop 拖动、未抬离）超此时长即确认为长按。
constexpr int64_t kLongPressTimeoutNs = 500LL * 1000 * 1000; // 500ms
// 俯仰灵敏度直接采用 参考实现 口径：tiltDelta = 180° · Δcy / 视口高（见 handleTouch），不再用自定
// 的固定「度/vp」常量（旧 0.15/0.25 为拍脑袋值，与 参考实现的 180°/屏高不一致）。
// 旋转/俯仰解耦阈值（度/事件）：双指连线角逐帧增量绝对值超此值判为「旋转手势」，本帧不据质心 Y 调仰角
// （对齐 参考实现：旋转仅改 heading、tilt 为独立手势；我们保留双指俯仰故需此阈值近似解耦）
constexpr double kRotateSuppressTiltDeg = 0.8;
// 惯性滑动：抬指瞬时速度低于此不起惯性；高于上限钳住（防极限甩动飞出国）；单位 vp/s
constexpr double kFlingMinVpPerSec = 60.0;
constexpr double kFlingMaxVpPerSec = 12000.0;

inline int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline double pinchAngleDeg(float ax, float ay, float bx, float by) {
    return std::atan2(static_cast<double>(by - ay), static_cast<double>(bx - ax)) * 180.0 / M_PI;
}
} // namespace

void XComponentBridge::handleTouch(const OH_NativeXComponent_TouchEvent &ev) {
    if (engine_ == nullptr) return;
    const uint32_t n = ev.numPoints;
    if (n == 0) return;
    // 主触点：touchPoints[0]（x/y 为相对 XComponent 左上角的 vp 坐标，与引擎既有通路同口径）
    const float x0 = ev.touchPoints[0].x;
    const float y0 = ev.touchPoints[0].y;

    switch (ev.type) {
        case OH_NATIVEXCOMPONENT_DOWN: {
            gDown_ = true;
            gDragging_ = false;
            gPinching_ = false;
            gHadMulti_ = false;
            gVelRefTsNs_ = 0;
            gLongPressFired_ = false; // 新按下序列复位长按标志
            if (host_) host_->cancelFling(); // 新按下接管相机：中止进行中的惯性滑动
            gLastX_ = gDownX_ = x0;
            gLastY_ = gDownY_ = y0;
            if (n >= 2) {
                gPinching_ = true;
                gDragging_ = true;
                gHadMulti_ = true;
                gLastPinchDist_ = std::hypot(ev.touchPoints[1].x - x0, ev.touchPoints[1].y - y0);
                gLastPinchAngleDeg_ = pinchAngleDeg(x0, y0, ev.touchPoints[1].x, ev.touchPoints[1].y);
                gLastCentroidY_ = (y0 + ev.touchPoints[1].y) * 0.5;
                if (host_) host_->cancelPendingLongPress(); // 双指不是长按
            } else if (host_) {
                // 单指按下装载长按候选：到时若未移动/未抬离则由渲染线程触发 handleLongPress
                host_->postPendingLongPress(x0, y0, steadyNowNs() + kLongPressTimeoutNs);
            }
            break;
        }
        case OH_NATIVEXCOMPONENT_MOVE: {
            if (!gDown_) break;
            if (n >= 2) {
                const double dist = std::hypot(ev.touchPoints[1].x - x0, ev.touchPoints[1].y - y0);
                const double cx = (x0 + ev.touchPoints[1].x) * 0.5;
                const double cy = (y0 + ev.touchPoints[1].y) * 0.5;
                if (!gPinching_) { // 中途补上第二指：以本帧为基准，不平移不缩放
                    gPinching_ = true;
                    gDragging_ = true;
                    gHadMulti_ = true;
                    gLastPinchDist_ = dist;
                    gLastPinchAngleDeg_ = pinchAngleDeg(x0, y0, ev.touchPoints[1].x, ev.touchPoints[1].y);
                    gLastCentroidY_ = cy;
                    if (host_) host_->cancelPendingLongPress(); // 进入双指，取消长按候选
                    break;
                }
                // 捏合缩放：逐帧距离比，以质心为锚
                if (dist > 0 && gLastPinchDist_ > 0) {
                    engine_->zoomBy(dist / gLastPinchDist_, cx, cy);
                    requestRender();
                }
                gLastPinchDist_ = dist;
                const bool is3d = (engine_->viewMode() == 1);
                if (is3d) {
                    // 双指旋转：连线角逐帧增量（归一到 (-180,180]）。先算旋转量，据其大小决定本帧是否让位仰角。
                    const double angle = pinchAngleDeg(x0, y0, ev.touchPoints[1].x, ev.touchPoints[1].y);
                    double deltaDeg = angle - gLastPinchAngleDeg_;
                    if (deltaDeg > 180) deltaDeg -= 360;
                    if (deltaDeg <= -180) deltaDeg += 360;
                    if (deltaDeg != 0) {
                        engine_->rotateHeading(deltaDeg);
                        requestRender();
                    }
                    gLastPinchAngleDeg_ = angle;
                    // 双指俯仰：质心 Y 逐帧增量按 参考实现 口径换算——180°·Δcy/视口高（cy 与视口高同为
                    // native 像素单位、与 panByPixels 分母同口径，免密度换算错配）。本帧若为旋转手势
                    // （|Δ角|≥阈值）则抑制仰角（对齐 参考实现 旋转不改 tilt），仅更新基准免旋转结束瞬间质心跳变误触发大仰角。
                    const double vh = static_cast<double>(engine_->viewportHeight());
                    const double tiltDelta = (vh > 0.0) ? -(cy - gLastCentroidY_) * 180.0 / vh : 0.0;
                    if (std::fabs(deltaDeg) < kRotateSuppressTiltDeg && tiltDelta != 0) {
                        engine_->rotateTilt(tiltDelta);
                        requestRender();
                    }
                }
                gLastCentroidY_ = cy;
                break;
            }
            // 单指平移
            if (!gDragging_) {
                if (std::fabs(x0 - gDownX_) + std::fabs(y0 - gDownY_) < kTouchSlop) break;
                gDragging_ = true;
                if (host_) host_->cancelPendingLongPress(); // 已越过拖动阈值，不再是长按
            }
            const double dx = x0 - gLastX_;
            const double dy = y0 - gLastY_;
            gLastX_ = x0;
            gLastY_ = y0;
            if (dx != 0 || dy != 0) {
                engine_->panByPixels(dx, dy);
                requestRender();
            }
            // 速度估计：保留约 60ms 前的参考触点（滑动窗口），UP 时据窗口内平均速度求初速，
            // 免逐帧瞬时速度抖动；ev.timeStamp 为单调 ns，只作差值用。
            if (gVelRefTsNs_ == 0 || ev.timeStamp - gVelRefTsNs_ >= 60000000LL) {
                gVelRefX_ = x0;
                gVelRefY_ = y0;
                gVelRefTsNs_ = ev.timeStamp;
            }
            break;
        }
        case OH_NATIVEXCOMPONENT_UP:
        case OH_NATIVEXCOMPONENT_CANCEL: {
            const bool canceled = (ev.type == OH_NATIVEXCOMPONENT_CANCEL);
            const float ux = ev.x; // 事件级 x/y 即触发本次 UP/CANCEL 的触点
            const float uy = ev.y;
            if (host_) host_->cancelPendingLongPress(); // 抬离/取消：长按候选不再成立
            if (!canceled && !gDragging_ && !gPinching_ && n == 1) {
                const double moved = std::fabs(ux - gDownX_) + std::fabs(uy - gDownY_);
                // 双击放大已移除：短按未移动且本次序列未触发长按，即为单击，直接投递确认
                //（deadline 传当前时刻，渲染线程下一帧触发回调；经 host 通路转投 ArkTS 主线程）。
                if (moved < kTouchSlop && !gLongPressFired_) {
                    if (host_) host_->postPendingTap(ux, uy, steadyNowNs());
                }
            }
            // 惯性滑动：纯单指拖动松手且窗口速度达标时，把初速度交给渲染线程逐帧衰减推进
            if (!canceled && gDragging_ && !gHadMulti_ && !gLongPressFired_ && host_ != nullptr) {
                const int64_t dtNs = ev.timeStamp - gVelRefTsNs_;
                if (gVelRefTsNs_ != 0 && dtNs >= 15000000LL && dtNs <= 250000000LL) {
                    double vx = (ux - gVelRefX_) * 1e9 / static_cast<double>(dtNs);
                    double vy = (uy - gVelRefY_) * 1e9 / static_cast<double>(dtNs);
                    const double sp = std::hypot(vx, vy);
                    if (sp > kFlingMinVpPerSec) {
                        if (sp > kFlingMaxVpPerSec) {
                            const double s = kFlingMaxVpPerSec / sp;
                            vx *= s;
                            vy *= s;
                        }
                        host_->startFling(vx, vy);
                    }
                }
            }
            // 双指中抬起一指（仍 n>=2 之外）：保持序列；全指抬离才复位
            if (n <= 1 && !canceled) {
                // 剩余无指：复位一次手势序列
                gDown_ = false;
                gDragging_ = false;
                gPinching_ = false;
            } else if (canceled) {
                gDown_ = false;
                gDragging_ = false;
                gPinching_ = false;
            } else if (n == 1) {
                gPinching_ = false; // 从双指降为单指
            }
            break;
        }
        default:
            break;
    }
}

void XComponentBridge::handleLongPress(float xVp, float yVp) {
    // 渲染线程在长按候选到点（单指保持按住、未越过拖动阈值、未抬离）时触发，坐标为视图内 vp。
    // 置位标志：本次按下序列后续的 UP 不再按单击/惯性处理（见 handleTouch 的 UP 分支门控）。
    gLongPressFired_ = true;
    // TODO：长按具体功能尚未确定，确定后在此实现（候选如落点上下文菜单 / 打点标注 /
    //   测量起点等），并按需经 NAPI 回调转投 ArkTS 侧（参照 setSingleTapCallback 通路）。
    (void) xVp;
    (void) yVp;
}

} // namespace gcohos
