#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: run_sbdriver_conformance_sample.sh [options]

Runs sbdriver_conformance with a checked-in sample manifest that includes
the resource lifecycle loop kind (res_loop_exec).

Required environment:
  SB_CONFORMANCE_DSN   Base DSN for conformance execution.

Options:
  --binary-params      Forward --binary-params to sbdriver_conformance.
  --text-params        Forward --text-params to sbdriver_conformance.
  --manifest <path>    Manifest path override.
  --output <path>      Output JSON path override.
  --no-build           Skip cmake configure/build and use existing binary.
  -h, --help           Show this help.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${CLI_DIR}/../../../../" && pwd)"
BUILD_DIR="${CLI_DIR}/build_txn_exec"
BINARY_PATH="${BUILD_DIR}/sbdriver_conformance"

MANIFEST_PATH="${SCRIPT_DIR}/sbwp_conformance_manifest.sample.json"
OUTPUT_PATH="${REPO_ROOT}/artifacts/driver-conformance/cli/sbdriver_conformance_sample.latest.json"
BUILD_ENABLED=1
DRIVER_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary-params|--text-params)
      DRIVER_ARGS+=("$1")
      shift
      ;;
    --manifest)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --manifest" >&2
        exit 2
      fi
      MANIFEST_PATH="$2"
      shift 2
      ;;
    --output)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --output" >&2
        exit 2
      fi
      OUTPUT_PATH="$2"
      shift 2
      ;;
    --no-build)
      BUILD_ENABLED=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${SB_CONFORMANCE_DSN:-}" ]]; then
  echo "SB_CONFORMANCE_DSN is required." >&2
  exit 2
fi

if [[ ! -f "${MANIFEST_PATH}" ]]; then
  echo "Manifest not found: ${MANIFEST_PATH}" >&2
  exit 2
fi

if [[ "${BUILD_ENABLED}" -eq 1 ]]; then
  cmake -S "${CLI_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}" --target sbdriver_conformance
fi

if [[ ! -x "${BINARY_PATH}" ]]; then
  echo "Conformance binary not found: ${BINARY_PATH}" >&2
  exit 2
fi

mkdir -p "$(dirname "${OUTPUT_PATH}")"
echo "Running sbdriver_conformance with manifest: ${MANIFEST_PATH}" >&2
echo "Writing JSON output to: ${OUTPUT_PATH}" >&2

"${BINARY_PATH}" "${DRIVER_ARGS[@]}" < "${MANIFEST_PATH}" | tee "${OUTPUT_PATH}"
