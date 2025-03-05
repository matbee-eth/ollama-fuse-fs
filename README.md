# Ollama FUSE Filesystem

This application creates a simplified view of the Ollama model filesystem using FUSE (Filesystem in Userspace).

## Overview

Ollama stores models in a complex directory structure under `$HOME/.ollama` or any other specified directory. This FUSE application crawls these directories and presents a simplified view where:

- Each model has its own directory at the root level
- Inside each model directory, there are `.gguf` and `.modelfile` files for each version

For example, the Ollama path:
```
$HOME/.ollama/models/manifests/registry.ollama.ai/library/llama3.2/latest
```

Will be presented as:
```
/mount_point/llama3.2/latest.gguf
/mount_point/llama3.2/latest.modelfile
```

## Requirements

- libfuse3-dev
- nlohmann-json (C++ JSON library)
- g++ with C++17 support
- CMake (for building)

## Installation

### Install dependencies

On Debian/Ubuntu:
```bash
sudo apt-get install libfuse3-dev nlohmann-json3-dev cmake
```

On Fedora:
```bash
sudo dnf install fuse3-devel nlohmann-json-devel cmake
```

### Build with CMake

```bash
mkdir -p build
cd build
cmake ..
make
```

## Usage

```bash
./ollama-fuse <mount_point> [ollama_dir1] [ollama_dir2] ... [-f] [-d] [--llama-swap] [--base-url URL]
```

Where:
- `mount_point` is the directory where you want to mount the filesystem
- `ollama_dir1`, `ollama_dir2`, etc. (optional) are paths to your Ollama directories (defaults to `$OLLAMA_HOME` if set, otherwise `$HOME/.ollama`)
- `-f` (optional) run in foreground (by default, the process runs in the background)
- `-d` (optional) enable debug output
- `--llama-swap` or `-ls` (optional) generate a llama-swap configuration file
- `--base-url URL` (optional) set the base URL for llama-swap proxy (default: http://0.0.0.0:11434)

You can specify multiple Ollama directories, and the application will combine models from all of them into a single unified view.

### Environment Variables

- `OLLAMA_HOME`: If set, this environment variable will be used as the default Ollama directory path when no path is specified on the command line

## llama-swap Integration

ollama-fuse can generate a configuration file for [llama-swap](https://github.com/mostlygeek/llama-swap), a transparent proxy server for llama.cpp that provides automatic model swapping. This allows you to use your Ollama models with llama.cpp server.

To generate a llama-swap configuration file, use the `--llama-swap` or `-ls` option:

```bash
./ollama-fuse <mount_point> [ollama_dir1] [ollama_dir2] ... --llama-swap
```

This will create a `llama-swap-config.yaml` file in the current directory with all your Ollama models configured for use with llama-swap.

You can customize the base URL for the proxy with the `--base-url` option:

```bash
./ollama-fuse <mount_point> [ollama_dir1] [ollama_dir2] ... --llama-swap --base-url http://127.0.0.1:8080
```

The generated configuration includes:
- All discovered Ollama models
- Appropriate paths to GGUF files
- Default settings for healthCheckTimeout and logRequests

After generating the configuration, you can use it with llama-swap:

```bash
llama-swap -config llama-swap-config.yaml
```

### Example

```bash
mkdir -p ~/ollama-mount
./ollama-fuse ~/ollama-mount
```

To run in foreground (useful for debugging):
```bash
./ollama-fuse ~/ollama-mount -f
```

To unmount:
```bash
fusermount -u ~/ollama-mount
```

## Features

- Presents a simplified view of Ollama models
- Automatically generates proper Modelfiles from the model metadata
- Correctly handles symlinks in the Ollama directory structure
- Supports all Ollama model formats including GGUF files

## File Structure

Each model in the mounted filesystem will have:

- A directory named after the model (e.g., `llama3.2`)
- Inside each model directory:
  - `<version>.gguf` - The model weights file
  - `<version>.modelfile` - A properly formatted Modelfile with:
    - FROM instruction based on the model family
    - TEMPLATE instruction from the template file
    - PARAMETER instruction for stop sequences
    - LICENSE instruction with the model license

## Notes

- The filesystem is read-only
- The `.modelfile` files are generated from the metadata in the manifest files
- The application requires that Ollama is installed and has at least one model downloaded
- Large model files (GGUF) are directly mapped from their original location
