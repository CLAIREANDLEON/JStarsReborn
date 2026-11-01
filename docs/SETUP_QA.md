# Native setup menu — review and QA checklist

This checklist defines expected behavior. It does not claim these manual checks have been executed; completed results belong in the test report with evidence.

## Meaning of each operation

The setup application installs the implemented native foundation. It does not decompile a PS4 executable or reconstruct missing game systems. Inspection creates a read-only inventory with hashes and signature hints. A completed inventory is not a successful game import. The install operation verifies files against the bundled manifest and copies only the foundation payload into a new folder. Checksums establish package consistency, not publisher authenticity.

Suggested concise UI labels:

| Element | Copy |
| --- | --- |
| Window title | J-Stars Reborn Setup |
| Persistent status | Native foundation — game import is not available yet. |
| Source selector | Choose game folder |
| Inspection action | Inspect files |
| Inspection result | Inventory complete. No compatible game importer is implemented. |
| Destination selector | Choose install folder |
| Install action | Install foundation |
| Successful install | Foundation installed. This is a diagnostic runtime, not the playable game. |
| Launch action | Launch foundation |

The menu must never label unimplemented conversion as Decompile, Build game, or Install full game. It must not identify arbitrary selected files as a verified J-Stars release. Inspection can complete even when import support is unavailable; foundation installation can proceed independently of the source folder.

## Manual interface checks

| Check | Expected result |
| --- | --- |
| First launch at 100%, 150%, and 200% scaling | All controls and status text are readable; no clipped paths or overlapping buttons; unsupported import status remains visible. |
| Keyboard-only navigation | Tab order follows source, inspect, destination, install, report/launch and close actions. Buttons respond to keyboard activation; focused control is visible. |
| Cancel folder picker | Existing selection remains unchanged; no inspection or installation starts. |
| Paths containing spaces and Unicode | Display, inventory, report opening, installation and launch preserve the exact path. |
| Source selection changes after inspection | Old success state is cleared; a prior inventory is not presented as a result for the new folder. |
| Source missing, unreadable, or linked | A useful error is visible; the application remains responsive; source data is not modified or executed. |
| Empty folder or arbitrary synthetic files | Inventory reports actual counts; no game-compatibility or decompilation success is implied. |
| Large inspection | UI remains responsive, real files/bytes update, duplicate work is disabled, and cancellation is available. |
| Destination already exists | Installation is rejected without overwriting files or merging into unrelated user data. |
| Destination overlaps package/source | Invalid paths are rejected before copying; input contents remain intact. |
| Missing or modified payload | Installation fails clearly; no success screen or enabled launch action; owned staging files are cleaned. |
| Cancel during copying | Work stops at a bounded opportunity; final destination is absent unless the commit already succeeded. Cancellation must not delete a committed installation. |
| Close while inspection/install is running | Cancellation is requested; callback state remains alive until the worker finishes; no callback accesses a destroyed window. |
| Successful install | File count/bytes reflect actual copied data; receipt states foundation-only; no game files were silently copied. |
| Launch installed foundation | Exact installed executable launches with the correct working directory; selected PS4/source files are never executed. |
| Launch failure | Missing executable or process-creation error is visible; setup remains usable. |
| Reopen setup | Existing installations are not silently overwritten. A new run does not display stale in-memory completion state. |

## Integration review gates

Use an STA COM apartment for a native IFileOpenDialog folder picker. The setup executable is separate from the runtime's MTA COM session. Catch exceptions at the window procedure boundary. Keep worker exceptions and progress delivery owned until completion; a posted message must not outlive its target state. Disable path changes or snapshot paths atomically when a job begins. Launch using an explicit executable path and safely quoted arguments, not a shell-built command string.

Since 0.2.2 the runtime writes config, logs and crash reports to %LOCALAPPDATA%\JStarsReborn, so a read-only installation directory is supported (not yet tested under Program Files on Windows hardware). Installation should not require administrator privileges or change machine-wide settings. The package should include runtime attribution files and its actual configuration. A local manifest is not a code-signing or update-trust system.

## Exact inputs needed before a full-game installer is possible

Supply authorized source/build/export materials if a source-port route exists, or accessible local game data with provenance plus independently verified format specifications. Identify the PS4 release/build used as reference and provide captures or measurements following REFERENCE_PROTOCOL.md. Animation, collision, skeleton, materials, timing, modes and other gameplay semantics still need specified and tested implementations. A setup menu cannot substitute for those systems.
