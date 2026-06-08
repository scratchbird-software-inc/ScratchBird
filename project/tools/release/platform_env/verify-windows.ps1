# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# PUBLIC_PLATFORM_ENV_VERIFY_WINDOWS
param(
  [Parameter(ValueFromRemainingArguments=$true)]
  [string[]] $RemainingArgs
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
python (Join-Path $ScriptDir "../public_platform_environment_verify.py") --platform windows @RemainingArgs
