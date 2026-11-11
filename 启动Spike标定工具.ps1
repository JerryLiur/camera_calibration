param([int]$Port = 8776, [switch]$Browser, [switch]$Foreground, [string]$Python)
& (Join-Path $PSScriptRoot 'spike_calibration_tool\start.ps1') -Port $Port -Browser:$Browser -Foreground:$Foreground -Python $Python
