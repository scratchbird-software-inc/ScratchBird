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
  install-into-stock-dbeaver.sh [path/to/dbeaver-install-or-launcher] [path/to/scratchbird-dbeaver-update-site-*.zip]

Description:
  Installs the ScratchBird DBeaver feature into an existing packaged DBeaver
  application using the p2 director. If no install path is provided, the script
  attempts to discover DBeaver from DBEAVER_HOME, PATH, and common install
  locations. If no zip path is provided, the script looks for a bundled
  scratchbird-dbeaver-update-site-*.zip next to itself.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -gt 2 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

find_bundled_zip() {
  find "${SCRIPT_DIR}" -maxdepth 1 -type f -name 'scratchbird-dbeaver-update-site-*.zip' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-
}

resolve_launcher() {
  local install_dir="$1"
  local candidates=(
    "${install_dir}/dbeaver"
    "${install_dir}/dbeaver-ce"
    "${install_dir}/Contents/MacOS/dbeaver"
    "${install_dir}/DBeaver.app/Contents/MacOS/dbeaver"
  )
  local candidate
  for candidate in "${candidates[@]}"; do
    if [[ -x "${candidate}" ]]; then
      echo "${candidate}"
      return 0
    fi
  done
  return 1
}

canonical_path() {
  local path="$1"
  if command -v readlink >/dev/null 2>&1; then
    readlink -f "${path}" 2>/dev/null && return 0
  fi
  if command -v realpath >/dev/null 2>&1; then
    realpath "${path}" 2>/dev/null && return 0
  fi
  return 1
}

install_dir_from_launcher() {
  local launcher="$1"
  local resolved=""
  local launcher_dir=""
  local contents_dir=""

  if ! resolved="$(canonical_path "${launcher}")"; then
    return 1
  fi

  launcher_dir="$(dirname "${resolved}")"
  contents_dir="$(dirname "${launcher_dir}")"

  if [[ "$(basename "${launcher_dir}")" == "MacOS" && "$(basename "${contents_dir}")" == "Contents" ]]; then
    dirname "${contents_dir}"
    return 0
  fi

  echo "${launcher_dir}"
}

normalize_install_hint() {
  local hint="$1"
  local launcher=""

  if [[ -z "${hint}" ]]; then
    return 1
  fi

  if [[ -d "${hint}" ]]; then
    launcher="$(resolve_launcher "${hint}" || true)"
    if [[ -n "${launcher}" ]]; then
      install_dir_from_launcher "${launcher}"
      return 0
    fi
    return 1
  fi

  if [[ -e "${hint}" ]]; then
    install_dir_from_launcher "${hint}"
    return 0
  fi

  return 1
}

find_install_dir_from_path() {
  local candidate=""
  local resolved=""

  for candidate in dbeaver dbeaver-ce; do
    if resolved="$(command -v "${candidate}" 2>/dev/null)"; then
      normalize_install_hint "${resolved}"
      return 0
    fi
  done

  return 1
}

find_common_install_dir() {
  local candidates=(
    "/usr/share/dbeaver-ce"
    "/usr/share/dbeaver"
    "/opt/dbeaver-ce"
    "/opt/dbeaver"
    "/Applications/DBeaver.app"
    "${HOME}/Applications/DBeaver.app"
  )
  local candidate=""

  for candidate in "${candidates[@]}"; do
    if resolve_launcher "${candidate}" >/dev/null 2>&1; then
      echo "${candidate}"
      return 0
    fi
  done

  return 1
}

auto_detect_install_dir() {
  if [[ -n "${DBEAVER_HOME:-}" ]]; then
    normalize_install_hint "${DBEAVER_HOME}" && return 0
  fi

  find_install_dir_from_path && return 0
  find_common_install_dir && return 0

  return 1
}

INSTALL_HINT="${1:-}"
ZIP_PATH="${2:-}"

if [[ $# -eq 1 && -f "${INSTALL_HINT}" && "${INSTALL_HINT}" == *.zip ]]; then
  ZIP_PATH="${INSTALL_HINT}"
  INSTALL_HINT=""
fi

INSTALL_DIR="$(normalize_install_hint "${INSTALL_HINT}" || auto_detect_install_dir || true)"
if [[ -z "${ZIP_PATH}" ]]; then
  ZIP_PATH="$(find_bundled_zip)"
fi

if [[ -z "${INSTALL_DIR}" ]]; then
  echo "Could not determine a DBeaver install location. Pass the install root or launcher path explicitly." >&2
  exit 1
fi

if [[ -z "${ZIP_PATH}" || ! -f "${ZIP_PATH}" ]]; then
  echo "ScratchBird update-site zip not found. Pass it explicitly or place it next to this script." >&2
  exit 1
fi

LAUNCHER="$(resolve_launcher "${INSTALL_DIR}" || true)"
if [[ -z "${LAUNCHER}" ]]; then
  echo "Could not locate a DBeaver launcher under ${INSTALL_DIR}" >&2
  exit 1
fi

echo "Installing ScratchBird into ${INSTALL_DIR}"
echo "Using update-site archive ${ZIP_PATH}"

# Replace any existing ScratchBird feature first so p2 doesn't fail on version conflicts.
"${LAUNCHER}" -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -uninstallIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group \
  -destination "${INSTALL_DIR}" \
  -profile DefaultProfile \
  -bundlepool "${INSTALL_DIR}" \
  -profileProperties org.eclipse.update.install.features=true \
  -purgeHistory || true

"${LAUNCHER}" -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -repository "jar:file:${ZIP_PATH}!/" \
  -installIU org.jkiss.dbeaver.ext.scratchbird.feature.feature.group \
  -destination "${INSTALL_DIR}" \
  -profile DefaultProfile \
  -bundlepool "${INSTALL_DIR}" \
  -profileProperties org.eclipse.update.install.features=true

ROOTS_OUTPUT="$(mktemp)"
trap 'rm -f "${ROOTS_OUTPUT}"' EXIT

"${LAUNCHER}" -nosplash -consoleLog \
  -application org.eclipse.equinox.p2.director \
  -destination "${INSTALL_DIR}" \
  -profile DefaultProfile \
  -bundlepool "${INSTALL_DIR}" \
  -listInstalledRoots > "${ROOTS_OUTPUT}" 2>&1

cat "${ROOTS_OUTPUT}"

if ! grep -q 'org\.jkiss\.dbeaver\.ext\.scratchbird\.feature\.feature\.group' "${ROOTS_OUTPUT}"; then
  echo "ScratchBird feature was not found in the installed roots output." >&2
  exit 1
fi

cat <<EOF
ScratchBird install completed successfully.

Launcher:
  ${LAUNCHER}

Installed root confirmed:
  org.jkiss.dbeaver.ext.scratchbird.feature.feature.group
EOF
