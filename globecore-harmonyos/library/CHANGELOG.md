# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与[语义化版本](https://semver.org/lang/zh-CN/)。

## [0.1.2]

### Changed
- 版本号由 0.1.1 提升至 0.1.2，与 Android 端 0.1.2.0 保持一致。

## [0.1.1]

### Added
- 鸿蒙（HarmonyOS NEXT）地图渲染 HAR：ArkTS 门面 + NAPI + 共享 C++ 渲染引擎。
- 支持 2D/3D 视图、瓦片底图（在线/离线）、矢量图层与要素拾取高亮、测量加点连线、定位标记。
- 内置 PROJ 投影数据（`NativeSrs.initProjData`）与 HTTPS CA 证书校验（`NativeNet.initCaBundle`）。
- 坐标反算、SRS 解析、矢量范围查询等公开 API（详见 `Index.ets` 导出清单）。
- 打包 `arm64-v8a`、`x86_64` 两个 ABI 的 `libglobecore.so`（静态链接 GDAL / PROJ / libcurl 等）。

## [0.1.0]

### Added
- GlobeCore C++ 渲染引擎初版，独立实现 Mercator 投影与球体渲染。
