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

# package
if (Get-Command pipx -ErrorAction SilentlyContinue) {
    $list = & pipx list --short 2>$null
    if ($list -match '^hard-drive-monitor ') { Say 'pipx uninstall hard-drive-monitor'; & pipx uninstall hard-drive-monitor }
}
$py = if (Get-Command py -ErrorAction SilentlyContinue) { @('py', '-3') } elseif (Get-Command python -ErrorAction SilentlyContinue) { @('python') } else { $null }
if ($py) {
    $pre = @($py | Select-Object -Skip 1)
    & $py[0] @pre -m pip show hard-drive-monitor *> $null
    if ($LASTEXITCODE -eq 0) { Say 'pip uninstall hard-drive-monitor'; & $py[0] @pre -m pip uninstall -y hard-drive-monitor | Out-Null }
}

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
