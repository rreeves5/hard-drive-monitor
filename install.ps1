<#
.SYNOPSIS
  Hard Drive Monitor installer for Windows. Installs the `hdmon` command and puts it on PATH
  so it works in any new Command Prompt or PowerShell window.

.PARAMETER NoService
  Do not schedule the background check (by default a Task Scheduler task runs `hdmon check`
  at logon and every hour; with highest privileges when this script runs elevated).

.PARAMETER System
  Add the scripts folder to the MACHINE Path (all users) instead of the user Path. Needs an
  elevated (Run as Administrator) PowerShell.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install.ps1
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -System
  powershell -ExecutionPolicy Bypass -File .\install.ps1 -NoService
#>
[CmdletBinding()]
param([switch]$System, [switch]$NoService)

# Native tools (pip, pipx) write warnings to stderr; with 'Stop' Windows PowerShell 5.1 would turn those
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

# --- find Python 3.8+
$PyCmd = $null
if (Get-Command py -ErrorAction SilentlyContinue) { $PyCmd = @('py', '-3') }
elseif (Get-Command python -ErrorAction SilentlyContinue) { $PyCmd = @('python') }
else { throw "Python 3.8+ not found. Install it with: winget install Python.Python.3.12" }
function Invoke-Py {
    # Simple (non-advanced) function so "-c" / "-m" pass through in $args untouched.
    $exe = $PyCmd[0]; $pre = @($PyCmd | Select-Object -Skip 1)
    & $exe @pre @args
}
$PyExe = (Invoke-Py -c "import sys; print(sys.executable)").Trim()
$ok = Invoke-Py -c "import sys; print(int(sys.version_info >= (3, 8)))"
if ($ok.Trim() -ne '1') { throw "Python 3.8+ is required ($PyExe is too old)." }

# --- install
$Method = ''
$BinDir = ''
if (Get-Command pipx -ErrorAction SilentlyContinue) {
    Say 'Installing with pipx'
    & pipx install --force $Src
    if ($LASTEXITCODE -ne 0) { throw 'pipx install failed' }
    $BinDir = (& pipx environment --value PIPX_BIN_DIR 2>$null)
    if (-not $BinDir) { $BinDir = Join-Path $env:USERPROFILE '.local\bin' }
    $BinDir = $BinDir.Trim()
    $Method = 'pipx'
} else {
    Say "pipx not found; installing with: $PyExe -m pip install --user"
    Invoke-Py -m pip install --user --upgrade $Src
    if ($LASTEXITCODE -ne 0) { throw 'pip install --user failed' }
    $BinDir = (Invoke-Py -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))").Trim()
    $Method = 'pip-user'
}

# --- cmd.exe shim if pip did not produce hdmon.exe (e.g. unusual Python builds)
$Shim = $null
if (-not (Test-Path (Join-Path $BinDir 'hdmon.exe'))) {
    New-Item -ItemType Directory -Force -Path $BinDir | Out-Null
    $Shim = Join-Path $BinDir 'hdmon.cmd'
    "@echo off`r`n`"$PyExe`" -m hard_drive_monitor %*`r`n" | Set-Content -Path $Shim -Encoding ASCII -NoNewline
    Say "Wrote cmd.exe shim $Shim"
}

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

$exe = if (Test-Path (Join-Path $BinDir 'hdmon.exe')) { Join-Path $BinDir 'hdmon.exe' } else { $Shim }
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
