# Windows evidence, checkpoint 0.3.0 (build w)

Machine: Windows 11 Pro 10.0.26300, Ryzen 7 9800X3D, Radeon RX 9070 XT (driver 32.0.32015.7029), Vulkan loader 1.4.357.0.
Input: the user's own decrypted dump `E:\CUSA01593` (eboot.bin sha256 in `run-*/input-sha256.txt`), read-only.
Everything here was produced by the runners in `tools/windows_verify/` (every run writes a new folder; nothing is overwritten).
Large logs are gzipped; nothing was edited.

## `run-20261101-070533-58b01c/` — the verification run of the checkpoint binary

Runner `run-gpu22.ps1`, binaries in `binaries-sha256.txt` (`jstars-boot-gpu-0.3.0-w.exe` 37207a32…, heap-guard build `…-whg.exe` ef7765b0…).

| Step | Exit | What it shows |
|---|---|---|
| `regress-stop-at-first-gnm` | 3 | The preserved 0.2.2 baseline: stops at `0H2vBYbTLHI#libSceGnmDriver` from `eboot.bin+0x45e3a9`. |
| `selftest-host` | 0 | 134 checks, 0 failed (includes the new "guest writes into the pthread objects" checks). |
| `audit-dump` | 0 | Table behind `docs/IMPORT_AUDIT.md`. |
| `heapguard-regress-stop-at-first-gnm` | 3 | Same check with every host allocation guard-paged. |
| `heapguard-game-gpu` | 7 | 60 s of the game (copyright screen, title, scripted menu input, 1,799 flips) with guard-paged heap: nothing trapped. |
| `game-gpu` | 7 | 620 s full-speed run with scripted input (`tests/pad-free-battle6.txt`): title, save prompt, main menu, Free Battle, character select, stage select, battle (intro cinematic, fight, K.O., Victory Burst, victory pose), "Fight the battle again?" and the post-match menu. 18,609 flips, 8.88 M draws, 1.26 M dispatches. Exit 7 = `--max-seconds` reached, not a crash. |

The game frames and audio themselves are game content and are **not** kept in this repository or in the package (they were handed to the owner separately; the run folder `E:\JStarsWork\results\20261101-070533-PC-58b01c\captures` holds all 207 PNGs). The 11 frames shown to the owner were frames 11 (title), 33 (character select), 45 (battle character intro), 51 (battle intro cinematic), 70 (fight HUD), 100 (K.O.), 150 (fight), 154 (Victory Burst), 159 (victory pose), 180 ("Fight the battle again?") and 206 (post-match menu). Each capture is
the image the renderer produced for a game flip; the `Present`/flip counters, `gpu/pm4-submit-*.bin` dumps and the `GPU stats` lines in the log tie them
to the game's own graphics submissions. The **input was automated** (`SCRIPTED TEST INPUT` lines in the log), not a controller.

Observations that are not clean (also in the CHANGELOG):
* 68 renderer errors `Bc5UnormBlock ... Storage is not supported` (see ADR-006).
* Audio: the game's own mix exceeds full scale (a single input channel peaks at 2.67, the stereo downmix at about 3.2); the soft limiter engaged in 66 of 126 five-second windows and about 0.25 % of the dumped samples sit at full scale. Whether it sounds right has not been judged by a listener.
* Trophies: the unlock call at about 487 s was answered INVALID_TROPHY_ID (trophy set is empty by design).

## `startup-crash-investigation/` — the intermittent start-up crash

`tools/windows_verify/regress-loop.ps1` launches `--stop-at-first-gnm` repeatedly, interleaving builds, and keeps the full log of every run that does not exit 3.

| Loop folder | Result |
|---|---|
| `…064659-…-c1b9f2` | **Invalid**: the exe list arrived as one string, so every launch failed instantly (exit empty, 0.0 s). Kept to show it; it measures nothing. Script fixed. |
| `…065114-…-5cb078` | 30× each: build t 29 exit 3 / 1 exit 4, build v 30 exit 3. |
| `…065850-…-15eba4` | 200× each: t 196/4, v 195/5; heap-guard build (before the fix) 0/200 — every run trapped. |
| `…070254-…-246a53` | 200× each: v 197/3, **fixed build w 200/0**, **fixed heap-guard build whg 200/0**. |

* `crash-build-t-it017.log`, `crash-build-v-it081.log`: the crash (all 13 crashes kept by the loops — 5 on build t, 8 on build v — show this exact signature: access violation at `ffffffffffffffff` in `ntdll.dll+0x27381`, last import `scePthreadAttrDestroy`; the one in run `…063312-…-873ce9` is the same).
* `heapguard-caught-guest-write-past-mutex-block.log`: the guard build's report — `FAULT ... at libc.prx+0x23a20`, `HEAPGUARD: address is in the guard page ABOVE ... a 8-byte block ... allocated from ...`; the instruction is the game libc's `or dword [rax+0x20], 0x100` on the mutex object.
