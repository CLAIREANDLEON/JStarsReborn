**Pre-release / playtest build.** If you skipped 0.5.3, everything from it (the Saves page, 60 fps, the 0.5.1 fixes) is included;
see the end of these notes.

### Smoother frame pacing (everyone)
Frames used to alternate between 16 and 31 ms because the renderer's frame loop could only wake on Windows' 15.6 ms timer tick, which
felt like stutter even when the fps counter said 60. The runtime now uses 1 ms timer resolution and a high-resolution timer.
On our test PC at 60 fps the 95th-percentile frame time went from 30.6 ms to 17.3 ms (a steady 60 fps now looks like 60 fps). It also
helps at the default 30 fps.

### New: Ultrawide / widescreen (experimental)
- Settings > **Ultrawide / widescreen**. On a screen wider than 16:9 (21:9, 32:9) the game shows more to the left and right instead of
  black bars; the vertical view is unchanged. At 3440x1440 the horizontal field of view goes from 60 to about 76 degrees.
- The HUD, menus and movies are stretched for now (they are drawn into the same picture). The tested game-only output is 1280x720, scaled to the window.
- Applied only after checking the game build; on anything else the game keeps its normal 16:9 picture.
- Please report anything that pops in at the screen edges.

### New: Fullscreen
Settings > **Fullscreen** opens the game borderless over the whole monitor. **Alt+Enter** or **F11** switches between fullscreen and
the window at any time.

### New: PlayStation controllers work directly
DualSense, DualSense Edge and DualShock 4 work over USB or Bluetooth without DS4Windows or Steam Input: all buttons, analog triggers,
the touchpad click and rumble. Thanks to CLAIREANDLEON for this. Tested with a DualSense over USB; please tell us how Bluetooth and the
DualShock 4 behave. Xbox (XInput) controllers and the keyboard work as before.

### Better diagnostic reports
Troubleshooting > **Create diagnostic report** now includes the start of the game log (GPU and driver details) and your 60 fps /
fullscreen / ultrawide settings for each run, and removes more personal details (folder paths, a custom player name, friends' room
addresses). Please still look at the report before sharing it.

### Also fixed
- A minimized game no longer slows down.

### From 0.5.3 (if you skipped it)
- **Saves page:** import a save (file or folder), unlock every character in one click (J-Points 999,999), restore backups.
- **60 fps (experimental):** Settings > 60 fps. Battle speed is correct; some animations or effects may still be off.
- **Online room search crash fixed** (joining a room is still not implemented), real-time audio priority for the "choppy audio"
  reports, and error dialogs return to the menu instead of closing the game.

### Not in this build yet
FSR 4 upscaling and frame generation are being worked on; they need data from the game's renderer (depth, motion) that we are
still mapping. Online matches between two players do not work yet.

### Please attach a report when something goes wrong
Launcher > **Troubleshooting > Create diagnostic report…** and attach the zip (it removes your user name and computer name).

### Requirements
Windows 10/11 x64, a Vulkan 1.3 GPU driver, and **your own legally obtained, decrypted dump** of J-Stars Victory VS+ CUSA01593 v01.02.
No game files, keys or firmware are included. See DISCLAIMER.txt. Piracy is not supported.

Not affiliated with or endorsed by the game's publisher, developer or rights holders. Provided as is, without warranty.
