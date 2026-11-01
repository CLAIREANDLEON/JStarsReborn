# Feasibility assessment — 2026-10-31

## Evidence available

The inspected starting workspace contained empty `outputs/` and `work/` folders. The supplied attachment was the development directive. No original source tree, game files, authorized SDK package, PS4 captures, file-format specifications, or gameplay measurements were supplied in those materials. This is a statement about the inspected inputs, not every file on this computer.

The foundation in this repository is newly written native application code. It does not load a console executable, emulate a console, or implement original combat. Build and execution evidence belongs in the project test report; source existence alone is not a passed milestone.

## Assessment

A native Windows runtime can be developed independently now. A faithful complete game cannot presently be specified or validated: the data formats, simulation behavior, animation semantics, content rights, and original platform dependencies remain unknown. No completion date or whole-game feasibility finding is justified by the available evidence. A PS4 executable is not portable C++ source and cannot simply be recompiled for Windows.

Two possible routes require different inputs:

1. **Authorized source port:** supplied source, build dependencies, platform abstractions, content export tools, and appropriate permissions. Review the actual source before estimating the replacement work.
2. **Independent implementation:** authorized observations and local data, independently documented formats and behavior, and specifications separated from implementation. No protected executable code is copied or translated. This route has not yet been shown sufficient to reproduce this game.

## Blocking inputs and acceptance gates

| Missing input | Work it blocks | Acceptance evidence |
| --- | --- | --- |
| Authorized source/build materials, if available | Direct source-port route and platform dependency mapping | Source inventory and a reproducible original or documented reference build |
| Legitimately supplied accessible local data and redistribution scope | Actual character, stage, animation, UI, and audio integration | Read-only inventory, provenance record, verified format specifications and bounded parser fixtures |
| Reference release/build identity and lawful PS4 captures | Accurate timing, modes, movesets, camera, AI and presentation | Repeated measured trials using REFERENCE_PROTOCOL.md |
| Animation, collision, skeleton and material semantics | Faithful simulation/rendering integration | Measured mappings and tested conversions for representative content |
| Determinism and network-behavior measurements | Rollback versus server-authoritative decision | Repeatable input/state traces with divergence analysis |
| Test hardware and real controller access | Performance targets and physical-device claims | Executed hardware matrix, frame-time captures, hot-plug and input tests |

Receiving an encrypted or inaccessible package does not authorize or enable a decryption implementation. The inventory tool does not decrypt, unpack, execute, or import it. Unknown bytes remain unknown.

## Clean-room working procedure

Record each input's origin, permission scope, release/version, and SHA-256. Keep source material local and separate from redistributable outputs. A specification researcher records externally observable behavior, units, confidence, reproducible steps and evidence IDs. Implementers consume the independent specification and approved standard-format fixtures. Keep unresolved findings explicit rather than filling gaps with generic combat. If a source-access route is authorized, record that route explicitly rather than calling it clean-room.

Each verified format gets its own written specification, bounds and size limits, positive/negative fixtures, parser tests, and conversion rules. Quarantine unknown or malformed files. Do not publish reference captures, manifests containing proprietary content, or game assets merely because the runtime code is distributable.

## Architecture and dependency decision

The initial native backend uses Win32 and Direct3D 12, with native input/audio foundations. This fits the Windows-only target and permits a graphics-device/swap-chain test without a third-party engine or proprietary SDK. Vulkan remains a future backend candidate; no measured cross-backend performance comparison has been made. Microsoft documents the [basic Direct3D 12 initialization flow](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-basic-direct3d-12-component) and [XAudio2 initialization and playback](https://learn.microsoft.com/en-us/windows/win32/xaudio2/full-project).

Keep platform services, rendering, input, audio and core lifecycle separate. Gameplay, animation, asset conversion, network services and mod execution are subsequent modules with evidence gates; empty interfaces do not count as implementations. C++20/CMake with either LLVM-MinGW headers/import libraries or the Windows SDK form the initial native dependency set; the read-only inventory and its tests use Python's standard library. Future third-party libraries require an exact version, license review and reproducible acquisition before inclusion.

The fixed-step rate is a configurable foundation value, not a verified PS4 combat rate. A clear-screen renderer and a successful native launch establish runtime plumbing only. They are not a playable port, texture pipeline, stable game, or performance result.
