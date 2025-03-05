#!/bin/bash

# Exit on error
set -e

# Clean build directory
rm -rf build-win
mkdir -p build-win

# Configure with CMake using the MinGW toolchain
cd build-win
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../mingw-toolchain.cmake \
    -DCROSS_COMPILE=ON \
    -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . -- -j$(nproc)

# Print success message
echo "Build completed successfully!"
echo "Windows executable: $(pwd)/ollama-fuse.exe"

# Return to the original directory
cd .. 