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
  install-into-dbeaver.sh /absolute/or/relative/path/to/dbeaver

Description:
  Copies ScratchBird DBeaver plugin sources into a DBeaver source checkout and
  patches required module/feature files so the plugin is built and bundled.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

if [[ $# -ne 1 ]]; then
  usage >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INTEGRATION_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DBEAVER_DIR="$(cd "$1" && pwd)"
PROJECT_DRIVERS_DIR="$(cd "${INTEGRATION_DIR}/../.." && pwd)"
PROJECT_ROOT="$(cd "${PROJECT_DRIVERS_DIR}/.." && pwd)"
REPO_ROOT="$(cd "${PROJECT_ROOT}/.." && pwd)"
BUILD_ROOT="${SCRATCHBIRD_DRIVER_BUILD_ROOT:-${REPO_ROOT}/build/drivers/adaptor/scratchbird-dbeaver-driver}"
JDBC_DIR="$(cd "${SCRATCHBIRD_JDBC_DIR:-${PROJECT_DRIVERS_DIR}/driver/jdbc}" && pwd)"
JDBC_BUILD_ROOT="${SCRATCHBIRD_JDBC_BUILD_ROOT:-${BUILD_ROOT}/jdbc}"

require_file() {
  local file="$1"
  if [[ ! -f "${file}" ]]; then
    echo "Missing expected file: ${file}" >&2
    exit 1
  fi
}

insert_after_regex_once() {
  local file="$1"
  local presence_regex="$2"
  local anchor_regex="$3"
  local new_line="$4"

  if grep -Eq "${presence_regex}" "${file}"; then
    return 0
  fi

  local tmp
  tmp="$(mktemp)"

  if ! awk -v re="${anchor_regex}" -v ins="${new_line}" '
    {
      print $0
      if (!inserted && $0 ~ re) {
        print ins
        inserted = 1
      }
    }
    END {
      if (!inserted) {
        exit 2
      }
    }
  ' "${file}" > "${tmp}"; then
    rm -f "${tmp}"
    echo "Failed to patch ${file}: anchor regex not found: ${anchor_regex}" >&2
    exit 1
  fi

  mv "${tmp}" "${file}"
}

sync_dir() {
  local src="$1"
  local dst="$2"
  rm -rf "${dst}"
  mkdir -p "$(dirname "${dst}")"
  cp -a "${src}" "${dst}"
}

copy_tree_for_build() {
  local src="$1"
  local dst="$2"
  rm -rf "${dst}"
  mkdir -p "$(dirname "${dst}")"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete \
      --exclude '/build/' \
      --exclude '/.gradle/' \
      "${src}/" "${dst}/"
  else
    cp -a "${src}" "${dst}"
    rm -rf "${dst}/build" "${dst}/.gradle"
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
  echo "Building ScratchBird JDBC jar for the DBeaver checkout..." >&2
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

  mkdir -p "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird/drivers/scratchbird"
  cp "${jdbc_jar}" \
    "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird/drivers/scratchbird/scratchbird-jdbc.jar"
}

write_dbeaver_reactor_poms() {
  cat > "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird/pom.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<project xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd"
         xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
    <modelVersion>4.0.0</modelVersion>
    <parent>
        <groupId>org.jkiss.dbeaver</groupId>
        <artifactId>plugins</artifactId>
        <version>1.0.0-SNAPSHOT</version>
        <relativePath>../</relativePath>
    </parent>
    <artifactId>org.jkiss.dbeaver.ext.scratchbird</artifactId>
    <version>1.0.1-SNAPSHOT</version>
    <packaging>eclipse-plugin</packaging>
</project>
EOF

  cat > "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird.ui/pom.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<project xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd"
         xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
    <modelVersion>4.0.0</modelVersion>
    <parent>
        <groupId>org.jkiss.dbeaver</groupId>
        <artifactId>plugins</artifactId>
        <version>1.0.0-SNAPSHOT</version>
        <relativePath>../</relativePath>
    </parent>
    <artifactId>org.jkiss.dbeaver.ext.scratchbird.ui</artifactId>
    <version>1.0.1-SNAPSHOT</version>
    <packaging>eclipse-plugin</packaging>
</project>
EOF

  cat > "${DBEAVER_DIR}/test/org.jkiss.dbeaver.ext.scratchbird.test/pom.xml" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<project xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd"
         xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
    <modelVersion>4.0.0</modelVersion>
    <parent>
        <groupId>org.jkiss.dbeaver</groupId>
        <artifactId>tests</artifactId>
        <version>1.0.0-SNAPSHOT</version>
        <relativePath>../</relativePath>
    </parent>
    <artifactId>org.jkiss.dbeaver.ext.scratchbird.test</artifactId>
    <version>1.0.0-SNAPSHOT</version>
    <packaging>eclipse-test-plugin</packaging>
</project>
EOF
}

require_file "${DBEAVER_DIR}/plugins/pom.xml"
require_file "${DBEAVER_DIR}/test/pom.xml"
require_file "${DBEAVER_DIR}/features/org.jkiss.dbeaver.db.feature/feature.xml"
require_file "${DBEAVER_DIR}/features/org.jkiss.dbeaver.test.feature/feature.xml"

sync_dir \
  "${INTEGRATION_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird" \
  "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird"

sync_dir \
  "${INTEGRATION_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird.ui" \
  "${DBEAVER_DIR}/plugins/org.jkiss.dbeaver.ext.scratchbird.ui"

sync_dir \
  "${INTEGRATION_DIR}/test/org.jkiss.dbeaver.ext.scratchbird.test" \
  "${DBEAVER_DIR}/test/org.jkiss.dbeaver.ext.scratchbird.test"

write_dbeaver_reactor_poms
stage_jdbc_driver

insert_after_regex_once \
  "${DBEAVER_DIR}/plugins/pom.xml" \
  "<module>org[.]jkiss[.]dbeaver[.]ext[.]scratchbird</module>" \
  "org[.]jkiss[.]dbeaver[.]ext[.]generic</module>" \
  "        <module>org.jkiss.dbeaver.ext.scratchbird</module>"

insert_after_regex_once \
  "${DBEAVER_DIR}/plugins/pom.xml" \
  "<module>org[.]jkiss[.]dbeaver[.]ext[.]scratchbird[.]ui</module>" \
  "org[.]jkiss[.]dbeaver[.]ext[.]scratchbird</module>" \
  "        <module>org.jkiss.dbeaver.ext.scratchbird.ui</module>"

insert_after_regex_once \
  "${DBEAVER_DIR}/test/pom.xml" \
  "<module>org[.]jkiss[.]dbeaver[.]ext[.]scratchbird[.]test</module>" \
  "org[.]jkiss[.]dbeaver[.]ext[.]generic[.]test</module>" \
  "        <module>org.jkiss.dbeaver.ext.scratchbird.test</module>"

insert_after_regex_once \
  "${DBEAVER_DIR}/features/org.jkiss.dbeaver.db.feature/feature.xml" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]scratchbird\"" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]generic\"" \
  "    <plugin id=\"org.jkiss.dbeaver.ext.scratchbird\" version = \"0.0.0\"/>"

