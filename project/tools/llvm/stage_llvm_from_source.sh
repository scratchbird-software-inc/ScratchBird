#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

#
# SEARCH_KEY: OEIC_LLVM_DYNAMIC_LOADER
# Build and stage the private LLVM shared library used by ScratchBird's optional
# native-compile accelerator. This script is intentionally not a correctness
# dependency for the optimizer; it only prepares dynamic/static acceleration
# material when the configured LLVM source checkout is available.

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: stage_llvm_from_source.sh <llvm-source-cmake-dir> <build-dir> <tools-root> <min-major>" >&2
  exit 2
fi

source_dir="$1"
build_dir="$2"
tools_root="$3"
min_major="$4"

if [[ ! -f "${source_dir}/CMakeLists.txt" ]]; then
  echo "LLVM source CMakeLists.txt not found: ${source_dir}" >&2
  exit 3
fi

cmake -S "${source_dir}" \
      -B "${build_dir}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_ENABLE_PROJECTS= \
      -DLLVM_TARGETS_TO_BUILD=host \
      -DLLVM_BUILD_LLVM_DYLIB=ON \
      -DLLVM_LINK_LLVM_DYLIB=ON

cmake --build "${build_dir}" --target LLVM -j2

mkdir -p "${tools_root}/lib" "${tools_root}/include" "${tools_root}/provenance"

library=""
while IFS= read -r candidate; do
  name="$(basename "${candidate}")"
  if [[ "${name}" =~ LLVM[-.]?([0-9]+) ]]; then
    major="${BASH_REMATCH[1]}"
    if [[ "${major}" -ge "${min_major}" ]]; then
      library="${candidate}"
      break
    fi
  fi
done < <(find "${build_dir}" -type f \( -name 'libLLVM-*.so*' -o -name 'libLLVM.so.*' -o -name 'LLVM-*.dll' -o -name 'libLLVM-*.dylib' \) | sort)

if [[ -z "${library}" ]]; then
  echo "No versioned libLLVM artifact with major >= ${min_major} found under ${build_dir}" >&2
  exit 4
fi

staged_library="${tools_root}/lib/$(basename "${library}")"
cp -f "${library}" "${staged_library}"

if [[ -d "${source_dir}/include" ]]; then
  cp -a "${source_dir}/include/." "${tools_root}/include/"
fi

sha256sum "${staged_library}" > "${tools_root}/provenance/$(basename "${staged_library}").sha256"
{
  echo "search_key=OEIC_LLVM_DYNAMIC_LOADER"
  echo "source_dir=${source_dir}"
  echo "build_dir=${build_dir}"
  echo "tools_root=${tools_root}"
  echo "staged_library=${staged_library}"
  echo "min_major=${min_major}"
  echo "targets=host"
  echo "build_type=Release"
  if command -v git >/dev/null 2>&1 && git -C "${source_dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "source_commit=$(git -C "${source_dir}" rev-parse HEAD)"
  else
    echo "source_commit=unavailable"
  fi
} > "${tools_root}/provenance/stage_llvm_from_source.env"

echo "staged ${staged_library}"
