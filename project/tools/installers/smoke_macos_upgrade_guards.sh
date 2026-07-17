#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

fail() {
  echo "smoke_macos_upgrade_guards=fail:$1" >&2
  exit 1
}

phase="${1:-}"
package="${2:-}"
proof_root="${3:-}"
[[ "$phase" == preinstall || "$phase" == postinstall ]] || \
  fail "usage:<preinstall|postinstall> <package.pkg> <proof-root>"
[[ -f "$package" && "$package" == *.pkg ]] || fail "package_missing_or_invalid"
[[ -n "$proof_root" ]] || fail "proof_root_missing"
mkdir -p "$proof_root"
proof_root="$(cd "$proof_root" && pwd -P)"
[[ -x /bin/launchctl ]] || fail "launchctl_missing"
/bin/launchctl print system >/dev/null 2>&1 || fail "launchctl_system_domain_unavailable"

runtime_plist=/Library/LaunchDaemons/com.scratchbird.upgrade-guard-test.plist
loaded_label=""
helper_victim=""

cleanup_dummy_job() {
  local cleanup_status=0
  set +e
  if [[ -n "$loaded_label" ]]; then
    sudo /bin/launchctl bootout "system/$loaded_label" >/dev/null 2>&1 || \
      cleanup_status=1
  fi
  sudo rm -f "$runtime_plist" || cleanup_status=1
  if [[ -n "$helper_victim" ]]; then
    sudo rm -f "$helper_victim" || cleanup_status=1
    helper_victim=""
  fi
  loaded_label=""
  set -e
  return "$cleanup_status"
}

cleanup_on_exit() {
  local primary_status=$?
  trap - EXIT
  cleanup_dummy_job || true
  exit "$primary_status"
}
trap cleanup_on_exit EXIT

bootstrap_dummy_job() {
  local label="$1"
  local draft="$proof_root/${label}.upgrade-guard.plist"
  cat > "$draft" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>$label</string>
  <key>ProgramArguments</key>
  <array><string>/usr/bin/true</string></array>
  <key>RunAtLoad</key>
  <false/>
  <key>KeepAlive</key>
  <false/>
</dict>
</plist>
PLIST
  plutil -lint "$draft" >/dev/null
  sudo install -o root -g wheel -m 0644 "$draft" "$runtime_plist"
  sudo /bin/launchctl bootstrap system "$runtime_plist"
  loaded_label="$label"
  sudo /bin/launchctl print "system/$label" >/dev/null
}

require_label_absent() {
  local label="$1"
  local status
  if /bin/launchctl print "system/$label" >/dev/null 2>&1; then
    fail "label_remained_loaded:$label"
  else
    status=$?
    [[ "$status" -eq 113 ]] || fail "label_absence_status:$label:$status"
  fi
}

