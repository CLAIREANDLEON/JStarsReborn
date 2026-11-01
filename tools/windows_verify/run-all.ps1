# JStarsReborn Windows verification runner. Writes everything to .\results. The game folder is only read.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$res = Join-Path $root 'results'
New-Item -ItemType Directory -Force $res | Out-Null
$game = 'E:\CUSA01593'
$summary = @()

function Run($name, $exe, $arguments, $timeout = 60) {
    $out = Join-Path $res "$name.txt"
    $err = Join-Path $res "$name.err.txt"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try {
        $sp = @{ FilePath = $exe; PassThru = $true; NoNewWindow = $true; RedirectStandardOutput = $out; RedirectStandardError = $err; WorkingDirectory = $root }
        if ($arguments) { $sp.ArgumentList = $arguments }
        $p = Start-Process @sp
        $null = $p.Handle   # keep the process handle so ExitCode is available after exit
        if (-not $p.WaitForExit($timeout * 1000)) { $p.Kill(); $code = 'TIMEOUT' } else { $p.WaitForExit(); $code = $p.ExitCode }
    } catch { $code = "LAUNCH-FAILED: $($_.Exception.Message)" }
    $script:summary += [pscustomobject]@{ name = $name; exit = "$code"; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 2) }
}

# Environment
$env = @()
$env += (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "OS: $($_.Caption) $($_.Version) $($_.OSArchitecture)" })
$env += (Get-CimInstance Win32_Processor | ForEach-Object { "CPU: $($_.Name)" })
$env += (Get-CimInstance Win32_VideoController | ForEach-Object { "GPU: $($_.Name) driver $($_.DriverVersion)" })
$env += "VulkanSDK dirs: " + ((Get-ChildItem "$env:USERPROFILE\VulkanSDK" -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')
$env | Set-Content (Join-Path $res 'environment.txt')

# Game-independent suites
foreach ($t in 'core','input','paths','gamecheck','pkg_intake','setup_inventory','setup_install') {
    Run "test-$t" (Join-Path $root "tests\${t}_tests.exe") '' 300
}

# Validator against the real dump (read-only), with the supported-build manifest
Run 'validate-game' (Join-Path $root 'pkg\JStarsSetup.exe') "--validate-game `"$game`" --versions `"$root\pkg\supported-versions.tsv`" --report `"$res\validation.json`"" 120

# Runtime exe: DX12, audio, crash reporter (own data dir; nothing written beside the game)
$data = Join-Path $res 'rt-data'
Run 'runtime-smoke-audio'  (Join-Path $root 'pkg\JStarsReborn.exe') "--smoke-frames 120 --no-dialogs --data-dir `"$data-a`"" 90
Run 'runtime-smoke-noaudio' (Join-Path $root 'pkg\JStarsReborn.exe') "--smoke-frames 120 --no-audio --no-dialogs --data-dir `"$data-b`"" 90
Run 'runtime-validation-layer' (Join-Path $root 'pkg\JStarsReborn.exe') "--validation --smoke-frames 120 --no-audio --no-dialogs --data-dir `"$data-c`"" 90
Run 'runtime-resize' (Join-Path $root 'pkg\JStarsReborn.exe') "--resize-smoke --smoke-frames 120 --no-audio --no-dialogs --data-dir `"$data-d`"" 90
Run 'crash-access' (Join-Path $root 'pkg\JStarsReborn.exe') "--crash-test access --no-dialogs --data-dir `"$data-e`"" 60
Run 'crash-terminate' (Join-Path $root 'pkg\JStarsReborn.exe') "--crash-test terminate --no-dialogs --data-dir `"$data-f`"" 60

# Game execution runtime (original code from the prepared image; stops at the first unimplemented import)
Run 'boot-windows' (Join-Path $root 'bin\jstars-boot.exe') "--boot `"$root\work\CUSA01593_01.02_d43b7b5db10b\prepared\boot-linked.bin`" --log `"$res\boot-windows.log`"" 120

$summary | ConvertTo-Json | Set-Content (Join-Path $res 'summary.json')
'DONE' | Set-Content (Join-Path $res 'DONE.txt')
