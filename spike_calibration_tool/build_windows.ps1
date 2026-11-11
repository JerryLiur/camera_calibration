param([string]$OutputDirectory, [string]$Python)
$ErrorActionPreference = 'Stop'
$spikeRoot = Split-Path -Parent $PSScriptRoot
$spikePython = if ($Python) { $Python } else { Join-Path $spikeRoot '.venv\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $spikePython -PathType Leaf)) {
    throw '找不到 Python 环境。请先执行 py -3.12 -m venv .venv，再安装 requirements-desktop.txt；也可用 -Python 指定 python.exe。'
}
$spikePython = (Resolve-Path -LiteralPath $spikePython).Path
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $spikeRoot 'output\spike_calibration_windows' }
$spikeOldPath = $env:PYTHONPATH
$spikeOldEncoding = $env:PYTHONIOENCODING
try {
    $env:PYTHONPATH = @($spikeRoot, $spikeOldPath) -join [IO.Path]::PathSeparator
    $env:PYTHONIOENCODING = 'utf-8'
    & $spikePython -c 'import numpy, cv2, webview, PyInstaller'
    if ($LASTEXITCODE -ne 0) { throw '缺少桌面或构建依赖。请用所选 Python 执行 -m pip install -r requirements-desktop.txt。' }
    Push-Location -LiteralPath $spikeRoot
    try {
        & $spikePython -m PyInstaller --clean --noconfirm --distpath $OutputDirectory --workpath (Join-Path $spikeRoot 'build\spike_calibration_windows') (Join-Path $PSScriptRoot 'windows.spec')
        if ($LASTEXITCODE -ne 0) { throw 'Spike 标定工具打包失败' }
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $OutputDirectory 'Spike标定工具\使用说明.md') -Force
    } finally { Pop-Location }
} finally {
    $env:PYTHONPATH = $spikeOldPath
    $env:PYTHONIOENCODING = $spikeOldEncoding
}
