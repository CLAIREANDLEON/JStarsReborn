@echo off
rem JStarsReborn 0.3.0 (build w): start-up check, self-test, a short heap-guarded game run, then a full Free Battle with scripted input at full speed; audio captured to WAV.
rem Needs bin\jstars-boot-gpu-0.3.0-w.exe, bin\jstars-boot-gpu-0.3.0-whg.exe and tests\pad-free-battle6.txt next to this file (E:\JStarsWork layout).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-gpu.ps1" -ExeName jstars-boot-gpu-0.3.0-w.exe -HeapGuardExe jstars-boot-gpu-0.3.0-whg.exe -HeapGuardSeconds 60 -SkipValidation -SkipNoPad -NoCount -PadScript E:\JStarsWork\tests\pad-free-battle6.txt -GpuSeconds 620 -SampleEvery 20 -CaptureFlips 30 -CaptureInterval 90 -CaptureMax 320 %*
pause
