# Setup 0.2 — executed tests

Date: 2026-10-31, America/New_York. Same Windows 11 / AMD host and portable Clang toolchain as TEST_REPORT.md. This report concerns the source-intake/setup milestone, not a playable-game conversion.

## Build and automated tests

JStarsSetup.exe compiled and linked as a native Windows x64 GUI application, with its Common Controls and DPI manifest embedded. No Python interpreter or downloaded engine is required to use the menu. Six CTest suites passed: existing core/input/asset tests and the three new native setup suites. Evidence: evidence/setup-ctest.txt.

| New suite | Result | Meaningful coverage |
| --- | --- | --- |
| Native folder inventory | 32 checks passed | SHA-256, Unicode paths, no source changes, file/depth/entry caps, cancellation, exclusive reports, output boundaries |
| Foundation installation | 121 checks passed | Exact copying, hash and size mismatches, traversal/reserved names, duplicate paths, overlap, cancellation/rollback, destination appearing during copy, Unicode |
| PKG intake | 24 checks passed | Synthetic header recognition, metadata hints, truncated/wrong input, cancellation, no overwrite, source preservation, Unicode |

Two optional native symlink fixtures and one existing Python symlink fixture were skipped because Windows denied link creation (error 1314). Reparse indicators and other path rejection behavior were separately exercised. There was no actual game PKG or proprietary-content fixture. No real-package extraction, decryption or conversion was performed.

## Native UI checks executed

The setup window was inspected through its actual Windows controls and screenshot. Text and action buttons were visible with no observed clipping at the tested size. The PKG file chooser opened with the *.pkg filter and cancelled correctly. The synthetic PKG path was entered in the editable source field; selecting a file through the modal picker was not exercised.

Import source produced a new JSON report, displayed the source-registered/conversion-unavailable result, and left the fixture unchanged. Install foundation copied a four-file test payload to a new workspace folder and enabled Launch installed. That launch opened the installed DX12 runtime; it later exited cleanly after 2,188 presents. The original user-opened runtime was not closed. These checks used newly created workspace test folders.

## Final package integration

The full allowlisted delivery payload was installed from the prepared package and its installed executable completed a 60-frame smoke run. A second installation to the same destination failed with exit 1 while preserving the existing installation. The final native PKG command produced valid JSON whose SHA-256 matched the unchanged synthetic input. See evidence/setup-integration.json, setup-ui-source.json, setup-ui-install.json and setup-launched-runtime.jsonl.

## Remaining checks and limits

Manual cancellation during a large physical PKG read, high-DPI/multi-monitor changes, file-dialog selection completion, registered JSON viewer behavior and close-during-copy remain checklist items in SETUP_QA.md. Core cancellation and rollback paths passed automated tests. No prolonged filesystem-race attack or hostile multi-process security proof is claimed.

PKG support is limited to source registration, bounded hashing and documented outer-header hints. The report explicitly states that content extraction, code conversion and game-import support are unavailable. The installer installs the existing native foundation only.
