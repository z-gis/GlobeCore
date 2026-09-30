// EglContext / MapRenderHost 实现。见 EglContext.h 顶部职责说明。
#include "EglContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <native_buffer/native_buffer.h> // NATIVEBUFFER_PIXEL_FMT_RGBA_8888

#include "core/GlobeEngine.h"
#include "util/Log.h"

namespace wwohos {

// ────────────────────────────── EglContext ──────────────────────────────

bool EglContext::initialize(OHNativeWindow *window) {
    if (window == nullptr) return false;

    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed: 0x%x", eglGetError());
        return false;
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) == EGL_FALSE) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        display_ = EGL_NO_DISPLAY;
        return false;
    }
    LOGI("EGL initialized: %d.%d", major, minor);

    // 引擎 render/ 层为 GLES2 指令集，申请 ES2 可渲染配置（ES3.2 驱动向下兼容）
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLint numConfig = 0;
    if (eglChooseConfig(display_, configAttribs, &config_, 1, &numConfig) == EGL_FALSE || numConfig < 1) {
        LOGE("eglChooseConfig failed: 0x%x (num=%d)", eglGetError(), numConfig);
        destroy();
        return false;
    }

    // XComponent 缓冲默认格式对齐 RGBA8888，避免与 EGL config 通道不匹配导致黑屏/偏色
    OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        destroy();
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config_,
                                      reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        destroy();
        return false;
    }

    if (!makeCurrent()) {
        destroy();
        return false;
    }
    window_ = window;
    return true;
}

void EglContext::destroy() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) {
            eglDestroySurface(display_, surface_);
            surface_ = EGL_NO_SURFACE;
        }
        if (context_ != EGL_NO_CONTEXT) {
            eglDestroyContext(display_, context_);
            context_ = EGL_NO_CONTEXT;
        }
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    config_ = nullptr;
    window_ = nullptr;
}

bool EglContext::makeCurrent() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE || context_ == EGL_NO_CONTEXT) {
        return false;
    }
    return eglMakeCurrent(display_, surface_, surface_, context_) != EGL_FALSE;
}

bool EglContext::swapBuffers() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) return false;
    return eglSwapBuffers(display_, surface_) != EGL_FALSE;
}

// ───────────────────────────── MapRenderHost ─────────────────────────────

MapRenderHost::MapRenderHost(globecore::GlobeEngine *engine, OHNativeWindow *window)
    : engine_(engine), window_(window) {
    // 「需重绘」回调接本宿主脏标记：对应 Android nativeSetRenderCallback 的反射 requestRender，
    // 鸿蒙侧纯 native 通路（瓦片/矢量异步到位后驱动 WHEN_DIRTY 模式下的下一帧）。
    engine_->setRenderCallback([this] { requestRender(); });
    thread_ = std::thread([this] { renderLoop(); });
}

MapRenderHost::~MapRenderHost() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // join 后再摘除回调：确保 lambda（捕获 this）不会在本宿主析构后被引擎线程触发
    engine_->setRenderCallback(nullptr);
}

void MapRenderHost::requestRender() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        frameDirty_ = true;
    }
    cv_.notify_all();
}

void MapRenderHost::setSurfaceSize(int32_t width, int32_t height) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (width <= 0 || height <= 0) return;
        width_ = width;
        height_ = height;
        sizeDirty_ = true;
        frameDirty_ = true; // 尺寸变化必然重绘（对齐 GLSurfaceView.onSurfaceChanged 后刷帧）
    }
    cv_.notify_all();
}

namespace {
inline int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr double kFlingStopVpPerSec = 20.0; // 低于此速度停止（vp/s）
constexpr double kFlingDecayPerSec = 6.0;   // 指数衰减时间常数≈167ms，0.7s 内收敛到停止阈值
} // namespace

void MapRenderHost::startFling(double vxVpPerSec, double vyVpPerSec) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        flingVx_ = vxVpPerSec;
        flingVy_ = vyVpPerSec;
        lastFlingNs_ = 0;
    }
    requestRender();
}

void MapRenderHost::cancelFling() {
    std::lock_guard<std::mutex> lock(mtx_);
    flingVx_ = 0.0;
    flingVy_ = 0.0;
    lastFlingNs_ = 0;
}

void MapRenderHost::postPendingTap(float xVp, float yVp, int64_t deadlineNs) {
    std::lock_guard<std::mutex> lock(mtx_);
    pendingTap_ = true;
    pendingTapX_ = xVp;
    pendingTapY_ = yVp;
    pendingTapDeadlineNs_ = deadlineNs;
    cv_.notify_all(); // 唤醒休眠中的循环以装载定时唤醒
}

void MapRenderHost::cancelPendingTap() {
    std::lock_guard<std::mutex> lock(mtx_);
    pendingTap_ = false;
}

void MapRenderHost::postPendingLongPress(float xVp, float yVp, int64_t deadlineNs) {
    std::lock_guard<std::mutex> lock(mtx_);
    pendingLongPress_ = true;
    pendingLongPressX_ = xVp;
    pendingLongPressY_ = yVp;
    pendingLongPressDeadlineNs_ = deadlineNs;
    cv_.notify_all(); // 唤醒休眠中的循环以装载定时唤醒
}

void MapRenderHost::cancelPendingLongPress() {
    std::lock_guard<std::mutex> lock(mtx_);
    pendingLongPress_ = false;
}

