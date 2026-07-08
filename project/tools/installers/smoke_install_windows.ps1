# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

param(
  [Parameter(Mandatory = $true)]
  [string] $PackagePath,
  [Parameter(Mandatory = $true)]
  [string] $WorkRoot
)

$ErrorActionPreference = "Stop"
if (Test-Path $WorkRoot) {
  Remove-Item -Recurse -Force $WorkRoot
}
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

if ($PackagePath.EndsWith(".msi", [System.StringComparison]::OrdinalIgnoreCase)) {
  $target = Join-Path $WorkRoot "msi"
  New-Item -ItemType Directory -Force -Path $target | Out-Null
  $process = Start-Process -FilePath "msiexec.exe" -ArgumentList @("/a", $PackagePath, "/qn", "TARGETDIR=$target") -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    throw "msiexec administrative extraction failed: $($process.ExitCode)"
  }
} else {
  Expand-Archive -Path $PackagePath -DestinationPath $WorkRoot -Force
}

$manifest = Get-ChildItem -Path $WorkRoot -Recurse -Filter "INSTALL_MANIFEST.json" | Select-Object -First 1
if (-not $manifest) {
  throw "missing INSTALL_MANIFEST.json"
}

$binDir = Get-ChildItem -Path $WorkRoot -Recurse -Directory | Where-Object { $_.FullName -match "ScratchBird.*bin$" } | Select-Object -First 1
if (-not $binDir) {
  throw "missing ScratchBird bin directory"
}

Write-Output "smoke_install_windows=passed:$WorkRoot"
