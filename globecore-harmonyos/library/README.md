# @zys/globecore

GlobeCore 独立实现的 C++ 地图渲染引擎在鸿蒙（HarmonyOS NEXT）上的 HAR 封装：ArkTS 门面 + NAPI + 共享 C++ 引擎（静态链接 GDAL / PROJ / libcurl 等）。

## 安装

```shell
ohpm install @zys/globecore
```

## 使用

```ts
import { NativeMapView, Camera, Position, NativeSrs, NativeNet } from '@zys/globecore';

// 首次需在 Ability 初始化时解压内置 PROJ 数据与 CA 证书
NativeSrs.initProjData(context);
NativeNet.initCaBundle(context);
```

公开 API 以 `Index.ets` 导出清单为准（`NativeMapView` / `ViewMode` / `Camera` / `Position` / `VectorStyle` / `NativeSrs` / `NativeNet` 等）。

## 支持 ABI

`arm64-v8a`、`x86_64`。

## 版本

`0.1.1`

## 许可

Apache-2.0，详见 `LICENSE`。