insert_after_regex_once \
  "${DBEAVER_DIR}/features/org.jkiss.dbeaver.db.feature/feature.xml" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]scratchbird[.]ui\"" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]scratchbird\"" \
  "    <plugin id=\"org.jkiss.dbeaver.ext.scratchbird.ui\" version = \"0.0.0\"/>"

insert_after_regex_once \
  "${DBEAVER_DIR}/features/org.jkiss.dbeaver.test.feature/feature.xml" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]scratchbird[.]test\"" \
  "plugin id=\"org[.]jkiss[.]dbeaver[.]ext[.]generic[.]test\"" \
  "    <plugin id=\"org.jkiss.dbeaver.ext.scratchbird.test\" version=\"0.0.0\"/>"

cat <<EOF
ScratchBird DBeaver source checkout install completed successfully.

ScratchBird DBeaver integration installed into:
  ${DBEAVER_DIR}

Next steps:
  1) Build plugin/test in DBeaver:
     cd ${DBEAVER_DIR}
     ../dbeaver-common/mvnw -f product/aggregate/pom.xml -pl ../../../dbeaver-common/modules/org.jkiss.utils,../../../dbeaver-common/modules/com.dbeaver.jdbc.api,../../plugins/org.jkiss.dbeaver.model,../../plugins/org.jkiss.dbeaver.model.rcp,../../plugins/org.jkiss.dbeaver.model.jdbc,../../plugins/org.jkiss.dbeaver.model.lsm,../../plugins/org.jkiss.dbeaver.model.sql,../../plugins/org.jkiss.dbeaver.model.sql.jdbc,../../plugins/org.jkiss.dbeaver.registry,../../plugins/org.jkiss.dbeaver.ui,../../plugins/org.jkiss.dbeaver.ui.forms,../../plugins/org.jkiss.dbeaver.ui.editors.base,../../plugins/org.jkiss.dbeaver.ui.editors.connection,../../plugins/org.jkiss.dbeaver.ui.editors.data,../../plugins/org.jkiss.dbeaver.ui.editors.entity,../../plugins/org.jkiss.dbeaver.ui.navigator,../../plugins/org.jkiss.dbeaver.data.transfer,../../plugins/org.jkiss.dbeaver.data.transfer.ui,../../plugins/org.jkiss.dbeaver.tasks.ui,../../plugins/org.jkiss.dbeaver.tasks.native,../../plugins/org.jkiss.dbeaver.tasks.native.ui,../../plugins/org.jkiss.dbeaver.ui.editors.sql,../../plugins/org.jkiss.dbeaver.ext.generic,../../plugins/org.jkiss.dbeaver.ext.scratchbird,../../plugins/org.jkiss.dbeaver.ext.scratchbird.ui,../../test/org.jkiss.dbeaver.ext.scratchbird.test -am verify
EOF
