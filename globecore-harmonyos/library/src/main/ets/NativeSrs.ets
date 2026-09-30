// NativeSrs.ets —— PROJ 坐标转换门面（对应 Android NativeSrs.kt）。
//
// 符号实现见引擎 bridge/srs.cpp 的 wwbridge 核心 + ohos/napi_srs.cpp NAPI 导出。
// PROJ 数据（proj.db 与格网改正数）随本 HAR 的 resources/rawfile/proj 分发
// （build-scripts --target ohos 的 install_proj_data 落位，与 Android assets/proj 同源），
// 宿主启动时调一次 initProjData(context) 即完成沙箱解压与搜索路径配置。
import { NativeLib } from './NativeLib';
import common from '@ohos.app.ability.common';
import resourceManager from '@ohos.resourceManager';
import fs from '@ohos.file.fs';

const SRS_TAG: string = 'NativeSrs';

export class NativeSrs {

  /** 设置 PROJ 数据目录（proj.db 所在沙箱路径，一般经 initProjData 自动传入） */
  static initProjDataPath(projDataPath: string | undefined): void {
    if (projDataPath === undefined || projDataPath === null) {
      return;
    }
    NativeLib.nativeInitProjDataPath(projDataPath);
  }

  /**
   * 初始化 PROJ 数据：把本 HAR rawfile 的 proj 目录解压到 `context.filesDir/proj`
   * 并设置 PROJ 搜索路径。宿主须在首次坐标转换前调用一次；已存在的文件跳过不重复解压。
   * 失败仅记日志不抛出，后续转换按既有口径返回 null 由调用方回退（对齐 Android initProjData）。
   */
  static initProjData(context: common.UIAbilityContext): void {
    try {
      const filesDir: string = context.filesDir;
      const projDir: string = `${filesDir}/proj`;
      if (!fs.accessSync(projDir)) {
        fs.mkdirSync(projDir);
      }
      const rm = context.resourceManager;
      NativeSrs.copyRawfileFolder(rm, 'proj', projDir);
      NativeSrs.initProjDataPath(projDir);
    } catch (e) {
      console.error(`${SRS_TAG}: Failed to extract proj data: ${e}`);
    }
  }

  /**
   * 递归复制 rawfile 目录到 destDir：getRawFileListSync 列目录（非空即子目录，递归）；
   * 文件存在时跳过，避免每次启动重复解压（对齐 Android copyAssetFolder 口径）。
   */
  private static copyRawfileFolder(rm: resourceManager.ResourceManager, rawPath: string, destDir: string): void {
    let entries: string[] = [];
    try {
      entries = rm.getRawFileListSync(rawPath);
    } catch (e) {
      console.error(`${SRS_TAG}: Failed to list rawfile: ${rawPath}: ${e}`);
      return;
    }
    if (!fs.accessSync(destDir)) {
      fs.mkdirSync(destDir, true);
    }
    for (let i = 0; i < entries.length; i++) {
      const name = entries[i];
      const subRaw = `${rawPath}/${name}`;
      // 子目录判定：清单非空即目录（PROJ 数据层级：proj/<网格子目录>/…）；
      // list 异常时按文件处理（部分设备对叶子路径抛错而非返回空清单）
      let isDir = false;
      try {
        isDir = rm.getRawFileListSync(subRaw).length > 0;
      } catch (e) {
        isDir = false;
      }
      if (isDir) {
        NativeSrs.copyRawfileFolder(rm, subRaw, `${destDir}/${name}`);
        continue;
      }
      try {
        const content: Uint8Array = rm.getRawFileContentSync(subRaw);
        const destFile = `${destDir}/${name}`;
        if (!fs.accessSync(destFile)) {
          const file = fs.openSync(destFile, fs.OpenMode.CREATE | fs.OpenMode.WRITE_ONLY);
          fs.writeSync(file.fd, content.buffer);
          fs.closeSync(file);
        }
      } catch (e) {
        console.error(`${SRS_TAG}: Failed to copy: ${subRaw}: ${e}`);
      }
    }
  }

  /**
   * 任意坐标系互转（PROJ 管道按 "源|目标" 缓存，可承受高频调用）：
   * 返回 [x, y]（目标坐标系）；转换不可用（proj.db 未就绪 / 编码非法）返回 null，调用方需自行回退。
   */
  static convert(x: number, y: number, srcCrs: string, tgtCrs: string): number[] | null {
    return NativeLib.nativeConvert(x, y, srcCrs, tgtCrs);
  }

  /** PROJ 版本号（major*10000 + minor*100 + patch，如 90200 = 9.2.0），供关于页显示 */
  static getProjVersion(): number {
    return NativeLib.nativeGetProjVersion();
  }
}
