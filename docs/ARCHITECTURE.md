# Architecture and recorded decisions

## ADR-001: native Windows foundation

Use C++20/CMake and Win32 + DX12 for the initial Windows-only target. DX12 is available through the native OS and open MinGW headers/import libraries or the Windows SDK. This avoids an engine dependency before content formats are known. Vulkan is not rejected on performance grounds; no comparative benchmark exists. The [Microsoft initialization guide](https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-a-basic-direct3d-12-component) provides the API lifecycle reference.

The runtime owns COM, the HWND and event loop. Rendering owns DXGI/D3D12 COM resources. Audio owns XAudio2 voices and the tone buffer; voices are destroyed before buffer storage. Input polls XInput and owns any vibration it requested. Core owns bounded settings parsing, atomic writes, thread-safe logging and the fixed-step clock. Only the main thread touches graphics and window state. One allocator and conservative fence waits prioritize initial correctness; this is not an optimized game renderer.

## ADR-002: no guessed gameplay

FixedStep emits a step count and interpolation fraction; main records the count without attaching fabricated physics/combat. The 60 Hz default is provisional. Reference measurements choose the real game simulation cadence later. Render cadence never directly scales a gameplay update. Excessive elapsed time is explicitly dropped under a bounded catch-up policy; online synchronization policy remains undecided.

## ADR-003: input/audio dependency plan

Use system XInput and XAudio2 for a small verifiable native base. A future pinned SDL3 dependency can be evaluated for DS4/DualSense mappings and hot-plug coverage; [SDL documents its gamepad mapping layer](https://wiki.libsdl.org/SDL3/CategoryGamepad). No SDL code is currently linked. Physical device behavior needs hardware tests, independent of normalization unit tests.

## ADR-004: data and mods

Inventory local inputs without mutation or decryption. Signature hints never authorize parsing. Verified importers must validate sizes, offsets, budgets, provenance and bounds before producing engine-owned intermediate data. Keep original files immutable. A future texture system should accept a verified standard container with explicit mip/format/dimension budgets, stable asset IDs and fallback behavior. Its actual parser/streamer is not implemented.

The mods/ directory is reserved; the runtime currently neither scans nor executes mods. Manifest/schema stability must follow an actual loader and tests. Do not ship a decorative example as a working mod. Proposed loader requirements remain the user's directive: dependency ordering, conflict reports, explicit cosmetic/gameplay classification, bounded scripts and compatibility fingerprints. No native plugins or scripts are executed by this build.

## Module state and next dependency gates

| Module | Current implementation | Next gate |
| --- | --- | --- |
| platform | Window lifecycle, timing, diagnostics controls | Suspend/resume/display/input integration tests |
| core | Logging, settings, fixed-step scheduler | Log rotation, crash dumps, save integrity layer |
| rendering | DX12 clear/present, resizing, validation switch | Real mesh/material fixtures and GPU validation evidence |
| input | XInput normalization/polling/owned rumble | Physical matrix, remapping, PlayStation devices |
| audio | XAudio2 ownership and procedural tone | Bounded standard audio decoder and streaming |
| assets | Read-only inventory tool | Authorized inputs and verified formats |
| animation/gameplay | Not implemented | Reference specifications and imported content |
| modding | Not implemented | Asset IDs, loader schema, compatibility semantics |
| networking | Not implemented | Measured combat determinism and sync requirements |

## Networking decision gate

Do not choose rollback merely because the genre is combat. First record deterministic state traces across repeated input playback and supported machines; quantify divergence and nondeterministic systems. Then compare server-authoritative reconciliation and deterministic rollback with measured latency and memory budgets. Dedicated server, lobby/authentication, rate limiting and UDP packet parsers require real implementations and adversarial tests before any online claim. No PSN access or PS4 crossplay is part of this build.
