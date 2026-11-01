# ADR-005: Execution runtime for J-Stars Victory VS+ (CUSA01593)

Status: **Accepted by the owner's direction (2026-10-31)**, implementation in progress. Nothing has been published.

## Decision

JStarsReborn follows the architecture of the inFAMOUS Second Son PC-port projects
(Castlesea/infamous-second-son-pc-port, HayatoG/iss_pc): a dedicated Windows application that loads the
user's own game folder, runs the **original x86-64 game code natively**, and supplies the console's operating
system, graphics, audio and input services itself. Full source reconstruction is not required. Reverse
engineering is used only for concrete blockers and every reconstructed routine must be verified against the
original.

## Why this fits J-Stars (observed, not assumed)

| Fact | Evidence |
|---|---|
| The dump's `eboot.bin` is a plaintext SELF whose segments carry no encryption flag, wrapping an x86-64 ET_SCE_DYNEXEC ELF | `evidence/execution/`, `validation.json` |
| The game ships its own `libc.prx` and `libSceFios2.prx` (the part of the PS4 system software a loader would otherwise need from firmware) | `sce_module/` |
| Linked image: eboot + libc.prx + libSceFios2.prx = 477 imports, 94 bound to the game's own modules, 383 for the host | `link.json` |
| The original libc and Fios2 initializers return 0 and the eboot entry point executes | `evidence/execution/boot-*.log` |

## Component decisions

| Component | Decision | Notes |
|---|---|---|
| SELF/ELF parsing, image layout, relocation, import inventory | **Adapt** (from iss_pc `prepare.py`, `link_modules.py`, `link_libc.py`) | Title-specific parts removed (resource directories, magic renamed to `JSTBOOT*`). Output goes to a work store keyed by title, version and eboot hash. |
| Loader, import binding, trap-on-unimplemented | **J-Stars specific**, informed by iss_pc `probe.c` | Unimplemented imports stop the process with NID, name, library and caller. No fake success values. |
| Kernel services (threads, sync, TLS, time, memory) | **J-Stars specific** | Written against what this game's libc/eboot calls. Layouts cross-checked with shadPS4 headers. |
| Pad, VideoOut, AudioOut, system/user services | **J-Stars specific**, backends pluggable | Linux harness has no display, no pad and a discarding audio sink; Windows backends follow. |
| GPU command processing (PM4, GCN shader recompiler, Vulkan) | **Reuse** shadPS4 `video_core` as the reference ports do | Not integrated yet; first blocker (`sceGnmDrawInitDefaultHardwareState200`). |
| Title-specific patches, offsets, "native passes", AOT shaders, gameplay hooks | **Not transplanted** | Belong to inFAMOUS and are meaningless for this executable. |
| Decryption, keys, firmware | **Excluded** | The dump is already plaintext; encrypted or uninspected content is reported, never processed. |

## Licensing consequences

Both reference projects and shadPS4 are GPL-2.0-or-later. Anything adapted from them (currently
`tools/prepare/*`, and the loader structure in `runtime/`) is GPL-2.0-or-later; code that links shadPS4
video_core will be too. A combined binary distributed to others must carry the GPL text and corresponding
source. Per-file notices record provenance. The owner said not to publish anything yet; no distribution has occurred.
Third-party components still to be vetted before any distribution: shadPS4 externals (SDL3, sirit, zydis,
LibAtrac9 and the audio/video decoders it pulls in).

## Execution model

- Host ABI: System V on the guest side. Native on the Linux harness; on Windows every guest-callable host
  function carries `__attribute__((sysv_abi))` (macro `GUEST_ABI`). x87 control word 0x037F on guest threads.
- TLS: libc/Fios2 use ELF dynamic TLS (`__tls_get_addr`, DTPMOD module ids written by the linker). The eboot
  has no PT_TLS. No `fs:` TCB loads were found by the iss_pc pattern (`fs_loads_patched=0`); this is a finding
  about this build, to be re-checked if a later stage faults.
- Memory: direct memory is a shared, offset-addressed backing store so CPU aliases agree; flexible memory is anonymous.

## Consequences / risks

- The GPU path dominates the remaining work and must be proven on hardware with Vulkan; this development
  container has no GPU.
- Windows host layer: VirtualAlloc/VirtualProtect, file-mapping direct memory and a vectored exception handler are written (`jstars_os.c`) and run under Wine. Still open on real Windows: red-zone hazard with Windows exception dispatch (guest code stores below rsp; the iss_pc runtime documents the rewrite), ISA rewriting for CPUs lacking BMI1/MOVBE/F16C/LZCNT, and the GPU driver environment. Direct-memory offsets/addresses are 64 KiB aligned on Windows.
- Network (Np*, Net, Http, Ssl) and online features are out of scope for the first playable build; they will be
  implemented as explicit "offline" results only after being reached and verified.
