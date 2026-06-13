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
  cat <<'USAGE'
Usage:
  build-p2-update-site.sh [output-dir]

Description:
  Builds the ScratchBird DBeaver extension and assembles a p2 update-site zip
  that can be installed into a stock DBeaver download with:
  Help -> Install New Software -> Add -> Archive...
USAGE
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
WORK_DIR="${SCRATCHBIRD_DBEAVER_WORK_DIR:-${BUILD_ROOT}/p2-work}"
BUILD_INTEGRATION_DIR="${WORK_DIR}/source"
JDBC_DIR="$(cd "${SCRATCHBIRD_JDBC_DIR:-${PROJECT_DRIVERS_DIR}/driver/jdbc}" && pwd)"
JDBC_BUILD_ROOT="${SCRATCHBIRD_JDBC_BUILD_ROOT:-${BUILD_ROOT}/jdbc}"
MAVEN_REPO_LOCAL="${MAVEN_REPO_LOCAL:-${BUILD_ROOT}/maven-repo}"
PLUGIN_DIR="${BUILD_INTEGRATION_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird"
PLUGIN_DRIVER_DIR="${PLUGIN_DIR}/drivers/scratchbird"

cleanup_staged_driver() {
  rm -f "${PLUGIN_DRIVER_DIR}/scratchbird-jdbc.jar"
  rmdir "${PLUGIN_DRIVER_DIR}" 2>/dev/null || true
}

copy_tree_for_build() {
  local src="$1"
  local dst="$2"
  rm -rf "${dst}"
  mkdir -p "$(dirname "${dst}")"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete \
      --exclude '/dist/' \
      --exclude '/repository/target/' \
      --exclude '/plugins/*/target/' \
      --exclude '/features/*/target/' \
      --exclude '/test/*/target/' \
      "${src}/" "${dst}/"
  else
    cp -a "${src}" "${dst}"
    find "${dst}" -type d \( -name target -o -name dist \) -prune -exec rm -rf {} +
  fi
}

find_jdbc_jar() {
  if [[ -n "${SCRATCHBIRD_JDBC_JAR:-}" ]]; then
    printf '%s\n' "${SCRATCHBIRD_JDBC_JAR}"
    return 0
  fi

  local libs_dir="${JDBC_BUILD_ROOT}/stage/build/libs"
  if [[ ! -d "${libs_dir}" ]]; then
    return 0
  fi

  find "${libs_dir}" -maxdepth 1 -type f -name 'scratchbird-jdbc-*.jar' \
    ! -name '*-sources.jar' \
    ! -name '*-javadoc.jar' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n 1 | cut -d' ' -f2-
}

build_jdbc_jar() {
  local jdbc_stage="${JDBC_BUILD_ROOT}/stage"
  echo "Building ScratchBird JDBC jar for DBeaver packaging..." >&2
  copy_tree_for_build "${JDBC_DIR}" "${jdbc_stage}"
  (
    cd "${jdbc_stage}"
    ./gradlew --no-daemon --console=plain -g "${BUILD_ROOT}/gradle-home" jar
  )
}

stage_jdbc_driver() {
  local jdbc_jar
  jdbc_jar="$(find_jdbc_jar)"
  if [[ -z "${jdbc_jar}" ]]; then
    build_jdbc_jar
    jdbc_jar="$(find_jdbc_jar)"
  fi

  if [[ -z "${jdbc_jar}" || ! -f "${jdbc_jar}" ]]; then
    echo "ScratchBird JDBC jar not found under ${JDBC_BUILD_ROOT}/stage/build/libs" >&2
    exit 1
  fi

  mkdir -p "${PLUGIN_DRIVER_DIR}"
  cp "${jdbc_jar}" "${PLUGIN_DRIVER_DIR}/scratchbird-jdbc.jar"
}

trap cleanup_staged_driver EXIT

choose_maven() {
  if [[ -n "${MAVEN_CMD:-}" ]]; then
    echo "${MAVEN_CMD}"
    return 0
  fi

  if [[ -x "${HOME}/local workspace/dbeaver-common/mvnw" ]]; then
    echo "${HOME}/local workspace/dbeaver-common/mvnw"
    return 0
  fi

  if command -v mvn >/dev/null 2>&1; then
    echo "mvn"
    return 0
  fi

  return 1
}

MAVEN="$(choose_maven || true)"
if [[ -z "${MAVEN}" ]]; then
  echo "No Maven launcher found. Install 'mvn' or set MAVEN_CMD." >&2
  exit 1
fi

copy_tree_for_build "${INTEGRATION_DIR}" "${BUILD_INTEGRATION_DIR}"

stage_jdbc_driver

${MAVEN} -f "${BUILD_INTEGRATION_DIR}/pom.xml" \
  -Dmaven.repo.local="${MAVEN_REPO_LOCAL}" \
  clean verify

REPOSITORY_DIR="${BUILD_INTEGRATION_DIR}/repository/target/repository"
if [[ ! -f "${REPOSITORY_DIR}/content.jar" && ! -f "${REPOSITORY_DIR}/content.xml" ]]; then
  echo "p2 repository output was not generated: ${REPOSITORY_DIR}" >&2
  exit 1
fi

mkdir -p "${OUTPUT_DIR}"
TIMESTAMP="$(date +%Y%m%dT%H%M%S)"
ZIP_PATH="$(cd "${OUTPUT_DIR}" && pwd)/scratchbird-dbeaver-update-site-${TIMESTAMP}.zip"

(
  cd "${REPOSITORY_DIR}"
  zip -qr "${ZIP_PATH}" .
)

cat <<EOF2
p2 update site built successfully.

Repository folder:
  ${REPOSITORY_DIR}

Archive:
  ${ZIP_PATH}

Install in DBeaver:
  Help -> Install New Software -> Add -> Archive...
EOF2
