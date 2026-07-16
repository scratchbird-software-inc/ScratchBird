#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

fail() {
  echo "smoke_install_macos=fail:$1" >&2
  exit 1
}

package="${1:-}"
work_root="${2:-}"

if [[ -z "$package" || -z "$work_root" ]]; then
  fail "usage:smoke_install_macos.sh <package.tar.gz|package.pkg> <work-root>"
fi
if [[ ! -f "$package" ]]; then
  fail "package_not_found:$package"
fi

rm -rf "$work_root"
mkdir -p "$work_root"

system_install="${SB_MACOS_SMOKE_INSTALL_SYSTEM:-false}"
require_signed="${SB_MACOS_REQUIRE_SIGNED:-false}"
payload_root=""
package_kind=""
lifecycle_log="$work_root/macos-lifecycle-smoke.txt"
pkg_scripts_root=""

case "$package" in
  *.tar.gz)
    package_kind="portable"
    payload_root="$work_root/extract"
    mkdir -p "$payload_root"
    tar -xzf "$package" -C "$payload_root"
    ;;
  *.pkg)
    package_kind="system-pkg"
    if [[ "$(uname -s)" != "Darwin" ]]; then
      fail "pkg_smoke_requires_darwin"
    fi
    command -v pkgutil >/dev/null 2>&1 || fail "pkgutil_not_found"
    pkgutil --expand "$package" "$work_root/pkg-expanded"
    payload_file="$(find "$work_root/pkg-expanded" -name Payload -type f | head -n 1)"
    [[ -n "$payload_file" ]] || fail "pkg_payload_missing"
    payload_root="$work_root/pkg-payload"
    mkdir -p "$payload_root"
    if command -v ditto >/dev/null 2>&1; then
      ditto -x "$payload_file" "$payload_root"
    else
      command -v cpio >/dev/null 2>&1 || fail "cpio_not_found"
      if gzip -t "$payload_file" >/dev/null 2>&1; then
        gzip -dc "$payload_file" | (cd "$payload_root" && cpio -idm --quiet)
      else
        (cd "$payload_root" && cpio -idm --quiet) < "$payload_file"
      fi
    fi
    scripts_path="$(find "$work_root/pkg-expanded" -name Scripts \( -type d -o -type f \) | head -n 1)"
    [[ -n "$scripts_path" ]] || fail "pkg_scripts_missing"
    if [[ -d "$scripts_path" ]]; then
      pkg_scripts_root="$scripts_path"
    else
      pkg_scripts_root="$work_root/pkg-scripts"
      mkdir -p "$pkg_scripts_root"
      if command -v ditto >/dev/null 2>&1; then
        ditto -x "$scripts_path" "$pkg_scripts_root"
      else
        if gzip -t "$scripts_path" >/dev/null 2>&1; then
          gzip -dc "$scripts_path" | (cd "$pkg_scripts_root" && cpio -idm --quiet)
        else
          (cd "$pkg_scripts_root" && cpio -idm --quiet) < "$scripts_path"
        fi
      fi
    fi
    postinstall_script="$(find "$pkg_scripts_root" -name postinstall -type f | head -n 1)"
    [[ -n "$postinstall_script" ]] || fail "pkg_postinstall_missing"
    grep -F '/opt/ScratchBird/libexec/scratchbird-macos-system-install' "$postinstall_script" >/dev/null || fail "pkg_postinstall_helper_missing"
    ! grep -F '@SCRATCHBIRD_VERSION@' "$postinstall_script" >/dev/null || fail "pkg_postinstall_version_token_unresolved"
    if grep -E 'launchctl[[:space:]]+(load|bootstrap|enable|start)' "$postinstall_script" >/dev/null; then
      fail "pkg_postinstall_service_activation_forbidden"
    fi
    if [[ "$system_install" == "true" ]]; then
      sudo installer -pkg "$package" -target /
      payload_root="/"
    fi
    ;;
  *)
    fail "unsupported_package_suffix:$package"
    ;;
esac

if [[ "$payload_root" == "/" ]]; then
  runtime_root="/opt/ScratchBird"
  config_root="/Library/Application Support/ScratchBird"
  launchd_root="/Library/LaunchDaemons"
