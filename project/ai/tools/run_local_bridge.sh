#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONPATH="${ROOT_DIR}/src${PYTHONPATH:+:${PYTHONPATH}}"

: "${SCRATCHBIRD_AI_BRIDGE_HOST:=127.0.0.1}"
: "${SCRATCHBIRD_AI_BRIDGE_PORT:=3095}"
: "${SCRATCHBIRD_AI_BRIDGE_DIALECTS:=native}"

cat <<EOF
Starting ScratchBird AI HTTP bridge
  host: ${SCRATCHBIRD_AI_BRIDGE_HOST}
  port: ${SCRATCHBIRD_AI_BRIDGE_PORT}
  dialects: ${SCRATCHBIRD_AI_BRIDGE_DIALECTS}
EOF

exec python3 -m scratchbird_ai.http_bridge
