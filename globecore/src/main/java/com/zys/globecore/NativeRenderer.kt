package com.zys.globecore

import android.opengl.GLSurfaceView
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * GLSurfaceView 渲染回调：仅把 GL 生命周期事件透传到 native GlobeEngine（句柄 [handle]），
 * 不含任何渲染逻辑（全部在 C++）。对应迁移文档「代码都在 jni 中，Kotlin 只是接口」。
 *
 * [onFirstFrame]：GL 线程画出第一帧后回调一次（供宿主淡出启动加载遮罩，对齐原主界面
 * `MapGlobeEngine.onFirstFrameSettled`）。onDrawFrame 在 GL 线程串行调用，故首帧标记无需同步。
 */
internal class NativeRenderer(
    private val handle: Long
) : GLSurfaceView.Renderer {
    override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
        NativeLib.nativeSurfaceCreated(handle)
    }

    override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
        NativeLib.nativeSurfaceChanged(handle, width, height)
    }

    override fun onDrawFrame(gl: GL10?) {
        NativeLib.nativeDrawFrame(handle)
    }
}
