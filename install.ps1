<#
.SYNOPSIS
  Hard Drive Monitor installer for Windows. Installs the `hdmon` command and puts it on PATH
  so it works in any new Command Prompt or PowerShell window.

.PARAMETER NoService
  Do not schedule the background check (by default a Task Scheduler task runs `hdmon check`
  at logon and every hour; with highest privileges when this script runs elevated).

.PARAMETER System
  Install to %ProgramFiles%\hard-drive-monitor\bin and add it to the MACHINE Path (all users)
  instead of %LOCALAPPDATA%\hard-drive-monitor\bin + the user Path. Needs an elevated PowerShell.

.PARAMETER Binary
  Path to a hdmon.exe to install. Default: hdmon.exe next to this script, then
  dist\hdmon-windows-x86_64.exe, then build\(Release\)hdmon.exe; if none exists the script builds
  from source with CMake (needs cmake and Visual Studio Build Tools or MinGW-w64).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install.ps1
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -System
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoService
#>
[CmdletBinding()]
param([switch]$System, [switch]$NoService, [string]$Binary = '')

# Native tools (cmake, schtasks) may write warnings to stderr; with 'Stop' Windows PowerShell 5.1 would turn those
# into terminating errors. Failures are checked explicitly via $LASTEXITCODE instead.
$ErrorActionPreference = 'Continue'
$Src = $PSScriptRoot
$StateDir = Join-Path $env:LOCALAPPDATA 'hard-drive-monitor'
$StateFile = Join-Path $StateDir 'install-state.json'

function Say($msg) { Write-Host "==> $msg" }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Send-EnvironmentChange {
    # Tell Explorer and other top-level windows that the environment changed (WM_SETTINGCHANGE).
    if (-not ('HdmonNative.Win32' -as [type])) {
        Add-Type -Namespace HdmonNative -Name Win32 -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
    }
    $result = [UIntPtr]::Zero
    [void][HdmonNative.Win32]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
}

function Normalize([string]$p) { return $p.Trim().TrimEnd('\').ToLowerInvariant() }

function Add-ToPath([string]$dir, [string]$scope) {
    $current = [Environment]::GetEnvironmentVariable('Path', $scope)
    if ($null -eq $current) { $current = '' }
    $parts = @($current -split ';' | Where-Object { $_ -ne '' })
    if ($parts | Where-Object { (Normalize $_) -eq (Normalize $dir) }) {
        Say "$dir is already on the $scope Path"
        return $false
    }
    $new = (@($parts) + $dir) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $new, $scope)
    Say "Added $dir to the $scope Path"
    return $true
}

if ($System -and -not (Test-Admin)) {
    throw "-System changes the Machine Path and needs an elevated PowerShell (Run as Administrator)."
}
$Scope = if ($System) { 'Machine' } else { 'User' }

$BinDir = if ($System) { Join-Path $env:ProgramFiles 'hard-drive-monitor\bin' } else { Join-Path $StateDir 'bin' }

# --- find a prebuilt binary (or build one)
function Find-Pair([string]$exe) {
    # hdmonw.exe (no console window, used by the scheduled task) sits next to hdmon.exe.
    $dir = Split-Path -Parent $exe
    $name = Split-Path -Leaf $exe
    $w = Join-Path $dir ($name -replace '^hdmon', 'hdmonw')
    if (Test-Path $w) { return $w } else { return $null }
}
$Candidates = @()
if ($Binary) { $Candidates += $Binary }
$Candidates += @((Join-Path $Src 'hdmon.exe'), (Join-Path $Src 'dist\hdmon-windows-x86_64.exe'),
                 (Join-Path $Src 'build\Release\hdmon.exe'), (Join-Path $Src 'build\hdmon.exe'))
$Exe = $null
foreach ($c in $Candidates) {
    if ($c -and (Test-Path $c)) {
        $null = & $c --version 2>$null
        if ($LASTEXITCODE -eq 0) { $Exe = (Resolve-Path $c).Path; break }
        if ($c -eq $Binary) { throw "-Binary $Binary does not run on this machine" }
    }
}
$Method = 'binary'
$BuildDir = $null
if (-not $Exe) {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw ('No prebuilt hdmon.exe found and cmake is not installed. Download a release binary, or install ' +
               'CMake + Visual Studio Build Tools (winget install Kitware.CMake Microsoft.VisualStudio.2022.BuildTools).')
    }
    $BuildDir = Join-Path ([IO.Path]::GetTempPath()) ("hdmon-build-" + [guid]::NewGuid().ToString('N'))
    Say "Building hdmon from source with CMake (in $BuildDir)"
    & cmake -S $Src -B $BuildDir -DHDMON_BUILD_TESTS=OFF | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }
    & cmake --build $BuildDir --config Release --parallel | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }
    $Exe = @((Join-Path $BuildDir 'Release\hdmon.exe'), (Join-Path $BuildDir 'hdmon.exe')) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Exe) { throw 'build finished but hdmon.exe was not found' }
    $Method = 'built'
} else {
    Say "Using prebuilt binary $Exe"
}

