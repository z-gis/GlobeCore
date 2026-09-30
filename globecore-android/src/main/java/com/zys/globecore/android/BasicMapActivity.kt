package com.zys.globecore.android

import android.os.Bundle
import com.zys.globecore.Camera
import com.zys.globecore.NativeMapView

/**
 * 演示 01 · 瓦片底图（0.1.1.1）：**只呈现 3D 球体模式**，在线高德影像底图。
 *  - [com.zys.globecore.NativeMapView.addTileLayer]：影像层（联网取瓦片）；
 *  - [com.zys.globecore.NativeMapView.setViewMode]：进入即锁定 3D（本演示不提供 2D 切换，2D 见演示 02）；
 *  - 顶栏动作按钮：放大 / 缩小 / 旋转 ± / 仰角 ±（经相机读-改-写驱动，旋转与俯仰仅 3D 显效）；
 *  - [com.zys.globecore.NativeMapView.setOnTapListener] + screenToGeo：单击屏幕点 → 经纬度。
 *
 * 交互：单指拖动平移、捏合缩放、双指旋转/俯仰、惯性滑行（手势已内联在 NativeMapView）。
 */
class BasicMapActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_basic_title)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 在线高德影像底图；相机定位北京（300 万米高度约可见华北）
        addBasemap(Camera(latitude = 39.9, longitude = 116.4, altitude = 3_000_000.0))
        // 本演示只呈现 3D：进入即设球体透视，不提供 2D 平面切换
        map.setViewMode(NativeMapView.ViewMode.THREE_D)

        // 单击 → 经纬度（采集/测量加点的同款口径）
        map.setOnTapListener { x, y ->
            val pos = map.screenToGeo(x, y)
            toast(if (pos == null) "未命中地面点" else "lon=%.6f, lat=%.6f".format(pos.longitude, pos.latitude))
        }

        // 相机姿态动作按钮（放大/缩小 2D/3D 均生效；旋转/仰角仅 3D 显效）
        addDemoAction("放大") { zoomIn() }
        addDemoAction("缩小") { zoomOut() }
        addDemoAction("旋转 +") { rotateHeading(30.0) }
        addDemoAction("旋转 -") { rotateHeading(-30.0) }
        addDemoAction("仰角 +") { tiltBy(15.0) }
        addDemoAction("仰角 -") { tiltBy(-15.0) }
    }
}
