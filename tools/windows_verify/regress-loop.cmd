@echo off
rem Start-up regression loop: builds interleaved, 200 iterations each (exit 3 expected from --stop-at-first-gnm); logs of failing iterations are kept.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0regress-loop.ps1" -Count 200 -ExeNames jstars-boot-gpu-0.3.0-w.exe,jstars-boot-gpu-0.3.0-whg.exe
pause
