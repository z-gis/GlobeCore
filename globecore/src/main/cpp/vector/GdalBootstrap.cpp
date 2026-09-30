#include "vector/GdalBootstrap.h"

#include <mutex>

#include "gdal/gdal_priv.h"
#include "gdal/ogrsf_frmts.h"

#include "util/Log.h"

namespace globecore {

void ensureGdalRegistered() {
    // 全进程一次：GDALAllRegister 注册全部栅格 + 矢量（OGR）驱动，重复调用虽幂等但有开销，
    // 用 call_once 收敛到首次。版本自检打印便于确认 .so 内 GDAL 依赖链正确链接、可运行期加载。
    static std::once_flag flag;
    std::call_once(flag, []() {
        GDALAllRegister();
        const char *ver = GDALVersionInfo("RELEASE_NAME");
        LOGI("GDAL registered: %s", ver ? ver : "(unknown)");
    });
}

} // namespace globecore
