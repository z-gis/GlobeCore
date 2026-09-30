package com.zys.globecore.android

import android.os.Bundle
import android.os.Handler
import android.os.Looper
import com.zys.globecore.Camera
import com.zys.globecore.NativeMapView
import com.zys.globecore.Position
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * 演示 05 · 定位标记（蓝点）：
 *  - [NativeMapView.setLocationMarker]：地理坐标 + 可见性 + 移动方位角（headingDeg ≥0 画方向箭头），
 *    标记以屏幕固定尺寸绘制在所有瓦片层之上（对齐真实 1Hz 定位回调驱动口径）。
 *
 * 本演示用 Handler 每 1s 沿「圆形航线」推进一次标记位置并计算方位角，模拟定位回调。
 */
class LocationMarkerActivity : BaseMapActivity() {

    override val demoTitle get() = getString(R.string.demo_marker_title)

    private val handler = Handler(Looper.getMainLooper())
    private var moving = false
    private var step = 0
    private var lastPos: Position? = null

    private val ticker = object : Runnable {
        override fun run() {
            advance()
            if (moving) handler.postDelayed(this, 1_000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 在线高德影像底图（蓝点近景）
        addBasemap(Camera(latitude = 39.92, longitude = 116.40, altitude = 30_000.0))

        // 初始静态蓝点（无方向箭头）
        val start = Position(latitude = 39.92, longitude = 116.38)
        map.setLocationMarker(start, visible = true)
        lastPos = start

        addDemoAction(getString(R.string.action_start_move)) { startMove() }
        addDemoAction(getString(R.string.action_stop_move)) { stopMove() }
        addDemoAction("隐藏标记") { map.setLocationMarker(lastPos!!, visible = false) }
    }

    private fun startMove() {
        if (moving) return
        moving = true
        handler.post(ticker)
        toast("模拟 1Hz 定位回调中…")
    }

    private fun stopMove() {
        moving = false
        handler.removeCallbacks(ticker)
    }

    /** 沿圆形航线推进一步：算新位置 + 方位角，推送标记（同款 API 即真实定位SDK回调里要做的） */
    private fun advance() {
        step += 6  // 每秒 6°，一圈 60s
        val center = Position(latitude = 39.92, longitude = 116.40)
        val rLat = 0.01
        val a = Math.toRadians(step.toDouble())
        val pos = Position(
            latitude = center.latitude + rLat * sin(a),
            longitude = center.longitude + rLat * cos(a) / cos(Math.toRadians(center.latitude)),
        )
        val heading = lastPos?.let { bearingDeg(it, pos) } ?: 0.0
        map.setLocationMarker(pos, visible = true, headingDeg = heading)
        lastPos = pos
        toast("定位更新 #${step / 6}  heading=${heading.roundToInt()}°")
    }

    /** 球面正方位角（顺时针自北 0..360） */
    private fun bearingDeg(from: Position, to: Position): Double {
        val lat1 = Math.toRadians(from.latitude)
        val lat2 = Math.toRadians(to.latitude)
        val dLon = Math.toRadians(to.longitude - from.longitude)
        val y = sin(dLon) * cos(lat2)
        val x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon)
        return (Math.toDegrees(atan2(y, x)) + 360.0) % 360.0
    }

    override fun onPause() {
        stopMove()
        super.onPause()
    }
}
