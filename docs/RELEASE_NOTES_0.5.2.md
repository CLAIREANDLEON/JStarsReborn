**Pre-release / playtest build.** Thank you to everyone who sent feedback for 0.5.1.

### Fixed from your reports
- **Online room search crash:** the second player's game stopped when it found the host's room. Fixed. *Joining* a room is still not
  implemented, so a two-player match cannot start yet; that is the next online milestone.
- **Choppy / scratchy audio:** the audio feed now gets real-time priority from Windows and recovers more smoothly after a dropout. We
  could not reproduce the problem on our test PC, so please tell us whether it is better for you.
- **"An error has occurred" situations** (for example online errors) now return to the menu instead of closing the game.

### New: 60 fps (experimental)
- Settings > **60 fps (experimental)**. The default is still the game's own 30 fps.
- The game's battle timer runs at the correct speed at 60 fps. Some animations, effects or menus may still be too fast or too slow:
  please report what you see.
- 120 fps is not offered: the game's internal timers cannot run exactly at 120. The plan is 60 fps plus frame generation.

### Please attach logs when something goes wrong
In the launcher, open **Help > Create diagnostic report…** and attach the zip to your report (it removes your user name and computer name).
Without it we cannot tell why the game "won't run" or stutters on a given PC.

### Requirements
Windows 10/11 x64, a Vulkan 1.3 GPU driver, and **your own legally obtained, decrypted dump** of J-Stars Victory VS+ CUSA01593 v01.02.
No game files, keys or firmware are included. See DISCLAIMER.txt. Piracy is not supported.

Not affiliated with or endorsed by the game's publisher, developer or rights holders. Provided as is, without warranty.
