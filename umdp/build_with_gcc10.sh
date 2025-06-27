#!/bin/bash

# Create a temporary toolchain directory
TOOLCHAIN_DIR="$HOME/toolchains/riscv-gcc10"
mkdir -p "$TOOLCHAIN_DIR"

# Link gcc-10 and other tools
ln -sf /usr/bin/riscv64-linux-gnu-gcc-10 "$TOOLCHAIN_DIR/riscv64-linux-gnu-gcc"
ln -sf /usr/bin/riscv64-linux-gnu-ld     "$TOOLCHAIN_DIR/riscv64-linux-gnu-ld"
ln -sf /usr/bin/riscv64-linux-gnu-as     "$TOOLCHAIN_DIR/riscv64-linux-gnu-as"
ln -sf /usr/bin/riscv64-linux-gnu-strip  "$TOOLCHAIN_DIR/riscv64-linux-gnu-strip" 2>/dev/null

# Export environment for kernel build
export PATH="$TOOLCHAIN_DIR:$PATH"
export CROSS_COMPILE=riscv64-linux-gnu-
export ARCH=riscv

# Show GCC version being used
echo "Using compiler:"
riscv64-linux-gnu-gcc --version | head -n1

# Build the kernel module
make -C ~/pi/duo-buildroot-sdk/linux_5.10 M=$(pwd) modules

