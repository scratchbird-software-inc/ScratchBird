#!/usr/bin/env bash
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0
# Linux qualification: bash run_linux_checks.sh [repository] [optional Core corpus]
# Core is local specification authority, never a bundled public build dependency.
set -euo pipefail
task_probes=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
task_tests=$(cd -- "$task_probes/.." && pwd)
task_default_repo=$(cd -- "$task_tests/../../.." && pwd)
task_repo=${1:-$task_default_repo}
task_candidate=$task_repo
task_build=$(mktemp -d /tmp/scratchbird-blob-test.XXXXXX)
cleanup() {
  case "$task_build" in /tmp/scratchbird-blob-test.*) ;; *) return 1 ;; esac
  for variant in plain asan tsan; do
    rm -f -- "$task_build/runtime-$variant.o" "$task_build/test-$variant.o" \
      "$task_build/registry-$variant.o" "$task_build/test-$variant"
  done
  rm -f -- "$task_build/ledger.o" "$task_build/ledger"
  rm -f -- "$task_build/profile.o" "$task_build/profile" \
    "$task_build/abi-c" "$task_build/abi-cpp20"
  rmdir -- "$task_build"
}
trap cleanup EXIT
task_inc=(-I"$task_candidate/project/src/core/datatypes" -I"$task_repo/project/include"
  -I"$task_repo/project/src/core/datatypes" -I"$task_repo/project/src"
  -I"$task_repo/project/src/core" -I"$task_repo/project/src/core/platform")
task_strict=(-std=c++23 -g -Wall -Wextra -Wpedantic -Werror)
for variant in plain asan tsan; do
  task_flags=()
  task_link=()
  if [[ $variant == asan ]]; then
    task_flags=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  elif [[ $variant == tsan ]]; then
    task_flags=(-fsanitize=thread -fno-omit-frame-pointer -fno-pie)
    task_link=(-no-pie)
  fi
  g++ "${task_strict[@]}" "${task_flags[@]}" "${task_inc[@]}" \
    -c "$task_candidate/project/src/core/datatypes/datatype_blob.cpp" -o "$task_build/runtime-$variant.o"
  g++ "${task_strict[@]}" "${task_flags[@]}" "${task_inc[@]}" \
    -c "$task_tests/base_blob_lifetime_v8_runtime_test.cpp" -o "$task_build/test-$variant.o"
  # Preserve the frozen generated registry's existing warning policy. Modified
  # runtime and tests above are always compiled with warnings-as-errors.
  g++ -std=c++23 -g -w "${task_flags[@]}" "${task_inc[@]}" \
    -c "$task_repo/project/src/core/datatypes/datatype_type_codec_identity_v3.cpp" -o "$task_build/registry-$variant.o"
  g++ -std=c++23 -pthread "${task_flags[@]}" "${task_link[@]}" \
    "$task_build/runtime-$variant.o" "$task_build/test-$variant.o" \
    "$task_build/registry-$variant.o" -o "$task_build/test-$variant"
  if [[ $variant == asan ]]; then
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 timeout 50 "$task_build/test-$variant"
  elif [[ $variant == tsan ]]; then
    TSAN_OPTIONS=halt_on_error=1 timeout 50 setarch "$(uname -m)" -R "$task_build/test-$variant"
  else
    timeout 50 "$task_build/test-$variant"
  fi
  printf 'PASS standalone variant=%s\n' "$variant"
  if [[ $variant == plain ]]; then
    g++ "${task_strict[@]}" "${task_inc[@]}" -c \
      "$task_tests/base_blob_profile_value_test.cpp" \
      -o "$task_build/profile.o"
    g++ -std=c++23 -pthread "$task_build/profile.o" "$task_build/runtime-plain.o" \
      "$task_build/registry-plain.o" -o "$task_build/profile"
    timeout 50 "$task_build/profile"
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror "${task_inc[@]}" \
      "$task_repo/project/tests/sbsql_sblr_alignment/base_blob_lifetime_abi_c_test.c" \
      -o "$task_build/abi-c"
    g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror "${task_inc[@]}" \
      "$task_repo/project/tests/sbsql_sblr_alignment/base_blob_lifetime_abi_cpp20_test.cpp" \
      -o "$task_build/abi-cpp20"
    "$task_build/abi-c"
    "$task_build/abi-cpp20"
    printf 'PASS migrated profile and unchanged repository C17/C++20 ABI tests\n'
  fi
done
for pair in transfer_publication_gdb.py:transfers transfer_admission_gdb.py:active_second retain_publication_gdb.py:retain_publication; do
  PYTHONDONTWRITEBYTECODE=1 timeout 50 gdb -q -batch -x "$task_probes/${pair%%:*}" \
    --args "$task_build/test-plain" "${pair#*:}"
done
g++ "${task_strict[@]}" "${task_inc[@]}" -c "$task_probes/ledger_lifecycle_probe.cpp" -o "$task_build/ledger.o"
g++ -std=c++23 -pthread "$task_build/ledger.o" "$task_build/runtime-plain.o" \
  "$task_build/registry-plain.o" -o "$task_build/ledger"
timeout 50 "$task_build/ledger"
if [[ $# -ge 2 ]]; then
  PYTHONDONTWRITEBYTECODE=1 python3 "$task_probes/invariant_validator.py" "$2"
fi
printf 'PASS Linux lifetime qualification; not global datatype completion\n'
