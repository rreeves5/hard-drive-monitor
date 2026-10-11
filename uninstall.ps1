<#
.SYNOPSIS
  Removes Hard Drive Monitor (hdmon), its scheduled task, and the PATH entry install.ps1 added.
  History and config are kept unless you pass -Purge.
#>
[CmdletBinding()]
param([switch]$Purge)

$ErrorActionPreference = 'Continue'
$StateDir = Join-Path $env:LOCALAPPDATA 'hard-drive-monitor'
$StateFile = Join-Path $StateDir 'install-state.json'
function Say($msg) { Write-Host "==> $msg" }
function Normalize([string]$p) { return $p.Trim().TrimEnd('\').ToLowerInvariant() }

$state = $null
if (Test-Path $StateFile) { $state = Get-Content $StateFile -Raw | ConvertFrom-Json }

# scheduled task
& schtasks /Query /TN hard-drive-monitor *> $null
if ($LASTEXITCODE -eq 0) { Say 'Removing scheduled task hard-drive-monitor'; & schtasks /Delete /TN hard-drive-monitor /F | Out-Null }

# binaries
$binDir = if ($state -and $state.bin_dir) { $state.bin_dir } else { Join-Path $StateDir 'bin' }
foreach ($f in 'hdmon.exe', 'hdmonw.exe', 'hdmon.exe.old', 'hdmonw.exe.old') {
    $p = Join-Path $binDir $f
    if (Test-Path $p) {
        Remove-Item $p -Force -ErrorAction SilentlyContinue
        if (Test-Path $p) { Write-Warning "Could not remove $p (in use, or needs Administrator)" } else { Say "Removed $p" }
    }
}
if ((Test-Path $binDir) -and -not (Get-ChildItem $binDir -Force)) { Remove-Item $binDir -Force -ErrorAction SilentlyContinue }

if ($state) {
    if ($state.shim -and (Test-Path $state.shim)) { Remove-Item $state.shim -Force; Say "Removed $($state.shim)" }
    if ($state.path_added) {
        $scope = $state.scope
        if ($scope -eq 'Machine') {
            $id = [Security.Principal.WindowsIdentity]::GetCurrent()
            if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
                Write-Warning "The PATH entry is in the Machine Path; re-run this script as Administrator to remove it."
                $scope = $null
            }
        }
        if ($scope) {
            $current = [Environment]::GetEnvironmentVariable('Path', $scope)
            $kept = @($current -split ';' | Where-Object { $_ -ne '' -and (Normalize $_) -ne (Normalize $state.bin_dir) })
            [Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), $scope)
            Say "Removed $($state.bin_dir) from the $scope Path"
            if (-not ('HdmonNative.Win32' -as [type])) {
                Add-Type -Namespace HdmonNative -Name Win32 -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
            }
            $r = [UIntPtr]::Zero
            [void][HdmonNative.Win32]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$r)
        }
    } else {
        Say "PATH was not changed by install.ps1 (the folder was already on it); leaving it alone."
    }
    Remove-Item $StateFile -Force -ErrorAction SilentlyContinue
} else {
    Say 'No install state found; PATH not touched.'
}

$conf = Join-Path $env:APPDATA 'hard-drive-monitor'
if ($Purge) {
    Remove-Item -Recurse -Force $StateDir, $conf -ErrorAction SilentlyContinue
    Say "Deleted history and config ($StateDir, $conf)"
} else {
    Write-Host "Kept history ($StateDir) and config ($conf). Use -Purge to delete them."
}
Write-Host 'Open a new Command Prompt or PowerShell window for the PATH change to take effect.'
exit 0   # do not leak a native tool's exit code (e.g. schtasks /Query for a missing task) to the caller
