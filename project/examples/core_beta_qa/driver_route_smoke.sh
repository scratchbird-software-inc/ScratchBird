#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
REPO_ROOT="${1:-${DEFAULT_REPO_ROOT}}"
BUILD_ROOT="${2:-${REPO_ROOT}/build}"
WORK_ROOT="${TMPDIR:-/tmp}/scratchbird-driver-route-smoke.$$"

cleanup() {
  rm -rf "${WORK_ROOT}"
}
trap cleanup EXIT

mkdir -p "${WORK_ROOT}"

if [[ ! -d "${BUILD_ROOT}" ]]; then
  echo "driver_route_smoke=missing_build_root:${BUILD_ROOT}" >&2
  exit 2
fi

# Representative driver smoke uses current standalone CTest gates so optional
# toolchain waivers remain deterministic and source-controlled by the driver
# package tests, not by this examples pack.
ctest --test-dir "${BUILD_ROOT}" \
  -R "driver_package_manifest_gate|driver_python_gate|driver_cpp_gate" \
  --output-on-failure

echo "driver_route_smoke=passed"