elif [[ "$package_kind" == "system-pkg" ]]; then
  runtime_root="$payload_root/opt/ScratchBird"
  config_root="$runtime_root/share/scratchbird/config-defaults"
  launchd_root="$payload_root/Library/LaunchDaemons"
else
  runtime_root="$payload_root/opt/ScratchBird"
  config_root="$payload_root/etc/scratchbird"
  launchd_root="$payload_root/Library/LaunchDaemons"
fi

[[ -d "$runtime_root/bin" ]] || fail "runtime_bin_missing"
[[ -d "$runtime_root/lib" ]] || fail "runtime_lib_missing"
[[ -d "$config_root" ]] || fail "config_root_missing"
[[ -d "$runtime_root/share/scratchbird/resources" ]] || fail "resources_missing"
[[ -f "$runtime_root/share/scratchbird/release/INSTALL_MANIFEST.json" ]] || fail "install_manifest_missing"
[[ -f "$runtime_root/share/scratchbird/release/SHA256SUMS" ]] || fail "sha256sums_missing"
[[ -f "$runtime_root/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json" ]] || fail "native_release_profile_missing"
[[ -f "$runtime_root/share/scratchbird/release/MACOS_SUPPORT_MATRIX.json" ]] || fail "macos_support_matrix_missing"

python3 "$(dirname "$0")/../release/verify_native_installed_payload.py" \
  "$payload_root" --config-root "$config_root"

if [[ "$package_kind" == "system-pkg" ]]; then
  [[ ! -e "$payload_root/etc/scratchbird" ]] || fail "pkg_duplicate_etc_config_root"
  [[ -f "$runtime_root/share/scratchbird/release/MACOS_SYSTEM_INSTALL_PROFILE.json" ]] || fail "macos_system_profile_missing"
  [[ -x "$runtime_root/libexec/scratchbird-macos-system-install" ]] || fail "macos_lifecycle_helper_missing"
fi

