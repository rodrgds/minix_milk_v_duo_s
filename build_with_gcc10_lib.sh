#!/bin/bash

# 1. Create a temporary toolchain directory
TOOLCHAIN_DIR="$HOME/toolchains/riscv-gcc10"
mkdir -p "$TOOLCHAIN_DIR"

# 2. Link gcc-10 and other tools
ln -sf /usr/bin/riscv64-linux-gnu-gcc-10 "$TOOLCHAIN_DIR/riscv64-linux-gnu-gcc"
ln -sf /usr/bin/riscv64-linux-gnu-g++-10 "$TOOLCHAIN_DIR/riscv64-linux-gnu-g++"
ln -sf /usr/bin/riscv64-linux-gnu-ar     "$TOOLCHAIN_DIR/riscv64-linux-gnu-ar"
ln -sf /usr/bin/riscv64-linux-gnu-strip  "$TOOLCHAIN_DIR/riscv64-linux-gnu-strip"

# 3. Set up environment
export PATH="$TOOLCHAIN_DIR:$PATH"

# 4. Use the proper RISC-V sysroot
SYSROOT="/home/luhen_39/pi/duo-buildroot-sdk/ramdisk/rootfs/common_musl_riscv64"

# 5. Disable problematic pkg-config to avoid x86_64 libraries
export PKG_CONFIG_LIBDIR=""
export PKG_CONFIG_PATH=""

# 6. Show compiler version
echo "Using compiler:"
riscv64-linux-gnu-gcc --version | head -n1

# 7. Create cross-compilation file for RISC-V with musl
cat > riscv64-cross-final.txt << EOF
[binaries]
c = 'riscv64-linux-gnu-gcc'
cpp = 'riscv64-linux-gnu-g++'
ar = 'riscv64-linux-gnu-ar'
strip = 'riscv64-linux-gnu-strip'
pkg-config = '/bin/false'

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'riscv64'
endian = 'little'

[built-in options]
c_args = ['--sysroot=$SYSROOT']
c_link_args = ['--sysroot=$SYSROOT']

[properties]
sys_root = '$SYSROOT'
pkg_config_libdir = ''
EOF

# 8. Build the library
cd libumdp

# Clean previous build
echo "Cleaning previous build..."
rm -rf build_riscv

# Configure with meson (without libnl dependencies)
echo "Configuring with meson (RISC-V build without libnl)..."
meson setup build_riscv --cross-file ../riscv64-cross-final.txt

# Compile
echo "Compiling..."
meson compile -C build_riscv

# Check if build was successful
if [ $? -eq 0 ]; then
    echo "Build successful!"
    echo "Built files:"
    ls -la build_riscv/libumdp.*
    
    # Show file architecture
    echo "Checking built library architecture:"
    file build_riscv/libumdp.so
    echo "Built library details:"
    file build_riscv/libumdp.a
else
    echo "Build failed!"
    exit 1
fi