bool MapRenderHost::advanceFling(int64_t nowNs) {
    double dx = 0.0;
    double dy = 0.0;
    bool active = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (flingVx_ == 0.0 && flingVy_ == 0.0) return false;
        double dtS = lastFlingNs_ == 0 ? 1.0 / 60.0
                                       : static_cast<double>(nowNs - lastFlingNs_) * 1e-9;
        if (dtS <= 0.0) dtS = 1.0 / 120.0;
        if (dtS > 0.05) dtS = 0.05; // 长停顿（如瓦片解码挤占）后钳住步长，防一次大跳
        dx = flingVx_ * dtS;
        dy = flingVy_ * dtS;
        const double k = std::exp(-dtS * kFlingDecayPerSec);
        flingVx_ *= k;
        flingVy_ *= k;
        active = std::hypot(flingVx_, flingVy_) >= kFlingStopVpPerSec;
        if (!active) {
            flingVx_ = flingVy_ = 0.0;
            lastFlingNs_ = 0;
        } else {
            lastFlingNs_ = nowNs;
        }
    }
    if (dx != 0.0 || dy != 0.0) engine_->panByPixels(dx, dy); // 锁外：与触摸线程同口径的相机推进
    return active;
}

void MapRenderHost::checkPendingTap(int64_t nowNs) {
    std::function<void(float, float)> cb;
    float x = 0.0f;
    float y = 0.0f;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!pendingTap_ || nowNs < pendingTapDeadlineNs_) return;
        pendingTap_ = false;
        cb = singleTapCb_; // 锁内拷贝，锁外触发（回调可能进 NAPI/ArkTS，不可持锁）
        x = pendingTapX_;
        y = pendingTapY_;
    }
    if (cb) cb(x, y);
}

void MapRenderHost::checkPendingLongPress(int64_t nowNs) {
    std::function<void(float, float)> cb;
    float x = 0.0f;
    float y = 0.0f;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!pendingLongPress_ || nowNs < pendingLongPressDeadlineNs_) return;
        pendingLongPress_ = false; // 到点即消费（一次按下只触发一次长按）
        cb = longPressCb_;         // 锁内拷贝，锁外触发
        x = pendingLongPressX_;
        y = pendingLongPressY_;
    }
    if (cb) cb(x, y);
}

void MapRenderHost::renderLoop() {
    if (!egl_.initialize(window_)) {
        LOGE("MapRenderHost: EGL init failed, render thread exits");
        return;
    }
    engine_->surfaceCreated();

    bool sizeApplied = false; // surfaceChanged(w,h) 至少成功应用一次后才允许 drawFrame
    for (;;) {
        int32_t w = 0;
        int32_t h = 0;
        bool quit = false;
        bool draw = false;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            // 自驱动帧源装载超时：惯性滑动需 ~60fps 节拍逐帧推进；待确认单击需到点唤醒。
            // 两者皆无时回到 WHEN_DIRTY 的无限休眠（零空转）。
            int64_t timeoutMs = -1;
            if (flingVx_ != 0.0 || flingVy_ != 0.0) timeoutMs = 16;
            if (pendingTap_) {
                int64_t remainMs = (pendingTapDeadlineNs_ - steadyNowNs() + 999999) / 1000000;
                if (remainMs < 0) remainMs = 0;
                timeoutMs = (timeoutMs < 0) ? remainMs : std::min(timeoutMs, remainMs);
            }
            if (pendingLongPress_) {
                int64_t remainMs = (pendingLongPressDeadlineNs_ - steadyNowNs() + 999999) / 1000000;
                if (remainMs < 0) remainMs = 0;
                timeoutMs = (timeoutMs < 0) ? remainMs : std::min(timeoutMs, remainMs);
            }
            if (timeoutMs < 0) {
                cv_.wait(lock, [this] { return quit_ || sizeDirty_ || frameDirty_; });
            } else {
                cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                             [this] { return quit_ || sizeDirty_ || frameDirty_; });
            }
            quit = quit_;
            if (!quit) {
                if (sizeDirty_) {
                    w = width_;
                    h = height_;
                    sizeDirty_ = false;
                }
                draw = frameDirty_;
                frameDirty_ = false; // 同帧合并多次唤醒（WHEN_DIRTY 语义）
            }
        }
        if (quit) break;
        if (w > 0 && h > 0) {
            engine_->surfaceChanged(w, h);
            sizeApplied = true;
        }
        if (draw && sizeApplied) {
            engine_->drawFrame(); // 引擎内部可能同步回调 requestRender → 下轮唤醒，天然形成续帧
            if (!egl_.swapBuffers()) {
                LOGE("eglSwapBuffers failed: 0x%x", eglGetError());
            }
        }
        // 帧后推进自驱动动画：惯性未停则续请一帧；单击/长按到点则触发回调（均不持锁执行）
        const int64_t nowNs = steadyNowNs();
        if (advanceFling(nowNs)) {
            requestRender();
        }
        checkPendingTap(nowNs);
        checkPendingLongPress(nowNs);
    }

    // GL 资源（着色器/VBO/纹理）必须在上下文销毁前、于本渲染线程释放
    engine_->releaseGl();
    egl_.destroy();
    LOGI("MapRenderHost: render thread exited");
}

} // namespace wwohos
