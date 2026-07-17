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
$ServiceDescription = "ScratchBird native SBsrv owner for shared SBgate and standalone SBParser"
$EvidenceName = "WINDOWS_SYSTEM_INSTALL_STATE.json"
$TransactionKey = "HKLM:\SOFTWARE\ScratchBird\InstallerTransaction"
$TransactionStateName = "State"
$TransactionSchema = "scratchbird.windows_installer_transaction.v1"
$AllowedActions = @(
  "PostInstall",
  "RollbackPostInstall",
  "CommitPostInstall",
  "PreRemove",
  "RollbackPreRemove",
  "Verify"
)
$GroupCreatedByThisRun = $false
$LifecyclePhase = "PRECHECK"

function Fail-Code {
  param([string] $Code, [int] $Status = 1)
  [Console]::Error.WriteLine($Code)
  exit $Status
}

function New-TransactionRegistryAcl {
  $systemSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-18")
  $administratorsSid = [Security.Principal.SecurityIdentifier]::new("S-1-5-32-544")
  $acl = [Security.AccessControl.RegistrySecurity]::new()
  $acl.SetAccessRuleProtection($true, $false)
  $acl.SetOwner($administratorsSid)
  foreach ($sid in @($systemSid, $administratorsSid)) {
    $rule = [Security.AccessControl.RegistryAccessRule]::new(
      $sid,
      [Security.AccessControl.RegistryRights]::FullControl,
      [Security.AccessControl.InheritanceFlags]::ContainerInherit,
      [Security.AccessControl.PropagationFlags]::None,
      [Security.AccessControl.AccessControlType]::Allow
    )
    [void]$acl.AddAccessRule($rule)
  }
  return $acl
}

function Assert-TransactionRegistryAcl {
  if (-not (Test-Path -LiteralPath $TransactionKey -PathType Container)) {
    throw [IO.DirectoryNotFoundException]::new()
  }
  $acl = Get-Acl -LiteralPath $TransactionKey -ErrorAction Stop
  if (-not $acl.AreAccessRulesProtected) {
    throw [UnauthorizedAccessException]::new()
  }
  $ownerSid = $acl.GetOwner(
    [Security.Principal.SecurityIdentifier])
  if ($ownerSid.Value -ne "S-1-5-32-544") {
    throw [UnauthorizedAccessException]::new()
  }
  $rules = @($acl.GetAccessRules(
      $true,
      $true,
      [Security.Principal.SecurityIdentifier]))
  if ($rules.Count -ne 2) {
    throw [UnauthorizedAccessException]::new()
  }
  $expectedSids = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal)
  [void]$expectedSids.Add("S-1-5-18")
  [void]$expectedSids.Add("S-1-5-32-544")
  foreach ($rule in $rules) {
    if ($rule.IsInherited -or
        $rule.AccessControlType -ne
          [Security.AccessControl.AccessControlType]::Allow -or
        $rule.RegistryRights -ne
          [Security.AccessControl.RegistryRights]::FullControl -or
        $rule.InheritanceFlags -ne
          [Security.AccessControl.InheritanceFlags]::ContainerInherit -or
        $rule.PropagationFlags -ne
          [Security.AccessControl.PropagationFlags]::None -or
        -not $expectedSids.Remove($rule.IdentityReference.Value)) {
      throw [UnauthorizedAccessException]::new()
    }
  }
  if ($expectedSids.Count -ne 0) {
    throw [UnauthorizedAccessException]::new()
  }
}

function Assert-TransactionRegistryValues {
  $key = Get-Item -LiteralPath $TransactionKey -Force -ErrorAction Stop
  $valueNames = @($key.GetValueNames())
  if ($valueNames.Count -ne 1 -or
      -not [string]::Equals(
        [string]$valueNames[0],
        $TransactionStateName,
        [StringComparison]::Ordinal)) {
    throw [IO.InvalidDataException]::new()
  }
  $valueKind = $key.GetValueKind($TransactionStateName)
  if ($valueKind -ne [Microsoft.Win32.RegistryValueKind]::String) {
    throw [IO.InvalidDataException]::new()
  }
}

