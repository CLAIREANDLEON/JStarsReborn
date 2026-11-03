# Opt-in native 60 FPS candidate. Generates a private boot copy; original inputs stay unchanged.
param(
    [Parameter(Mandatory=$true)][string]$Runtime,
    [Parameter(Mandatory=$true)][string]$Boot,
    [Parameter(Mandatory=$true)][string]$Game,
    [Parameter(Mandatory=$true)][string]$WorkDir,
    [string]$Python = 'python',
    [ValidateRange(0,100)][int]$Volume = 0,
    [ValidateRange(0,86400)][int]$MaxSeconds = 0,
    [switch]$PrepareOnly,
    # Also rewrite the literal 30 Hz constants (tools/re/rate_constants.py; Claude 2026-11-03). Off: the original candidate.
    [switch]$RateConstants
)
$ErrorActionPreference = 'Stop'
$patcher = Join-Path $PSScriptRoot 're\prepare_timing_experiment.py'
foreach ($file in @($Runtime, $Boot, $patcher, (Join-Path $Game 'eboot.bin'))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required file missing: $file" }
}
$runtimeHash = (Get-FileHash -LiteralPath $Runtime -Algorithm SHA256).Hash.ToLowerInvariant()
if ($runtimeHash -ne 'e185b77c93b0ed5ed76d56c5cc3f306087b0eb7d5f146f76484a3c5811c1a181') {
    throw 'This experiment is validated only with the documented runtime revision.'
}
$runName = 'native60-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6)
$run = Join-Path ([IO.Path]::GetFullPath($WorkDir)) $runName
$null = New-Item -ItemType Directory -Path $run
$testBoot = Join-Path $run 'boot-experiment.bin'
& $Python $patcher --eboot (Join-Path $Game 'eboot.bin') --boot $Boot --out $testBoot --after-ticks 0 --engine-rate 60
if ($LASTEXITCODE -ne 0) { throw 'Timing experiment preparation failed; the game was not started.' }
if ($RateConstants) {
    $constants = Join-Path $PSScriptRoot 're\rate_constants.py'
    $patched = Join-Path $run 'boot-experiment-constants.bin'
    & $Python $constants --boot $testBoot --out $patched --rate 60
    if ($LASTEXITCODE -ne 0) { throw 'Rate constants could not be applied; the game was not started.' }
    $testBoot = $patched
}
$names = Join-Path (Split-Path -Parent $Boot) 'import-names.tsv'
if (Test-Path -LiteralPath $names) { Copy-Item -LiteralPath $names -Destination (Join-Path $run 'import-names.tsv') }
$manifest = [ordered]@{
    status = 'experimental: battle countdown verified, full gameplay audit pending'
    runtime_sha256 = $runtimeHash
    boot_sha256 = (Get-FileHash -LiteralPath $testBoot -Algorithm SHA256).Hash.ToLowerInvariant()
    engine_rate = 60
    rate_constants = [bool]$RateConstants
    presentation_hz = 60
    volume = $Volume
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'experiment.json')
Write-Host "Prepared $runName. Generated boot files and raw logs must remain private."
if ($PrepareOnly) { return }
$gameArgs = @('--boot', $testBoot, '--game-root', $Game,
    '--user-dir', (Join-Path $run 'user'), '--log', (Join-Path $run 'game.log'),
    '--gpu-dir', (Join-Path $run 'gpu'), '--capture-dir', (Join-Path $run 'captures'),
    '--capture-flips', '30', '--capture-interval', '600', '--capture-max', '30',
    '--volume', "$Volume", '--vblank-hz', '60', '--frame-stats')
if ($MaxSeconds -gt 0) { $gameArgs += @('--max-seconds', "$MaxSeconds") }
# Windows PowerShell joins Start-Process arguments into one command line.
# These arguments contain no embedded quotes; quote paths as complete arguments.
foreach ($arg in $gameArgs) { if ($arg.Contains('"') -or $arg.EndsWith('\')) { throw 'Unsupported quoted or trailing-backslash argument.' } }
$commandLine = ($gameArgs | ForEach-Object { '"' + $_ + '"' }) -join ' '
$process = Start-Process -FilePath $Runtime -ArgumentList $commandLine -WindowStyle Hidden -PassThru `
    -WorkingDirectory (Split-Path -Parent $Runtime) `
    -RedirectStandardOutput (Join-Path $run 'stdout.txt') -RedirectStandardError (Join-Path $run 'stderr.txt')
$null = $process.Handle
$process.WaitForExit()
$process.ExitCode | Set-Content -LiteralPath (Join-Path $run 'exit.txt')
Write-Host "Test ended with exit code $($process.ExitCode)."
