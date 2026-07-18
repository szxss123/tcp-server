#!/usr/bin/env bash
set -euo pipefail

sudo apt update
sudo apt install -y build-essential cmake gdb git netcat-openbsd

echo "Toolchain installed. Versions:"
g++ --version | head -n 1
cmake --version | head -n 1
gdb --version | head -n 1
git --version

