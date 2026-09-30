# 交叉编译工具链解析：按 $TARGET（android | ohos）分派。
#   android 分支：期望 NDK r23b（Linux 版），本地探测不到时从 Google 官方仓库自动下载；
#   ohos   分支：解析 HarmonyOS NEXT 的 OHOS NDK（DevEco Studio / Command Line Tools SDK 内
#                native/ 目录）。华为侧下载需登录账号，不做自动下载——探测不到即给出明确指引。
# 两分支共同导出：NDK_ROOT（android）或 OHOS_NATIVE + TOOLCHAIN（ohos，指向 .../native/llvm，
# 其下 bin/ 与 sysroot/ 布局与 Android 分支同名，子脚本可统一以 $TOOLCHAIN/bin/xxx 引用工具）。
TARGET=${TARGET:-android}

if [ "$TARGET" = "ohos" ]; then
  # ==================== OHOS NDK（HarmonyOS NEXT） ====================
  # sysroot 定位：不同 OHOS NDK 版本布局不同——
  #   统一布局（5.0.3+/26.x 实测）：<native>/sysroot/usr/lib/<triple>，clang 传 --sysroot=<native>/sysroot；
  #   旧布局（早期 NEXT native）：<native>/llvm/sysroot/<triple>，clang 传 --sysroot=<该目录>。
  # 由 ww_ohos_sysroot_dir <native> 回显 clang --sysroot 应用的根目录（两种布局自适应）。
  ww_ohos_sysroot_dir() {
    if [ -d "$1/sysroot/usr/lib/aarch64-linux-ohos" ]; then
      echo "$1/sysroot"
    elif [ -d "$1/llvm/sysroot/aarch64-linux-ohos" ]; then
      echo "$1/llvm/sysroot/aarch64-linux-ohos"
    else
      return 1
    fi
  }

  # 有效性校验（对齐 Android 侧经验）：不止看目录存在，必须 clang 真能冒烟编译——
  # Windows 挂载盘（drvfs/9p）上符号链接/执行位会丢失，仅查目录会漏报损坏。
  ohos_ndk_valid() {
    local sr
    [ -n "$1" ] && [ -x "$1/llvm/bin/clang" ] \
      && [ -f "$1/build/cmake/ohos.toolchain.cmake" ] \
      && sr="$(ww_ohos_sysroot_dir "$1")" \
      && printf 'int main(){return 0;}' | "$1/llvm/bin/clang" \
           --target=aarch64-linux-ohos --sysroot="$sr" \
           -c -x c - -o /dev/null >/dev/null 2>&1
  }

  RESOLVED_OHOS=""
  # 探测顺序：显式 OHOS_NDK_ROOT / DevEco 注入的 OHOS_SDK_NATIVE → 用户目录常见安装位 →
  # 工作区 ndk/ 下手工解包 → WSL 下 Windows 盘 DevEco SDK 安装位（可能因 drvfs 失效，命中会提示）
  OHOS_CANDIDATES=(
    "${OHOS_NDK_ROOT:-}"
    "${OHOS_SDK_NATIVE:-}"
    "$HOME/command-line-tools/sdk/default/openharmony/native"
    "$MAIN_DIR/ndk/native"
  )
  # 工作区 ndk/ 下的解包目录（如 ndk/ohos-sdk/linux/native）
  for d in "$MAIN_DIR"/ndk/*/native "$MAIN_DIR"/ndk/*/*/native; do
    [ -d "$d" ] && OHOS_CANDIDATES+=("$d")
  done
  # WSL：Windows 盘上的 DevEco / Command Line Tools SDK（跨用户通配）
  for d in /mnt/c/Users/*/AppData/Local/Huawei/Sdk/*/openharmony/native \
           /mnt/c/Users/*/command-line-tools/sdk/default/openharmony/native \
           "/mnt/c/Program Files/Huawei/DevEco Studio"/*/sdk/*/openharmony/native; do
    [ -d "$d" ] && OHOS_CANDIDATES+=("$d")
  done

  for cand in "${OHOS_CANDIDATES[@]}"; do
    if ohos_ndk_valid "$cand"; then
      RESOLVED_OHOS="$cand"
      echo "使用 OHOS NDK: $RESOLVED_OHOS"
      case "$RESOLVED_OHOS" in
        /mnt/*)
          echo "警告：该 OHOS NDK 位于 Windows 挂载盘（9p/drvfs）。工具链若无符号链接损坏" \
               "可继续使用；一旦报 clang/链接器异常请先整目录拷到 WSL 原生盘（ext4）："
          echo "        cp -a \"$RESOLVED_OHOS\" \"\$HOME/ohos-native\" && export OHOS_NDK_ROOT=\$HOME/ohos-native"
          ;;
      esac
      break
    fi
  done

  if [ -z "$RESOLVED_OHOS" ]; then
    echo "错误：未找到可用的 OHOS NDK（HarmonyOS NEXT 交叉编译必需）。"
    echo "获取方式（华为侧下载需登录，无法自动下载）："
    echo "  1) 安装 DevEco Studio（含 SDK），或下载 Command Line Tools for HarmonyOS（Linux x64），"
    echo "     解压后得到 .../sdk/default/openharmony/native 目录；"
    echo "  2) 将其拷入 WSL 原生盘（ext4）后任选其一指定："
    echo "       export OHOS_NDK_ROOT=<路径>/openharmony/native"
    echo "     或放入 $MAIN_DIR/ndk/ 下（形如 ndk/<任意目录名>/native）后重跑。"
    exit 1
  fi

  export OHOS_NATIVE="$RESOLVED_OHOS"
  # DevEco 官方 toolchain 文件读取这两个变量定位 SDK
  export OHOS_NDK_HOME="$OHOS_NATIVE"
  export OHOS_SDK_NATIVE="$OHOS_NATIVE"
  # llvm 根：布局与 Android TOOLCHAIN 对齐（bin/ 下的 clang/llvm-ar 等）
  export TOOLCHAIN="$OHOS_NATIVE/llvm"
  # clang --sysroot 应传入的 sysroot 根（自适应统一/旧布局），供 build-all.sh 复用
  export WW_OHOS_SYSROOT="$(ww_ohos_sysroot_dir "$OHOS_NATIVE")"
  echo "OHOS_NATIVE=$OHOS_NATIVE"
  [ -f "$OHOS_NATIVE/oh-uni-package.json" ] && head -n 6 "$OHOS_NATIVE/oh-uni-package.json"

else
  # ==================== Android NDK（原逻辑，保持零变化） ====================
  # NDK 期望版本与 Linux 版下载地址（Google 官方仓库；r23b 与本项目迭代期一致，
  # 仅为编译期工具链，与模块自身构建用的 NDK 28.x 不冲突，静态库 ABI 兼容）
  NDK_VERSION=r23b
  NDK_DIR_NAME=android-ndk-$NDK_VERSION
  NDK_LINUX_URL=https://dl.google.com/android/repository/android-ndk-$NDK_VERSION-linux.zip
  # NDK 下载/解压的专用目录（依赖 MAIN_DIR）；zip 与解压出的 NDK 都落在此，避免污染当前/脚本目录
  NDK_INSTALL_DIR=${NDK_INSTALL_DIR:-$MAIN_DIR/ndk}

  # 校验候选目录是否为可用的 Linux 版 NDK（不仅看 bin 目录存在，更要 clang 真能执行——
  # 在 Windows 挂载盘 9p 上解压会丢符号链接，bin/clang 无法解析，必须拦下）
  ndk_valid() {
    [ -n "$1" ] && [ -d "$1/toolchains/llvm/prebuilt/linux-x86_64/bin" ] \
      && "$1/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --version >/dev/null 2>&1
  }

  # 解析顺序：1) 外部显式 NDK_ROOT；2) 本地常见位置探测（含 ndk/ 目录）；3) 都没有 → 下载 Linux 版
  RESOLVED_NDK=""
  if ndk_valid "$NDK_ROOT"; then
    RESOLVED_NDK="$NDK_ROOT"
    echo "使用外部指定的 NDK_ROOT: $RESOLVED_NDK"
  else
    for cand in \
      "$NDK_INSTALL_DIR/$NDK_DIR_NAME" \
      "$MAIN_DIR/$NDK_DIR_NAME" \
      "$ANDROID_NDK_HOME" \
      "$ANDROID_NDK_ROOT" \
      "${ANDROID_HOME:+$ANDROID_HOME/ndk-bundle}" \
      "${ANDROID_SDK_ROOT:+$ANDROID_SDK_ROOT/ndk-bundle}"; do
      if ndk_valid "$cand"; then
        RESOLVED_NDK="$cand"
        echo "本地已存在 NDK: $RESOLVED_NDK"
        break
      fi
    done
  fi

  # 本地未找到：下载并解压 Linux 版 NDK 到 ndk/ 目录
  if [ -z "$RESOLVED_NDK" ]; then
    mkdir -p "$NDK_INSTALL_DIR"
    ZIP="$NDK_INSTALL_DIR/$NDK_DIR_NAME-linux.zip"
    echo "未找到本地 NDK，下载 Linux 版：$NDK_LINUX_URL"
    if [ -f "$ZIP" ]; then
      echo "已存在安装包，跳过下载：$ZIP"
    elif command -v wget >/dev/null 2>&1; then
      wget -q --show-progress -O "$ZIP" "$NDK_LINUX_URL"
    elif command -v curl >/dev/null 2>&1; then
      curl -fL -o "$ZIP" "$NDK_LINUX_URL"
    else
      echo "错误：需要 wget 或 curl 以下载 NDK"
      exit 1
    fi
    if ! command -v unzip >/dev/null 2>&1; then
      echo "错误：需要 unzip 以解压 NDK（sudo apt install unzip）"
      exit 1
    fi
    echo "解压 NDK 到 $NDK_INSTALL_DIR ..."
    rm -rf "$NDK_INSTALL_DIR/$NDK_DIR_NAME"
    unzip -q "$ZIP" -d "$NDK_INSTALL_DIR"
    RESOLVED_NDK="$NDK_INSTALL_DIR/$NDK_DIR_NAME"
    if ! ndk_valid "$RESOLVED_NDK"; then
      echo "错误：解压后未找到有效 NDK：$RESOLVED_NDK"
      exit 1
    fi
  fi

  export NDK_ROOT="$RESOLVED_NDK"
  export TOOLCHAIN=$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64
  echo "NDK_ROOT=$NDK_ROOT"

  # 冒烟测试：clang 必须真能执行，否则多为把 NDK 解压到了 Windows 挂载盘（9p/drvfs，符号链接丢失）
  if ! "$TOOLCHAIN/bin/clang" --version >/dev/null 2>&1; then
    echo "错误：NDK clang 无法执行：$TOOLCHAIN/bin/clang"
    echo "      多因在 WSL 把 NDK 解压到 Windows 挂载盘（/mnt/...，9p），符号链接/可执行位丢失。"
    echo "      解决：改用 WSL 原生目录（ext4）作构建工作区后重跑，例如："
    echo "        MAIN_DIR=\$HOME/android-jni-build bash \"$BASE_PATH/build-all.sh\""
    echo "      （build-all.sh 在检测到 /mnt 挂载盘时已会自动做此重定向）"
    exit 1
  fi
fi
