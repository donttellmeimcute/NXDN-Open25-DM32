# SPDX-License-Identifier: GPL-3.0-or-later
# Run only after the operator confirms PTT+SK1 update mode and green LED.
param([string]$Port = 'COM5')
$ErrorActionPreference = 'Stop'
$bench = Join-Path $env:USERPROFILE 'Documents\DM32Bench'
$probe = Join-Path $bench 'tools\dm32_update_probe.py'
$python = Join-Path $bench '.venv\Scripts\python.exe'
$expected = 'a3f86df206d8f584b48c2009187e7c37dc127bd84d7d3e9bbaa9f2f6ee95e69a'
if ((Get-FileHash -Algorithm SHA256 $probe).Hash.ToLowerInvariant() -ne $expected) {
    throw 'Identification probe source differs from reviewed version'
}
$devices = @(Get-CimInstance Win32_PnPEntity | Where-Object {
    $_.Name -match ('\(' + [regex]::Escape($Port) + '\)$') -and
    $_.PNPDeviceID -match 'VID_1A86&PID_7523' -and $_.Status -eq 'OK'
})
if ($devices.Count -ne 1) { throw 'Expected CH340 port is absent or ambiguous' }
$reports = Join-Path $bench 'reports'
$null = New-Item -ItemType Directory -Path $reports -Force
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$out = Join-Path $reports ('update-loader-probe-' + $stamp + '.json')
& $python $probe --port $Port --out $out
if ($LASTEXITCODE -ne 0) { throw ('Identification failed; report: ' + $out) }
Write-Output ('Report: ' + $out)
