# Port architecture: where JStarsReborn is, where it is going, and the boundary between them

## Today (0.4.x): a compatibility runtime

```
   your own decrypted dump                JStarsReborn
   ───────────────────────                ─────────────────────────────────────────────
   eboot.bin            ──┐
   sce_module/libc.prx  ──┼─ preparation ─> boot-linked.bin ─┐
   sce_module/libSceFios2.prx ┘ (src/prepare)                │
                                                             v
   data/*.cpk, movies, icons  ───── read at run time ──> runtime/ (C)
                                                          ├── loader: maps the image, binds 477 imports
                                                          ├── replacements for the console OS: threads,
                                                          │   memory, files, save data, events, user/system
                                                          │   services, audio (WASAPI), input (XInput/keyboard),
                                                          │   network (jstars_net.c)
                                                          └── gpu/ : shadPS4-derived command processor,
                                                              GCN->SPIR-V recompiler, Vulkan renderer
```

The game's own x86-64 code runs natively. Of the 477 import-table entries, 94 resolve into the game's own
bundled modules; the rest are the runtime's. Nothing is emulated at the instruction level.

**This is the reference implementation and the measuring stick, not the destination.** It is what tells us what
correct behaviour looks like, because it is the behaviour of the real game.

## The destination: a source-based PC port

A build whose shipped executable contains no original PS4 executable or proprietary PS4 module code, that loads
the owner's assets as data, and whose game logic and platform layers are maintainable source. The acceptance
gates are in [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).

## The migration boundary

Replacement happens one bounded unit at a time, behind a switch, with the original path kept until the
replacement is proven:

```
   game code (original)  ──calls──>  [ boundary ]  ──> runtime replacement        (today, everywhere)
   game code (original)  ──calls──>  [ boundary ]  ──> reconstructed unit         (migration, unit by unit)
   reconstructed caller  ──calls──>  [ boundary ]  ──> reconstructed unit         (destination)
```

A unit may cross the boundary only when all of this holds, which is the rule the directive sets and this project
adopts: the calling convention, register and stack behaviour, structure layout, pointer identity, allocation and
free ownership, threading, TLS, lifetime, numeric behaviour and timing assumptions are preserved and tested. Mixed
execution is tested before coverage grows.

### Where the boundary is easiest to move first

The import boundary is already a clean, documented interface: 477 entries, each with a known signature, and a
runtime that implements them. Anything *below* that line (the console's services) is already reconstructed source
in this repository. The work ahead is *above* the line, inside the game's own code, and the natural first targets
are the game routines that sit directly on top of an interface we already own:

| Candidate | Why it is a good first unit | Status |
|---|---|---|
| Transport at `eboot.bin+0x2cc710` | One function: creates a socket, sets two options, binds it, starts its receive thread. Its whole interface is `libSceNet`, which this project implements. | Mapped ([FUNCTION_MAP.md](reverse-engineering/FUNCTION_MAP.md)); replacement not started |
| Datagram handler at `eboot.bin+0x2b2410` | The receive thread's only callee; its input is one datagram and its output is game state. Differential testing against captured datagrams is practical. | Not yet analysed |
| Online service start at `eboot.bin+0x2bdd40` | Pure sequencing over interfaces we own; no gameplay state. | Call order recorded |

Rendering is deliberately **not** an early target: the vendored renderer is the one part that is both large and
working, and the handoff asks for it to stay stable while networking lands.

## Rules this project keeps while migrating

1. The playable build stays playable. A replacement ships disabled until it passes.
2. No claim without evidence. "Implemented" means there is a test or a recorded run; everything else is marked.
3. The owner's game files are read, never written, never redistributed, and never committed.
4. Licence boundaries are recorded per vendored component (`gpu/VENDOR.md`, `THIRD_PARTY_NOTICES.md`).
