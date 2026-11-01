# JStarsReborn checkpoint 0.3 Windows runner (GPU-linked runtime).
# Every invocation writes to its own new folder  results\<UTC stamp>-<computer>-<random>\  and never
# overwrites an earlier run. stdout, stderr, exit code and a completion record are captured for every
# step, including steps that fail, time out or cannot launch. The game folder is only read.
param(
    [string]$Game = 'E:\CUSA01593',
    [int]$GpuSeconds = 90,
    [int]$CaptureFlips = 30,
    [int]$CaptureInterval = 300,   # flips between captures (~10 s at 30 flips/s)
    [int]$CaptureMax = 12,
    [string]$PadScript = '',       # automated test input (default: tests\pad-title-probe.txt when present)
    [int]$SampleEvery = 15,        # seconds between guest-thread samples (where is each thread)
    [string[]]$Experiments = @(),   # diagnosis-only platform behaviors, each run as its own step (see docs/EXPERIMENTS.md)
    [switch]$Quick,                 # regression + self-test + the -Experiments steps only (fast iteration)
    [switch]$SkipValidation,
    [switch]$NoCount,               # leave out --count-imports / --trace-draws (full-speed run; no per-import statistics)
    [switch]$NoAudio,               # pass --no-audio (timed null sink instead of the host audio device)
    [switch]$SkipNoPad,             # skip the 25 s no-controller step (the other steps are unchanged)
    [string]$UserDir = '',          # persistent save/user folder for the scripted step (default: a fresh folder inside the run)
    [string]$HeapGuardExe = '',     # optional heap-guard diagnostic build (bin\<name>): one extra short game step where every host heap overflow / use-after-free / bad free is trapped at the offending instruction
    [int]$HeapGuardSeconds = 60,
    [string]$ExeName = ''     # default: the newest bin\jstars-boot-gpu*.exe (recorded in binaries-sha256.txt)
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$rand = -join ((48..57) + (97..102) | Get-Random -Count 6 | ForEach-Object { [char]$_ })
$run = Join-Path $root "results\$stamp-$($env:COMPUTERNAME)-$rand"
New-Item -ItemType Directory -Force $run | Out-Null
$steps = New-Object System.Collections.ArrayList
$started = (Get-Date).ToUniversalTime().ToString('o')
$status = 'INCOMPLETE'
if ($ExeName) { $exe = Join-Path $root "bin\$ExeName" }
else { $exe = (Get-ChildItem (Join-Path $root 'bin') -Filter 'jstars-boot-gpu*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
if (-not $PadScript) { $cand = Join-Path $root 'tests\pad-title-probe.txt'; if (Test-Path $cand) { $PadScript = $cand } }
$boot = Join-Path $root 'work\CUSA01593_01.02_d43b7b5db10b\prepared\boot-linked.bin'

function Step($name, $arguments, $timeout, $envVars, $exePath) {
    if (-not $exePath) { $exePath = $exe }
    $out = Join-Path $run "$name.stdout.txt"; $err = Join-Path $run "$name.stderr.txt"
    $sw = [Diagnostics.Stopwatch]::StartNew(); $code = $null; $note = ''
    $saved = @{}
    try {
        if ($envVars) { foreach ($k in $envVars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $envVars[$k]) } }
        $p = Start-Process -FilePath $exePath -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError $err -WorkingDirectory $root
        $null = $p.Handle
        if (-not $p.WaitForExit($timeout * 1000)) { try { $p.Kill() } catch {}; $code = 'TIMEOUT-KILLED' } else { $p.WaitForExit(); $code = $p.ExitCode }
    } catch { $code = 'LAUNCH-FAILED'; $note = $_.Exception.Message }
    finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
    [void]$steps.Add([pscustomobject]@{ name = $name; exit = "$code"; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 2); args = $arguments; note = $note })
    $steps | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'steps.json')   # written after every step
    return $code
}

