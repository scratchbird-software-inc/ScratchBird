#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
compose=(docker compose --project-directory "${script_dir}" -f "${script_dir}/compose.yaml")
if [[ -f "${script_dir}/.env" ]]; then
  compose+=(--env-file "${script_dir}/.env")
fi

usage() {
  cat <<'EOF'
Usage: lab.sh <command> [profile]

Commands:
  validate                 Validate tracked fixtures and Compose expansion.
  up [core|enterprise|browser|workload|full]
                           Start a profile (default: core).
  smoke [profile]          Run service-level positive and negative checks.
  status [profile]         Show containers for a profile.
  logs [profile]           Write combined service logs to logs/.
  fault <oidc|ldap|mtls> <cut|restore|latency> [milliseconds]
                           Control a Toxiproxy provider path.
  down                     Stop the lab and remove its disposable volumes.
EOF
}

require_docker() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "authentication_lab_skip=docker_cli_missing" >&2
    exit 77
  fi
  if ! docker compose version >/dev/null 2>&1; then
    echo "authentication_lab_skip=docker_compose_missing" >&2
    exit 77
  fi
  if ! docker info >/dev/null 2>&1; then
    echo "authentication_lab_skip=docker_daemon_unavailable" >&2
    exit 77
  fi
}

profile_args() {
  local profile="$1"
  case "${profile}" in
    core)
      printf '%s\n' --profile core
      ;;
    enterprise)
      printf '%s\n' --profile core --profile enterprise
      ;;
    browser)
      printf '%s\n' --profile core --profile browser
      ;;
    workload)
      printf '%s\n' --profile workload
      ;;
    full)
      printf '%s\n' --profile core --profile enterprise --profile browser --profile workload
      ;;
    *)
      echo "unknown authentication-lab profile: ${profile}" >&2
      exit 2
      ;;
  esac
}

has_profile() {
  local selected="$1"
  local expected="$2"
  [[ "${selected}" == "${expected}" || "${selected}" == "full" ||
     ("${expected}" == "core" && ("${selected}" == "enterprise" || "${selected}" == "browser")) ]]
}

configure_toxiproxy() {
  local api_port="${SB_AUTH_LAB_TOXIPROXY_API_PORT:-18474}"
  local api="http://127.0.0.1:${api_port}"
  for attempt in $(seq 1 30); do
    if curl -fsS "${api}/version" >/dev/null 2>&1; then
      break
    fi
    if [[ "${attempt}" == "30" ]]; then
      echo "toxiproxy API did not become ready" >&2
      return 1
    fi
    sleep 1
  done

  create_proxy() {
    local name="$1"
    local listen="$2"
    local upstream="$3"
    curl -fsS -X DELETE "${api}/proxies/${name}" >/dev/null 2>&1 || true
    curl -fsS -X POST "${api}/proxies" \
      -H 'Content-Type: application/json' \
      -d "{\"name\":\"${name}\",\"listen\":\"${listen}\",\"upstream\":\"${upstream}\",\"enabled\":true}" \
      >/dev/null
  }

  create_proxy oidc 0.0.0.0:28080 keycloak:8080
  create_proxy ldap 0.0.0.0:21389 openldap:1389
  create_proxy mtls 0.0.0.0:28443 mtls:443
}

bootstrap_spire() {
  local server_socket="/run/spire/server/private/api.sock"
  mkdir -p "${script_dir}/generated/spire"
  "${compose[@]}" --profile workload up -d --wait spire-server
  "${compose[@]}" --profile workload exec -T spire-server \
    /opt/spire/bin/spire-server bundle show -format pem \
    -socketPath "${server_socket}" \
    >"${script_dir}/generated/spire/bootstrap.crt"

  if ! "${compose[@]}" --profile workload exec -T spire-agent \
      /opt/spire/bin/spire-agent healthcheck \
      -socketPath /run/spire/agent/public/api.sock >/dev/null 2>&1; then
    local token_output
    local join_token
    token_output="$("${compose[@]}" --profile workload exec -T spire-server \
      /opt/spire/bin/spire-server token generate \
      -socketPath "${server_socket}" \
      -spiffeID spiffe://scratchbird.test/auth-lab-agent)"
    join_token="$(printf '%s\n' "${token_output}" | sed -n 's/^Token: //p' | head -1)"
    if [[ -z "${join_token}" ]]; then
      echo "SPIRE join-token generation failed: ${token_output}" >&2
      return 1
    fi

    env SB_AUTH_LAB_SPIRE_JOIN_TOKEN="${join_token}" \
      "${compose[@]}" --profile workload up -d --wait spire-agent
  fi

  local entry_output
  entry_output="$("${compose[@]}" --profile workload exec -T spire-server \
    /opt/spire/bin/spire-server entry show \
    -socketPath "${server_socket}")"
  if ! grep -q 'spiffe://scratchbird.test/workload/scratchbird' \
      <<<"${entry_output}"; then
    "${compose[@]}" --profile workload exec -T spire-server \
      /opt/spire/bin/spire-server entry create \
      -socketPath "${server_socket}" \
      -parentID spiffe://scratchbird.test/auth-lab-agent \
      -spiffeID spiffe://scratchbird.test/workload/scratchbird \
      -selector unix:uid:10001 >/dev/null
  fi
}

