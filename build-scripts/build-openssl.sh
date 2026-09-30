#!/bin/bash
# build-openssl.sh —— 供 build-all.sh 在 build_one_abi 内 source 的 per-ABI 子脚本。
# 为“当前 $ABI”交叉编译 OpenSSL 静态库（libssl.a / libcrypto.a），并 stage 到 $PREFIX：
#   $PREFIX/lib/libssl.a、$PREFIX/lib/libcrypto.a
#   $PREFIX/include/openssl/*.h
# 不单独进 jniLibs：libcurl.a 会把这些静态目标合并进去（见 build-curl.sh）。
#
# 依赖 build_one_abi 注入的环境变量：ABI PREFIX NDK_ROOT(仅 android) TOOLCHAIN API HOST CC TARGET。
# 源码包 openssl-$OPENSSL_VER.tar.gz 缺失时自动下载（可多镜像回退），下载/解压都落在 $MAIN_DIR。

OPENSSL_VER=${OPENSSL_VER:-1.1.1w}
OPENSSL_TARBALL="openssl-${OPENSSL_VER}.tar.gz"
OPENSSL_URLS=(
  "https://github.com/openssl/openssl/archive/refs/tags/OpenSSL_${OPENSSL_VER//./_}.tar.gz"
  "https://www.openssl.org/source/old/${OPENSSL_VER%.*}/$OPENSSL_TARBALL"
)

# —— 取源码：优先复用 $MAIN_DIR 已有 tarball，其次 $PKG_CACHE，最后 wget ——
cd "$MAIN_DIR"
if [ ! -f "$OPENSSL_TARBALL" ]; then
  fetched=0
  for cache in ${PKG_CACHE:-}; do
    if [ -f "$cache/$OPENSSL_TARBALL" ]; then
      cp -f "$cache/$OPENSSL_TARBALL" "$OPENSSL_TARBALL" && { echo "从缓存复用 $OPENSSL_TARBALL"; fetched=1; break; }
    fi
  done
  if [ "$fetched" -eq 0 ]; then
    for u in "${OPENSSL_URLS[@]}"; do
      echo "下载 OpenSSL 源码：$u"
      if timeout 300 wget -q -O "$OPENSSL_TARBALL" "$u" && [ "$(stat -c %s "$OPENSSL_TARBALL" 2>/dev/null || echo 0)" -gt 1048576 ]; then
        break
      fi
      rm -f "$OPENSSL_TARBALL"
    done
  fi
fi
[ -f "$OPENSSL_TARBALL" ] || { echo "[openssl] 源码包缺失且下载失败：$OPENSSL_TARBALL"; exit 1; }

# —— 解压并规整目录名（github archive 顶层是 openssl-OpenSSL_1_1_1w/）——
SRC_DIR="openssl-${OPENSSL_VER}"
if [ ! -d "$SRC_DIR" ]; then
  top=$(tar tzf "$OPENSSL_TARBALL" | head -1 | cut -d/ -f1)
  tar xzf "$OPENSSL_TARBALL"
  [ "$top" != "$SRC_DIR" ] && mv "$top" "$SRC_DIR"
fi
cd "$SRC_DIR"

# —— 目标平台名（OpenSSL Configure 用）——
# ohos 无官方 Configure 目标，映射到同架构 linux-* 目标（clang --target/--sysroot 由
# build_one_abi 注入的 CC wrapper 提供，musl 下 gnu 工具链路径兼容）
if [ "$TARGET" = "ohos" ]; then
  case "$ABI" in
    x86_64)    SSL_TARGET="linux-x86_64" ;;
    arm64-v8a) SSL_TARGET="linux-aarch64" ;;
    *) echo "[openssl] 不支持的 OHOS ABI：$ABI"; exit 1 ;;
  esac
else
  case "$ABI" in
    x86_64)    SSL_TARGET="android-x86_64" ;;
    arm64-v8a) SSL_TARGET="android-arm64"  ;;
    *) echo "[openssl] 不支持的 ABI：$ABI"; exit 1 ;;
  esac
fi

# —— 交叉环境 ——
export PATH="$TOOLCHAIN/bin:$PATH"
export AR="${AR:-$TOOLCHAIN/bin/llvm-ar}"
export RANLIB="${RANLIB:-$TOOLCHAIN/bin/llvm-ranlib}"
export NM="${NM:-$TOOLCHAIN/bin/llvm-nm}"
export STRIP="${STRIP:-$TOOLCHAIN/bin/llvm-strip}"
if [ "$TARGET" != "ohos" ]; then
  # android：NDK 环境变量 + 显式 clang wrapper（历史行为不变）
  export ANDROID_NDK_ROOT="$NDK_ROOT"
  export ANDROID_NDK_HOME="$NDK_ROOT"   # openssl 1.1.1 的 android 配置读 ANDROID_NDK_HOME
  export ANDROID_DEV="$TOOLCHAIN/sysroot/usr"
  export CC="$TOOLCHAIN/bin/${HOST}${API}-clang"
  export CXX="$TOOLCHAIN/bin/${HOST}${API}-clang++"
fi
# ohos：CC/CXX 已由 build_one_abi 注入（带 --target/--sysroot 的 wrapper），不再覆盖

# 两遍 ABI 复用同一份源码目录：先 distclean 清掉上一 ABI 的构建缓存，避免串味
make distclean >/dev/null 2>&1 || true

if [ "$TARGET" = "ohos" ]; then
  # linux-* 目标下 openssl 尊重环境变量 CC（即 build_one_abi 注入的 clang wrapper），
  # 无需 --cross-compile-prefix；musl 下不需 API 宏
  ./Configure "$SSL_TARGET" \
    --prefix="$PWD/_install" --openssldir="$PWD/_install/ssl" \
    no-shared no-tests no-engine no-comp no-ui-console
else
  ./Configure "$SSL_TARGET" \
    --prefix="$PWD/_install" --openssldir="$PWD/_install/ssl" \
    -D__ANDROID_API__="$API" \
    no-shared no-tests no-engine no-comp no-ui-console
fi

# 只编静态库，跳过 apps/openssl 命令行与文档（更快，也避免装 app）
make -j"$(nproc)" build_libs

# —— stage 到 $PREFIX ——
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp -f libcrypto.a libssl.a "$PREFIX/lib/"
rm -rf "$PREFIX/include/openssl"
cp -rf include/openssl "$PREFIX/include/"
echo "[openssl] staged -> $PREFIX/lib/{libssl.a,libcrypto.a} + include/openssl"
