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
WORK_ROOT="${TMPDIR:-/tmp}/scratchbird-embedded-public-abi-smoke.$$"

cleanup() {
  rm -rf "${WORK_ROOT}"
}
trap cleanup EXIT

mkdir -p "${WORK_ROOT}"

TARGET_PLATFORM="${SB_PUBLIC_TARGET_PLATFORM:-}"
if [[ -z "${TARGET_PLATFORM}" && -f "${BUILD_ROOT}/CMakeCache.txt" ]]; then
  TARGET_PLATFORM="$(sed -n 's/^SB_PUBLIC_TARGET_PLATFORM:[^=]*=//p' "${BUILD_ROOT}/CMakeCache.txt" | head -n 1)"
fi
TARGET_PLATFORM="${TARGET_PLATFORM:-linux}"
ABI_FIXTURE="${BUILD_ROOT}/output/${TARGET_PLATFORM}/bin/sb_engine_public_abi_cpp_fixture"
SBLR_FIXTURE="${BUILD_ROOT}/output/${TARGET_PLATFORM}/bin/sb_engine_public_sblr_admission_fixture"

for fixture in "${ABI_FIXTURE}" "${SBLR_FIXTURE}"; do
  if [[ ! -x "${fixture}" ]]; then
    echo "embedded_public_abi_smoke=missing_fixture:${fixture}" >&2
    exit 2
  fi
done

# The embedded example exercises public ABI and SBLR admission fixtures. The
# engine executes admitted SBLR/internal API work, not SQL text.
"${ABI_FIXTURE}"
"${SBLR_FIXTURE}"

echo "embedded_public_abi_smoke=passed"
