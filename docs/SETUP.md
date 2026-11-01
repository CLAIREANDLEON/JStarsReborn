# Native setup menu — 0.2

Extract the complete delivery ZIP and open JStarsSetup.exe. Keep the executable, setup-payload.txt, JStarsReborn.exe, config, docs and licenses together.

1. Choose PKG selects a local .pkg file. Choose folder accepts previously extracted local data. The path can also be pasted directly.
2. Import source records package metadata or inventories folder contents into %LOCALAPPDATA%\JStarsReborn\reports (per-user; the setup folder may be read-only). Open report opens the generated JSON with its registered Windows application. This does not decrypt, extract or convert game content. See PKG_SUPPORT.md.
3. Choose parent selects the existing parent for a new JStarsReborn-PC directory; the destination is also editable. Install foundation checks the local payload manifest and stages verified runtime files before committing the new folder. Existing destinations are rejected and never replaced. The destination must be separate from both the package tree and selected game-data tree.
4. Launch installed starts the installed JStarsReborn.exe with its installation as working directory. The visible result is the existing DirectX 12 diagnostic runtime. Escape exits that runtime.

The setup menu stays responsive while file work runs on a cancellable worker. Closing while work is active requests cancellation before destroying the window. A late cancellation after the final commit does not remove a successful installation. The current build has no upgrade, uninstaller, Start menu shortcut, registry changes, elevation, telemetry or background service.

## Installation contents

The runtime-only installation contains the native runtime, config and documentation/licenses plus install-receipt.json. It does not include source PKGs, game assets or the setup application. Keep the original setup bundle to inspect another source. The receipt explicitly identifies foundation_only=true and game_data_imported=false. Local hashes check file integrity against the supplied manifest; this is not a signed publisher installer.

## Build the setup bundle

Build using BUILD.md, then run:

~~~powershell
cmake --install build/native --prefix build/package
python tools/make_setup_payload.py --package build/package
~~~

The allowlist generator includes the runtime, config, Markdown documentation and notices. It excludes game data, logs, reports, source code and the setup executable from the runtime installation payload. CMake installs JStarsSetup.exe into the outer setup bundle. The final ZIP should contain that complete outer folder. Creating the manifest is required before the Install foundation button can work.

Useful diagnostic commands:

~~~powershell
JStarsSetup.exe --validate-game C:/dumps/my-decrypted-dump --report C:/reports/validation.json [--versions supported-versions.tsv]
JStarsSetup.exe --inspect-pkg C:/source/game.pkg --report C:/reports/game.json
JStarsSetup.exe --inspect C:/source/extracted --report C:/reports/files.json
JStarsSetup.exe --package C:/setup/bundle --install C:/new-parent/JStarsReborn-PC
JStarsSetup.exe --ui-smoke-ms 500
~~~

Use --source and --destination to prefill the UI without starting work. A command-line inspection output must be new and outside its source. Installation destination's parent must already exist. No process from the selected game folder is executed.
