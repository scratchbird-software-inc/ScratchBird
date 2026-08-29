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
WORK_ROOT="${TMPDIR:-/tmp}/scratchbird-admin-lifecycle-smoke.$$"

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
FIXTURE="${BUILD_ROOT}/output/${TARGET_PLATFORM}/bin/database_lifecycle_admin_cli_conformance"
if [[ ! -x "${FIXTURE}" ]]; then
  echo "admin_lifecycle_smoke=missing_fixture:${FIXTURE}" >&2
  exit 2
fi

# This example uses the admin lifecycle fixture as the runnable route. It does
# not ask the engine to execute SQL text; parser/client text remains outside the
# engine authority boundary.
"${FIXTURE}"

echo "admin_lifecycle_smoke=passed"
