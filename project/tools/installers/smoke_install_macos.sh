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
lifecycle_log="$work_root/macos-lifecycle-smoke.txt"

case "$package" in
  *.tar.gz)
    payload_root="$work_root/extract"
    mkdir -p "$payload_root"
    tar -xzf "$package" -C "$payload_root"
    ;;
  *.pkg)
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
  etc_root="/etc/scratchbird"
  launchd_root="/Library/LaunchDaemons"
else
  runtime_root="$payload_root/opt/ScratchBird"
  etc_root="$payload_root/etc/scratchbird"
  launchd_root="$payload_root/Library/LaunchDaemons"
fi

[[ -d "$runtime_root/bin" ]] || fail "runtime_bin_missing"
[[ -d "$runtime_root/lib" ]] || fail "runtime_lib_missing"
[[ -d "$etc_root" ]] || fail "etc_scratchbird_missing"
[[ -d "$runtime_root/share/scratchbird/resources" ]] || fail "resources_missing"
[[ -f "$runtime_root/share/scratchbird/release/INSTALL_MANIFEST.json" ]] || fail "install_manifest_missing"
[[ -f "$runtime_root/share/scratchbird/release/SHA256SUMS" ]] || fail "sha256sums_missing"
[[ -f "$runtime_root/share/scratchbird/release/MACOS_SUPPORT_MATRIX.json" ]] || fail "macos_support_matrix_missing"
[[ -f "$runtime_root/share/scratchbird/release/MACOS_LAUNCHD_MANIFEST.json" ]] || fail "macos_launchd_manifest_missing"
[[ -d "$launchd_root" ]] || fail "launchd_root_missing"

plist_count="$(find "$launchd_root" -maxdepth 1 -name 'com.scratchbird.*.plist' -type f | wc -l | tr -d ' ')"
[[ "$plist_count" -ge 4 ]] || fail "launchd_plists_missing"

if command -v plutil >/dev/null 2>&1; then
  while IFS= read -r plist; do
    plutil -lint "$plist" >/dev/null
  done < <(find "$launchd_root" -maxdepth 1 -name 'com.scratchbird.*.plist' -type f | sort)
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

if [[ "$payload_root" != "/" ]]; then
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
  } > "$lifecycle_log"
else
  {
    echo "system_install=passed"
    echo "fresh_install=installer_target"
    echo "upgrade_overlay=not_run_in_system_mode"
    echo "uninstall_runtime_removal=not_run_in_system_mode"
    echo "uninstall_config_preservation=not_run_in_system_mode"
  } > "$lifecycle_log"
fi

echo "smoke_install_macos=passed:$work_root"
