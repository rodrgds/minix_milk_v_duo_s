#!/bin/bash
set -e

echo "=== Building RISC-V libumdp with libnl support ==="

# Create a temporary toolchain directory
TOOLCHAIN_DIR="$HOME/toolchains/riscv-gcc10"
mkdir -p "$TOOLCHAIN_DIR"

# Link gcc-10 and other tools
ln -sf /usr/bin/riscv64-linux-gnu-gcc-10 "$TOOLCHAIN_DIR/riscv64-linux-gnu-gcc"
ln -sf /usr/bin/riscv64-linux-gnu-g++-10 "$TOOLCHAIN_DIR/riscv64-linux-gnu-g++"
ln -sf /usr/bin/riscv64-linux-gnu-ar     "$TOOLCHAIN_DIR/riscv64-linux-gnu-ar"
ln -sf /usr/bin/riscv64-linux-gnu-strip  "$TOOLCHAIN_DIR/riscv64-linux-gnu-strip"
ln -sf /usr/bin/riscv64-linux-gnu-ranlib "$TOOLCHAIN_DIR/riscv64-linux-gnu-ranlib"

# Set up environment
export PATH="$TOOLCHAIN_DIR:$PATH"

# RISC-V sysroot and libnl paths
SYSROOT="/home/luhen_39/pi/duo-buildroot-sdk/ramdisk/rootfs/common_musl_riscv64"
INSTALL_PREFIX="$HOME/riscv-libnl-static"  # Use static version
INSTALL_PREFIX_DYNAMIC="$HOME/riscv-libnl"

# Check which libnl installation to use
if [ -f "$INSTALL_PREFIX/lib/libnl-3.a" ]; then
    echo "Using static libnl from $INSTALL_PREFIX"
    LIBNL_PATH="$INSTALL_PREFIX"
elif [ -f "$INSTALL_PREFIX_DYNAMIC/lib/libnl-3.so" ]; then
    echo "Using dynamic libnl from $INSTALL_PREFIX_DYNAMIC"
    LIBNL_PATH="$INSTALL_PREFIX_DYNAMIC"
else
    echo "ERROR: No libnl installation found!"
    echo "Please run the libnl build first"
    exit 1
fi

cd /home/luhen_39/pi/minix/linux-usermode-driver-platform

# Create cross-compilation file with libnl paths
cat > riscv64-cross-with-libnl-fixed.txt << EOF2
[binaries]
c = 'riscv64-linux-gnu-gcc'
cpp = 'riscv64-linux-gnu-g++'
ar = 'riscv64-linux-gnu-ar'
strip = 'riscv64-linux-gnu-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'riscv64'
cpu = 'riscv64'
endian = 'little'

[built-in options]
c_args = ['--sysroot=$SYSROOT', '-I$LIBNL_PATH/include/libnl3']
c_link_args = ['--sysroot=$SYSROOT', '-L$LIBNL_PATH/lib']
default_library = 'static'

[properties]
sys_root = '$SYSROOT'
pkg_config_path = '$LIBNL_PATH/lib/pkgconfig'
EOF2

# Set pkg-config to find the RISC-V libnl
export PKG_CONFIG_PATH="$LIBNL_PATH/lib/pkgconfig"

# Verify pkg-config can find libnl
echo "Testing pkg-config for libnl:"
pkg-config --exists libnl-3.0 && echo "libnl-3.0: OK" || echo "libnl-3.0: FAIL"
pkg-config --exists libnl-genl-3.0 && echo "libnl-genl-3.0: OK" || echo "libnl-genl-3.0: FAIL"

if pkg-config --exists libnl-3.0 libnl-genl-3.0; then
    echo "Include paths:"
    pkg-config --cflags libnl-3.0 libnl-genl-3.0
    echo "Library paths:"
    pkg-config --libs libnl-3.0 libnl-genl-3.0
else
    echo "ERROR: pkg-config cannot find libnl libraries"
    echo "Available .pc files:"
    ls -la "$LIBNL_PATH/lib/pkgconfig/"
    exit 1
fi

# Build the library
cd libumdp

# Clean previous build
echo "Cleaning previous build..."
rm -rf build_riscv_libnl

# Configure with meson (with libnl dependencies)
echo "Configuring with meson (RISC-V build with libnl)..."
meson setup build_riscv_libnl --cross-file ../riscv64-cross-with-libnl-fixed.txt

# Compile
echo "Compiling..."
meson compile -C build_riscv_libnl

# Check if build was successful
if [ $? -eq 0 ]; then
    echo "Build successful with libnl support!"
    echo "Built files:"
    ls -la build_riscv_libnl/
    
    # Show file architecture
    echo "Checking built library architecture:"
    file build_riscv_libnl/libumdp.* 2>/dev/null || echo "No library files found"
    
else
    echo "Build failed!"
    echo "Check the error messages above"
    exit 1
fi
