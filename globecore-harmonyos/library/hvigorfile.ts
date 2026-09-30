import { harTasks } from '@ohos/hvigor-ohos-plugin';
import * as fs from 'fs';
import * as path from 'path';

/**
 * 版本化 HAR 产出插件：在 assembleHar 完成打包（产出 globecore.har）之后，
 * 额外复制一份带版本号的文件名 globecore-<version>.har，
 * 版本号取自本模块 oh-package.json5。对齐 Android 侧 copyVersionedReleaseAar 约定。
 *
 * 触发方式（依赖 assembleHar，会先自动完成打包再执行本任务）：
 *   hvigorw --mode module -p module=globecore@default -p product=default createVersionedHar --no-daemon
 */
function createVersionedHarPlugin(): any {
  return {
    pluginId: 'createVersionedHar',
    apply(node: any): void {
      node.registerTask({
        name: 'createVersionedHar',
        dependencies: ['assembleHar'],
        run: (taskContext: any): void => {
          const moduleDir: string =
            taskContext && taskContext.modulePath
              ? String(taskContext.modulePath)
              : String(node.getNodeDir());
          const moduleName: string =
            taskContext && taskContext.moduleName
              ? String(taskContext.moduleName)
              : String(node.getNodeName());
          const outDir = path.join(moduleDir, 'build', 'default', 'outputs', 'default');
          const pkgRaw = fs.readFileSync(path.join(moduleDir, 'oh-package.json5'), 'utf-8');
          const pkgName = (pkgRaw.match(/"name"\s*:\s*"([^"]+)"/) || [])[1] || moduleName;
          const version = (pkgRaw.match(/"version"\s*:\s*"([^"]+)"/) || [])[1] || 'unknown';
          // hvigor 产出的默认 .har 以模块名命名（globecore.har），非 ohpm 包名
          const srcHar = path.join(outDir, `${moduleName}.har`);
          if (!fs.existsSync(srcHar)) {
            console.warn(`[createVersionedHar] 未找到打包产物：${srcHar}`);
            return;
          }
          // 作用域名 @zys/globecore 含 '/' 不能作文件名，替换为 '-'
          const safeName = pkgName.replace(/\//g, '-');
          const dstHar = path.join(outDir, `${safeName}-${version}.har`);
          fs.copyFileSync(srcHar, dstHar);
          console.log(`[createVersionedHar] 已生成版本化 HAR：${dstHar}`);
        },
      });
    },
  };
}

export default {
  system: harTasks,
  plugins: [createVersionedHarPlugin()],
};
