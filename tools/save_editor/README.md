# Save editor

> Since 0.5.3 the launcher's **Saves** page does this without PowerShell (Unlock every character, import, backups and restore,
> `src/launcher/saves.cpp`). This script stays for advanced use (`gauges`, custom values).

`jstars-save.ps1` edits the save that JStarsReborn writes, so you can play every character without clearing Story mode
first. It only touches your own local save; no game files are read or changed.

## Use

1. Start the game once so it creates its save, then close it. The script refuses to run while `jstars-runtime` is open.
2. In PowerShell, from this folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\jstars-save.ps1 -Action all
```

3. Start the game again. Every character is unlocked and owned, and J-Points are set to 999,999.

A backup named `SAVE-DATA.DAT.bak-<date>-<time>` is written next to the save before every change. To undo, close the game,
delete `SAVE-DATA.DAT` and rename the backup to `SAVE-DATA.DAT`.

| Action | What it does |
|---|---|
| `check` | Reports whether the checksum is valid. Changes nothing. This is the default. |
| `all` | `unlock` plus `owned`. |
| `unlock` | Sets the character and support unlock flags and J-Points. |
| `owned` | Sets the owned-character flags that the J-Points shop sets when you buy a character. |
| `gauges` | Fills the three shop gauges. |
| `fix` | Only recomputes the checksum. |

Use `-Save <path>` for a save in another location.

## Format notes

The save is `%LOCALAPPDATA%\JStarsReborn\user\savedata\1\CUSA01593\SYSTEM-DATA\SAVE-DATA.DAT`, unencrypted.

- Checksum: `S` is the sum of the little-endian 16-bit words from offset `0x8` to the end of the file. The 32-bit value at
  `0x0` is `S >> 8` and the 32-bit value at `0x4` is `S & 0xFF`.
- `0x5C40`: character unlock flags, 52 bits. `0x5C48` and `0x269B8`: support character flags, 13 bits.
- `0x5C58`: total J-Points earned. `0x5C5C`: current J-Points.
- `0x5C60` and `0x269A0`: the three shop gauges, three 32-bit values each.
- `0x5C6C`: owned-character flags. 46 bits set gives every battle character.

Confirmed in game on JStarsReborn 0.5.1 with CUSA01593 version 01.02.
