#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1

time brew install \
  cmake \
  ninja \
  pkg-config \
  openssl@3 \
  boost \
  icu4c \
  libxml2 \
  lz4 \
  zstd \
  llvm \
  geos \
  proj \
  googletest \
  unixodbc
