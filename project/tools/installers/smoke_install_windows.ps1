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
  [string] $WorkRoot,
  [string] $UpgradePackagePath,
  [switch] $AdministrativeExtractOnly
)

$ErrorActionPreference = "Stop"
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)

function Invoke-Msi {
  param(
    [Parameter(Mandatory = $true)] [string[]] $Arguments,
    [Parameter(Mandatory = $true)] [string] $Failure
  )
  $process = Start-Process -FilePath "msiexec.exe" -ArgumentList $Arguments -Wait -PassThru
  if ($process.ExitCode -notin @(0, 3010)) {
    throw "$($Failure): $($process.ExitCode)"
  }
}

function Invoke-MsiExpectedFailure {
  param(
    [Parameter(Mandatory = $true)] [string[]] $Arguments,
    [Parameter(Mandatory = $true)] [string] $Failure
  )
  $process = Start-Process -FilePath "msiexec.exe" -ArgumentList $Arguments -Wait -PassThru
  if ($process.ExitCode -ne 1603) {
    throw "$($Failure): expected MSI error 1603, got $($process.ExitCode)"
  }
  return $process.ExitCode
}

function Assert-MsiLogContainsTokens {
  param(
    [Parameter(Mandatory = $true)] [string] $Path,
    [Parameter(Mandatory = $true)] [string[]] $Tokens
  )
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "MSI log is missing: $Path"
  }
  $text = Get-Content -LiteralPath $Path -Raw
  foreach ($token in $Tokens) {
    if (-not $text.Contains($token)) {
      throw "MSI log does not prove expected transaction action: $token"
    }
  }
}

$InstallerTransactionRegistryPath = "HKLM:\SOFTWARE\ScratchBird\InstallerTransaction"

function Assert-InstallerTransactionJournalAbsent {
  if (Test-Path -LiteralPath $InstallerTransactionRegistryPath) {
    throw "ScratchBird installer transaction journal remains"
  }
}

