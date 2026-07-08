#!/usr/bin/env sh
# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# PUBLIC_PLATFORM_ENV_VERIFY_MACOS

set -eu

repo_root="${1:-$(pwd)}"
build_root="${2:-build/public-release-macos}"
csv_output="${3:-${build_root}/public-platform-env-verify.csv}"
evidence_output="${4:-${build_root}/public-platform-env-verify.json}"

python3 "${repo_root}/project/tools/release/public_platform_environment_verify.py" \
  --repo-root "${repo_root}" \
  --build-root "${build_root}" \
  --platform macos \
  --csv-output "${csv_output}" \
  --evidence-output "${evidence_output}"
