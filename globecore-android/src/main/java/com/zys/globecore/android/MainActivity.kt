package com.zys.globecore.android

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

/**
 * 演示应用主页：功能清单入口（以逐项演示库能力的示例集合形式组织）。
 * 每项对应一个演示 [com.zys.globecore.NativeMapView] 单一能力的独立 Activity。
 */
class MainActivity : AppCompatActivity() {

    private data class Demo(val title: String, val desc: String, val target: Class<out Activity>)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val demos = listOf(
            Demo(getString(R.string.demo_basic_title), getString(R.string.demo_basic_desc), BasicMapActivity::class.java),
            Demo(getString(R.string.demo_globe_title), getString(R.string.demo_globe_desc), Globe3DActivity::class.java),
            Demo(getString(R.string.demo_vector_title), getString(R.string.demo_vector_desc), VectorLayerActivity::class.java),
            Demo(getString(R.string.demo_overlay_title), getString(R.string.demo_overlay_desc), OverlayActivity::class.java),
            Demo(getString(R.string.demo_marker_title), getString(R.string.demo_marker_desc), LocationMarkerActivity::class.java),
        )

        val header = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(48), dp(20), dp(12))
            addView(TextView(this@MainActivity).apply {
                text = getString(R.string.app_name)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f)
                setTypeface(typeface, android.graphics.Typeface.BOLD)
            })
            addView(TextView(this@MainActivity).apply {
                text = getString(R.string.menu_subtitle)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                alpha = 0.6f
            })
        }

        val list = ListView(this).apply {
            adapter = object : ArrayAdapter<Demo>(this@MainActivity, 0, demos) {
                override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
                    val demo = getItem(position)!!
                    return LinearLayout(this@MainActivity).apply {
                        orientation = LinearLayout.VERTICAL
                        setPadding(dp(20), dp(14), dp(20), dp(14))
                        gravity = Gravity.CENTER_VERTICAL
                        addView(TextView(this@MainActivity).apply {
                            text = demo.title
                            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
                            setTypeface(typeface, android.graphics.Typeface.BOLD)
                        })
                        addView(TextView(this@MainActivity).apply {
                            text = demo.desc
                            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                            alpha = 0.6f
                        })
                    }
                }
            }
            setOnItemClickListener { _, _, position, _ ->
                startActivity(Intent(this@MainActivity, demos[position].target))
            }
        }

        setContentView(LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            addView(header)
            addView(list, LinearLayout.LayoutParams(-1, -1))
        })
    }

    private fun dp(value: Int): Int = TypedValue.applyDimension(
        TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics
    ).toInt()
}
