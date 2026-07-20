# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

<#
.SYNOPSIS
  Idempotent Windows system-package lifecycle for native ScratchBird.

.DESCRIPTION
  Creates no database. Creates or validates the local SAM ScratchBird
  filesystem-operations group without adding human members, installs one
  manual/stopped service named scratchbird under the managed virtual account
  NT SERVICE\scratchbird, and protects mutable ProgramData directories.
#>

[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $Action,
  [string] $InstallRoot,
  [string] $StateRoot,
  [string] $PackageVersion = "unknown",
  [string] $PackageFormat = "msi"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$ServiceName = "scratchbird"
$ServiceDisplayName = "ScratchBird Native Database Server"
$ServiceAccount = "NT SERVICE\scratchbird"
$GroupName = "ScratchBird"
$GroupDescription = "ScratchBird filesystem operations group; no database or security authority"
$EvidenceName = "WINDOWS_SYSTEM_INSTALL_STATE.json"
$AllowedActions = @("PostInstall", "PreRemove", "Verify")
$ServiceCreatedByThisRun = $false
$LifecyclePhase = "PRECHECK"

function Rollback-CreatedService {
  if ($Action -ne "PostInstall" -or -not $script:ServiceCreatedByThisRun) {
    return
  }
  try {
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    & $sc delete $ServiceName 1>$null 2>$null
  } catch {
    # Preserve the original fail-closed diagnostic below.
  }
  $script:ServiceCreatedByThisRun = $false
}

function Fail-Code {
  param([string] $Code, [int] $Status = 1)
  Rollback-CreatedService
  [Console]::Error.WriteLine($Code)
  exit $Status
}

function Invoke-NativeQuiet {
  param([string] $FilePath, [string[]] $Arguments, [string] $FailureCode)
  & $FilePath @Arguments 1>$null 2>$null
  $nativeExitCode = [int]$LASTEXITCODE
  if ($nativeExitCode -ne 0) {
    if ($FailureCode -eq "BOOTSTRAP.INSTALL_DEFAULTS_INVALID") {
      Fail-Code "${FailureCode}.$script:LifecyclePhase.NATIVE_EXIT_$nativeExitCode"
    }
    Fail-Code $FailureCode
  }
}

function Test-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = [Security.Principal.WindowsPrincipal]::new($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-CanonicalPath {
  param([string] $Path)
  if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
  try {
    return [IO.Path]::GetFullPath($Path).TrimEnd("\")
  } catch {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
}

function Test-PathEqual {
  param([string] $Left, [string] $Right)
  return [string]::Equals((Get-CanonicalPath $Left), (Get-CanonicalPath $Right), [StringComparison]::OrdinalIgnoreCase)
}

function Assert-SystemPaths {
  $expectedInstallRoot = Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "ScratchBird"
  $expectedStateRoot = Join-Path ([Environment]::GetFolderPath("CommonApplicationData")) "ScratchBird"
  if (-not (Test-PathEqual $script:InstallRoot $expectedInstallRoot) -or -not (Test-PathEqual $script:StateRoot $expectedStateRoot)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
}

function Assert-NotReparsePoint {
  param([string] $Path)
  if (Test-Path -LiteralPath $Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
    }
  }
}

function Ensure-Directory {
  param([string] $Path)
  Assert-NotReparsePoint $Path
  if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
  }
  Assert-NotReparsePoint $Path
}

function Get-SystemNetExecutable {
  $systemDirectory = [Environment]::SystemDirectory
  if ([string]::IsNullOrWhiteSpace($systemDirectory) -or -not [IO.Path]::IsPathRooted($systemDirectory)) {
    throw [InvalidOperationException]::new()
  }
  $canonicalSystemDirectory = [IO.Path]::GetFullPath($systemDirectory).TrimEnd("\")
  $candidate = Join-Path $canonicalSystemDirectory "net.exe"
  if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
    throw [InvalidOperationException]::new()
  }
  $item = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw [InvalidOperationException]::new()
  }
  $canonicalCandidate = [IO.Path]::GetFullPath($item.FullName)
  if (-not [string]::Equals(
      [IO.Path]::GetDirectoryName($canonicalCandidate),
      $canonicalSystemDirectory,
      [StringComparison]::OrdinalIgnoreCase) -or
      -not [string]::Equals(
        [IO.Path]::GetFileName($canonicalCandidate),
        "net.exe",
        [StringComparison]::OrdinalIgnoreCase)) {
    throw [InvalidOperationException]::new()
  }
  return $canonicalCandidate
}

function Get-LocalScratchBirdGroup {
  $rows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
  if ($rows.Count -ne 1) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  $row = $rows[0]
  if (-not [string]::Equals($row.Domain, $env:COMPUTERNAME, [StringComparison]::OrdinalIgnoreCase) -or -not $row.LocalAccount -or [int]$row.SIDType -ne 4 -or $row.SID -notmatch '^S-1-5-21-(\d+-){3}\d+$') {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  return $row
}

function Ensure-LocalScratchBirdGroup {
  $rows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
  $conflictingUsers = @(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE")
  if ($conflictingUsers.Count -ne 0) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  if ($rows.Count -eq 0) {
    try {
      $net = Get-SystemNetExecutable
      & $net "localgroup" $GroupName "/add" "/comment:$GroupDescription" 1>$null 2>$null
      $nativeStatus = [int]$LASTEXITCODE
      if ($nativeStatus -ne 0) {
        Fail-Code "BOOTSTRAP.GROUP_CREATE_FAILED.NET_CREATE_EXIT_$nativeStatus"
      }
    } catch {
      Fail-Code "BOOTSTRAP.GROUP_CREATE_FAILED.NET_CREATE"
    }
  } elseif ($rows.Count -ne 1) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  return Get-LocalScratchBirdGroup
}

function Convert-AdsSidToString {
  param($Value)
  return [Security.Principal.SecurityIdentifier]::new([byte[]]$Value, 0).Value
}

function Get-ServiceRecord {
  $rows = @(Get-CimInstance -ClassName Win32_Service -Filter "Name='scratchbird'")
  if ($rows.Count -gt 1) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  if ($rows.Count -eq 0) {
    return $null
  }
  return $rows[0]
}

function Get-ExpectedServiceCommand {
  $server = Join-Path $InstallRoot "bin\SBsrv.exe"
  $config = Join-Path $StateRoot "config\SBsrv.conf"
  return ('"{0}" --config "{1}" --service' -f $server, $config)
}

function Assert-ServiceRecord {
  param($Service, [switch] $RequireFreshDefaults)
  if (-not [string]::Equals($Service.StartName, $ServiceAccount, [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.PathName, (Get-ExpectedServiceCommand), [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.ServiceType, "Own Process", [StringComparison]::OrdinalIgnoreCase)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  if ($RequireFreshDefaults -and (-not [string]::Equals($Service.StartMode, "Manual", [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase))) {
    Fail-Code "BOOTSTRAP.SERVICE_START_FORBIDDEN"
  }
  $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
  $sidType = (Get-ItemProperty -LiteralPath $serviceKey -Name "ServiceSidType" -ErrorAction Stop).ServiceSidType
  if ([int]$sidType -ne 3) {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
}

function New-ManagedVirtualService {
  # CreateServiceW accepts the virtual account and a true NULL password pointer
  # in one SCM operation. Do not route this through sc.exe: its text command
  # line cannot represent the required pointer contract safely. Atomic creation
  # also means no service record is ever created under the SCM default identity.
  $nativeSource = @'
using System;
using System.Runtime.InteropServices;

namespace ScratchBird.WindowsInstaller
{
    public static class ServiceNative
    {
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr OpenSCManagerW(
            string machineName,
            string databaseName,
            UInt32 desiredAccess);

        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateServiceW(
            IntPtr serviceManager,
            string serviceName,
            string displayName,
            UInt32 desiredAccess,
            UInt32 serviceType,
            UInt32 startType,
            UInt32 errorControl,
            string binaryPathName,
            string loadOrderGroup,
            IntPtr tagId,
            string dependencies,
            string serviceStartName,
            IntPtr lpPassword);

        [DllImport("advapi32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool CloseServiceHandle(IntPtr serviceHandle);
    }
}
'@
  $manager = [IntPtr]::Zero
  $service = [IntPtr]::Zero
  $failureCode = $null
  try {
    Add-Type -TypeDefinition $nativeSource -ErrorAction Stop
    # Windows PowerShell binds `$null for a [string] P/Invoke argument as an
    # empty string.  An empty machine name is permitted for the local SCM, but
    # an empty database name is ERROR_INVALID_NAME (123).  Pass the documented
    # active SCM database explicitly so this contract is independent of host
    # PowerShell null-marshalling behavior.
    $manager = [ScratchBird.WindowsInstaller.ServiceNative]::OpenSCManagerW(
      [string]::Empty,
      "ServicesActive",
      [uint32]2
    )
    if ($manager -eq [IntPtr]::Zero) {
      $lastError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      $failureCode = "BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$script:LifecyclePhase.OPEN_SCM_ERROR_$lastError"
    } else {
      $service = [ScratchBird.WindowsInstaller.ServiceNative]::CreateServiceW(
        $manager,
        $ServiceName,
        $ServiceDisplayName,
        [uint32]2,
        [uint32]0x00000010,
        [uint32]0x00000003,
        [uint32]0x00000001,
        (Get-ExpectedServiceCommand),
        $null,
        [IntPtr]::Zero,
        $null,
        $ServiceAccount,
        [IntPtr]::Zero
      )
      if ($service -eq [IntPtr]::Zero) {
        $lastError = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        $failureCode = "BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$script:LifecyclePhase.CREATE_SERVICE_ERROR_$lastError"
      }
    }
  } catch {
    $failureCode = "BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$script:LifecyclePhase.NATIVE_BINDING"
  } finally {
    if ($service -ne [IntPtr]::Zero) {
      [void][ScratchBird.WindowsInstaller.ServiceNative]::CloseServiceHandle($service)
    }
    if ($manager -ne [IntPtr]::Zero) {
      [void][ScratchBird.WindowsInstaller.ServiceNative]::CloseServiceHandle($manager)
    }
  }
  if ($null -ne $failureCode) {
    Fail-Code $failureCode
  }
}

function Ensure-SBsrvService {
  $script:LifecyclePhase = "SERVICE_IDENTITY.QUERY_EXISTING"
  $existing = Get-ServiceRecord
  $preexisting = $null -ne $existing
  $previousStartMode = if ($preexisting) { [string]$existing.StartMode } else { "absent" }
  $previousState = if ($preexisting) { [string]$existing.State } else { "absent" }
  if ($preexisting) {
    $script:LifecyclePhase = "SERVICE_IDENTITY.ASSERT_EXISTING"
    Assert-ServiceRecord $existing
  } else {
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    $script:LifecyclePhase = "SERVICE_IDENTITY.CREATE"
    New-ManagedVirtualService
    $script:ServiceCreatedByThisRun = $true
    $script:LifecyclePhase = "SERVICE_IDENTITY.DESCRIPTION"
    Invoke-NativeQuiet $sc @("description", $ServiceName, "ScratchBird native SBsrv owner for shared SBgate and standalone SBParser") "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  $sc = Join-Path $env:SystemRoot "System32\sc.exe"
  $script:LifecyclePhase = "SERVICE_IDENTITY.SIDTYPE"
  Invoke-NativeQuiet $sc @("sidtype", $ServiceName, "restricted") "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  $script:LifecyclePhase = "SERVICE_IDENTITY.QUERY_RESULT"
  $service = Get-ServiceRecord
  if ($null -eq $service) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  $script:LifecyclePhase = "SERVICE_IDENTITY.ASSERT_RESULT"
  Assert-ServiceRecord $service -RequireFreshDefaults:(-not $preexisting)
  return [ordered]@{
    preexisting = $preexisting
    previous_start_mode = $previousStartMode
    previous_state = $previousState
    current_start_mode = [string]$service.StartMode
    current_state = [string]$service.State
  }
}

function Get-ManagedServiceSid {
  try {
    $account = [Security.Principal.NTAccount]::new($ServiceAccount)
    $sid = $account.Translate([Security.Principal.SecurityIdentifier])
  } catch {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
  if ($sid.Value -notmatch '^S-1-5-80-(\d+-){4}\d+$') {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
  $localGroups = @(Get-CimInstance -ClassName Win32_Group -Filter "LocalAccount=TRUE")
  if ($localGroups.Count -eq 0) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  $seenGroupSids = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
  foreach ($groupRecord in $localGroups) {
    if (-not $groupRecord.LocalAccount -or [int]$groupRecord.SIDType -ne 4 -or [string]::IsNullOrWhiteSpace([string]$groupRecord.SID) -or -not $seenGroupSids.Add([string]$groupRecord.SID)) {
      Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
    }
    try {
      $localGroup = [ADSI]("WinNT://{0}/{1},group" -f $groupRecord.Domain, $groupRecord.Name)
      foreach ($member in @($localGroup.PSBase.Invoke("Members"))) {
        $sidBytes = $member.GetType().InvokeMember("objectSid", [Reflection.BindingFlags]::GetProperty, $null, $member, $null)
        if ((Convert-AdsSidToString $sidBytes) -eq $sid.Value) {
          Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
        }
      }
    } catch {
      Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
    }
  }
  return $sid
}

function Add-DirectoryRule {
  param([Security.AccessControl.DirectorySecurity] $Acl, [Security.Principal.SecurityIdentifier] $Sid, [Security.AccessControl.FileSystemRights] $Rights)
  $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
  $rule = [Security.AccessControl.FileSystemAccessRule]::new($Sid, $Rights, $inheritance, [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow)
  [void]$Acl.AddAccessRule($rule)
}

function Set-ProtectedDirectoryAcl {
  param([string] $Path, [Security.Principal.SecurityIdentifier] $GroupSid, [Security.Principal.SecurityIdentifier] $ServiceSid, [string] $Profile)
  Ensure-Directory $Path
  $systemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
  $administratorsSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
  $acl = [Security.AccessControl.DirectorySecurity]::new()
  $acl.SetAccessRuleProtection($true, $false)
  $acl.SetOwner($administratorsSid)
  Add-DirectoryRule $acl $systemSid ([Security.AccessControl.FileSystemRights]::FullControl)
  Add-DirectoryRule $acl $administratorsSid ([Security.AccessControl.FileSystemRights]::FullControl)
  if ($Profile -eq "config") {
    Add-DirectoryRule $acl $GroupSid ([Security.AccessControl.FileSystemRights]::Modify)
    Add-DirectoryRule $acl $ServiceSid ([Security.AccessControl.FileSystemRights]::ReadAndExecute)
  } elseif ($Profile -eq "secret") {
    Add-DirectoryRule $acl $ServiceSid ([Security.AccessControl.FileSystemRights]::ReadAndExecute)
  } elseif ($Profile -eq "install") {
    Add-DirectoryRule $acl $GroupSid ([Security.AccessControl.FileSystemRights]::ReadAndExecute)
  } else {
    Add-DirectoryRule $acl $GroupSid ([Security.AccessControl.FileSystemRights]::Modify)
    Add-DirectoryRule $acl $ServiceSid ([Security.AccessControl.FileSystemRights]::Modify)
  }
  Set-Acl -LiteralPath $Path -AclObject $acl
  $verified = Get-Acl -LiteralPath $Path
  if (-not $verified.AreAccessRulesProtected -or $verified.Owner -notmatch '(?i)Administrators$') {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
}

function Grant-ServiceRuntimeReadExecute {
  param([Security.Principal.SecurityIdentifier] $ServiceSid)
  $acl = Get-Acl -LiteralPath $InstallRoot
  $inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor [Security.AccessControl.InheritanceFlags]::ObjectInherit
  $rule = [Security.AccessControl.FileSystemAccessRule]::new(
    $ServiceSid,
    [Security.AccessControl.FileSystemRights]::ReadAndExecute,
    $inheritance,
    [Security.AccessControl.PropagationFlags]::None,
    [Security.AccessControl.AccessControlType]::Allow
  )
  [void]$acl.SetAccessRule($rule)
  Set-Acl -LiteralPath $InstallRoot -AclObject $acl
  $verified = Get-Acl -LiteralPath $InstallRoot
  $present = @($verified.Access | Where-Object {
    $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
    $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value -eq $ServiceSid.Value -and
    ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ReadAndExecute) -eq [Security.AccessControl.FileSystemRights]::ReadAndExecute
  })
  if ($present.Count -eq 0) {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
}

function Set-ProtectedServiceRegistryAcl {
  param([Security.Principal.SecurityIdentifier] $ServiceSid)
  $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
  $systemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
  $administratorsSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
  $acl = [Security.AccessControl.RegistrySecurity]::new()
  $acl.SetAccessRuleProtection($true, $false)
  $acl.SetOwner($administratorsSid)
  foreach ($entry in @(
    @{ Sid = $systemSid; Rights = [Security.AccessControl.RegistryRights]::FullControl },
    @{ Sid = $administratorsSid; Rights = [Security.AccessControl.RegistryRights]::FullControl },
    @{ Sid = $ServiceSid; Rights = [Security.AccessControl.RegistryRights]::ReadKey }
  )) {
    $rule = [Security.AccessControl.RegistryAccessRule]::new(
      $entry.Sid,
      $entry.Rights,
      [Security.AccessControl.InheritanceFlags]::ContainerInherit,
      [Security.AccessControl.PropagationFlags]::None,
      [Security.AccessControl.AccessControlType]::Allow
    )
    [void]$acl.AddAccessRule($rule)
  }
  Set-Acl -LiteralPath $serviceKey -AclObject $acl
  $verified = Get-Acl -LiteralPath $serviceKey
  if (-not $verified.AreAccessRulesProtected) {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
}

function Copy-MissingConfigurationDefaults {
  $defaultsRoot = Join-Path $InstallRoot "share\scratchbird\config-defaults"
  $configRoot = Join-Path $StateRoot "config"
  Assert-NotReparsePoint $defaultsRoot
  if (-not (Test-Path -LiteralPath $defaultsRoot -PathType Container)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  Ensure-Directory $configRoot
  $installValue = $InstallRoot.Replace("\", "/")
  $stateValue = $StateRoot.Replace("\", "/")
  foreach ($source in @(Get-ChildItem -LiteralPath $defaultsRoot -File -Force)) {
    $destination = Join-Path $configRoot $source.Name
    if (-not (Test-Path -LiteralPath $destination)) {
      $content = [IO.File]::ReadAllText($source.FullName)
      $content = $content.Replace("@SCRATCHBIRD_INSTALL_ROOT@", $installValue)
      $content = $content.Replace("@SCRATCHBIRD_STATE_ROOT@", $stateValue)
      if ($content.Contains("@SCRATCHBIRD_")) {
        Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
      }
      $temporary = "$destination.tmp"
      [IO.File]::WriteAllText(
        $temporary,
        $content,
        [Text.UTF8Encoding]::new($false)
      )
      Move-Item -LiteralPath $temporary -Destination $destination
    }
  }
}

function Get-InstallerDatabaseArtifactInventory {
  if (-not (Test-Path -LiteralPath $StateRoot -PathType Container)) {
    return @()
  }
  return @(Get-ChildItem -LiteralPath $StateRoot -Recurse -File -Force -ErrorAction Stop | Where-Object {
    $_.Name -match '(?i)\.(sbdb|sbrd)$' -or $_.Name -match '(?i)\.sb\.(security_principal_events|local_password_auth)$'
  } | ForEach-Object {
    "{0}|{1}|{2}" -f $_.FullName, $_.Length, $_.LastWriteTimeUtc.Ticks
  } | Sort-Object)
}

function Assert-DatabaseArtifactInventoryUnchanged {
  param([string[]] $Before)
  $after = @(Get-InstallerDatabaseArtifactInventory)
  if ([string]::Join([Environment]::NewLine, $Before) -ne [string]::Join([Environment]::NewLine, $after)) {
    Fail-Code "BOOTSTRAP.AUTH_DB_INPUT_INVALID"
  }
}

function Write-InstallEvidence {
  param($Group, $ServiceState)
  $installStateRoot = Join-Path $StateRoot "install"
  Ensure-Directory $installStateRoot
  $evidencePath = Join-Path $installStateRoot $EvidenceName
  $tempPath = "$evidencePath.tmp"
  $payload = [ordered]@{
    schema_id = "scratchbird.windows_system_install_state.v1"
    package_format = $PackageFormat
    package_version = $PackageVersion
    distribution_profile = "native-sbsql-only"
    native_default_port = 3092
    filesystem_operations_group_sid = [string]$Group.SID
    human_service_group_membership_mutated = $false
    create_time_os_authorization = "administrator_only"
    service_name = $ServiceName
    service_account = $ServiceAccount
    service_account_leaf_name = "scratchbird"
    service_sid_type = "restricted"
    service_authority_scope = "filesystem_directory_and_process_execution_only_no_database_or_security_authority"
    service_local_sam_group_membership = $false
    service_preexisting = [bool]$ServiceState.preexisting
    service_previous_start_mode = [string]$ServiceState.previous_start_mode
    service_previous_state = [string]$ServiceState.previous_state
    service_current_start_mode = [string]$ServiceState.current_start_mode
    service_current_state = [string]$ServiceState.current_state
    topology = "client_to_optional_SBmgr_not_used_with_emulation_to_shared_SBgate_to_standalone_selected_SBParser_to_SBPS_IPC_to_SBsrv_engine"
    database_files_created = $false
    security_sidecars_created = $false
    upgrade_preserved_existing_service_state = [bool]$ServiceState.preexisting
  }
  $json = ($payload | ConvertTo-Json -Depth 4) + [Environment]::NewLine
  [IO.File]::WriteAllText($tempPath, $json, [Text.UTF8Encoding]::new($false))
  Move-Item -LiteralPath $tempPath -Destination $evidencePath -Force
}

function Remove-SBsrvService {
  $service = Get-ServiceRecord
  if ($null -eq $service) {
    return
  }
  if (-not [string]::Equals($service.StartName, $ServiceAccount, [StringComparison]::OrdinalIgnoreCase)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  $sc = Join-Path $env:SystemRoot "System32\sc.exe"
  if (-not [string]::Equals($service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase)) {
    & $sc stop $ServiceName 1>$null 2>$null
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
      Start-Sleep -Milliseconds 500
      $service = Get-ServiceRecord
      if ($null -eq $service -or [string]::Equals($service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase)) {
        break
      }
    }
    if ($null -ne $service -and -not [string]::Equals($service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase)) {
      Fail-Code "BOOTSTRAP.SERVICE_START_FORBIDDEN"
    }
  }
  Invoke-NativeQuiet $sc @("delete", $ServiceName) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
}

try {
  if ($AllowedActions -notcontains $Action) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
  if (-not (Test-Administrator)) {
    Fail-Code "BOOTSTRAP.OS_AUTHORITY_DENIED"
  }
  if ($PackageVersion -notmatch '^[A-Za-z0-9._+~-]{1,96}$' -or $PackageFormat -notmatch '^[A-Za-z0-9._-]{1,32}$') {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
  if ([string]::IsNullOrWhiteSpace($InstallRoot)) {
    $InstallRoot = Join-Path ([Environment]::GetFolderPath("ProgramFiles")) "ScratchBird"
  }
  if ([string]::IsNullOrWhiteSpace($StateRoot)) {
    $StateRoot = Join-Path ([Environment]::GetFolderPath("CommonApplicationData")) "ScratchBird"
  }
  $InstallRoot = Get-CanonicalPath $InstallRoot
  $StateRoot = Get-CanonicalPath $StateRoot
  $LifecyclePhase = "SYSTEM_PATHS"
  Assert-SystemPaths
  $LifecyclePhase = "INSTALL_ROOT"
  Assert-NotReparsePoint $InstallRoot
  if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  $LifecyclePhase = "STATE_ROOT"
  Ensure-Directory $StateRoot
  $LifecyclePhase = "DATABASE_INVENTORY_BEFORE"
  $databaseArtifactsBefore = @(Get-InstallerDatabaseArtifactInventory)

  if ($Action -eq "PreRemove") {
    $LifecyclePhase = "PRE_REMOVE_SERVICE"
    Remove-SBsrvService
    exit 0
  }

  $LifecyclePhase = "GROUP_IDENTITY"
  $group = if ($Action -eq "PostInstall") { Ensure-LocalScratchBirdGroup } else { Get-LocalScratchBirdGroup }
  $LifecyclePhase = "SERVICE_IDENTITY"
  $serviceState = if ($Action -eq "PostInstall") {
    Ensure-SBsrvService
  } else {
    $service = Get-ServiceRecord
    if ($null -eq $service) {
      Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
    }
    Assert-ServiceRecord $service
    [ordered]@{
      preexisting = $true
      previous_start_mode = [string]$service.StartMode
      previous_state = [string]$service.State
      current_start_mode = [string]$service.StartMode
      current_state = [string]$service.State
    }
  }
  $LifecyclePhase = "SERVICE_SID"
  $serviceSid = Get-ManagedServiceSid
  $LifecyclePhase = "GROUP_SID"
  $groupSid = [Security.Principal.SecurityIdentifier]::new([string]$group.SID)
  $LifecyclePhase = "RUNTIME_ACL"
  Grant-ServiceRuntimeReadExecute $serviceSid
  $LifecyclePhase = "SERVICE_REGISTRY_ACL"
  Set-ProtectedServiceRegistryAcl $serviceSid

  $LifecyclePhase = "STATE_DIRECTORY_ACL"
  foreach ($entry in @(
    @{ Relative = "config"; Profile = "config" },
    @{ Relative = "data"; Profile = "mutable" },
    @{ Relative = "log"; Profile = "mutable" },
    @{ Relative = "run"; Profile = "mutable" },
    @{ Relative = "run\control"; Profile = "mutable" },
    @{ Relative = "run\listener\control"; Profile = "mutable" },
    @{ Relative = "run\listener\runtime"; Profile = "mutable" },
    @{ Relative = "run\manager\control"; Profile = "mutable" },
    @{ Relative = "run\manager\runtime"; Profile = "mutable" },
    @{ Relative = "tls"; Profile = "secret" },
    @{ Relative = "secrets"; Profile = "secret" },
    @{ Relative = "install"; Profile = "install" }
  )) {
    Set-ProtectedDirectoryAcl (Join-Path $StateRoot $entry.Relative) $groupSid $serviceSid $entry.Profile
  }

  if ($Action -eq "PostInstall") {
    $LifecyclePhase = "CONFIG_DEFAULTS"
    Copy-MissingConfigurationDefaults
  }

  $LifecyclePhase = "DATABASE_INVENTORY_AFTER"
  Assert-DatabaseArtifactInventoryUnchanged $databaseArtifactsBefore
  $LifecyclePhase = "INSTALL_EVIDENCE"
  Write-InstallEvidence $group $serviceState
  $LifecyclePhase = "DATABASE_INVENTORY_FINAL"
  Assert-DatabaseArtifactInventoryUnchanged $databaseArtifactsBefore
  exit 0
} catch {
  Rollback-CreatedService
  [Console]::Error.WriteLine("BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$LifecyclePhase")
  exit 1
}
