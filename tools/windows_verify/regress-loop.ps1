# JStarsReborn: repeated start-up regression (--stop-at-first-gnm must exit 3 at the first Gnm call) to measure how often the
# intermittent early-start faults happen. Every invocation writes to its own new folder  results\regress-loop-<UTC>-<computer>-<random>\
# (never overwrites an earlier run). One line per iteration in loop.tsv (iteration, exe, exit code, seconds); the log of every
# iteration that did NOT exit 3 is kept in full, the logs of passing iterations are deleted. The game folder is only read.
param(
    [int]$Count = 30,
    [string[]]$ExeNames = @(),   # default: the newest bin\jstars-boot-gpu*.exe; several names run interleaved (A/B comparison)
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
$rand = -join ((48..57) + (97..102) | Get-Random -Count 6 | ForEach-Object { [char]$_ })
$run = Join-Path $root "results\regress-loop-$stamp-$($env:COMPUTERNAME)-$rand"
New-Item -ItemType Directory -Force $run | Out-Null
# 'a.exe,b.exe' passed through cmd.exe / powershell -File arrives as ONE string: split it (an unsplit list made every launch fail at once)
$ExeNames = @($ExeNames | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($ExeNames.Count -eq 0) { $ExeNames = @((Get-ChildItem (Join-Path $root 'bin') -Filter 'jstars-boot-gpu*.exe' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).Name) }
$boot = Join-Path $root 'work\CUSA01593_01.02_d43b7b5db10b\prepared\boot-linked.bin'
foreach ($n in $ExeNames) { if (-not (Test-Path (Join-Path $root "bin\$n"))) { "MISSING bin\$n - nothing was run" | Set-Content (Join-Path $run 'ABORTED.txt'); 'ABORTED' | Set-Content (Join-Path $run 'COMPLETE.txt'); exit 2 } }
if (-not (Test-Path $boot)) { "MISSING $boot - nothing was run" | Set-Content (Join-Path $run 'ABORTED.txt'); 'ABORTED' | Set-Content (Join-Path $run 'COMPLETE.txt'); exit 2 }
$tsv = Join-Path $run 'loop.tsv'
"iteration`texe`texit`tseconds`tkept_log" | Set-Content $tsv
$summary = @{}
for ($i = 1; $i -le $Count; $i++) {
    foreach ($name in $ExeNames) {
        $exe = Join-Path $root "bin\$name"
        $log = Join-Path $run ("it{0:000}-{1}.log" -f $i, $name)
        $sw = [Diagnostics.Stopwatch]::StartNew(); $code = $null; $note = ''
        try {
            # same launch style as the verification runners (-NoNewWindow, working directory = runner folder)
            $p = Start-Process -FilePath $exe -ArgumentList "--boot `"$boot`" --stop-at-first-gnm --log `"$log`"" -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $run 'o.txt') -RedirectStandardError (Join-Path $run 'e.txt') -WorkingDirectory $root
            $null = $p.Handle
            if (-not $p.WaitForExit($TimeoutSeconds * 1000)) { try { $p.Kill() } catch {} ; $code = 'timeout' } else { $p.WaitForExit(); $code = $p.ExitCode }
        } catch { $code = 'LAUNCH-FAILED'; $note = $_.Exception.Message; $note | Add-Content (Join-Path $run 'launch-errors.txt') }
        $sw.Stop()
        $kept = 'no'
        if ("$code" -eq '3') { Remove-Item $log -ErrorAction SilentlyContinue } else { $kept = 'yes'; Copy-Item (Join-Path $run 'o.txt') (Join-Path $run ("it{0:000}-{1}.stdout.txt" -f $i, $name)) -ErrorAction SilentlyContinue; Copy-Item (Join-Path $run 'e.txt') (Join-Path $run ("it{0:000}-{1}.stderr.txt" -f $i, $name)) -ErrorAction SilentlyContinue }
        "{0}`t{1}`t{2}`t{3:N1}`t{4}" -f $i, $name, $code, $sw.Elapsed.TotalSeconds, $kept | Add-Content $tsv
        $k = "$name exit $code"; if ($summary.ContainsKey($k)) { $summary[$k]++ } else { $summary[$k] = 1 }
    }
}
Remove-Item (Join-Path $run 'o.txt'), (Join-Path $run 'e.txt') -ErrorAction SilentlyContinue
($summary.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0}: {1}" -f $_.Name, $_.Value }) | Set-Content (Join-Path $run 'summary.txt')
'COMPLETE' | Set-Content (Join-Path $run 'COMPLETE.txt')