function Write-TransactionState {
  param($State)
  Assert-TransactionRegistryAcl
  $json = $State | ConvertTo-Json -Depth 12 -Compress
  if ([string]::IsNullOrWhiteSpace($json) -or $json.Length -gt 65535) {
    throw [IO.InvalidDataException]::new()
  }
  $key = Get-Item -LiteralPath $TransactionKey -Force -ErrorAction Stop
  if (@($key.GetValueNames()) -contains $TransactionStateName) {
    Set-ItemProperty `
      -LiteralPath $TransactionKey `
      -Name $TransactionStateName `
      -Value $json `
      -Force `
      -ErrorAction Stop
  } else {
    New-ItemProperty `
      -LiteralPath $TransactionKey `
      -Name $TransactionStateName `
      -Value $json `
      -PropertyType String `
      -Force `
      -ErrorAction Stop |
        Out-Null
  }
  Assert-TransactionRegistryAcl
  Assert-TransactionRegistryValues
}

function Initialize-TransactionState {
  param($State)
  if (Test-Path -LiteralPath $TransactionKey) {
    throw [InvalidOperationException]::new()
  }
  $transactionParent = Split-Path -Parent $TransactionKey
  if (-not (Test-Path -LiteralPath $transactionParent -PathType Container)) {
    New-Item -Path $transactionParent -Force |
      Out-Null
  }
  New-Item -Path $TransactionKey -ErrorAction Stop |
    Out-Null
  Set-Acl `
    -LiteralPath $TransactionKey `
    -AclObject (New-TransactionRegistryAcl) `
    -ErrorAction Stop
  Write-TransactionState $State
}

function Read-TransactionState {
  param([string] $ExpectedOperation, [switch] $AllowMissing)
  if (-not (Test-Path -LiteralPath $TransactionKey -PathType Container)) {
    if ($AllowMissing) {
      return $null
    }
    throw [IO.DirectoryNotFoundException]::new()
  }
  Assert-TransactionRegistryAcl
  Assert-TransactionRegistryValues
  $json = Get-ItemPropertyValue `
    -LiteralPath $TransactionKey `
    -Name $TransactionStateName `
    -ErrorAction Stop
  if ($json -isnot [string] -or [string]::IsNullOrWhiteSpace($json)) {
    throw [IO.InvalidDataException]::new()
  }
  $state = $json | ConvertFrom-Json -ErrorAction Stop
  $nonceGuid = [Guid]::Empty
  if ($null -eq $state -or
      -not [string]::Equals(
        [string]$state.schema_id,
        $TransactionSchema,
        [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        [string]$state.operation,
        $ExpectedOperation,
        [StringComparison]::Ordinal) -or
      -not [Guid]::TryParseExact(
        [string]$state.nonce,
        "D",
        [ref]$nonceGuid) -or
      $nonceGuid -eq [Guid]::Empty -or
      -not (Test-PathEqual ([string]$state.install_root) $InstallRoot) -or
      -not (Test-PathEqual ([string]$state.state_root) $StateRoot) -or
      -not [string]::Equals(
        [string]$state.package_version,
        $PackageVersion,
        [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        [string]$state.package_format,
        $PackageFormat,
        [StringComparison]::Ordinal)) {
    throw [IO.InvalidDataException]::new()
  }
  Assert-TransactionStateShape $state
  return $state
}

function Assert-BooleanProperties {
  param($Object, [string[]] $Names)
  if ($null -eq $Object) {
    throw [IO.InvalidDataException]::new()
  }
  $available = @($Object.PSObject.Properties.Name)
  foreach ($name in $Names) {
    if ($available -notcontains $name -or
        $Object.$name -isnot [bool]) {
      throw [IO.InvalidDataException]::new()
    }
  }
}

function Assert-TransactionStateShape {
  param($State)
  if ([string]::Equals(
      [string]$State.operation,
      "post_install",
      [StringComparison]::Ordinal)) {
    Assert-BooleanProperties $State @("commit_completed")
    Assert-BooleanProperties $State.group @(
      "preexisting",
      "create_intent",
      "create_completed",
      "comment_normalize_intent",
      "comment_normalized"
    )
    Assert-BooleanProperties $State.service @(
      "preexisting",
      "create_intent",
      "create_completed",
      "sid_type_completed",
      "registry_acl_completed",
      "display_name_normalize_intent",
      "display_name_normalized",
      "description_normalize_intent",
      "description_normalized"
    )
    if ([bool]$State.group.create_intent -eq
          [bool]$State.group.preexisting -or
        [bool]$State.service.create_intent -eq
          [bool]$State.service.preexisting -or
        $State.group.sid_before -isnot [string] -or
        $State.group.comment_before -isnot [string] -or
        $State.service.display_name_before -isnot [string] -or
        $State.service.description_before -isnot [string] -or
        $State.service.start_mode_before -isnot [string] -or
        $State.service.state_before -isnot [string] -or
        $State.service.service_security_before -isnot [string] -or
        $State.service.registry_acl_before -isnot [string]) {
      throw [IO.InvalidDataException]::new()
    }
    if ([bool]$State.service.preexisting) {
      [void](ConvertTo-NormalizedSddl (
        [string]$State.service.service_security_before))
      [void](ConvertTo-NormalizedSddl (
        [string]$State.service.registry_acl_before))
    }
    if (([bool]$State.group.preexisting -and
        [string]$State.group.sid_before -notmatch
          '^S-1-5-21-(\d+-){3}\d+$') -or
        (-not [bool]$State.group.preexisting -and
        -not [string]::IsNullOrEmpty(
          [string]$State.group.sid_before))) {
      throw [IO.InvalidDataException]::new()
    }
    if ([bool]$State.commit_completed -and
        (-not [bool]$State.group.comment_normalized -or
        -not [bool]$State.service.display_name_normalized -or
        -not [bool]$State.service.description_normalized -or
        (-not [bool]$State.group.preexisting -and
          -not [bool]$State.group.create_completed) -or
        (-not [bool]$State.service.preexisting -and
          (-not [bool]$State.service.create_completed -or
          -not [bool]$State.service.sid_type_completed -or
          -not [bool]$State.service.registry_acl_completed)))) {
      throw [IO.InvalidDataException]::new()
    }
    return
  }
  if ([string]::Equals(
      [string]$State.operation,
      "pre_remove",
      [StringComparison]::Ordinal)) {
    Assert-BooleanProperties $State.service @(
      "stop_intent",
      "stop_completed",
      "delete_intent",
      "delete_completed"
    )
    $snapshot = $State.service.snapshot
    Assert-BooleanProperties $snapshot @("present")
    if (-not [string]::Equals(
        [string]$snapshot.name,
        $ServiceName,
        [StringComparison]::Ordinal) -or
        [bool]$State.service.delete_intent -ne
          [bool]$snapshot.present) {
      throw [IO.InvalidDataException]::new()
    }
    if ([bool]$snapshot.present) {
      foreach ($name in @(
        "display_name",
        "description",
        "path_name",
        "start_name",
        "start_mode",
        "state",
        "service_type",
        "error_control",
        "service_security_sddl",
        "registry_security_sddl"
      )) {
        if ($snapshot.PSObject.Properties.Name -notcontains $name -or
            $snapshot.$name -isnot [string]) {
          throw [IO.InvalidDataException]::new()
        }
      }
      Assert-BooleanProperties $snapshot @(
        "delayed_auto_start_present"
      )
      if (-not [string]::Equals(
          [string]$snapshot.path_name,
          (Get-ExpectedServiceCommand),
          [StringComparison]::OrdinalIgnoreCase) -or
          -not [string]::Equals(
            [string]$snapshot.start_name,
            $ServiceAccount,
            [StringComparison]::OrdinalIgnoreCase) -or
          -not [string]::Equals(
            [string]$snapshot.service_type,
            "Own Process",
            [StringComparison]::OrdinalIgnoreCase) -or
          [int]$snapshot.service_sid_type -ne 3 -or
          @("Stopped", "Running") -notcontains
            [string]$snapshot.state) {
        throw [IO.InvalidDataException]::new()
      }
      [void](Get-ScStartMode ([string]$snapshot.start_mode))
      [void](Get-ScErrorControl ([string]$snapshot.error_control))
      [void](ConvertTo-NormalizedSddl (
        [string]$snapshot.service_security_sddl))
      [void](ConvertTo-NormalizedSddl (
        [string]$snapshot.registry_security_sddl))
    }
    return
  }
  throw [IO.InvalidDataException]::new()
}

function Remove-TransactionState {
  param($ExpectedState)
  if (-not (Test-Path -LiteralPath $TransactionKey -PathType Container)) {
    return
  }
  $current = Read-TransactionState `
    -ExpectedOperation ([string]$ExpectedState.operation)
  if (-not [string]::Equals(
      [string]$current.nonce,
      [string]$ExpectedState.nonce,
      [StringComparison]::Ordinal)) {
    throw [InvalidOperationException]::new()
  }
  Remove-Item -LiteralPath $TransactionKey -Force -ErrorAction Stop
}

function Invoke-NativeQuiet {
  param([string] $FilePath, [string[]] $Arguments, [string] $FailureCode)
  & $FilePath @Arguments 1>$null 2>$null
  if ($LASTEXITCODE -ne 0) {
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

function Get-LocalScratchBirdGroup {
  $rows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
  if ($rows.Count -ne 1) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  $row = $rows[0]
  if (-not [string]::Equals($row.Domain, $env:COMPUTERNAME, [StringComparison]::OrdinalIgnoreCase) -or -not $row.LocalAccount -or [int]$row.SIDType -ne 4 -or $row.SID -notmatch '^S-1-5-21-(\d+-){3}\d+$') {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  try {
    $group = [ADSI]("WinNT://{0}/{1},group" -f $row.Domain, $row.Name)
    $members = @($group.PSBase.Invoke("Members") | Where-Object { $null -ne $_ })
  } catch {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  if ($members.Count -ne 0) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  # Keep this check in the final validator, not just the pre-create inventory:
  # a concurrent local-user creation must never be admitted as the managed
  # virtual service account used by the SCM.
  $conflictingUsers = @(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE")
  if ($conflictingUsers.Count -ne 0) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  return $row
}

function Get-LocalScratchBirdGroupComment {
  param($Group)
  if ($null -eq $Group) {
    throw [ArgumentNullException]::new("Group")
  }
  return [string]$Group.Description
}

function Get-PostInstallGroupComment {
  param([string] $Nonce)
  return "ScratchBird installer transaction $Nonce"
}

function Set-LocalScratchBirdGroupComment {
  param([string] $Comment)
  $net = Get-SystemNetExecutable
  & $net "localgroup" $GroupName "/comment:$Comment" 1>$null 2>$null
  if ([int]$LASTEXITCODE -ne 0) {
    throw [InvalidOperationException]::new()
  }
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

function Ensure-LocalScratchBirdGroup {
  param($TransactionState)
  $script:LifecyclePhase = "GROUP_IDENTITY_INVENTORY"
  $rows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
  $conflictingUsers = @(Get-CimInstance -ClassName Win32_UserAccount -Filter "Name='scratchbird' AND LocalAccount=TRUE")
  if ($conflictingUsers.Count -ne 0) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  if ($rows.Count -eq 0) {
    try {
      $script:LifecyclePhase = "GROUP_IDENTITY_NATIVE_PATH"
      $net = Get-SystemNetExecutable
      $script:LifecyclePhase = "GROUP_IDENTITY_CREATE"
      $transactionComment = Get-PostInstallGroupComment (
        [string]$TransactionState.nonce)
      & $net "localgroup" $GroupName "/add" "/comment:$transactionComment" 1>$null 2>$null
      $nativeStatus = [int]$LASTEXITCODE
      if ($nativeStatus -ne 0) {
        $nativeStatusText = $nativeStatus.ToString(
          [Globalization.CultureInfo]::InvariantCulture)
        $script:LifecyclePhase = "GROUP_IDENTITY_CREATE_EXIT_$nativeStatusText"
        throw [InvalidOperationException]::new()
      }
      $script:GroupCreatedByThisRun = $true
      $TransactionState.group.create_completed = $true
      Write-TransactionState $TransactionState
    } catch {
      $creationFailurePhase = $script:LifecyclePhase
      # A concurrent administrator or installer may have created the exact
      # group after the initial inventory. Accept only that independently
      # verified final state; native-path/creation failures otherwise stay
      # closed.
      $script:LifecyclePhase = "GROUP_IDENTITY_POSTFAILURE_INVENTORY"
      $postFailureRows = @(Get-CimInstance -ClassName Win32_Group -Filter "Name='ScratchBird' AND LocalAccount=TRUE")
      if ($postFailureRows.Count -ne 1) {
        Fail-Code "BOOTSTRAP.GROUP_CREATE_FAILED.$creationFailurePhase"
      }
    }
  } elseif ($rows.Count -ne 1) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  $script:LifecyclePhase = "GROUP_IDENTITY_FINAL_VALIDATE"
  $group = Get-LocalScratchBirdGroup
  if (-not [bool]$TransactionState.group.preexisting -and
      -not [string]::Equals(
        (Get-LocalScratchBirdGroupComment $group),
        (Get-PostInstallGroupComment ([string]$TransactionState.nonce)),
        [StringComparison]::Ordinal)) {
    Fail-Code "BOOTSTRAP.GROUP_CREATE_FAILED.GROUP_IDENTITY_OWNERSHIP"
  }
  return $group
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

function Get-ServiceRegistryKey {
  return "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
}

function Get-ServiceDescription {
  $serviceKey = Get-ServiceRegistryKey
  if (-not (Test-Path -LiteralPath $serviceKey -PathType Container)) {
    throw [IO.DirectoryNotFoundException]::new()
  }
  $description = Get-ItemPropertyValue `
    -LiteralPath $serviceKey `
    -Name "Description" `
    -ErrorAction SilentlyContinue
  return [string]$description
}

function Get-PostInstallServiceDisplayName {
  param([string] $Nonce)
  return "ScratchBird installer transaction $Nonce"
}

function Get-PostInstallServiceDescription {
  param([string] $Nonce)
  return "ScratchBird installer transaction $Nonce"
}

function Assert-ServiceRecord {
  param($Service, [switch] $RequireFreshDefaults)
  if (-not [string]::Equals($Service.StartName, $ServiceAccount, [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.PathName, (Get-ExpectedServiceCommand), [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.ServiceType, "Own Process", [StringComparison]::OrdinalIgnoreCase)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  if ($RequireFreshDefaults -and (-not [string]::Equals($Service.StartMode, "Manual", [StringComparison]::OrdinalIgnoreCase) -or -not [string]::Equals($Service.State, "Stopped", [StringComparison]::OrdinalIgnoreCase))) {
    Fail-Code "BOOTSTRAP.SERVICE_START_FORBIDDEN"
  }
  $serviceKey = Get-ServiceRegistryKey
  $sidType = (Get-ItemProperty -LiteralPath $serviceKey -Name "ServiceSidType" -ErrorAction Stop).ServiceSidType
  if ([int]$sidType -ne 3) {
    Fail-Code "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  }
}

function Ensure-SBsrvService {
  param($TransactionState)
  $existing = Get-ServiceRecord
  $preexisting = [bool]$TransactionState.service.preexisting
  if ($preexisting -ne ($null -ne $existing)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  $previousStartMode = if ($preexisting) { [string]$existing.StartMode } else { "absent" }
  $previousState = if ($preexisting) { [string]$existing.State } else { "absent" }
  if ($preexisting) {
    Assert-ServiceRecord $existing
  } else {
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    $command = Get-ExpectedServiceCommand
    $temporaryDisplayName = Get-PostInstallServiceDisplayName (
      [string]$TransactionState.nonce)
    $temporaryDescription = Get-PostInstallServiceDescription (
      [string]$TransactionState.nonce)
    Invoke-NativeQuiet $sc @(
      "create",
      $ServiceName,
      "binPath= $command",
      "start= demand",
      "obj= $ServiceAccount",
      "password= ",
      "DisplayName= $temporaryDisplayName"
    ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
    Invoke-NativeQuiet $sc @(
      "description",
      $ServiceName,
      $temporaryDescription
    ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
    $TransactionState.service.create_completed = $true
    Write-TransactionState $TransactionState
  }
  if (-not $preexisting) {
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    Invoke-NativeQuiet $sc @(
      "sidtype",
      $ServiceName,
      "restricted"
    ) "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
    $TransactionState.service.sid_type_completed = $true
    Write-TransactionState $TransactionState
  }
  $service = Get-ServiceRecord
  if ($null -eq $service) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  Assert-ServiceRecord $service -RequireFreshDefaults:(-not $preexisting)
  if (-not $preexisting -and
      (-not [string]::Equals(
        [string]$service.DisplayName,
        (Get-PostInstallServiceDisplayName (
          [string]$TransactionState.nonce)),
        [StringComparison]::Ordinal) -or
       -not [string]::Equals(
        (Get-ServiceDescription),
        (Get-PostInstallServiceDescription (
          [string]$TransactionState.nonce)),
        [StringComparison]::Ordinal))) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  return [ordered]@{
    preexisting = $preexisting
    previous_start_mode = $previousStartMode
    previous_state = $previousState
    current_start_mode = [string]$service.StartMode
    current_state = [string]$service.State
  }
}

function New-PostInstallTransactionState {
  $nonce = [Guid]::NewGuid().ToString("D")
  $groupRows = @(
    Get-CimInstance `
      -ClassName Win32_Group `
      -Filter "Name='ScratchBird' AND LocalAccount=TRUE"
  )
  $conflictingUsers = @(
    Get-CimInstance `
      -ClassName Win32_UserAccount `
      -Filter "Name='scratchbird' AND LocalAccount=TRUE"
  )
  if ($conflictingUsers.Count -ne 0 -or $groupRows.Count -gt 1) {
    Fail-Code "BOOTSTRAP.GROUP_INPUT_INVALID"
  }
  $groupPreexisting = $groupRows.Count -eq 1
  $groupSidBefore = ""
  $groupComment = ""
  if ($groupPreexisting) {
    $validatedGroup = Get-LocalScratchBirdGroup
    $groupSidBefore = [string]$validatedGroup.SID
    $groupComment = Get-LocalScratchBirdGroupComment $validatedGroup
  }

  $service = Get-ServiceRecord
  $servicePreexisting = $null -ne $service
  $serviceDescriptionBefore = ""
  $serviceSecurityBefore = ""
  $serviceRegistryAclBefore = ""
  if ($servicePreexisting) {
    Assert-ServiceRecord $service
    $serviceDescriptionBefore = Get-ServiceDescription
    $serviceSecurityBefore = Get-ServiceSecuritySddl
    $serviceRegistryAclBefore = (
      Get-Acl -LiteralPath (Get-ServiceRegistryKey) -ErrorAction Stop
    ).Sddl
  }

  return [ordered]@{
    schema_id = $TransactionSchema
    operation = "post_install"
    nonce = $nonce
    package_version = $PackageVersion
    package_format = $PackageFormat
    install_root = $InstallRoot
    state_root = $StateRoot
    commit_completed = $false
    state_root_preexisting = [bool](
      Test-Path -LiteralPath $StateRoot -PathType Container)
    preservation = [ordered]@{
      database_state = "preserve_never_delete"
      existing_configuration = "preserve_never_overwrite"
      existing_state_directory_acls = "preserve_on_identity_rollback"
      preexisting_group = "preserve_never_delete_or_modify"
      preexisting_service_configuration = "preserve_never_modify"
      preexisting_service_registry_acl = "preserve_never_modify"
    }
    group = [ordered]@{
      preexisting = [bool]$groupPreexisting
      sid_before = [string]$groupSidBefore
      comment_before = [string]$groupComment
      create_intent = [bool](-not $groupPreexisting)
      create_completed = $false
      comment_normalize_intent = $false
      comment_normalized = [bool]$groupPreexisting
    }
    service = [ordered]@{
      preexisting = [bool]$servicePreexisting
      display_name_before = if ($servicePreexisting) {
        [string]$service.DisplayName
      } else {
        ""
      }
      description_before = [string]$serviceDescriptionBefore
      start_mode_before = if ($servicePreexisting) {
        [string]$service.StartMode
      } else {
        "absent"
      }
      state_before = if ($servicePreexisting) {
        [string]$service.State
      } else {
        "absent"
      }
      service_security_before = [string]$serviceSecurityBefore
      registry_acl_before = [string]$serviceRegistryAclBefore
      create_intent = [bool](-not $servicePreexisting)
      create_completed = $false
      sid_type_completed = $false
      registry_acl_completed = $false
      display_name_normalize_intent = $false
      display_name_normalized = [bool]$servicePreexisting
      description_normalize_intent = $false
      description_normalized = [bool]$servicePreexisting
    }
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
  $serviceKey = Get-ServiceRegistryKey
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
    filesystem_operations_group_member_count = 0
    filesystem_operations_group_creation_policy = "absolute_System32_net.exe_localgroup_add_when_missing"
    filesystem_operations_group_created_by_this_run = [bool]$GroupCreatedByThisRun
    lifecycle_process_architecture = "64_bit"
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
  param($TransactionState)
  $service = Get-ServiceRecord
  if ($null -eq $service) {
    if ($null -ne $TransactionState) {
      $TransactionState.service.delete_completed = $true
      Write-TransactionState $TransactionState
    }
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
  if ($null -ne $TransactionState) {
    $TransactionState.service.stop_completed = $true
    $TransactionState.service.delete_intent = $true
    Write-TransactionState $TransactionState
  }
  Invoke-NativeQuiet $sc @("delete", $ServiceName) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  for ($attempt = 0; $attempt -lt 60; $attempt++) {
    if ($null -eq (Get-ServiceRecord)) {
      break
    }
    Start-Sleep -Milliseconds 250
  }
  if ($null -ne (Get-ServiceRecord)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }
  if ($null -ne $TransactionState) {
    $TransactionState.service.delete_completed = $true
    Write-TransactionState $TransactionState
  }
}

function Get-ServiceSecuritySddl {
  $sc = Join-Path $env:SystemRoot "System32\sc.exe"
  $output = @(& $sc "sdshow" $ServiceName 2>$null)
  if ([int]$LASTEXITCODE -ne 0) {
    throw [InvalidOperationException]::new()
  }
  $candidates = @(
    $output |
      ForEach-Object { ([string]$_).Trim() } |
      Where-Object { $_ -match '^(?:O:|G:|D:|S:)' }
  )
  if ($candidates.Count -ne 1) {
    throw [IO.InvalidDataException]::new()
  }
  $descriptor = [Security.AccessControl.RawSecurityDescriptor]::new(
    [string]$candidates[0])
  return $descriptor.GetSddlForm(
    [Security.AccessControl.AccessControlSections]::All)
}

function Get-ScStartMode {
  param([string] $StartMode)
  switch ($StartMode) {
    "Auto" { return "auto" }
    "Manual" { return "demand" }
    "Disabled" { return "disabled" }
    default { throw [IO.InvalidDataException]::new() }
  }
}

function Get-ScErrorControl {
  param([string] $ErrorControl)
  switch ($ErrorControl) {
    "Ignore" { return "ignore" }
    "Normal" { return "normal" }
    "Severe" { return "severe" }
    "Critical" { return "critical" }
    default { throw [IO.InvalidDataException]::new() }
  }
}

function New-PreRemoveTransactionState {
  $nonce = [Guid]::NewGuid().ToString("D")
  $service = Get-ServiceRecord
  $servicePresent = $null -ne $service
  $snapshot = [ordered]@{
    present = [bool]$servicePresent
    name = $ServiceName
  }
  if ($servicePresent) {
    Assert-ServiceRecord $service
    if (@("Stopped", "Running") -notcontains [string]$service.State) {
      Fail-Code "BOOTSTRAP.SERVICE_START_FORBIDDEN"
    }
    $serviceKey = Get-ServiceRegistryKey
    $registryKey = Get-Item -LiteralPath $serviceKey -Force -ErrorAction Stop
    $delayedAutoStartPresent = @(
      $registryKey.GetValueNames()
    ) -contains "DelayedAutoStart"
    $delayedAutoStart = if ($delayedAutoStartPresent) {
      [int](Get-ItemPropertyValue `
        -LiteralPath $serviceKey `
        -Name "DelayedAutoStart" `
        -ErrorAction Stop)
    } else {
      0
    }
    $snapshot = [ordered]@{
      present = $true
      name = [string]$service.Name
      display_name = [string]$service.DisplayName
      description = [string](Get-ServiceDescription)
      path_name = [string]$service.PathName
      start_name = [string]$service.StartName
      start_mode = [string]$service.StartMode
      state = [string]$service.State
      service_type = [string]$service.ServiceType
      error_control = [string]$service.ErrorControl
      service_sid_type = [int](
        Get-ItemPropertyValue `
          -LiteralPath $serviceKey `
          -Name "ServiceSidType" `
          -ErrorAction Stop)
      delayed_auto_start_present = [bool]$delayedAutoStartPresent
      delayed_auto_start = [int]$delayedAutoStart
      service_security_sddl = [string](Get-ServiceSecuritySddl)
      registry_security_sddl = [string](
        (Get-Acl -LiteralPath $serviceKey -ErrorAction Stop).Sddl)
    }
  }
  return [ordered]@{
    schema_id = $TransactionSchema
    operation = "pre_remove"
    nonce = $nonce
    package_version = $PackageVersion
    package_format = $PackageFormat
    install_root = $InstallRoot
    state_root = $StateRoot
    preservation = [ordered]@{
      database_state = "preserve_never_delete"
      configuration = "preserve_never_delete"
      state_directory_acls = "preserve_never_delete"
      filesystem_operations_group = "preserve_never_delete"
      service_snapshot = "restore_only_when_exact_service_remains_absent"
    }
    service = [ordered]@{
      snapshot = $snapshot
      stop_intent = [bool](
        $servicePresent -and
        [string]::Equals(
          [string]$service.State,
          "Running",
          [StringComparison]::OrdinalIgnoreCase))
      stop_completed = [bool](
        -not $servicePresent -or
        [string]::Equals(
          [string]$service.State,
          "Stopped",
          [StringComparison]::OrdinalIgnoreCase))
      delete_intent = [bool]$servicePresent
      delete_completed = [bool](-not $servicePresent)
    }
  }
}

function Restore-PreRemoveService {
  param($Snapshot)
  if (-not [bool]$Snapshot.present) {
    return
  }
  if ($null -ne (Get-ServiceRecord)) {
    # A still-present or concurrently recreated service is not ours to alter.
    return
  }
  if (-not [string]::Equals(
      [string]$Snapshot.name,
      $ServiceName,
      [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        [string]$Snapshot.path_name,
        (Get-ExpectedServiceCommand),
        [StringComparison]::OrdinalIgnoreCase) -or
      -not [string]::Equals(
        [string]$Snapshot.start_name,
        $ServiceAccount,
        [StringComparison]::OrdinalIgnoreCase) -or
      -not [string]::Equals(
        [string]$Snapshot.service_type,
        "Own Process",
        [StringComparison]::OrdinalIgnoreCase) -or
      [int]$Snapshot.service_sid_type -ne 3 -or
      @("Stopped", "Running") -notcontains [string]$Snapshot.state) {
    throw [IO.InvalidDataException]::new()
  }

  $sc = Join-Path $env:SystemRoot "System32\sc.exe"
  $startMode = Get-ScStartMode ([string]$Snapshot.start_mode)
  $errorControl = Get-ScErrorControl ([string]$Snapshot.error_control)
  Invoke-NativeQuiet $sc @(
    "create",
    $ServiceName,
    "type= own",
    "start= $startMode",
    "error= $errorControl",
    "binPath= $([string]$Snapshot.path_name)",
    "obj= $ServiceAccount",
    "password= ",
    "DisplayName= $([string]$Snapshot.display_name)"
  ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  Invoke-NativeQuiet $sc @(
    "description",
    $ServiceName,
    [string]$Snapshot.description
  ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  Invoke-NativeQuiet $sc @(
    "sidtype",
    $ServiceName,
    "restricted"
  ) "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"
  Invoke-NativeQuiet $sc @(
    "sdset",
    $ServiceName,
    [string]$Snapshot.service_security_sddl
  ) "BOOTSTRAP.DIRECTORY_PERMISSION_INVALID"

  $serviceKey = Get-ServiceRegistryKey
  if ([bool]$Snapshot.delayed_auto_start_present) {
    New-ItemProperty `
      -LiteralPath $serviceKey `
      -Name "DelayedAutoStart" `
      -Value ([int]$Snapshot.delayed_auto_start) `
      -PropertyType DWord `
      -Force `
      -ErrorAction Stop |
        Out-Null
  } else {
    Remove-ItemProperty `
      -LiteralPath $serviceKey `
      -Name "DelayedAutoStart" `
      -Force `
      -ErrorAction SilentlyContinue
  }
  $registryAcl = [Security.AccessControl.RegistrySecurity]::new()
  $registryAcl.SetSecurityDescriptorSddlForm(
    [string]$Snapshot.registry_security_sddl,
    [Security.AccessControl.AccessControlSections]::All)
  Set-Acl `
    -LiteralPath $serviceKey `
    -AclObject $registryAcl `
    -ErrorAction Stop

  if ([string]::Equals(
      [string]$Snapshot.state,
      "Running",
      [StringComparison]::OrdinalIgnoreCase)) {
    Invoke-NativeQuiet $sc @(
      "start",
      $ServiceName
    ) "BOOTSTRAP.SERVICE_START_FORBIDDEN"
  }
  $restored = Get-ServiceRecord
  if ($null -eq $restored) {
    throw [InvalidOperationException]::new()
  }
  Assert-ServiceRecord $restored
  if (-not [string]::Equals(
      [string]$restored.DisplayName,
      [string]$Snapshot.display_name,
      [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        (Get-ServiceDescription),
        [string]$Snapshot.description,
        [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        [string]$restored.StartMode,
        [string]$Snapshot.start_mode,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw [IO.InvalidDataException]::new()
  }
  for ($attempt = 0; $attempt -lt 40; $attempt++) {
    $restored = Get-ServiceRecord
    if ($null -ne $restored -and
        [string]::Equals(
          [string]$restored.State,
          [string]$Snapshot.state,
          [StringComparison]::OrdinalIgnoreCase)) {
      break
    }
    Start-Sleep -Milliseconds 250
  }
  if ($null -eq $restored -or
      -not [string]::Equals(
        [string]$restored.State,
        [string]$Snapshot.state,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw [IO.InvalidDataException]::new()
  }
  $restoredServiceSddl = Get-ServiceSecuritySddl
  $restoredRegistrySddl = (
    Get-Acl -LiteralPath $serviceKey -ErrorAction Stop
  ).Sddl
  if (-not [string]::Equals(
      $restoredServiceSddl,
      [string]$Snapshot.service_security_sddl,
      [StringComparison]::Ordinal) -or
      -not [string]::Equals(
        $restoredRegistrySddl,
        [string]$Snapshot.registry_security_sddl,
        [StringComparison]::Ordinal)) {
    throw [UnauthorizedAccessException]::new()
  }
}

function ConvertTo-NormalizedSddl {
  param([string] $Sddl)
  if ([string]::IsNullOrWhiteSpace($Sddl)) {
    throw [IO.InvalidDataException]::new()
  }
  $descriptor = [Security.AccessControl.RawSecurityDescriptor]::new($Sddl)
  return $descriptor.GetSddlForm(
    [Security.AccessControl.AccessControlSections]::All)
}

function Test-ServiceMatchesSnapshot {
  param($Snapshot, [switch] $IgnoreRuntimeState)
  $service = Get-ServiceRecord
  if (-not [bool]$Snapshot.present) {
    return $null -eq $service
  }
  if ($null -eq $service) {
    return $false
  }
  try {
    if (-not [string]::Equals(
        [string]$service.Name,
        [string]$Snapshot.name,
        [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          [string]$service.DisplayName,
          [string]$Snapshot.display_name,
          [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          (Get-ServiceDescription),
          [string]$Snapshot.description,
          [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          [string]$service.PathName,
          [string]$Snapshot.path_name,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.StartName,
          [string]$Snapshot.start_name,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.StartMode,
          [string]$Snapshot.start_mode,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.ServiceType,
          [string]$Snapshot.service_type,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.ErrorControl,
          [string]$Snapshot.error_control,
          [StringComparison]::OrdinalIgnoreCase)) {
      return $false
    }
    if (-not $IgnoreRuntimeState -and
        -not [string]::Equals(
          [string]$service.State,
          [string]$Snapshot.state,
          [StringComparison]::OrdinalIgnoreCase)) {
      return $false
    }
    $serviceKey = Get-ServiceRegistryKey
    $sidType = Get-ItemPropertyValue `
      -LiteralPath $serviceKey `
      -Name "ServiceSidType" `
      -ErrorAction Stop
    if ([int]$sidType -ne [int]$Snapshot.service_sid_type) {
      return $false
    }
    $registryKey = Get-Item -LiteralPath $serviceKey -Force -ErrorAction Stop
    $delayedPresent = @(
      $registryKey.GetValueNames()
    ) -contains "DelayedAutoStart"
    if ($delayedPresent -ne [bool]$Snapshot.delayed_auto_start_present) {
      return $false
    }
    if ($delayedPresent) {
      $delayedValue = Get-ItemPropertyValue `
        -LiteralPath $serviceKey `
        -Name "DelayedAutoStart" `
        -ErrorAction Stop
      if ([int]$delayedValue -ne [int]$Snapshot.delayed_auto_start) {
        return $false
      }
    }
    $serviceSddl = ConvertTo-NormalizedSddl (Get-ServiceSecuritySddl)
    $snapshotServiceSddl = ConvertTo-NormalizedSddl (
      [string]$Snapshot.service_security_sddl)
    $registrySddl = ConvertTo-NormalizedSddl (
      (Get-Acl -LiteralPath $serviceKey -ErrorAction Stop).Sddl)
    $snapshotRegistrySddl = ConvertTo-NormalizedSddl (
      [string]$Snapshot.registry_security_sddl)
    return (
      [string]::Equals(
        $serviceSddl,
        $snapshotServiceSddl,
        [StringComparison]::Ordinal) -and
      [string]::Equals(
        $registrySddl,
        $snapshotRegistrySddl,
        [StringComparison]::Ordinal)
    )
  } catch {
    return $false
  }
}

function Get-ExactEmptyScratchBirdGroup {
  try {
    $rows = @(
      Get-CimInstance `
        -ClassName Win32_Group `
        -Filter "Name='ScratchBird' AND LocalAccount=TRUE" `
        -ErrorAction Stop
    )
    if ($rows.Count -ne 1) {
      return $null
    }
    $row = $rows[0]
    if (-not [string]::Equals(
        [string]$row.Domain,
        $env:COMPUTERNAME,
        [StringComparison]::OrdinalIgnoreCase) -or
        -not $row.LocalAccount -or
        [int]$row.SIDType -ne 4 -or
        [string]$row.SID -notmatch '^S-1-5-21-(\d+-){3}\d+$') {
      return $null
    }
    $conflictingUsers = @(
      Get-CimInstance `
        -ClassName Win32_UserAccount `
        -Filter "Name='scratchbird' AND LocalAccount=TRUE" `
        -ErrorAction Stop
    )
    if ($conflictingUsers.Count -ne 0) {
      return $null
    }
    $adsiGroup = [ADSI](
      "WinNT://{0}/{1},group" -f $row.Domain, $row.Name)
    $members = @(
      $adsiGroup.PSBase.Invoke("Members") |
        Where-Object { $null -ne $_ }
    )
    if ($members.Count -ne 0) {
      return $null
    }
    return $row
  } catch {
    return $null
  }
}

function Test-ExactServiceCore {
  param($Service)
  if ($null -eq $Service) {
    return $false
  }
  try {
    if (-not [string]::Equals(
        [string]$Service.Name,
        $ServiceName,
        [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          [string]$Service.StartName,
          $ServiceAccount,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.PathName,
          (Get-ExpectedServiceCommand),
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.ServiceType,
          "Own Process",
          [StringComparison]::OrdinalIgnoreCase)) {
      return $false
    }
    $sidType = Get-ItemPropertyValue `
      -LiteralPath (Get-ServiceRegistryKey) `
      -Name "ServiceSidType" `
      -ErrorAction Stop
    return [int]$sidType -eq 3
  } catch {
    return $false
  }
}

function Test-PostInstallOwnedService {
  param($Service, $TransactionState)
  if ($null -eq $Service) {
    return $false
  }
  try {
    $nonce = [string]$TransactionState.nonce
    $displayNameIsTemporary = [string]::Equals(
      [string]$Service.DisplayName,
      (Get-PostInstallServiceDisplayName $nonce),
      [StringComparison]::Ordinal)
    $displayNameIsProvenFinal = (
      [bool]$TransactionState.service.display_name_normalize_intent -and
      [string]::Equals(
        [string]$Service.DisplayName,
        $ServiceDisplayName,
        [StringComparison]::Ordinal)
    )
    if (-not [string]::Equals(
        [string]$Service.Name,
        $ServiceName,
        [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          [string]$Service.StartName,
          $ServiceAccount,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.PathName,
          (Get-ExpectedServiceCommand),
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.ServiceType,
          "Own Process",
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.StartMode,
          "Manual",
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$Service.State,
          "Stopped",
          [StringComparison]::OrdinalIgnoreCase) -or
        (-not $displayNameIsTemporary -and
          -not $displayNameIsProvenFinal)) {
      return $false
    }
    $description = Get-ServiceDescription
    $descriptionIsTemporary = (
      [string]::Equals(
        $description,
        "",
        [StringComparison]::Ordinal) -or
      [string]::Equals(
        $description,
        (Get-PostInstallServiceDescription $nonce),
        [StringComparison]::Ordinal)
    )
    $descriptionIsProvenFinal = (
      [bool]$TransactionState.service.description_normalize_intent -and
      [string]::Equals(
        $description,
        $ServiceDescription,
        [StringComparison]::Ordinal)
    )
    return $descriptionIsTemporary -or $descriptionIsProvenFinal
  } catch {
    return $false
  }
}

function Invoke-RollbackPostInstall {
  $transaction = Read-TransactionState `
    -ExpectedOperation "post_install" `
    -AllowMissing
  if ($null -eq $transaction) {
    return
  }
  if ([bool]$transaction.service.create_intent -and
      -not [bool]$transaction.service.preexisting) {
    $service = Get-ServiceRecord
    if ($null -ne $service) {
      if (-not (Test-PostInstallOwnedService `
          $service `
          $transaction)) {
        throw [IO.InvalidDataException]::new()
      }
      $sc = Join-Path $env:SystemRoot "System32\sc.exe"
      & $sc "delete" $ServiceName 1>$null 2>$null
      if ([int]$LASTEXITCODE -ne 0) {
        throw [InvalidOperationException]::new()
      }
      for ($attempt = 0; $attempt -lt 60; $attempt++) {
        if ($null -eq (Get-ServiceRecord)) {
          break
        }
        Start-Sleep -Milliseconds 250
      }
      if ($null -ne (Get-ServiceRecord)) {
        throw [InvalidOperationException]::new()
      }
    }
  }

  if ([bool]$transaction.group.create_intent -and
      -not [bool]$transaction.group.preexisting) {
    $groupRows = @(
      Get-CimInstance `
        -ClassName Win32_Group `
        -Filter "Name='ScratchBird' AND LocalAccount=TRUE" `
        -ErrorAction Stop
    )
    if ($groupRows.Count -gt 0) {
      $group = Get-ExactEmptyScratchBirdGroup
      if ($null -eq $group) {
        throw [IO.InvalidDataException]::new()
      }
      $groupComment = Get-LocalScratchBirdGroupComment $group
      $groupCommentIsTemporary = [string]::Equals(
        $groupComment,
        (Get-PostInstallGroupComment (
          [string]$transaction.nonce)),
        [StringComparison]::Ordinal)
      $groupCommentIsProvenFinal = (
        [bool]$transaction.group.comment_normalize_intent -and
        [string]::Equals(
          $groupComment,
          $GroupDescription,
          [StringComparison]::Ordinal)
      )
      if (-not $groupCommentIsTemporary -and
          -not $groupCommentIsProvenFinal) {
        throw [IO.InvalidDataException]::new()
      }
      $net = Get-SystemNetExecutable
      & $net "localgroup" $GroupName "/delete" 1>$null 2>$null
      if ([int]$LASTEXITCODE -ne 0) {
        throw [InvalidOperationException]::new()
      }
      $remainingGroups = @()
      for ($attempt = 0; $attempt -lt 20; $attempt++) {
        $remainingGroups = @(
          Get-CimInstance `
            -ClassName Win32_Group `
            -Filter "Name='ScratchBird' AND LocalAccount=TRUE" `
            -ErrorAction Stop
        )
        if ($remainingGroups.Count -eq 0) {
          break
        }
        Start-Sleep -Milliseconds 250
      }
      if ($remainingGroups.Count -ne 0) {
        throw [InvalidOperationException]::new()
      }
    }
  }
  # Database state, configuration, and directory ACLs are intentionally
  # preserved. Clear only after exact nonce-owned identity cleanup succeeds.
  Remove-TransactionState $transaction
}

function Invoke-CommitPostInstall {
  $transaction = Read-TransactionState `
    -ExpectedOperation "post_install" `
    -AllowMissing
  if ($null -eq $transaction) {
    return
  }

  $group = Get-LocalScratchBirdGroup
  if ([bool]$transaction.group.preexisting) {
    if (-not [string]::Equals(
        [string]$group.SID,
        [string]$transaction.group.sid_before,
        [StringComparison]::Ordinal) -or
        -not [string]::Equals(
        (Get-LocalScratchBirdGroupComment $group),
        [string]$transaction.group.comment_before,
        [StringComparison]::Ordinal)) {
      throw [IO.InvalidDataException]::new()
    }
  } else {
    if (-not [bool]$transaction.group.create_completed) {
      throw [IO.InvalidDataException]::new()
    }
    $currentComment = Get-LocalScratchBirdGroupComment $group
    $transactionComment = Get-PostInstallGroupComment (
      [string]$transaction.nonce)
    if (-not [bool]$transaction.group.comment_normalized) {
      if ([string]::Equals(
          $currentComment,
          $transactionComment,
          [StringComparison]::Ordinal)) {
        $transaction.group.comment_normalize_intent = $true
        Write-TransactionState $transaction
        Set-LocalScratchBirdGroupComment $GroupDescription
      } elseif (-not (
          [bool]$transaction.group.comment_normalize_intent -and
          [string]::Equals(
            $currentComment,
            $GroupDescription,
            [StringComparison]::Ordinal))) {
        throw [IO.InvalidDataException]::new()
      }
      $group = Get-LocalScratchBirdGroup
      if (-not [string]::Equals(
          (Get-LocalScratchBirdGroupComment $group),
          $GroupDescription,
          [StringComparison]::Ordinal)) {
        throw [IO.InvalidDataException]::new()
      }
      $transaction.group.comment_normalized = $true
      Write-TransactionState $transaction
    } elseif (-not [string]::Equals(
        $currentComment,
        $GroupDescription,
        [StringComparison]::Ordinal)) {
      throw [IO.InvalidDataException]::new()
    }
  }

  $service = Get-ServiceRecord
  if (-not (Test-ExactServiceCore $service)) {
    throw [IO.InvalidDataException]::new()
  }
  if ([bool]$transaction.service.preexisting) {
    $currentServiceSecurity = Get-ServiceSecuritySddl
    $currentRegistryAcl = (
      Get-Acl -LiteralPath (Get-ServiceRegistryKey) -ErrorAction Stop
    ).Sddl
    if (-not [string]::Equals(
        [string]$service.DisplayName,
        [string]$transaction.service.display_name_before,
        [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          (Get-ServiceDescription),
          [string]$transaction.service.description_before,
          [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          [string]$service.StartMode,
          [string]$transaction.service.start_mode_before,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.State,
          [string]$transaction.service.state_before,
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          (ConvertTo-NormalizedSddl $currentServiceSecurity),
          (ConvertTo-NormalizedSddl (
            [string]$transaction.service.service_security_before)),
          [StringComparison]::Ordinal) -or
        -not [string]::Equals(
          $currentRegistryAcl,
          [string]$transaction.service.registry_acl_before,
          [StringComparison]::Ordinal)) {
      throw [IO.InvalidDataException]::new()
    }
  } else {
    if (-not [bool]$transaction.service.create_completed -or
        -not [bool]$transaction.service.sid_type_completed -or
        -not [bool]$transaction.service.registry_acl_completed -or
        -not [string]::Equals(
          [string]$service.StartMode,
          "Manual",
          [StringComparison]::OrdinalIgnoreCase) -or
        -not [string]::Equals(
          [string]$service.State,
          "Stopped",
          [StringComparison]::OrdinalIgnoreCase)) {
      throw [IO.InvalidDataException]::new()
    }
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    $temporaryDisplayName = Get-PostInstallServiceDisplayName (
      [string]$transaction.nonce)
    if (-not [bool]$transaction.service.display_name_normalized) {
      if ([string]::Equals(
          [string]$service.DisplayName,
          $temporaryDisplayName,
          [StringComparison]::Ordinal)) {
        $transaction.service.display_name_normalize_intent = $true
        Write-TransactionState $transaction
        Invoke-NativeQuiet $sc @(
          "config",
          $ServiceName,
          "DisplayName= $ServiceDisplayName"
        ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
      } elseif (-not (
          [bool]$transaction.service.display_name_normalize_intent -and
          [string]::Equals(
            [string]$service.DisplayName,
            $ServiceDisplayName,
            [StringComparison]::Ordinal))) {
        throw [IO.InvalidDataException]::new()
      }
      $service = Get-ServiceRecord
      if ($null -eq $service -or
          -not [string]::Equals(
            [string]$service.DisplayName,
            $ServiceDisplayName,
            [StringComparison]::Ordinal)) {
        throw [IO.InvalidDataException]::new()
      }
      $transaction.service.display_name_normalized = $true
      Write-TransactionState $transaction
    } elseif (-not [string]::Equals(
        [string]$service.DisplayName,
        $ServiceDisplayName,
        [StringComparison]::Ordinal)) {
      throw [IO.InvalidDataException]::new()
    }

    $temporaryDescription = Get-PostInstallServiceDescription (
      [string]$transaction.nonce)
    $currentDescription = Get-ServiceDescription
    if (-not [bool]$transaction.service.description_normalized) {
      if ([string]::Equals(
          $currentDescription,
          $temporaryDescription,
          [StringComparison]::Ordinal)) {
        $transaction.service.description_normalize_intent = $true
        Write-TransactionState $transaction
        Invoke-NativeQuiet $sc @(
          "description",
          $ServiceName,
          $ServiceDescription
        ) "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
      } elseif (-not (
          [bool]$transaction.service.description_normalize_intent -and
          [string]::Equals(
            $currentDescription,
            $ServiceDescription,
            [StringComparison]::Ordinal))) {
        throw [IO.InvalidDataException]::new()
      }
      if (-not [string]::Equals(
          (Get-ServiceDescription),
          $ServiceDescription,
          [StringComparison]::Ordinal)) {
        throw [IO.InvalidDataException]::new()
      }
      $transaction.service.description_normalized = $true
      Write-TransactionState $transaction
    } elseif (-not [string]::Equals(
        $currentDescription,
        $ServiceDescription,
        [StringComparison]::Ordinal)) {
      throw [IO.InvalidDataException]::new()
    }
  }
  # This action runs before InstallFinalize. Retain the checked journal as
  # rollback authority until the MSI transaction commits; the external commit
  # cleanup custom action deletes the fixed journal only after InstallFinalize.
  $transaction.commit_completed = $true
  Write-TransactionState $transaction
}

function Invoke-PreRemove {
  $transaction = New-PreRemoveTransactionState
  Initialize-TransactionState $transaction
  if ([bool]$transaction.service.snapshot.present) {
    Remove-SBsrvService $transaction
  }
}

function Invoke-RollbackPreRemove {
  $transaction = Read-TransactionState `
    -ExpectedOperation "pre_remove" `
    -AllowMissing
  if ($null -eq $transaction) {
    return
  }
  $snapshot = $transaction.service.snapshot
  $service = Get-ServiceRecord
  if ([bool]$snapshot.present -and $null -eq $service) {
    Restore-PreRemoveService $transaction.service.snapshot
  } elseif ([bool]$snapshot.present -and
      $null -ne $service -and
      [string]::Equals(
        [string]$snapshot.state,
        "Running",
        [StringComparison]::OrdinalIgnoreCase) -and
      [string]::Equals(
        [string]$service.State,
        "Stopped",
        [StringComparison]::OrdinalIgnoreCase) -and
      (Test-ServiceMatchesSnapshot `
        $snapshot `
        -IgnoreRuntimeState)) {
    $sc = Join-Path $env:SystemRoot "System32\sc.exe"
    Invoke-NativeQuiet $sc @(
      "start",
      $ServiceName
    ) "BOOTSTRAP.SERVICE_START_FORBIDDEN"
    for ($attempt = 0; $attempt -lt 40; $attempt++) {
      $service = Get-ServiceRecord
      if ($null -ne $service -and
          [string]::Equals(
            [string]$service.State,
            "Running",
            [StringComparison]::OrdinalIgnoreCase)) {
        break
      }
      Start-Sleep -Milliseconds 250
    }
  }
  if (-not (Test-ServiceMatchesSnapshot $snapshot)) {
    # Preserve the journal and any still-present/concurrently recreated
    # service when the exact pre-remove snapshot cannot be proven.
    throw [IO.InvalidDataException]::new()
  }
  # State, configuration, directory ACLs, and the empty local group survive
  # both uninstall and uninstall rollback. Clear only after exact restoration.
  Remove-TransactionState $transaction
}

try {
  if ($AllowedActions -notcontains $Action) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
  }
  if (-not (Test-Administrator)) {
    Fail-Code "BOOTSTRAP.OS_AUTHORITY_DENIED"
  }
  if (-not [Environment]::Is64BitProcess) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID" 2
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
  $LifecyclePhase = "PATH_VALIDATION"
  Assert-SystemPaths
  Assert-NotReparsePoint $InstallRoot
  if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
    Fail-Code "BOOTSTRAP.INSTALL_DEFAULTS_INVALID"
  }

  if ($Action -eq "RollbackPostInstall") {
    $LifecyclePhase = "ROLLBACK_POST_INSTALL"
    Invoke-RollbackPostInstall
    exit 0
  }
  if ($Action -eq "CommitPostInstall") {
    $LifecyclePhase = "COMMIT_POST_INSTALL"
    Invoke-CommitPostInstall
    exit 0
  }
  if ($Action -eq "RollbackPreRemove") {
    $LifecyclePhase = "ROLLBACK_PRE_REMOVE"
    Invoke-RollbackPreRemove
    exit 0
  }
  if ($Action -eq "PreRemove") {
    Assert-NotReparsePoint $StateRoot
    $LifecyclePhase = "DATABASE_INVENTORY_BEFORE"
    $databaseArtifactsBefore = @(Get-InstallerDatabaseArtifactInventory)
    $LifecyclePhase = "PRE_REMOVE_JOURNAL_AND_SERVICE"
    Invoke-PreRemove
    $LifecyclePhase = "DATABASE_INVENTORY_AFTER"
    Assert-DatabaseArtifactInventoryUnchanged $databaseArtifactsBefore
    exit 0
  }

  $transaction = $null
  if ($Action -eq "PostInstall") {
    $LifecyclePhase = "POST_INSTALL_JOURNAL_INVENTORY"
    $transaction = New-PostInstallTransactionState
    $LifecyclePhase = "POST_INSTALL_JOURNAL_CREATE"
    Initialize-TransactionState $transaction
  }
  Ensure-Directory $StateRoot
  $LifecyclePhase = "DATABASE_INVENTORY_BEFORE"
  $databaseArtifactsBefore = @(Get-InstallerDatabaseArtifactInventory)

  $LifecyclePhase = "GROUP_IDENTITY"
  $group = if ($Action -eq "PostInstall") {
    Ensure-LocalScratchBirdGroup $transaction
  } else {
    Get-LocalScratchBirdGroup
  }
  $LifecyclePhase = "SERVICE_IDENTITY"
  $serviceState = if ($Action -eq "PostInstall") {
    Ensure-SBsrvService $transaction
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
  $groupSid = [Security.Principal.SecurityIdentifier]::new([string]$group.SID)
  $LifecyclePhase = "RUNTIME_ACL"
  Grant-ServiceRuntimeReadExecute $serviceSid
  if ($Action -eq "PostInstall" -and
      -not [bool]$transaction.service.preexisting) {
    $LifecyclePhase = "SERVICE_REGISTRY_ACL"
    Set-ProtectedServiceRegistryAcl $serviceSid
    $transaction.service.registry_acl_completed = $true
    Write-TransactionState $transaction
  }

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
    $LifecyclePhase = "CONFIGURATION_DEFAULTS"
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
  [Console]::Error.WriteLine(
    "BOOTSTRAP.INSTALL_DEFAULTS_INVALID.$LifecyclePhase"
  )
  exit 1
}
