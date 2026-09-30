#ifndef GLOBECORE_VECTOR_GDALBOOTSTRAP_H
#define GLOBECORE_VECTOR_GDALBOOTSTRAP_H

namespace globecore {

/**
 * GDAL/OGR 全进程一次性引导。
 *
 * globecore 自链 GDAL/OGR（native 侧直接读矢量），首次使用矢量能力前须调用一次
 * [ensureRegistered]：内部以 `std::once_flag` 保证 `GDALAllRegister()`（注册全部矢量/栅格驱动）
 * 全进程仅执行一次，并打印 GDAL 版本自检到 logcat（TAG=globecore）。
 *
 * 该调用同时作为「链接期强制拉入 GDAL 符号」的锚点：GlobeEngine 构造时触发，
 * 若 GDAL 依赖链未正确链接会在加载 .so 时立即暴露未定义符号，而非等到首次读矢量。
 *
 * 线程安全：`std::call_once` 保证并发调用下只初始化一次；GDAL 驱动注册表本身进程级共享。
 */
void ensureGdalRegistered();

} // namespace globecore

#endif // GLOBECORE_VECTOR_GDALBOOTSTRAP_H
