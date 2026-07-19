#!/bin/bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

# Prove the installed fixed-selector launcher clears launchd's host-computed
# groups before it execs a ScratchBird process. This selects only the bootstrap
# authority probe; it never starts a server, listener, manager, parser, or
# database process.

set -euo pipefail

fail() {
  echo "smoke_macos_launchd_credential=fail:$1" >&2
  exit 1
}

[[ "$(uname -s)" == Darwin ]] || fail "darwin_required"
[[ $# -eq 3 ]] || fail "usage"

probe_source=$1
profile_path=$2
proof_root=$3

[[ "$probe_source" = /* && -f "$probe_source" && -x "$probe_source" ]] || \
  fail "probe_invalid"
[[ "$profile_path" == "/Library/Application Support/ScratchBird/SBbootstrap.profile" ]] || \
  fail "profile_path_invalid"
sudo test -f "$profile_path" || fail "profile_missing"
sudo test ! -L "$profile_path" || fail "profile_symlink"

mkdir -p "$proof_root"
proof_root="$(cd "$proof_root" && pwd -P)"

label=com.scratchbird.credential-probe
runtime_plist=/Library/LaunchDaemons/com.scratchbird.credential-probe.plist
runtime_probe=/var/lib/scratchbird/install/sb_bootstrap_launchd_credential_probe
runtime_launcher=/opt/ScratchBird/bin/SBlaunch
runtime_stdout=/var/log/scratchbird/launchd/credential-probe.out
runtime_stderr=/var/log/scratchbird/launchd/credential-probe.err
canary_root=/var/lib/scratchbird/install/launchd-credential-canaries
proof_plist="$proof_root/launchd-credential-probe.plist"

sudo test -x "$runtime_launcher" || fail "launcher_missing"
sudo test ! -L "$runtime_launcher" || fail "launcher_symlink"
launcher_metadata=$(sudo stat -f '%u:%Lp' "$runtime_launcher" 2>/dev/null || true)
[[ "$launcher_metadata" == 0:* ]] || fail "launcher_owner_invalid"
launcher_mode=${launcher_metadata#*:}
[[ "$launcher_mode" =~ ^[0-7]{3,4}$ ]] || fail "launcher_mode_invalid"
(( (8#$launcher_mode & 0022) == 0 )) || fail "launcher_write_authority_invalid"

for path in "$runtime_plist" "$runtime_probe" "$runtime_stdout" \
  "$runtime_stderr" "$canary_root"; do
  if sudo test -e "$path" || sudo test -L "$path"; then
    fail "preexisting_path:$path"
  fi
done
if sudo launchctl print "system/$label" >/dev/null 2>&1; then
  fail "preexisting_launchd_job"
fi
for service_label in com.scratchbird.sbsrv com.scratchbird.sbmgr; do
  if sudo launchctl print "system/$service_label" >/dev/null 2>&1; then
    fail "scratchbird_service_preloaded:$service_label"
  fi
done

cleanup() {
  local primary_status=$?
  local cleanup_status=0
  trap - EXIT
  set +e
  if sudo launchctl print "system/$label" >/dev/null 2>&1; then
    sudo launchctl bootout "system/$label" >/dev/null 2>&1 || cleanup_status=1
  fi
  sudo launchctl print "system/$label" >/dev/null 2>&1 && cleanup_status=1
  sudo rm -f "$runtime_plist" "$runtime_probe" \
    "$runtime_stdout" "$runtime_stderr" || cleanup_status=1
  sudo rm -rf "$canary_root" || cleanup_status=1
  for path in "$runtime_plist" "$runtime_probe" "$runtime_stdout" \
    "$runtime_stderr" "$canary_root"; do
    if sudo test -e "$path" || sudo test -L "$path"; then
      cleanup_status=1
    fi
  done
  set -e
  if [[ "$primary_status" -ne 0 ]]; then
    exit "$primary_status"
  fi
  exit "$cleanup_status"
}
trap cleanup EXIT

cat > "$proof_plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>com.scratchbird.credential-probe</string>
  <key>ProgramArguments</key>
  <array>
    <string>/opt/ScratchBird/bin/SBlaunch</string>
    <string>credential-probe</string>
  </array>
  <key>UserName</key>
  <string>root</string>
  <key>GroupName</key>
  <string>wheel</string>
  <key>InitGroups</key>
  <false/>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <false/>
  <key>StandardOutPath</key>
  <string>/var/log/scratchbird/launchd/credential-probe.out</string>
  <key>StandardErrorPath</key>
  <string>/var/log/scratchbird/launchd/credential-probe.err</string>
</dict>
</plist>
PLIST

plutil -lint "$proof_plist" >/dev/null
primary_gid="$(sudo dscl . -read /Users/scratchbird PrimaryGroupID | awk 'NR == 1 { print $2 }')"
[[ "$primary_gid" =~ ^[0-9]+$ && "$primary_gid" -ne 0 ]] || \
  fail "primary_group_invalid"
read -r -a resolved_group_ids <<< "$(id -G scratchbird)"
read -r -a resolved_group_names <<< "$(id -Gn scratchbird)"
[[ "${#resolved_group_ids[@]}" -eq "${#resolved_group_names[@]}" ]] || \
  fail "resolved_group_inventory_mismatch"
sudo install -d -o root -g scratchbird -m 0750 "$canary_root"
canary_count=0
for index in "${!resolved_group_ids[@]}"; do
  gid=${resolved_group_ids[$index]}
  name=${resolved_group_names[$index]}
  [[ "$gid" =~ ^[0-9]+$ ]] || fail "resolved_group_id_invalid"
  # everyone and localaccounts are universal directory principals, not
  # supplementary process credentials. The raw kernel-group check separately
  # proves that neither numeric GID was copied into the service credential.
  case "$name" in
    scratchbird|everyone|localaccounts) continue ;;
  esac
  [[ "$gid" != "$primary_gid" ]] || continue
  sudo install -o root -g "$gid" -m 0060 /dev/null \
    "$canary_root/host-computed-group-$gid.permission-probe"
  canary_count=$((canary_count + 1))
done
[[ "$canary_count" -gt 0 ]] || fail "authority_canary_inventory_empty"
{
  echo "resolved_group_ids=${resolved_group_ids[*]}"
  echo "resolved_group_names=${resolved_group_names[*]}"
  echo "host_computed_authority_canary_count=$canary_count"
} > "$proof_root/launchd-host-computed-group-inventory.txt"

sudo install -o root -g scratchbird -m 0550 "$probe_source" "$runtime_probe"
sudo install -o root -g wheel -m 0644 "$proof_plist" "$runtime_plist"
sudo launchctl bootstrap system "$runtime_plist"

for _ in {1..300}; do
  if sudo grep -Fxq "launchd_service_process_credential=passed" \
    "$runtime_stdout" 2>/dev/null; then
    break
  fi
  if sudo test -s "$runtime_stderr"; then
    break
  fi
  sleep 0.1
done

sudo launchctl print "system/$label" > "$proof_root/launchd-credential-probe-state.txt"
sudo test -f "$runtime_stdout" || fail "stdout_missing"
sudo cat "$runtime_stdout" > "$proof_root/launchd-service-credential.txt"
if sudo test -f "$runtime_stderr"; then
  sudo cat "$runtime_stderr" > "$proof_root/launchd-service-credential.err.txt"
fi
sudo test ! -s "$runtime_stderr" || fail "probe_stderr_not_empty"

grep -Fx "launchd_service_process_credential=passed" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || fail "credential_rejected"
grep -Fx "effective_user=scratchbird" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || fail "user_mismatch"
grep -Fx "effective_group=scratchbird" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || fail "group_mismatch"
grep -Fx "supplementary_group_policy=exact_scratchbird_only" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "supplementary_group_mismatch"
grep -Fx "group_access_list_count=1" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "group_access_list_count"
grep -Fx "group_access_list_policy=exactly_one_effective_gid" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "group_access_list_policy"
grep -Fx "additional_supplementary_group_count=0" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "additional_supplementary_group_count"
grep -Fx "host_computed_authority_canaries=refused" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "host_computed_authority_canary"
grep -Fx "host_computed_authority_canary_count=$canary_count" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "host_computed_authority_canary_count"
grep -Fx "bootstrap_authority_regain=refused" \
  "$proof_root/launchd-service-credential.txt" >/dev/null || \
  fail "authority_regain"

for service_label in com.scratchbird.sbsrv com.scratchbird.sbmgr; do
  if sudo launchctl print "system/$service_label" >/dev/null 2>&1; then
    fail "scratchbird_service_loaded:$service_label"
  fi
done

echo "smoke_macos_launchd_credential=passed"