try {
    # environment + exact binaries under test
    $e = @()
    $e += (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "OS: $($_.Caption) $($_.Version) $($_.OSArchitecture)" })
    $e += (Get-CimInstance Win32_Processor | ForEach-Object { "CPU: $($_.Name)" })
    $e += (Get-CimInstance Win32_VideoController | ForEach-Object { "GPU: $($_.Name) driver $($_.DriverVersion)" })
    $e += "Vulkan loader: " + ((Get-Item "$env:WINDIR\System32\vulkan-1.dll" -ErrorAction SilentlyContinue).VersionInfo.FileVersion)
    $e += "VulkanSDK dirs: " + ((Get-ChildItem "$env:USERPROFILE\VulkanSDK" -ErrorAction SilentlyContinue | ForEach-Object Name) -join ', ')
    $e += "Runner: run-gpu.ps1 $stamp"
    $e | Set-Content (Join-Path $run 'environment.txt')
    Get-FileHash (@($exe, $boot) + $(if ($HeapGuardExe) { @(Join-Path $root "bin\$HeapGuardExe") } else { @() })) -Algorithm SHA256 -ErrorAction SilentlyContinue | ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path -Leaf $_.Path)" } | Set-Content (Join-Path $run 'binaries-sha256.txt')
    if (Test-Path "$Game\eboot.bin") { (Get-FileHash "$Game\eboot.bin" -Algorithm SHA256).Hash.ToLower() + '  eboot.bin (read-only input)' | Set-Content (Join-Path $run 'input-sha256.txt') }

    # 1. regression: the verified 0.2.2 checkpoint (stops at the first graphics call, exit 3)
    $c = Step 'regress-stop-at-first-gnm' "--boot `"$boot`" --stop-at-first-gnm --log `"$run\regress-stop-at-first-gnm.log`"" 180
    # 2. host services
    $c = Step 'selftest-host' "--selftest-host --log `"$run\selftest-host.log`"" 120
    # 3. audit table of the runtime-implemented imports
    $c = Step 'audit-dump' '--audit-dump' 30
    # 4. the game with the GPU library: window, Vulkan device, command processor, presentation
    $gdir = Join-Path $run 'gpu'; New-Item -ItemType Directory -Force $gdir | Out-Null
    # 3b. heap-guard build: the same start-up check and a short scripted game run with every host allocation guard-paged
    if ($HeapGuardExe) {
        $hgPath = Join-Path $root "bin\$HeapGuardExe"
        $c = Step 'heapguard-regress-stop-at-first-gnm' "--boot `"$boot`" --stop-at-first-gnm --log `"$run\heapguard-regress.log`"" 180 $null $hgPath
        $hdir = Join-Path $run 'gpu-heapguard'; New-Item -ItemType Directory -Force $hdir | Out-Null
        $hpad = ''; if ($PadScript) { $hpad = "--pad-script `"$PadScript`"" }
        $c = Step 'heapguard-game-gpu' "--boot `"$boot`" --game-root `"$Game`" --user-dir `"$run\user-heapguard`" --log `"$run\heapguard-game-gpu.log`" --gpu-dir `"$hdir`" --capture-dir `"$run\captures-heapguard`" --capture-flips 90 --capture-max 1 --capture-interval 60 $hpad --no-audio --max-seconds $HeapGuardSeconds" ($HeapGuardSeconds + 90) $null $hgPath
    }
    if (-not $Quick) {
    # 4a. no controller: --pad-none (no source at all, even if an XInput pad is attached) -> the game's own "connect a controller" screen (unless an XInput pad is attached)
    $ndir = Join-Path $run 'gpu-nopad'; New-Item -ItemType Directory -Force $ndir | Out-Null
    if (-not $SkipNoPad) { $c = Step 'game-gpu-nopad' "--boot `"$boot`" --game-root `"$Game`" --user-dir `"$run\user-nopad`" --log `"$run\game-gpu-nopad.log`" --gpu-dir `"$ndir`" --capture-dir `"$run\captures-nopad`" --capture-flips 90 --capture-max 2 --capture-interval 60 --pad-none --sample-threads 10 --max-seconds 25" 85 }
    # 4b. scripted test input (see the log lines 'SCRIPTED TEST INPUT') driving the game
    $padArg = ''; if ($PadScript) { $padArg = "--pad-script `"$PadScript`"" }
    if ($NoAudio) { $padArg += ' --no-audio' }
    $adir = Join-Path $run 'audio'; New-Item -ItemType Directory -Force $adir | Out-Null
    $padArg += " --dump-audio `"$adir\game`""   # <prefix>.portN.wav: stereo S16 downmix of what the game submitted (first 100 s per port)
    $cnt = if ($NoCount) { '' } else { '--count-imports --trace-draws 3000,60' }
    $udir = if ($UserDir) { $UserDir } else { "$run\user" }
    $c = Step 'game-gpu' "--boot `"$boot`" --game-root `"$Game`" --user-dir `"$udir`" --log `"$run\game-gpu.log`" --gpu-dir `"$gdir`" --capture-dir `"$run\captures`" --capture-flips $CaptureFlips --capture-interval $CaptureInterval --capture-max $CaptureMax $padArg --sample-threads $SampleEvery $cnt --max-seconds $GpuSeconds" ($GpuSeconds + 60)
    # 5. same run with the Vulkan validation layer (if installed)
    if (-not $SkipValidation) {
        $vdir = Join-Path $run 'gpu-validation'; New-Item -ItemType Directory -Force $vdir | Out-Null
        $c = Step 'game-gpu-vk-validation' "--boot `"$boot`" --game-root `"$Game`" --user-dir `"$run\user-validation`" --log `"$run\game-gpu-vk-validation.log`" --gpu-dir `"$vdir`" --capture-dir `"$run\captures-validation`" --capture-flips $CaptureFlips --capture-interval $CaptureInterval --capture-max 4 $padArg --max-seconds $GpuSeconds" ($GpuSeconds + 60) @{ JSTARS_VK_VALIDATION = '1' }
    }
    }  # end of -not $Quick
    $padArg = ''; if ($PadScript) { $padArg = "--pad-script `"$PadScript`"" }
    foreach ($ex in $Experiments) {
        $edir = Join-Path $run "gpu-exp-$ex"; New-Item -ItemType Directory -Force $edir | Out-Null
        $c = Step "game-gpu-exp-$ex" "--boot `"$boot`" --game-root `"$Game`" --user-dir `"$run\user-exp-$ex`" --log `"$run\game-gpu-exp-$ex.log`" --gpu-dir `"$edir`" --capture-dir `"$run\captures-exp-$ex`" --capture-flips 60 --capture-interval 300 --capture-max 6 $padArg --sample-threads 15 --count-imports --trace-draws 3000,30 --experiment $ex --max-seconds 60" 120
    }
    $status = 'COMPLETE'
} catch {
    $status = "RUNNER-ERROR: $($_.Exception.Message)"
} finally {
    $record = [pscustomobject]@{
        status = $status; started_utc = $started; finished_utc = (Get-Date).ToUniversalTime().ToString('o')
        computer = $env:COMPUTERNAME; run_folder = $run; steps = $steps
    }
    $record | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $run 'COMPLETE.json')
}
Write-Host "Run folder: $run  status: $status"