function Assert-NoScratchBirdIdentityAndJournal {
  for ($attempt = 0; $attempt -lt 20; $attempt++) {
    $serviceCount = @(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'").Count
    $groupCount = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE").Count
    $journalExists = Test-Path -LiteralPath $InstallerTransactionRegistryPath
    if ($serviceCount -eq 0 -and $groupCount -eq 0 -and -not $journalExists) {
      break
    }
    Start-Sleep -Milliseconds 500
  }
  if (@(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'").Count -ne 0) {
    throw "scratchbird service remains after failed fresh-install rollback"
  }
  if (@(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE").Count -ne 0) {
    throw "ScratchBird group remains after failed fresh-install rollback"
  }
  if (@(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE").Count -ne 0) {
    throw "scratchbird local SAM user exists after failed fresh-install rollback"
  }
  Assert-InstallerTransactionJournalAbsent
}

function New-ShortAdministrativeExtractRoot {
  $driveRoot = [IO.Path]::GetPathRoot($env:SystemRoot)
  if ([string]::IsNullOrWhiteSpace($driveRoot)) {
    throw "Windows system drive root is unavailable"
  }
  $suffix = [Guid]::NewGuid().ToString("N").Substring(0, 8)
  $path = Join-Path $driveRoot ("sbm-{0}-{1}" -f $PID, $suffix)
  if ($path.Length -gt 48) {
    throw "administrative extraction root exceeds path-length budget"
  }
  if (Test-Path -LiteralPath $path) {
    throw "administrative extraction root collision"
  }
  New-Item -ItemType Directory -Path $path | Out-Null
  return [IO.Path]::GetFullPath($path)
}

function Get-MsiProperty {
  param([string] $Path, [string] $Name)
  $installer = New-Object -ComObject WindowsInstaller.Installer
  $database = $installer.GetType().InvokeMember(
    "OpenDatabase",
    "InvokeMethod",
    $null,
    $installer,
    @($Path, 0)
  )
  $view = $database.GetType().InvokeMember(
    "OpenView",
    "InvokeMethod",
    $null,
    $database,
    @("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$Name'")
  )
  [void]$view.GetType().InvokeMember("Execute", "InvokeMethod", $null, $view, $null)
  $record = $view.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $view, $null)
  if ($null -eq $record) {
    throw "missing MSI property: $Name"
  }
  return $record.GetType().InvokeMember("StringData", "GetProperty", $null, $record, 1)
}

function Get-ExactScratchBirdGroup {
  $rows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
  if ($rows.Count -ne 1 -or [int]$rows[0].SIDType -ne 4 -or -not [string]::Equals($rows[0].Domain, $env:COMPUTERNAME, [StringComparison]::OrdinalIgnoreCase)) {
    throw "ScratchBird local SAM alias validation failed"
  }
  $group = [ADSI]("WinNT://{0}/{1},group" -f $rows[0].Domain, $rows[0].Name)
  $members = @($group.PSBase.Invoke("Members") | Where-Object { $null -ne $_ })
  if ($members.Count -ne 0) {
    throw "ScratchBird filesystem-operations group must have no members"
  }
  if (@(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE").Count -ne 0) {
    throw "ScratchBird must not create a local SAM service user"
  }
  return $rows[0]
}

function Get-ScratchBirdServiceSecuritySddl {
  $sc = Join-Path $env:SystemRoot "System32\sc.exe"
  if (-not (Test-Path -LiteralPath $sc -PathType Leaf)) {
    throw "System32 sc.exe is unavailable"
  }
  $output = @(& $sc "sdshow" "scratchbird" 2>$null)
  if ([int]$LASTEXITCODE -ne 0) {
    throw "scratchbird service security descriptor query failed"
  }
  $sddlRows = @($output | ForEach-Object {
    ([string]$_).Trim()
  } | Where-Object {
    $_ -match '^[OGDS]:'
  })
  if ($sddlRows.Count -ne 1) {
    throw "scratchbird service security descriptor is invalid"
  }
  return [string]$sddlRows[0]
}

function ConvertTo-NormalizedScratchBirdSddl {
  param([string] $Sddl)
  if ([string]::IsNullOrWhiteSpace($Sddl)) {
    throw "ScratchBird security descriptor is empty"
  }
  $descriptor = [Security.AccessControl.RawSecurityDescriptor]::new($Sddl)
  return $descriptor.GetSddlForm(
    [Security.AccessControl.AccessControlSections]::All
  )
}

function Get-ScratchBirdIdentitySnapshot {
  $group = Get-ExactScratchBirdGroup
  $services = @(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'")
  if ($services.Count -ne 1) {
    throw "exactly one scratchbird service is required for identity snapshot"
  }
  $service = $services[0]
  $serviceRegistryPath = "HKLM:\SYSTEM\CurrentControlSet\Services\scratchbird"
  $serviceSidType = (Get-ItemProperty -LiteralPath $serviceRegistryPath -Name "ServiceSidType").ServiceSidType
  $serviceSid = [Security.Principal.NTAccount]::new(
    "NT SERVICE\scratchbird"
  ).Translate([Security.Principal.SecurityIdentifier]).Value
  $serviceRegistryAcl = Get-Acl -LiteralPath $serviceRegistryPath
  $serviceRegistryKey = Get-Item -LiteralPath $serviceRegistryPath -Force
  $delayedAutoStartPresent = @(
    $serviceRegistryKey.GetValueNames()
  ) -contains "DelayedAutoStart"
  $delayedAutoStart = if ($delayedAutoStartPresent) {
    [int]$serviceRegistryKey.GetValue("DelayedAutoStart")
  } else {
    $null
  }
  $serviceSecuritySddl = ConvertTo-NormalizedScratchBirdSddl (
    Get-ScratchBirdServiceSecuritySddl
  )
  $registrySddl = ConvertTo-NormalizedScratchBirdSddl (
    [string]$serviceRegistryAcl.Sddl
  )
  return [ordered]@{
    group = [ordered]@{
      name = [string]$group.Name
      domain = [string]$group.Domain
      sid = [string]$group.SID
      sid_type = [int]$group.SIDType
      description = [string]$group.Description
      member_count = 0
    }
    service = [ordered]@{
      name = [string]$service.Name
      display_name = [string]$service.DisplayName
      path_name = [string]$service.PathName
      start_name = [string]$service.StartName
      start_mode = [string]$service.StartMode
      state = [string]$service.State
      service_type = [string]$service.ServiceType
      error_control = [string]$service.ErrorControl
      description = [string]$serviceRegistryKey.GetValue("Description")
      delayed_auto_start_present = [bool]$delayedAutoStartPresent
      delayed_auto_start = $delayedAutoStart
      service_sid = $serviceSid
      service_sid_type = [int]$serviceSidType
      service_security_sddl = $serviceSecuritySddl
      registry_sddl = $registrySddl
    }
  }
}

function Assert-ScratchBirdIdentitySnapshot {
  param(
    [Parameter(Mandatory = $true)] $Expected
  )
  $actual = Get-ScratchBirdIdentitySnapshot
  $expectedJson = $Expected | ConvertTo-Json -Depth 8 -Compress
  $actualJson = $actual | ConvertTo-Json -Depth 8 -Compress
  if (-not [string]::Equals($actualJson, $expectedJson, [StringComparison]::Ordinal)) {
    throw "failed uninstall did not restore the snapshotted ScratchBird service identity/configuration/runtime-state fields and preserved group identity"
  }
}

function Assert-SidNotInAnyLocalSamGroup {
  param([string] $ForbiddenSid)
  $groups = @(Get-CimInstance -ClassName Win32_Group -Filter "LocalAccount=TRUE")
  if ($groups.Count -eq 0) {
    throw "local SAM group inventory is empty"
  }
  foreach ($groupRecord in $groups) {
    if (-not $groupRecord.LocalAccount -or [int]$groupRecord.SIDType -ne 4) {
      throw "local SAM group record is invalid"
    }
    $group = [ADSI]("WinNT://{0}/{1},group" -f $groupRecord.Domain, $groupRecord.Name)
    foreach ($member in @($group.PSBase.Invoke("Members"))) {
      $bytes = $member.GetType().InvokeMember("objectSid", [Reflection.BindingFlags]::GetProperty, $null, $member, $null)
      $memberSid = [Security.Principal.SecurityIdentifier]::new([byte[]]$bytes, 0).Value
      if ($memberSid -eq $ForbiddenSid) {
        throw "managed service SID has forbidden local SAM group membership: $($groupRecord.Name)"
      }
    }
  }
}

function Assert-ProtectedAcl {
  param([string] $Path, [string[]] $RequiredSids)
  $acl = Get-Acl -LiteralPath $Path
  if (-not $acl.AreAccessRulesProtected) {
    throw "ACL inheritance remains enabled: $Path"
  }
  $actual = @($acl.Access | ForEach-Object {
    $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
  })
  foreach ($sid in $RequiredSids) {
    if ($actual -notcontains $sid) {
      throw "required ACL SID is missing: $Path"
    }
  }
}

function Assert-AclContainsSid {
  param([string] $Path, [string] $RequiredSid)
  $actual = @((Get-Acl -LiteralPath $Path).Access | ForEach-Object {
    $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
  })
  if ($actual -notcontains $RequiredSid) {
    throw "required ACL SID is missing: $Path"
  }
}

function Assert-NoInstallerDatabaseArtifacts {
  param([string] $StateRoot)
  $forbidden = @(Get-ChildItem -LiteralPath $StateRoot -Recurse -File -Force | Where-Object {
    $_.Name -match '(?i)\.(sbdb|sbrd)$' -or $_.Name -match '(?i)\.sb\.(security_principal_events|local_password_auth)$'
  })
  if ($forbidden.Count -ne 0) {
    throw "installer created database or security sidecar artifacts"
  }
}

function Assert-InstalledWindowsSystem {
  param([switch] $RequireGroupCreatedByThisRun)
  $runtimeRoot = Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "ScratchBird"
  $stateRoot = Join-Path ([Environment]::GetFolderPath("CommonApplicationData")) "ScratchBird"
  foreach ($binary in @("SBsrv", "SBgate", "SBParser", "SBmgr")) {
    if (-not (Test-Path -LiteralPath (Join-Path $runtimeRoot "bin\$binary.exe") -PathType Leaf)) {
      throw "installed native component missing: $binary"
    }
  }

  $group = Get-ExactScratchBirdGroup

  $services = @(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'")
  if ($services.Count -ne 1) {
    throw "exactly one scratchbird service was not installed"
  }
  $service = $services[0]
  if (-not [string]::Equals($service.StartName, "NT SERVICE\scratchbird", [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($service.StartMode, "Manual", [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase)) {
    throw "scratchbird service identity or fresh-install state is invalid"
  }
  if ($service.PathName -notmatch '(?i)SBsrv\.exe.*--service' -or $service.PathName -notmatch '(?i)SBsrv\.conf') {
    throw "scratchbird service command does not own SBsrv"
  }
  $sidType = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\scratchbird" -Name "ServiceSidType").ServiceSidType
  if ([int]$sidType -ne 3) {
    throw "scratchbird service SID is not restricted"
  }
  foreach ($forbiddenService in @("SBgate", "SBParser", "SBmgr", "ScratchBirdSBgate", "ScratchBirdSBParser", "ScratchBirdSBmgr")) {
    if (@(Get-CimInstance -ClassName Win32_Service -Filter "Name='$forbiddenService'").Count -ne 0) {
      throw "parser/listener/manager service was installed: $forbiddenService"
    }
  }

  $serviceSid = [Security.Principal.NTAccount]::new("NT SERVICE\scratchbird").Translate([Security.Principal.SecurityIdentifier]).Value
  if ($serviceSid -notmatch '^S-1-5-80-(\d+-){4}\d+$') {
    throw "managed virtual account SID validation failed"
  }
  Assert-SidNotInAnyLocalSamGroup $serviceSid
  Assert-ProtectedAcl "HKLM:\SYSTEM\CurrentControlSet\Services\scratchbird" @("S-1-5-18", "S-1-5-32-544", $serviceSid)
  Assert-AclContainsSid $runtimeRoot $serviceSid
  foreach ($relative in @("config", "data", "log", "run", "run\control")) {
    Assert-ProtectedAcl (Join-Path $stateRoot $relative) @("S-1-5-18", "S-1-5-32-544", [string]$group.SID, $serviceSid)
  }
  foreach ($relative in @("tls", "secrets")) {
    Assert-ProtectedAcl (Join-Path $stateRoot $relative) @("S-1-5-18", "S-1-5-32-544", $serviceSid)
  }

  $evidencePath = Join-Path $stateRoot "install\WINDOWS_SYSTEM_INSTALL_STATE.json"
  if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
    throw "Windows system install evidence is missing"
  }
  $evidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
  if ($evidence.native_default_port -ne 3092 -or $evidence.service_name -ne "scratchbird" -or $evidence.service_account -ne "NT SERVICE\scratchbird" -or $evidence.service_authority_scope -ne "filesystem_directory_and_process_execution_only_no_database_or_security_authority" -or $evidence.service_local_sam_group_membership -ne $false -or $evidence.filesystem_operations_group_member_count -ne 0 -or $evidence.filesystem_operations_group_creation_policy -ne "absolute_System32_net.exe_localgroup_add_when_missing" -or $evidence.lifecycle_process_architecture -ne "64_bit" -or $evidence.human_service_group_membership_mutated -ne $false -or $evidence.create_time_os_authorization -ne "administrator_only" -or $evidence.filesystem_operations_group_sid -ne [string]$group.SID -or $evidence.topology -ne "client_to_optional_SBmgr_not_used_with_emulation_to_shared_SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_SBsrv_engine" -or $evidence.database_files_created -ne $false -or $evidence.security_sidecars_created -ne $false) {
    throw "Windows system install evidence is invalid"
  }
  if ($evidence.filesystem_operations_group_created_by_this_run -isnot [bool]) {
    throw "Windows group creation evidence is invalid"
  }
  if ($RequireGroupCreatedByThisRun -and -not $evidence.filesystem_operations_group_created_by_this_run) {
    throw "Fresh MSI install did not create the ScratchBird group"
  }
  Assert-InstallerTransactionJournalAbsent

  $configRoot = Join-Path $stateRoot "config"
  $runtimeValue = $runtimeRoot.Replace("\", "/")
  $stateValue = $stateRoot.Replace("\", "/")
  $expectedConfigTokens = @{
    "SBsrv.conf" = @(
      "data_dir = $stateValue/run/sb_server",
      "control_dir = $stateValue/run/sb_server/control",
      "log_file = $stateValue/log/SBsrv.log",
      "default_path = $stateValue/data/default.sbdb",
      "executable_path = $runtimeValue/bin/SBgate.exe",
      "parser_executable_path = $runtimeValue/bin/SBParser.exe",
      "control_dir = $stateValue/run/listener/control",
      "runtime_dir = $stateValue/run/listener/runtime",
      "sbps_endpoint = $stateValue/run/sb_server/control/sb_server.sbps.sock",
      "port = 3092"
    )
    "SBgate.conf" = @(
      "parser_executable = $runtimeValue/bin/SBParser.exe",
      "server_endpoint = $stateValue/run/sb_server/control/sb_server.sbps.sock",
      "control_dir = $stateValue/run/listener/control",
      "runtime_dir = $stateValue/run/listener/runtime",
      "port = 3092"
    )
    "SBmgr.conf" = @(
      "manager.runtime_dir = $stateValue/run/manager/runtime",
      "manager.control_dir = $stateValue/run/manager/control",
      "manager.log.path = $stateValue/log/SBmgr.log",
      "manager.owner.database_path = $stateValue/data/default.sbdb",
      "manager.proxy.port = 3092",
      "manager.backend.native_port = 0"
    )
    "SBParser.conf" = @(
      "parser.family = sbsql",
      "parser.worker_binary = $runtimeValue/bin/SBParser.exe",
      "parser.execution.engine_transport = sbps_ipc_only",
      "parser.execution.direct_engine_link = forbidden",
      "parser.execution.cross_parser_dependency = forbidden"
    )
    "SBbootstrap.profile" = @(
      "platform = windows",
      'service_identity = NT SERVICE\scratchbird',
      "service_group = ScratchBird"
    )
  }
  foreach ($configName in $expectedConfigTokens.Keys) {
    $configPath = Join-Path $stateRoot "config\$configName"
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
      throw "installed configuration missing: $configName"
    }
    $configText = Get-Content -LiteralPath $configPath -Raw
    foreach ($token in $expectedConfigTokens[$configName]) {
      if (-not $configText.Contains($token)) {
        throw "installed configuration token missing: $configName $token"
      }
    }
    if ($configText -match '(?i)@SCRATCHBIRD_|operator_required|compatibility|emulation|firebird|mysql|postgres') {
      throw "installed configuration contains a forbidden marker: $configName"
    }
  }
  python project/tools/release/verify_native_installed_payload.py `
    $runtimeRoot `
    --config-root $configRoot `
    --config-mode system-installed
  if ($LASTEXITCODE -ne 0) {
    throw "installed native system verification failed: $LASTEXITCODE"
  }
  Assert-NoInstallerDatabaseArtifacts $stateRoot
  return @{
    runtime_root = $runtimeRoot
    state_root = $stateRoot
    evidence_path = $evidencePath
    service_sid = $serviceSid
  }
}

function Invoke-PackageSmoke {
if (Test-Path $WorkRoot) {
  Remove-Item -Recurse -Force $WorkRoot
}
New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null

$isMsi = $PackagePath.EndsWith(".msi", [System.StringComparison]::OrdinalIgnoreCase)
if ($isMsi) {
  $payloadRoot = New-ShortAdministrativeExtractRoot
  $script:AdministrativeExtractRoot = $payloadRoot
  $administrativeLog = Join-Path $WorkRoot "msi-administrative-extract.log"
  Invoke-Msi @(
    "/a",
    "`"$PackagePath`"",
    "/qn",
    "/norestart",
    "TARGETDIR=`"$payloadRoot`"",
    "/l*v",
    "`"$administrativeLog`""
  ) "msiexec administrative extraction failed"
  @{
    schema_id = "scratchbird.windows_msi_administrative_extract.v1"
    status = "passed"
    lifecycle_executed = $false
    purpose = "payload inspection only"
    extraction_root_policy = "system_drive_short_root_max_48_characters"
    extraction_root_length = $payloadRoot.Length
  } | ConvertTo-Json | Set-Content (Join-Path $WorkRoot "administrative-extract-proof.json")
} else {
  $payloadRoot = Join-Path $WorkRoot "portable-zip"
  Expand-Archive -Path $PackagePath -DestinationPath $payloadRoot -Force
}

$manifest = Get-ChildItem -Path $payloadRoot -Recurse -Filter "INSTALL_MANIFEST.json" | Select-Object -First 1
if (-not $manifest) {
  throw "missing INSTALL_MANIFEST.json"
}

$binDir = Get-ChildItem -Path $payloadRoot -Recurse -Directory | Where-Object { $_.FullName -match "ScratchBird.*bin$" } | Select-Object -First 1
if (-not $binDir) {
  throw "missing ScratchBird bin directory"
}

if ($isMsi) {
  $configDefaults = Join-Path $binDir.Parent.FullName "share\scratchbird\config-defaults"
  python project/tools/release/verify_native_installed_payload.py `
    $payloadRoot `
    --config-root $configDefaults `
    --config-mode system-defaults
} else {
  python project/tools/release/verify_native_installed_payload.py $payloadRoot
}
if ($LASTEXITCODE -ne 0) {
  throw "native installed payload verification failed: $LASTEXITCODE"
}

$savedPath = $env:PATH
$env:PATH = "$($binDir.FullName);$env:SystemRoot\System32;$env:SystemRoot"
try {
  $nativeProfile = Get-ChildItem -Path $payloadRoot -Recurse -Filter "NATIVE_RELEASE_PROFILE.json" | Select-Object -First 1
  if (-not $nativeProfile) {
    throw "missing NATIVE_RELEASE_PROFILE.json"
  }
  $profile = Get-Content $nativeProfile.FullName -Raw | ConvertFrom-Json
  if ($profile.llvm_runtime.delivery -ne "bundled") {
    throw "Windows LLVM runtime delivery is not bundled"
  }
  $llvmPath = Join-Path $binDir.FullName $profile.llvm_runtime.runtime_library
  if (-not (Test-Path $llvmPath)) {
    throw "missing bundled LLVM runtime: $llvmPath"
  }
  $llvmHandle = [System.Runtime.InteropServices.NativeLibrary]::Load($llvmPath)
  try {
    "llvm_runtime_library=$($profile.llvm_runtime.runtime_library)`nllvm_runtime_load=passed" | Set-Content (Join-Path $WorkRoot "llvm-runtime-load.txt")
  } finally {
    [System.Runtime.InteropServices.NativeLibrary]::Free($llvmHandle)
  }
  foreach ($binary in @("SBsql", "SBadm", "SBbak", "SBsec", "SBdoc", "SBcop", "SBsrv", "SBgate", "SBmgr", "SBParser")) {
    $executable = Join-Path $binDir.FullName "$binary.exe"
    if (-not (Test-Path $executable)) {
      throw "missing native executable: $binary"
    }
    $outputPath = Join-Path $WorkRoot "$binary.help.txt"
    $errorPath = Join-Path $WorkRoot "$binary.help.err.txt"
    $process = Start-Process -FilePath $executable -ArgumentList @("--help") -Wait -PassThru -NoNewWindow -RedirectStandardOutput $outputPath -RedirectStandardError $errorPath
    $status = $process.ExitCode
    $outputBytes = (Get-Item $outputPath).Length + (Get-Item $errorPath).Length
    if ($status -notin @(0, 1, 2) -or $outputBytes -eq 0) {
      throw "native binary launch failed: $binary status=$status"
    }
  }
} finally {
  $env:PATH = $savedPath
}

if (-not $isMsi) {
  Write-Output "smoke_install_windows=passed:portable-zip:$WorkRoot"
  return
}
if ($AdministrativeExtractOnly) {
  Write-Output "smoke_install_windows=passed:administrative-extract-only:$WorkRoot"
  return
}
if (@(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'").Count -ne 0) {
  throw "actual MSI smoke requires a host without a pre-existing scratchbird service"
}
if (@(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE").Count -ne 0 -or @(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE").Count -ne 0) {
  throw "actual MSI smoke requires a host without a pre-existing ScratchBird SAM identity"
}
Assert-InstallerTransactionJournalAbsent

$failedFreshInstallLog = Join-Path $WorkRoot "msi-fault-injected-fresh-install.log"
$failedFreshInstallExitCode = Invoke-MsiExpectedFailure @(
  "/i",
  "`"$PackagePath`"",
  "/qn",
  "/norestart",
  "WIXFAILWHENDEFERRED=1",
  "/l*v",
  "`"$failedFreshInstallLog`""
) "fault-injected fresh MSI install"
Assert-MsiLogContainsTokens $failedFreshInstallLog @(
  "ScratchBirdPostInstall",
  "ScratchBirdFinalizePostInstall",
  "ScratchBirdCleanupPostInstall",
  "Wix4FailWhenDeferred_X64",
  "ScratchBirdRollbackPostInstall"
)
Assert-NoScratchBirdIdentityAndJournal

$retryStateRoot = Join-Path ([Environment]::GetFolderPath("CommonApplicationData")) "ScratchBird"
$retryConfigRoot = Join-Path $retryStateRoot "config"
if (-not (Test-Path -LiteralPath $retryConfigRoot -PathType Container)) {
  throw "fault-injected install did not preserve its ProgramData configuration directory"
}
Assert-NoInstallerDatabaseArtifacts $retryStateRoot
$retryConfigMarker = Join-Path $retryConfigRoot "qa-failed-install-retry-preserve.conf"
[IO.File]::WriteAllText(
  $retryConfigMarker,
  "operator_config_preserved_across_identity_only_rollback_and_retry"
)
$retryConfigHash = (
  Get-FileHash -LiteralPath $retryConfigMarker -Algorithm SHA256
).Hash

$installLog = Join-Path $WorkRoot "msi-actual-install.log"
Invoke-Msi @(
  "/i",
  "`"$PackagePath`"",
  "/qn",
  "/norestart",
  "/l*v",
  "`"$installLog`""
) "msiexec actual install failed"
Assert-MsiLogContainsTokens $installLog @(
  "ScratchBirdFinalizePostInstall",
  "ScratchBirdCleanupPostInstall"
)
$installed = Assert-InstalledWindowsSystem -RequireGroupCreatedByThisRun
if (
  -not (Test-Path -LiteralPath $retryConfigMarker -PathType Leaf) -or
  (Get-FileHash -LiteralPath $retryConfigMarker -Algorithm SHA256).Hash -ne
    $retryConfigHash
) {
  throw "normal retry did not preserve failed-install ProgramData configuration"
}

$configMarker = Join-Path $installed.state_root "config\qa-operator-preserve.conf"
$dataMarker = Join-Path $installed.state_root "data\qa-operator-preserve.dat"
[IO.File]::WriteAllText($configMarker, "operator_config_preserve_fixture")
[IO.File]::WriteAllText($dataMarker, "operator_data_preserve_fixture")
$configHash = (Get-FileHash -LiteralPath $configMarker -Algorithm SHA256).Hash
$dataHash = (Get-FileHash -LiteralPath $dataMarker -Algorithm SHA256).Hash

$installedPackagePath = $PackagePath
$upgradeStatus = "not_requested"
if (-not [string]::IsNullOrWhiteSpace($UpgradePackagePath)) {
  $UpgradePackagePath = [IO.Path]::GetFullPath($UpgradePackagePath)
  if (-not (Test-Path -LiteralPath $UpgradePackagePath -PathType Leaf) -or -not $UpgradePackagePath.EndsWith(".msi", [StringComparison]::OrdinalIgnoreCase)) {
    throw "UpgradePackagePath is not an MSI"
  }
  $upgradeLog = Join-Path $WorkRoot "msi-actual-upgrade.log"
  Invoke-Msi @(
    "/i",
    "`"$UpgradePackagePath`"",
    "/qn",
    "/norestart",
    "/l*v",
    "`"$upgradeLog`""
  ) "msiexec actual upgrade failed"
  [void](Assert-InstalledWindowsSystem)
  if ((Get-FileHash -LiteralPath $configMarker -Algorithm SHA256).Hash -ne $configHash -or (Get-FileHash -LiteralPath $dataMarker -Algorithm SHA256).Hash -ne $dataHash) {
    throw "MSI upgrade did not preserve operator config/data"
  }
  $installedPackagePath = $UpgradePackagePath
  $upgradeStatus = "passed"
}

$productCode = Get-MsiProperty $installedPackagePath "ProductCode"
$installedIdentitySnapshot = Get-ScratchBirdIdentitySnapshot
$failedUninstallLog = Join-Path $WorkRoot "msi-fault-injected-uninstall.log"
$failedUninstallExitCode = Invoke-MsiExpectedFailure @(
  "/x",
  $productCode,
  "/qn",
  "/norestart",
  "WIXFAILWHENDEFERRED=1",
  "/l*v",
  "`"$failedUninstallLog`""
) "fault-injected MSI uninstall"
Assert-MsiLogContainsTokens $failedUninstallLog @(
  "ScratchBirdPreRemove",
  "ScratchBirdCommitPreRemove",
  "Wix4FailWhenDeferred_X64",
  "ScratchBirdRollbackPreRemove"
)
Assert-ScratchBirdIdentitySnapshot $installedIdentitySnapshot
Assert-InstallerTransactionJournalAbsent
[void](Assert-InstalledWindowsSystem)
if (
  (Get-FileHash -LiteralPath $retryConfigMarker -Algorithm SHA256).Hash -ne
    $retryConfigHash -or
  (Get-FileHash -LiteralPath $configMarker -Algorithm SHA256).Hash -ne
    $configHash -or
  (Get-FileHash -LiteralPath $dataMarker -Algorithm SHA256).Hash -ne
    $dataHash
) {
  throw "fault-injected uninstall changed preserved ProgramData content"
}

$uninstallLog = Join-Path $WorkRoot "msi-actual-uninstall.log"
Invoke-Msi @(
  "/x",
  $productCode,
  "/qn",
  "/norestart",
  "/l*v",
  "`"$uninstallLog`""
) "msiexec actual uninstall failed"
Assert-MsiLogContainsTokens $uninstallLog @(
  "ScratchBirdCommitPreRemove"
)

for ($attempt = 0; $attempt -lt 20; $attempt++) {
  if (@(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'").Count -eq 0) {
    break
  }
  Start-Sleep -Milliseconds 500
}
if (@(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'").Count -ne 0) {
  throw "scratchbird service record remains after uninstall"
}
Assert-InstallerTransactionJournalAbsent
if (
  -not (Test-Path -LiteralPath $retryConfigMarker -PathType Leaf) -or
  -not (Test-Path -LiteralPath $configMarker -PathType Leaf) -or
  -not (Test-Path -LiteralPath $dataMarker -PathType Leaf)
) {
  throw "MSI uninstall removed operator config/data"
}
if (
  (Get-FileHash -LiteralPath $retryConfigMarker -Algorithm SHA256).Hash -ne
    $retryConfigHash -or
  (Get-FileHash -LiteralPath $configMarker -Algorithm SHA256).Hash -ne
    $configHash -or
  (Get-FileHash -LiteralPath $dataMarker -Algorithm SHA256).Hash -ne
    $dataHash
) {
  throw "MSI uninstall changed operator config/data"
}
Assert-NoInstallerDatabaseArtifacts $installed.state_root
[void](Get-ExactScratchBirdGroup)

@{
  schema_id = "scratchbird.windows_msi_actual_install_smoke.v1"
  administrative_extract = "passed_separate_no_lifecycle_claim"
  fault_injected_fresh_install = "failed_as_expected_identity_rollback_passed"
  fault_injected_fresh_install_exit_code = $failedFreshInstallExitCode
  actual_install = "passed"
  upgrade = $upgradeStatus
  fault_injected_uninstall = "failed_as_expected_snapshotted_service_fields_restore_and_preserved_group_verification_passed"
  fault_injected_uninstall_exit_code = $failedUninstallExitCode
  uninstall = "passed"
  installer_transaction_journal_cleared = $true
  installer_transaction_rollback_scope = "service_and_filesystem_operations_group_identity_only"
  rollback_disabled_policy = "blocked_by_package_launch_condition"
  post_install_identity_finalization = "checked_deferred_before_install_finalize"
  post_install_journal_cleanup = "ignored_commit_after_successful_install_finalize_fixed_absolute_System32_reg.exe_exact_key_delete"
  pre_remove_journal_cleanup = "ignored_commit_after_successful_install_finalize_fixed_absolute_System32_reg.exe_exact_key_delete"
  failed_install_programdata_configuration_and_acl_policy = "preserved_not_rolled_back_and_required_acl_reapplied_on_retry"
  service_fresh_install = "manual_stopped"
  service_account = "NT SERVICE\scratchbird"
  filesystem_operations_group_member_count = 0
  filesystem_operations_group_created_on_fresh_install = $true
  filesystem_operations_group_preserved_during_failed_uninstall = $true
  filesystem_operations_group_preserved_after_uninstall = $true
  service_snapshotted_identity_configuration_and_runtime_state_fields_restored_after_failed_uninstall = $true
  service_authority_scope = "filesystem_directory_and_process_execution_only_no_database_or_security_authority"
  service_local_sam_group_membership = $false
  human_service_group_membership_mutated = $false
  create_time_os_authorization = "administrator_only"
  native_default_port = 3092
  topology = "client_to_optional_SBmgr_not_used_with_emulation_to_shared_SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_SBsrv_engine"
  configuration_preserved = $true
  data_preserved = $true
  database_files_created = $false
  security_sidecars_created = $false
  scm_runtime_start_proof = "not_claimed_by_installer_lifecycle_smoke"
} | ConvertTo-Json | Set-Content (Join-Path $WorkRoot "msi-actual-install-smoke.json")

Write-Output "smoke_install_windows=passed:$WorkRoot"
}

$script:AdministrativeExtractRoot = $null
try {
  Invoke-PackageSmoke
} finally {
  if (-not [string]::IsNullOrWhiteSpace($script:AdministrativeExtractRoot)) {
    $cleanupRoot = $script:AdministrativeExtractRoot
    if (Test-Path -LiteralPath $cleanupRoot) {
      Remove-Item -LiteralPath $cleanupRoot -Recurse -Force
    }
    if (Test-Path -LiteralPath $cleanupRoot) {
      throw "administrative extraction root cleanup failed"
    }
    if (Test-Path -LiteralPath $WorkRoot -PathType Container) {
      @{
        schema_id = "scratchbird.windows_msi_administrative_extract_cleanup.v1"
        status = "passed"
        extraction_root_removed = $true
        extraction_root_policy = "system_drive_short_root_max_48_characters"
      } | ConvertTo-Json | Set-Content (
        Join-Path $WorkRoot "administrative-extract-cleanup-proof.json"
      )
    }
  }
}
