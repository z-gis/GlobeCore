package com.zys.globecore.android

import android.os.Bundle
import com.zys.globecore.Camera
import com.zys.globecore.NativeMapView

/**
 * 演示 02 · 2D / 3D 视图切换（0.1.1.1）：在线高德影像底图。
 *  - [com.zys.globecore.NativeMapView.setViewMode]：平面墨卡托正交 ↔ WGS84 球体透视，
 *    两模式共用相机状态，切换保持视角连续、瓦片纹理缓存互通；
 *  - 放大 / 缩小按钮（相机高度读-改-写，两模式均生效）；
 *  - 3D 下双指旋转（heading）/ 双指俯仰（tilt，native 钳 [0,80]）手势生效。
 */
class Globe3DActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_globe_title)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 高相机位便于看球体全貌；在线高德影像底图
        addBasemap(Camera(latitude = 30.0, longitude = 110.0, altitude = 30_000_000.0))

        addDemoAction("2D 平面") { map.setViewMode(NativeMapView.ViewMode.TWO_D) }
        addDemoAction("3D 球体") { map.setViewMode(NativeMapView.ViewMode.THREE_D) }
        addDemoAction("放大") { zoomIn() }
        addDemoAction("缩小") { zoomOut() }
        addDemoAction("倾斜视角") {
            map.setViewMode(NativeMapView.ViewMode.THREE_D)
            // heading/tilt 仅 3D 透视通路消费：北偏西 + 俯仰 60° 斜视地球
            map.setCamera(Camera(latitude = 35.0, longitude = 105.0, altitude = 8_000_000.0, heading = 30.0, tilt = 60.0))
        }
    }
}