if [[ "$phase" == preinstall ]]; then
  for label in com.scratchbird.sbsrv com.scratchbird.sbmgr; do
    bootstrap_dummy_job "$label"
    log="$proof_root/${label}.preinstall-refusal.txt"
    set +e
    sudo installer -pkg "$package" -target / > "$log" 2>&1
    status=$?
    set -e
    [[ "$status" -ne 0 ]] || fail "loaded_job_package_install_accepted:$label"
    [[ ! -e /opt/ScratchBird && ! -L /opt/ScratchBird ]] || \
      fail "preinstall_refusal_replaced_payload:$label"
    [[ ! -e "/Library/Application Support/ScratchBird" ]] || \
      fail "preinstall_refusal_created_config:$label"
    ! pkgutil --pkg-info com.scratchbird.cde >/dev/null 2>&1 || \
      fail "preinstall_refusal_created_receipt:$label"
    ! sudo dscl . -read /Users/scratchbird >/dev/null 2>&1 || \
      fail "preinstall_refusal_created_user:$label"
    ! sudo dscl . -read /Groups/scratchbird >/dev/null 2>&1 || \
      fail "preinstall_refusal_created_group:$label"
    cleanup_dummy_job || fail "dummy_cleanup_failed:$label"
    require_label_absent "$label"
  done

  topology_victim="$proof_root/preinstall-topology-victim.txt"
  printf '%s\n' scratchbird-preinstall-topology-victim > "$topology_victim"
  victim_hash="$(shasum -a 256 "$topology_victim" | awk '{print $1}')"

  sudo ln -s "$topology_victim" /opt/ScratchBird
  set +e
  sudo installer -pkg "$package" -target / \
    > "$proof_root/preinstall-symlink-refusal.txt" 2>&1
  status=$?
  set -e
  [[ "$status" -ne 0 ]] || fail "unsafe_existing_symlink_accepted"
  sudo test -L /opt/ScratchBird || fail "unsafe_symlink_replaced_before_refusal"
  [[ "$(shasum -a 256 "$topology_victim" | awk '{print $1}')" == \
    "$victim_hash" ]] || fail "unsafe_symlink_victim_changed"
  sudo rm -f /opt/ScratchBird

  helper_victim_draft="$proof_root/preinstall-helper-hardlink-victim"
  printf '%s\n' scratchbird-preinstall-helper-hardlink-victim \
    > "$helper_victim_draft"
  helper_victim="/opt/.scratchbird-upgrade-guard-helper-victim-$$"
  sudo install -o root -g wheel -m 0755 "$helper_victim_draft" "$helper_victim"
  sudo install -d -o root -g wheel -m 0755 \
    /opt/ScratchBird /opt/ScratchBird/libexec
  sudo ln "$helper_victim" \
    /opt/ScratchBird/libexec/scratchbird-macos-system-install
  helper_victim_hash="$(sudo shasum -a 256 "$helper_victim" | awk '{print $1}')"
  set +e
  sudo installer -pkg "$package" -target / \
    > "$proof_root/preinstall-hardlink-refusal.txt" 2>&1
  status=$?
  set -e
  [[ "$status" -ne 0 ]] || fail "unsafe_existing_hardlink_accepted"
  [[ "$(sudo stat -f '%l' "$helper_victim")" -eq 2 ]] || \
    fail "unsafe_hardlink_replaced_before_refusal"
  [[ "$(sudo shasum -a 256 "$helper_victim" | awk '{print $1}')" == \
    "$helper_victim_hash" ]] || fail "unsafe_hardlink_victim_changed"
  sudo rm -rf /opt/ScratchBird
  sudo rm -f "$helper_victim"
  helper_victim=""

  sudo ln -s "$topology_victim" \
    /Library/LaunchDaemons/com.scratchbird.sbsrv.plist
  set +e
  sudo installer -pkg "$package" -target / \
    > "$proof_root/preinstall-plist-symlink-refusal.txt" 2>&1
  status=$?
  set -e
  [[ "$status" -ne 0 ]] || fail "unsafe_existing_plist_symlink_accepted"
  sudo test -L /Library/LaunchDaemons/com.scratchbird.sbsrv.plist || \
    fail "unsafe_plist_symlink_replaced_before_refusal"
  [[ "$(shasum -a 256 "$topology_victim" | awk '{print $1}')" == \
    "$victim_hash" ]] || fail "unsafe_plist_symlink_victim_changed"
  sudo rm -f /Library/LaunchDaemons/com.scratchbird.sbsrv.plist

  [[ ! -e /opt/ScratchBird && ! -L /opt/ScratchBird ]] || \
    fail "topology_refusal_cleanup_failed"
  [[ ! -e /Library/LaunchDaemons/com.scratchbird.sbsrv.plist && \
     ! -L /Library/LaunchDaemons/com.scratchbird.sbsrv.plist ]] || \
    fail "plist_topology_refusal_cleanup_failed"
  ! pkgutil --pkg-info com.scratchbird.cde >/dev/null 2>&1 || \
    fail "topology_refusal_created_receipt"
  ! sudo dscl . -read /Users/scratchbird >/dev/null 2>&1 || \
    fail "topology_refusal_created_user"
  ! sudo dscl . -read /Groups/scratchbird >/dev/null 2>&1 || \
    fail "topology_refusal_created_group"

  {
    echo "loaded_sbsrv_preinstall_refusal=passed"
    echo "loaded_sbmgr_preinstall_refusal=passed"
    echo "payload_replacement_before_preinstall=not_performed"
    echo "identity_creation_before_preinstall=not_performed"
    echo "unsafe_existing_symlink_refusal=passed"
    echo "unsafe_existing_hardlink_refusal=passed"
    echo "unsafe_existing_plist_symlink_refusal=passed"
  } > "$proof_root/macos-preinstall-loaded-job-guard.txt"
  echo "smoke_macos_upgrade_guards=passed:preinstall"
  exit 0
fi

helper=/opt/ScratchBird/libexec/scratchbird-macos-system-install
config_root="/Library/Application Support/ScratchBird"
state=/var/lib/scratchbird/install/MACOS_SYSTEM_INSTALL_STATE.json
[[ -x "$helper" && -f "$config_root/SBsrv.conf" && -f "$state" ]] || \
  fail "installed_system_surface_missing"

