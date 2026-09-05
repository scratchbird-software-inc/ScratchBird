#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
profile="${1:-core}"
compose=(docker compose --project-directory "${script_dir}" -f "${script_dir}/compose.yaml")
if [[ -f "${script_dir}/.env" ]]; then
  # shellcheck disable=SC1091
  source "${script_dir}/.env"
  compose+=(--env-file "${script_dir}/.env")
fi

mkdir -p "${script_dir}/logs"
log_path="${script_dir}/logs/smoke-${profile}-$(date -u +%Y%m%dT%H%M%SZ).log"
exec > >(tee "${log_path}") 2>&1

retry() {
  local description="$1"
  shift
  for attempt in $(seq 1 40); do
    if "$@"; then
      return 0
    fi
    if [[ "${attempt}" == "40" ]]; then
      echo "authentication_lab_smoke_failure=${description}" >&2
      return 1
    fi
    sleep 1
  done
}

has_profile() {
  local expected="$1"
  [[ "${profile}" == "${expected}" || "${profile}" == "full" ||
     ("${expected}" == "core" && ("${profile}" == "enterprise" || "${profile}" == "browser")) ]]
}

smoke_core() {
  local keycloak_port="${SB_AUTH_LAB_KEYCLOAK_PORT:-18080}"
  local mtls_port="${SB_AUTH_LAB_MTLS_PORT:-18443}"
  local proxy_port="${SB_AUTH_LAB_PROXY_PORT:-18081}"
  local oidc_fault_port="${SB_AUTH_LAB_OIDC_FAULT_PORT:-28080}"
  local pki="${script_dir}/generated/pki"

  retry oidc_discovery curl -fsS \
    "http://127.0.0.1:${keycloak_port}/realms/scratchbird/.well-known/openid-configuration" \
    -o /tmp/scratchbird-auth-lab-oidc.json
  grep -q 'scratchbird' /tmp/scratchbird-auth-lab-oidc.json

  retry oidc_password_grant curl -fsS -X POST \
    "http://127.0.0.1:${keycloak_port}/realms/scratchbird/protocol/openid-connect/token" \
    -d grant_type=password -d client_id=scratchbird-cli \
    -d client_secret=scratchbird-client-secret \
    -d username=alice -d password=alice-password \
    -o /tmp/scratchbird-auth-lab-token.json
  grep -q 'access_token' /tmp/scratchbird-auth-lab-token.json
  if curl -fsS -X POST \
      "http://127.0.0.1:${keycloak_port}/realms/scratchbird/protocol/openid-connect/token" \
      -d grant_type=password -d client_id=scratchbird-cli \
      -d client_secret=scratchbird-client-secret \
      -d username=alice -d password=wrong-password >/dev/null 2>&1; then
    echo "OIDC accepted an invalid password" >&2
    return 1
  fi
  curl -fsS "http://127.0.0.1:${keycloak_port}/realms/scratchbird/protocol/saml/descriptor" \
    | grep -q 'EntityDescriptor'

  "${compose[@]}" --profile core exec -T openldap ldapsearch -x \
    -H ldap://127.0.0.1:1389 \
    -D uid=alice,ou=people,dc=scratchbird,dc=test \
    -w alice-password -b dc=scratchbird,dc=test '(uid=alice)' dn \
    | grep -q 'uid=alice'
  if "${compose[@]}" --profile core exec -T openldap ldapwhoami -x \
      -H ldap://127.0.0.1:1389 \
      -D uid=alice,ou=people,dc=scratchbird,dc=test \
      -w wrong-password >/dev/null 2>&1; then
    echo "LDAP accepted an invalid password" >&2
    return 1
  fi
  ldaps_search() {
    "${compose[@]}" --profile core exec -T openldap \
      env LDAPTLS_CACERT=/pki/ca.crt ldapsearch -x \
      -H ldaps://ldap.scratchbird.test:1636 \
      -D uid=alice,ou=people,dc=scratchbird,dc=test \
      -w alice-password -b dc=scratchbird,dc=test '(uid=alice)' dn \
      | grep -q 'uid=alice'
  }
  retry ldaps_bind ldaps_search
  if "${compose[@]}" --profile core exec -T openldap \
      env LDAPTLS_REQCERT=demand LDAPTLS_CACERT=/pki/untrusted-client.crt \
      ldapwhoami -x -H ldaps://ldap.scratchbird.test:1636 \
      -D uid=alice,ou=people,dc=scratchbird,dc=test \
      -w alice-password >/dev/null 2>&1; then
    echo "LDAPS accepted an untrusted CA" >&2
    return 1
  fi

  "${compose[@]}" --profile core exec -T radius /opt/bin/radtest \
    alice alice-password 127.0.0.1 0 scratchbird-radius-secret \
    | grep -q 'Access-Accept'
  radius_reject="$("${compose[@]}" --profile core exec -T radius \
    /opt/bin/radtest alice wrong-password 127.0.0.1 0 \
    scratchbird-radius-secret 2>&1 || true)"
  grep -q 'Access-Reject' <<<"${radius_reject}"

  curl -fsS --cacert "${pki}/ca.crt" \
    --cert "${pki}/alice-client.crt" --key "${pki}/alice-client.key" \
    "https://localhost:${mtls_port}/identity" | grep -q 'SUCCESS'
  openssl x509 -in "${pki}/wrong-san-client.crt" -noout \
    -ext subjectAltName | grep -q 'spiffe://untrusted.example/user/alice'
  for rejected in revoked-client wrong-eku-client untrusted-client; do
    if curl -fsS --cacert "${pki}/ca.crt" \
        --cert "${pki}/${rejected}.crt" --key "${pki}/${rejected}.key" \
        "https://localhost:${mtls_port}/identity" >/dev/null 2>&1; then
      echo "mTLS accepted ${rejected}" >&2
      return 1
    fi
  done
  if curl -fsS --cacert "${pki}/ca.crt" \
      "https://localhost:${mtls_port}/identity" >/dev/null 2>&1; then
    echo "mTLS accepted a request without a client certificate" >&2
    return 1
  fi

  curl -fsS "http://127.0.0.1:${proxy_port}/assertion?scenario=valid" \
    | grep -q 'HMAC-SHA256'
  curl -fsS "http://127.0.0.1:${proxy_port}/assertion?scenario=bad_signature" \
    | grep -q 'bad_signature'
  retry toxiproxy_oidc curl -fsS \
    "http://127.0.0.1:${oidc_fault_port}/realms/scratchbird/.well-known/openid-configuration" \
    -o /dev/null
}

