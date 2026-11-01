# JStarsReborn 0.4.0 launcher: Windows test record

Machine: the owner's Windows 11 PC (Ryzen 7 9800X3D, RX 9070 XT, OS build 26300), game dump `E:\CUSA01593` (read-only),
packaged copy unzipped to `E:\JStarsWork\JStarsReborn 0.4.0\` (a path with spaces). Data folder `%LOCALAPPDATA%\JStarsReborn`.
User paths in the files here are replaced by `<user>`. Build w (0.3.0) and its evidence are unchanged
(`evidence/windows-gpu-0.3.0/`).

Binaries under test (final package): `runtime\jstars-runtime.exe` sha256
`25bc88b9ae23afb0abb38f6904ded69ea4ccb5003dd01ac2cc2b17badbda87e0` (runtime 0.4.0, used from rc2 on; rc1 had build y).
Launcher `JStarsReborn.exe` sha256 `e5bec69483d33ee5735a8cd01f8079a7dd485413b0dd0cde725d09f21b48960e`. The launcher went
through release candidates 1 and 2 and the final code; the 15:05-15:13 UTC tests below ran the final code linked with a
time stamp (sha256 `4d43b341...`), which differs from the shipped binary only in the 7 bytes of the PE time stamp and
checksum (the link is now reproducible, `-Wl,--no-insert-timestamp`); the shipped binary itself was then started, used
for a Play and closed (session 20261101-152720, exit 0). All 29 files of the deployed folder match `SHA256SUMS.txt`.

## What was done on the packaged copy, and what was seen

| Check | How | Result |
|---|---|---|
| First run, data migration | rc1 started with 0.2.2 data in `%LOCALAPPDATA%\JStarsReborn` | earlier data copied to `backup-before-launcher-0.4.0-20261101-143643`, originals untouched (0.2.2 `settings.ini` hash unchanged); 0.2.2 window size imported (`launcher.log`) |
| Default game folder | first run, `E:\CUSA01593` present | validated and selected automatically; the dump's own `sce_sys\icon0.png` shown, read locally |
| Invalid folder | Browse to `E:\<other-folder>` (a folder above a dump) | readable problems listed, Play disabled; choosing `E:\CUSA01593` again made it Ready (`launcher.log` 14:45-14:46). Since then the launcher also names the subfolder that holds the game (unit test) |
| Preparation | first Play (rc1) and after Delete prepared data (final) | native preparation in about a second, record `inputs.txt` written last; the rebuilt `boot-linked.bin` has the same SHA-256 as before (`8d356ebe...`) |
| Normal Play | every session | `session.json` mode: live input, no scripted input, no time limit, no capture; command line has no `--pad-script`, `--max-seconds`, capture or diagnostic switches; no command window appeared |
| Normal exit | game window X, and launcher Stop game (WM_CLOSE) | exit 0, "Game closed." in the launcher and `session.json` (sessions 143743, 143934, 144715, 150628, 151309) |
| Relaunch / reuse | several Plays | prepared data reused without re-preparation; launcher back to Ready after each session |
| Duplicate launch | second `JStarsReborn.exe` start while the first was open (minimised) | the existing window was restored; no second instance |
| Settings persistence | window 1600x900 and volume 70 set in rc2; launcher closed and the final build started | Settings page showed 1600 x 900 and 70 %; both on the command line of later sessions |
| Readable failure report | prepared boot image deliberately replaced by a consistent but invalid one, then Play (session 151216) | runtime exit 2 `FATAL: bad boot file signature`; launcher: "The game could not start ... The prepared data seems damaged: use Troubleshooting > Delete prepared data, then press Play again." plus "Open the session logs?" (Yes opened the session folder); Delete prepared data, Play: re-prepared and started normally (151309) |
| Diagnostic report | Troubleshooting, rc2 | ZIP valid (report.txt, launcher.log, newest sessions); no user name, computer name or profile path inside; final build: save dialog starts in Documents and refuses the game folder (tried `E:\CUSA01593\report-test.zip`: refused, nothing written there) |
| Keyboard navigation | Tab to Play, Enter (final build) | the game started |
| Live controller | the owner played in session 144715 | XInput picked up at 217 s; about 700 logged transitions (`session-20261101-144715-pad.log`); save data created, menus, a Free Battle to its result screen; second launch loaded that save (session 150628 mounted SYSTEM-DATA, main menu showed the JP total) |
| Keyboard | Enter at the title (sessions 144715 and 150628) | presses reach the runtime (`pad: live input buttons -> CROSS` with source keyboard), but the game was not seen to leave the title on a keyboard press; in 150628 a controller was connected, so the keyboard was not the source. Not shown to work end to end |
| Audio output path | session 144715 | `sceAudioOutOpen ... [host audio device: Headphones (2- <audio device>)]` (the Windows default device, WASAPI shared mode); 133 five-second meter windows, 0 underruns, soft limiter active in 61 windows. Nobody in this project has judged how it sounds |
| Force end of a hung game | - | implemented (10 s after WM_CLOSE the launcher asks before TerminateProcess); not exercised, no hang occurred |

## Runtime verification of the final runtime

See `run-gpu26-summary.txt` (runner `tools/windows_verify/run-gpu.ps1` with the release runtime copied as
`bin\jstars-boot-gpu-0.4.0-z.exe`).
