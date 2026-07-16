#!/usr/bin/env sh
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: smoke_install_linux.sh <scratchbird-linux.tar.gz> <work-root>" >&2
  exit 2
fi

archive=$1
work_root=$2
rm -rf "$work_root"
mkdir -p "$work_root"
tar -xzf "$archive" -C "$work_root"

test -d "$work_root/opt/ScratchBird/bin"
test -d "$work_root/opt/ScratchBird/lib"
test -d "$work_root/etc/scratchbird"
test -f "$work_root/opt/ScratchBird/share/scratchbird/release/INSTALL_MANIFEST.json"
test -f "$work_root/opt/ScratchBird/share/scratchbird/release/SHA256SUMS"
test -f "$work_root/opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json"

if [ ! -d "$work_root/opt/ScratchBird/share/scratchbird/resources" ]; then
  echo "missing resources directory" >&2
  exit 1
fi

python3 "$(dirname "$0")/../release/verify_native_installed_payload.py" "$work_root"

llvm_runtime_library=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["llvm_runtime"]["runtime_library"])' \
  "$work_root/opt/ScratchBird/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json")
python3 -c 'import ctypes,sys; ctypes.CDLL(sys.argv[1])' "$llvm_runtime_library"
printf 'llvm_runtime_library=%s\nllvm_runtime_load=passed\n' "$llvm_runtime_library" \
  > "$work_root/llvm-runtime-load.txt"

for binary in SBsql SBadm SBbak SBsec SBdoc SBcop SBsrv SBgate SBmgr SBParser; do
  executable="$work_root/opt/ScratchBird/bin/$binary"
  ldd "$executable" > "$work_root/$binary.ldd.txt"
  if grep -q 'not found' "$work_root/$binary.ldd.txt"; then
    echo "unresolved runtime dependency: $binary" >&2
    exit 1
  fi
  set +e
  "$executable" --help > "$work_root/$binary.help.txt" 2>&1
  status=$?
  set -e
  if [ "$status" -gt 2 ] || [ ! -s "$work_root/$binary.help.txt" ]; then
    echo "native binary launch failed: $binary status=$status" >&2
    exit 1
  fi
done

echo "smoke_install_linux=passed:$work_root"
