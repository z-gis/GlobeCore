package com.zys.globecore.android

import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import com.zys.globecore.Camera
import com.zys.globecore.NativeMapView
import com.zys.globecore.NativeSrs
import java.io.File

/**
 * 演示页通用骨架：PROJ 初始化、[NativeMapView] 创建与 GL 生命周期转发、
 * 顶部「标题 + 横向动作按钮条」布局，让各演示子类只聚焦自己的功能 API。
 *
 * 集成三步曲（tutorials/01）在这里一次完成：
 *  [NativeSrs.initProjData] → 创建 [NativeMapView] → Activity 生命周期转发
 *  （onResume/onPause 驱动 GL 线程，onDestroy 释放 native 实例）。
 */
abstract class BaseMapActivity : AppCompatActivity() {

    /** 地图视图（native 渲染内核的 Kotlin 门面） */
    protected lateinit var map: NativeMapView
        private set

    private lateinit var actionRow: LinearLayout

    /** 页面标题（演示项名称），显示在顶栏 */
    protected abstract val demoTitle: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // 一次性：解压内置 proj 数据并设置 PROJ 搜索路径（幂等，可重复调）
        NativeSrs.initProjData(this)

        map = NativeMapView(this)

        val titleView = TextView(this).apply {
            text = demoTitle
            setTextColor(0xFFFFFFFF.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
        }
        actionRow = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }

        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xB3000000.toInt())
            setPadding(dp(12), dp(28), dp(12), dp(4))
            addView(titleView)
            addView(HorizontalScrollView(this@BaseMapActivity).apply { addView(actionRow) })
        }

        setContentView(FrameLayout(this).apply {
            addView(map, FrameLayout.LayoutParams(-1, -1))
            addView(bar, FrameLayout.LayoutParams(-1, -2, Gravity.TOP))
        })
    }

    /** 在动作条追加一个演示按钮 */
    protected fun addDemoAction(label: String, onClick: () -> Unit) {
        actionRow.addView(Button(this).apply {
            text = label
            isAllCaps = false
            textSize = 13f
            setOnClickListener { onClick() }
        })
    }

    /**
     * 挂在线高德影像底图（0.1.2.0：demo 删除离线基图，默认高德影像）：
     * [NativeMapView.addTileLayer] 影像层（style=6），自带磁盘缓存目录、联网取瓦片
     * （需宿主声明 INTERNET 权限，见 AndroidManifest）。相机定位到 [camera]。
     *
     * 注：图层按加入次序共享下标（layers_）——本方法恒为各演示页最先建层，故影像=0；
     * 矢量/栅格/叠加层在其后加入，绘制由 native 按「底图→栅格→矢量」分层，互不影响拾取。
     */
    protected fun addBasemap(
        camera: Camera = Camera(20.0, 0.0, 30_000_000.0),
    ) {
        map.addTileLayer(
            cacheDir = File(filesDir, "tiles/amap").absolutePath,
            urlTemplate = TileSources.AMAP_IMAGERY,
            maxLevel = TileSources.AMAP_MAX_LEVEL,
        )
        map.setCamera(camera)
    }

    /** 读回当前相机、按 [mutate] 修改后回写（放大/缩小/旋转/仰角按钮共用；句柄已释放则忽略）。 */
    private inline fun adjustCamera(mutate: (Camera) -> Camera) {
        map.getCamera()?.let { map.setCamera(mutate(it)) }
    }

    /** 放大：视野高度减半逐级贴近（钳最小 200m，避免贴地翻转）。2D/3D 均生效。 */
    protected fun zoomIn() =
        adjustCamera { it.copy(altitude = (it.altitude * 0.5).coerceAtLeast(200.0)) }

    /** 缩小：视野高度翻倍逐级拉远（钳最大 4e7m，保证整球可见）。2D/3D 均生效。 */
    protected fun zoomOut() =
        adjustCamera { it.copy(altitude = (it.altitude * 2.0).coerceAtMost(40_000_000.0)) }

    /** 水平旋转：方位角 [deltaDeg] 度（heading 仅 3D 透视通路消费，2D 恒正北不显效）。 */
    protected fun rotateHeading(deltaDeg: Double) =
        adjustCamera { it.copy(heading = it.heading + deltaDeg) }

    /** 俯仰：tilt [deltaDeg] 度（仅 3D 消费；钳 [0,80] 与 native 口径一致）。 */
    protected fun tiltBy(deltaDeg: Double) =
        adjustCamera { it.copy(tilt = (it.tilt + deltaDeg).coerceIn(0.0, 80.0)) }

    protected fun toast(msg: CharSequence) {
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
    }

    protected fun dp(value: Int): Int = TypedValue.applyDimension(
        TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics
    ).toInt()

    // GLSurfaceView 生命周期要求：转发驱动 GL 线程与 native 实例释放
    override fun onResume() {
        super.onResume()
        map.onResume()
    }

    override fun onPause() {
        map.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        map.destroy()  // 释放 native GlobeEngine 实例
        super.onDestroy()
    }
}
