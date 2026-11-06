# SPDX-License-Identifier: GPL-3.0-or-later
# Owner authorized installation, then requested no pre-install reads.
# Default: switch OFF first. -RestartRunning tries normal remote restart first.
param([switch]$RestartRunning,[switch]$WarmUpdate)
$ErrorActionPreference = 'Stop'
if ($RestartRunning -and $WarmUpdate) { throw 'Choose one entry mode' }
$bench = Join-Path $env:USERPROFILE 'Documents\DM32Bench'
$tools = Join-Path $bench 'tools'
$pins = @{
    'warm_update.py' = '3091e61a357ee706afe260487396cfbdf6be79d5bc86869654817289ec12eb81'
    'native_modem.py' = '5bbe66ef6f88d1484582adff700f91b53e97a2b2c6ac5ef7c789982c6a34498d'
    'normal_reboot.py' = 'fe29c685cdc24acb45f08163bb5c1c6ecfe6323cc1c217669a7d49054228c176'
    'install_firstboot.py' = 'b7d7c5d132e1515445edfe3d3fb2188e9e6348dff432a3d4580d6e980c0cec3c'
    'dm32_readonly_backup.py' = 'f1c9b3d0dd37c5f2b74d1376481178c314c529a6bd80dea6745c226bdd606c24'
    'prepare_unit_image.py' = '839473851155df6b289cc4bf6b781ada784aa6a1bcb6d05c3bce70f0ba5b0431'
    'firstboot_check.py' = '6bf82f128993798206754ab664d55c504df73bdf6799f036281c581b383882fe'
}
foreach ($p in $pins.GetEnumerator()) {
    if ((Get-FileHash -Algorithm SHA256 (Join-Path $tools $p.Key)).Hash.ToLowerInvariant() -ne $p.Value) {
        throw ('Reviewed tool hash mismatch: ' + $p.Key)
    }
}
$ports = @(Get-CimInstance Win32_PnPEntity | Where-Object {
    $_.Name -match '\(COM5\)$' -and $_.PNPDeviceID -match 'VID_1A86&PID_7523' -and $_.Status -eq 'OK'
})
if ($ports.Count -ne 1) { throw 'Expected CH340 COM5 is absent or ambiguous' }
$running = @(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -eq 'python.exe' -and $_.CommandLine -match 'install_firstboot\.py'
})
if ($running.Count) { throw 'An installation process already exists' }
$reports = Join-Path $bench 'reports'
$null = New-Item -ItemType Directory -Path $reports -Force
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$session = Join-Path $reports ('firstboot-install-' + $stamp)
$stdout = $session + '.stdout.log'
$stderr = $session + '.stderr.log'
$python = Join-Path $bench '.venv\Scripts\python.exe'
$script = Join-Path $tools 'install_firstboot.py'
$backup = Join-Path $bench 'backups\DM32-stock-048-20261105T233920Z'
$package = Join-Path $bench 'images\unit048-firstboot-p25-rolling-20261106'
$installed = Join-Path $bench 'reports\firstboot-install-20261106T181313357Z\application-after.bin'
$arguments = @('-u', ('"' + $script + '"'), '--port', 'COM5', '--backup', ('"' + $backup + '"'),
    '--package', ('"' + $package + '"'), '--out', ('"' + $session + '"'), '--wait', '600',
    '--use-saved-backup', '--installed-application', ('"' + $installed + '"'), '--sector-write')
if ($WarmUpdate) { $arguments += '--warm-update' }
if ($RestartRunning) {
    $arguments += '--restart-running'
    $arguments[$arguments.IndexOf('--wait') + 1] = '20'
}
$process = Start-Process -FilePath $python -ArgumentList $arguments -WorkingDirectory $bench -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden
$null = $process.Handle # Cache the handle so ExitCode survives a quick exit.
$active = [ordered]@{ pid=$process.Id; session=$session; stdout=$stdout; stderr=$stderr; state='running' }
$activePath = Join-Path $tools 'install-active.json'
$active | ConvertTo-Json | Set-Content -Encoding UTF8 $activePath
Write-Output ('INSTALL_SESSION ' + $session)
$process.WaitForExit()
$process.Refresh()
$active.state = 'finished'
$active.exit_code = $process.ExitCode
$active | ConvertTo-Json | Set-Content -Encoding UTF8 $activePath
Write-Output ('INSTALL_EXIT ' + $process.ExitCode)
exit $process.ExitCode
