# Windows Support for ollama-fuse

This document provides instructions for building and using ollama-fuse on Windows.

## Prerequisites

### For Windows Users

1. Install [WinFsp](https://github.com/winfsp/winfsp/releases) - Download and install the latest version
2. Install [Ollama](https://ollama.com/download/windows) for Windows

### For Cross-Compilation from Linux

1. Install MinGW-w64: `sudo apt-get install mingw-w64`
2. Install CMake: `sudo apt-get install cmake`
3. Download WinFsp MSI installer and extract headers/libraries (see below)

## Building for Windows

### Cross-Compiling from Linux

1. Download the WinFsp MSI installer:
   ```
   mkdir -p winfsp-deps
   cd winfsp-deps
   wget https://github.com/winfsp/winfsp/releases/download/v2.0/winfsp-2.0.23075.msi
   ```

2. Extract the necessary files from the MSI:
   ```
   sudo apt-get install msitools
   mkdir -p extracted
   msiextract -C extracted winfsp-2.0.23075.msi
   ```

3. Organize the extracted files:
   ```
   mkdir -p include/fuse3 include/winfsp lib
   cp -r "extracted/Program Files/DYNAMIC/inc/fuse3/"* include/fuse3/
   cp -r "extracted/Program Files/DYNAMIC/inc/winfsp/"* include/winfsp/
   cp "extracted/Program Files/DYNAMIC/lib/winfsp-x64.lib" lib/
   cd ..
   ```

4. Run the build script:
   ```
   ./build-windows.sh
   ```

5. The Windows executable will be created at `build-win/ollama-fuse.exe`

### Building Directly on Windows

1. Install [MSYS2](https://www.msys2.org/)
2. Install the required packages:
   ```
   pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-make
   ```

3. Configure and build:
   ```
   mkdir build
   cd build
   cmake .. -G "MinGW Makefiles"
   mingw32-make
   ```

## Usage on Windows

1. Install WinFsp from https://github.com/winfsp/winfsp/releases
2. Install Ollama for Windows from https://ollama.com/download/windows
3. Create a mount point (drive letter or empty directory)
4. Run ollama-fuse:
   ```
   ollama-fuse.exe X:
   ```
   Replace `X:` with your desired mount point

5. Access your Ollama models through the mounted drive

## Troubleshooting

### Common Issues

1. **WinFsp not found**: Ensure WinFsp is installed and its bin directory is in your PATH
2. **Access denied errors**: Run the command prompt or PowerShell as Administrator
3. **Models not showing up**: Verify that Ollama is running and has downloaded models

### Debugging

For additional debugging information, run with the `-d` flag:
```
ollama-fuse.exe -d X:
```

## Limitations

1. Some features may work differently on Windows compared to Linux
2. Performance may vary depending on your system configuration
3. File permissions are handled differently on Windows

## Contributing

Contributions to improve Windows support are welcome! Please submit pull requests or open issues on GitHub. 