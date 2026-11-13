param(
    [Parameter(Mandatory=$true)][string]$RuntimeBin,
    [string]$Python = "$PSScriptRoot/../.venv/Scripts/python.exe",
    [string]$BuildDir = "$PSScriptRoot/build"
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$runtime = (Resolve-Path -LiteralPath $RuntimeBin).Path
$pythonExe = (Resolve-Path -LiteralPath $Python).Path
$logDir = Join-Path $repo ('output/industrial_audit/release_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Path $logDir | Out-Null
function Invoke-Checked([string]$name, [string]$exe, [string[]]$arguments) {
    & $exe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $logDir "$name.log")
    if ($LASTEXITCODE -ne 0) { throw "$name failed with exit $LASTEXITCODE; see $logDir" }
}
$previousPath = $env:PATH
$previousPythonEncoding = $env:PYTHONIOENCODING
Push-Location $repo
try {
    $env:PATH = "$runtime;$previousPath"
    $env:PYTHONIOENCODING = 'utf-8'
    Invoke-Checked 'python_tests' $pythonExe @('-m','unittest','tests.test_spike_calibration_engine','tests.test_spike_calibration_tool','-v')
    Invoke-Checked 'generate_data' $pythonExe @('scripts/generate_mock_calibration_data.py')
    Invoke-Checked 'mock_scenarios' $pythonExe @('scripts/check_mock_calibration.py','--data','output/mock_calibration_data','--output',"$logDir/python_mock")
    Invoke-Checked 'http' $pythonExe @('scripts/verify_spike_calibration_tool.py','--output',"$logDir/http")
    Invoke-Checked 'configure' (Join-Path $runtime 'cmake.exe') @('-S','qt','-B',$BuildDir,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_PREFIX_PATH=$($runtime.Replace('\','/'))/..",'-DSPIKE_BUILD_SELFTEST=ON','-DSPIKE_BUILD_TESTS=ON','-DSPIKE_REQUIRE_MOCK_DATA=ON',"-DSPIKE_MOCK_DATA_DIR=$repo/output/mock_calibration_data")
    Invoke-Checked 'build' (Join-Path $runtime 'cmake.exe') @('--build',$BuildDir,'--parallel','4')
    Invoke-Checked 'ctest' (Join-Path $runtime 'ctest.exe') @('--test-dir',$BuildDir,'--output-on-failure')
    $ctestLog = Get-Content -LiteralPath (Join-Path $logDir 'ctest.log') -Raw
    if ($ctestLog -match 'Skipped|Not Run') { throw 'Release checks must not skip any tests.' }
    Write-Output "Software regression passed; hardware acceptance remains separate. Evidence: $logDir"
} finally {
    Pop-Location
    $env:PATH = $previousPath
    $env:PYTHONIOENCODING = $previousPythonEncoding
}
