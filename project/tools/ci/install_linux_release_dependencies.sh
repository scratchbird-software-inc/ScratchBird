#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build python3 python3-pip \
  clang-tidy-18 cppcheck g++-mingw-w64-x86-64 wget gnupg lsb-release \
  libssl-dev libboost-dev libicu-dev libxml2-dev zlib1g-dev \
  liblz4-dev libzstd-dev zstd libgeos-dev libproj-dev libgtest-dev unixodbc-dev

wget -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh
chmod +x /tmp/llvm.sh
sudo /tmp/llvm.sh 23
sudo apt-get install -y libllvm23 llvm-23-dev