llvm_runtime_library=$(python3 -c '
import json, platform, sys
profile = json.load(open(sys.argv[1], encoding="utf-8"))
llvm = profile["llvm_runtime"]
value = llvm.get("runtime_library")
if value is None:
    architecture = "arm64" if platform.machine() == "arm64" else "x86_64"
    value = llvm["runtime_libraries_by_architecture"][architecture]
print(value)
' "$runtime_root/share/scratchbird/release/NATIVE_RELEASE_PROFILE.json")
python3 -c 'import ctypes,sys; ctypes.CDLL(sys.argv[1])' "$llvm_runtime_library"
printf 'llvm_runtime_library=%s\nllvm_runtime_load=passed\n' "$llvm_runtime_library" \
  > "$work_root/llvm-runtime-load.txt"

for binary in SBsql SBadm SBbak SBsec SBdoc SBcop SBsrv SBgate SBmgr SBParser; do
  executable="$runtime_root/bin/$binary"
  set +e
  "$executable" --help > "$work_root/$binary.help.txt" 2>&1
  status=$?
  set -e
  if [[ "$status" -gt 2 || ! -s "$work_root/$binary.help.txt" ]]; then
    fail "native_binary_launch_failed:$binary:$status"
  fi
done

if [[ "$package_kind" == "system-pkg" ]]; then
  [[ -f "$runtime_root/share/scratchbird/release/MACOS_LAUNCHD_MANIFEST.json" ]] || fail "macos_launchd_manifest_missing"
  [[ -d "$launchd_root" ]] || fail "launchd_root_missing"
  plist_count="$(find "$launchd_root" -maxdepth 1 -name 'com.scratchbird.*.plist' -type f | wc -l | tr -d ' ')"
  [[ "$plist_count" -eq 2 ]] || fail "launchd_top_level_service_set_mismatch"
  [[ -f "$launchd_root/com.scratchbird.sbsrv.plist" ]] || fail "sbsrv_launchd_plist_missing"
  [[ -f "$launchd_root/com.scratchbird.sbmgr.plist" ]] || fail "sbmgr_launchd_plist_missing"
  [[ ! -e "$launchd_root/com.scratchbird.sbgate.plist" ]] || fail "sbgate_must_be_server_managed"
  [[ ! -e "$launchd_root/com.scratchbird.sbparser.plist" ]] || fail "sbparser_must_be_listener_managed"
  python3 - "$launchd_root" <<'PY'
import pathlib
import plistlib
import sys

root = pathlib.Path(sys.argv[1])
expected = {
    "com.scratchbird.sbsrv": "/Library/Application Support/ScratchBird/SBsrv.conf",
    "com.scratchbird.sbmgr": "/Library/Application Support/ScratchBird/SBmgr.conf",
}
for label, config in expected.items():
    payload = plistlib.loads((root / f"{label}.plist").read_bytes())
    if payload.get("UserName") != "scratchbird":
        raise SystemExit(f"launchd_user_mismatch:{label}")
    if payload.get("GroupName") != "scratchbird":
        raise SystemExit(f"launchd_group_mismatch:{label}")
    if payload.get("Disabled") is not True:
        raise SystemExit(f"launchd_not_disabled:{label}")
    if payload.get("RunAtLoad") is not False or payload.get("KeepAlive") is not False:
        raise SystemExit(f"launchd_default_activity_mismatch:{label}")
    arguments = payload.get("ProgramArguments", [])
    if "--config" not in arguments or config not in arguments:
        raise SystemExit(f"launchd_config_mismatch:{label}")
PY
  if command -v plutil >/dev/null 2>&1; then
    while IFS= read -r plist; do
      plutil -lint "$plist" >/dev/null
    done < <(find "$launchd_root" -maxdepth 1 -name 'com.scratchbird.*.plist' -type f | sort)
  fi
else
  [[ ! -e "$runtime_root/share/scratchbird/release/MACOS_LAUNCHD_MANIFEST.json" ]] || fail "portable_launchd_manifest_forbidden"
  [[ ! -e "$launchd_root" ]] || fail "portable_launchd_root_forbidden"
fi

if command -v otool >/dev/null 2>&1; then
  while IFS= read -r binary; do
    otool -L "$binary" >/dev/null
  done < <(find "$runtime_root/bin" "$runtime_root/lib" -type f \( -perm -111 -o -name '*.dylib' -o -name '*.so' \) | sort)
fi

if [[ "$(uname -s)" == "Darwin" && "$package" == *.pkg ]]; then
  if command -v pkgutil >/dev/null 2>&1; then
    if ! pkgutil --check-signature "$package" >"$work_root/pkg-signature.txt" 2>&1; then
      [[ "$require_signed" == "false" ]] || fail "pkg_signature_required"
    fi
  fi
  if command -v spctl >/dev/null 2>&1; then
    if ! spctl --assess --type install "$package" >"$work_root/spctl.txt" 2>&1; then
      [[ "$require_signed" == "false" ]] || fail "spctl_assess_required"
    fi
  fi
fi

if [[ "$payload_root" != "/" && "$package_kind" == "system-pkg" ]]; then
  lifecycle_root="$work_root/lifecycle"
  fresh_root="$lifecycle_root/fresh"
  upgraded_root="$lifecycle_root/upgraded"
  mkdir -p "$fresh_root" "$upgraded_root"
  cp -R "$payload_root"/. "$fresh_root"/
  helper="$fresh_root/opt/ScratchBird/libexec/scratchbird-macos-system-install"
  "$helper" post-install \
    --root "$fresh_root" \
    --identity-mode fixture \
    --package-format fixture \
    --package-version 0.0.0-smoke
  live_config="$fresh_root/Library/Application Support/ScratchBird"
  [[ -d "$live_config" ]] || fail "lifecycle_canonical_config_missing"
  [[ ! -e "$fresh_root/etc/scratchbird" ]] || fail "lifecycle_duplicate_etc_config_root"
  printf 'preserve=yes\n' > "$live_config/local-preserve.conf"
  cp -R "$fresh_root"/. "$upgraded_root"/
  upgraded_helper="$upgraded_root/opt/ScratchBird/libexec/scratchbird-macos-system-install"
  "$upgraded_helper" post-install \
    --root "$upgraded_root" \
    --identity-mode fixture \
    --package-format fixture \
    --package-version 0.0.0-smoke
  upgraded_config="$upgraded_root/Library/Application Support/ScratchBird"
  [[ -f "$upgraded_config/local-preserve.conf" ]] || fail "lifecycle_upgrade_config_not_preserved"
  "$upgraded_helper" pre-remove \
    --root "$upgraded_root" \
    --identity-mode fixture \
    --package-format fixture \
    --package-version 0.0.0-smoke
  [[ -f "$upgraded_root/var/lib/scratchbird/install/config-preserve/local-preserve.conf" ]] || fail "lifecycle_pre_remove_config_not_preserved"
  if find "$upgraded_root/var/lib/scratchbird" -type f \
      \( -name '*.sbdb' -o -name '*.sb.security_principal_events' \
         -o -name '*.sb.local_password_auth' \) -print -quit | grep -q .; then
    fail "lifecycle_installer_created_database_or_security_sidecar"
  fi
  rm -rf "$upgraded_root/opt/ScratchBird"
  [[ ! -e "$upgraded_root/opt/ScratchBird" ]] || fail "lifecycle_uninstall_runtime_removal_failed"
  [[ -f "$upgraded_config/local-preserve.conf" ]] || fail "lifecycle_uninstall_config_not_preserved"
  {
    echo "fresh_install=passed"
    echo "canonical_config_root=passed"
    echo "upgrade_idempotence=passed"
    echo "config_preservation=passed"
    echo "pre_remove_preservation=passed"
    echo "uninstall_runtime_removal=passed"
    echo "uninstall_config_preservation=passed"
    echo "database_creation=not_performed"
  } > "$lifecycle_log"
elif [[ "$payload_root" != "/" ]]; then
  lifecycle_root="$work_root/lifecycle"
  fresh_root="$lifecycle_root/fresh"
  upgraded_root="$lifecycle_root/upgraded"
  mkdir -p "$fresh_root" "$upgraded_root"
  cp -R "$payload_root"/. "$fresh_root"/
  [[ -d "$fresh_root/opt/ScratchBird/bin" ]] || fail "lifecycle_fresh_bin_missing"
  mkdir -p "$fresh_root/etc/scratchbird"
  printf 'preserve=yes\n' > "$fresh_root/etc/scratchbird/local-preserve.conf"
  cp -R "$fresh_root"/. "$upgraded_root"/
  cp -R "$payload_root"/. "$upgraded_root"/
  [[ -f "$upgraded_root/etc/scratchbird/local-preserve.conf" ]] || fail "lifecycle_upgrade_config_not_preserved"
  rm -rf "$upgraded_root/opt/ScratchBird"
  [[ ! -e "$upgraded_root/opt/ScratchBird" ]] || fail "lifecycle_uninstall_runtime_removal_failed"
  [[ -f "$upgraded_root/etc/scratchbird/local-preserve.conf" ]] || fail "lifecycle_uninstall_config_not_preserved"
  {
    echo "fresh_install=passed"
    echo "upgrade_overlay=passed"
    echo "config_preservation=passed"
    echo "uninstall_runtime_removal=passed"
    echo "uninstall_config_preservation=passed"
    echo "service_definition=absent"
    echo "execution_mode=foreground_only"
  } > "$lifecycle_log"
else
  if launchctl print system/com.scratchbird.sbsrv >/dev/null 2>&1; then
    fail "system_install_loaded_sbsrv"
  fi
  if launchctl print system/com.scratchbird.sbmgr >/dev/null 2>&1; then
    fail "system_install_loaded_sbmgr"
  fi
  {
    echo "system_install=passed"
    echo "service_loaded=false"
    echo "fresh_install=installer_target"
    echo "upgrade_overlay=not_run_in_system_mode"
    echo "uninstall_runtime_removal=not_run_in_system_mode"
    echo "uninstall_config_preservation=not_run_in_system_mode"
  } > "$lifecycle_log"
fi

echo "smoke_install_macos=passed:$work_root"
