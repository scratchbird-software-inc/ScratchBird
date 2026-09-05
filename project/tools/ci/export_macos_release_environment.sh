#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail
: "${GITHUB_ENV:?GITHUB_ENV is required}"

{
  echo "OPENSSL_ROOT_DIR=$(brew --prefix openssl@3)"
  echo "ICU_ROOT=$(brew --prefix icu4c)"
  echo "BOOST_ROOT=$(brew --prefix boost)"
  echo "LIBXML2_ROOT=$(brew --prefix libxml2)"
  echo "LLVM_DIR=$(brew --prefix llvm)/lib/cmake/llvm"
  echo "SB_LLVM_LIBRARY=$(brew --prefix llvm)/lib/libLLVM.dylib"
  echo "SB_LLVM_RUNTIME_LIBRARY=$(brew --prefix llvm)/lib/libLLVM.dylib"
  echo "ODBC_ROOT=$(brew --prefix unixodbc)"
  echo "SB_LLVM_MIN_MAJOR=22"
  echo "CMAKE_PREFIX_PATH=$(brew --prefix openssl@3);$(brew --prefix icu4c);$(brew --prefix libxml2);$(brew --prefix boost);$(brew --prefix llvm);$(brew --prefix unixodbc)"
  echo "PATH=$(brew --prefix llvm)/bin:$PATH"
  echo "PKG_CONFIG_PATH=$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix icu4c)/lib/pkgconfig:$(brew --prefix libxml2)/lib/pkgconfig:$(brew --prefix unixodbc)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
} >> "$GITHUB_ENV"