smoke_enterprise() {
  samba_groups="$("${compose[@]}" --profile enterprise exec -T samba-ad \
    samba-tool group listmembers database-users)"
  grep -q '^alice$' <<<"${samba_groups}"
  disabled_user="$("${compose[@]}" --profile enterprise exec -T samba-ad \
    samba-tool user show disabled-alice)"
  grep -q '^userAccountControl: 514$' <<<"${disabled_user}"
  kerberos_ticket="$("${compose[@]}" --profile enterprise exec -T samba-ad \
    bash -lc "printf '%s\\n' 'Alice-Password1!' | kinit alice@SCRATCHBIRD.TEST && klist" \
    2>&1)"
  grep -q 'alice@SCRATCHBIRD.TEST' <<<"${kerberos_ticket}"
  pam_result="$("${compose[@]}" --profile enterprise exec -T unix-auth \
    bash -lc "printf '%s\\n' 'alice-password' | pamtester scratchbird alice authenticate" \
    2>&1)"
  grep -q 'successfully authenticated' <<<"${pam_result}"
  peer_result="$("${compose[@]}" --profile enterprise exec -T --user 10001:10001 \
    unix-auth python3 /opt/scratchbird/peer_client.py \
    /run/scratchbird-auth/peer.sock)"
  grep -q '"uid": 10001' <<<"${peer_result}"
}

smoke_browser() {
  local selenium_port="${SB_AUTH_LAB_SELENIUM_PORT:-14444}"
  retry selenium curl -fsS "http://127.0.0.1:${selenium_port}/status" \
    -o /tmp/scratchbird-auth-lab-selenium.json
  grep -Eq '"ready"[[:space:]]*:[[:space:]]*true' \
    /tmp/scratchbird-auth-lab-selenium.json
  python3 "${script_dir}/scripts/browser_webauthn_smoke.py" \
    "http://127.0.0.1:${selenium_port}"
}

smoke_workload() {
  "${compose[@]}" --profile workload exec -T spire-server \
    /opt/spire/bin/spire-server entry show \
    -socketPath /run/spire/server/private/api.sock \
    | grep -q 'spiffe://scratchbird.test/workload/scratchbird'
  "${compose[@]}" --profile workload exec -T spire-agent \
    /opt/spire/bin/spire-agent healthcheck \
    -socketPath /run/spire/agent/public/api.sock
}

has_profile core && smoke_core
has_profile enterprise && smoke_enterprise
has_profile browser && smoke_browser
has_profile workload && smoke_workload

echo "authentication_lab_smoke=${profile}:passed"
echo "authentication_lab_smoke_log=${log_path}"