command="${1:-}"
case "${command}" in
  validate)
    require_docker
    python3 "${script_dir}/validate_lab.py"
    mapfile -t all_profiles < <(profile_args full)
    "${compose[@]}" "${all_profiles[@]}" config --quiet
    echo "authentication_lab_definition=valid"
    ;;
  up)
    require_docker
    profile="${2:-core}"
    "${script_dir}/scripts/generate_pki.sh"
    mapfile -t profiles < <(profile_args "${profile}")

    non_workload_profiles=()
    for ((index = 0; index < ${#profiles[@]}; index += 2)); do
      if [[ "${profiles[index + 1]}" != "workload" ]]; then
        non_workload_profiles+=("${profiles[index]}" "${profiles[index + 1]}")
      fi
    done
    if ((${#non_workload_profiles[@]})); then
      "${compose[@]}" "${non_workload_profiles[@]}" up -d --build --wait
    fi
    if has_profile "${profile}" workload; then
      bootstrap_spire
    fi
    if has_profile "${profile}" core; then
      configure_toxiproxy
    fi
    echo "authentication_lab_started=${profile}"
    ;;
  smoke)
    require_docker
    exec "${script_dir}/smoke.sh" "${2:-core}"
    ;;
  status)
    require_docker
    mapfile -t profiles < <(profile_args "${2:-core}")
    "${compose[@]}" "${profiles[@]}" ps
    ;;
  logs)
    require_docker
    profile="${2:-core}"
    mapfile -t profiles < <(profile_args "${profile}")
    mkdir -p "${script_dir}/logs"
    log_path="${script_dir}/logs/${profile}-$(date -u +%Y%m%dT%H%M%SZ).log"
    "${compose[@]}" "${profiles[@]}" logs --no-color >"${log_path}"
    echo "authentication_lab_log=${log_path}"
    ;;
  fault)
    require_docker
    provider="${2:-}"
    action="${3:-}"
    milliseconds="${4:-500}"
    case "${provider}" in oidc | ldap | mtls) ;; *) usage; exit 2 ;; esac
    api="http://127.0.0.1:${SB_AUTH_LAB_TOXIPROXY_API_PORT:-18474}"
    case "${action}" in
      cut | restore)
        enabled=false
        [[ "${action}" == "restore" ]] && enabled=true
        curl -fsS -X POST "${api}/proxies/${provider}" \
          -H 'Content-Type: application/json' \
          -d "{\"enabled\":${enabled}}" >/dev/null
        ;;
      latency)
        curl -fsS -X DELETE "${api}/proxies/${provider}/toxics/scratchbird-latency" \
          >/dev/null 2>&1 || true
        curl -fsS -X POST "${api}/proxies/${provider}/toxics" \
          -H 'Content-Type: application/json' \
          -d "{\"name\":\"scratchbird-latency\",\"type\":\"latency\",\"stream\":\"downstream\",\"toxicity\":1.0,\"attributes\":{\"latency\":${milliseconds},\"jitter\":0}}" \
          >/dev/null
        ;;
      *)
        usage
        exit 2
        ;;
    esac
    echo "authentication_lab_fault=${provider}:${action}"
    ;;
  down)
    require_docker
    mapfile -t profiles < <(profile_args full)
    "${compose[@]}" "${profiles[@]}" down --volumes --remove-orphans
    echo "authentication_lab_stopped=true"
    ;;
  *)
    usage
    exit 2
    ;;
esac