# --- copy into the bin dir (a running copy, e.g. the scheduled task, is renamed out of the way first)
New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
function Install-File([string]$from, [string]$to) {
    if (Test-Path $to) {
        try { Remove-Item $to -Force -ErrorAction Stop }
        catch { Move-Item $to "$to.old" -Force -ErrorAction SilentlyContinue }
    }
    Copy-Item $from $to -Force
}
Install-File $Exe (Join-Path $BinDir 'hdmon.exe')
$W = Find-Pair $Exe
if ($W) { Install-File $W (Join-Path $BinDir 'hdmonw.exe') }
if ($BuildDir) { Remove-Item $BuildDir -Recurse -Force -ErrorAction SilentlyContinue }
$Shim = $null

# --- PATH
$Added = Add-ToPath $BinDir $Scope
if (-not (($env:Path -split ';') | Where-Object { $_ -and (Normalize $_) -eq (Normalize $BinDir) })) {
    $env:Path = "$env:Path;$BinDir"     # this window too
}
Send-EnvironmentChange

New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
$prev = $null
if (Test-Path $StateFile) { $prev = Get-Content $StateFile -Raw | ConvertFrom-Json }
$state = [pscustomobject]@{
    method    = $Method
    bin_dir   = $BinDir
    scope     = $Scope
    path_added = [bool]($Added -or ($prev -and $prev.path_added -and $prev.bin_dir -eq $BinDir -and $prev.scope -eq $Scope))
    shim      = $Shim
}
$state | ConvertTo-Json | Set-Content -Path $StateFile -Encoding UTF8

$exe = Join-Path $BinDir 'hdmon.exe'
$ver = & $exe --version
Say "Installed: $ver ($Method, $exe)"
$IsAdmin = Test-Admin
if (-not (Get-Command smartctl -ErrorAction SilentlyContinue) -and
    -not (Test-Path "$env:ProgramFiles\smartmontools\bin\smartctl.exe")) {
    Say 'smartmontools is not installed yet: winget install smartmontools.smartmontools  (or: choco install smartmontools)'
    Say 'Until then Hard Drive Monitor uses the Windows Storage counters (fewer attributes).'
}

# --- background check (on by default): Task Scheduler, at logon + hourly
if ($NoService) {
    Say 'Skipped scheduling (-NoService). Turn it on later with: hdmon install-service'
} else {
    Say 'Scheduling the background check (at logon + hourly)'
    & $exe install-service
    if ($LASTEXITCODE -eq 0) {
        $state | Add-Member -NotePropertyName service -NotePropertyValue $true -Force
        $state | ConvertTo-Json | Set-Content -Path $StateFile -Encoding UTF8
        if (-not $IsAdmin) {
            Write-Warning ('This window is not elevated, so the task runs without admin rights and smartctl cannot ' +
                           'open the drives. For full SMART data re-run install.ps1 (or hdmon install-service) ' +
                           'from an elevated PowerShell (Run as Administrator).')
        }
    } else {
        Write-Warning 'Could not create the scheduled task (see the message above).'
    }
}

Write-Host ''
Write-Host 'Open a NEW Command Prompt or PowerShell window, then run:'
Write-Host '  hdmon status'
Write-Host 'Use an elevated (Run as Administrator) window so smartctl can read the drives.'
Write-Host 'Turn the background check off with:  hdmon uninstall-service'
Write-Host 'Remove everything with:              powershell -ExecutionPolicy Bypass -File .\uninstall.ps1'