bootstrap_dummy_job com.scratchbird.sbsrv
config_before="$(sudo shasum -a 256 "$config_root/SBsrv.conf" | awk '{print $1}')"
state_before="$(sudo shasum -a 256 "$state" | awk '{print $1}')"
set +e
helper_output="$(sudo "$helper" post-install --identity-mode system --root / \
  --package-format pkg --package-version upgrade-guard 2>&1)"
helper_status=$?
set -e
[[ "$helper_status" -ne 0 ]] || fail "loaded_job_helper_recheck_accepted"
[[ "$helper_output" == BOOTSTRAP.SERVICE_STATE_INVALID ]] || \
  fail "loaded_job_helper_recheck_not_code_only"
config_after="$(sudo shasum -a 256 "$config_root/SBsrv.conf" | awk '{print $1}')"
state_after="$(sudo shasum -a 256 "$state" | awk '{print $1}')"
[[ "$config_after" == "$config_before" && "$state_after" == "$state_before" ]] || \
  fail "loaded_job_helper_recheck_mutated_state"
cleanup_dummy_job || fail "dummy_cleanup_failed:postinstall"
require_label_absent com.scratchbird.sbsrv

python_bin="$(command -v python3)"
[[ "$python_bin" = /* && -x "$python_bin" ]] || fail "python3_missing"
sudo "$python_bin" - "$config_root" <<'PY'
from pathlib import Path
import sys

root = Path(sys.argv[1])
replacements = {
    "SBsrv.conf": (
        "log_file = /var/log/scratchbird/runtime/SBsrv.log",
        "log_file = /var/log/scratchbird/SBsrv.log",
    ),
    "SBmgr.conf": (
        "manager.log.path = /var/log/scratchbird/runtime/SBmgr.log",
        "manager.log.path = /var/log/scratchbird/SBmgr.log",
    ),
}
for name, (current, legacy) in replacements.items():
    path = root / name
    text = path.read_text(encoding="utf-8")
    if text.count(current) != 1 or legacy in text:
        raise SystemExit(f"legacy_seed_topology_invalid:{name}")
    path.write_text(text.replace(current, legacy, 1), encoding="utf-8")
server = root / "SBsrv.conf"
with server.open("a", encoding="utf-8") as stream:
    stream.write("upgrade_guard_operator_marker = preserved\n")
PY
sudo "$python_bin" - <<'PY'
from pathlib import Path

path = Path("/var/lib/scratchbird/data/upgrade-guard-operator.dat")
path.write_text("operator-data-preserved\n", encoding="utf-8")
PY
sudo chown scratchbird:scratchbird \
  /var/lib/scratchbird/data/upgrade-guard-operator.dat
sudo chmod 0640 /var/lib/scratchbird/data/upgrade-guard-operator.dat

sudo installer -pkg "$package" -target / \
  > "$proof_root/system-installer-legacy-log-upgrade.txt" 2>&1
sudo grep -Fx 'log_file = /var/log/scratchbird/runtime/SBsrv.log' \
  "$config_root/SBsrv.conf" >/dev/null || fail "server_log_default_not_migrated"
sudo grep -Fx 'manager.log.path = /var/log/scratchbird/runtime/SBmgr.log' \
  "$config_root/SBmgr.conf" >/dev/null || fail "manager_log_default_not_migrated"
sudo grep -Fx 'upgrade_guard_operator_marker = preserved' \
  "$config_root/SBsrv.conf" >/dev/null || fail "operator_config_not_preserved"
[[ "$(sudo cat /var/lib/scratchbird/data/upgrade-guard-operator.dat)" == \
  operator-data-preserved ]] || fail "operator_data_not_preserved"
sudo "$python_bin" - "$state" <<'PY'
import json
import sys

state = json.load(open(sys.argv[1], encoding="utf-8"))
if state.get("legacy_packaged_log_default_migrations") != 2:
    raise SystemExit("legacy_log_migration_count_invalid")
if state.get("legacy_packaged_log_default_migration_policy") != (
    "exact_prior_packaged_line_only_preserve_all_other_configuration_lines"
):
    raise SystemExit("legacy_log_migration_policy_invalid")
PY
require_label_absent com.scratchbird.sbsrv
require_label_absent com.scratchbird.sbmgr

{
  echo "loaded_job_postinstall_recheck=passed"
  echo "loaded_job_recheck_mutation=not_performed"
  echo "legacy_packaged_log_default_migrations=2"
  echo "operator_config_preservation=passed"
  echo "operator_data_preservation=passed"
} > "$proof_root/macos-postinstall-upgrade-guard.txt"
echo "smoke_macos_upgrade_guards=passed:postinstall"
