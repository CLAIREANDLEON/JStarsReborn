# JStarsReborn checkpoint 0.3.0 (unpublished; nothing has been published)

**Build revision:** source tree digest `3bfc3e5c7bba2093` (see `SOURCE_REVISION.txt`; there is no git history). Runtime under test: `bin/jstars-boot-gpu-0.3.0-w.exe`, sha256 `37207a32…24a2`.

## Highest game-execution milestone (executed on Windows 11, Ryzen 7 9800X3D / RX 9070 XT)
The game's original code, running natively from your own decrypted dump, got through the whole Free Battle path with every frame rendered by the shadPS4-derived Vulkan renderer from the game's own graphics submissions: title → save prompt → main menu → Free Battle → character select → stage select → battle intro → live fight with HUD → K.O. → Victory Burst → victory pose → "Fight the battle again?" → post-match menu. 620 s run, 18,609 flips, 8.88 M draws, no crash (exit 7 = time limit). **Input was scripted test input, not a controller. Nobody has played it or listened to it.**

## What executes what
* **Original game code, unmodified (relocated only):** eboot.bin and the game's bundled libc.prx / libSceFios2.prx (94 of 477 imports resolve into them).
* **Native replacements for console OS services:** threads, synchronisation, time, memory, files, events, system/user services, save data, trophies (explicitly empty set), audio output (WASAPI), pad input (scripted/XInput/keyboard).
* **Translated console behaviour:** the Gnm command stream (PM4) and GCN shaders are processed by the vendored shadPS4 command processor, shader recompiler (to SPIR-V) and Vulkan renderer.
* **Not done:** movies (AvPlayer is a protocol stub; nothing decoded or shown), trophies (empty set, nothing can unlock), online/NP features (offline answers or trap).

## What changed since the verified 0.2.2 checkpoint
Renderer integration (pinned shadPS4 b4e7ae77; licences and boundaries in `gpu/VENDOR.md`, `THIRD_PARTY_NOTICES.md`), audio, input, AvPlayer/trophy/save-data handling, diagnostics, runners, and **the fix of a ~2 % intermittent start-up crash**: the game's libc writes into the object behind every pthread mutex; the host object was too small on Windows and corrupted the heap. Found with the new guard-page heap build; 0/200 failures after the fix vs 3/200 (v) in the same loop. Details: `docs/ADR-006-…`, `evidence/windows-gpu-0.3.0/README.md`.

## Verification of this exact binary (run `20261101-070533-PC-58b01c`)
`--stop-at-first-gnm` exits 3 at `0H2vBYbTLHI#libSceGnmDriver` (eboot.bin+0x45e3a9) · host self-test 134/134 · start-up loop 200/200 exit 3 (run `…070254…246a53`) · heap-guard build: start-up 200/200 and a 60 s game run without a trap · 620 s Free Battle, exit 7. Import audit (`docs/IMPORT_AUDIT.md`): **477 unique import-table entries** (not the 26,919 relocation records): 94 original code, 58 implemented-verified, 145 partial, 88 diagnostic stubs, 92 unsupported (trap, exit 3, named); 131 called in the recorded runs.

## Package contents
`bin/` the two executables · `windows_verify/` runners (`run-gpu.cmd/.ps1`, `regress-loop.cmd/.ps1`) · `tests/pad/` scripted-input files · `docs/` import audit, ADR-006 · `CHANGELOG.md`, `THIRD_PARTY_NOTICES.md` · `evidence/windows-gpu-0.3.0/` logs, steps, hashes, crash investigation · `source/JStarsReborn-0.3.0-source.tar.gz` + `source-tree.sha256` · `SHA256SUMS.txt`. No game content, keys, firmware or decrypted files are included (game frames/audio are kept apart from the package).
`JStarsReborn.exe`, `JStarsSetup.exe` and `checkpoint-manifest.json` are the 0.2.2 artifacts and were **not rebuilt**, so they are not part of this package.

## Remaining blockers and open items
1. Human control is untested: nobody has driven the game with a real controller or the keyboard (runner without `-PadScript`).
2. Audio quality unjudged: the game's own mix exceeds full scale in battle (single channel up to 2.67); a soft limiter is applied (engaged in 66 of 126 five-second windows; ~0.25 % of samples at full scale).
3. Renderer: ~70 `Bc5UnormBlock … Storage not supported` errors per battle run (creation continues; textures not audited); Vulkan validation 4 GiB-buffer VUID not investigated.
4. Red-zone protection on Windows does not exist (no symptom seen); crash `ace318` unexplained (possibly the fixed bug).
5. Movies, trophies, online features, other game modes untested/unimplemented; second-launch save load with build w not retested.
