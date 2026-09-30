package com.zys.globecore

import android.content.Context
import java.io.File
import java.io.FileOutputStream

/**
 * PROJ 坐标转换门面：本模块自有的数据/投影能力入口，符号实现见 cpp/bridge/srs.cpp
 * （`Java_com_zys_globecore_NativeSrs_*`）。
 *
 * 依赖方向约定：globecore 先于 app 存在，JNI 符号只挂本模块类名；
 * app 侧（坐标显示、"移动到"对话框、关于页）经本门面委托调用，不直接声明 external fun。
 *
 * PROJ 数据（proj.db 与格网改正数文件）随本模块 assets/proj 分发（AAR 合入宿主 APK），
 * 宿主启动时调一次 [initProjData] 即可完成解压与搜索路径配置。
 */
object NativeSrs {

    init {
        // 加载失败仅记录：后续 external 调用抛 UnsatisfiedLinkError，由调用方各自兜底（与宿主原行为一致）
        try {
            System.loadLibrary("globecore")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e("NativeSrs", "Failed to load native library 'globecore'", e)
        }
    }

    /** 设置 PROJ 数据目录（proj.db 与格网改正数文件所在路径，一般经 [initProjData] 自动传入） */
    external fun initProjDataPath(projDataPath: String?)

    /**
     * 初始化 PROJ 数据：把本模块内置的 assets/proj 解压到 `context.filesDir/proj`
     * 并设置 PROJ 搜索路径（[initProjDataPath]）。宿主须在首次坐标转换前调用一次；
     * 已存在的文件跳过不重复解压。失败仅记日志不抛出，后续转换按既有口径返回 null 由调用方回退。
     */
    fun initProjData(context: Context) {
        try {
            val app = context.applicationContext
            val projDir = File(app.filesDir, "proj")
            if (!projDir.exists()) {
                projDir.mkdirs()
            }
            copyAssetFolder(app, "proj", projDir)
            initProjDataPath(projDir.absolutePath)
        } catch (e: Exception) {
            android.util.Log.e("NativeSrs", "Failed to extract proj data", e)
        }
    }

    /**
     * 递归复制 assets 目录到 [destDir]：子目录建同名目录后递归（保持层级），
     * 文件已存在时跳过，避免每次启动重复解压。
     */
    private fun copyAssetFolder(context: Context, assetPath: String, destDir: File) {
        try {
            val children = context.assets.list(assetPath)
            // 空清单即叶子节点（assets 对文件返回空数组），按文件复制
            if (children.isNullOrEmpty()) {
                val leaf = File(destDir, assetPath.substringAfterLast('/'))
                if (!leaf.exists()) copyAssetFile(context, assetPath, leaf)
                return
            }
            if (!destDir.exists()) destDir.mkdirs()
            for (child in children) {
                val subPath = "$assetPath/$child"
                val subChildren = context.assets.list(subPath)
                if (subChildren.isNullOrEmpty()) {
                    val destFile = File(destDir, child)
                    if (!destFile.exists()) copyAssetFile(context, subPath, destFile)
                } else {
                    copyAssetFolder(context, subPath, File(destDir, child))
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("NativeSrs", "Failed to copy asset: $assetPath", e)
        }
    }

    private fun copyAssetFile(context: Context, assetPath: String, destFile: File) {
        try {
            context.assets.open(assetPath).use { input ->
                FileOutputStream(destFile).use { out -> input.copyTo(out) }
            }
        } catch (e: Exception) {
            android.util.Log.e("NativeSrs", "Failed to copy: $assetPath", e)
        }
    }

    /**
     * 任意坐标系互转（PROJ 管道按 "源|目标" 缓存，可承受高频调用）：
     * 返回 [x, y]（目标坐标系）；转换不可用（proj.db 未就绪 / 编码非法）返回 null，调用方需自行回退。
     */
    external fun convert(x: Double, y: Double, srcCrs: String, tgtCrs: String): DoubleArray?

    /** PROJ 主版本号（如 9），供关于页显示 */
    external fun getProjVersion(): Int
}
