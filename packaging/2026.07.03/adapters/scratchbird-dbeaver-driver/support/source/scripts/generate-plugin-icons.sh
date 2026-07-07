#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

if [ -z "${BASH_VERSION:-}" ]; then
  if command -v bash >/dev/null 2>&1; then
    exec bash "$0" "$@"
  fi
  echo "This script requires bash." >&2
  exit 1
fi
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  generate-plugin-icons.sh [path/to/ScratchBirdLogo.svg]

Description:
  Regenerates the ScratchBird DBeaver plugin icons from the canonical SVG logo.
  The default input is the artwork export under this public project tree.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if ! command -v inkscape >/dev/null 2>&1; then
  echo "Inkscape is required to regenerate the plugin icons." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTEGRATION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEFAULT_SVG="${SCRATCHBIRD_LOGO_SVG:-${INTEGRATION_DIR}/assets/ScratchBirdLogo.svg}"
SOURCE_SVG="${1:-${DEFAULT_SVG}}"
ICON_DIR="${INTEGRATION_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird/icons"

if [[ ! -f "${SOURCE_SVG}" ]]; then
  echo "ScratchBird logo SVG not found: ${SOURCE_SVG}" >&2
  exit 1
fi

mkdir -p "${ICON_DIR}"

inkscape "${SOURCE_SVG}" \
  --export-type=png \
  --export-filename="${ICON_DIR}/scratchbird_icon.png" \
  -w 16 \
  -h 16 >/dev/null 2>&1

inkscape "${SOURCE_SVG}" \
  --export-type=png \
  --export-filename="${ICON_DIR}/scratchbird_icon_big.png" \
  -w 64 \
  -h 64 >/dev/null 2>&1

cat <<EOF
ScratchBird plugin icons regenerated successfully.

Source SVG:
  ${SOURCE_SVG}

Generated files:
  ${ICON_DIR}/scratchbird_icon.png
  ${ICON_DIR}/scratchbird_icon_big.png
EOF
