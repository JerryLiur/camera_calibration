param([int]$Port = 8776, [switch]$Browser, [switch]$Foreground, [string]$Python)
$ErrorActionPreference = 'Stop'
$spikeRoot = Split-Path -Parent $PSScriptRoot
$spikePython = if ($Python) { $Python } else { Join-Path $spikeRoot '.venv\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $spikePython -PathType Leaf)) {
    throw '找不到 Python 环境。请先执行 py -3.12 -m venv .venv，再按 README.md 安装依赖；也可用 -Python 指定 python.exe。'
}
$spikePython = (Resolve-Path -LiteralPath $spikePython).Path
$spikeData = Join-Path $spikeRoot 'spike_calibration_workspace'
$spikeLog = Join-Path $spikeData 'logs'
New-Item -ItemType Directory -Force -Path $spikeLog | Out-Null
$spikeOldPath = $env:PYTHONPATH
try {
    $env:PYTHONPATH = @($spikeRoot, $spikeOldPath) -join [IO.Path]::PathSeparator
    & $spikePython -c 'import numpy, cv2'
    if ($LASTEXITCODE -ne 0) { throw '缺少运行依赖。请用所选 Python 执行 -m pip install -r requirements.txt。' }
    if (-not $Browser) {
        & $spikePython -c 'import webview'
        if ($LASTEXITCODE -ne 0) { throw '缺少桌面依赖。请用所选 Python 执行 -m pip install -r requirements-desktop.txt，并安装 Microsoft Edge WebView2 Runtime。' }
    }
    if ($Browser) {
        $spikeExisting = Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue
        if ($spikeExisting) {
            try {
                $spikeState = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/state" -TimeoutSec 3
                if ($spikeState.app -eq 'spike-calibration-tool') { Start-Process "http://127.0.0.1:$Port"; exit 0 }
            } catch {}
            throw "端口 $Port 已被其他程序使用，请使用 -Port 指定其他端口。"
        }
        $spikeArgs = '-m spike_calibration_tool --data "' + $spikeData + '" serve --port ' + $Port
        if ($Foreground) {
            Push-Location -LiteralPath $spikeRoot
            try { & $spikePython -m spike_calibration_tool --data $spikeData serve --port $Port --open } finally { Pop-Location }
        } else {
            $spikeProcess = Start-Process -FilePath $spikePython -ArgumentList $spikeArgs -WorkingDirectory $spikeRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $spikeLog 'server.stdout.log') -RedirectStandardError (Join-Path $spikeLog 'server.stderr.log')
            $spikeReady = $false
            for ($spikeAttempt=0; $spikeAttempt -lt 50; $spikeAttempt++) {
                Start-Sleep -Milliseconds 250
                try {
                    $spikeState = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/state" -TimeoutSec 1
                    if ($spikeState.app -eq 'spike-calibration-tool') { $spikeReady=$true; break }
                } catch {}
                if ($spikeProcess.HasExited) { break }
            }
            if (-not $spikeReady) { throw "启动失败，请查看 $spikeLog" }
            Start-Process "http://127.0.0.1:$Port"
        }
    } else {
        $spikeArgs = '-m spike_calibration_tool --data "' + $spikeData + '" desktop'
        $spikeWindowPython = Join-Path (Split-Path -Parent $spikePython) 'pythonw.exe'
        if (-not (Test-Path -LiteralPath $spikeWindowPython)) { $spikeWindowPython = $spikePython }
        Start-Process -FilePath $spikeWindowPython -ArgumentList $spikeArgs -WorkingDirectory $spikeRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $spikeLog 'desktop.stdout.log') -RedirectStandardError (Join-Path $spikeLog 'desktop.stderr.log')
    }
} finally { $env:PYTHONPATH = $spikeOldPath }
