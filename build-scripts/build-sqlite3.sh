
if [ ! -f sqlite-autoconf-3370200.tar.gz ]; then
  wget -q https://sqlite.org/2022/sqlite-autoconf-3370200.tar.gz
# else
#   rm -f sqlite-autoconf-3370200
fi

rm -rf sqlite-autoconf-3370200

tar xzf sqlite-autoconf-3370200.tar.gz
cd sqlite-autoconf-3370200

# --host 用 HOST_CONFIGURE（android 与 HOST 相同；ohos 映射为 *-unknown-linux-musl，
# 避开老 config.sub 不认 *-linux-ohos 三元组的问题；编译器已由 CC 显式指定）
./configure \
  --prefix="$PREFIX" \
  --host="${HOST_CONFIGURE:-$HOST}" \
  CC="$CC" CXX="$CXX"  \
  CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS"

make -j$(nproc)
make install
cd $MAIN_DIR