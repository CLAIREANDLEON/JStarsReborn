JStarsReborn 0.4.0 - unofficial Windows PC port runtime for J-Stars Victory VS+ (PS4, CUSA01593 version 01.02)
====================================================================================================

Legal (read DISCLAIMER.txt)
  Unofficial fan project. Not affiliated with, authorized or endorsed by Bandai Namco Entertainment, Spike Chunsoft,
  Shueisha, Sony Interactive Entertainment or any other rights holder; all game names and content belong to their owners.
  Use only a dump you made yourself from a copy of the game you legally own. Piracy is not supported in any form.
  Provided as is, without warranty; use at your own risk.

What this is
  JStarsReborn runs your own, already-decrypted copy of the PS4 game on Windows. The game's original program code
  runs natively; JStarsReborn replaces the console's system services, graphics (Vulkan), audio and input.
  No game files, keys or firmware are included, and none are downloaded.

You need
  - Windows 10/11 64-bit and a graphics driver with Vulkan 1.3 (any current AMD/NVIDIA/Intel driver).
  - Your own decrypted dump of J-Stars Victory VS+ CUSA01593 with the 01.02 update applied: a folder with
    eboot.bin, sce_sys\param.sfo and sce_module\libc.prx + libSceFios2.prx. Other versions are refused.
  - An Xbox-compatible controller is recommended. The keyboard also works (see the Controls page).

How to play
  1. Unzip this folder anywhere (paths with spaces are fine). Do not separate JStarsReborn.exe from the runtime folder.
  2. Start JStarsReborn.exe.
  3. On "Game folder", click Browse and choose your game folder. The launcher checks it (read-only).
  4. Press Play. The first time, the game is prepared (a few seconds); then the game window opens.
  5. Close the game window to end the session. The launcher shows how the session ended.

Where your data goes
  %LOCALAPPDATA%\JStarsReborn  - settings (launcher.ini), saves (user\), prepared data (prepared\),
  session logs (sessions\<date-time>\), launcher log (logs\). Your game folder is never written to.
  On first start, data left there by an earlier version is copied to a backup-before-launcher-... folder first.

Problems
  Troubleshooting page: open the latest session logs or create a diagnostic report (a ZIP you save yourself;
  it is never uploaded and contains no game files, saves or prepared data).

Not supported in this version
  Movies are skipped. Trophies and online features are not available. Only the title screen, menus, Free Battle
  and the J-Adventure world map have been exercised; other modes are untested. Controller/keyboard play and the
  audio have not yet been confirmed by a human tester. See CHANGELOG.md for the full list of known issues.

Contents
  JStarsReborn.exe            launcher (this is what you start)
  runtime\jstars-runtime.exe  game runtime (started by the launcher; not meant to be started by hand)
  DISCLAIMER.txt              legal disclaimer (please read)
  LICENSES\                   license texts and third-party notices (GPL-2.0-or-later as a whole)
  source\                     complete corresponding source code and build instructions
  SHA256SUMS.txt              checksums of every file in this folder
  VERSION.txt                 versions and build information
