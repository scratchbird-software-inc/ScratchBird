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

if [ ! -d "$work_root/opt/ScratchBird/share/scratchbird/resources" ]; then
  echo "missing resources directory" >&2
  exit 1
fi

echo "smoke_install_linux=passed:$work_root"
