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
: "${SCRATCHBIRD_AI_ADAPTER_MODE:=http}"
: "${SCRATCHBIRD_AI_HTTP_BASE_URL:=http://${SCRATCHBIRD_AI_BRIDGE_HOST}:${SCRATCHBIRD_AI_BRIDGE_PORT}}"
: "${SCRATCHBIRD_AI_BRIDGE_LOG:=/tmp/scratchbird-ai-bridge.log}"

if ! python3 -c "import mcp.server.fastmcp" >/dev/null 2>&1; then
  cat >&2 <<'EOF'
ScratchBird AI MCP runtime is not installed for the current python3.
Install the optional dependency first, for example:
  python3 -m pip install -e '.[mcp]'
EOF
  exit 1
fi

cleanup() {
  if [[ -n "${BRIDGE_PID:-}" ]]; then
    kill "${BRIDGE_PID}" >/dev/null 2>&1 || true
    wait "${BRIDGE_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

echo "Starting ScratchBird AI HTTP bridge..."
python3 -m scratchbird_ai.http_bridge >"${SCRATCHBIRD_AI_BRIDGE_LOG}" 2>&1 &
BRIDGE_PID=$!

echo "Bridge pid: ${BRIDGE_PID}"
echo "Bridge log: ${SCRATCHBIRD_AI_BRIDGE_LOG}"
sleep 1

echo "Starting ScratchBird AI MCP server (adapter mode=${SCRATCHBIRD_AI_ADAPTER_MODE})..."
exec python3 -m scratchbird_ai.mcp_server
