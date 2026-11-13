param(
    [Parameter(Mandatory=$true)][string]$RuntimeBin,
    [string]$BuildDir = "$PSScriptRoot/build",
    [string]$OutputDir = "$PSScriptRoot/../output/SpikeCalibrationQt"
)
$ErrorActionPreference = 'Stop'
$runtime = (Resolve-Path -LiteralPath $RuntimeBin).Path
$build = (Resolve-Path -LiteralPath $BuildDir).Path
$exe = Join-Path $build 'SpikeCalibrationTool.exe'
if (!(Test-Path -LiteralPath $exe)) { throw "Build the Release executable first: $exe" }
if (Test-Path -LiteralPath $OutputDir) { throw "Use a new output directory to avoid mixing package versions: $OutputDir" }
$package = (New-Item -ItemType Directory -Path $OutputDir).FullName
$oldPath = $env:PATH
try {
    $env:PATH = "$runtime;$oldPath"
    Copy-Item -LiteralPath $exe -Destination $package
    & (Join-Path $runtime 'windeployqt.exe') --release --no-translations --no-system-d3d-compiler --dir $package $exe
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }
    # Resolve OpenCV and MinGW transitive DLL imports as well as Qt plugins.
    $objdump = Join-Path $runtime 'objdump.exe'
    if (!(Test-Path -LiteralPath $objdump)) { throw 'This packaging script requires the MinGW objdump tool.' }
    $visited = @{}
    $queue = [System.Collections.Generic.Queue[string]]::new()
    Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object Extension -In '.exe','.dll' | ForEach-Object { $queue.Enqueue($_.FullName) }
    while ($queue.Count) {
        $binary = $queue.Dequeue()
        if ($visited.ContainsKey($binary)) { continue }
        $visited[$binary] = $true
        $imports = & $objdump -p $binary
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect $binary" }
        foreach ($line in $imports) {
            if ($line -match 'DLL Name:\s*(\S+)') {
                $name = $Matches[1]
                $source = Join-Path $runtime $name
                $dest = Join-Path $package $name
                if ((Test-Path -LiteralPath $source) -and !(Test-Path -LiteralPath $dest)) {
                    Copy-Item -LiteralPath $source -Destination $dest
                    $queue.Enqueue($dest)
                } elseif (!(Test-Path -LiteralPath $source) -and !(Test-Path -LiteralPath $dest) -and
                          !(Test-Path -LiteralPath (Join-Path "$env:SystemRoot/System32" $name)) -and
                          $name -notmatch '^(api-ms-|ext-ms-)') {
                    throw "Unresolved runtime dependency: $name from $binary"
                }
            }
        }
    }
    Copy-Item -LiteralPath "$PSScriptRoot/README.md" -Destination $package
    Copy-Item -LiteralPath "$PSScriptRoot/docs" -Destination $package -Recurse
    $licenses = Join-Path (Split-Path $runtime) 'share/licenses'
    if (Test-Path -LiteralPath $licenses) { Copy-Item -LiteralPath $licenses -Destination (Join-Path $package 'third-party-licenses') -Recurse }
    Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
        [pscustomobject]@{path=$_.FullName.Substring($package.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash;bytes=$_.Length}
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $package 'manifest-sha256.json') -Encoding utf8
    Write-Output "Package ready: $package"
} finally { $env:PATH = $oldPath }
