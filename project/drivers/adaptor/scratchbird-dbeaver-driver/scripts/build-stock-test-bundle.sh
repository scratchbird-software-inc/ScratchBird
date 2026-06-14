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
  build-stock-test-bundle.sh [output-dir]

Description:
  Builds a fresh ScratchBird DBeaver p2 update-site, bundles it with stock
  install helpers for Unix and Windows, and writes a single handoff zip for
  tester machines.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTEGRATION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT_DRIVERS_DIR="$(cd "${INTEGRATION_DIR}/../.." && pwd)"
PROJECT_ROOT="$(cd "${PROJECT_DRIVERS_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${PROJECT_ROOT}/.." && pwd)"
BUILD_ROOT="${SCRATCHBIRD_DRIVER_BUILD_ROOT:-${REPO_ROOT}/build/drivers/adaptor/scratchbird-dbeaver-driver}"
OUTPUT_DIR="${1:-${BUILD_ROOT}/dist}"
WORK_DIR="${SCRATCHBIRD_DBEAVER_BUNDLE_WORK_DIR:-${BUILD_ROOT}/stock-bundle-work}"
TIMESTAMP="$(date +%Y%m%dT%H%M%S)"
P2_OUTPUT_DIR="${WORK_DIR}/p2"
BUNDLE_DIR_NAME="scratchbird-dbeaver-stock-test-bundle-${TIMESTAMP}"
BUNDLE_DIR="${WORK_DIR}/${BUNDLE_DIR_NAME}"

cleanup() {
  rm -rf "${WORK_DIR}"
}

trap cleanup EXIT

find_update_site_zip() {
  if [[ ! -d "${P2_OUTPUT_DIR}" ]]; then
    return 0
  fi
  find "${P2_OUTPUT_DIR}" -maxdepth 1 -type f -name 'scratchbird-dbeaver-update-site-*.zip' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-
}

rm -rf "${WORK_DIR}"
mkdir -p "${OUTPUT_DIR}"
rm -f \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-update-site-*.zip \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-stock-test-bundle-*.zip \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-source-checkout-proof-*.json \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-stock-install-proof-*.json \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-lifecycle-proof-*.json \
  "${OUTPUT_DIR}"/scratchbird-dbeaver-lifecycle-proof-latest.json \
  "${OUTPUT_DIR}"/SHA256SUMS.txt

"${SCRIPT_DIR}/build-p2-update-site.sh" "${P2_OUTPUT_DIR}"

UPDATE_SITE_ZIP="$(find_update_site_zip)"
if [[ -z "${UPDATE_SITE_ZIP}" || ! -f "${UPDATE_SITE_ZIP}" ]]; then
  echo "Fresh ScratchBird update-site zip was not generated under ${P2_OUTPUT_DIR}" >&2
  exit 1
fi
UPDATE_SITE_DIST_ZIP="${OUTPUT_DIR}/$(basename "${UPDATE_SITE_ZIP}")"
cp "${UPDATE_SITE_ZIP}" "${UPDATE_SITE_DIST_ZIP}"

mkdir -p "${BUNDLE_DIR}"
cp "${UPDATE_SITE_ZIP}" "${BUNDLE_DIR}/"
cp "${SCRIPT_DIR}/install-into-stock-dbeaver.sh" "${BUNDLE_DIR}/"
cp "${SCRIPT_DIR}/install-into-stock-dbeaver.bat" "${BUNDLE_DIR}/"
cp "${SCRIPT_DIR}/uninstall-from-stock-dbeaver.sh" "${BUNDLE_DIR}/"
cp "${SCRIPT_DIR}/uninstall-from-stock-dbeaver.bat" "${BUNDLE_DIR}/"

cat > "${BUNDLE_DIR}/README.txt" <<EOF
ScratchBird DBeaver Stock Test Bundle
Built: ${TIMESTAMP}

Files
-----
- $(basename "${UPDATE_SITE_ZIP}")
- install-into-stock-dbeaver.sh
- install-into-stock-dbeaver.bat
- uninstall-from-stock-dbeaver.sh
- uninstall-from-stock-dbeaver.bat
- THIRD-PARTY-NOTICES.txt

Purpose
-------
This bundle installs or removes the ScratchBird DBeaver extension in an
existing packaged DBeaver application without requiring a DBeaver source
checkout.

Unix/Linux/macOS Install
------------------------
1. Unzip this bundle.
2. Run:
   ./install-into-stock-dbeaver.sh
   or:
   ./install-into-stock-dbeaver.sh /path/to/dbeaver-install
   or:
   ./install-into-stock-dbeaver.sh /path/to/dbeaver-launcher

Unix/Linux/macOS Uninstall
--------------------------
1. Unzip this bundle.
2. Run:
   ./uninstall-from-stock-dbeaver.sh
   or:
   ./uninstall-from-stock-dbeaver.sh /path/to/dbeaver-install
   or:
   ./uninstall-from-stock-dbeaver.sh /path/to/dbeaver-launcher

Windows Install
---------------
1. Extract this bundle.
2. Run:
   install-into-stock-dbeaver.bat
   or:
   install-into-stock-dbeaver.bat DBEAVER_INSTALL_DIR
   or:
   install-into-stock-dbeaver.bat DBEAVER_LAUNCHER

Windows Uninstall
-----------------
1. Extract this bundle.
2. Run:
   uninstall-from-stock-dbeaver.bat
   or:
   uninstall-from-stock-dbeaver.bat DBEAVER_INSTALL_DIR
   or:
   uninstall-from-stock-dbeaver.bat DBEAVER_LAUNCHER

Notes
-----
- If no install path is provided, the installers attempt to discover DBeaver
  from DBEAVER_HOME, PATH, and common install locations.
- The installers auto-detect the bundled scratchbird-dbeaver-update-site-*.zip
  when it is kept next to the install scripts.
- The DBeaver install target must be writable.
- After install, restart DBeaver before testing.
EOF

cat > "${BUNDLE_DIR}/THIRD-PARTY-NOTICES.txt" <<'EOF'
ScratchBird DBeaver Stock Test Bundle Third-Party Notices

Component: DBeaver
License: Apache-2.0
Boundary: stock DBeaver application and extension API used by the adapter.

Component: Eclipse Platform
License: EPL-2.0
Boundary: target platform dependencies used by DBeaver and Tycho packaging.

Component: Tycho
License: EPL-2.0
Boundary: build-time Eclipse plugin packaging dependency.

Component: SWT
License: EPL-2.0
Boundary: DBeaver UI dependency.

Component: JFace
License: EPL-2.0
Boundary: DBeaver UI dependency.

Component: ScratchBird JDBC
License: MPL-2.0
Boundary: bundled ScratchBird JDBC driver artifact staged as scratchbird-jdbc.jar.
EOF

if command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${BUNDLE_DIR}"
    sha256sum ./* > SHA256SUMS.txt
  )
fi

BUNDLE_ZIP="$(cd "${OUTPUT_DIR}" && pwd)/${BUNDLE_DIR_NAME}.zip"
(
  cd "${WORK_DIR}"
  zip -qr "${BUNDLE_ZIP}" "${BUNDLE_DIR_NAME}"
)

if command -v sha256sum >/dev/null 2>&1; then
  (
    cd "${OUTPUT_DIR}"
    sha256sum "$(basename "${BUNDLE_ZIP}")" "$(basename "${UPDATE_SITE_DIST_ZIP}")" > SHA256SUMS.txt
  )
fi

cat <<EOF
ScratchBird stock test bundle built successfully.

Bundle zip:
  ${BUNDLE_ZIP}

Update-site zip:
  ${UPDATE_SITE_DIST_ZIP}

Checksum manifest:
  ${OUTPUT_DIR}/SHA256SUMS.txt

Bundle contents:
  ${BUNDLE_DIR_NAME}/README.txt
  ${BUNDLE_DIR_NAME}/$(basename "${UPDATE_SITE_ZIP}")
  ${BUNDLE_DIR_NAME}/install-into-stock-dbeaver.sh
  ${BUNDLE_DIR_NAME}/install-into-stock-dbeaver.bat
  ${BUNDLE_DIR_NAME}/uninstall-from-stock-dbeaver.sh
  ${BUNDLE_DIR_NAME}/uninstall-from-stock-dbeaver.bat
  ${BUNDLE_DIR_NAME}/THIRD-PARTY-NOTICES.txt
EOF
