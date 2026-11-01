# Dependency matrix: what still needs the original executable

One row per subsystem. "Original dependency" means the shipped build still executes the game's own code (or its
bundled PS4 modules) for this. "Replacement" is code in this repository. Nothing here is a plan; it is the state
at the commit that last touched the row.

Legend: **own** = implemented in this repository and exercised · **partial** = implemented, gaps listed ·
**original** = the game's own code does it · **absent** = neither exists yet.

| Subsystem | Original dependency | Replacement | Tests | Blockers |
|---|---|---|---|---|
| Image loading, relocation, import binding | none | **own** (`src/prepare`, `runtime/jstars_loader.c`) | native preparation is byte-identical to the Python reference (`tools/prepare_native_check.sh`) | — |
| Threads, mutexes, condition variables, TLS | none | **own** (`runtime/jstars_kernel.c`) | host self-test | — |
| Memory, direct memory, page tracking | none | **own** (`runtime/jstars_memory.c`, `jstars_os.c`) | host self-test | Windows red-zone risk under page-fault tracking (ADR-006), no symptom observed |
| File system, `/app0`, save data | none | **own** (`runtime/jstars_fs.c`, `jstars_savedata.c`) | host self-test; real save created and reloaded on Windows | — |
| System/user services, RTC | none | **own** (`runtime/jstars_system.c`) | host self-test | one fixed local user |
| Input (pad) | none | **own** (`runtime/jstars_input.c`, `jstars_os.c`) | host self-test; controller drove a real match | keyboard not yet seen to work end to end |
| Audio output | none | **own** (`runtime/jstars_av.c`, `jstars_audiohost.c`) | host self-test; 0 underruns in a 90-minute session | nobody has judged how it sounds |
| Graphics | none for the console API; the **renderer is vendored** shadPS4 code | **own integration**, vendored core (`gpu/`) | recorded runs | 4 GiB sparse buffers on a 2 GiB-max device (VUID-06409); no PC rendering path yet |
| Movies | n/a | **absent** (`sceAvPlayer*` is a protocol stub) | — | no decoder |
| Trophies | n/a | **partial**: an explicitly empty set | host self-test | the trophy pack is encrypted and not read |
| Sockets (`libSceNet`) | none | **own** (`runtime/jstars_net.c`), including virtual-port multiplexing | host self-test: 27 checks | the framing between peers is this project's own, so no PS4 interoperability |
| Network status (`libSceNetCtl`) | none | **partial**: state, address, NAT type reported without probing | host self-test | NAT type is reported, not measured |
| NP sign-in (`libSceNpManager`) | none | **partial**: local identity, state callback delivered from `sceNpCheckCallback` | host self-test: 13 checks | no account system; `sceNpCheckNpAvailability` and the request API are unimplemented |
| Matching / rooms (`libSceNpMatching2`) | the game's own code drives it | **absent**: 16 of 18 imports stop the run | — | **current work**; needs contexts, requests, callbacks and a room backend |
| Peer connections (`libSceNpSignaling`) | the game's own code drives it | **absent**: 6 of 8 imports stop the run | — | needs address exchange between peers |
| Leaderboards, stats, store (`NpScore`, `NpTus`, `NpCommerce`, `NpWebApi`) | the game's own code drives it | **absent** | — | not needed for a Friendly Match; scope decision pending |
| Game transport (`eboot.bin+0x2cc710` and its receive thread) | **original** | absent | — | first reconstruction candidate once matching works |
| Battle synchronisation, serialization | **original** | absent | — | not analysed |
| Game logic, modes, progression, UI, AI | **original** | absent | — | not analysed |
| Mod loading, texture replacement | n/a | **absent** | — | after online |

## How a row moves

A row moves from *original* to *own* only with: recorded behaviour of the original, a buildable replacement, unit
tests for normal and failure paths, differential testing against captured inputs, and then a real-game run. Until
the final gate, the original path stays available behind a switch.
