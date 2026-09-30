export MINIZIP_VER=1.2.13

# Allow standalone execution without sourcing build-all.sh
: "${ABI:=x86_64}"
: "${API:=24}"
: "${TARGET:=android}"
: "${WW_PFX:=}"
: "${MAIN_DIR:=/mnt/k/dev/MobileMap/GdalDemo/gdal-build}"
: "${NDK_ROOT:=$MAIN_DIR/android-ndk-r23b}"
: "${TOOLCHAIN:=$NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64}"
: "${PREFIX:=$MAIN_DIR/export/third_party$WW_PFX-$ABI}"
# 单独直跑时（不经 build-all.sh）默认按 Android 工具链参数；经 build-all 时已被注入覆盖
: "${WW_CMAKE_TARGET_ARGS:=-DCMAKE_TOOLCHAIN_FILE=$NDK_ROOT/build/cmake/android.toolchain.cmake -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$API}"

case "$ABI" in
  x86_64)
    if [ "$TARGET" = "ohos" ]; then TARGET_TRIPLE="x86_64-linux-ohos"; else TARGET_TRIPLE="x86_64-linux-android"; fi
    ;;
  arm64-v8a)
    if [ "$TARGET" = "ohos" ]; then TARGET_TRIPLE="aarch64-linux-ohos"; else TARGET_TRIPLE="aarch64-linux-android"; fi
    ;;
  *)
    echo "Unsupported ABI: $ABI"
    exit 1
    ;;
esac

if [ "$TARGET" = "ohos" ]; then
  # ohos：CC/AR 由 build_one_abi 注入（clang 带 --target/--sysroot 的 wrapper）；直跑兼容兼未注入时回退裸 clang+显式 sysroot
  OHOS_SYSROOT="$TOOLCHAIN/sysroot/$TARGET_TRIPLE"
  : "${CC:=$TOOLCHAIN/bin/clang --target=$TARGET_TRIPLE --sysroot=$OHOS_SYSROOT}"
  : "${AR:=$TOOLCHAIN/bin/llvm-ar}"
else
  : "${CC:=$TOOLCHAIN/bin/${TARGET_TRIPLE}${API}-clang}"
  : "${AR:=$TOOLCHAIN/bin/llvm-ar}"
fi

if ! ${CC} --version >/dev/null 2>&1; then
  echo "CC not executable: $CC"
  exit 1
fi

if [ ! -x "$AR" ]; then
  echo "AR not found or not executable: $AR"
  exit 1
fi

mkdir -p "$PREFIX/lib" "$PREFIX/include"

rm -rf "zlib-$MINIZIP_VER"
tar xzf "zlib-$MINIZIP_VER.tar.gz"
cd "zlib-$MINIZIP_VER"
rm -rf "build-$TARGET-$ABI"
mkdir "build-$TARGET-$ABI" && cd "build-$TARGET-$ABI"
# 目标工具链参数由 build-all.sh 按 TARGET 注入（android: NDK toolchain；ohos: ohos.toolchain）
cmake .. \
  $WW_CMAKE_TARGET_ARGS \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
make -j$(nproc)
make install
cd "$MAIN_DIR"
echo "zlib done"

# minizip 源码（用 zlib 自带的 minizip）
export MINIZIP_SRC="$MAIN_DIR/zlib-$MINIZIP_VER/contrib/minizip"

cd "$MINIZIP_SRC"
rm -rf "$PREFIX/include/minizip"
mkdir -p "$PREFIX/include/minizip"
rm -f *.o libminizip.a

"$CC" -c -fPIC -I. -I"$PREFIX/include" ioapi.c zip.c unzip.c
"$AR" rcs libminizip.a ioapi.o zip.o unzip.o
cp libminizip.a "$PREFIX/lib/"
cp *.h "$PREFIX/include/minizip"
cd "$MAIN_DIR"
echo "minizip done"

