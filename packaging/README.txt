JStarsReborn 0.5.4 (playtest preview) - unofficial Windows PC port runtime for J-Stars Victory VS+ (PS4, CUSA01593 version 01.02)
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
  - A controller is recommended: Xbox-compatible (XInput), or a DualSense / DualShock 4 over USB or Bluetooth (no DS4Windows
    or Steam Input needed). The keyboard also works (see the Controls page).

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

Saves
  The launcher's Saves page imports a save (a SAVE-DATA.DAT, or a folder with one: another JStarsReborn install, a shadPS4 user
  folder, a decrypted PS4 save), unlocks every character in one click, and restores earlier saves. Every change backs up the
  current save first (%LOCALAPPDATA%\JStarsReborn\save-backups). Start the game once to create its save, and close the game first.

Fullscreen and ultrawide (Settings, off by default)
  "Fullscreen" opens the game borderless over the whole monitor; Alt+Enter or F11 switches between fullscreen and the window.
  "Smooth pacing" (on by default): in fullscreen, a 144/165/175 Hz monitor runs at 120 Hz while the game plays, so 30/60 fps
  frames are shown evenly; it switches back when you leave fullscreen or close the game.
  "Ultrawide / widescreen" (experimental): on screens wider than 16:9 the game shows more to the sides instead of black bars.
  The HUD and menus are stretched for now. Applied only after the runtime checks the game build.

60 fps (experimental, off by default)
  Settings > "60 fps (experimental)". The game runs its own engine at 60 instead of 30; it is applied only after the runtime
  checks the game build (otherwise the game runs at 30 as usual). Battle speed is correct at 60; some animations or effects
  may still be off - please report them with a diagnostic report. 120 fps is not offered.

Online (experimental, still in testing, off by default)
  Online play is peer-to-peer: the two games talk to each other directly. There is no JStarsReborn server and nothing
  contacts PlayStation Network; no account is used. Turn it on in the launcher's Online page.
  - Same network: rooms are found automatically.
  - Over the internet: the host presses "Get my room code" (the launcher asks the router to open UDP ports 47391 and
    3658 with UPnP) and sends the code to the other player, who types it into "Friend's room code". If the router has
    no UPnP, forward UDP 47391 and 3658 to the host PC by hand and use the host's public address instead of a code.
  - Then both press Play > Online Battle > Friendly Match: the host chooses Create a Room, the other player Search for a Room.
  - Windows may ask to allow JStarsReborn on networks: allow it.
  What works: hosting a room, and seeing other players' rooms (0.5.2 fixes the crash when a search found a room).
  What does NOT work yet: joining a room, so a match
  between two players cannot start yet; Rank Match and Quick Match. Unfinished functions make the game show
  "A communication error has occurred" and return to the menu.

Not supported in this version
  Movies are skipped. Trophies are not available. Online play is the experimental test described above. Only the title screen, menus, Free Battle
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
