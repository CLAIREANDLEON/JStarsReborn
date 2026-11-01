# PKG source intake — precise support boundary

The native setup menu accepts a local .pkg file using the Choose PKG picker or its editable path field. Import source registers the package's path, size, SHA-256 and limited outer-header metadata in a new JSON report. The input is neither copied into the installation nor modified. This is the source-intake stage of an import pipeline, not game extraction or conversion.

## Implemented and tested

The reader checks the documented CNT signature (bytes 7F 43 4E 54), requires at least 0x5A0 bytes, and reads an optional printable-ASCII content-ID hint from offset 0x40 over 0x30 bytes. These facts are documented in the public [LibOrbisPkg constants](https://github.com/maxton/LibOrbisPkg/blob/master/LibOrbisPkg/PKG/Pkg.cs) and [header reader](https://github.com/maxton/LibOrbisPkg/blob/master/LibOrbisPkg/PKG/PkgReader.cs). This project independently implements those limited field observations; no library code, keys or decryption routines are included.

Recognition of the signature is not validation of the whole container, proof that it is PS4 rather than another CNT variant, title identification, authenticity, or compatibility. The package may be encrypted or otherwise unsupported; this build does not determine or decrypt its payload. Every successful report sets container_validated=false, content_extracted=false, executable_converted=false and game_import_supported=false.

Source reads use 1 MiB blocks, a 128 GiB package cap, file identity/size/timestamp checks, opened-path checks, and reparse-point rejection. Cancellation leaves no report file. Reports use exclusive commit and must be outside the source package's parent folder. Unknown, truncated or mismatched outer headers produce an error. The implementation reuses the native inventory module's file-handling safeguards.

Tests use newly constructed synthetic headers, not proprietary packages. Twenty-four PKG checks passed, including cancellation, invalid signatures, truncated input, path boundaries, Unicode filenames, metadata handling and preservation of source bytes. No actual J-Stars PKG was supplied or tested.

## What is still missing

A compatible, verified content extractor; accessible authorized game data and format specifications; asset conversion; original animation/combat reconstruction; and reference validation. A package containing a console executable is not Windows source code. Registering it cannot supply the missing game implementation. The menu therefore never marks the game playable or offers a pretend decompile action.

## Command line

JStarsSetup.exe --inspect-pkg C:/local/source/game.pkg --report C:/separate/reports/game.json

For extracted folders, use --inspect instead of --inspect-pkg. Exit 0 means the requested metadata report was produced; it does not mean a game was installed or converted.
