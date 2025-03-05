#!/bin/bash
# Demo script for ollama-fuse

# Create mount directory if it doesn't exist
mkdir -p ~/ollama-mount

# Kill any existing ollama-fuse processes
pkill -f ollama-fuse

# Determine Ollama directory path
if [ -n "$OLLAMA_HOME" ]; then
    OLLAMA_DIR="$OLLAMA_HOME"
else
    OLLAMA_DIR="$HOME/.ollama"
fi

# Mount the Ollama filesystem
./build/ollama-fuse ~/ollama-mount "$OLLAMA_DIR" -f &
FUSE_PID=$!

# Wait for the filesystem to be mounted
sleep 2

# List available models
echo "Available models:"
ls -la ~/ollama-mount

# Choose a model (first one found)
MODEL=$(ls ~/ollama-mount | head -1)
if [ -z "$MODEL" ]; then
    echo "No models found!"
    fusermount -u ~/ollama-mount
    exit 1
fi

echo "Selected model: $MODEL"

# List model files
echo "Model files:"
ls -la ~/ollama-mount/$MODEL

# Display modelfile content
echo "Modelfile content:"
cat ~/ollama-mount/$MODEL/*.modelfile

# Cleanup
echo "Press Enter to unmount and exit..."
read
kill $FUSE_PID
fusermount -u ~/ollama-mount
echo "Unmounted successfully"
