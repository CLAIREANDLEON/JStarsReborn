@echo off
cd /d "%~dp0"
echo started %date% %time% > launcher.log
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-all.ps1" >> launcher.log 2>&1
echo exit %errorlevel% >> launcher.log
